#include "test_linkr_debugger_config_http_stubs.h"

#include "../../src/linkr_debugger_config_http.h"
#include "../../src/linkr_debugger_config_service.h"
#include "../../src/linkr_debugger_http_body.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE_LOCAL(array) (sizeof(array) / sizeof((array)[0]))
#define SAVED_ID_CAP 64U

_Static_assert(LINKR_DEBUGGER_CONFIG_HTTP_ROUTE_CONFIG == 3,
	       "config HTTP route ID changed");
_Static_assert(LINKR_DEBUGGER_CONFIG_HTTP_RESPONSE_CAP == 4160U,
	       "config HTTP response cap changed");
_Static_assert(LINKR_DEBUGGER_HTTP_BODY_SLOTS == 4U,
	       "config HTTP body slot count changed");
_Static_assert(HTTP_SERVER_TRANSACTION_ABORTED == -1,
	       "Zephyr aborted status changed");
_Static_assert(HTTP_SERVER_REQUEST_DATA_MORE == 0,
	       "Zephyr more status changed");
_Static_assert(HTTP_SERVER_REQUEST_DATA_FINAL == 1,
	       "Zephyr final status changed");
_Static_assert(HTTP_SERVER_TRANSACTION_COMPLETE == 2,
	       "Zephyr complete status changed");
_Static_assert(HTTP_DELETE == 0 && HTTP_GET == 1 && HTTP_POST == 3 && HTTP_PUT == 4,
	       "Zephyr HTTP method IDs changed");

static const enum linkr_debugger_config_http_route config_route =
	LINKR_DEBUGGER_CONFIG_HTTP_ROUTE_CONFIG;
static const struct http_header sentinel_headers[] = {
	{ "X-Sentinel", "unchanged" },
};
static const uint8_t sentinel_body[] = "sentinel";
static size_t regression_failure_count;

static void regression_assert_contains(const char *name, const char *text,
				       const char *expected)
{
	if (strstr(text, expected) == NULL) {
		fprintf(stderr, "REGRESSION ASSERTION FAILED [%s]: missing %s\n",
			name, expected);
		regression_failure_count++;
	}
}

struct fake_service {
	enum linkr_debugger_config_service_result status_result;
	enum linkr_debugger_config_service_result save_result;
	enum linkr_debugger_config_service_result clear_result;
	struct linkr_debugger_config_service_status status;
	struct linkr_debugger_config_operation_report save_report;
	size_t init_calls;
	size_t status_calls;
	size_t save_calls;
	size_t clear_calls;
	size_t saved_item_count;
	char saved_ids[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES][SAVED_ID_CAP];
	bool last_save_confirmed;
};

static struct fake_service fake;

static uint8_t default_value(const struct linkr_debugger_config_item_desc *item)
{
	if (item->domain == LINKR_DEBUGGER_CONFIG_DOMAIN_POWER ||
	    item->domain == LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO) {
		return 0U;
	}

	switch (item->item_id) {
	case LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID:
		return LINKR_DEBUGGER_CONFIG_SD_TARGET;
	case LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID:
		return LINKR_DEBUGGER_CONFIG_USB_TARGET;
	case LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID:
		return LINKR_DEBUGGER_CONFIG_TF_WP_WRITABLE;
	case LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID:
		return LINKR_DEBUGGER_CONFIG_VIN_3V3;
	default:
		assert(false);
		return 0U;
	}
}

static bool item_value_requires_confirmation(
	const struct linkr_debugger_config_item_desc *item, uint8_t value)
{
	const struct linkr_debugger_config_entry entry = {
		.domain = item->domain,
		.item_id = item->item_id,
		.value = value,
	};
	bool required = false;

	assert(linkr_debugger_config_classify_entry(&entry, &required) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_OK);
	return required;
}

static void fill_live_catalog(struct linkr_debugger_config_service_status *status)
{
	memset(status, 0, sizeof(*status));
	status->available = true;
	status->reason = LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY;
	status->snapshot_present = true;
	status->snapshot_version = LINKR_DEBUGGER_CONFIG_VERSION;
	status->item_count = linkr_debugger_config_item_count;
	for (size_t i = 0U; i < status->item_count; i++) {
		struct linkr_debugger_config_item_status *row = &status->items[i];
		uint8_t value = default_value(&linkr_debugger_config_items[i]);

		row->item = &linkr_debugger_config_items[i];
		row->current_available = true;
		row->current_value = value;
		row->current_requires_confirmation =
			item_value_requires_confirmation(row->item, value);
		row->apply_state = LINKR_DEBUGGER_CONFIG_APPLY_NOT_SAVED;
	}
}

