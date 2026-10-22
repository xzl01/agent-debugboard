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
#include "../../src/linkr_debugger_json_value.h"
#include "../../src/linkr_debugger_task.h"
#include "../../src/linkr_debugger_task_catalog.h"
#include "../../src/linkr_debugger_task_mutation.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define ENTRY_COUNT 64U
#define RESPONSE_COPY_CAP LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE

const char *linkr_debugger_json_schema(void)
{
	return "radxa-linkr-debugger.v1";
}

static bool capture_busy;
static bool flash_busy;
static unsigned int capture_acquire_calls;

bool linkr_debugger_capture_arbiter_try_acquire(enum linkr_debugger_capture_owner owner)
{
	assert(owner == LINKR_DEBUGGER_CAPTURE_OWNER_TASK);
	capture_acquire_calls++;
	return !capture_busy;
}

bool linkr_debugger_capture_arbiter_release(enum linkr_debugger_capture_owner owner)
{
	assert(owner == LINKR_DEBUGGER_CAPTURE_OWNER_TASK);
	return true;
}

bool linkr_debugger_flash_arbiter_try_acquire(enum linkr_debugger_flash_owner owner)
{
	assert(owner == LINKR_DEBUGGER_FLASH_OWNER_TASK);
	return !flash_busy;
}

bool linkr_debugger_flash_arbiter_release(enum linkr_debugger_flash_owner owner)
{
	assert(owner == LINKR_DEBUGGER_FLASH_OWNER_TASK);
	return true;
}

struct memory_entry {
	bool present;
	size_t len;
	uint8_t data[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE];
	char name[48];
};

static struct memory_entry entries[ENTRY_COUNT];
static bool force_enospc;
static uint8_t random_byte;

static struct memory_entry *memory_find(const char *name)
{
	for (size_t i = 0U; i < ENTRY_COUNT; i++) {
		if (entries[i].present && strcmp(entries[i].name, name) == 0) {
			return &entries[i];
		}
	}
	return NULL;
}

static int memory_subsys_init(void *context)
{
	(void)context;
	return 0;
}

static int memory_load_one(void *context, const char *name, void *value, size_t cap)
{
	struct memory_entry *entry;
	size_t copied;

	(void)context;
	entry = memory_find(name);
	if (entry == NULL) {
		return 0;
	}
	copied = entry->len < cap ? entry->len : cap;
	memcpy(value, entry->data, copied);
	return (int)entry->len;
}

static int memory_save_one(void *context, const char *name, const void *value,
			   size_t len)
{
	struct memory_entry *entry;

	(void)context;
	if (force_enospc) {
		force_enospc = false;
		return -ENOSPC;
	}
	assert(len <= LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE);
	entry = memory_find(name);
	if (entry == NULL) {
		for (size_t i = 0U; i < ENTRY_COUNT; i++) {
			if (!entries[i].present) {
				entry = &entries[i];
				break;
			}
		}
		assert(entry != NULL);
		(void)snprintf(entry->name, sizeof(entry->name), "%s", name);
		entry->present = true;
	}
	memcpy(entry->data, value, len);
	entry->len = len;
	return 0;
}

static int memory_delete_one(void *context, const char *name)
{
	struct memory_entry *entry;

	(void)context;
	entry = memory_find(name);
	if (entry != NULL) {
		entry->present = false;
		entry->len = 0U;
	}
	return 0;
}

static void memory_random(void *context, void *value, size_t len)
{
	uint8_t *bytes = value;

	(void)context;
	for (size_t i = 0U; i < len; i++) {
		bytes[i] = ++random_byte;
	}
}

static const struct linkr_debugger_task_backend_ops memory_backend = {
	.subsys_init = memory_subsys_init,
	.load_one = memory_load_one,
	.save_one = memory_save_one,
	.delete_one = memory_delete_one,
	.random = memory_random,
};

static const enum linkr_debugger_task_http_route route_tasks =
	LINKR_DEBUGGER_TASK_HTTP_ROUTE_TASKS;
