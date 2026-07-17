/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_network.h"

#include "linkr_debugger_dns.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>

LOG_MODULE_REGISTER(linkr_debugger_network, LOG_LEVEL_INF);

#define DHCP_SERVER_BASE_ADDR "172.29.203.10"
#define NETWORK_START_INITIAL_DELAY_MS 100U
#define NETWORK_START_RETRY_INITIAL_MS 250U
#define NETWORK_START_RETRY_MAX_MS 5000U

static void dhcp_start_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(dhcp_start_work, dhcp_start_work_handler);
static bool dhcp_started;
static bool dns_started;
static uint32_t network_retry_delay_ms = NETWORK_START_RETRY_INITIAL_MS;

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

static int start_dhcp_server(void)
{
	struct net_if *iface;
	struct net_if *addr_iface = NULL;
	struct net_in_addr base_addr;
	struct net_in_addr gw;
	struct net_in_addr addr;
	int ret;

	ret = linkr_debugger_network_get_ncm_iface(&iface);
	if (ret < 0) {
		LOG_WRN("No Ethernet/NCM interface found yet (%d)", ret);
		return ret;
	}

	ret = net_addr_pton(AF_INET, "172.29.203.1", &addr);
	if (ret < 0) {
		return ret;
	}

	if (net_if_ipv4_addr_lookup(&addr, &addr_iface) == NULL || addr_iface != iface) {
		if (net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
			LOG_WRN("Failed to add captive portal IPv4 address, will retry");
			return -EAGAIN;
		}
	}

	if (net_addr_pton(AF_INET, "172.29.203.1", &gw) == 0) {
		net_if_ipv4_set_gw(iface, &gw);
	}

	ret = net_addr_pton(AF_INET, DHCP_SERVER_BASE_ADDR, &base_addr);
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
			LOG_INF("DHCPv4 server started from %s", DHCP_SERVER_BASE_ADDR);
		}
	}

	if (!dns_started) {
		ret = linkr_debugger_dns_start();
		if (ret < 0) {
			LOG_WRN("DNS captive responder start failed (%d), will retry", ret);
			return ret;
		}
		dns_started = true;
	}

	return 0;
}

static void dhcp_start_work_handler(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);

	ret = start_dhcp_server();
	if (ret < 0) {
		k_work_reschedule(&dhcp_start_work, K_MSEC(network_retry_delay_ms));
		if (network_retry_delay_ms < NETWORK_START_RETRY_MAX_MS) {
			network_retry_delay_ms *= 2U;
			if (network_retry_delay_ms > NETWORK_START_RETRY_MAX_MS) {
				network_retry_delay_ms = NETWORK_START_RETRY_MAX_MS;
			}
		}
		return;
	}

	network_retry_delay_ms = NETWORK_START_RETRY_INITIAL_MS;
}

int linkr_debugger_network_init(void)
{
	int ret = start_dhcp_server();

	if (ret < 0) {
		k_work_reschedule(&dhcp_start_work, K_MSEC(NETWORK_START_INITIAL_DELAY_MS));
	} else {
		network_retry_delay_ms = NETWORK_START_RETRY_INITIAL_MS;
	}

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
