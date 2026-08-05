#include "test_linkr_debugger_config_http_stubs.h"

#include "../../src/linkr_debugger_config_http.h"
#include "../../src/linkr_debugger_config_http_encode.h"
#include "../../src/linkr_debugger_config_http_json.h"
#include "../../src/linkr_debugger_config_http_parse.h"
#include "../../src/linkr_debugger_config_http_result.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE_LOCAL(array) (sizeof(array) / sizeof((array)[0]))
#define RESPONSE_STORAGE_SIZE (LINKR_DEBUGGER_CONFIG_HTTP_RESPONSE_CAP + 1U)

_Static_assert(LINKR_DEBUGGER_HTTP_BODY_CAP == 1024U,
	       "config HTTP request cap changed");
_Static_assert(sizeof(((struct linkr_debugger_config_http_save_payload *)0)->storage) ==
	       1025U, "config HTTP parser storage changed");
_Static_assert(ARRAY_SIZE_LOCAL(
	       ((struct linkr_debugger_config_http_save_payload *)0)->request.item_ids) ==
	       LINKR_DEBUGGER_CONFIG_MAX_ENTRIES,
	       "config HTTP parser item bound changed");
_Static_assert(LINKR_DEBUGGER_CONFIG_MAX_ENTRIES == 23U,
	       "config HTTP catalog bound changed");
_Static_assert(RESPONSE_STORAGE_SIZE == 4161U,
	       "config HTTP response storage changed");

static void assert_contains(const char *text, const char *expected)
{
	assert(strstr(text, expected) != NULL);
}

static size_t append_format(char *buffer, size_t capacity, size_t offset,
			    const char *format, ...)
{
	va_list args;
	int written;

	assert(offset < capacity);
	va_start(args, format);
	written = vsnprintf(buffer + offset, capacity - offset, format, args);
	va_end(args);
	assert(written >= 0);
	assert((size_t)written < capacity - offset);
	return offset + (size_t)written;
}

static size_t build_save_json(char *buffer, size_t capacity, size_t item_count,
			      bool confirm, bool confirm_first)
{
	size_t offset = 0U;

	if (confirm_first) {
		offset = append_format(buffer, capacity, offset,
				       "{\"confirm\":%s,\"items\":[",
				       confirm ? "true" : "false");
	} else {
		offset = append_format(buffer, capacity, offset, "{\"items\":[");
	}

	for (size_t i = 0U; i < item_count; i++) {
		const struct linkr_debugger_config_item_desc *item =
			&linkr_debugger_config_items[i % linkr_debugger_config_item_count];

		offset = append_format(buffer, capacity, offset, "%s\"%s\"",
				       i == 0U ? "" : ",", item->id);
	}

	if (confirm_first) {
		offset = append_format(buffer, capacity, offset, "]}");
	} else {
		offset = append_format(buffer, capacity, offset,
				       "],\"confirm\":%s}", confirm ? "true" : "false");
	}
	return offset;
}

static void assert_save_parse_result(
	const char *json, enum linkr_debugger_config_http_parse_result expected)
{
	struct linkr_debugger_config_http_save_payload payload;

	memset(&payload, 0xa5, sizeof(payload));
	assert(linkr_debugger_config_http_parse_save(
		       (const uint8_t *)json, strlen(json), &payload) == expected);
}