static const enum linkr_debugger_task_http_route route_catalog =
	LINKR_DEBUGGER_TASK_HTTP_ROUTE_CATALOG;
static const enum linkr_debugger_task_http_route route_ir =
	LINKR_DEBUGGER_TASK_HTTP_ROUTE_IR;
static const enum linkr_debugger_task_http_route route_mutations =
	LINKR_DEBUGGER_TASK_HTTP_ROUTE_MUTATIONS;
static const enum linkr_debugger_task_http_route route_source_0 =
	LINKR_DEBUGGER_TASK_HTTP_ROUTE_SOURCE_0;

static const struct http_header mutation_headers[] = {
	{ .name = "Content-Type", .value = LINKR_DEBUGGER_TASK_MUTATION_CONTENT_TYPE },
};

static void reset_store(void)
{
	memset(entries, 0, sizeof(entries));
	capture_busy = false;
	flash_busy = false;
	capture_acquire_calls = 0U;
	force_enospc = false;
	random_byte = 0U;
	linkr_debugger_task_test_set_backend(&memory_backend, NULL);
	linkr_debugger_task_init();
}

static void init_client(struct http_client_ctx *client, enum http_method method)
{
	memset(client, 0, sizeof(*client));
	client->method = method;
}

static void call_route(struct http_client_ctx *client,
		       enum http_transaction_status status,
		       const void *data, size_t data_len,
		       const struct http_header *headers, size_t header_count,
		       const enum linkr_debugger_task_http_route *route,
		       struct http_response_ctx *response)
{
	struct http_request_ctx request = {
		.data = (uint8_t *)data,
		.data_len = data_len,
		.headers = (struct http_header *)headers,
		.header_count = header_count,
		.headers_status = HTTP_HEADER_STATUS_OK,
	};

	assert(linkr_debugger_task_http_handle(client, status, &request, response,
					       (void *)route) == 0);
}

static void complete(struct http_client_ctx *client,
		     const enum linkr_debugger_task_http_route *route)
{
	struct http_response_ctx ignored = { 0 };

	call_route(client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, 0U, NULL, 0U,
		   route, &ignored);
}

static void abort_transaction(struct http_client_ctx *client,
			      const enum linkr_debugger_task_http_route *route)
{
	struct http_response_ctx ignored = { 0 };

	call_route(client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, 0U, NULL, 0U,
		   route, &ignored);
}

static void copy_response(const struct http_response_ctx *response,
			  char text[RESPONSE_COPY_CAP])
{
	assert(response->body_len < RESPONSE_COPY_CAP);
	memcpy(text, response->body, response->body_len);
	text[response->body_len] = '\0';
}

static void assert_json_status(const struct http_response_ctx *response,
			       enum http_status status, const char *needle)
{
	char text[RESPONSE_COPY_CAP];

	assert(response->status == status && response->final_chunk);
	assert(response->header_count == 2U);
	assert(strcmp(response->headers[0].name, "Cache-Control") == 0);
	assert(strcmp(response->headers[0].value, "no-store") == 0);
	assert(strcmp(response->headers[1].name, "Content-Type") == 0);
	assert(strcmp(response->headers[1].value, "application/json") == 0);
	copy_response(response, text);
	assert(strstr(text, needle) != NULL);
}

static void assert_content_type(const struct http_response_ctx *response,
				const char *expected)
{
	assert(response->header_count == 2U);
	assert(strcmp(response->headers[1].name, "Content-Type") == 0);
	assert(strcmp(response->headers[1].value, expected) == 0);
}

struct transaction_output {
	char body[2U * RESPONSE_COPY_CAP];
	size_t len;
	unsigned int chunks;
};

