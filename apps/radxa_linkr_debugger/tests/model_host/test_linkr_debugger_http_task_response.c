/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "../../src/linkr_debugger_http_task_response.h"
#include "../../src/linkr_debugger_task.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

const char *linkr_debugger_json_schema(void)
{
	return "radxa-linkr-debugger.v1";
}

static void extract_blob(const char *json, char *blob, size_t blob_cap, size_t *blob_len)
{
	const char *src = strstr(json, "\"blob\":\"");
	size_t len = 0U;

	assert(src != NULL);
	src += strlen("\"blob\":\"");
	while (*src != '\0' && *src != '"') {
		char value;

		if (*src != '\\') {
			value = *src++;
		} else {
			src++;
			switch (*src++) {
			case 'n': value = '\n'; break;
			case 'r': value = '\r'; break;
			case 't': value = '\t'; break;
			case '"': value = '"'; break;
			case '\\': value = '\\'; break;
			default: assert(false); return;
			}
		}
		assert(len < blob_cap);
		blob[len++] = value;
	}
	assert(*src == '"');
	*blob_len = len;
}

static void fill_maximum_blob(char *blob)
{
	static const char prefix[] =
		"# linkr-task.v1\n"
		"# task max\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":0}\n";
	size_t len = sizeof(prefix) - 1U;

	memcpy(blob, prefix, len);
	while (len < LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE) {
		size_t chunk = LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE - len;

		if (chunk > 256U) {
			chunk = 256U;
		}
		blob[len] = '#';
		if (chunk > 1U) {
			memset(blob + len + 1U, 'x', chunk - 2U);
			blob[len + chunk - 1U] = '\n';
		}
		len += chunk;
	}
}

static void test_store_and_clear_responses(void)
{
	char buffer[128];
	const char *store =
		"{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,\"command\":\"task\","
		"\"action\":\"store\",\"stored\":true}\n";
	const char *clear =
		"{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,\"command\":\"task\","
		"\"action\":\"clear\",\"cleared\":true}\n";
	int written;

	written = linkr_debugger_http_task_store_response(buffer, sizeof(buffer));
	assert(written == (int)strlen(store));
	assert(strcmp(buffer, store) == 0);
	written = linkr_debugger_http_task_clear_response(buffer, sizeof(buffer));
	assert(written == (int)strlen(clear));
	assert(strcmp(buffer, clear) == 0);
}

static void test_task_route_and_method_contract(void)
{
	assert(linkr_debugger_http_task_path_is_supported(LINKR_DEBUGGER_TASK_HTTP_PATH));
	assert(!linkr_debugger_http_task_path_is_supported("/api/v1/orch"));
	assert(!linkr_debugger_http_task_path_is_supported("/api/v1/orch/tasks"));
	assert(!linkr_debugger_http_task_path_is_supported("/api/v1/tasks/boot"));
	assert(linkr_debugger_http_task_action_for_method(1U) ==
	       LINKR_DEBUGGER_TASK_HTTP_ACTION_LIST);
	assert(linkr_debugger_http_task_action_for_method(4U) ==
	       LINKR_DEBUGGER_TASK_HTTP_ACTION_STORE);
	assert(linkr_debugger_http_task_action_for_method(0U) ==
	       LINKR_DEBUGGER_TASK_HTTP_ACTION_CLEAR);
	assert(linkr_debugger_http_task_action_for_method(3U) ==
	       LINKR_DEBUGGER_TASK_HTTP_ACTION_METHOD_NOT_ALLOWED);
}

static void test_empty_list_response(void)
{
	struct linkr_debugger_task_status status = { .backend_available = true };
	char buffer[LINKR_DEBUGGER_TASK_HTTP_RESPONSE_CAP];
	const char *expected =
		"{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,\"command\":\"task\","
		"\"action\":\"list\",\"backend\":{\"available\":true},\"task_count\":0,"
		"\"tasks\":[],\"blob\":\"\"}\n";
	int written;

	written = linkr_debugger_http_task_list_response(buffer, sizeof(buffer), &status, "", 0U);
	assert(written == (int)strlen(expected));
	assert(strcmp(buffer, expected) == 0);
}

static void test_maximum_blob_round_trip_and_capacity_failure(void)
{
	struct linkr_debugger_task_status status = { .backend_available = true, .task_count = 1U };
	char blob[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
	char decoded[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
	char response[LINKR_DEBUGGER_TASK_HTTP_RESPONSE_CAP];
	char too_small[64];
	size_t decoded_len;
	int written;

	memcpy(status.tasks[0].id, "max", sizeof("max"));
	memcpy(status.tasks[0].name, "max", sizeof("max"));
	status.tasks[0].request_count = 1U;
	fill_maximum_blob(blob);
	written = linkr_debugger_http_task_list_response(response, sizeof(response), &status,
						 blob, sizeof(blob));
	assert(written > 0);
	extract_blob(response, decoded, sizeof(decoded), &decoded_len);
	assert(decoded_len == sizeof(blob));
	assert(memcmp(decoded, blob, sizeof(blob)) == 0);
	memset(too_small, 'x', sizeof(too_small));
	assert(linkr_debugger_http_task_list_response(too_small, sizeof(too_small), &status,
						     blob, sizeof(blob)) < 0);
	assert(too_small[0] == '\0');
}

int main(void)
{
	test_store_and_clear_responses();
	test_task_route_and_method_contract();
	test_empty_list_response();
	test_maximum_blob_round_trip_and_capacity_failure();
	puts("linkr_debugger_http_task_response: all tests passed");
	return 0;
}
