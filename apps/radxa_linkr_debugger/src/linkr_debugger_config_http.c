#include "linkr_debugger_config_http.h"
#include "linkr_debugger_config_http_encode.h"
#include "linkr_debugger_config_http_parse.h"
#include "linkr_debugger_config_http_result.h"
#include "linkr_debugger_http_body.h"

#include <stdint.h>

#define RESPONSE_STORAGE_SIZE (LINKR_DEBUGGER_CONFIG_HTTP_RESPONSE_CAP + 1U)

struct config_http_context {
	enum linkr_debugger_config_http_action action; char *buffer;
	struct http_response_ctx *response; bool body_method; };

static char config_response[RESPONSE_STORAGE_SIZE], apply_response[RESPONSE_STORAGE_SIZE];
static const struct http_header no_store_header[] = { { "Cache-Control", "no-store" } };
static const struct linkr_debugger_config_http_error internal_error = { HTTP_500_INTERNAL_SERVER_ERROR, "internal_error", "internal config error", NULL };
static const struct linkr_debugger_config_http_error invalid_json = { HTTP_400_BAD_REQUEST, "invalid_json", "request body does not match the config schema", NULL };
static const struct linkr_debugger_config_http_error body_too_large = { HTTP_413_PAYLOAD_TOO_LARGE, "body_too_large", "request body exceeds 1024 bytes", NULL };
static const struct linkr_debugger_config_http_error storage_read_error = { HTTP_500_INTERNAL_SERVER_ERROR, "storage_error", "failed to read config storage", NULL };

static bool select_operation(enum linkr_debugger_config_http_route route, enum http_method method, struct http_response_ctx *response, struct config_http_context *context)
{
	context->response = response;
	context->buffer = route == LINKR_DEBUGGER_CONFIG_HTTP_ROUTE_APPLY ? apply_response : config_response;
	context->body_method = false;
	context->action = route == LINKR_DEBUGGER_CONFIG_HTTP_ROUTE_APPLY ?
		LINKR_DEBUGGER_CONFIG_HTTP_ACTION_APPLY : LINKR_DEBUGGER_CONFIG_HTTP_ACTION_GET;
	if (route == LINKR_DEBUGGER_CONFIG_HTTP_ROUTE_CONFIG && method == HTTP_GET) return true;
	if (route == LINKR_DEBUGGER_CONFIG_HTTP_ROUTE_CONFIG && method == HTTP_PUT) {
		context->action = LINKR_DEBUGGER_CONFIG_HTTP_ACTION_SAVE; context->body_method = true;
		return true;
	}
	if (route == LINKR_DEBUGGER_CONFIG_HTTP_ROUTE_CONFIG && method == HTTP_DELETE) {
		context->action = LINKR_DEBUGGER_CONFIG_HTTP_ACTION_CLEAR; return true;
	}
	if (route == LINKR_DEBUGGER_CONFIG_HTTP_ROUTE_APPLY && method == HTTP_POST) {
		context->body_method = true; return true;
	}
	return false;
}

static void respond(const struct config_http_context *context, enum http_status status, size_t size)
{
	context->response->status = status; context->response->headers = no_store_header;
	context->response->header_count = sizeof(no_store_header) / sizeof(no_store_header[0]);
	context->response->body = (const uint8_t *)context->buffer; context->response->body_len = size;
	context->response->final_chunk = true;
}

static void send_error(const struct config_http_context *context,
		       const struct linkr_debugger_config_http_error *error,
		       const struct linkr_debugger_config_operation_report *report)
{
	size_t size = 0U;
	enum http_status status = error->status;
	enum linkr_debugger_config_http_encode_result encoded;

	encoded = linkr_debugger_config_http_encode_error(
		context->action, error, report, context->buffer, RESPONSE_STORAGE_SIZE, &size);
	if (encoded != LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_OK) {
		status = HTTP_500_INTERNAL_SERVER_ERROR;
		(void)linkr_debugger_config_http_encode_error(
			context->action, &internal_error, NULL,
			context->buffer, RESPONSE_STORAGE_SIZE, &size);
	}
	respond(context, status, size);
}

static void send_result_error(const struct config_http_context *context,
			      enum linkr_debugger_config_service_result result,
			      const struct linkr_debugger_config_operation_report *report)
{
	struct linkr_debugger_config_http_error error;

	if (!linkr_debugger_config_http_map_service_result(context->action, result, &error))
		return send_error(context, &internal_error, NULL);
	send_error(context, &error, report);
}

static void send_encoded(const struct config_http_context *context,
			 enum linkr_debugger_config_http_encode_result result, size_t size)
{
	if (result == LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_OK) respond(context, HTTP_200_OK, size); else send_error(context, &internal_error, NULL);
}