static void set_status_reason(enum linkr_debugger_config_service_reason reason)
{
	fake.status.reason = reason;
	fake.status.available = reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY ||
				reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT;
	fake.status.snapshot_present = reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY;
	fake.status.snapshot_version = fake.status.snapshot_present ?
		LINKR_DEBUGGER_CONFIG_VERSION : 0U;
	if (reason != LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY) {
		fake.status.saved_count = 0U;
		fake.status.applied_count = 0U;
		fake.status.pending_count = 0U;
		fake.status.failed_count = 0U;
		fake.status.failed_item = NULL;
		for (size_t i = 0U; i < fake.status.item_count; i++) {
			fake.status.items[i].saved = false;
			fake.status.items[i].apply_state =
				LINKR_DEBUGGER_CONFIG_APPLY_NOT_SAVED;
		}
	}
}

static void set_existing_snapshot(size_t pending_count, size_t failed_count)
{
	struct linkr_debugger_config_item_status *row;

	assert(pending_count <= 1U);
	assert(failed_count <= 1U);
	set_status_reason(LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY);
	fake.status.saved_count = 1U + failed_count;
	fake.status.applied_count = pending_count == 0U && failed_count == 0U ? 1U : 0U;
	fake.status.pending_count = pending_count;
	fake.status.failed_count = failed_count;
	row = &fake.status.items[0];
	row->saved = true;
	row->saved_value = row->current_value;
	row->saved_requires_confirmation = row->current_requires_confirmation;
	row->apply_state = pending_count != 0U ?
		LINKR_DEBUGGER_CONFIG_APPLY_PENDING : LINKR_DEBUGGER_CONFIG_APPLY_APPLIED;
	if (failed_count != 0U) {
		row = &fake.status.items[1];
		row->saved = true;
		row->saved_value = row->current_value;
		row->saved_requires_confirmation = row->current_requires_confirmation;
		row->apply_state = LINKR_DEBUGGER_CONFIG_APPLY_FAILED;
		fake.status.failed_item = row->item;
	} else {
		fake.status.failed_item = NULL;
	}
}

static void reset_fixture(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.status_result = LINKR_DEBUGGER_CONFIG_SERVICE_OK;
	fake.save_result = LINKR_DEBUGGER_CONFIG_SERVICE_OK;
	fake.clear_result = LINKR_DEBUGGER_CONFIG_SERVICE_OK;
	fake.save_report.snapshot_version = LINKR_DEBUGGER_CONFIG_VERSION;
	fill_live_catalog(&fake.status);
	set_existing_snapshot(0U, 0U);
	linkr_debugger_http_body_reset_all();
}

enum linkr_debugger_config_service_result linkr_debugger_config_service_init(void)
{
	fake.init_calls++;
	return LINKR_DEBUGGER_CONFIG_SERVICE_OK;
}

enum linkr_debugger_config_service_result linkr_debugger_config_service_status_get(
	struct linkr_debugger_config_service_status *status)
{
	fake.status_calls++;
	assert(status != NULL);
	*status = fake.status;
	return fake.status_result;
}