static void test_save_parser_schema(void)
{
	static const char *const invalid[] = {
		"",
		"{}",
		"{\"items\":[]}",
		"{\"confirm\":false}",
		"{\"items\":[],\"confirm\":false,\"extra\":0}",
		"{\"items\":[],\"items\":[],\"confirm\":false}",
		"{\"items\":[],\"confirm\":false,\"confirm\":false}",
		"{\"items\":\"power/12v_out\",\"confirm\":false}",
		"{\"items\":[],\"confirm\":0}",
		"{\"items\":[1],\"confirm\":false}",
		"{\"items\":[\"power/12v_\\qout\"],\"confirm\":false}",
		"{\"items\":[],\"confirm\":false}x",
	};
	struct linkr_debugger_config_http_save_payload payload;
	char json[LINKR_DEBUGGER_HTTP_BODY_CAP + 2U];
	size_t length;

	length = build_save_json(json, sizeof(json), 2U, false, false);
	assert(linkr_debugger_config_http_parse_save(
		       (const uint8_t *)json, length, &payload) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_PARSE_OK);
	assert(payload.request.item_count == 2U);
	assert(!payload.request.confirmed);
	for (size_t i = 0U; i < payload.request.item_count; i++) {
		assert(strcmp(payload.request.item_ids[i],
			      linkr_debugger_config_items[i].id) == 0);
		assert(payload.request.item_ids[i] >= payload.storage);
		assert(payload.request.item_ids[i] < payload.storage + sizeof(payload.storage));
	}

	length = build_save_json(json, sizeof(json), 2U, true, true);
	assert(linkr_debugger_config_http_parse_save(
		       (const uint8_t *)json, length, &payload) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_PARSE_OK);
	assert(payload.request.confirmed);

	assert_save_parse_result("{\"items\":[],\"confirm\":false}",
				 LINKR_DEBUGGER_CONFIG_HTTP_PARSE_OK);
	assert_save_parse_result(
		"{\"items\":[\"future/item\",\"future/item\"],\"confirm\":false}",
		LINKR_DEBUGGER_CONFIG_HTTP_PARSE_OK);

	length = build_save_json(json, sizeof(json),
				 LINKR_DEBUGGER_CONFIG_MAX_ENTRIES, false, false);
	assert(linkr_debugger_config_http_parse_save(
		       (const uint8_t *)json, length, &payload) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_PARSE_OK);
	assert(payload.request.item_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);

	length = build_save_json(json, sizeof(json),
				 LINKR_DEBUGGER_CONFIG_MAX_ENTRIES + 1U, false, false);
	assert(linkr_debugger_config_http_parse_save(
		       (const uint8_t *)json, length, &payload) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_PARSE_INVALID_JSON);

	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(invalid); i++) {
		assert_save_parse_result(invalid[i],
					 LINKR_DEBUGGER_CONFIG_HTTP_PARSE_INVALID_JSON);
	}

	length = build_save_json(json, sizeof(json), 0U, false, false);
	memset(json + length, ' ', LINKR_DEBUGGER_HTTP_BODY_CAP - length);
	assert(linkr_debugger_config_http_parse_save(
		       (const uint8_t *)json, LINKR_DEBUGGER_HTTP_BODY_CAP, &payload) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_PARSE_OK);
	json[LINKR_DEBUGGER_HTTP_BODY_CAP] = ' ';
	assert(linkr_debugger_config_http_parse_save(
		       (const uint8_t *)json, LINKR_DEBUGGER_HTTP_BODY_CAP + 1U, &payload) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_PARSE_INVALID_JSON);
}

static void test_json_writer(void)
{
	struct linkr_debugger_config_http_json json;
	char buffer[64];
	char short_buffer[8];

	linkr_debugger_config_http_json_init(&json, buffer, sizeof(buffer));
	assert(linkr_debugger_config_http_json_append(&json, "{\"v\":") == 0);
	assert(linkr_debugger_config_http_json_string(&json, "a\"b\\c\n") == 0);
	assert(linkr_debugger_config_http_json_append(&json, "}") == 0);
	assert(strcmp(buffer, "{\"v\":\"a\\\"b\\\\c\\n\"}") == 0);
	assert(json.length == strlen(buffer));
	assert(!json.failed);

	linkr_debugger_config_http_json_init(&json, short_buffer, sizeof(short_buffer));
	assert(linkr_debugger_config_http_json_append(&json, "12345678") != 0);
	assert(json.failed);
	assert(json.length == 0U);
	assert(short_buffer[0] == '\0');
	assert(linkr_debugger_config_http_json_append(&json, "x") != 0);
	linkr_debugger_config_http_json_discard(&json);
	assert(json.failed);
	assert(json.length == 0U);
}

static uint8_t longest_valid_value(const struct linkr_debugger_config_item_desc *item)
{
	if (item->domain == LINKR_DEBUGGER_CONFIG_DOMAIN_POWER) {
		return LINKR_DEBUGGER_CONFIG_POWER_OFF;
	}
	if (item->domain == LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO) {
		return LINKR_DEBUGGER_CONFIG_GPIO_VALUE_MASK;
	}

	switch (item->item_id) {
	case LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID:
		return LINKR_DEBUGGER_CONFIG_SD_USB_READER;
	case LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID:
		return LINKR_DEBUGGER_CONFIG_USB_TARGET;
	case LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID:
		return LINKR_DEBUGGER_CONFIG_TF_WP_PROTECTED;
	case LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID:
		return LINKR_DEBUGGER_CONFIG_VIN_3V3;
	default:
		assert(false);
		return 0U;
	}
}

