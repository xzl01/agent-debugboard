/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_json_cursor.h"

#include <stdint.h>
#include <string.h>

void linkr_debugger_json_skip_space(struct linkr_debugger_json_cursor *cursor)
{
	while (cursor->text[cursor->offset] == ' ' || cursor->text[cursor->offset] == '\t' ||
	       cursor->text[cursor->offset] == '\r' || cursor->text[cursor->offset] == '\n') {
		cursor->offset++;
	}
}

bool linkr_debugger_json_take(struct linkr_debugger_json_cursor *cursor, char expected)
{
	if (cursor->text[cursor->offset] != expected) {
		return false;
	}
	cursor->offset++;
	return true;
}

bool linkr_debugger_json_utf8_valid(const char *text, size_t text_len)
{
	size_t offset = 0U;

	if (text == NULL) {
		return false;
	}
	while (offset < text_len) {
		unsigned char first = (unsigned char)text[offset++];
		size_t remaining;
		uint32_t value;
		uint32_t minimum;

		if (first == 0U) {
			return false;
		}
		if (first <= 0x7fU) {
			continue;
		}
		if (first >= 0xc2U && first <= 0xdfU) {
			remaining = 1U;
			value = first & 0x1fU;
			minimum = 0x80U;
		} else if (first >= 0xe0U && first <= 0xefU) {
			remaining = 2U;
			value = first & 0x0fU;
			minimum = 0x800U;
		} else if (first >= 0xf0U && first <= 0xf4U) {
			remaining = 3U;
			value = first & 0x07U;
			minimum = 0x10000U;
		} else {
			return false;
		}
		if (remaining > text_len - offset) {
			return false;
		}
		for (size_t index = 0U; index < remaining; index++) {
			unsigned char continuation = (unsigned char)text[offset++];

			if ((continuation & 0xc0U) != 0x80U) {
				return false;
			}
			value = (value << 6) | (continuation & 0x3fU);
		}
		if (value < minimum || (value >= 0xd800U && value <= 0xdfffU) ||
		    value > 0x10ffffU) {
			return false;
		}
	}
	return true;
}

static int hex_value(char ch)
{
	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	}
	if (ch >= 'a' && ch <= 'f') {
		return ch - 'a' + 10;
	}
	if (ch >= 'A' && ch <= 'F') {
		return ch - 'A' + 10;
	}
	return -1;
}

struct linkr_debugger_json_string_cursor {
	const char *text;
	size_t offset;
	size_t end;
};

static bool json_string_take(struct linkr_debugger_json_string_cursor *cursor, char expected)
{
	if (cursor->offset >= cursor->end || cursor->text[cursor->offset] != expected) {
		return false;
	}
	cursor->offset++;
	return true;
}

static bool parse_hex_quad(struct linkr_debugger_json_string_cursor *cursor, uint32_t *value)
{
	uint32_t result = 0U;

	for (size_t index = 0U; index < 4U; index++) {
		char ch;
		int digit;

		if (cursor->offset >= cursor->end) {
			return false;
		}
		ch = cursor->text[cursor->offset];
		digit = hex_value(ch);
		if (digit < 0) {
			return false;
		}
		cursor->offset++;
		result = (result << 4) | (uint32_t)digit;
	}
	*value = result;
	return true;
}

static bool append_utf8(char *output, size_t capacity, size_t *length, uint32_t value)
{
	size_t count = value <= 0x7fU ? 1U : value <= 0x7ffU ? 2U : value <= 0xffffU ? 3U : 4U;

	if (value == 0U || value > 0x10ffffU || *length + count >= capacity) {
		return false;
	}
	if (count == 1U) {
		if (output != NULL) {
			output[*length] = (char)value;
		}
		(*length)++;
	} else {
		if (output != NULL) {
			for (size_t index = count - 1U; index > 0U; index--) {
				output[*length + index] = (char)(0x80U | (value & 0x3fU));
				value >>= 6;
			}
			output[*length] = (char)((0xf0U << (4U - count)) | value);
		}
		*length += count;
	}
	return true;
}

static bool copy_utf8(struct linkr_debugger_json_string_cursor *cursor, unsigned char first,
			      char *output, size_t capacity, size_t *length)
{
	size_t remaining;
	uint32_t value;
	uint32_t minimum;

	if (first >= 0xc2U && first <= 0xdfU) {
		remaining = 1U;
		value = first & 0x1fU;
		minimum = 0x80U;
	} else if (first >= 0xe0U && first <= 0xefU) {
		remaining = 2U;
		value = first & 0x0fU;
		minimum = 0x800U;
	} else if (first >= 0xf0U && first <= 0xf4U) {
		remaining = 3U;
		value = first & 0x07U;
		minimum = 0x10000U;
	} else {
		return false;
	}
	if (remaining > cursor->end - cursor->offset) {
		return false;
	}
	for (size_t index = 0U; index < remaining; index++) {
		unsigned char continuation = (unsigned char)cursor->text[cursor->offset++];

		if ((continuation & 0xc0U) != 0x80U) {
			return false;
		}
		value = (value << 6) | (continuation & 0x3fU);
	}
	if (value < minimum || (value >= 0xd800U && value <= 0xdfffU) ||
	    value > 0x10ffffU || *length + remaining + 1U >= capacity) {
		return false;
	}
	if (output != NULL) {
		output[*length] = (char)first;
		memmove(output + *length + 1U, cursor->text + cursor->offset - remaining,
			remaining);
	}
	*length += remaining;
	(*length)++;
	return true;
}

