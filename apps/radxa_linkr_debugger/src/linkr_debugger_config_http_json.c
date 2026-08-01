#include "linkr_debugger_config_http_json.h"

#include <stdarg.h>
#include <stdio.h>

static int json_fail(struct linkr_debugger_config_http_json *json)
{
	if (json != NULL) {
		if (json->data != NULL && json->capacity > 0U) {
			json->data[0] = '\0';
		}
		json->length = 0U;
		json->failed = true;
	}
	return -1;
}

void linkr_debugger_config_http_json_init(
	struct linkr_debugger_config_http_json *json, char *data, size_t capacity)
{
	if (json == NULL) {
		return;
	}
	json->data = data;
	json->capacity = capacity;
	json->length = 0U;
	json->failed = data == NULL || capacity == 0U;
	if (data != NULL && capacity > 0U) {
		data[0] = '\0';
	}
}

int linkr_debugger_config_http_json_append(
	struct linkr_debugger_config_http_json *json, const char *format, ...)
{
	va_list args;
	int written;
	size_t remaining;

	if (json == NULL || format == NULL) {
		return json_fail(json);
	}
	if (json->failed || json->data == NULL || json->capacity == 0U ||
	    json->length >= json->capacity || json->data[json->length] != '\0') {
		return json_fail(json);
	}

	remaining = json->capacity - json->length;
	va_start(args, format);
	written = vsnprintf(json->data + json->length, remaining, format, args);
	va_end(args);
	if (written < 0 || (size_t)written >= remaining) {
		return json_fail(json);
	}
	json->length += (size_t)written;
	return 0;
}

int linkr_debugger_config_http_json_string(
	struct linkr_debugger_config_http_json *json, const char *value)
{
	int result;

	if (value == NULL ||
	    linkr_debugger_config_http_json_append(json, "\"") != 0) {
		return json_fail(json);
	}
	for (const unsigned char *cursor = (const unsigned char *)value;
	     *cursor != '\0'; cursor++) {
		switch (*cursor) {
		case '"':
			result = linkr_debugger_config_http_json_append(json, "\\\"");
			break;
		case '\\':
			result = linkr_debugger_config_http_json_append(json, "\\\\");
			break;
		case '\b':
			result = linkr_debugger_config_http_json_append(json, "\\b");
			break;
		case '\f':
			result = linkr_debugger_config_http_json_append(json, "\\f");
			break;
		case '\n':
			result = linkr_debugger_config_http_json_append(json, "\\n");
			break;
		case '\r':
			result = linkr_debugger_config_http_json_append(json, "\\r");
			break;
		case '\t':
			result = linkr_debugger_config_http_json_append(json, "\\t");
			break;
		default:
			result = *cursor < 0x20U ?
				linkr_debugger_config_http_json_append(json, "\\u%04x", *cursor) :
				linkr_debugger_config_http_json_append(json, "%c", *cursor);
			break;
		}
		if (result != 0) {
			return -1;
		}
	}
	return linkr_debugger_config_http_json_append(json, "\"");
}

void linkr_debugger_config_http_json_discard(
	struct linkr_debugger_config_http_json *json)
{
	(void)json_fail(json);
}
