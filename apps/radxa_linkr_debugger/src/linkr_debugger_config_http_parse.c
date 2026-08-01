#include "linkr_debugger_config_http_parse.h"

#include <string.h>

struct json_cursor {
	char *data;
	size_t length;
	size_t offset;
};

static void skip_space(struct json_cursor *cursor)
{
	while (cursor->offset < cursor->length &&
	       cursor->data[cursor->offset] != '\0' &&
	       strchr(" \t\n\r", cursor->data[cursor->offset]) != NULL)
		cursor->offset++;
}

static bool take(struct json_cursor *cursor, char expected)
{
	bool matched = cursor->offset < cursor->length &&
		       cursor->data[cursor->offset] == expected;

	if (matched)
		cursor->offset++;
	return matched;
}

static bool unicode_control(uint32_t codepoint)
{
	return codepoint < 0x20U || (codepoint >= 0x7fU && codepoint <= 0x9fU);
}

static int hex_value(char value)
{
	return value >= '0' && value <= '9' ? value - '0' :
	       value >= 'a' && value <= 'f' ? value - 'a' + 10 :
	       value >= 'A' && value <= 'F' ? value - 'A' + 10 : -1;
}

static bool parse_hex4(struct json_cursor *cursor, uint32_t *value)
{
	uint32_t decoded = 0U;

	if (cursor->length - cursor->offset < 4U) return false;
	for (size_t i = 0U; i < 4U; i++) {
		int digit = hex_value(cursor->data[cursor->offset++]);

		if (digit < 0) return false;
		decoded = (decoded << 4) | (uint32_t)digit;
	}
	*value = decoded;
	return true;
}

static bool parse_codepoint(struct json_cursor *cursor, uint32_t *codepoint)
{
	uint32_t first;

	if (!parse_hex4(cursor, &first)) return false;
	if (first >= 0xd800U && first <= 0xdbffU) {
		uint32_t second;

		if (!take(cursor, '\\') || !take(cursor, 'u') ||
		    !parse_hex4(cursor, &second) || second < 0xdc00U ||
		    second > 0xdfffU)
			return false;
		first = 0x10000U + ((first - 0xd800U) << 10) + second - 0xdc00U;
	} else if (first >= 0xdc00U && first <= 0xdfffU) {
		return false;
	}
	if (unicode_control(first)) return false;
	*codepoint = first;
	return true;
}

static void write_codepoint(char *data, size_t *offset, uint32_t codepoint)
{
	size_t width = codepoint <= 0x7fU ? 1U : codepoint <= 0x7ffU ? 2U :
		       codepoint <= 0xffffU ? 3U : 4U;
	uint8_t prefix = width == 2U ? 0xc0U : width == 3U ? 0xe0U : 0xf0U;

	if (width == 1U) {
		data[(*offset)++] = (char)codepoint;
		return;
	}
	for (size_t i = width - 1U; i > 0U; i--) {
		data[*offset + i] = (char)(0x80U | (codepoint & 0x3fU));
		codepoint >>= 6;
	}
	data[*offset] = (char)(prefix | codepoint);
	*offset += width;
}

static bool raw_utf8_width(const struct json_cursor *cursor, size_t *width)
{
	uint8_t first = (uint8_t)cursor->data[cursor->offset];
	size_t count = first >= 0xc2U && first <= 0xdfU ? 2U :
		       first >= 0xe0U && first <= 0xefU ? 3U :
		       first >= 0xf0U && first <= 0xf4U ? 4U : 0U;

	if (count == 0U || count > cursor->length - cursor->offset) return false;
	for (size_t i = 1U; i < count; i++) {
		uint8_t next = (uint8_t)cursor->data[cursor->offset + i];

		if ((next & 0xc0U) != 0x80U) return false;
	}
	uint8_t second = (uint8_t)cursor->data[cursor->offset + 1U];
	if ((first == 0xc2U && second <= 0x9fU) ||
	    (first == 0xe0U && second < 0xa0U) ||
	    (first == 0xedU && second >= 0xa0U) ||
	    (first == 0xf0U && second < 0x90U) ||
	    (first == 0xf4U && second > 0x8fU))
		return false;
	*width = count;
	return true;
}

static bool parse_string(struct json_cursor *cursor, char **value)
{
	size_t write;

	if (!take(cursor, '"')) return false;
	*value = &cursor->data[cursor->offset];
	write = cursor->offset;
	while (cursor->offset < cursor->length) {
		uint8_t current = (uint8_t)cursor->data[cursor->offset];

		if (current == '"') {
			cursor->offset++;
			cursor->data[write] = '\0';
			return true;
		}
		if (current == '\\') {
			uint32_t codepoint;
			char escape;

			cursor->offset++;
			if (cursor->offset >= cursor->length) return false;
			escape = cursor->data[cursor->offset++];
			if (escape == '"' || escape == '\\' || escape == '/')
				cursor->data[write++] = escape;
			else if (escape != 'u' || !parse_codepoint(cursor, &codepoint))
				return false;
			else
				write_codepoint(cursor->data, &write, codepoint);
		} else if (current < 0x80U) {
			if (unicode_control(current))
				return false;
			cursor->data[write++] = cursor->data[cursor->offset++];
		} else {
			size_t width;

			if (!raw_utf8_width(cursor, &width))
				return false;
			memmove(&cursor->data[write], &cursor->data[cursor->offset], width);
			write += width;
			cursor->offset += width;
		}
	}
	return false;
}

