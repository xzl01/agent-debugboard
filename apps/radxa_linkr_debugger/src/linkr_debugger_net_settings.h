/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_NET_SETTINGS_H_
#define RADXA_LINKR_DEBUGGER_NET_SETTINGS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINKR_DEBUGGER_NET_SETTINGS_KEY "linkr/net/subnet"
#define LINKR_DEBUGGER_NET_SUBNET_DEFAULT 203U
#define LINKR_DEBUGGER_NET_SUBNET_MIN 1U
#define LINKR_DEBUGGER_NET_SUBNET_MAX 254U
#define LINKR_DEBUGGER_NET_DEVICE_HOST 1U
#define LINKR_DEBUGGER_NET_DHCP_BASE_HOST 10U

bool linkr_debugger_net_subnet_valid(uint8_t subnet);
int linkr_debugger_net_parse_subnet(const char *text, uint8_t *subnet);
int linkr_debugger_net_format_ipv4(uint8_t subnet, uint8_t host, char *buf,
				   size_t cap);
int linkr_debugger_net_format_captive_uri(uint8_t subnet, char *buf,
					  size_t cap);
uint8_t linkr_debugger_net_subnet_load(void);
int linkr_debugger_net_subnet_save(uint8_t subnet);

#endif /* RADXA_LINKR_DEBUGGER_NET_SETTINGS_H_ */