static bool json_parse_string(const char *text, size_t text_len, size_t *offset,
			      char *output, size_t capacity, size_t *output_len)
{
	static const char decoded[] = "\b\f\n\r\t";
	struct linkr_debugger_json_string_cursor cursor = {
		.text = text,
		.offset = *offset,
		.end = text_len,
	};
	size_t length = 0U;

	if (text == NULL || capacity == 0U || output_len == NULL ||
	    !json_string_take(&cursor, '"')) {
		return false;
	}
	while (cursor.offset < cursor.end) {
		unsigned char ch = (unsigned char)cursor.text[cursor.offset++];

		if (ch == '"') {
			if (output != NULL) {
				output[length] = '\0';
			}
			*offset = cursor.offset;
			*output_len = length;
			return true;
		}
		if (ch < 0x20U) {
			return false;
		}
		if (ch != '\\') {
			if (ch >= 0x80U) {
				if (!copy_utf8(&cursor, ch, output, capacity, &length)) {
					return false;
				}
				continue;
			}
			if (length + 1U >= capacity) {
				return false;
			}
			if (output != NULL) {
				output[length] = (char)ch;
			}
			length++;
			continue;
		}
		if (cursor.offset >= cursor.end) {
			return false;
		}
		ch = (unsigned char)cursor.text[cursor.offset++];
		if (strchr("\"\\/", ch) != NULL) {
			if (length + 1U >= capacity) {
				return false;
			}
			if (output != NULL) {
				output[length] = (char)ch;
			}
			length++;
		} else if (strchr("bfnrt", ch) != NULL) {
			const char *escape = strchr("bfnrt", ch);

			if (length + 1U >= capacity) {
				return false;
			}
			if (output != NULL) {
				output[length] = decoded[escape - "bfnrt"];
			}
			length++;
		} else if (ch == 'u') {
			uint32_t value;

			if (!parse_hex_quad(&cursor, &value)) {
				return false;
			}
			if (value >= 0xd800U && value <= 0xdbffU) {
				uint32_t low;

				if (!json_string_take(&cursor, '\\') ||
				    !json_string_take(&cursor, 'u') ||
				    !parse_hex_quad(&cursor, &low) || low < 0xdc00U || low > 0xdfffU) {
					return false;
				}
				value = 0x10000U + ((value - 0xd800U) << 10) + (low - 0xdc00U);
			} else if (value >= 0xdc00U && value <= 0xdfffU) {
				return false;
			}
			if (!append_utf8(output, capacity, &length, value)) {
				return false;
			}
		} else {
			return false;
		}
	}
	return false;
}

bool linkr_debugger_json_parse_string_n(const char *text, size_t text_len,
					      size_t *offset, char *output,
					      size_t capacity, size_t *output_len)
{
	if (text == NULL || offset == NULL || output == NULL) {
		return false;
	}
	return json_parse_string(text, text_len, offset, output, capacity, output_len);
}

bool linkr_debugger_json_parse_string(struct linkr_debugger_json_cursor *cursor,
						      char *output, size_t capacity)
{
	size_t output_len;

	if (cursor == NULL || cursor->text == NULL || output == NULL) {
		return false;
	}
	return linkr_debugger_json_parse_string_n(cursor->text, strlen(cursor->text),
						  &cursor->offset, output, capacity,
						  &output_len);
}

bool linkr_debugger_json_skip_string(struct linkr_debugger_json_cursor *cursor,
					     size_t capacity)
{
	size_t output_len;

	if (cursor == NULL || cursor->text == NULL) {
		return false;
	}
	return json_parse_string(cursor->text, strlen(cursor->text), &cursor->offset,
				 NULL, capacity, &output_len);
}

bool linkr_debugger_json_decode_complete_string(const char *text, size_t text_len,
						 char *output, size_t output_cap,
						 size_t *output_len)
{
	size_t offset = 0U;

	if (output == NULL || !json_parse_string(text, text_len, &offset, output, output_cap,
					      output_len)) {
		return false;
	}
	return offset == text_len;
}

int linkr_debugger_json_append_string(void *context, linkr_debugger_json_write_fn write,
				      const char *value, size_t len)
{
	static const char hex[] = "0123456789abcdef";
	int result;

	if (write == NULL) {
		return -1;
	}
	if (write(context, "\"", 1U) != 0) {
		return -1;
	}
	for (size_t index = 0U; index < len; index++) {
		unsigned char ch = (unsigned char)value[index];

		switch (ch) {
		case '"':
			result = write(context, "\\\"", 2U);
			break;
		case '\\':
			result = write(context, "\\\\", 2U);
			break;
		case '\b':
			result = write(context, "\\b", 2U);
			break;
		case '\f':
			result = write(context, "\\f", 2U);
			break;
		case '\n':
			result = write(context, "\\n", 2U);
			break;
		case '\r':
			result = write(context, "\\r", 2U);
			break;
		case '\t':
			result = write(context, "\\t", 2U);
			break;
		default:
			if (ch < 0x20U) {
				char escaped[4] = {
					'\\', 'u', hex[ch >> 4], hex[ch & 0x0fU],
				};

				result = write(context, escaped, sizeof(escaped));
			} else {
				result = write(context, (const char *)&ch, 1U);
			}
			break;
		}
		if (result != 0) {
			return -1;
		}
	}
	return write(context, "\"", 1U);
}
