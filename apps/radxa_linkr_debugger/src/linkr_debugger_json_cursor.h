/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_JSON_CURSOR_H_
#define RADXA_LINKR_DEBUGGER_JSON_CURSOR_H_

#include <stdbool.h>
#include <stddef.h>

struct linkr_debugger_json_cursor {
	const char *text;
	size_t offset;
};

bool linkr_debugger_json_utf8_valid(const char *text, size_t text_len);
void linkr_debugger_json_skip_space(struct linkr_debugger_json_cursor *cursor);
bool linkr_debugger_json_take(struct linkr_debugger_json_cursor *cursor, char expected);
bool linkr_debugger_json_parse_string(struct linkr_debugger_json_cursor *cursor,
					      char *output, size_t capacity);
bool linkr_debugger_json_parse_string_n(const char *text, size_t text_len,
					      size_t *offset, char *output,
					      size_t capacity, size_t *output_len);
bool linkr_debugger_json_skip_string(struct linkr_debugger_json_cursor *cursor,
					     size_t capacity);
bool linkr_debugger_json_decode_complete_string(const char *text, size_t text_len,
						 char *output, size_t output_cap,
						 size_t *output_len);

typedef int (*linkr_debugger_json_write_fn)(void *context, const char *text,
					    size_t len);
int linkr_debugger_json_append_string(void *context, linkr_debugger_json_write_fn write,
				      const char *value, size_t len);

#endif
