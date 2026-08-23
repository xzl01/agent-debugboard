/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

/* allow: SIZE_OK — cohesive /api/v1/tasks endpoint handler; the PUT upload
 * state machine, method dispatch, and response wiring share one HTTP
 * transaction lifecycle and stay readable as a single unit.
 */
#include "linkr_debugger_task_http.h"

#include "linkr_debugger_http_task_response.h"
#include "linkr_debugger_json_cursor.h"
#include "linkr_debugger_task.h"
#include "linkr_debugger_task_catalog.h"
#include "linkr_debugger_task_mutation.h"

#include <stdio.h>
#include <string.h>

const char *linkr_debugger_json_schema(void);

struct linkr_debugger_task_http_upload {
	struct http_client_ctx *client;
	size_t len;
	char buf[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
};

static struct linkr_debugger_task_http_upload task_upload;
static uint8_t task_http_response_buf[LINKR_DEBUGGER_TASK_HTTP_RESPONSE_CAP];
static const struct http_header task_http_json_headers[] = {
	{ .name = "Cache-Control", .value = "no-store" },
};

/* FIXME(review-20260821): One shared response workspace depends on serialized
 * HTTP callback response lifetimes. Consequence: concurrent dispatch could overwrite a body.
 * Remove when the HTTP layer provides transaction-owned response storage.
 */

static void task_upload_reset(void)
{
	task_upload.client = NULL;
	task_upload.len = 0U;
}

static size_t task_http_strnlen(const char *text, size_t max_len)
{
	size_t len = 0U;

	while (len < max_len && text[len] != '\0') {
		len++;
	}
	return len;
}

static void task_http_set_json_response(struct http_response_ctx *response_ctx,
					uint8_t *buf, size_t len,
					enum http_status status)
{
	response_ctx->status = status;
	response_ctx->headers = task_http_json_headers;
	response_ctx->header_count = sizeof(task_http_json_headers) / sizeof(task_http_json_headers[0]);
	response_ctx->body = buf;
	response_ctx->body_len = task_http_strnlen((char *)buf, len);
	response_ctx->final_chunk = true;
}

static void task_http_set_catalog_response(struct http_response_ctx *response_ctx)
{
	size_t body_len;

	response_ctx->status = HTTP_200_OK;
	response_ctx->headers = task_http_json_headers;
	response_ctx->header_count = sizeof(task_http_json_headers) / sizeof(task_http_json_headers[0]);
	response_ctx->body = linkr_debugger_task_catalog_json(&body_len);
	response_ctx->body_len = body_len;
	response_ctx->final_chunk = true;
}

static void task_http_error(struct http_response_ctx *response_ctx, uint8_t *buf,
			    size_t len, enum http_status status, const char *code,
			    const char *message)
{
	int written = snprintf((char *)buf, len,
		"{\"schema\":\"%s\",\"ok\":false,\"command\":\"task\","
		"\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}\n",
		linkr_debugger_json_schema(), code, message);

	if (written < 0 || (size_t)written >= len) {
		buf[0] = '\0';
	}
	task_http_set_json_response(response_ctx, buf, len, status);
}

struct task_http_get_context {
	uint8_t *json_buf;
	size_t json_buf_len;
	const struct linkr_debugger_task_status *status;
	int written;
};

static void task_http_encode_list(const char *blob, size_t blob_len, void *context)
{
	struct task_http_get_context *get_context = context;

	get_context->written = linkr_debugger_http_task_list_response(
		(char *)get_context->json_buf, get_context->json_buf_len,
		get_context->status, blob, blob_len);
}

static int task_http_handle_get(uint8_t *json_buf, size_t json_buf_len,
				struct http_response_ctx *response_ctx)
{
	struct linkr_debugger_task_status status;
	struct task_http_get_context context = {
		.json_buf = json_buf,
		.json_buf_len = json_buf_len,
		.status = &status,
	};
	enum linkr_debugger_task_result result;

	result = linkr_debugger_task_status_get(&status);