static void transaction_output_add(struct transaction_output *output,
				   const struct http_response_ctx *response)
{
	if (!response->final_chunk) {
		assert(response->body == NULL && response->body_len == 0U);
		return;
	}
	assert(response->body != NULL);
	assert(output->len + response->body_len < sizeof(output->body));
	memcpy(output->body + output->len, response->body, response->body_len);
	output->len += response->body_len;
	output->body[output->len] = '\0';
	output->chunks++;
}

static void assert_one_cmd_json(const struct transaction_output *output,
				const char *ok_field, const char *detail)
{
	assert(output->chunks == 1U);
	assert(linkr_debugger_json_value_valid(output->body, 16U, 193U));
	assert(strstr(output->body, "\"schema\":\"radxa-linkr-debugger.v1\"") != NULL);
	assert(strstr(output->body, "\"command\":\"tasks\"") != NULL);
	assert(strstr(output->body, ok_field) != NULL);
	assert(strstr(output->body, detail) != NULL);
	if (strcmp(ok_field, "\"ok\":false") == 0) {
		assert(strstr(output->body, "\"error\":{") != NULL);
		assert(strstr(output->body, "\"code\":") != NULL);
		assert(strstr(output->body, "\"message\":") != NULL);
	}
}

/* Minimal JSON escaping for task IDs only; source/IR travel as canonical
 * base64 and never need JSON escaping. */
static size_t json_escape(char *out, size_t cap, const char *text)
{
	size_t written = 0U;

	for (size_t i = 0U; text[i] != '\0'; i++) {
		char ch = text[i];

		assert(written + 3U < cap);
		if (ch == '"' || ch == '\\') {
			out[written++] = '\\';
		}
		out[written++] = ch;
	}
	out[written] = '\0';
	return written;
}

/* Canonical RFC 4648 base64 with padding and zero pad bits. */
static size_t base64_encode(const uint8_t *data, size_t len, char *out,
			    size_t cap)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t written = 0U;

	assert(cap >= ((len + 2U) / 3U) * 4U + 1U);
	for (size_t i = 0U; i < len; i += 3U) {
		uint32_t chunk = (uint32_t)data[i] << 16;
		size_t remain = len - i;

		if (remain > 1U) {
			chunk |= (uint32_t)data[i + 1U] << 8;
		}
		if (remain > 2U) {
			chunk |= data[i + 2U];
		}
		out[written++] = alphabet[(chunk >> 18) & 0x3fU];
		out[written++] = alphabet[(chunk >> 12) & 0x3fU];
		out[written++] = remain > 1U ? alphabet[(chunk >> 6) & 0x3fU] : '=';
		out[written++] = remain > 2U ? alphabet[chunk & 0x3fU] : '=';
	}
	out[written] = '\0';
	return written;
}

static size_t mutation_clear(uint8_t *body, size_t cap, uint64_t revision,
			     const uint8_t catalog[32], bool bad_catalog)
{
	char catalog_hex[2U * LINKR_DEBUGGER_TASK_SHA256_SIZE + 1U];
	int written;

	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_SHA256_SIZE; i++) {
		uint8_t byte = catalog[i];

		if (bad_catalog && i == 0U) {
			byte ^= 1U;
		}
		(void)snprintf(catalog_hex + 2U * i, 3U, "%02x", byte);
	}
	written = snprintf((char *)body, cap,
		"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"clear\","
		"\"expected_revision\":\"%llu\",\"catalog_fingerprint\":\"%s\"}",
		(unsigned long long)revision, catalog_hex);
	assert(written > 0 && (size_t)written < cap);
	return (size_t)written;
}