static bool value_requires_confirmation(
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

static void refresh_status_counts(struct linkr_debugger_config_service_status *status)
{
	status->saved_count = 0U;
	status->applied_count = 0U;
	status->pending_count = 0U;
	status->failed_count = 0U;
	status->failed_item = NULL;
	for (size_t i = 0U; i < status->item_count; i++) {
		struct linkr_debugger_config_item_status *row = &status->items[i];

		status->saved_count += row->saved ? 1U : 0U;
		switch (row->apply_state) {
		case LINKR_DEBUGGER_CONFIG_APPLY_NOT_SAVED:
			break;
		case LINKR_DEBUGGER_CONFIG_APPLY_APPLIED:
			status->applied_count++;
			break;
		case LINKR_DEBUGGER_CONFIG_APPLY_PENDING:
			status->pending_count++;
			break;
		case LINKR_DEBUGGER_CONFIG_APPLY_FAILED:
			status->failed_count++;
			if (status->failed_item == NULL) {
				status->failed_item = row->item;
			}
			break;
		default:
			break;
		}
	}
}

static void fill_catalog_status(struct linkr_debugger_config_service_status *status)
{
	memset(status, 0, sizeof(*status));
	status->available = true;
	status->reason = LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY;
	status->snapshot_present = true;
	status->snapshot_version = LINKR_DEBUGGER_CONFIG_VERSION;
	status->item_count = linkr_debugger_config_item_count;
	for (size_t i = 0U; i < status->item_count; i++) {
		struct linkr_debugger_config_item_status *row = &status->items[i];
		uint8_t value = longest_valid_value(&linkr_debugger_config_items[i]);

		row->item = &linkr_debugger_config_items[i];
		row->current_available = true;
		row->current_value = value;
		row->current_requires_confirmation =
			value_requires_confirmation(row->item, value);
		row->saved = true;
		row->saved_value = value;
		row->saved_requires_confirmation = row->current_requires_confirmation;
		row->apply_state = LINKR_DEBUGGER_CONFIG_APPLY_PENDING;
	}
	refresh_status_counts(status);
}

static size_t catalog_index(uint8_t domain, uint8_t item_id)
{
	const struct linkr_debugger_config_item_desc *item =
		linkr_debugger_config_find_item(domain, item_id);

	assert(item != NULL);
	return (size_t)(item - linkr_debugger_config_items);
}

static size_t encode_status(struct linkr_debugger_config_service_status *status,
			    char buffer[RESPONSE_STORAGE_SIZE])
{
	size_t encoded_size = 99U;

	refresh_status_counts(status);
	assert(linkr_debugger_config_http_encode_get(
		       status, buffer, RESPONSE_STORAGE_SIZE, &encoded_size) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_OK);
	assert(encoded_size == strlen(buffer));
	assert(encoded_size <= LINKR_DEBUGGER_CONFIG_HTTP_RESPONSE_CAP);
	return encoded_size;
}

static void set_row_value(struct linkr_debugger_config_service_status *status,
			  uint8_t domain, uint8_t item_id, uint8_t value)
{
	struct linkr_debugger_config_item_status *row =
		&status->items[catalog_index(domain, item_id)];
	bool required = value_requires_confirmation(row->item, value);

	row->current_value = value;
	row->saved_value = value;
	row->current_requires_confirmation = required;
	row->saved_requires_confirmation = required;
}

static void assert_typed_value(uint8_t domain, uint8_t item_id, uint8_t value,
			       const char *expected)
{
	struct linkr_debugger_config_service_status status;
	char buffer[RESPONSE_STORAGE_SIZE];

	fill_catalog_status(&status);
	set_row_value(&status, domain, item_id, value);
	(void)encode_status(&status, buffer);
	assert_contains(buffer, expected);
}

static void test_typed_values(void)
{
	assert_typed_value(LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
			   LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
			   LINKR_DEBUGGER_CONFIG_POWER_OFF,
			   "\"id\":\"power/12v_out\",\"kind\":\"power\"," \
			   "\"current\":{\"state\":\"off\"}");
	assert_typed_value(LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
			   LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
			   LINKR_DEBUGGER_CONFIG_POWER_ON,
			   "\"id\":\"power/12v_out\",\"kind\":\"power\"," \
			   "\"current\":{\"state\":\"on\"}");
	assert_typed_value(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			   LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID,
			   LINKR_DEBUGGER_CONFIG_SD_TARGET,
			   "\"id\":\"switch/sd\",\"kind\":\"switch\"," \
			   "\"current\":{\"route\":\"target\"}");
	assert_typed_value(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			   LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID,
			   LINKR_DEBUGGER_CONFIG_SD_USB_READER,
			   "\"current\":{\"route\":\"usb-reader\"}");
	assert_typed_value(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			   LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID,
			   LINKR_DEBUGGER_CONFIG_USB_PC,
			   "\"current\":{\"route\":\"pc\"}");
	assert_typed_value(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			   LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID,
			   LINKR_DEBUGGER_CONFIG_USB_TARGET,
			   "\"id\":\"switch/usb\",\"kind\":\"switch\"," \
			   "\"current\":{\"route\":\"target\"}");
	assert_typed_value(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			   LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID,
			   LINKR_DEBUGGER_CONFIG_TF_WP_WRITABLE,
			   "\"current\":{\"route\":\"writable\"}");
	assert_typed_value(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			   LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID,
			   LINKR_DEBUGGER_CONFIG_TF_WP_PROTECTED,
			   "\"current\":{\"route\":\"protected\"}");
	assert_typed_value(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			   LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID,
			   LINKR_DEBUGGER_CONFIG_VIN_1V8,
			   "\"current\":{\"route\":\"1.8v\"}");
	assert_typed_value(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			   LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID,
			   LINKR_DEBUGGER_CONFIG_VIN_3V3,
			   "\"current\":{\"route\":\"3.3v\"}");
	assert_typed_value(LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 7U, 0U,
			   "\"current\":{\"direction\":\"input\",\"value\":0}");
	assert_typed_value(LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 7U,
			   LINKR_DEBUGGER_CONFIG_GPIO_LEVEL,
			   "\"current\":{\"direction\":\"input\",\"value\":1}");
	assert_typed_value(LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 7U,
			   LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT,
			   "\"current\":{\"direction\":\"output\",\"value\":0}");
	assert_typed_value(LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 7U,
			   LINKR_DEBUGGER_CONFIG_GPIO_VALUE_MASK,
			   "\"current\":{\"direction\":\"output\",\"value\":1}");
}

static void test_nullability_and_confirmation_precedence(void)
{
	struct linkr_debugger_config_service_status status;
	struct linkr_debugger_config_item_status *row;
	char buffer[RESPONSE_STORAGE_SIZE];

	fill_catalog_status(&status);
	row = &status.items[catalog_index(LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
					 LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID)];
	row->current_available = false;
	row->saved = true;
	row->saved_value = LINKR_DEBUGGER_CONFIG_POWER_ON;
	row->saved_requires_confirmation = true;
	row->apply_state = LINKR_DEBUGGER_CONFIG_APPLY_PENDING;
	(void)encode_status(&status, buffer);
	assert_contains(buffer,
		"\"current\":null,\"saved\":{\"state\":\"on\"},"
		"\"selected\":true,\"requires_confirm\":true,"
		"\"apply_state\":\"pending\"");

	fill_catalog_status(&status);
	row = &status.items[catalog_index(LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
					 LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID)];
	row->saved = false;
	row->apply_state = LINKR_DEBUGGER_CONFIG_APPLY_NOT_SAVED;
	row->current_requires_confirmation = false;
	(void)encode_status(&status, buffer);
	assert_contains(buffer,
		"\"current\":{\"state\":\"off\"},\"saved\":null,"
		"\"selected\":false,\"requires_confirm\":false,"
		"\"apply_state\":\"not_saved\"");

	row->current_available = false;
	(void)encode_status(&status, buffer);
	assert_contains(buffer,
		"\"current\":null,\"saved\":null,\"selected\":false,"
		"\"requires_confirm\":null,\"apply_state\":\"not_saved\"");
}

static bool json_structure_complete(const char *json, size_t length)
{
	char stack[64];
	size_t depth = 0U;
	bool in_string = false;
	bool escaped = false;

	for (size_t i = 0U; i < length; i++) {
		char ch = json[i];

		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == '"') {
				in_string = false;
			} else if ((unsigned char)ch < 0x20U) {
				return false;
			}
			continue;
		}
		if (ch == '"') {
			in_string = true;
		} else if (ch == '{' || ch == '[') {
			if (depth == ARRAY_SIZE_LOCAL(stack)) {
				return false;
			}
			stack[depth++] = ch;
		} else if (ch == '}' || ch == ']') {
			char expected = ch == '}' ? '{' : '[';

			if (depth == 0U || stack[--depth] != expected) {
				return false;
			}
		}
	}
	return !in_string && !escaped && depth == 0U && length > 0U &&
	       json[length - 1U] == '\n';
}