static bool get_status(const struct config_http_context *context,
		       struct linkr_debugger_config_service_status *status)
{
	enum linkr_debugger_config_service_result result = linkr_debugger_config_service_status_get(status);

	if (result == LINKR_DEBUGGER_CONFIG_SERVICE_OK) return true;
	send_result_error(context, result, NULL);
	return false;
}

static enum linkr_debugger_config_service_result result_from_reason(
	enum linkr_debugger_config_service_reason reason)
{
	static const enum linkr_debugger_config_service_result results[] = {
		LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT, LINKR_DEBUGGER_CONFIG_SERVICE_OK,
		LINKR_DEBUGGER_CONFIG_SERVICE_NO_SNAPSHOT, LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE,
		LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR, LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT,
		LINKR_DEBUGGER_CONFIG_SERVICE_UNSUPPORTED_VERSION };

	return (unsigned int)reason < sizeof(results) / sizeof(results[0]) ? results[reason] : LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
}

static void handle_get(const struct config_http_context *context)
{
	struct linkr_debugger_config_service_status status;
	size_t size = 0U; enum linkr_debugger_config_http_encode_result encoded;

	if (!get_status(context, &status)) return;
	encoded = linkr_debugger_config_http_encode_get(&status, context->buffer, RESPONSE_STORAGE_SIZE, &size);
	send_encoded(context, encoded, size);
}

static void handle_save(const struct config_http_context *context,
			const struct linkr_debugger_http_body_view *body)
{
	struct linkr_debugger_config_http_save_payload payload;
	struct linkr_debugger_config_operation_report report = { 0 };
	enum linkr_debugger_config_service_result result;
	size_t size = 0U; enum linkr_debugger_config_http_encode_result encoded;

	if (linkr_debugger_config_http_parse_save(body->data, body->len, &payload) !=
	    LINKR_DEBUGGER_CONFIG_HTTP_PARSE_OK) return send_error(context, &invalid_json, NULL);
	result = linkr_debugger_config_service_save(&payload.request, &report);
	if (result != LINKR_DEBUGGER_CONFIG_SERVICE_OK)
		return send_result_error(context, result, &report);
	encoded = linkr_debugger_config_http_encode_save(&payload.request, &report, context->buffer,
		RESPONSE_STORAGE_SIZE, &size);
	send_encoded(context, encoded, size);
}

static void handle_apply(const struct config_http_context *context,
			 const struct linkr_debugger_http_body_view *body)
{
	struct linkr_debugger_config_service_status status;
	struct linkr_debugger_config_operation_report report = { 0 };
	enum linkr_debugger_config_service_result result;
	bool confirmed = false;
	size_t size = 0U; enum linkr_debugger_config_http_encode_result encoded;

	if (linkr_debugger_config_http_parse_apply(body->data, body->len, &confirmed) !=
	    LINKR_DEBUGGER_CONFIG_HTTP_PARSE_OK) return send_error(context, &invalid_json, NULL);
	if (!get_status(context, &status)) return;
	if (status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT) {
		if (!status.available || status.snapshot_present) send_error(context, &internal_error, NULL);
		else send_result_error(context, LINKR_DEBUGGER_CONFIG_SERVICE_NO_SNAPSHOT, NULL);
		return;
	}
	if (status.reason != LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY)
		return send_result_error(context, result_from_reason(status.reason), NULL);
	if (!status.available || !status.snapshot_present) return send_error(context, &internal_error, NULL);
	if (status.pending_count == 0U && status.failed_count == 0U) {
		encoded = linkr_debugger_config_http_encode_apply(
			&report, true, context->buffer, RESPONSE_STORAGE_SIZE, &size);
		send_encoded(context, encoded, size);
		return;
	}
	result = linkr_debugger_config_service_apply(confirmed, &report);
	if (result != LINKR_DEBUGGER_CONFIG_SERVICE_OK)
		return send_result_error(context, result, &report);
	encoded = linkr_debugger_config_http_encode_apply(
		&report, false, context->buffer, RESPONSE_STORAGE_SIZE, &size);
	send_encoded(context, encoded, size);
}