static size_t mutation_upsert(uint8_t *body, size_t cap, const char *id,
			      const char *source, const char *ir,
			      uint64_t revision, const uint8_t catalog[32])
{
	char catalog_hex[2U * LINKR_DEBUGGER_TASK_SHA256_SIZE + 1U];
	static char encoded[(((LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE + 2U) / 3U) * 4U) + 1U];
	char escaped_id[4U * LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN];
	int written;
	size_t len;

	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_SHA256_SIZE; i++) {
		(void)snprintf(catalog_hex + 2U * i, 3U, "%02x", catalog[i]);
	}
	(void)json_escape(escaped_id, sizeof(escaped_id), id);
	written = snprintf((char *)body, cap,
		"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"upsert\","
		"\"expected_revision\":\"%llu\",\"catalog_fingerprint\":\"%s\","
		"\"id\":\"%s\"",
		(unsigned long long)revision, catalog_hex, escaped_id);
	assert(written > 0 && (size_t)written < cap);
	len = (size_t)written;
	if (source != NULL) {
		(void)base64_encode((const uint8_t *)source, strlen(source), encoded,
				    sizeof(encoded));
		written = snprintf((char *)body + len, cap - len,
				   ",\"source_base64\":\"%s\"", encoded);
		assert(written > 0 && (size_t)written < cap - len);
		len += (size_t)written;
	}
	(void)base64_encode((const uint8_t *)ir, strlen(ir), encoded,
			    sizeof(encoded));
	written = snprintf((char *)body + len, cap - len, ",\"ir_base64\":\"%s\"}",
			   encoded);
	assert(written > 0 && (size_t)written < cap - len);
	len += (size_t)written;
	return len;
}

static void test_chunked_mutation_then_metadata_ir_and_source(void)
{
	static const char source[] =
		"{\"schema\":\"linkr-test.v1\",\"name\":\"exact\"}\n";
	static const char ir[] =
		"# linkr-task.v1\n# task A\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP7\",\"body\":\"{}\"}\n";
	struct linkr_debugger_task_status status;
	struct http_client_ctx client;
	struct http_response_ctx response = { 0 };
	uint8_t body[2048];
	char text[RESPONSE_COPY_CAP];
	size_t body_len;

	reset_store();
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	body_len = mutation_upsert(body, sizeof(body), "A", source, ir,
				   status.revision, status.catalog_fingerprint);
	init_client(&client, HTTP_POST);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_MORE, body, 17U,
		   mutation_headers, 1U, &route_mutations, &response);
	assert(response.body == NULL);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, body + 17U, body_len - 17U,
		   NULL, 0U, &route_mutations, &response);
	assert_json_status(&response, HTTP_200_OK, "\"action\":\"upsert\"");
	assert(strstr((const char *)response.body, "\"revision\":\"1\"") != NULL);
	complete(&client, &route_mutations);

	init_client(&client, HTTP_GET);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U, NULL, 0U,
		   &route_tasks, &response);
	assert_json_status(&response, HTTP_200_OK, "\"source_state\":\"present\"");
	copy_response(&response, text);
	assert(strstr(text, "\"blob\"") == NULL);
	complete(&client, &route_tasks);

	init_client(&client, HTTP_GET);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U, NULL, 0U,
		   &route_ir, &response);
	assert(response.status == HTTP_200_OK && response.body_len == sizeof(ir) - 1U);
	assert_content_type(&response, LINKR_DEBUGGER_TASK_IR_CONTENT_TYPE);
	assert(memcmp(response.body, ir, response.body_len) == 0);
	complete(&client, &route_ir);

	init_client(&client, HTTP_GET);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U, NULL, 0U,
		   &route_source_0, &response);
	assert(response.status == HTTP_200_OK && response.body_len == sizeof(source) - 1U);
	assert_content_type(&response, LINKR_DEBUGGER_TASK_SOURCE_CONTENT_TYPE);
	assert(memcmp(response.body, source, response.body_len) == 0);
	complete(&client, &route_source_0);
}

