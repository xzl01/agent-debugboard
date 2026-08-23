/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_json_value.h"

#include "linkr_debugger_json_cursor.h"

#include <string.h>

#define LINKR_DEBUGGER_JSON_VALUE_MAX_STRING_CAPACITY 193U

static bool parse_value(struct linkr_debugger_json_cursor *cursor, size_t depth,
			size_t max_depth, size_t string_capacity);

static bool parse_compound(struct linkr_debugger_json_cursor *cursor, size_t depth,
				   size_t max_depth, size_t string_capacity, bool object)
{
	char close = object ? '}' : ']';

	if (depth >= max_depth) {
		return false;
	}
	cursor->offset++;
	linkr_debugger_json_skip_space(cursor);
	if (linkr_debugger_json_take(cursor, close)) {
		return true;
	}
	for (;;) {
		if (object && (!linkr_debugger_json_skip_string(cursor, string_capacity) ||
			       (linkr_debugger_json_skip_space(cursor),
				!linkr_debugger_json_take(cursor, ':')))) {
			return false;
		}
		linkr_debugger_json_skip_space(cursor);
		if (!parse_value(cursor, depth + 1U, max_depth, string_capacity)) {
			return false;
		}
		linkr_debugger_json_skip_space(cursor);
		if (linkr_debugger_json_take(cursor, close)) {
			return true;
		}
		if (!linkr_debugger_json_take(cursor, ',')) {
			return false;
		}
		linkr_debugger_json_skip_space(cursor);
	}
}

static bool parse_number(struct linkr_debugger_json_cursor *cursor)
{
	size_t start = cursor->offset;

	(void)linkr_debugger_json_take(cursor, '-');
	if (linkr_debugger_json_take(cursor, '0')) {
		if (cursor->text[cursor->offset] >= '0' && cursor->text[cursor->offset] <= '9') {
			return false;
		}
	} else {
		if (cursor->text[cursor->offset] < '1' || cursor->text[cursor->offset] > '9') {
			return false;
		}
		while (cursor->text[cursor->offset] >= '0' && cursor->text[cursor->offset] <= '9') {
			cursor->offset++;
		}
	}
	if (linkr_debugger_json_take(cursor, '.')) {
		if (cursor->text[cursor->offset] < '0' || cursor->text[cursor->offset] > '9') {
			return false;
		}
		while (cursor->text[cursor->offset] >= '0' && cursor->text[cursor->offset] <= '9') {
			cursor->offset++;
		}
	}
	if (cursor->text[cursor->offset] == 'e' || cursor->text[cursor->offset] == 'E') {
		cursor->offset++;
		if (cursor->text[cursor->offset] == '+' || cursor->text[cursor->offset] == '-') {
			cursor->offset++;
		}
		if (cursor->text[cursor->offset] < '0' || cursor->text[cursor->offset] > '9') {
			return false;
		}
		while (cursor->text[cursor->offset] >= '0' && cursor->text[cursor->offset] <= '9') {
			cursor->offset++;
		}
	}
	return cursor->offset > start;
}

static bool parse_value(struct linkr_debugger_json_cursor *cursor, size_t depth,
				size_t max_depth, size_t string_capacity)
{
	const char *literal = NULL;
	size_t length = 0U;

	linkr_debugger_json_skip_space(cursor);
	if (cursor->text[cursor->offset] == '"') {
		return linkr_debugger_json_skip_string(cursor, string_capacity);
	}
	if (cursor->text[cursor->offset] == '{') {
		return parse_compound(cursor, depth, max_depth, string_capacity, true);
	}
	if (cursor->text[cursor->offset] == '[') {
		return parse_compound(cursor, depth, max_depth, string_capacity, false);
	}
	if (cursor->text[cursor->offset] == 't') {
		literal = "true";
		length = 4U;
	} else if (cursor->text[cursor->offset] == 'f') {
		literal = "false";
		length = 5U;
	} else if (cursor->text[cursor->offset] == 'n') {
		literal = "null";
		length = 4U;
	} else {
		return parse_number(cursor);
	}
	if (strncmp(cursor->text + cursor->offset, literal, length) != 0) {
		return false;
	}
	cursor->offset += length;
	return true;
}

bool linkr_debugger_json_value_valid(const char *text, size_t max_depth,
				     size_t string_capacity)
{
	struct linkr_debugger_json_cursor cursor = { .text = text };

	if (text == NULL || string_capacity == 0U ||
	    string_capacity > LINKR_DEBUGGER_JSON_VALUE_MAX_STRING_CAPACITY ||
	    !parse_value(&cursor, 0U, max_depth, string_capacity)) {
		return false;
	}
	linkr_debugger_json_skip_space(&cursor);
	return cursor.text[cursor.offset] == '\0';
}
