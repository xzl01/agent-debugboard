/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_network.h"

#include "linkr_debugger_net_settings.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>

LOG_MODULE_REGISTER(linkr_debugger_network, LOG_LEVEL_INF);

#define NETWORK_START_INITIAL_DELAY_MS 100U
#define NETWORK_START_RETRY_INITIAL_MS 250U
#define NETWORK_START_RETRY_MAX_MS 5000U

static void dhcp_start_work_handler(struct k_work *work);
static void network_reconfigure_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(dhcp_start_work, dhcp_start_work_handler);
static K_WORK_DEFINE(network_reconfigure_work, network_reconfigure_work_handler);
static K_MUTEX_DEFINE(network_lock);
static K_MUTEX_DEFINE(network_request_lock);
static K_SEM_DEFINE(network_request_done, 0, 1);
static bool dhcp_started;
static uint8_t network_subnet = LINKR_DEBUGGER_NET_SUBNET_DEFAULT;
static bool network_subnet_loaded;
static uint32_t network_retry_delay_ms = NETWORK_START_RETRY_INITIAL_MS;
static uint8_t network_requested_subnet;
static int network_request_result;
static bool network_request_reconnect_required;

int linkr_debugger_network_get_ncm_iface(struct net_if **iface)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(cdc_ncm_eth0));
	struct net_if *candidate;

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}

	candidate = net_if_lookup_by_dev(dev);

	if (candidate == NULL) {
		return -ENODEV;
	}

	*iface = candidate;
	return 0;
}

uint8_t linkr_debugger_network_subnet(void)
{
	if (!network_subnet_loaded) {
		network_subnet = linkr_debugger_net_subnet_load();
		network_subnet_loaded = true;
	}

	return network_subnet;
}

static void network_retry_schedule_locked(void)
{
	(void)k_work_reschedule(&dhcp_start_work, K_MSEC(network_retry_delay_ms));

	if (network_retry_delay_ms < NETWORK_START_RETRY_MAX_MS) {
		network_retry_delay_ms *= 2U;
		if (network_retry_delay_ms > NETWORK_START_RETRY_MAX_MS) {
			network_retry_delay_ms = NETWORK_START_RETRY_MAX_MS;
		}
	}
}

static int start_dhcp_server_locked(uint8_t subnet)
{
	struct net_if *iface;
	struct net_in_addr base_addr;
	struct net_in_addr addr;
	char device_ip[16];
	char base_ip[16];
	int ret;

	ret = linkr_debugger_network_get_ncm_iface(&iface);
	if (ret < 0) {
		LOG_WRN("No Ethernet/NCM interface found yet (%d)", ret);
		return ret;
	}

	ret = linkr_debugger_net_format_ipv4(subnet, LINKR_DEBUGGER_NET_DEVICE_HOST,
					     device_ip, sizeof(device_ip));
	if (ret < 0) {
		LOG_ERR("Invalid device IPv4 address (%d)", ret);
		return ret;
	}

	ret = linkr_debugger_net_format_ipv4(subnet, LINKR_DEBUGGER_NET_DHCP_BASE_HOST,
					     base_ip, sizeof(base_ip));
	if (ret < 0) {
		LOG_ERR("Invalid DHCP base address (%d)", ret);
		return ret;
	}

	ret = net_addr_pton(AF_INET, device_ip, &addr);
	if (ret < 0) {
		return ret;
	}

	/*
	 * net_if_ipv4_addr_add() is idempotent while this manual address owns its
	 * add reference. If an earlier remove left the address alive because a
	 * socket still references it, add() reacquires the released add reference
	 * and marks the address added again. A lookup alone cannot do that.
	 */
	if (net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
		LOG_WRN("Failed to add captive portal IPv4 address, will retry");
		return -EAGAIN;
	}

	ret = net_addr_pton(AF_INET, base_ip, &base_addr);
	if (ret < 0) {
		LOG_ERR("Invalid DHCP base address (%d)", ret);
		return ret;
	}

	if (!dhcp_started) {
		ret = net_dhcpv4_server_start(iface, &base_addr);
		if (ret == -EALREADY) {
			dhcp_started = true;
			LOG_INF("DHCPv4 server already running");
		} else if (ret < 0) {
			LOG_WRN("Failed to start DHCPv4 server (%d), will retry", ret);
			return ret;
		} else {
			dhcp_started = true;
			LOG_INF("DHCPv4 server started from %s", base_ip);
		}
	}

	return 0;
}

