#include "linkr_debugger_config_http_json.h"

#include "linkr_debugger_json_cursor.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

static int config_json_write(void *context, const char *text, size_t len)
{
	return linkr_debugger_config_http_json_append(
		(struct linkr_debugger_config_http_json *)context, "%.*s",
		(int)len, text);
}

int linkr_debugger_config_http_json_string(
	struct linkr_debugger_config_http_json *json, const char *value)
{
	if (value == NULL ||
	    linkr_debugger_json_append_string(json, config_json_write, value,
					      strlen(value)) != 0) {
		return json_fail(json);
	}
	return 0;
}

void linkr_debugger_config_http_json_discard(
	struct linkr_debugger_config_http_json *json)
{
	(void)json_fail(json);
}
