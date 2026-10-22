/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_NETWORK_H_
#define RADXA_LINKR_DEBUGGER_NETWORK_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/net/net_if.h>

#include "linkr_debugger_net_settings.h"

int linkr_debugger_network_get_ncm_iface(struct net_if **iface);
bool linkr_debugger_network_has_preferred_ipv4(void);

/*
 * Current network identity. Loads the persisted identity on first use and
 * only derives (plus persists) when no valid persisted identity exists.
 */
int linkr_debugger_network_identity(struct linkr_debugger_net_identity *identity);

/*
 * Persist the identity atomically and apply it to the NCM interface. Manual
 * values are authoritative: persistence happens before any live apply and a
 * persistence failure leaves the running configuration untouched.
 */
int linkr_debugger_network_set_identity(const struct linkr_debugger_net_identity *identity);
int linkr_debugger_network_set_identity_with_result(const struct linkr_debugger_net_identity *identity,
						    bool *reconnect_required);

int linkr_debugger_network_init(void);

#endif /* RADXA_LINKR_DEBUGGER_NETWORK_H_ */
