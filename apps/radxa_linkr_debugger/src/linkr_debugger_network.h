/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_NETWORK_H_
#define RADXA_LINKR_DEBUGGER_NETWORK_H_

#include <zephyr/net/net_if.h>

int linkr_debugger_network_get_ncm_iface(struct net_if **iface);
bool linkr_debugger_network_has_preferred_ipv4(void);
int linkr_debugger_network_init(void);

#endif /* RADXA_LINKR_DEBUGGER_NETWORK_H_ */