static void test_stale_catalog_busy_storage_and_content_type_errors(void)
{
	struct linkr_debugger_task_status status;
	struct http_client_ctx client;
	struct http_response_ctx response = { 0 };
	struct http_header wrong_type = { .name = "Content-Type", .value = "text/plain" };
	uint8_t body[512];
	size_t body_len;

	reset_store();
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	body_len = mutation_clear(body, sizeof(body), status.revision + 1U,
				  status.catalog_fingerprint, false);
	init_client(&client, HTTP_POST);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, body, body_len,
		   mutation_headers, 1U, &route_mutations, &response);
	assert_json_status(&response, HTTP_409_CONFLICT, "revision_conflict");
	assert(strstr((const char *)response.body, "\"revision\":\"0\"") != NULL);
	complete(&client, &route_mutations);

	body_len = mutation_clear(body, sizeof(body), status.revision,
				  status.catalog_fingerprint, true);
	init_client(&client, HTTP_POST);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, body, body_len,
		   mutation_headers, 1U, &route_mutations, &response);
	assert_json_status(&response, HTTP_409_CONFLICT, "catalog_conflict");
	complete(&client, &route_mutations);

	body_len = mutation_clear(body, sizeof(body), status.revision,
				  status.catalog_fingerprint, false);
	init_client(&client, HTTP_POST);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, body, body_len,
		   &wrong_type, 1U, &route_mutations, &response);
	assert_json_status(&response, HTTP_400_BAD_REQUEST, "invalid_mutation");
	complete(&client, &route_mutations);

	flash_busy = true;
	init_client(&client, HTTP_POST);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, body, body_len,
		   mutation_headers, 1U, &route_mutations, &response);
	assert_json_status(&response, HTTP_409_CONFLICT, "\"code\":\"busy\"");
	complete(&client, &route_mutations);
	flash_busy = false;

	force_enospc = true;
	init_client(&client, HTTP_POST);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, body, body_len,
		   mutation_headers, 1U, &route_mutations, &response);
	assert_json_status(&response, HTTP_507_INSUFFICIENT_STORAGE, "storage_full");
	complete(&client, &route_mutations);
}

static void assert_store_revision(uint64_t revision)
{
	struct linkr_debugger_task_status status;

	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	assert(status.revision == revision);
}

static void test_oversize_discards_until_complete(void)
{
	struct linkr_debugger_task_status status;
	struct http_client_ctx client;
	struct http_response_ctx response = { 0 };
	struct transaction_output failed = { 0 };
	struct transaction_output succeeded = { 0 };
	uint8_t body[LINKR_DEBUGGER_TASK_MUTATION_MAX_SIZE + 1U];
	uint8_t valid[512];
	size_t valid_len;

	reset_store();
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	valid_len = mutation_clear(valid, sizeof(valid), status.revision,
				   status.catalog_fingerprint, false);
	memset(body, 0, sizeof(body));
	init_client(&client, HTTP_POST);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_MORE, body, sizeof(body),
		   mutation_headers, 1U,
		   &route_mutations, &response);
	transaction_output_add(&failed, &response);

	/* The rest of this failed transaction is discarded, even if it is a
	 * complete valid mutation.
	 */
	call_route(&client, HTTP_SERVER_REQUEST_DATA_MORE, valid, valid_len,
		   mutation_headers, 1U, &route_mutations, &response);
	transaction_output_add(&failed, &response);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, valid, valid_len,
		   mutation_headers, 1U, &route_mutations, &response);
	assert_json_status(&response, HTTP_400_BAD_REQUEST, "body_too_large");
	transaction_output_add(&failed, &response);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, valid, valid_len,
		   mutation_headers, 1U, &route_mutations, &response);
	transaction_output_add(&failed, &response);
	assert_one_cmd_json(&failed, "\"ok\":false", "\"code\":\"body_too_large\"");
	assert(capture_acquire_calls == 0U);
	assert_store_revision(0U);

	complete(&client, &route_mutations);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, valid, valid_len,
		   mutation_headers, 1U, &route_mutations, &response);
	assert_json_status(&response, HTTP_200_OK, "\"action\":\"clear\"");
	transaction_output_add(&succeeded, &response);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, valid, valid_len,
		   mutation_headers, 1U, &route_mutations, &response);
	transaction_output_add(&succeeded, &response);
	assert_one_cmd_json(&succeeded, "\"ok\":true", "\"action\":\"clear\"");
	assert(capture_acquire_calls == 1U);
	assert_store_revision(1U);
	complete(&client, &route_mutations);
}

