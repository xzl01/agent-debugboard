/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "../../src/linkr_debugger_task_http.h"

#include "../../src/linkr_debugger_capture_arbiter.h"
#include "../../src/linkr_debugger_flash_arbiter.h"
#include "../../src/linkr_debugger_http_task_response.h"
#include "../../src/linkr_debugger_json_cursor.h"
#include "../../src/linkr_debugger_task.h"
#include "../../src/linkr_debugger_task_catalog.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

const char *linkr_debugger_json_schema(void)
{
	return "radxa-linkr-debugger.v1";
}

#define ARRAY_SIZE_LOCAL(array) (sizeof(array) / sizeof((array)[0]))
#define RESPONSE_TEXT_CAP (LINKR_DEBUGGER_TASK_HTTP_RESPONSE_CAP + 1U)

/* Fake capture/flash arbiters with call counters and busy injection. */
static unsigned int capture_acquire_calls;
static unsigned int capture_release_calls;
static unsigned int flash_acquire_calls;
static unsigned int flash_release_calls;
static bool capture_busy;
static bool flash_busy;

bool linkr_debugger_capture_arbiter_try_acquire(enum linkr_debugger_capture_owner owner)
{
	assert(owner == LINKR_DEBUGGER_CAPTURE_OWNER_TASK);
	capture_acquire_calls++;
	return !capture_busy;
}

bool linkr_debugger_capture_arbiter_release(enum linkr_debugger_capture_owner owner)
{
	assert(owner == LINKR_DEBUGGER_CAPTURE_OWNER_TASK);
	capture_release_calls++;
	return true;
}

bool linkr_debugger_flash_arbiter_try_acquire(enum linkr_debugger_flash_owner owner)
{
	assert(owner == LINKR_DEBUGGER_FLASH_OWNER_TASK);
	flash_acquire_calls++;
	return !flash_busy;
}

bool linkr_debugger_flash_arbiter_release(enum linkr_debugger_flash_owner owner)
{
	assert(owner == LINKR_DEBUGGER_FLASH_OWNER_TASK);
	flash_release_calls++;
	return true;
}