static int reconfigure_network_locked(uint8_t subnet, bool *reconnect_required)
{
	struct net_if *iface;
	struct net_in_addr old_addr;
	char old_ip[16];
	uint8_t old_subnet;
	int ret;

	old_subnet = linkr_debugger_network_subnet();
	if (reconnect_required != NULL) {
		*reconnect_required = old_subnet != subnet;
	}

	ret = linkr_debugger_net_subnet_save(subnet);
	if (ret < 0) {
		LOG_ERR("Failed to persist network subnet (%d)", ret);
		return ret;
	}

	ret = linkr_debugger_network_get_ncm_iface(&iface);
	if (ret < 0) {
		network_subnet = subnet;
		network_subnet_loaded = true;
		network_retry_schedule_locked();
		LOG_WRN("Network subnet persisted but interface is unavailable (%d)", ret);
		return 0;
	}

	if (old_subnet != subnet) {
		if (linkr_debugger_net_format_ipv4(old_subnet,
						   LINKR_DEBUGGER_NET_DEVICE_HOST,
						   old_ip, sizeof(old_ip)) < 0) {
			return -EINVAL;
		}

		if (dhcp_started) {
			ret = net_dhcpv4_server_stop(iface);
			if (ret < 0 && ret != -ENOENT) {
				LOG_WRN("Failed to stop DHCPv4 server (%d)", ret);
			}
			dhcp_started = false;
		}

		if (net_addr_pton(AF_INET, old_ip, &old_addr) == 0) {
			(void)net_if_ipv4_addr_rm(iface, &old_addr);
		}
	}

	network_subnet = subnet;
	network_subnet_loaded = true;

	ret = start_dhcp_server_locked(subnet);
	if (ret < 0) {
		network_retry_schedule_locked();
		return ret;
	}

	LOG_INF("Network subnet set to %u", (unsigned int)subnet);
	network_retry_delay_ms = NETWORK_START_RETRY_INITIAL_MS;
	return 0;
}

static void network_reconfigure_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&network_lock, K_FOREVER);
	network_request_result = reconfigure_network_locked(
		network_requested_subnet, &network_request_reconnect_required);
	k_mutex_unlock(&network_lock);

	k_sem_give(&network_request_done);
}

int linkr_debugger_network_set_subnet(uint8_t subnet)
{
	return linkr_debugger_network_set_subnet_with_result(subnet, NULL);
}

int linkr_debugger_network_set_subnet_with_result(uint8_t subnet,
						  bool *reconnect_required)
{
	int ret;

	if (reconnect_required != NULL) {
		*reconnect_required = false;
	}

	if (!linkr_debugger_net_subnet_valid(subnet)) {
		return -EINVAL;
	}

	/* Never submit work and wait for it from the queue that must run it. */
	if (k_current_get() == k_work_queue_thread_get(&k_sys_work_q)) {
		k_mutex_lock(&network_lock, K_FOREVER);
		ret = reconfigure_network_locked(subnet, reconnect_required);
		k_mutex_unlock(&network_lock);
		return ret;
	}

	/*
	 * There is one request payload/result pair. Keep it exclusively owned by
	 * the caller until the system-workqueue handler publishes the result.
	 * The caller waits without holding network_lock.
	 */
	k_mutex_lock(&network_request_lock, K_FOREVER);
	k_sem_reset(&network_request_done);
	network_requested_subnet = subnet;
	network_request_reconnect_required = false;
	ret = k_work_submit(&network_reconfigure_work);
	if (ret >= 0) {
		(void)k_sem_take(&network_request_done, K_FOREVER);
		ret = network_request_result;
		if (reconnect_required != NULL) {
			*reconnect_required = network_request_reconnect_required;
		}
	}
	k_mutex_unlock(&network_request_lock);

	return ret;
}

static void dhcp_start_work_handler(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);

	k_mutex_lock(&network_lock, K_FOREVER);
	ret = start_dhcp_server_locked(linkr_debugger_network_subnet());
	if (ret < 0) {
		network_retry_schedule_locked();
	} else {
		network_retry_delay_ms = NETWORK_START_RETRY_INITIAL_MS;
	}
	k_mutex_unlock(&network_lock);
}

int linkr_debugger_network_init(void)
{
	int ret;

	network_subnet = linkr_debugger_net_subnet_load();
	network_subnet_loaded = true;

	k_mutex_lock(&network_lock, K_FOREVER);
	ret = start_dhcp_server_locked(network_subnet);
	if (ret < 0) {
		(void)k_work_reschedule(&dhcp_start_work,
					K_MSEC(NETWORK_START_INITIAL_DELAY_MS));
	} else {
		network_retry_delay_ms = NETWORK_START_RETRY_INITIAL_MS;
	}
	k_mutex_unlock(&network_lock);

	return 0;
}

bool linkr_debugger_network_has_preferred_ipv4(void)
{
	struct net_if *iface;

	if (linkr_debugger_network_get_ncm_iface(&iface) < 0) {
		return false;
	}

	return net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}