static void test_malformed_discards_until_abort(void)
{
	struct linkr_debugger_task_status status;
	struct http_client_ctx client;
	struct http_response_ctx response = { 0 };
	struct transaction_output failed = { 0 };
	uint8_t malformed[512];
	uint8_t valid[512];
	size_t valid_len;

	reset_store();
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	valid_len = mutation_clear(valid, sizeof(valid), status.revision,
				   status.catalog_fingerprint, false);
	memcpy(malformed, valid, valid_len);
	malformed[0] = 'X';
	init_client(&client, HTTP_POST);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, malformed, valid_len,
		   mutation_headers, 1U, &route_mutations, &response);
	assert_json_status(&response, HTTP_400_BAD_REQUEST, "invalid_mutation");
	transaction_output_add(&failed, &response);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, valid, valid_len,
		   mutation_headers, 1U, &route_mutations, &response);
	transaction_output_add(&failed, &response);
	assert_one_cmd_json(&failed, "\"ok\":false", "\"code\":\"invalid_mutation\"");
	assert(capture_acquire_calls == 0U);
	assert_store_revision(0U);

	abort_transaction(&client, &route_mutations);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, valid, valid_len,
		   mutation_headers, 1U, &route_mutations, &response);
	assert_json_status(&response, HTTP_200_OK, "\"action\":\"clear\"");
	assert(capture_acquire_calls == 1U);
	assert_store_revision(1U);
	complete(&client, &route_mutations);
}

static void test_content_failure_and_clients_do_not_share_terminal_state(void)
{
	struct linkr_debugger_task_status status;
	struct http_client_ctx client_a;
	struct http_client_ctx client_b;
	struct http_response_ctx response_a = { 0 };
	struct http_response_ctx response_b = { 0 };
	struct transaction_output failed = { 0 };
	struct http_header wrong_type = { .name = "Content-Type", .value = "text/plain" };
	uint8_t valid[512];
	size_t valid_len;

	reset_store();
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	valid_len = mutation_clear(valid, sizeof(valid), status.revision,
				   status.catalog_fingerprint, false);
	init_client(&client_a, HTTP_POST);
	call_route(&client_a, HTTP_SERVER_REQUEST_DATA_MORE, valid, valid_len,
		   &wrong_type, 1U, &route_mutations, &response_a);
	transaction_output_add(&failed, &response_a);
	call_route(&client_a, HTTP_SERVER_REQUEST_DATA_FINAL, valid, valid_len,
		   mutation_headers, 1U, &route_mutations, &response_a);
	assert_json_status(&response_a, HTTP_400_BAD_REQUEST, "invalid_mutation");
	transaction_output_add(&failed, &response_a);
	call_route(&client_a, HTTP_SERVER_REQUEST_DATA_FINAL, valid, valid_len,
		   mutation_headers, 1U, &route_mutations, &response_a);
	transaction_output_add(&failed, &response_a);
	assert_one_cmd_json(&failed, "\"ok\":false", "\"code\":\"invalid_mutation\"");

	/* A second client is busy, but never inherits client A's 400 state. */
	init_client(&client_b, HTTP_POST);
	call_route(&client_b, HTTP_SERVER_REQUEST_DATA_FINAL, valid, valid_len,
		   mutation_headers, 1U, &route_mutations, &response_b);
	assert_json_status(&response_b, HTTP_409_CONFLICT, "\"code\":\"busy\"");
	assert(capture_acquire_calls == 0U);
	assert_store_revision(0U);
	complete(&client_b, &route_mutations);
	complete(&client_a, &route_mutations);

	call_route(&client_b, HTTP_SERVER_REQUEST_DATA_FINAL, valid, valid_len,
		   mutation_headers, 1U, &route_mutations, &response_b);
	assert_json_status(&response_b, HTTP_200_OK, "\"action\":\"clear\"");
	assert(capture_acquire_calls == 1U);
	assert_store_revision(1U);
	complete(&client_b, &route_mutations);
}

