/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "linkr_debugger_captive_portal.h"

static void test_captive_non_api_paths_redirect(void)
{
	const char *paths[] = {
		"/",
		"/generate_204",
		"/hotspot-detect.html",
		"/connecttest.txt",
		"/ncsi.txt",
		"/unknown?x=1",
		"/api/v1statusjunk",
	};

	for (size_t i = 0U; i < sizeof(paths) / sizeof(paths[0]); i++) {
		assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_GET,
							       paths[i]) ==
		       LINKR_DEBUGGER_CAPTIVE_ACTION_REDIRECT);
	}
}

static void test_captive_api_path(void)
{
	assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_GET,
						       "/captive-portal/api") ==
	       LINKR_DEBUGGER_CAPTIVE_ACTION_CAPPORT_JSON);
	assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_GET,
						       "/captive-portal/api?x=1") ==
	       LINKR_DEBUGGER_CAPTIVE_ACTION_CAPPORT_JSON);
	assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_HEAD,
						       "/captive-portal/api") ==
	       LINKR_DEBUGGER_CAPTIVE_ACTION_CAPPORT_JSON);
	assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_GET,
						       "/captive-portal/apix") ==
	       LINKR_DEBUGGER_CAPTIVE_ACTION_REDIRECT);
}

static void test_captive_unknown_api_paths_json_404(void)
{
	const char *paths[] = {
		"/api/v1",
		"/api/v1/",
		"/api/v1/unknown",
		"/api/v1/statusjunk",
	};

	for (size_t i = 0U; i < sizeof(paths) / sizeof(paths[0]); i++) {
		assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_GET,
							       paths[i]) ==
		       LINKR_DEBUGGER_CAPTIVE_ACTION_API_NOT_FOUND);
	}
}

static void test_captive_methods(void)
{
	assert(linkr_debugger_captive_method_has_body(LINKR_DEBUGGER_CAPTIVE_METHOD_GET));
	assert(!linkr_debugger_captive_method_has_body(LINKR_DEBUGGER_CAPTIVE_METHOD_HEAD));
	assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_HEAD,
						       "/does-not-exist") ==
	       LINKR_DEBUGGER_CAPTIVE_ACTION_REDIRECT);
	assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_OTHER,
						       "/captive-portal/api") ==
	       LINKR_DEBUGGER_CAPTIVE_ACTION_METHOD_NOT_ALLOWED);
}

static void test_captive_canonical_url(void)
{
	assert(strcmp(linkr_debugger_captive_portal_url(), "http://172.29.203.1/") == 0);
	assert(strstr(linkr_debugger_captive_capport_body(), ":8080") == NULL);
	assert(strstr(linkr_debugger_captive_capport_body(),
		      "\"user-portal-url\":\"http://172.29.203.1/\"") != NULL);
}

int main(void)
{
	test_captive_non_api_paths_redirect();
	test_captive_api_path();
	test_captive_unknown_api_paths_json_404();
	test_captive_methods();
	test_captive_canonical_url();
	printf("linkr_debugger_portal_test: OK\n");
	return 0;
}