static size_t count_occurrences(const char *text, const char *needle)
{
	size_t count = 0U;
	size_t needle_len = strlen(needle);

	while ((text = strstr(text, needle)) != NULL) {
		count++;
		text += needle_len;
	}
	return count;
}

static void test_complete_catalog_and_capacity(void)
{
	struct linkr_debugger_config_service_status status;
	char full[RESPONSE_STORAGE_SIZE];
	char exact[RESPONSE_STORAGE_SIZE];
	char short_buffer[RESPONSE_STORAGE_SIZE];
	const char *cursor;
	size_t encoded_size;
	size_t exact_size = 99U;
	size_t short_size = 99U;

	fill_catalog_status(&status);
	encoded_size = encode_status(&status, full);
	assert(encoded_size == 4150U);
	assert(encoded_size <= 4160U);
	assert(json_structure_complete(full, encoded_size));
	assert(count_occurrences(full, "\"id\":") ==
	       LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert_contains(full, "\"schema\":\"radxa-linkr-debugger.v1\"");
	assert_contains(full, "\"backend\":{\"available\":true,\"reason\":\"ready\"}");
	assert_contains(full, "\"snapshot\":{\"present\":true,\"version\":1}");
	assert_contains(full, "\"pending\":23");

	cursor = full;
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		char needle[64];
		const char *found;

		assert(snprintf(needle, sizeof(needle), "\"id\":\"%s\"",
				linkr_debugger_config_items[i].id) > 0);
		found = strstr(cursor, needle);
		assert(found != NULL);
		cursor = found + strlen(needle);
	}

	assert(linkr_debugger_config_http_encode_get(
		       &status, exact, 4151U, &exact_size) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_OK);
	assert(exact_size == 4150U);
	assert(memcmp(exact, full, 4151U) == 0);

	memset(short_buffer, 0xa5, sizeof(short_buffer));
	assert(linkr_debugger_config_http_encode_get(
		       &status, short_buffer, 4150U, &short_size) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_NO_SPACE);
	assert(short_size == 0U);
	assert(short_buffer[0] == '\0');

	{
		size_t invalid_size = 99U;

		status.snapshot_version = 2U;
		assert(linkr_debugger_config_http_encode_get(
			       &status, full, sizeof(full), &invalid_size) ==
		       LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_INVALID_STATE);
		assert(invalid_size == 0U);
	}
}