enum linkr_debugger_config_service_result linkr_debugger_config_service_save(
	const struct linkr_debugger_config_save_request *request,
	struct linkr_debugger_config_operation_report *report)
{
	fake.save_calls++;
	assert(request != NULL);
	assert(report != NULL);
	assert(request->item_count <= LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	fake.saved_item_count = request->item_count;
	fake.last_save_confirmed = request->confirmed;
	for (size_t i = 0U; i < request->item_count; i++) {
		int written;

		assert(request->item_ids[i] != NULL);
		written = snprintf(fake.saved_ids[i], sizeof(fake.saved_ids[i]), "%s",
				   request->item_ids[i]);
		assert(written >= 0);
		assert((size_t)written < sizeof(fake.saved_ids[i]));
	}
	*report = fake.save_report;
	report->result = fake.save_result;
	return fake.save_result;
}

enum linkr_debugger_config_service_result linkr_debugger_config_service_clear(void)
{
	fake.clear_calls++;
	return fake.clear_result;
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
	response->body = sentinel_body;
	response->body_len = sizeof(sentinel_body) - 1U;
	response->final_chunk = false;
}

static void assert_response_sentinel(const struct http_response_ctx *response)
{
	assert(response->status == (enum http_status)599);
	assert(response->headers == sentinel_headers);
	assert(response->header_count == ARRAY_SIZE_LOCAL(sentinel_headers));
	assert(response->body == sentinel_body);
	assert(response->body_len == sizeof(sentinel_body) - 1U);
	assert(!response->final_chunk);
}

static void call_config(struct http_client_ctx *client,
			enum http_transaction_status transaction_status,
			const void *data, size_t data_len,
			const enum linkr_debugger_config_http_route *route,
			struct http_response_ctx *response)
{
	struct http_request_ctx request = {
		.data = (uint8_t *)data,
		.data_len = data_len,
		.headers = NULL,
		.header_count = 0U,
		.headers_status = HTTP_HEADER_STATUS_NONE,
	};

	assert(linkr_debugger_config_http_handle(
		       client, transaction_status, &request, response, (void *)route) == 0);
}

static void response_text(const struct http_response_ctx *response,
			  char text[LINKR_DEBUGGER_CONFIG_HTTP_RESPONSE_CAP + 1U])
{
	assert(response->body != NULL);
	assert(response->body_len <= LINKR_DEBUGGER_CONFIG_HTTP_RESPONSE_CAP);
	memcpy(text, response->body, response->body_len);
	text[response->body_len] = '\0';
}

static void assert_terminal_response(const struct http_response_ctx *response,
				     enum http_status expected_status,
				     const char *expected_fragment)
{
	char text[LINKR_DEBUGGER_CONFIG_HTTP_RESPONSE_CAP + 1U];

	assert(response->status == expected_status);
	assert(response->headers != NULL);
	assert(response->header_count == 1U);
	assert(strcmp(response->headers[0].name, "Cache-Control") == 0);
	assert(strcmp(response->headers[0].value, "no-store") == 0);
	assert(response->final_chunk);
	response_text(response, text);
	assert(strstr(text, "\"schema\":\"radxa-linkr-debugger.v1\"") != NULL);
	assert(strstr(text, expected_fragment) != NULL);
	assert(response->body_len > 0U);
	assert(text[response->body_len - 1U] == '\n');
}

static void call_final(enum http_method method,
		       const enum linkr_debugger_config_http_route *route,
		       const char *body, struct http_response_ctx *response)
{
	struct http_client_ctx client;

	init_client(&client, method);
	set_response_sentinel(response);
	call_config(&client, HTTP_SERVER_REQUEST_DATA_FINAL, body,
		    body == NULL ? 0U : strlen(body), route, response);
}

static void test_get_all_status_reasons(void)
{
	static const struct {
		enum linkr_debugger_config_service_reason reason;
		const char *name;
	} cases[] = {
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNINITIALIZED, "uninitialized" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY, "ready" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT, "absent" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE,
		  "backend_unavailable" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_STORAGE_ERROR, "storage_error" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_INVALID_SNAPSHOT,
		  "invalid_snapshot" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNSUPPORTED_VERSION,
		  "unsupported_version" },
	};

	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(cases); i++) {
		struct http_response_ctx response;
		char expected[64];

		reset_fixture();
		set_status_reason(cases[i].reason);
		assert(snprintf(expected, sizeof(expected), "\"reason\":\"%s\"",
				cases[i].name) > 0);
		call_final(HTTP_GET, &config_route, NULL, &response);
		assert_terminal_response(&response, HTTP_200_OK, expected);
		assert(fake.status_calls == 1U);
		assert(fake.save_calls == 0U);
		assert(fake.clear_calls == 0U);
	}

	reset_fixture();
	fake.status_result = LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	{
		struct http_response_ctx response;

		call_final(HTTP_GET, &config_route, NULL, &response);
		assert_terminal_response(&response, HTTP_500_INTERNAL_SERVER_ERROR,
					 "\"code\":\"internal_error\"");
	}
}