/* In-memory task storage backend. */
static char store_buf[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
static size_t store_len;
static bool store_present;
static bool fail_load;
static bool fail_save;
static bool fail_delete;

static int fake_load_one(void *context, const char *name, void *value, size_t value_size)
{
	(void)context;
	assert(name != NULL);
	if (fail_load) {
		return -EIO;
	}
	if (!store_present) {
		return -ENOENT;
	}
	assert(value_size >= store_len);
	memcpy(value, store_buf, store_len);
	return (int)store_len;
}

static int fake_save_one(void *context, const char *name, const void *value, size_t value_size)
{
	(void)context;
	assert(name != NULL);
	if (fail_save || value_size > sizeof(store_buf)) {
		return -EIO;
	}
	memcpy(store_buf, value, value_size);
	store_len = value_size;
	store_present = true;
	return 0;
}

static int fake_delete_one(void *context, const char *name)
{
	(void)context;
	assert(name != NULL);
	if (fail_delete) {
		return -EIO;
	}
	store_present = false;
	store_len = 0U;
	return 0;
}

static const struct linkr_debugger_task_backend_ops fake_backend = {
	.load_one = fake_load_one,
	.save_one = fake_save_one,
	.delete_one = fake_delete_one,
};

static const enum linkr_debugger_task_http_route tasks_route =
	LINKR_DEBUGGER_TASK_HTTP_ROUTE_TASKS;
static const enum linkr_debugger_task_http_route catalog_route =
	LINKR_DEBUGGER_TASK_HTTP_ROUTE_CATALOG;

static const struct http_header sentinel_headers[] = { { "X-Sentinel", "sentinel" } };
static const char sentinel_body[] = "sentinel";

static void reset_all(void)
{
	capture_acquire_calls = 0U;
	capture_release_calls = 0U;
	flash_acquire_calls = 0U;
	flash_release_calls = 0U;
	capture_busy = false;
	flash_busy = false;
	store_len = 0U;
	store_present = false;
	fail_load = false;
	fail_save = false;
	fail_delete = false;
	linkr_debugger_task_test_set_backend(&fake_backend, NULL);
	linkr_debugger_task_init();
}

static void init_client(struct http_client_ctx *client, enum http_method method)
{
	memset(client, 0, sizeof(*client));
	client->method = method;
}

static void set_response_sentinel(struct http_response_ctx *response)
{
	response->status = (enum http_status)599;
	response->headers = sentinel_headers;
	response->header_count = ARRAY_SIZE_LOCAL(sentinel_headers);
	response->body = (const uint8_t *)sentinel_body;
	response->body_len = sizeof(sentinel_body) - 1U;
	response->final_chunk = false;
}

static void assert_response_sentinel(const struct http_response_ctx *response)
{
	assert(response->status == (enum http_status)599);
	assert(response->headers == sentinel_headers);
	assert(response->header_count == ARRAY_SIZE_LOCAL(sentinel_headers));
	assert(response->body == (const uint8_t *)sentinel_body);
	assert(response->body_len == sizeof(sentinel_body) - 1U);
	assert(!response->final_chunk);
}

static void call_task(struct http_client_ctx *client,
		      enum http_transaction_status transaction_status,
		      const void *data, size_t data_len,
		      const enum linkr_debugger_task_http_route *route,
		      struct http_response_ctx *response)
{
	struct http_request_ctx request = {
		.data = (uint8_t *)data,
		.data_len = data_len,
		.headers = NULL,
		.header_count = 0U,
		.headers_status = HTTP_HEADER_STATUS_NONE,
	};

	assert(linkr_debugger_task_http_handle(
		       client, transaction_status, &request, response, (void *)route) == 0);
}

static void call_final(enum http_method method, const void *data, size_t data_len,
		       struct http_response_ctx *response)
{
	struct http_client_ctx client;

	init_client(&client, method);
	set_response_sentinel(response);
	call_task(&client, HTTP_SERVER_REQUEST_DATA_FINAL, data, data_len,
		  &tasks_route, response);
}

static void response_text(const struct http_response_ctx *response, char text[RESPONSE_TEXT_CAP])
{
	assert(response->body != NULL);
	assert(response->body_len < RESPONSE_TEXT_CAP);
	memcpy(text, response->body, response->body_len);
	text[response->body_len] = '\0';
}

static void assert_well_formed_response(const struct http_response_ctx *response,
					enum http_status expected_status)
{
	assert(response->status == expected_status);
	assert(response->headers != NULL);
	assert(response->header_count == 1U);
	assert(strcmp(response->headers[0].name, "Cache-Control") == 0);
	assert(strcmp(response->headers[0].value, "no-store") == 0);
	assert(response->final_chunk);
	assert(response->body != NULL);
	assert(response->body_len > 0U);
	assert(response->body[response->body_len - 1U] == '\n');
}

static void assert_error_response(const struct http_response_ctx *response,
				  enum http_status expected_status, const char *expected_code)
{
	char text[RESPONSE_TEXT_CAP];

	assert_well_formed_response(response, expected_status);
	response_text(response, text);
	assert(strstr(text, "\"schema\":\"radxa-linkr-debugger.v1\"") != NULL);
	assert(strstr(text, "\"ok\":false") != NULL);
	assert(strstr(text, "\"command\":\"task\"") != NULL);
	assert(strstr(text, expected_code) != NULL);
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

static size_t json_quote_blob(const char *blob, size_t blob_len, char *out, size_t out_cap)
{
	size_t len = 0U;

	out[len++] = '"';
	for (size_t i = 0U; i < blob_len; i++) {
		const char *escaped = NULL;

		switch (blob[i]) {
		case '"': escaped = "\\\""; break;
		case '\\': escaped = "\\\\"; break;
		case '\n': escaped = "\\n"; break;
		case '\r': escaped = "\\r"; break;
		case '\t': escaped = "\\t"; break;
		default: break;
		}
		if (escaped != NULL) {
			size_t escaped_len = strlen(escaped);

			assert(len + escaped_len < out_cap);
			memcpy(out + len, escaped, escaped_len);
			len += escaped_len;
		} else {
			assert(len + 1U < out_cap);
			out[len++] = blob[i];
		}
	}
	assert(len + 1U < out_cap);
	out[len++] = '"';
	return len;
}

static void get_and_assert_blob_equals(const char *expected, size_t expected_len)
{
	struct http_response_ctx response;
	char text[RESPONSE_TEXT_CAP];
	char decoded[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
	size_t decoded_len;

	call_final(HTTP_GET, NULL, 0U, &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	response_text(&response, text);
	assert(strstr(text, "\"action\":\"list\"") != NULL);
	extract_blob(text, decoded, sizeof(decoded), &decoded_len);
	assert(decoded_len == expected_len);
	assert(memcmp(decoded, expected, expected_len) == 0);
}

static void test_get_empty_returns_frozen_empty_contract(void)
{
	struct http_response_ctx response;
	char text[RESPONSE_TEXT_CAP];
	const char *expected =
		"{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,\"command\":\"task\","
		"\"action\":\"list\",\"backend\":{\"available\":true},\"task_count\":0,"
		"\"tasks\":[],\"blob\":\"\"}\n";

	reset_all();
	call_final(HTTP_GET, NULL, 0U, &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	response_text(&response, text);
	assert(strcmp(text, expected) == 0);
	assert(capture_acquire_calls == 0U);
	assert(flash_acquire_calls == 0U);
}

static void test_get_with_stored_blob_returns_summaries_and_exact_blob(void)
{
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task alpha\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/power/12v_out\",\"body\":\"{\\\"state\\\":\\\"on\\\"}\",\"wait_ms\":250}\n"
		"# task beta\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/switch/sd\",\"body\":\"{\\\"route\\\":\\\"usb-reader\\\"}\"}\n";
	struct http_response_ctx response;
	char text[RESPONSE_TEXT_CAP];

	reset_all();
	assert(linkr_debugger_task_tasks_store(blob, sizeof(blob) - 1U) ==
	       LINKR_DEBUGGER_TASK_OK);
	capture_acquire_calls = 0U;
	flash_acquire_calls = 0U;
	call_final(HTTP_GET, NULL, 0U, &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	response_text(&response, text);
	assert(strstr(text, "\"schema\":\"radxa-linkr-debugger.v1\"") != NULL);
	assert(strstr(text, "\"ok\":true") != NULL);
	assert(strstr(text, "\"command\":\"task\"") != NULL);
	assert(strstr(text, "\"action\":\"list\"") != NULL);
	assert(strstr(text, "\"backend\":{\"available\":true}") != NULL);
	assert(strstr(text, "\"task_count\":2") != NULL);
	assert(strstr(text, "\"id\":\"alpha\"") != NULL);
	assert(strstr(text, "\"name\":\"alpha\"") != NULL);
	assert(strstr(text, "\"request_count\":1") != NULL);
	assert(strstr(text, "\"id\":\"beta\"") != NULL);
	assert(capture_acquire_calls == 0U);
	assert(flash_acquire_calls == 0U);
	get_and_assert_blob_equals(blob, sizeof(blob) - 1U);
	assert(capture_acquire_calls == 0U);
	assert(flash_acquire_calls == 0U);
}

static void test_put_then_get_max_blob_round_trip(void)
{
	char blob[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
	struct http_response_ctx response;
	char text[RESPONSE_TEXT_CAP];

	reset_all();
	fill_maximum_blob(blob);
	call_final(HTTP_PUT, blob, sizeof(blob), &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	response_text(&response, text);
	assert(strstr(text, "\"ok\":true") != NULL);
	assert(strstr(text, "\"command\":\"task\"") != NULL);
	assert(strstr(text, "\"action\":\"store\"") != NULL);
	assert(strstr(text, "\"stored\":true") != NULL);
	get_and_assert_blob_equals(blob, sizeof(blob));
}

static void test_put_chunked_upload_round_trip(void)
{
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task chunked\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP11\",\"body\":\"{\\\"direction\\\":\\\"output\\\",\\\"value\\\":1}\",\"wait_ms\":10}\n";
	const size_t split = 17U;
	struct http_client_ctx client;
	struct http_response_ctx response;
	char text[RESPONSE_TEXT_CAP];

	reset_all();
	init_client(&client, HTTP_PUT);
	set_response_sentinel(&response);
	call_task(&client, HTTP_SERVER_REQUEST_DATA_MORE, blob, split,
		  &tasks_route, &response);
	assert_response_sentinel(&response);
	set_response_sentinel(&response);
	call_task(&client, HTTP_SERVER_REQUEST_DATA_FINAL, blob + split,
		  sizeof(blob) - 1U - split, &tasks_route, &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	response_text(&response, text);
	assert(strstr(text, "\"action\":\"store\"") != NULL);
	get_and_assert_blob_equals(blob, sizeof(blob) - 1U);
}

static void test_put_oversized_returns_413_and_upload_recovers(void)
{
	char oversized[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE + 1U];
	struct http_response_ctx response;
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task small\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":\"{\\\"state\\\":\\\"off\\\"}\"}\n";

	reset_all();
	memset(oversized, 'x', sizeof(oversized));
	call_final(HTTP_PUT, oversized, sizeof(oversized), &response);
	assert_error_response(&response, HTTP_413_PAYLOAD_TOO_LARGE,
			      "\"code\":\"body_too_large\"");
	call_final(HTTP_PUT, blob, sizeof(blob) - 1U, &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	get_and_assert_blob_equals(blob, sizeof(blob) - 1U);
}

static void test_put_invalid_blob_returns_400(void)
{
	static const char blob[] = "# linkr-task.v1\n# task broken\nnot-a-request\n";
	struct http_response_ctx response;

	reset_all();
	call_final(HTTP_PUT, blob, sizeof(blob) - 1U, &response);
	assert_error_response(&response, HTTP_400_BAD_REQUEST, "\"code\":\"invalid_blob\"");
}

static void test_put_storage_failure_returns_500(void)
{
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task small\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":\"{\\\"state\\\":\\\"off\\\"}\"}\n";
	struct http_response_ctx response;

	reset_all();
	fail_save = true;
	call_final(HTTP_PUT, blob, sizeof(blob) - 1U, &response);
	assert_error_response(&response, HTTP_500_INTERNAL_SERVER_ERROR,
			      "\"code\":\"storage_error\"");
}

static void test_put_backend_unavailable_returns_500(void)
{
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task small\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":\"{\\\"state\\\":\\\"off\\\"}\"}\n";
	struct http_response_ctx response;

	reset_all();
	fail_load = true;
	linkr_debugger_task_init();
	call_final(HTTP_PUT, blob, sizeof(blob) - 1U, &response);
	assert_error_response(&response, HTTP_500_INTERNAL_SERVER_ERROR,
			      "\"code\":\"backend_unavailable\"");
}

static void test_put_capture_busy_returns_409(void)
{
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task small\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":\"{\\\"state\\\":\\\"off\\\"}\"}\n";
	struct http_response_ctx response;

	reset_all();
	capture_busy = true;
	call_final(HTTP_PUT, blob, sizeof(blob) - 1U, &response);
	assert_error_response(&response, HTTP_409_CONFLICT, "\"code\":\"busy\"");
	assert(flash_acquire_calls == 0U);
}

static void test_put_flash_busy_returns_409_and_releases_capture(void)
{
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task small\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":\"{\\\"state\\\":\\\"off\\\"}\"}\n";
	struct http_response_ctx response;

	reset_all();
	flash_busy = true;
	call_final(HTTP_PUT, blob, sizeof(blob) - 1U, &response);
	assert_error_response(&response, HTTP_409_CONFLICT, "\"code\":\"busy\"");
	assert(capture_acquire_calls == 1U);
	assert(capture_release_calls == 1U);
}

static void test_put_missing_body_returns_400(void)
{
	struct http_response_ctx response;

	reset_all();
	call_final(HTTP_PUT, NULL, 0U, &response);
	assert_error_response(&response, HTTP_400_BAD_REQUEST, "\"code\":\"missing_body\"");
}

static void test_put_json_quoted_body_is_unescaped(void)
{
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task quoted\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP12\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":5}\n";
	char quoted[2U * sizeof(blob) + 2U];
	size_t quoted_len;
	struct http_response_ctx response;

	reset_all();
	quoted_len = json_quote_blob(blob, sizeof(blob) - 1U, quoted, sizeof(quoted));
	call_final(HTTP_PUT, quoted, quoted_len, &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	get_and_assert_blob_equals(blob, sizeof(blob) - 1U);
}

static void test_put_json_quoted_body_accepts_unicode(void)
{
	static const char quoted[] =
		"\"# linkr-task.v1\\n"
		"# task unicode\\n"
		"# \\u00e9 \\uD83D\\uDE80 raw \xc3\xa9 \xf0\x9f\x9a\x80\\n"
		"{\\\"method\\\":\\\"PUT\\\",\\\"path\\\":\\\"/api/v1/gpio/GP12\\\","
		"\\\"body\\\":\\\"{\\\\\\\"direction\\\\\\\":\\\\\\\"input\\\\\\\"}\\\","
		"\\\"wait_ms\\\":5}\\n\"";
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task unicode\n"
		"# \xc3\xa9 \xf0\x9f\x9a\x80 raw \xc3\xa9 \xf0\x9f\x9a\x80\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP12\","
		"\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":5}\n";
	struct http_response_ctx response;

	reset_all();
	call_final(HTTP_PUT, quoted, sizeof(quoted) - 1U, &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	get_and_assert_blob_equals(blob, sizeof(blob) - 1U);
}

static void test_put_json_quoted_body_rejects_invalid_tokens(void)
{
	static const char *const invalid[] = {
		"\"# linkr-task.v1\\n# task invalid\\n# \\q\\n"
		"{\\\"method\\\":\\\"PUT\\\",\\\"path\\\":\\\"/api/v1/gpio/GP12\\\","
		"\\\"body\\\":\\\"{\\\\\\\"direction\\\\\\\":\\\\\\\"input\\\\\\\"}\\\"}\\n\"",
		"\"# linkr-task.v1\\n# task invalid\\n# \\u12\\n"
		"{\\\"method\\\":\\\"PUT\\\",\\\"path\\\":\\\"/api/v1/gpio/GP12\\\","
		"\\\"body\\\":\\\"{\\\\\\\"direction\\\\\\\":\\\\\\\"input\\\\\\\"}\\\"}\\n\"",
		"\"# linkr-task.v1\\n# task invalid\\n# \\uD800\\n"
		"{\\\"method\\\":\\\"PUT\\\",\\\"path\\\":\\\"/api/v1/gpio/GP12\\\","
		"\\\"body\\\":\\\"{\\\\\\\"direction\\\\\\\":\\\\\\\"input\\\\\\\"}\\\"}\\n\"",
		"\"# linkr-task.v1\\n# task invalid\\n# slash\\\"",
		"\"# linkr-task.v1\\n# task invalid\\n# quote\" trailing\"",
	};
	struct http_response_ctx response;

	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(invalid); i++) {
		reset_all();
		call_final(HTTP_PUT, invalid[i], strlen(invalid[i]), &response);
		assert_error_response(&response, HTTP_400_BAD_REQUEST,
				      "\"code\":\"invalid_body\"");
	}
}

static void test_put_c0_control_byte_returns_400_and_nothing_persisted(void)
{
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task c0\n"
		"#\x01 raw control\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":\"{\\\"state\\\":\\\"off\\\"}\"}\n";
	struct http_response_ctx response;
	char text[RESPONSE_TEXT_CAP];

	reset_all();
	call_final(HTTP_PUT, blob, sizeof(blob) - 1U, &response);
	assert_error_response(&response, HTTP_400_BAD_REQUEST, "\"code\":\"invalid_blob\"");
	call_final(HTTP_GET, NULL, 0U, &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	response_text(&response, text);
	assert(strstr(text, "\"task_count\":0,\"tasks\":[],\"blob\":\"\"") != NULL);
}

static void test_get_discards_invalid_utf8_from_storage(void)
{
	static const char invalid_blob[] =
		"# linkr-task.v1\n"
		"# \xc0\x80 retained comment\n"
		"# task valid\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"}\n";
	struct http_response_ctx response;
	char text[RESPONSE_TEXT_CAP];

	reset_all();
	memcpy(store_buf, invalid_blob, sizeof(invalid_blob) - 1U);
	store_len = sizeof(invalid_blob) - 1U;
	store_present = true;
	linkr_debugger_task_init();
	call_final(HTTP_GET, NULL, 0U, &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	response_text(&response, text);
	assert(linkr_debugger_json_utf8_valid(text, response.body_len));
	assert(strstr(text, "\"task_count\":0,\"tasks\":[],\"blob\":\"\"") != NULL);
}

static void test_put_tab_and_crlf_blob_round_trip(void)
{
	static const char blob[] =
		"# linkr-task.v1\r\n"
		"# task tabs\r\n"
		"#\tindented comment\r\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP13\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":0}\r\n";
	struct http_response_ctx response;

	reset_all();
	call_final(HTTP_PUT, blob, sizeof(blob) - 1U, &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	get_and_assert_blob_equals(blob, sizeof(blob) - 1U);
}

static void test_delete_clears_blob_and_get_returns_empty(void)
{
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task small\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":\"{\\\"state\\\":\\\"off\\\"}\"}\n";
	struct http_response_ctx response;
	char text[RESPONSE_TEXT_CAP];

	reset_all();
	assert(linkr_debugger_task_tasks_store(blob, sizeof(blob) - 1U) ==
	       LINKR_DEBUGGER_TASK_OK);
	call_final(HTTP_DELETE, NULL, 0U, &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	response_text(&response, text);
	assert(strstr(text, "\"ok\":true") != NULL);
	assert(strstr(text, "\"command\":\"task\"") != NULL);
	assert(strstr(text, "\"action\":\"clear\"") != NULL);
	assert(strstr(text, "\"cleared\":true") != NULL);
	call_final(HTTP_GET, NULL, 0U, &response);
	response_text(&response, text);
	assert(strstr(text, "\"task_count\":0,\"tasks\":[],\"blob\":\"\"") != NULL);
}

static void test_delete_busy_returns_409(void)
{
	struct http_response_ctx response;

	reset_all();
	flash_busy = true;
	call_final(HTTP_DELETE, NULL, 0U, &response);
	assert_error_response(&response, HTTP_409_CONFLICT, "\"code\":\"busy\"");
	assert(capture_release_calls == 1U);
	assert(flash_release_calls == 0U);
}

static void test_post_returns_405(void)
{
	struct http_client_ctx client;
	struct http_response_ctx response;

	reset_all();
	call_final(HTTP_POST, NULL, 0U, &response);
	assert_error_response(&response, HTTP_405_METHOD_NOT_ALLOWED,
			      "\"code\":\"method_not_allowed\"");
	init_client(&client, HTTP_POST);
	set_response_sentinel(&response);
	call_task(&client, HTTP_SERVER_REQUEST_DATA_MORE, "x", 1U,
		  &tasks_route, &response);
	assert_error_response(&response, HTTP_405_METHOD_NOT_ALLOWED,
			      "\"code\":\"method_not_allowed\"");
}

static void test_catalog_get_returns_immutable_response(void)
{
	const uint8_t *catalog;
	size_t catalog_len;
	struct http_client_ctx client;
	struct http_response_ctx response;

	reset_all();
	init_client(&client, HTTP_GET);
	set_response_sentinel(&response);
	call_task(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U,
		  &catalog_route, &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	catalog = linkr_debugger_task_catalog_json(&catalog_len);
	assert(response.body == catalog);
	assert(response.body_len == catalog_len);
	assert(strstr((const char *)response.body, "\"action\":\"catalog\"") != NULL);
	assert(capture_acquire_calls == 0U);
	assert(flash_acquire_calls == 0U);
}

static void test_catalog_non_get_returns_405(void)
{
	struct http_client_ctx client;
	struct http_response_ctx response;

	reset_all();
	init_client(&client, HTTP_POST);
	set_response_sentinel(&response);
	call_task(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U,
		  &catalog_route, &response);
	assert_error_response(&response, HTTP_405_METHOD_NOT_ALLOWED,
		      "\"code\":\"method_not_allowed\"");
}

static void test_unknown_route_returns_404(void)
{
	static const enum linkr_debugger_task_http_route bogus_route =
		(enum linkr_debugger_task_http_route)99;
	struct http_client_ctx client;
	struct http_response_ctx response;

	reset_all();
	init_client(&client, HTTP_GET);
	set_response_sentinel(&response);
	call_task(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U,
		  &bogus_route, &response);
	assert_error_response(&response, HTTP_404_NOT_FOUND, "\"code\":\"not_found\"");
}

static void test_concurrent_upload_conflict_returns_409(void)
{
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task first\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/power/20v_out\",\"body\":\"{\\\"state\\\":\\\"on\\\"}\"}\n";
	const size_t split = 11U;
	struct http_client_ctx client_a;
	struct http_client_ctx client_b;
	struct http_response_ctx response;

	reset_all();
	init_client(&client_a, HTTP_PUT);
	init_client(&client_b, HTTP_PUT);
	set_response_sentinel(&response);
	call_task(&client_a, HTTP_SERVER_REQUEST_DATA_MORE, blob, split,
		  &tasks_route, &response);
	assert_response_sentinel(&response);
	call_task(&client_b, HTTP_SERVER_REQUEST_DATA_MORE, blob, split,
		  &tasks_route, &response);
	assert_error_response(&response, HTTP_409_CONFLICT, "\"code\":\"busy\"");
	set_response_sentinel(&response);
	call_task(&client_a, HTTP_SERVER_REQUEST_DATA_FINAL, blob + split,
		  sizeof(blob) - 1U - split, &tasks_route, &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	get_and_assert_blob_equals(blob, sizeof(blob) - 1U);
}

static void test_aborted_upload_resets_state(void)
{
	static const char partial[] = "# linkr-task.v1\n# task truncated\n";
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task final\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/switch/usb\",\"body\":\"{\\\"route\\\":\\\"pc\\\"}\"}\n";
	struct http_client_ctx client;
	struct http_response_ctx response;

	reset_all();
	init_client(&client, HTTP_PUT);
	set_response_sentinel(&response);
	call_task(&client, HTTP_SERVER_REQUEST_DATA_MORE, partial,
		  sizeof(partial) - 1U, &tasks_route, &response);
	assert_response_sentinel(&response);
	set_response_sentinel(&response);
	call_task(&client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, 0U,
		  &tasks_route, &response);
	assert_response_sentinel(&response);
	call_final(HTTP_PUT, blob, sizeof(blob) - 1U, &response);
	assert_well_formed_response(&response, HTTP_200_OK);
	get_and_assert_blob_equals(blob, sizeof(blob) - 1U);
}

static void test_foreign_upload_terminal_does_not_reset_owner(void)
{
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task owner\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/switch/usb\","
		"\"body\":\"{\\\"route\\\":\\\"pc\\\"}\"}\n";
	const size_t split = 19U;
	const enum http_transaction_status terminal_statuses[] = {
		HTTP_SERVER_TRANSACTION_ABORTED,
		HTTP_SERVER_TRANSACTION_COMPLETE,
	};
	struct http_client_ctx client_a;
	struct http_client_ctx client_b;
	struct http_response_ctx response;

	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(terminal_statuses); i++) {
		reset_all();
		init_client(&client_a, HTTP_PUT);
		init_client(&client_b, HTTP_PUT);
		set_response_sentinel(&response);
		call_task(&client_a, HTTP_SERVER_REQUEST_DATA_MORE, blob, split,
			  &tasks_route, &response);
		assert_response_sentinel(&response);
		set_response_sentinel(&response);
		call_task(&client_b, terminal_statuses[i], NULL, 0U, &tasks_route, &response);
		assert_response_sentinel(&response);
		set_response_sentinel(&response);
		call_task(&client_a, HTTP_SERVER_REQUEST_DATA_FINAL, blob + split,
			  sizeof(blob) - 1U - split, &tasks_route, &response);
		assert_well_formed_response(&response, HTTP_200_OK);
		get_and_assert_blob_equals(blob, sizeof(blob) - 1U);
	}
}

int main(void)
{
	test_get_empty_returns_frozen_empty_contract();
	test_get_with_stored_blob_returns_summaries_and_exact_blob();
	test_put_then_get_max_blob_round_trip();
	test_put_chunked_upload_round_trip();
	test_put_oversized_returns_413_and_upload_recovers();
	test_put_invalid_blob_returns_400();
	test_put_storage_failure_returns_500();
	test_put_backend_unavailable_returns_500();
	test_put_capture_busy_returns_409();
	test_put_flash_busy_returns_409_and_releases_capture();
	test_put_missing_body_returns_400();
	test_put_json_quoted_body_is_unescaped();
	test_put_json_quoted_body_accepts_unicode();
	test_put_json_quoted_body_rejects_invalid_tokens();
	test_put_c0_control_byte_returns_400_and_nothing_persisted();
	test_get_discards_invalid_utf8_from_storage();
	test_put_tab_and_crlf_blob_round_trip();
	test_delete_clears_blob_and_get_returns_empty();
	test_delete_busy_returns_409();
	test_post_returns_405();
	test_catalog_get_returns_immutable_response();
	test_catalog_non_get_returns_405();
	test_unknown_route_returns_404();
	test_concurrent_upload_conflict_returns_409();
	test_aborted_upload_resets_state();
	test_foreign_upload_terminal_does_not_reset_owner();
	puts("linkr_debugger_task_http: all tests passed");
	return 0;
}
