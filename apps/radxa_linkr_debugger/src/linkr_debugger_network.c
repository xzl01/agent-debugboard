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
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
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
static struct linkr_debugger_net_identity network_identity;
static bool network_identity_loaded;
static uint32_t network_retry_delay_ms = NETWORK_START_RETRY_INITIAL_MS;
static struct linkr_debugger_net_identity network_requested_identity;
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

int linkr_debugger_network_identity(struct linkr_debugger_net_identity *identity)
{
	uint8_t chip_id[LINKR_DEBUGGER_NET_CHIP_ID_LEN];
	ssize_t chip_id_len;
	int ret;

	if (identity == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&network_lock, K_FOREVER);

	if (!network_identity_loaded) {
		chip_id_len = hwinfo_get_device_id(chip_id, sizeof(chip_id));
		ret = linkr_debugger_net_identity_resolve(
			chip_id, chip_id_len > 0 ? (size_t)chip_id_len : 0U,
			&network_identity);
		if (ret < 0) {
			k_mutex_unlock(&network_lock);
			LOG_ERR("No identity and bad chip ID (%d)",
				ret);
			return ret;
		}
		network_identity_loaded = true;
	}

	*identity = network_identity;
	k_mutex_unlock(&network_lock);
	return 0;
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

static int start_dhcp_server_locked(void)
{
	struct net_if *iface;
	struct net_in_addr base_addr;
	struct net_in_addr addr;
	struct net_in_addr netmask;
	char device_ip[LINKR_DEBUGGER_NET_IPV4_STR_LEN];
	char base_ip[LINKR_DEBUGGER_NET_IPV4_STR_LEN];
	int ret;

	ret = linkr_debugger_network_get_ncm_iface(&iface);
	if (ret < 0) {
		LOG_WRN("No NCM interface (%d)", ret);
		return ret;
	}

	ret = linkr_debugger_net_format_ipv4(network_identity.network,
					     LINKR_DEBUGGER_NET_DEVICE_HOST,
					     device_ip, sizeof(device_ip));
	if (ret < 0) {
		LOG_ERR("Bad board IPv4 (%d)", ret);
		return ret;
	}

	ret = linkr_debugger_net_format_ipv4(network_identity.network,
					     LINKR_DEBUGGER_NET_DHCP_BASE_HOST,
					     base_ip, sizeof(base_ip));
	if (ret < 0) {
		LOG_ERR("Bad DHCP base (%d)", ret);
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
		LOG_WRN("Failed to add IPv4, retry");
		return -EAGAIN;
	}

	ret = net_addr_pton(AF_INET, "255.255.255.252", &netmask);
	if (ret < 0 || !net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask)) {
		LOG_ERR("Netmask failed");
		return -EINVAL;
	}

	ret = net_addr_pton(AF_INET, base_ip, &base_addr);
	if (ret < 0) {
		LOG_ERR("Bad DHCP base (%d)", ret);
		return ret;
	}

	if (!dhcp_started) {
		ret = net_dhcpv4_server_start(iface, &base_addr);
		if (ret == -EALREADY) {
			dhcp_started = true;
		} else if (ret < 0) {
			LOG_WRN("DHCP start failed (%d)", ret);
			return ret;
		} else {
			dhcp_started = true;
		}
	}

	return 0;
}

/*
 * Apply the current identity to the NCM interface. The link address must be
 * set while the interface is not running (net_if_set_link_addr() rejects a
 * running interface), which is what CONFIG_ETH_NET_IF_NO_AUTO_START enables:
 * the MAC is applied first, then net_if_up() makes the interface operational.
 */