static void test_raw_ir_response_lifetime_blocks_without_being_overwritten(void)
{
	static const char source[] = "{\"schema\":\"linkr-test.v1\"}\n";
	static const char ir[] =
		"# linkr-task.v1\n# task A\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP7\",\"body\":\"{}\"}\n";
	struct linkr_debugger_task_status status;
	struct http_client_ctx setup;
	struct http_client_ctx raw_client;
	struct http_client_ctx mutation_client;
	struct http_response_ctx response = { 0 };
	struct http_response_ctx busy_response = { 0 };
	uint8_t upsert[2048];
	uint8_t clear[512];
	uint8_t saved_ir[sizeof(ir) - 1U];
	size_t upsert_len;
	size_t clear_len;

	reset_store();
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	upsert_len = mutation_upsert(upsert, sizeof(upsert), "A", source, ir,
				     status.revision, status.catalog_fingerprint);
	init_client(&setup, HTTP_POST);
	call_route(&setup, HTTP_SERVER_REQUEST_DATA_FINAL, upsert, upsert_len,
		   mutation_headers, 1U, &route_mutations, &response);
	assert(response.status == HTTP_200_OK);
	complete(&setup, &route_mutations);
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	clear_len = mutation_clear(clear, sizeof(clear), status.revision,
				   status.catalog_fingerprint, false);
	capture_acquire_calls = 0U;

	init_client(&raw_client, HTTP_GET);
	call_route(&raw_client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U, NULL, 0U,
		   &route_ir, &response);
	assert(response.status == HTTP_200_OK && response.body_len == sizeof(saved_ir));
	assert_content_type(&response, LINKR_DEBUGGER_TASK_IR_CONTENT_TYPE);
	memcpy(saved_ir, response.body, sizeof(saved_ir));

	init_client(&mutation_client, HTTP_POST);
	call_route(&mutation_client, HTTP_SERVER_REQUEST_DATA_FINAL, clear, clear_len,
		   mutation_headers, 1U, &route_mutations, &busy_response);
	assert_json_status(&busy_response, HTTP_409_CONFLICT, "\"code\":\"busy\"");
	assert(memcmp(response.body, saved_ir, sizeof(saved_ir)) == 0);
	assert(capture_acquire_calls == 0U);
	assert_store_revision(1U);
	call_route(&mutation_client, HTTP_SERVER_REQUEST_DATA_FINAL, clear, clear_len,
		   mutation_headers, 1U, &route_mutations, &busy_response);
	assert(!busy_response.final_chunk && busy_response.body == NULL);
	assert(memcmp(response.body, saved_ir, sizeof(saved_ir)) == 0);
	complete(&mutation_client, &route_mutations);
	complete(&raw_client, &route_ir);

	call_route(&mutation_client, HTTP_SERVER_REQUEST_DATA_FINAL, clear, clear_len,
		   mutation_headers, 1U, &route_mutations, &busy_response);
	assert_json_status(&busy_response, HTTP_200_OK, "\"action\":\"clear\"");
	assert(capture_acquire_calls == 1U);
	assert_store_revision(2U);
	complete(&mutation_client, &route_mutations);
}