static void test_put_save_and_confirmation(void)
{
	struct http_response_ctx response;
	const char *safe_body =
		"{\"items\":[\"power/12v_out\"],\"confirm\":false}";
	const char *danger_body =
		"{\"items\":[\"switch/usb\"],\"confirm\":false}";
	const char *confirmed_body =
		"{\"items\":[\"switch/usb\"],\"confirm\":true}";

	reset_fixture();
	call_final(HTTP_PUT, &config_route, safe_body, &response);
	assert_terminal_response(&response, HTTP_200_OK, "\"action\":\"save\"");
	assert(fake.save_calls == 1U);
	assert(fake.saved_item_count == 1U);
	assert(strcmp(fake.saved_ids[0], "power/12v_out") == 0);
	assert(!fake.last_save_confirmed);
	assert(fake.status_calls == 0U && fake.clear_calls == 0U);

	reset_fixture();
	fake.save_result = LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED;
	fake.save_report.confirmation_count = 1U;
	fake.save_report.confirmation_items[0] = linkr_debugger_config_find_item(
		LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH, LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID);
	call_final(HTTP_PUT, &config_route, danger_body, &response);
	assert_terminal_response(&response, HTTP_409_CONFLICT,
				 "\"code\":\"confirmation_required\"");
	{
		char text[LINKR_DEBUGGER_CONFIG_HTTP_RESPONSE_CAP + 1U];

		response_text(&response, text);
		assert(strstr(text, "\"dangerous_items\":[\"switch/usb\"]") != NULL);
	}
	assert(fake.save_calls == 1U);
	assert(!fake.last_save_confirmed);
	assert(fake.clear_calls == 0U);

	reset_fixture();
	call_final(HTTP_PUT, &config_route, confirmed_body, &response);
	assert_terminal_response(&response, HTTP_200_OK, "\"action\":\"save\"");
	assert(fake.save_calls == 1U);
	assert(fake.last_save_confirmed);
}

struct mutation_result_case {
	enum linkr_debugger_config_service_result result;
	enum http_status status;
	const char *code;
	const char *activity;
};

static void assert_response_activity(const struct http_response_ctx *response,
				     const char *activity)
{
	char text[LINKR_DEBUGGER_CONFIG_HTTP_RESPONSE_CAP + 1U];

	if (activity == NULL) {
		return;
	}
	response_text(response, text);
	assert(strstr(text, activity) != NULL);
}

