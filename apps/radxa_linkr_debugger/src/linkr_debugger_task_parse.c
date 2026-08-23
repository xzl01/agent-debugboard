/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_task_parse.h"

#include "linkr_debugger_json_cursor.h"
#include "linkr_debugger_json_value.h"
#include "linkr_debugger_task.h"

#include <stddef.h>
#include <string.h>

static bool body_valid(const char *body)
{
	return linkr_debugger_json_value_valid(body, LINKR_DEBUGGER_TASK_MAX_JSON_DEPTH,
					LINKR_DEBUGGER_TASK_MAX_BODY_LEN + 1U);
}

static bool path_valid(const char *path)
{
	static const char *const prefixes[] = {
		"/api/v1/power/", "/api/v1/gpio/", "/api/v1/switch/"
	};
	const char *identifier = NULL;

	for (size_t index = 0U; index < sizeof(prefixes) / sizeof(prefixes[0]); index++) {
		size_t length = strlen(prefixes[index]);

		if (strncmp(path, prefixes[index], length) == 0) {
			identifier = path + length;
			break;
		}
	}
	if (identifier == NULL || identifier[0] == '\0' || strcmp(identifier, ".") == 0 ||
	    strcmp(identifier, "..") == 0) {
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)identifier; *p != '\0'; p++) {
		if (*p <= 0x20U || *p >= 0x7fU || strchr("/\\%?#", *p) != NULL) {
			return false;
		}
	}
	return true;
}

static bool parse_wait_ms(struct linkr_debugger_json_cursor *cursor, int32_t *wait_ms)
{
	uint32_t value = 0U;
	char ch = cursor->text[cursor->offset];

	if (ch < '0' || ch > '9') {
		return false;
	}
	if (ch == '0' && cursor->text[cursor->offset + 1U] >= '0' &&
	    cursor->text[cursor->offset + 1U] <= '9') {
		return false;
	}
	while ((ch = cursor->text[cursor->offset]) >= '0' && ch <= '9') {
		uint32_t digit = (uint32_t)(ch - '0');

		if (value > (LINKR_DEBUGGER_TASK_MAX_WAIT_MS - digit) / 10U) {
			return false;
		}
		value = value * 10U + digit;
		cursor->offset++;
	}
	*wait_ms = (int32_t)value;
	return true;
}

static unsigned int field_for_key(const char *key)
{
	if (strcmp(key, "method") == 0) {
		return 1U;
	}
	if (strcmp(key, "path") == 0) {
		return 2U;
	}
	if (strcmp(key, "body") == 0) {
		return 4U;
	}
	if (strcmp(key, "wait_ms") == 0) {
		return 8U;
	}
	return 0U;
}

static bool parse_field_value(struct linkr_debugger_json_cursor *cursor, unsigned int field,
			      struct linkr_debugger_task_request_fields *request)
{
	if (field == 1U) {
		return linkr_debugger_json_parse_string(cursor, request->method,
						 sizeof(request->method));
	}
	if (field == 2U) {
		return linkr_debugger_json_parse_string(cursor, request->path,
						 sizeof(request->path));
	}
	if (field == 4U) {
		return linkr_debugger_json_parse_string(cursor, request->body,
						 sizeof(request->body));
	}
	return parse_wait_ms(cursor, &request->wait_ms);
}

bool linkr_debugger_task_parse_request(
	const char *line, struct linkr_debugger_task_request_fields *request)
{
	struct linkr_debugger_json_cursor cursor = { .text = line };
	char key[16];
	unsigned int fields = 0U;

	if (line == NULL || request == NULL) {
		return false;
	}
	memset(request, 0, sizeof(*request));
	linkr_debugger_json_skip_space(&cursor);
	if (!linkr_debugger_json_take(&cursor, '{')) {
		return false;
	}
	linkr_debugger_json_skip_space(&cursor);
	for (;;) {
		unsigned int field;

		if (!linkr_debugger_json_parse_string(&cursor, key, sizeof(key))) {
			return false;
		}
		field = field_for_key(key);
		if (field == 0U || (fields & field) != 0U) {
			return false;
		}
		fields |= field;
		linkr_debugger_json_skip_space(&cursor);
		if (!linkr_debugger_json_take(&cursor, ':')) {
			return false;
		}
		linkr_debugger_json_skip_space(&cursor);
		if (!parse_field_value(&cursor, field, request)) {
			return false;
		}
		linkr_debugger_json_skip_space(&cursor);
		if (linkr_debugger_json_take(&cursor, '}')) {
			break;
		}
		if (!linkr_debugger_json_take(&cursor, ',')) {
			return false;
		}
		linkr_debugger_json_skip_space(&cursor);
	}
	linkr_debugger_json_skip_space(&cursor);
	return cursor.text[cursor.offset] == '\0' && (fields & 7U) == 7U &&
	       strcmp(request->method, "PUT") == 0 && path_valid(request->path) &&
	       body_valid(request->body);
}