static void assert_encode_get_invalid(
	struct linkr_debugger_config_service_status *status)
{
	char buffer[RESPONSE_STORAGE_SIZE];
	size_t encoded_size = 99U;

	memset(buffer, 0xa5, sizeof(buffer));
	assert(linkr_debugger_config_http_encode_get(
		       status, buffer, sizeof(buffer), &encoded_size) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_INVALID_STATE);
	assert(encoded_size == 0U);
}

static void test_encoder_invalid_states(void)
{
	struct linkr_debugger_config_service_status status;
	const struct linkr_debugger_config_item_desc *saved_item;

	fill_catalog_status(&status);
	status.item_count--;
	assert_encode_get_invalid(&status);

	fill_catalog_status(&status);
	saved_item = status.items[0].item;
	status.items[0].item = status.items[1].item;
	status.items[1].item = saved_item;
	assert_encode_get_invalid(&status);

	fill_catalog_status(&status);
	status.reason = (enum linkr_debugger_config_service_reason)99;
	assert_encode_get_invalid(&status);

	fill_catalog_status(&status);
	status.items[0].apply_state = (enum linkr_debugger_config_apply_state)99;
	assert_encode_get_invalid(&status);

	fill_catalog_status(&status);
	status.items[0].current_value = 2U;
	assert_encode_get_invalid(&status);

	fill_catalog_status(&status);
	status.items[catalog_index(LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 7U)].current_value = 4U;
	assert_encode_get_invalid(&status);
}

