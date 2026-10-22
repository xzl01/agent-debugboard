/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_CAPTIVE_PORTAL_H_
#define RADXA_LINKR_DEBUGGER_CAPTIVE_PORTAL_H_

#include <stdbool.h>
#include <stddef.h>

#define LINKR_DEBUGGER_CAPTIVE_PORTAL_API_PATH "/captive-portal/api"
/* Zephyr DHCP option 114 auto-URI appends this path to the board address. */
#define LINKR_DEBUGGER_CAPTIVE_PORTAL_PROBE_PATH "/generate_204"

enum linkr_debugger_captive_method {
	LINKR_DEBUGGER_CAPTIVE_METHOD_GET,
	LINKR_DEBUGGER_CAPTIVE_METHOD_HEAD,
	LINKR_DEBUGGER_CAPTIVE_METHOD_OTHER,
};

enum linkr_debugger_captive_action {
	LINKR_DEBUGGER_CAPTIVE_ACTION_CAPPORT_JSON,
	LINKR_DEBUGGER_CAPTIVE_ACTION_API_NOT_FOUND,
	LINKR_DEBUGGER_CAPTIVE_ACTION_REDIRECT,
	LINKR_DEBUGGER_CAPTIVE_ACTION_METHOD_NOT_ALLOWED,
};

enum linkr_debugger_captive_action linkr_debugger_captive_select_action(
	enum linkr_debugger_captive_method method, const char *path);
bool linkr_debugger_captive_method_has_body(enum linkr_debugger_captive_method method);

/*
 * Build the capport JSON body for the given per-device portal URL. The URL is
 * the board's own network identity (http://<device-address>/), formatted by
 * the caller from the persisted network identity.
 */
int linkr_debugger_captive_capport_body(const char *portal_url, char *buf,
					size_t cap);

#endif /* RADXA_LINKR_DEBUGGER_CAPTIVE_PORTAL_H_ */
