/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_http_body.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define CLIENT_A ((uintptr_t)0x1000U)
#define CLIENT_B ((uintptr_t)0x2000U)
#define METHOD_POST 1U
#define METHOD_GET 2U
#define ROUTE_LOGIC_ANALYZER 10U
#define ROUTE_OTHER 11U

static void assert_view_eq(const struct linkr_debugger_http_body_view *view,
			   const char *expected)
{
	assert(view != NULL);
	assert(view->data != NULL || view->len == 0U);
	assert(view->len == strlen(expected));
	assert(memcmp(view->data, expected, view->len) == 0);
}

static void test_one_shot_final(void)
{
	struct linkr_debugger_http_body_view view;
	const char body[] = "{\"post_samples\":16}";

	linkr_debugger_http_body_reset_all();
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_FINAL, body,
						 strlen(body), &view) == LINKR_DEBUGGER_HTTP_BODY_READY);
	assert_view_eq(&view, body);
	linkr_debugger_http_body_clear(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER);
}

static void test_two_fragment_body(void)
{
	struct linkr_debugger_http_body_view view;

	linkr_debugger_http_body_reset_all();
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_MORE, "{\"a", 3U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_WAITING);
	assert(view.data == NULL);
	assert(view.len == 0U);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_FINAL, "\":1}", 4U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_READY);
	assert_view_eq(&view, "{\"a\":1}");
	linkr_debugger_http_body_clear(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER);
}

static void test_three_fragment_body(void)
{
	struct linkr_debugger_http_body_view view;

	linkr_debugger_http_body_reset_all();
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_MORE, "{\"", 2U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_WAITING);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_MORE, "pins\":[7,10]",
						 12U, &view) == LINKR_DEBUGGER_HTTP_BODY_WAITING);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_FINAL, "}", 1U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_READY);
	assert_view_eq(&view, "{\"pins\":[7,10]}");
	linkr_debugger_http_body_clear(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER);
}

static void test_exact_cap(void)
{
	struct linkr_debugger_http_body_view view;
	uint8_t body[LINKR_DEBUGGER_HTTP_BODY_CAP];

	memset(body, 'x', sizeof(body));
	linkr_debugger_http_body_reset_all();
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_FINAL, body,
						 sizeof(body), &view) == LINKR_DEBUGGER_HTTP_BODY_READY);
	assert(view.data != NULL);
	assert(view.len == sizeof(body));
	assert(memcmp(view.data, body, sizeof(body)) == 0);
	linkr_debugger_http_body_clear(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER);
}

static void test_overflow_clears_slot(void)
{
	struct linkr_debugger_http_body_view view;
	uint8_t body[LINKR_DEBUGGER_HTTP_BODY_CAP];

	memset(body, 'x', sizeof(body));
	linkr_debugger_http_body_reset_all();
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_MORE, body,
						 sizeof(body), &view) == LINKR_DEBUGGER_HTTP_BODY_WAITING);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_FINAL, "x", 1U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_TOO_LARGE);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_FINAL, "{}", 2U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_READY);
	assert_view_eq(&view, "{}");
	linkr_debugger_http_body_clear(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER);
}

static void test_client_isolation(void)
{
	struct linkr_debugger_http_body_view view;

	linkr_debugger_http_body_reset_all();
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_MORE, "A", 1U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_WAITING);
	assert(linkr_debugger_http_body_accumulate(CLIENT_B, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_FINAL, "B", 1U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_READY);
	assert_view_eq(&view, "B");
	linkr_debugger_http_body_clear(CLIENT_B, METHOD_POST, ROUTE_LOGIC_ANALYZER);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_FINAL, "Z", 1U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_READY);
	assert_view_eq(&view, "AZ");
	linkr_debugger_http_body_clear(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER);
}

static void test_route_method_isolation(void)
{
	struct linkr_debugger_http_body_view view;

	linkr_debugger_http_body_reset_all();
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_MORE, "A", 1U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_WAITING);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_OTHER,
						 LINKR_DEBUGGER_HTTP_BODY_MORE, "B", 1U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_MISMATCH);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_FINAL, "C", 1U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_READY);
	assert_view_eq(&view, "C");
	linkr_debugger_http_body_clear(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER);

	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_MORE, "D", 1U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_WAITING);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_GET, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_FINAL, "E", 1U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_MISMATCH);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_FINAL, "F", 1U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_READY);
	assert_view_eq(&view, "F");
	linkr_debugger_http_body_clear(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER);
}

static void test_aborted_complete_reset(void)
{
	struct linkr_debugger_http_body_view view;

	linkr_debugger_http_body_reset_all();
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_MORE, "old", 3U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_WAITING);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_ABORTED, NULL, 0U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_CLEARED);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_FINAL, "new", 3U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_READY);
	assert_view_eq(&view, "new");
	linkr_debugger_http_body_clear(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER);

	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_MORE, "old", 3U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_WAITING);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_COMPLETE, NULL, 0U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_CLEARED);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_FINAL, "newer", 5U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_READY);
	assert_view_eq(&view, "newer");
	linkr_debugger_http_body_clear(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER);
}

static void test_empty_fragments(void)
{
	struct linkr_debugger_http_body_view view;

	linkr_debugger_http_body_reset_all();
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_MORE, NULL, 0U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_WAITING);
	assert(linkr_debugger_http_body_accumulate(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER,
						 LINKR_DEBUGGER_HTTP_BODY_FINAL, NULL, 0U,
						 &view) == LINKR_DEBUGGER_HTTP_BODY_READY);
	assert(view.data != NULL);
	assert(view.len == 0U);
	linkr_debugger_http_body_clear(CLIENT_A, METHOD_POST, ROUTE_LOGIC_ANALYZER);
}

static void test_lifecycle_gate(void)
{
	assert(linkr_debugger_http_body_should_handle(true,
						   LINKR_DEBUGGER_HTTP_BODY_MORE));
	assert(linkr_debugger_http_body_should_handle(true,
						   LINKR_DEBUGGER_HTTP_BODY_FINAL));
	assert(linkr_debugger_http_body_should_handle(true,
						   LINKR_DEBUGGER_HTTP_BODY_ABORTED));
	assert(linkr_debugger_http_body_should_handle(true,
						   LINKR_DEBUGGER_HTTP_BODY_COMPLETE));

	assert(!linkr_debugger_http_body_should_handle(false,
						    LINKR_DEBUGGER_HTTP_BODY_MORE));
	assert(linkr_debugger_http_body_should_handle(false,
						   LINKR_DEBUGGER_HTTP_BODY_FINAL));
	assert(!linkr_debugger_http_body_should_handle(false,
						    LINKR_DEBUGGER_HTTP_BODY_ABORTED));
	assert(!linkr_debugger_http_body_should_handle(false,
						    LINKR_DEBUGGER_HTTP_BODY_COMPLETE));
}

int main(void)
{
	test_one_shot_final();
	test_two_fragment_body();
	test_three_fragment_body();
	test_exact_cap();
	test_overflow_clears_slot();
	test_client_isolation();
	test_route_method_isolation();
	test_aborted_complete_reset();
	test_empty_fragments();
	test_lifecycle_gate();
	return 0;
}