static void test_reason_and_apply_state_names(void)
{
	static const char *const reasons[] = {
		"uninitialized", "ready", "absent", "backend_unavailable",
		"storage_error", "invalid_snapshot", "unsupported_version",
	};
	static const char *const states[] = {
		"not_saved", "applied", "pending", "failed",
	};

	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(reasons); i++) {
		assert(strcmp(linkr_debugger_config_http_reason_name(
			      (enum linkr_debugger_config_service_reason)i), reasons[i]) == 0);
	}
	assert(linkr_debugger_config_http_reason_name(
		       (enum linkr_debugger_config_service_reason)99) == NULL);
	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(states); i++) {
		assert(strcmp(linkr_debugger_config_http_apply_state_name(
			      (enum linkr_debugger_config_apply_state)i), states[i]) == 0);
	}
	assert(linkr_debugger_config_http_apply_state_name(
		       (enum linkr_debugger_config_apply_state)99) == NULL);
}

struct mapping_expectation {
	bool error;
	enum http_status status;
	const char *code;
	const char *message;
	const char *activity;
};

#define MAP_OK { false, HTTP_200_OK, NULL, NULL, NULL }
#define MAP_ERROR(http_status_value, code_value, message_value) \
	{ true, http_status_value, code_value, message_value, NULL }
#define MAP_BUSY(message_value, activity_value) \
	{ true, HTTP_409_CONFLICT, "busy", message_value, activity_value }
#define MAP_INTERNAL \
	MAP_ERROR(HTTP_500_INTERNAL_SERVER_ERROR, "internal_error", "internal config error")

static const struct mapping_expectation mapping_expectations
	[LINKR_DEBUGGER_CONFIG_HTTP_ACTION_CLEAR + 1U]
	[LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED + 1U] = {
	[LINKR_DEBUGGER_CONFIG_HTTP_ACTION_GET] = {
		MAP_OK, MAP_INTERNAL, MAP_INTERNAL, MAP_INTERNAL,
		MAP_INTERNAL, MAP_INTERNAL, MAP_INTERNAL, MAP_INTERNAL,
		MAP_INTERNAL, MAP_INTERNAL, MAP_INTERNAL, MAP_INTERNAL,
		MAP_INTERNAL, MAP_INTERNAL, MAP_INTERNAL, MAP_INTERNAL,
	},
	[LINKR_DEBUGGER_CONFIG_HTTP_ACTION_SAVE] = {
		MAP_OK,
		MAP_INTERNAL,
		MAP_ERROR(HTTP_400_BAD_REQUEST, "empty_selection",
			  "at least one config item is required"),
		MAP_ERROR(HTTP_400_BAD_REQUEST, "unknown_item", "unknown config item"),
		MAP_ERROR(HTTP_400_BAD_REQUEST, "duplicate_item", "duplicate config item"),
		MAP_ERROR(HTTP_409_CONFLICT, "item_unavailable",
			  "config item is unavailable"),
		MAP_ERROR(HTTP_409_CONFLICT, "confirmation_required",
			  "confirmation is required"),
		MAP_BUSY("configuration is blocked by active capture", "capture"),
		MAP_BUSY("configuration is blocked by active OTA", "ota"),
		MAP_ERROR(HTTP_500_INTERNAL_SERVER_ERROR, "backend_unavailable",
			  "config storage backend is unavailable"),
		MAP_INTERNAL,
		MAP_ERROR(HTTP_500_INTERNAL_SERVER_ERROR, "invalid_snapshot",
			  "saved config snapshot is invalid"),
		MAP_ERROR(HTTP_500_INTERNAL_SERVER_ERROR, "unsupported_version",
			  "saved config snapshot version is unsupported"),
		MAP_ERROR(HTTP_500_INTERNAL_SERVER_ERROR, "storage_write_failed",
			  "failed to update config storage"),
		MAP_ERROR(HTTP_500_INTERNAL_SERVER_ERROR, "control_capture_failed",
			  "failed to capture current control state"),
		MAP_ERROR(HTTP_500_INTERNAL_SERVER_ERROR, "apply_failed",
			  "failed to apply saved config"),
	},
	[LINKR_DEBUGGER_CONFIG_HTTP_ACTION_CLEAR] = {
		MAP_OK,
		MAP_INTERNAL,
		MAP_INTERNAL,
		MAP_INTERNAL,
		MAP_INTERNAL,
		MAP_INTERNAL,
		MAP_INTERNAL,
		MAP_BUSY("configuration is blocked by active capture", "capture"),
		MAP_BUSY("configuration is blocked by active OTA", "ota"),
		MAP_ERROR(HTTP_500_INTERNAL_SERVER_ERROR, "backend_unavailable",
			  "config storage backend is unavailable"),
		MAP_INTERNAL,
		MAP_INTERNAL,
		MAP_INTERNAL,
		MAP_ERROR(HTTP_500_INTERNAL_SERVER_ERROR, "storage_write_failed",
			  "failed to update config storage"),
		MAP_INTERNAL,
		MAP_INTERNAL,
	},
};