static void handle_clear(const struct config_http_context *context)
{
	struct linkr_debugger_config_service_status status;
	enum linkr_debugger_config_service_result result;
	size_t size = 0U; enum linkr_debugger_config_http_encode_result encoded;

	if (!get_status(context, &status)) return;
	if (status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_STORAGE_ERROR) return send_error(context, &storage_read_error, NULL);
	if (status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT) {
		if (!status.available || status.snapshot_present)
			return send_error(context, &internal_error, NULL);
		encoded = linkr_debugger_config_http_encode_clear(
			true, context->buffer, RESPONSE_STORAGE_SIZE, &size);
		send_encoded(context, encoded, size);
		return;
	}
	if (status.reason != LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY &&
	    status.reason != LINKR_DEBUGGER_CONFIG_SERVICE_REASON_INVALID_SNAPSHOT &&
	    status.reason != LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNSUPPORTED_VERSION)
		return send_result_error(context, result_from_reason(status.reason), NULL);
	result = linkr_debugger_config_service_clear();
	if (result != LINKR_DEBUGGER_CONFIG_SERVICE_OK)
		return send_result_error(context, result, NULL);
	encoded = linkr_debugger_config_http_encode_clear(false, context->buffer, RESPONSE_STORAGE_SIZE, &size);
	send_encoded(context, encoded, size);
}

static void handle_final(const struct config_http_context *context,
			 const struct linkr_debugger_http_body_view *body)
{
	switch (context->action) {
	case LINKR_DEBUGGER_CONFIG_HTTP_ACTION_GET: handle_get(context); break;
	case LINKR_DEBUGGER_CONFIG_HTTP_ACTION_SAVE: handle_save(context, body); break;
	case LINKR_DEBUGGER_CONFIG_HTTP_ACTION_APPLY: handle_apply(context, body); break;
	case LINKR_DEBUGGER_CONFIG_HTTP_ACTION_CLEAR: handle_clear(context); break;
	}
}

static void clear_body(struct http_client_ctx *client, enum linkr_debugger_config_http_route route)
{ linkr_debugger_http_body_clear((uintptr_t)client, (uint16_t)client->method, (uint16_t)route); }

int linkr_debugger_config_http_handle(
	struct http_client_ctx *client, enum http_transaction_status status,
	const struct http_request_ctx *request_ctx, struct http_response_ctx *response_ctx,
	void *user_data)
{
	struct config_http_context context = {
		.action = LINKR_DEBUGGER_CONFIG_HTTP_ACTION_GET, .buffer = config_response,
		.response = response_ctx };
	struct linkr_debugger_http_body_view body = { 0 };
	enum linkr_debugger_config_http_route route; enum linkr_debugger_http_body_event event;
	enum linkr_debugger_http_body_result body_result;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		if (client != NULL && user_data != NULL) {
			route = *(const enum linkr_debugger_config_http_route *)user_data;
			if (select_operation(route, client->method, response_ctx, &context) &&
			    context.body_method) {
				event = status == HTTP_SERVER_TRANSACTION_ABORTED ?
					LINKR_DEBUGGER_HTTP_BODY_ABORTED : LINKR_DEBUGGER_HTTP_BODY_COMPLETE;
				(void)linkr_debugger_http_body_accumulate(
					(uintptr_t)client, (uint16_t)client->method, (uint16_t)route,
					event, NULL, 0U, NULL);
			}
		}
		return 0;
	}
	if (response_ctx == NULL) return 0;
	if (client == NULL || user_data == NULL) {
		send_error(&context, &internal_error, NULL); return 0;
	}
	route = *(const enum linkr_debugger_config_http_route *)user_data;
	if (!select_operation(route, client->method, response_ctx, &context)) {
		send_error(&context, &internal_error, NULL); return 0;
	}
	if (request_ctx == NULL) {
		if (context.body_method) clear_body(client, route);
		send_error(&context, &internal_error, NULL); return 0;
	}
	if (status == HTTP_SERVER_REQUEST_DATA_MORE && !context.body_method) return 0;
	if (status != HTTP_SERVER_REQUEST_DATA_MORE && status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		send_error(&context, &internal_error, NULL); return 0;
	}
	if (!context.body_method) {
		handle_final(&context, &body);
		return 0;
	}
	event = status == HTTP_SERVER_REQUEST_DATA_MORE ?
		LINKR_DEBUGGER_HTTP_BODY_MORE : LINKR_DEBUGGER_HTTP_BODY_FINAL;
	body_result = linkr_debugger_http_body_accumulate(
		(uintptr_t)client, (uint16_t)client->method, (uint16_t)route, event,
		request_ctx->data, request_ctx->data_len, &body);
	if (body_result == LINKR_DEBUGGER_HTTP_BODY_WAITING) return 0;
	if (body_result == LINKR_DEBUGGER_HTTP_BODY_READY) {
		handle_final(&context, &body);
		clear_body(client, route);
		return 0;
	}
	clear_body(client, route);
	if (body_result == LINKR_DEBUGGER_HTTP_BODY_TOO_LARGE) {
		send_error(&context, &body_too_large, NULL);
	} else {
		send_error(&context, &internal_error, NULL);
	}
	return 0;
}
