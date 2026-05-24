/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AGENT_DEBUGBOARD_NETWORK_H_
#define AGENT_DEBUGBOARD_NETWORK_H_

#include <zephyr/net/net_if.h>

int debugboard_network_get_ncm_iface(struct net_if **iface);
bool debugboard_network_has_preferred_ipv4(void);
int debugboard_network_init(void);

#endif /* AGENT_DEBUGBOARD_NETWORK_H_ */