static void test_put_error_mapping(void)
{
	static const struct mutation_result_case cases[] = {
		{ LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION, HTTP_400_BAD_REQUEST,
		  "empty_selection", NULL },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM, HTTP_400_BAD_REQUEST,
		  "unknown_item", NULL },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM, HTTP_400_BAD_REQUEST,
		  "duplicate_item", NULL },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_ITEM_UNAVAILABLE, HTTP_409_CONFLICT,
		  "item_unavailable", NULL },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED, HTTP_409_CONFLICT,
		  "confirmation_required", NULL },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE, HTTP_409_CONFLICT,
		  "busy", "\"activity\":\"capture\"" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_FLASH, HTTP_409_CONFLICT,
		  "busy", "\"activity\":\"ota\"" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE,
		  HTTP_500_INTERNAL_SERVER_ERROR, "backend_unavailable", NULL },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT,
		  HTTP_500_INTERNAL_SERVER_ERROR, "invalid_snapshot", NULL },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_UNSUPPORTED_VERSION,
		  HTTP_500_INTERNAL_SERVER_ERROR, "unsupported_version", NULL },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR,
		  HTTP_500_INTERNAL_SERVER_ERROR, "storage_write_failed", NULL },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_CONTROL_CAPTURE_FAILED,
		  HTTP_500_INTERNAL_SERVER_ERROR, "control_capture_failed", NULL },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_NO_SNAPSHOT,
		  HTTP_500_INTERNAL_SERVER_ERROR, "internal_error", NULL },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED,
		  HTTP_500_INTERNAL_SERVER_ERROR, "apply_failed", NULL },
	};
	const char *body = "{\"items\":[],\"confirm\":false}";

	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(cases); i++) {
		struct http_response_ctx response;
		char code[64];

		reset_fixture();
		fake.save_result = cases[i].result;
		if (cases[i].result == LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED) {
			fake.save_report.confirmation_count = 1U;
			fake.save_report.confirmation_items[0] = &linkr_debugger_config_items[0];
		}
		if (cases[i].result == LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED) {
			fake.save_report.applied_count = 1U;
			fake.save_report.applied_items[0] = &linkr_debugger_config_items[0];
			fake.save_report.failed_item = &linkr_debugger_config_items[4];
			fake.save_report.pending_count = 1U;
			fake.save_report.pending_items[0] = &linkr_debugger_config_items[4];
		}
		assert(snprintf(code, sizeof(code), "\"code\":\"%s\"", cases[i].code) > 0);
		call_final(HTTP_PUT, &config_route, body, &response);
		assert_terminal_response(&response, cases[i].status, code);
		assert_response_activity(&response, cases[i].activity);
		assert(fake.save_calls == 1U);
		assert(fake.clear_calls == 0U);
		if (cases[i].result == LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED) {
			char text[LINKR_DEBUGGER_CONFIG_HTTP_RESPONSE_CAP + 1U];

			response_text(&response, text);
			assert(strstr(text, "\"applied_items\":[\"power/12v_out\"]") != NULL);
			assert(strstr(text, "\"failed_item\":\"switch/sd\"") != NULL);
			assert(strstr(text, "\"pending_items\":[\"switch/sd\"]") != NULL);
		}
	}

	reset_fixture();
	{
		struct http_response_ctx response;

		call_final(HTTP_PUT, &config_route,
			   "{\"items\":[],\"confirm\":false,\"extra\":1}", &response);
		assert_terminal_response(&response, HTTP_400_BAD_REQUEST,
					 "\"code\":\"invalid_json\"");
		{
			char text[LINKR_DEBUGGER_CONFIG_HTTP_RESPONSE_CAP + 1U];

			response_text(&response, text);
			regression_assert_contains(
				"invalid_json_message", text,
				"\"message\":\"request body does not match the config schema\"");
		}
		assert(fake.save_calls == 0U);
	}
}

static void test_operation_error_fallback_envelope(void)
{
	const struct linkr_debugger_config_item_desc foreign_failed_item = {
		.domain = LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		.item_id = UINT8_MAX,
		.id = "power/not-in-catalog",
	};
	struct http_response_ctx response;
	char text[LINKR_DEBUGGER_CONFIG_HTTP_RESPONSE_CAP + 1U];

	reset_fixture();
	fake.save_result = LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED;
	fake.save_report.failed_item = &foreign_failed_item;
	call_final(HTTP_PUT, &config_route,
		   "{\"items\":[\"power/12v_out\"],\"confirm\":true}", &response);

	assert_terminal_response(&response, HTTP_500_INTERNAL_SERVER_ERROR,
				 "\"code\":\"internal_error\"");
	response_text(&response, text);
	assert(strstr(text, "\"schema\":\"radxa-linkr-debugger.v1\"") != NULL);
	assert(strstr(text, "\"ok\":false") != NULL);
	assert(strstr(text, "\"command\":\"config\"") != NULL);
	regression_assert_contains("fallback_action", text,
				   "\"action\":\"save\"");
	assert(strstr(text, "\"code\":\"internal_error\"") != NULL);
	assert(strstr(text, "\"message\":\"internal config error\"") != NULL);
	assert(response.body_len <= LINKR_DEBUGGER_CONFIG_HTTP_RESPONSE_CAP);
	assert(text[response.body_len - 1U] == '\n');
	assert(fake.save_calls == 1U);
	assert(fake.clear_calls == 0U);
}

