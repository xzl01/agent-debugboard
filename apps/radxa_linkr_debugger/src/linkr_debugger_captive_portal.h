/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_CAPTIVE_PORTAL_H_
#define RADXA_LINKR_DEBUGGER_CAPTIVE_PORTAL_H_

#include <stdbool.h>

#define LINKR_DEBUGGER_CAPTIVE_PORTAL_API_PATH "/captive-portal/api"
#define LINKR_DEBUGGER_CAPTIVE_PORTAL_URL "http://172.29.203.1/"

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
const char *linkr_debugger_captive_portal_url(void);
const char *linkr_debugger_captive_capport_body(void);

#endif /* RADXA_LINKR_DEBUGGER_CAPTIVE_PORTAL_H_ */
