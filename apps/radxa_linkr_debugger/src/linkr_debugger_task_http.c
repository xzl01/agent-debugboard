/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 *
 * Task-v2 HTTP transaction ownership. The mutation arena and raw response
 * arena are distinct and remain owned until the corresponding transaction
 * completes or aborts.
 */

#include "linkr_debugger_task_http.h"

#include "linkr_debugger_http_task_response.h"
#include "linkr_debugger_task.h"
#include "linkr_debugger_task_catalog.h"
#include "linkr_debugger_task_mutation.h"

#ifndef LINKR_DEBUGGER_TASK_HTTP_HOST_TEST
#include <zephyr/net/http/parser.h>
HTTP_SERVER_REGISTER_HEADER_CAPTURE(linkr_debugger_task_content_type_capture,
				    "Content-Type");
#endif

#include <stdio.h>
#include <string.h>
#include <strings.h>

const char *linkr_debugger_json_schema(void);

struct task_http_input_state {
	struct http_client_ctx *client;
	size_t len;
	bool headers_seen;
	bool content_type_valid;
	bool headers_dropped;
	bool terminal;
	bool terminal_emitted;
	enum http_status terminal_status;
	const uint8_t *terminal_body;
	size_t terminal_body_len;
};

/* Task-v2 arenas sized by the JSON mutation contract: two canonical
 * base64 4000-byte payloads plus envelope keys, and 4001-byte raw
 * outputs. */
static uint8_t task_http_input[LINKR_DEBUGGER_TASK_MUTATION_MAX_SIZE];
static struct task_http_input_state task_input_state;
static uint8_t task_http_raw_response[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE];
static struct http_client_ctx *task_response_owner;
/* Single-owner mutation outcome: avoids a large frame on the 3 KiB HTTP
 * server stack. task_input_state/task_response_owner serialize users.
 */
static struct linkr_debugger_task_mutation_outcome task_http_outcome;

static const struct http_header task_http_json_headers[] = {
	{ .name = "Cache-Control", .value = "no-store" },
	{ .name = "Content-Type", .value = "application/json" },
};

static const struct http_header task_http_ir_headers[] = {
	{ .name = "Cache-Control", .value = "no-store" },
	{ .name = "Content-Type", .value = LINKR_DEBUGGER_TASK_IR_CONTENT_TYPE },
};

static const struct http_header task_http_source_headers[] = {
	{ .name = "Cache-Control", .value = "no-store" },
	{ .name = "Content-Type", .value = LINKR_DEBUGGER_TASK_SOURCE_CONTENT_TYPE },
};

static const uint8_t task_http_busy_response[] =
	"{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":false,"
	"\"command\":\"tasks\",\"error\":{\"code\":\"busy\","
	"\"message\":\"another task transaction owns the response arena\"}}\n";

static void task_http_set_response(struct http_response_ctx *response,
				   const struct http_header *headers,
				   size_t header_count, const uint8_t *body,
				   size_t body_len, enum http_status status)
{
	response->status = status;
	response->headers = headers;
	response->header_count = header_count;
	response->body = body;
	response->body_len = body_len;
	response->final_chunk = true;
}

static void task_http_set_json_response(struct http_response_ctx *response,
					const uint8_t *body, size_t body_len,
					enum http_status status)
{
	task_http_set_response(response, task_http_json_headers,
			       sizeof(task_http_json_headers) /
				       sizeof(task_http_json_headers[0]),
			       body, body_len, status);
}

static void task_http_set_no_response(struct http_response_ctx *response)
{
	response->status = (enum http_status)0;
	response->headers = NULL;
	response->header_count = 0U;
	response->body = NULL;
	response->body_len = 0U;
	response->final_chunk = false;
}

static void task_http_set_static_busy(struct http_response_ctx *response)
{
	task_http_set_json_response(response, task_http_busy_response,
				    sizeof(task_http_busy_response) - 1U,
				    HTTP_409_CONFLICT);
}

static void task_http_latch_terminal(struct http_response_ctx *response,
				     bool emit_now)
{
	task_input_state.terminal = true;
	task_input_state.terminal_emitted = emit_now;
	task_input_state.terminal_status = response->status;
	task_input_state.terminal_body = response->body;
	task_input_state.terminal_body_len = response->body_len;
	if (!emit_now) {
		task_http_set_no_response(response);
	}
}

static void task_http_replay_terminal(struct http_response_ctx *response)
{
	task_http_set_json_response(response, task_input_state.terminal_body,
				    task_input_state.terminal_body_len,
				    task_input_state.terminal_status);
	task_input_state.terminal_emitted = true;
}