static void test_delete_preflight_and_errors(void)
{
	struct http_response_ctx response;

	reset_fixture();
	set_status_reason(LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT);
	call_final(HTTP_DELETE, &config_route, NULL, &response);
	assert_terminal_response(&response, HTTP_200_OK, "\"noop\":true");
	assert(fake.status_calls == 1U && fake.clear_calls == 0U);

	reset_fixture();
	call_final(HTTP_DELETE, &config_route, NULL, &response);
	assert_terminal_response(&response, HTTP_200_OK, "\"noop\":false");
	assert(fake.clear_calls == 1U);
	assert(fake.save_calls == 0U);

	for (enum linkr_debugger_config_service_reason reason =
		     LINKR_DEBUGGER_CONFIG_SERVICE_REASON_INVALID_SNAPSHOT;
	     reason <= LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNSUPPORTED_VERSION; reason++) {
		reset_fixture();
		set_status_reason(reason);
		call_final(HTTP_DELETE, &config_route, NULL, &response);
		assert_terminal_response(&response, HTTP_200_OK, "\"noop\":false");
		assert(fake.clear_calls == 1U);
		assert(fake.save_calls == 0U);
	}

	reset_fixture();
	set_status_reason(LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE);
	call_final(HTTP_DELETE, &config_route, NULL, &response);
	assert_terminal_response(&response, HTTP_500_INTERNAL_SERVER_ERROR,
				 "\"code\":\"backend_unavailable\"");
	assert(fake.clear_calls == 0U);

	reset_fixture();
	set_status_reason(LINKR_DEBUGGER_CONFIG_SERVICE_REASON_STORAGE_ERROR);
	call_final(HTTP_DELETE, &config_route, NULL, &response);
	assert_terminal_response(&response, HTTP_500_INTERNAL_SERVER_ERROR,
				 "\"code\":\"storage_error\"");
	assert(fake.clear_calls == 0U);

	reset_fixture();
	fake.clear_result = LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE;
	call_final(HTTP_DELETE, &config_route, NULL, &response);
	assert_terminal_response(&response, HTTP_409_CONFLICT,
				 "\"code\":\"busy\"");
	assert_response_activity(&response, "\"activity\":\"capture\"");
	assert(fake.clear_calls == 1U);

	reset_fixture();
	fake.clear_result = LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR;
	call_final(HTTP_DELETE, &config_route, NULL, &response);
	assert_terminal_response(&response, HTTP_500_INTERNAL_SERVER_ERROR,
				 "\"code\":\"storage_write_failed\"");
	assert(fake.clear_calls == 1U);
}

static void test_fragment_boundaries(void)
{
	struct http_client_ctx client;
	struct http_response_ctx response;
	char body[LINKR_DEBUGGER_HTTP_BODY_CAP + 1U];
	char callback_local_fragment[64];
	const char *valid = "{\"items\":[],\"confirm\":false}";
	const char *prefix = "{\"items\":[\"power/12v_out\"]";
	const char *suffix = ",\"confirm\":false}";
	size_t valid_len = strlen(valid);

	reset_fixture();
	memcpy(body, valid, valid_len);
	memset(body + valid_len, ' ', LINKR_DEBUGGER_HTTP_BODY_CAP - valid_len);
	init_client(&client, HTTP_PUT);
	set_response_sentinel(&response);
	call_config(&client, HTTP_SERVER_REQUEST_DATA_MORE, body, 512U,
		    &config_route, &response);
	assert_response_sentinel(&response);
	call_config(&client, HTTP_SERVER_REQUEST_DATA_FINAL, body + 512U,
		    LINKR_DEBUGGER_HTTP_BODY_CAP - 512U, &config_route, &response);
	assert_terminal_response(&response, HTTP_200_OK, "\"action\":\"save\"");
	assert(fake.save_calls == 1U);

	reset_fixture();
	init_client(&client, HTTP_PUT);
	set_response_sentinel(&response);
	call_config(&client, HTTP_SERVER_REQUEST_DATA_MORE, body, 512U,
		    &config_route, &response);
	assert_response_sentinel(&response);
	call_config(&client, HTTP_SERVER_REQUEST_DATA_MORE, body + 512U, 512U,
		    &config_route, &response);
	assert_response_sentinel(&response);
	call_config(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "x", 1U,
		    &config_route, &response);
	assert_terminal_response(&response, HTTP_413_PAYLOAD_TOO_LARGE,
				 "\"code\":\"body_too_large\"");
	assert(fake.save_calls == 0U);

	set_response_sentinel(&response);
	call_config(&client, HTTP_SERVER_REQUEST_DATA_FINAL, valid, valid_len,
		    &config_route, &response);
	assert_terminal_response(&response, HTTP_200_OK, "\"action\":\"save\"");

	reset_fixture();
	init_client(&client, HTTP_PUT);
	assert(strlen(prefix) < sizeof(callback_local_fragment));
	memcpy(callback_local_fragment, prefix, strlen(prefix) + 1U);
	set_response_sentinel(&response);
	call_config(&client, HTTP_SERVER_REQUEST_DATA_MORE,
		    callback_local_fragment, strlen(callback_local_fragment),
		    &config_route, &response);
	memset(callback_local_fragment, 'x', strlen(prefix));
	call_config(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
		    suffix, strlen(suffix), &config_route, &response);
	assert_terminal_response(&response, HTTP_200_OK, "\"action\":\"save\"");
	assert(strcmp(fake.saved_ids[0], "power/12v_out") == 0);
}