static int apply_identity_locked(const struct linkr_debugger_net_identity *previous,
				 bool restart_link)
{
	struct net_if *iface;
	struct net_in_addr old_addr;
	char old_ip[LINKR_DEBUGGER_NET_IPV4_STR_LEN];
	int ret;

	ret = linkr_debugger_network_get_ncm_iface(&iface);
	if (ret < 0) {
		network_retry_schedule_locked();
		LOG_WRN("Saved, no iface (%d)", ret);
		return 0;
	}

	if (restart_link) {
		if (dhcp_started) {
			(void)net_dhcpv4_server_stop(iface);
			dhcp_started = false;
		}
		/* Force the interface out of RUNNING so the MAC can change. */
		(void)net_if_down(iface);
	}

	if (restart_link || !net_if_is_up(iface)) {
		ret = net_if_set_link_addr(iface, network_identity.mac,
					   LINKR_DEBUGGER_NET_MAC_LEN,
					   NET_LINK_ETHERNET);
		if (ret < 0 && restart_link) {
			(void)net_if_up(iface);
		}
		if (ret < 0) {
			LOG_ERR("MAC failed (%d)", ret);
		}
	}

	if (!net_if_is_up(iface)) {
		ret = net_if_up(iface);
		if (ret < 0 && ret != -EALREADY) {
			LOG_WRN("NCM up failed (%d)", ret);
		}
	}

	if (previous != NULL && previous->network != network_identity.network) {
		if (dhcp_started) {
			ret = net_dhcpv4_server_stop(iface);
			if (ret < 0 && ret != -ENOENT) {
				LOG_WRN("DHCP stop failed (%d)", ret);
			}
			dhcp_started = false;
		}

		if (linkr_debugger_net_format_ipv4(previous->network,
						   LINKR_DEBUGGER_NET_DEVICE_HOST,
						   old_ip, sizeof(old_ip)) == 0 &&
		    net_addr_pton(AF_INET, old_ip, &old_addr) == 0) {
			(void)net_if_ipv4_addr_rm(iface, &old_addr);
		}
	}

	ret = start_dhcp_server_locked();
	if (ret < 0) {
		network_retry_schedule_locked();
		return ret;
	}

	network_retry_delay_ms = NETWORK_START_RETRY_INITIAL_MS;
	return 0;
}

static int reconfigure_identity_locked(const struct linkr_debugger_net_identity *identity,
				       bool *reconnect_required)
{
	struct linkr_debugger_net_identity previous;
	bool had_previous = network_identity_loaded;
	bool mac_changed;
	bool network_changed;
	bool restart_link;
	int ret;

	/*
	 * Manual values are authoritative and persistent: the whole identity is
	 * written as one atomic blob before anything is applied live, so a
	 * failure here leaves the running configuration untouched.
	 */
	ret = linkr_debugger_net_identity_save(identity);
	if (ret < 0) {
		LOG_ERR("Identity persist failed (%d)", ret);
		return ret;
	}

	previous = network_identity;
	mac_changed = had_previous &&
		      memcmp(previous.mac, identity->mac, LINKR_DEBUGGER_NET_MAC_LEN) != 0;
	network_changed = had_previous && previous.network != identity->network;
	restart_link = mac_changed;

	if (reconnect_required != NULL) {
		*reconnect_required = mac_changed || network_changed;
	}

	network_identity = *identity;
	network_identity_loaded = true;

	/* A value-identical manual set must not disturb the running link or
	 * promote a derived identity. Persistence already succeeded above.
	 */
	if (had_previous && !mac_changed && !network_changed) {
		return 0;
	}

	ret = apply_identity_locked(had_previous ? &previous : NULL, restart_link);

	return ret;
}

static void network_reconfigure_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&network_lock, K_FOREVER);
	network_request_result = reconfigure_identity_locked(
		&network_requested_identity, &network_request_reconnect_required);
	k_mutex_unlock(&network_lock);

	k_sem_give(&network_request_done);
}

int linkr_debugger_network_set_identity(const struct linkr_debugger_net_identity *identity)
{
	return linkr_debugger_network_set_identity_with_result(identity, NULL);
}

int linkr_debugger_network_set_identity_with_result(const struct linkr_debugger_net_identity *identity,
						    bool *reconnect_required)
{
	int ret;

	if (reconnect_required != NULL) {
		*reconnect_required = false;
	}

	if (!linkr_debugger_net_identity_valid(identity)) {
		return -EINVAL;
	}

	/* Never submit work and wait for it from the queue that must run it. */
	if (k_current_get() == k_work_queue_thread_get(&k_sys_work_q)) {
		k_mutex_lock(&network_lock, K_FOREVER);
		ret = reconfigure_identity_locked(identity, reconnect_required);
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
	network_requested_identity = *identity;
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
	ret = apply_identity_locked(NULL, false);
	if (ret < 0) {
		network_retry_schedule_locked();
	} else if (linkr_debugger_network_has_preferred_ipv4()) {
		network_retry_delay_ms = NETWORK_START_RETRY_INITIAL_MS;
	}
	k_mutex_unlock(&network_lock);
}

int linkr_debugger_network_init(void)
{
	struct linkr_debugger_net_identity identity;
	int ret;

	ret = linkr_debugger_network_identity(&identity);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&network_lock, K_FOREVER);
	ret = apply_identity_locked(NULL, false);
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