static bool task_http_acquire_response(struct http_client_ctx *client,
				       struct http_response_ctx *response)
{
	if (task_response_owner == NULL) {
		task_response_owner = client;
		return true;
	}
	if (task_response_owner == client) {
		return true;
	}
	task_http_set_static_busy(response);
	return false;
}

static void task_http_release_client(struct http_client_ctx *client)
{
	if (task_response_owner == client) {
		task_response_owner = NULL;
	}
	if (task_input_state.client == client) {
		memset(&task_input_state, 0, sizeof(task_input_state));
	}
}

static const char *task_http_header_value(const struct http_request_ctx *request,
					  const char *name)
{
	if (request == NULL || request->headers == NULL) {
		return NULL;
	}
	for (size_t i = 0U; i < request->header_count; i++) {
		if (request->headers[i].name != NULL &&
		    strcasecmp(request->headers[i].name, name) == 0) {
			return request->headers[i].value;
		}
	}
	return NULL;
}

static bool task_http_content_type_valid(const struct http_request_ctx *request)
{
	const char *value = task_http_header_value(request, "Content-Type");
	size_t expected_len = strlen(LINKR_DEBUGGER_TASK_MUTATION_CONTENT_TYPE);

	return value != NULL &&
		strncasecmp(value, LINKR_DEBUGGER_TASK_MUTATION_CONTENT_TYPE,
			    expected_len) == 0 &&
		(value[expected_len] == '\0' || value[expected_len] == ';');
}

static void task_http_error(struct http_response_ctx *response,
			    enum http_status status, const char *code,
			    const char *message,
			    const struct linkr_debugger_task_status *current)
{
	int written = linkr_debugger_http_task_error_response(
		(char *)task_http_raw_response, sizeof(task_http_raw_response),
		code, message, current);

	if (written < 0) {
		written = 0;
	}
	task_http_set_json_response(response, task_http_raw_response, (size_t)written,
				    status);
}

static void task_http_result_error(struct http_response_ctx *response,
				   enum linkr_debugger_task_result result,
				   const struct linkr_debugger_task_status *current)
{
	switch (result) {
	case LINKR_DEBUGGER_TASK_INVALID_ARGUMENT:
	case LINKR_DEBUGGER_TASK_INVALID_MUTATION:
		task_http_error(response, HTTP_400_BAD_REQUEST, "invalid_mutation",
				"task mutation is malformed", NULL);
		break;
	case LINKR_DEBUGGER_TASK_INVALID_TASK:
		task_http_error(response, HTTP_400_BAD_REQUEST, "invalid_task",
				"task source or IR is invalid", NULL);
		break;
	case LINKR_DEBUGGER_TASK_BUSY:
		task_http_error(response, HTTP_409_CONFLICT, "busy",
				"task or flash storage is busy", NULL);
		break;
	case LINKR_DEBUGGER_TASK_REVISION_CONFLICT:
		task_http_error(response, HTTP_409_CONFLICT, "revision_conflict",
				"task revision changed", current);
		break;
	case LINKR_DEBUGGER_TASK_CATALOG_CONFLICT:
		task_http_error(response, HTTP_409_CONFLICT, "catalog_conflict",
				"task catalog changed", current);
		break;
	case LINKR_DEBUGGER_TASK_STORAGE_FULL:
		task_http_error(response, HTTP_507_INSUFFICIENT_STORAGE, "storage_full",
				"task storage is full", NULL);
		break;
	case LINKR_DEBUGGER_TASK_READBACK_FAILED:
		task_http_error(response, HTTP_500_INTERNAL_SERVER_ERROR, "readback_failed",
				"task storage readback failed", NULL);
		break;
	case LINKR_DEBUGGER_TASK_SOURCE_ABSENT:
		task_http_error(response, HTTP_404_NOT_FOUND, "task_source_absent",
				"task slot has no lossless source", NULL);
		break;
	case LINKR_DEBUGGER_TASK_BODY_TOO_LARGE:
		task_http_error(response, HTTP_400_BAD_REQUEST, "body_too_large",
				"task source or IR exceeds 4000 bytes", NULL);
		break;
	case LINKR_DEBUGGER_TASK_BACKEND_UNAVAILABLE:
	case LINKR_DEBUGGER_TASK_STORAGE_ERROR:
	default:
		task_http_error(response, HTTP_500_INTERNAL_SERVER_ERROR, "storage_error",
				"task storage is unavailable", NULL);
		break;
	}
}

static int task_http_handle_list(struct http_client_ctx *client,
				 struct http_response_ctx *response)
{
	struct linkr_debugger_task_status status;
	int written;