static void assert_optional_string(const char *actual, const char *expected)
{
	if (expected == NULL) {
		assert(actual == NULL);
	} else {
		assert(actual != NULL);
		assert(strcmp(actual, expected) == 0);
	}
}

static void test_exhaustive_result_mapping(void)
{
	for (int action = LINKR_DEBUGGER_CONFIG_HTTP_ACTION_GET;
	     action <= LINKR_DEBUGGER_CONFIG_HTTP_ACTION_CLEAR; action++) {
		for (int result = LINKR_DEBUGGER_CONFIG_SERVICE_OK;
		     result <= LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED; result++) {
			const struct mapping_expectation *expected =
				&mapping_expectations[action][result];
			struct linkr_debugger_config_http_error error = {
				.status = HTTP_200_OK,
				.code = "sentinel",
				.message = "sentinel",
				.activity = "sentinel",
			};
			bool mapped = linkr_debugger_config_http_map_service_result(
				(enum linkr_debugger_config_http_action)action,
				(enum linkr_debugger_config_service_result)result, &error);

			assert(mapped == expected->error);
			if (!mapped) {
				continue;
			}
			assert(error.status == expected->status);
			assert_optional_string(error.code, expected->code);
			assert_optional_string(error.message, expected->message);
			assert_optional_string(error.activity, expected->activity);
		}
	}

	for (int action = LINKR_DEBUGGER_CONFIG_HTTP_ACTION_GET;
	     action <= LINKR_DEBUGGER_CONFIG_HTTP_ACTION_CLEAR; action++) {
		struct linkr_debugger_config_http_error error;

		assert(linkr_debugger_config_http_map_service_result(
			       (enum linkr_debugger_config_http_action)action,
			       (enum linkr_debugger_config_service_result)999, &error));
		assert(error.status == HTTP_500_INTERNAL_SERVER_ERROR);
		assert(strcmp(error.code, "internal_error") == 0);
	}
}

