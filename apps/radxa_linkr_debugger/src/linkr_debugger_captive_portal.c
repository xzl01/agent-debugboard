/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_captive_portal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool path_equals_ignoring_query(const char *path, const char *target)
{
	size_t target_len;

	if (path == NULL || target == NULL) {
		return false;
	}

	target_len = strlen(target);
	return strncmp(path, target, target_len) == 0 &&
	       (path[target_len] == '\0' || path[target_len] == '?');
}

static bool path_is_api_root_or_child(const char *path)
{
	static const char api_root[] = "/api/v1";
	size_t root_len;

	if (path == NULL) {
		return false;
	}

	root_len = strlen(api_root);
	return strncmp(path, api_root, root_len) == 0 &&
	       (path[root_len] == '\0' || path[root_len] == '?' || path[root_len] == '/');
}

enum linkr_debugger_captive_action linkr_debugger_captive_select_action(
	enum linkr_debugger_captive_method method, const char *path)
{
	if (method != LINKR_DEBUGGER_CAPTIVE_METHOD_GET &&
	    method != LINKR_DEBUGGER_CAPTIVE_METHOD_HEAD) {
		return LINKR_DEBUGGER_CAPTIVE_ACTION_METHOD_NOT_ALLOWED;
	}

	if (path_equals_ignoring_query(path, LINKR_DEBUGGER_CAPTIVE_PORTAL_API_PATH) ||
	    path_equals_ignoring_query(path, LINKR_DEBUGGER_CAPTIVE_PORTAL_PROBE_PATH)) {
		return LINKR_DEBUGGER_CAPTIVE_ACTION_CAPPORT_JSON;
	}

	if (path_is_api_root_or_child(path)) {
		return LINKR_DEBUGGER_CAPTIVE_ACTION_API_NOT_FOUND;
	}

	return LINKR_DEBUGGER_CAPTIVE_ACTION_REDIRECT;
}

bool linkr_debugger_captive_method_has_body(enum linkr_debugger_captive_method method)
{
	return method == LINKR_DEBUGGER_CAPTIVE_METHOD_GET;
}

int linkr_debugger_captive_capport_body(const char *portal_url, char *buf,
					size_t cap)
{
	int written;

	if (portal_url == NULL || portal_url[0] == '\0' || buf == NULL ||
	    cap == 0U) {
		return -EINVAL;
	}

	written = snprintf(buf, cap,
			   "{\"captive\":true,\"user-portal-url\":\"%s\","
			   "\"venue-info-url\":\"%s\"}\n",
			   portal_url, portal_url);
	if (written < 0) {
		return -EINVAL;
	}
	if ((size_t)written >= cap) {
		return -ENOSPC;
	}

	return 0;
}