	if (!task_http_acquire_response(client, response)) {
		return 0;
	}
	if (linkr_debugger_task_status_get(&status) != LINKR_DEBUGGER_TASK_OK ||
	    !status.backend_available) {
		task_http_result_error(response, LINKR_DEBUGGER_TASK_BACKEND_UNAVAILABLE,
				       NULL);
		return 0;
	}
	written = linkr_debugger_http_task_list_response(
		(char *)task_http_raw_response, sizeof(task_http_raw_response), &status);
	if (written < 0) {
		task_http_result_error(response, LINKR_DEBUGGER_TASK_STORAGE_ERROR, NULL);
		return 0;
	}
	task_http_set_json_response(response, task_http_raw_response, (size_t)written,
				    HTTP_200_OK);
	return 0;
}

static int task_http_handle_ir(struct http_client_ctx *client,
			       struct http_response_ctx *response)
{
	size_t len = 0U;
	enum linkr_debugger_task_result result;

	if (!task_http_acquire_response(client, response)) {
		return 0;
	}
	result = linkr_debugger_task_ir_snapshot(task_http_raw_response,
						 sizeof(task_http_raw_response), &len);
	if (result != LINKR_DEBUGGER_TASK_OK) {
		task_http_result_error(response, result, NULL);
		return 0;
	}
	task_http_set_response(response, task_http_ir_headers,
			       sizeof(task_http_ir_headers) /
				       sizeof(task_http_ir_headers[0]),
			       task_http_raw_response, len, HTTP_200_OK);
	return 0;
}

static int task_http_handle_source(struct http_client_ctx *client,
				   struct http_response_ctx *response, uint8_t slot)
{
	size_t len = 0U;
	enum linkr_debugger_task_result result;

	if (task_input_state.client != NULL && task_input_state.client != client) {
		task_http_set_static_busy(response);
		return 0;
	}
	memset(&task_input_state, 0, sizeof(task_input_state));
	task_input_state.client = client;
	result = linkr_debugger_task_source_snapshot(slot, task_http_input,
						     sizeof(task_http_input), &len);
	if (result != LINKR_DEBUGGER_TASK_OK) {
		memset(&task_input_state, 0, sizeof(task_input_state));
		if (!task_http_acquire_response(client, response)) {
			return 0;
		}
		task_http_result_error(response, result, NULL);
		return 0;
	}
	task_input_state.len = len;
	task_http_set_response(response, task_http_source_headers,
			       sizeof(task_http_source_headers) /
				       sizeof(task_http_source_headers[0]),
			       task_http_input, len, HTTP_200_OK);
	return 0;
}

static int task_http_handle_mutation(struct http_client_ctx *client,
				     enum http_transaction_status transaction_status,
				     const struct http_request_ctx *request,
				     struct http_response_ctx *response)
{
	struct linkr_debugger_task_mutation_outcome *outcome = &task_http_outcome;
	enum linkr_debugger_task_mutation_opcode opcode = 0;
	enum linkr_debugger_task_result result;
	bool final = transaction_status == HTTP_SERVER_REQUEST_DATA_FINAL;
	int written;

