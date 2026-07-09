/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_network.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>

LOG_MODULE_REGISTER(linkr_debugger_network, LOG_LEVEL_INF);

#define DHCP_SERVER_BASE_ADDR "172.29.203.10"

static void dhcp_start_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(dhcp_start_work, dhcp_start_work_handler);

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

static void start_dhcp_server(void)
{
	struct net_if *iface;
	struct net_in_addr base_addr;
	struct in_addr addr;
	int ret;

	ret = linkr_debugger_network_get_ncm_iface(&iface);
	if (ret < 0) {
		LOG_ERR("No Ethernet/NCM interface found (%d)", ret);
		return;
	}

	if (!net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED)) {
		if (net_addr_pton(AF_INET, "172.29.203.1", &addr) == 0) {
			if (net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
				LOG_ERR("Failed to add IPv4 address");
				return;
			}
		}
	}

	ret = net_addr_pton(AF_INET, DHCP_SERVER_BASE_ADDR, &base_addr);
	if (ret < 0) {
		LOG_ERR("Invalid DHCP base address (%d)", ret);
		return;
	}

	ret = net_dhcpv4_server_start(iface, &base_addr);
	if (ret < 0) {
		LOG_ERR("Failed to start DHCPv4 server (%d)", ret);
		return;
	}

	LOG_INF("DHCPv4 server started from %s", DHCP_SERVER_BASE_ADDR);
}

static void dhcp_start_work_handler(struct k_work *work)
{
	start_dhcp_server();
}

int linkr_debugger_network_init(void)
{
	k_work_reschedule(&dhcp_start_work, K_MSEC(100));
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