static void test_get_delete_wait_for_final(void)
{
	struct http_client_ctx client;
	struct http_response_ctx response;

	reset_fixture();
	init_client(&client, HTTP_GET);
	set_response_sentinel(&response);
	call_config(&client, HTTP_SERVER_REQUEST_DATA_MORE, "suffix", 6U,
		    &config_route, &response);
	assert_response_sentinel(&response);
	assert(fake.status_calls == 0U);
	call_config(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U,
		    &config_route, &response);
	assert_terminal_response(&response, HTTP_200_OK, "\"action\":\"get\"");

	reset_fixture();
	init_client(&client, HTTP_DELETE);
	set_response_sentinel(&response);
	call_config(&client, HTTP_SERVER_REQUEST_DATA_MORE, "suffix", 6U,
		    &config_route, &response);
	assert_response_sentinel(&response);
	assert(fake.status_calls == 0U && fake.clear_calls == 0U);
	call_config(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U,
		    &config_route, &response);
	assert_terminal_response(&response, HTTP_200_OK, "\"action\":\"clear\"");
}

static void test_client_and_route_interleaving(void)
{
	struct http_client_ctx first_client;
	struct http_client_ctx second_client;
	struct http_response_ctx first_response;
	struct http_response_ctx second_response;

	reset_fixture();
	init_client(&first_client, HTTP_PUT);
	init_client(&second_client, HTTP_PUT);
	set_response_sentinel(&first_response);
	set_response_sentinel(&second_response);
	call_config(&first_client, HTTP_SERVER_REQUEST_DATA_MORE,
		    "{\"items\":[\"power/12v_out\"]", strlen("{\"items\":[\"power/12v_out\"]"),
		    &config_route, &first_response);
	call_config(&second_client, HTTP_SERVER_REQUEST_DATA_MORE,
		    "{\"items\":[\"switch/sd\"]", strlen("{\"items\":[\"switch/sd\"]"),
		    &config_route, &second_response);
	assert_response_sentinel(&first_response);
	assert_response_sentinel(&second_response);
	call_config(&second_client, HTTP_SERVER_REQUEST_DATA_FINAL,
		    ",\"confirm\":false}", strlen(",\"confirm\":false}"),
		    &config_route, &second_response);
	assert_terminal_response(&second_response, HTTP_200_OK,
				 "\"action\":\"save\"");
	call_config(&first_client, HTTP_SERVER_REQUEST_DATA_FINAL,
		    ",\"confirm\":true}", strlen(",\"confirm\":true}"),
		    &config_route, &first_response);
	assert_terminal_response(&first_response, HTTP_200_OK,
				 "\"action\":\"save\"");
	assert(fake.save_calls == 2U);
	assert(fake.saved_item_count == 1U);
	assert(strcmp(fake.saved_ids[0], "power/12v_out") == 0);
	assert(fake.last_save_confirmed);
}

