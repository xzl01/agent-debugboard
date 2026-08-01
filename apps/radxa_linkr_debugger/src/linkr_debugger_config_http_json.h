#ifndef RADXA_LINKR_DEBUGGER_CONFIG_HTTP_JSON_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_HTTP_JSON_H_

#include <stdbool.h>
#include <stddef.h>

struct linkr_debugger_config_http_json {
	char *data;
	size_t capacity;
	size_t length;
	bool failed;
};

void linkr_debugger_config_http_json_init(
	struct linkr_debugger_config_http_json *json,
	char *data, size_t capacity);

int linkr_debugger_config_http_json_append(
	struct linkr_debugger_config_http_json *json,
	const char *format, ...);

int linkr_debugger_config_http_json_string(
	struct linkr_debugger_config_http_json *json,
	const char *value);

void linkr_debugger_config_http_json_discard(
	struct linkr_debugger_config_http_json *json);

#endif