static void test_operation_encoders_and_error_details(void)
{
	const struct linkr_debugger_config_item_desc *power =
		linkr_debugger_config_find_item(
			LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
			LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID);
	const struct linkr_debugger_config_item_desc *usb =
		linkr_debugger_config_find_item(
			LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID);
	struct linkr_debugger_config_save_request request = {
		.item_count = 2U,
		.item_ids = { "switch/usb", "power/12v_out" },
		.confirmed = true,
	};
	struct linkr_debugger_config_operation_report report = {0};
	struct linkr_debugger_config_http_error error;
	char buffer[RESPONSE_STORAGE_SIZE];
	size_t encoded_size;

	assert(power != NULL && usb != NULL);
	report.snapshot_version = LINKR_DEBUGGER_CONFIG_VERSION;
	report.confirmation_count = 1U;
	report.confirmation_items[0] = usb;
	assert(linkr_debugger_config_http_encode_save(
		       &request, &report, buffer, sizeof(buffer), &encoded_size) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_OK);
	assert_contains(buffer, "\"action\":\"save\"");
	assert_contains(buffer,
		"\"saved_items\":[\"switch/usb\",\"power/12v_out\"]");
	assert_contains(buffer, "\"confirmation_items\":[\"switch/usb\"]");
	assert_contains(buffer, "\"applied_items\":[]");
	assert_contains(buffer, "\"snapshot\":{\"present\":true,\"version\":1}");

	assert(linkr_debugger_config_http_encode_clear(
		       false, buffer, sizeof(buffer), &encoded_size) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_OK);
	assert_contains(buffer, "\"action\":\"clear\",\"noop\":false");
	assert_contains(buffer,
		"\"snapshot\":{\"present\":false,\"version\":null},\"pending\":0");
	assert(linkr_debugger_config_http_encode_clear(
		       true, buffer, sizeof(buffer), &encoded_size) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_OK);
	assert_contains(buffer, "\"action\":\"clear\",\"noop\":true");

	memset(&report, 0, sizeof(report));
	report.confirmation_count = 1U;
	report.confirmation_items[0] = usb;
	assert(linkr_debugger_config_http_map_service_result(
		       LINKR_DEBUGGER_CONFIG_HTTP_ACTION_SAVE,
		       LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED, &error));
	assert(linkr_debugger_config_http_encode_error(
		       LINKR_DEBUGGER_CONFIG_HTTP_ACTION_SAVE, &error, &report,
		       buffer, sizeof(buffer), &encoded_size) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_OK);
	assert_contains(buffer, "\"code\":\"confirmation_required\"");
	assert_contains(buffer, "\"dangerous_items\":[\"switch/usb\"]");

	assert(linkr_debugger_config_http_map_service_result(
		       LINKR_DEBUGGER_CONFIG_HTTP_ACTION_SAVE,
		       LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE, &error));
	assert(linkr_debugger_config_http_encode_error(
		       LINKR_DEBUGGER_CONFIG_HTTP_ACTION_SAVE, &error, NULL,
		       buffer, sizeof(buffer), &encoded_size) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_OK);
	assert_contains(buffer, "\"code\":\"busy\"");
	assert_contains(buffer, "\"activity\":\"capture\"");

	memset(&report, 0, sizeof(report));
	report.applied_count = 1U;
	report.applied_items[0] = power;
	report.failed_item = usb;
	report.pending_count = 1U;
	report.pending_items[0] = usb;
	assert(linkr_debugger_config_http_map_service_result(
		       LINKR_DEBUGGER_CONFIG_HTTP_ACTION_SAVE,
		       LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED, &error));
	assert(linkr_debugger_config_http_encode_error(
		       LINKR_DEBUGGER_CONFIG_HTTP_ACTION_SAVE, &error, &report,
		       buffer, sizeof(buffer), &encoded_size) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_OK);
	assert_contains(buffer, "\"code\":\"apply_failed\"");
	assert_contains(buffer, "\"applied_items\":[\"power/12v_out\"]");
	assert_contains(buffer, "\"failed_item\":\"switch/usb\"");
	assert_contains(buffer, "\"pending_items\":[\"switch/usb\"]");
}

static void test_confirmation_error_places_dangerous_items_outside_error(void)
{
	const struct linkr_debugger_config_item_desc *usb =
		linkr_debugger_config_find_item(
			LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID);
	struct linkr_debugger_config_operation_report report = {0};
	struct linkr_debugger_config_http_error error;
	char buffer[RESPONSE_STORAGE_SIZE];
	size_t encoded_size;

	assert(usb != NULL);
	report.confirmation_count = 1U;
	report.confirmation_items[0] = usb;
	assert(linkr_debugger_config_http_map_service_result(
		       LINKR_DEBUGGER_CONFIG_HTTP_ACTION_SAVE,
		       LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED, &error));
	assert(linkr_debugger_config_http_encode_error(
		       LINKR_DEBUGGER_CONFIG_HTTP_ACTION_SAVE, &error, &report,
		       buffer, sizeof(buffer), &encoded_size) ==
	       LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_OK);
	assert_contains(buffer,
		"\"message\":\"confirmation is required\"},\"dangerous_items\":[\"switch/usb\"]");
	assert(strcmp(buffer,
		"{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":false,\"command\":\"config\","
		"\"action\":\"save\",\"error\":{\"code\":\"confirmation_required\","
		"\"message\":\"confirmation is required\"},\"dangerous_items\":[\"switch/usb\"]}\n") == 0);
}

int main(void)
{
	test_save_parser_schema();
	test_json_writer();
	test_typed_values();
	test_nullability_and_confirmation_precedence();
	test_complete_catalog_and_capacity();
	test_encoder_invalid_states();
	test_reason_and_apply_state_names();
	test_exhaustive_result_mapping();
	test_operation_encoders_and_error_details();
	test_confirmation_error_places_dangerous_items_outside_error();
	puts("linkr_debugger_config_http_codec: all tests passed");
	return 0;
}