static bool parse_bool(struct json_cursor *cursor, bool *value)
{
	size_t available = cursor->length - cursor->offset;

	if (available >= 4U &&
	    memcmp(&cursor->data[cursor->offset], "true", 4U) == 0) {
		cursor->offset += 4U;
		*value = true;
	} else if (available >= 5U &&
		   memcmp(&cursor->data[cursor->offset], "false", 5U) == 0) {
		cursor->offset += 5U;
		*value = false;
	} else {
		return false;
	}
	return true;
}

static bool parse_items(struct json_cursor *cursor,
			struct linkr_debugger_config_save_request *request)
{
	if (!take(cursor, '[')) return false;
	skip_space(cursor);
	if (take(cursor, ']')) return true;
	for (;;) {
		char *item_id;

		if (request->item_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES ||
		    !parse_string(cursor, &item_id))
			return false;
		request->item_ids[request->item_count++] = item_id;
		skip_space(cursor);
		if (take(cursor, ']'))
			return true;
		if (!take(cursor, ',')) return false;
		skip_space(cursor);
	}
}

static bool parse_object(struct json_cursor *cursor,
			 struct linkr_debugger_config_save_request *request, bool apply,
			 bool *confirmed_out)
{
	bool confirmed = false;
	unsigned int fields = 0U;

	if (!take(cursor, '{')) return false;
	skip_space(cursor);
	for (;;) {
		char *name;

		if (cursor->offset >= cursor->length ||
		    cursor->data[cursor->offset] == '}' || !parse_string(cursor, &name))
			return false;
		skip_space(cursor);
		if (!take(cursor, ':')) return false;
		skip_space(cursor);
		if (strcmp(name, "items") == 0) {
			if (apply || (fields & 1U) != 0U || !parse_items(cursor, request))
				return false;
			fields |= 1U;
		} else if (strcmp(name, "confirm") == 0) {
			if ((fields & 2U) != 0U || !parse_bool(cursor, &confirmed))
				return false;
			fields |= 2U;
		} else {
			return false;
		}
		skip_space(cursor);
		if (take(cursor, '}'))
			break;
		if (!take(cursor, ',')) return false;
		skip_space(cursor);
	}
	skip_space(cursor);
	if (cursor->offset != cursor->length)
		return false;
	if (apply) {
		if (confirmed_out == NULL)
			return false;
		*confirmed_out = confirmed;
		return fields == 2U;
	}
	request->confirmed = confirmed;
	return fields == 3U;
}

static bool parse_body(const uint8_t *data, size_t len, char *storage,
		       struct linkr_debugger_config_save_request *request, bool apply,
		       bool *confirmed_out)
{
	struct json_cursor cursor;

	if (data == NULL || len == 0U || len > LINKR_DEBUGGER_HTTP_BODY_CAP)
		return false;
	if (apply && confirmed_out == NULL)
		return false;
	memcpy(storage, data, len);
	storage[len] = '\0';
	cursor = (struct json_cursor){storage, len, 0U};
	skip_space(&cursor);
	return parse_object(&cursor, request, apply, confirmed_out);
}

enum linkr_debugger_config_http_parse_result
linkr_debugger_config_http_parse_save(
	const uint8_t *data, size_t len,
	struct linkr_debugger_config_http_save_payload *payload)
{
	if (payload == NULL)
		return LINKR_DEBUGGER_CONFIG_HTTP_PARSE_INVALID_JSON;
	memset(payload, 0, sizeof(*payload));
	if (parse_body(data, len, payload->storage, &payload->request, false, NULL))
		return LINKR_DEBUGGER_CONFIG_HTTP_PARSE_OK;
	memset(payload, 0, sizeof(*payload));
	return LINKR_DEBUGGER_CONFIG_HTTP_PARSE_INVALID_JSON;
}

enum linkr_debugger_config_http_parse_result
linkr_debugger_config_http_parse_apply(const uint8_t *data, size_t len,
				       bool *confirmed)
{
	char storage[LINKR_DEBUGGER_HTTP_BODY_CAP + 1U];

	if (confirmed == NULL)
		return LINKR_DEBUGGER_CONFIG_HTTP_PARSE_INVALID_JSON;
	if (!parse_body(data, len, storage, NULL, true, confirmed))
		return LINKR_DEBUGGER_CONFIG_HTTP_PARSE_INVALID_JSON;
	return LINKR_DEBUGGER_CONFIG_HTTP_PARSE_OK;
}