	if (result != LINKR_DEBUGGER_TASK_OK) {
		task_http_error(response_ctx, json_buf, json_buf_len,
				HTTP_500_INTERNAL_SERVER_ERROR,
				"status_unavailable", "task status unavailable");
		return -1;
	}
	result = linkr_debugger_task_blob_visit(task_http_encode_list, &context);
	if (result != LINKR_DEBUGGER_TASK_OK) {
		task_http_error(response_ctx, json_buf, json_buf_len,
				HTTP_500_INTERNAL_SERVER_ERROR,
				"snapshot_unavailable", "task blob snapshot unavailable");
		return -1;
	}
	if (context.written < 0) {
		task_http_error(response_ctx, json_buf, json_buf_len,
				HTTP_500_INTERNAL_SERVER_ERROR,
				"response_too_large", "failed to encode task response");
		return -1;
	}
	return 0;
}

static void task_http_error_result(uint8_t *json_buf, size_t json_buf_len,
				   struct http_response_ctx *response_ctx,
				   enum linkr_debugger_task_result result)
{
	switch (result) {
	case LINKR_DEBUGGER_TASK_BACKEND_UNAVAILABLE:
		task_http_error(response_ctx, json_buf, json_buf_len,
				HTTP_500_INTERNAL_SERVER_ERROR,
				"backend_unavailable", "task storage is unavailable");
		break;
	case LINKR_DEBUGGER_TASK_INVALID_BLOB:
		task_http_error(response_ctx, json_buf, json_buf_len,
				HTTP_400_BAD_REQUEST, "invalid_blob",
				"task blob is invalid or contains unsupported requests");
		break;
	case LINKR_DEBUGGER_TASK_BUSY:
		task_http_error(response_ctx, json_buf, json_buf_len,
				HTTP_409_CONFLICT, "busy", "task store is busy");
		break;
	case LINKR_DEBUGGER_TASK_STORAGE_ERROR:
		task_http_error(response_ctx, json_buf, json_buf_len,
				HTTP_500_INTERNAL_SERVER_ERROR,
				"storage_error", "failed to write task storage");
		break;
	default:
		task_http_error(response_ctx, json_buf, json_buf_len,
				HTTP_500_INTERNAL_SERVER_ERROR,
				"internal_error", "internal task error");
		break;
	}
}

static int task_http_handle_tasks_put(struct http_client_ctx *client,
				      enum http_transaction_status status,
				      const struct http_request_ctx *request_ctx,
				      struct http_response_ctx *response_ctx)
{
	enum linkr_debugger_task_result result;
	int written;
	bool final = status == HTTP_SERVER_REQUEST_DATA_FINAL;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		if (task_upload.client == client) {
			task_upload_reset();
		}
		return 0;
	}

	if (task_upload.client == NULL) {
		task_upload.client = client;
		task_upload.len = 0U;
	} else if (task_upload.client != client) {
		task_http_error(response_ctx, task_http_response_buf,
					sizeof(task_http_response_buf),
			HTTP_409_CONFLICT, "busy",
			"another task upload is in progress");
		return 0;
	}

	if (request_ctx->data != NULL && request_ctx->data_len > 0U) {
		if (task_upload.len + request_ctx->data_len >
		    LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE) {
			task_upload_reset();
			task_http_error(response_ctx, task_http_response_buf,
						sizeof(task_http_response_buf),
					HTTP_413_PAYLOAD_TOO_LARGE,
					"body_too_large",
					"task blob exceeds the 4096 byte limit");
			return 0;
		}
		memcpy(task_upload.buf + task_upload.len, request_ctx->data,
		       request_ctx->data_len);
		task_upload.len += request_ctx->data_len;
	}

	if (!final) {
		return 0;
	}

	if (task_upload.len == 0U) {
		task_upload_reset();
		task_http_error(response_ctx, task_http_response_buf,
				sizeof(task_http_response_buf),
				HTTP_400_BAD_REQUEST, "missing_body",
				"missing task blob body");
		return 0;
	}

	if (task_upload.buf[0] == '"') {
		size_t decoded_len;

		if (!linkr_debugger_json_decode_complete_string(
				task_upload.buf, task_upload.len, task_upload.buf,
				sizeof(task_upload.buf), &decoded_len)) {
			task_upload_reset();
			task_http_error(response_ctx, task_http_response_buf,
					sizeof(task_http_response_buf),
					HTTP_400_BAD_REQUEST, "invalid_body",
					"task blob body is not valid JSON text");
			return 0;
		}
		task_upload.len = decoded_len;
	}

	if (linkr_debugger_task_mutation_acquire() != LINKR_DEBUGGER_TASK_OK) {
		task_upload_reset();
		task_http_error(response_ctx, task_http_response_buf,
				sizeof(task_http_response_buf),
				HTTP_409_CONFLICT, "busy",
				"capture or OTA activity is in progress");
		return 0;
	}
	result = linkr_debugger_task_tasks_store(task_upload.buf, task_upload.len);
	task_upload_reset();
	linkr_debugger_task_mutation_release();
	if (result != LINKR_DEBUGGER_TASK_OK) {
		task_http_error_result(task_http_response_buf,
				       sizeof(task_http_response_buf), response_ctx, result);
		return 0;
	}