static void test_large_json_body_across_many_chunks(void)
{
	static char source[LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE + 1U];
	static uint8_t body[LINKR_DEBUGGER_TASK_MUTATION_MAX_SIZE];
	char ir[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE + 1U];
	struct linkr_debugger_task_status status;
	struct http_client_ctx client;
	struct http_response_ctx response = { 0 };
	size_t body_len;
	size_t offset;
	size_t ir_len;
	int written;

	reset_store();
	/* Worst-case valid canonical source: quotes, backslashes and
	 * multi-byte UTF-8 fill exactly 4000 bytes and still must arrive
	 * losslessly from the bounded base64 body. */
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE; i += 8U) {
		source[i + 0U] = '\\';
		source[i + 1U] = '"';
		source[i + 2U] = (char)0xe4;
		source[i + 3U] = (char)0xb8;
		source[i + 4U] = (char)0xad;
		source[i + 5U] = (char)0xe6;
		source[i + 6U] = (char)0x96;
		source[i + 7U] = (char)0x87;
	}
	source[LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE] = '\0';
	ir_len = 0U;
	written = snprintf(ir, sizeof(ir),
		"# linkr-task.v1\n# task A\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP7\",\"body\":\"{}\"}\n");
	assert(written > 0);
	ir_len = (size_t)written;
	while (ir_len + 5U <= LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE) {
		memcpy(ir + ir_len, "#pad\n", 5U);
		ir_len += 5U;
	}
	ir[ir_len] = '\0';
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	body_len = mutation_upsert(body, sizeof(body), "A", source, ir,
				   status.revision, status.catalog_fingerprint);
	/* Both maximum payloads fit the bounded body: two 5336-character
	 * base64 fields plus the fixed envelope keys. */
	assert(body_len > 2U * (((LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE + 2U) / 3U) * 4U));
	assert(body_len <= LINKR_DEBUGGER_TASK_MUTATION_MAX_SIZE);

	init_client(&client, HTTP_POST);
	for (offset = 0U; offset + 256U < body_len; offset += 256U) {
		call_route(&client, HTTP_SERVER_REQUEST_DATA_MORE, body + offset, 256U,
			   mutation_headers, 1U, &route_mutations, &response);
		assert(response.body == NULL);
	}
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, body + offset,
		   body_len - offset, NULL, 0U, &route_mutations, &response);
	assert_json_status(&response, HTTP_200_OK, "\"action\":\"upsert\"");
	complete(&client, &route_mutations);

	init_client(&client, HTTP_GET);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U, NULL, 0U,
		   &route_source_0, &response);
	assert(response.status == HTTP_200_OK &&
	       response.body_len == LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE);
	assert(memcmp(response.body, source, response.body_len) == 0);
	complete(&client, &route_source_0);

	init_client(&client, HTTP_GET);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U, NULL, 0U,
		   &route_ir, &response);
	assert(response.status == HTTP_200_OK && response.body_len == ir_len);
	assert(memcmp(response.body, ir, response.body_len) == 0);
	complete(&client, &route_ir);
}

static void test_catalog_source_absent_and_methods(void)
{
	struct http_client_ctx client;
	struct http_response_ctx response = { 0 };

	reset_store();
	init_client(&client, HTTP_GET);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U, NULL, 0U,
		   &route_catalog, &response);
	assert_json_status(&response, HTTP_200_OK, "\"action\":\"catalog\"");
	complete(&client, &route_catalog);

	init_client(&client, HTTP_GET);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U, NULL, 0U,
		   &route_source_0, &response);
	assert_json_status(&response, HTTP_404_NOT_FOUND, "task_source_absent");
	complete(&client, &route_source_0);

	init_client(&client, HTTP_DELETE);
	call_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U, NULL, 0U,
		   &route_tasks, &response);
	assert_json_status(&response, HTTP_405_METHOD_NOT_ALLOWED, "\"error\":{");
	complete(&client, &route_tasks);
}

int main(void)
{
	test_chunked_mutation_then_metadata_ir_and_source();
	test_stale_catalog_busy_storage_and_content_type_errors();
	test_oversize_discards_until_complete();
	test_malformed_discards_until_abort();
	test_content_failure_and_clients_do_not_share_terminal_state();
	test_raw_ir_response_lifetime_blocks_without_being_overwritten();
	test_large_json_body_across_many_chunks();
	test_catalog_source_absent_and_methods();
	puts("linkr_debugger_task_http: all tests passed");
	return 0;
}
