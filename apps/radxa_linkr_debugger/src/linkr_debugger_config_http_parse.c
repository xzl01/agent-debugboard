#include "linkr_debugger_config_http_parse.h"

#include "linkr_debugger_json_cursor.h"

#include <stdbool.h>
#include <string.h>

#define LINKR_DEBUGGER_CONFIG_HTTP_FIELD_CAP 16U

static bool parse_bool(struct linkr_debugger_json_cursor *cursor, bool *value)
{
	if (linkr_debugger_json_take(cursor, 't')) {
		if (strncmp(&cursor->text[cursor->offset], "rue", 3U) != 0) {
			return false;
		}
		cursor->offset += 3U;
		*value = true;
		return true;
	}
	if (linkr_debugger_json_take(cursor, 'f')) {
		if (strncmp(&cursor->text[cursor->offset], "alse", 4U) != 0) {
			return false;
		}
		cursor->offset += 4U;
		*value = false;
		return true;
	}
	return false;
}

static bool parse_items(struct linkr_debugger_json_cursor *cursor, size_t text_len,
			struct linkr_debugger_config_save_request *request,
			char *storage, size_t storage_cap)
{
	size_t used = 0U;

	if (!linkr_debugger_json_take(cursor, '[')) {
		return false;
	}
	linkr_debugger_json_skip_space(cursor);
	if (linkr_debugger_json_take(cursor, ']')) {
		return true;
	}
	for (;;) {
		size_t output_len = 0U;
		char *item;

		if (request->item_count >= LINKR_DEBUGGER_CONFIG_MAX_ENTRIES ||
		    used >= storage_cap) {
			return false;
		}
		item = storage + used;
		if (!linkr_debugger_json_parse_string_n(
			    cursor->text, text_len, &cursor->offset, item,
			    storage_cap - used, &output_len)) {
			return false;
		}
		used += output_len + 1U;
		request->item_ids[request->item_count++] = item;
		linkr_debugger_json_skip_space(cursor);
		if (linkr_debugger_json_take(cursor, ']')) {
			return true;
		}
		if (!linkr_debugger_json_take(cursor, ',')) {
			return false;
		}
		linkr_debugger_json_skip_space(cursor);
	}
}

static bool parse_object(struct linkr_debugger_json_cursor *cursor, size_t text_len,
			 struct linkr_debugger_config_save_request *request,
			 char *storage, size_t storage_cap)
{
	bool confirmed = false;
	unsigned int fields = 0U;

	if (!linkr_debugger_json_take(cursor, '{')) {
		return false;
	}
	linkr_debugger_json_skip_space(cursor);
	for (;;) {
		char field[LINKR_DEBUGGER_CONFIG_HTTP_FIELD_CAP];
		size_t output_len = 0U;

		if (!linkr_debugger_json_parse_string_n(
			    cursor->text, text_len, &cursor->offset, field,
			    sizeof(field), &output_len)) {
			return false;
		}
		linkr_debugger_json_skip_space(cursor);
		if (!linkr_debugger_json_take(cursor, ':')) {
			return false;
		}
		linkr_debugger_json_skip_space(cursor);
		if (strcmp(field, "items") == 0) {
			if ((fields & 1U) != 0U ||
			    !parse_items(cursor, text_len, request, storage, storage_cap)) {
				return false;
			}
			fields |= 1U;
		} else if (strcmp(field, "confirm") == 0) {
			if ((fields & 2U) != 0U || !parse_bool(cursor, &confirmed)) {
				return false;
			}
			fields |= 2U;
		} else {
			return false;
		}
		linkr_debugger_json_skip_space(cursor);
		if (linkr_debugger_json_take(cursor, '}')) {
			break;
		}
		if (!linkr_debugger_json_take(cursor, ',')) {
			return false;
		}
		linkr_debugger_json_skip_space(cursor);
	}
	linkr_debugger_json_skip_space(cursor);
	request->confirmed = confirmed;
	return cursor->offset == text_len && fields == 3U;
}

static bool parse_body(const uint8_t *data, size_t len,
		       struct linkr_debugger_config_http_save_payload *payload)
{
	struct linkr_debugger_json_cursor cursor;

	if (data == NULL || len == 0U || len > LINKR_DEBUGGER_HTTP_BODY_CAP) {
		return false;
	}
	memcpy(payload->storage, data, len);
	payload->storage[len] = '\0';
	cursor.text = payload->storage;
	cursor.offset = 0U;
	linkr_debugger_json_skip_space(&cursor);
	return parse_object(&cursor, len, &payload->request, payload->storage,
			    sizeof(payload->storage));
}

enum linkr_debugger_config_http_parse_result
linkr_debugger_config_http_parse_save(
	const uint8_t *data, size_t len,
	struct linkr_debugger_config_http_save_payload *payload)
{
	if (payload == NULL) {
		return LINKR_DEBUGGER_CONFIG_HTTP_PARSE_INVALID_JSON;
	}
	memset(payload, 0, sizeof(*payload));
	if (parse_body(data, len, payload)) {
		return LINKR_DEBUGGER_CONFIG_HTTP_PARSE_OK;
	}
	memset(payload, 0, sizeof(*payload));
	return LINKR_DEBUGGER_CONFIG_HTTP_PARSE_INVALID_JSON;
}