static void test_four_slots_and_fifth_client(void)
{
	struct http_client_ctx clients[LINKR_DEBUGGER_HTTP_BODY_SLOTS + 1U];
	struct http_response_ctx responses[ARRAY_SIZE_LOCAL(clients)];
	const char *prefix = "{\"items\":[\"power/12v_out\"]";
	const char *suffix = ",\"confirm\":false}";

	reset_fixture();
	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(clients); i++) {
		init_client(&clients[i], HTTP_PUT);
		set_response_sentinel(&responses[i]);
	}
	for (size_t i = 0U; i < LINKR_DEBUGGER_HTTP_BODY_SLOTS; i++) {
		call_config(&clients[i], HTTP_SERVER_REQUEST_DATA_MORE,
			    prefix, strlen(prefix), &config_route, &responses[i]);
		assert_response_sentinel(&responses[i]);
	}
	call_config(&clients[LINKR_DEBUGGER_HTTP_BODY_SLOTS],
		    HTTP_SERVER_REQUEST_DATA_MORE, prefix, strlen(prefix),
		    &config_route, &responses[LINKR_DEBUGGER_HTTP_BODY_SLOTS]);
	assert_terminal_response(&responses[LINKR_DEBUGGER_HTTP_BODY_SLOTS],
				 HTTP_500_INTERNAL_SERVER_ERROR,
				 "\"code\":\"internal_error\"");

	for (size_t i = 0U; i < LINKR_DEBUGGER_HTTP_BODY_SLOTS; i++) {
		call_config(&clients[i], HTTP_SERVER_REQUEST_DATA_FINAL,
			    suffix, strlen(suffix), &config_route, &responses[i]);
		assert_terminal_response(&responses[i], HTTP_200_OK,
					 "\"action\":\"save\"");
	}
	assert(fake.save_calls == LINKR_DEBUGGER_HTTP_BODY_SLOTS);
}

static void test_abort_complete_and_final_cleanup(void)
{
	struct http_client_ctx client;
	struct http_response_ctx response;
	const char *prefix = "{\"items\":[\"power/12v_out\"]";
	const char *full =
		"{\"items\":[\"power/12v_out\"],\"confirm\":false}";

	for (enum http_transaction_status terminal = HTTP_SERVER_TRANSACTION_ABORTED;
	     terminal <= HTTP_SERVER_TRANSACTION_COMPLETE; terminal += 3) {
		reset_fixture();
		init_client(&client, HTTP_PUT);
		set_response_sentinel(&response);
		call_config(&client, HTTP_SERVER_REQUEST_DATA_MORE,
			    prefix, strlen(prefix), &config_route, &response);
		assert_response_sentinel(&response);
		call_config(&client, terminal, NULL, 0U, &config_route, &response);
		assert_response_sentinel(&response);
		assert(fake.save_calls == 0U);
		call_config(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
			    full, strlen(full), &config_route, &response);
		assert_terminal_response(&response, HTTP_200_OK,
					 "\"action\":\"save\"");
		assert(fake.save_calls == 1U);
	}

	reset_fixture();
	init_client(&client, HTTP_PUT);
	set_response_sentinel(&response);
	call_config(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
		    full, strlen(full), &config_route, &response);
	assert_terminal_response(&response, HTTP_200_OK, "\"action\":\"save\"");
	assert(strcmp(fake.saved_ids[0], "power/12v_out") == 0);
	call_config(&client, HTTP_SERVER_TRANSACTION_COMPLETE,
		    NULL, 0U, &config_route, &response);
	assert_terminal_response(&response, HTTP_200_OK, "\"action\":\"save\"");
	assert(strcmp(fake.saved_ids[0], "power/12v_out") == 0);
}

static void test_invalid_route_and_method(void)
{
	static const enum linkr_debugger_config_http_route invalid_route =
		(enum linkr_debugger_config_http_route)99;
	struct http_response_ctx response;

	reset_fixture();
	call_final(HTTP_POST, &config_route, "{\"confirm\":true}", &response);
	assert_terminal_response(&response, HTTP_500_INTERNAL_SERVER_ERROR,
				 "\"code\":\"internal_error\"");

	reset_fixture();
	call_final(HTTP_GET, &invalid_route, NULL, &response);
	assert_terminal_response(&response, HTTP_500_INTERNAL_SERVER_ERROR,
				 "\"code\":\"internal_error\"");
}

int main(void)
{
	test_get_all_status_reasons();
	test_put_save_and_confirmation();
	test_put_error_mapping();
	test_operation_error_fallback_envelope();
	test_delete_preflight_and_errors();
	test_fragment_boundaries();
	test_get_delete_wait_for_final();
	test_client_and_route_interleaving();
	test_four_slots_and_fifth_client();
	test_abort_complete_and_final_cleanup();
	test_invalid_route_and_method();
	assert(regression_failure_count == 0U);
	puts("linkr_debugger_config_http: all tests passed");
	return 0;
}