	if (task_input_state.client == NULL) {
		memset(&task_input_state, 0, sizeof(task_input_state));
		task_input_state.client = client;
	} else if (task_input_state.client != client) {
		task_http_set_static_busy(response);
		return 0;
	}
	if (task_input_state.terminal) {
		if (final && !task_input_state.terminal_emitted) {
			task_http_replay_terminal(response);
		} else {
			task_http_set_no_response(response);
		}
		return 0;
	}
	task_http_set_no_response(response);
	if (request != NULL && (request->headers != NULL ||
				request->headers_status == HTTP_HEADER_STATUS_DROPPED)) {
		task_input_state.headers_seen = true;
		task_input_state.headers_dropped =
			request->headers_status == HTTP_HEADER_STATUS_DROPPED;
		task_input_state.content_type_valid = task_http_content_type_valid(request);
		if (task_input_state.headers_dropped ||
		    !task_input_state.content_type_valid) {
			if (!task_http_acquire_response(client, response)) {
				task_http_latch_terminal(response, final);
				return 0;
			}
			task_http_error(response, HTTP_400_BAD_REQUEST, "invalid_mutation",
				task_input_state.headers_dropped ?
					"task mutation headers were dropped" :
					"task mutation content type is invalid",
				NULL);
			task_http_latch_terminal(response, final);
			return 0;
		}
	}
	if (request != NULL && request->data != NULL && request->data_len > 0U) {
		if (request->data_len > sizeof(task_http_input) - task_input_state.len) {
			if (!task_http_acquire_response(client, response)) {
				task_http_latch_terminal(response, final);
				return 0;
			}
			task_http_error(response, HTTP_400_BAD_REQUEST,
					"body_too_large",
					"task mutation body is too large", NULL);
			task_http_latch_terminal(response, final);
			return 0;
		}
		memcpy(task_http_input + task_input_state.len,
		       request->data, request->data_len);
		task_input_state.len += request->data_len;
	}
	if (!final) {
		return 0;
	}
	if (!task_http_acquire_response(client, response)) {
		task_http_latch_terminal(response, true);
		return 0;
	}
	if (task_input_state.headers_dropped) {
		task_http_error(response, HTTP_400_BAD_REQUEST, "invalid_mutation",
				"task mutation headers were dropped", NULL);
		task_http_latch_terminal(response, true);
		return 0;
	}
	if (!task_input_state.headers_seen || !task_input_state.content_type_valid) {
		task_http_error(response, HTTP_400_BAD_REQUEST, "invalid_mutation",
				"task mutation content type is invalid", NULL);
		task_http_latch_terminal(response, true);
		return 0;
	}
	memset(outcome, 0, sizeof(*outcome));
	result = linkr_debugger_task_mutation_execute(task_http_input,
						      task_input_state.len,
						      outcome, &opcode);
	if (result != LINKR_DEBUGGER_TASK_OK) {
		task_http_result_error(response, result,
			(result == LINKR_DEBUGGER_TASK_REVISION_CONFLICT ||
			 result == LINKR_DEBUGGER_TASK_CATALOG_CONFLICT) ?
				&outcome->status : NULL);
		task_http_latch_terminal(response, true);
		return 0;
	}
	written = linkr_debugger_http_task_mutation_response(
		(char *)task_http_raw_response, sizeof(task_http_raw_response),
		outcome, opcode);
	if (written < 0) {
		task_http_result_error(response, LINKR_DEBUGGER_TASK_STORAGE_ERROR, NULL);
		task_http_latch_terminal(response, true);
		return 0;
	}
	task_http_set_json_response(response, task_http_raw_response, (size_t)written,
				    HTTP_200_OK);
	task_http_latch_terminal(response, true);
	return 0;
}

static int task_http_method_error(struct http_client_ctx *client,
				  struct http_response_ctx *response)
{
	if (task_http_acquire_response(client, response)) {
		task_http_error(response, HTTP_405_METHOD_NOT_ALLOWED,
				"invalid_mutation", "method not allowed", NULL);
	}
	return 0;
}

int linkr_debugger_task_http_handle(
	struct http_client_ctx *client,
	enum http_transaction_status transaction_status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response,
	void *user_data)
{
	enum linkr_debugger_task_http_route route = user_data != NULL ?
		*(const enum linkr_debugger_task_http_route *)user_data :
		LINKR_DEBUGGER_TASK_HTTP_ROUTE_TASKS;

	if (client == NULL || response == NULL) {
		return -1;
	}
	if (transaction_status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    transaction_status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		task_http_release_client(client);
		return 0;
	}
	if (route == LINKR_DEBUGGER_TASK_HTTP_ROUTE_MUTATIONS) {
		return client->method == HTTP_POST ?
			task_http_handle_mutation(client, transaction_status, request, response) :
			task_http_method_error(client, response);
	}
	if (transaction_status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	if (client->method != HTTP_GET) {
		return task_http_method_error(client, response);
	}
	switch (route) {
	case LINKR_DEBUGGER_TASK_HTTP_ROUTE_TASKS:
		return task_http_handle_list(client, response);
	case LINKR_DEBUGGER_TASK_HTTP_ROUTE_CATALOG: {
		size_t len;
		const uint8_t *catalog = linkr_debugger_task_catalog_json(&len);

		task_http_set_json_response(response, catalog, len, HTTP_200_OK);
		return 0;
	}
	case LINKR_DEBUGGER_TASK_HTTP_ROUTE_IR:
		return task_http_handle_ir(client, response);
	case LINKR_DEBUGGER_TASK_HTTP_ROUTE_SOURCE_0:
	case LINKR_DEBUGGER_TASK_HTTP_ROUTE_SOURCE_1:
	case LINKR_DEBUGGER_TASK_HTTP_ROUTE_SOURCE_2:
	case LINKR_DEBUGGER_TASK_HTTP_ROUTE_SOURCE_3:
		return task_http_handle_source(client, response,
			(uint8_t)(route - LINKR_DEBUGGER_TASK_HTTP_ROUTE_SOURCE_0));
	default:
		if (task_http_acquire_response(client, response)) {
			task_http_error(response, HTTP_400_BAD_REQUEST, "invalid_mutation",
					"unknown task route", NULL);
		}
		return 0;
	}
}