	written = linkr_debugger_http_task_store_response((char *)task_http_response_buf,
							      sizeof(task_http_response_buf));

	if (written < 0) {
		task_http_error(response_ctx, task_http_response_buf,
				sizeof(task_http_response_buf),
				HTTP_500_INTERNAL_SERVER_ERROR,
				"response_too_large", "failed to encode task response");
		return 0;
	}
	task_http_set_json_response(response_ctx, task_http_response_buf,
				    (size_t)written + 1U, HTTP_200_OK);
	return 0;
}

static int task_http_handle_tasks_delete(struct http_response_ctx *response_ctx)
{
	enum linkr_debugger_task_result result;
	int written;

	if (linkr_debugger_task_mutation_acquire() != LINKR_DEBUGGER_TASK_OK) {
		task_http_error(response_ctx, task_http_response_buf,
				sizeof(task_http_response_buf),
				HTTP_409_CONFLICT, "busy",
				"capture or OTA activity is in progress");
		return -1;
	}
	result = linkr_debugger_task_tasks_clear();
	linkr_debugger_task_mutation_release();
	if (result != LINKR_DEBUGGER_TASK_OK) {
		task_http_error_result(task_http_response_buf,
				       sizeof(task_http_response_buf), response_ctx, result);
		return -1;
	}
	written = linkr_debugger_http_task_clear_response((char *)task_http_response_buf,
							      sizeof(task_http_response_buf));
	if (written < 0) {
		task_http_error(response_ctx, task_http_response_buf,
				sizeof(task_http_response_buf),
				HTTP_500_INTERNAL_SERVER_ERROR,
				"response_too_large", "failed to encode task response");
		return -1;
	}
	task_http_set_json_response(response_ctx, task_http_response_buf,
				    (size_t)written + 1U, HTTP_200_OK);
	return 0;
}

int linkr_debugger_task_http_handle(struct http_client_ctx *client,
				    enum http_transaction_status status,
				    const struct http_request_ctx *request_ctx,
				    struct http_response_ctx *response_ctx,
				    void *user_data)
{
	enum linkr_debugger_task_http_route route = user_data != NULL ?
		*(const enum linkr_debugger_task_http_route *)user_data :
		LINKR_DEBUGGER_TASK_HTTP_ROUTE_TASKS;

	if (route == LINKR_DEBUGGER_TASK_HTTP_ROUTE_CATALOG) {
		if (client->method != HTTP_GET) {
			task_http_error(response_ctx, task_http_response_buf,
					sizeof(task_http_response_buf),
					HTTP_405_METHOD_NOT_ALLOWED,
					"method_not_allowed", "method not allowed");
			return 0;
		}
		if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
			task_http_set_catalog_response(response_ctx);
		}
		return 0;
	}
	if (route != LINKR_DEBUGGER_TASK_HTTP_ROUTE_TASKS) {
		task_http_error(response_ctx, task_http_response_buf,
				sizeof(task_http_response_buf),
				HTTP_404_NOT_FOUND, "not_found", "unknown task path");
		return 0;
	}
	switch (linkr_debugger_http_task_action_for_method((unsigned int)client->method)) {
	case LINKR_DEBUGGER_TASK_HTTP_ACTION_STORE:
		return task_http_handle_tasks_put(client, status, request_ctx, response_ctx);
	case LINKR_DEBUGGER_TASK_HTTP_ACTION_LIST:
		break;
	case LINKR_DEBUGGER_TASK_HTTP_ACTION_CLEAR:
		break;
	case LINKR_DEBUGGER_TASK_HTTP_ACTION_METHOD_NOT_ALLOWED:
		task_http_error(response_ctx, task_http_response_buf,
				sizeof(task_http_response_buf),
				HTTP_405_METHOD_NOT_ALLOWED,
				"method_not_allowed", "method not allowed");
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	if (client->method == HTTP_GET) {
		if (task_http_handle_get(task_http_response_buf, sizeof(task_http_response_buf),
					 response_ctx) < 0) {
			return 0;
		}
		task_http_set_json_response(response_ctx, task_http_response_buf,
					    sizeof(task_http_response_buf), HTTP_200_OK);
		return 0;
	}
	if (client->method == HTTP_DELETE) {
		(void)task_http_handle_tasks_delete(response_ctx);
		return 0;
	}
	return 0;
}
