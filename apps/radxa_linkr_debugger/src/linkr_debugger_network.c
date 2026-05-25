/*
 * SPDX-License-Identifier: Apache-2.0
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

int linkr_debugger_network_init(void)
{
	struct net_if *iface;
	struct net_in_addr base_addr;
	int ret;

	ret = linkr_debugger_network_get_ncm_iface(&iface);
	if (ret < 0) {
		LOG_ERR("No Ethernet/NCM interface found (%d)", ret);
		return ret;
	}

	ret = net_addr_pton(AF_INET, "172.29.203.10", &base_addr);
	if (ret < 0) {
		LOG_ERR("Invalid DHCP base address (%d)", ret);
		return ret;
	}

	ret = net_dhcpv4_server_start(iface, &base_addr);
	if (ret < 0) {
		LOG_ERR("Failed to start DHCPv4 server (%d)", ret);
		return ret;
	}

	LOG_INF("DHCPv4 server started from 172.29.203.10");
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
