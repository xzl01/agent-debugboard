/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_task_mutation.h"

#include "linkr_debugger_capture_arbiter.h"
#include "linkr_debugger_flash_arbiter.h"
#include "linkr_debugger_json_cursor.h"
#include "linkr_debugger_task_blob.h"

#include <string.h>

#define TASK_JSON_KEY_MAX 24U
#define TASK_JSON_ACTION_MAX 12U
#define TASK_JSON_REVISION_MAX 24U
#define TASK_JSON_DIGEST_MAX 72U
#define TASK_JSON_SCHEMA_MAX 32U

#define TASK_MUTATION_KEY_SCHEMA 0U
#define TASK_MUTATION_KEY_ACTION 1U
#define TASK_MUTATION_KEY_REVISION 2U
#define TASK_MUTATION_KEY_CATALOG 3U
#define TASK_MUTATION_KEY_ID 4U
#define TASK_MUTATION_KEY_SOURCE 5U
#define TASK_MUTATION_KEY_IR 6U
#define TASK_MUTATION_KEY_COUNT 7U

struct task_json_cursor {
	const char *text;
	size_t len;
	size_t offset;
};

static void task_json_space(struct task_json_cursor *cursor)
{
	while (cursor->offset < cursor->len) {
		char ch = cursor->text[cursor->offset];

		if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
			break;
		}
		cursor->offset++;
	}
}

static bool task_json_take(struct task_json_cursor *cursor, char expected)
{
	task_json_space(cursor);
	if (cursor->offset < cursor->len &&
	    cursor->text[cursor->offset] == expected) {
		cursor->offset++;
		return true;
	}
	return false;
}

static bool task_json_string(struct task_json_cursor *cursor, char *output,
			     size_t capacity, size_t *output_len)
{
	task_json_space(cursor);
	if (cursor->offset >= cursor->len ||
	    cursor->text[cursor->offset] != '"') {
		return false;
	}
	return linkr_debugger_json_parse_string_n(cursor->text, cursor->len,
						  &cursor->offset, output, capacity,
						  output_len);
}

/* Strict canonical base64 (RFC 4648 with padding): standard alphabet,
 * exact '=' padding only at the end, zero pad bits, no whitespace. */
static int task_base64_value(char ch)
{
	if (ch >= 'A' && ch <= 'Z') {
		return ch - 'A';
	}
	if (ch >= 'a' && ch <= 'z') {
		return ch - 'a' + 26;
	}
	if (ch >= '0' && ch <= '9') {
		return ch - '0' + 52;
	}
	if (ch == '+') {
		return 62;
	}
	if (ch == '/') {
		return 63;
	}
	return -1;
}

enum task_base64_result {
	TASK_BASE64_OK = 0,
	TASK_BASE64_MALFORMED,
	TASK_BASE64_TOO_LARGE,
};

/* Decode a raw base64 token in place. Decoded bytes never extend past the
 * bytes already consumed (4 characters yield at most 3 bytes), so the
 * output may alias the input at the token start. Oversize input is
 * rejected before decoding; malformed padding, alphabet and non-zero pad
 * bits are rejected as malformed. */
static enum task_base64_result task_base64_decode_inplace(char *text, size_t len,
							 size_t limit,
							 size_t *decoded_len)
{
	size_t pad = 0U;
	size_t out = 0U;

	if ((len % 4U) != 0U) {
		return TASK_BASE64_MALFORMED;
	}
	if (len > ((limit + 2U) / 3U) * 4U) {
		return TASK_BASE64_TOO_LARGE;
	}
	while (pad < 2U && pad < len && text[len - 1U - pad] == '=') {
		pad++;
	}
	for (size_t i = 0U; i < len - pad; i++) {
		if (task_base64_value(text[i]) < 0) {
			return TASK_BASE64_MALFORMED;
		}
	}
	if (pad == 2U && (task_base64_value(text[len - 3U]) & 0x0f) != 0) {
		return TASK_BASE64_MALFORMED;
	}
	if (pad == 1U && (task_base64_value(text[len - 2U]) & 0x03) != 0) {
		return TASK_BASE64_MALFORMED;
	}
	for (size_t i = 0U; i < len; i += 4U) {
		bool last = (i + 4U == len);
		int a = task_base64_value(text[i]);
		int b = task_base64_value(text[i + 1U]);
		int c = (last && pad == 2U) ? 0 : task_base64_value(text[i + 2U]);
		int d = (last && pad >= 1U) ? 0 : task_base64_value(text[i + 3U]);

		text[out++] = (char)(uint8_t)(((unsigned int)a << 2) |
					      ((unsigned int)b >> 4));
		if (!last || pad < 2U) {
			text[out++] = (char)(uint8_t)(((unsigned int)b << 4) |
						      ((unsigned int)c >> 2));
		}
		if (!last || pad < 1U) {
			text[out++] = (char)(uint8_t)(((unsigned int)c << 6) |
						      (unsigned int)d);
		}
	}
	if (out > limit) {
		return TASK_BASE64_TOO_LARGE;
	}
	*decoded_len = out;
	return TASK_BASE64_OK;
}

/* Parse a canonical base64 JSON string token and decode it in place. The
 * raw token content must already be canonical base64: any JSON escape,
 * quote, whitespace or non-alphabet byte is rejected before decoding. */
static bool task_json_base64_inplace(struct task_json_cursor *cursor,
				     size_t limit, char **output,
				     size_t *output_len,
				     enum task_base64_result *failure)
{
	size_t start;
	size_t end;

	task_json_space(cursor);
	if (cursor->offset >= cursor->len ||
	    cursor->text[cursor->offset] != '"') {
		return false;
	}
	start = cursor->offset + 1U;
	end = start;
	while (end < cursor->len && cursor->text[end] != '"') {
		if (task_base64_value(cursor->text[end]) < 0 &&
		    cursor->text[end] != '=') {
			return false;
		}
		end++;
	}
	if (end >= cursor->len) {
		return false;
	}
	cursor->offset = end + 1U;
	*failure = task_base64_decode_inplace((char *)cursor->text + start,
					     end - start, limit, output_len);
	if (*failure != TASK_BASE64_OK) {
		return false;
	}
	*output = (char *)cursor->text + start;
	return true;
}

static bool task_parse_revision(const char *text, size_t len, uint64_t *value)
{
	uint64_t result = 0U;

	if (len == 0U || len > 20U || (len > 1U && text[0] == '0')) {
		return false;
	}
	for (size_t i = 0U; i < len; i++) {
		uint8_t ch = (uint8_t)text[i];
		uint64_t digit;

		if (ch < '0' || ch > '9') {
			return false;
		}
		digit = (uint64_t)(ch - '0');
		if (result > (UINT64_MAX - digit) / 10U) {
			return false;
		}
		result = result * 10U + digit;
	}
	*value = result;
	return true;
}

static int task_hex_value(char ch)
{
	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	}
	if (ch >= 'a' && ch <= 'f') {
		return ch - 'a' + 10;
	}
	return -1;
}

static bool task_parse_digest(const char *text, size_t len,
			      uint8_t digest[LINKR_DEBUGGER_TASK_SHA256_SIZE])
{
	if (len != 2U * LINKR_DEBUGGER_TASK_SHA256_SIZE) {
		return false;
	}
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_SHA256_SIZE; i++) {
		int high = task_hex_value(text[2U * i]);
		int low = task_hex_value(text[2U * i + 1U]);

		if (high < 0 || low < 0) {
			return false;
		}
		digest[i] = (uint8_t)((high << 4) | low);
	}
	return true;
}

static bool task_mutation_semantics(
	struct linkr_debugger_task_store_mutation *mutation,
	bool has_id, bool has_source, bool has_ir)
{
	bool id_valid = has_id &&
		linkr_debugger_task_id_valid(mutation->id, strlen(mutation->id));

	switch (mutation->opcode) {
	case LINKR_DEBUGGER_TASK_MUTATION_UPSERT:
		if (!has_id || !has_ir || !id_valid || mutation->ir_len == 0U ||
		    (has_source && mutation->source_len == 0U)) {
			return false;
		}
		if (!has_source) {
			mutation->source = NULL;
			mutation->source_len = 0U;
		}
		return true;
	case LINKR_DEBUGGER_TASK_MUTATION_DELETE:
		return has_id && id_valid && !has_source && !has_ir;
	case LINKR_DEBUGGER_TASK_MUTATION_CLEAR:
		return !has_id && !has_source && !has_ir;
	case LINKR_DEBUGGER_TASK_MUTATION_IMPORT:
		return !has_id && !has_source && has_ir && mutation->ir_len > 0U;
	default:
		return false;
	}
}

static enum linkr_debugger_task_result task_mutation_parse(
	uint8_t *body, size_t body_len,
	struct linkr_debugger_task_store_mutation *mutation)
{
	struct task_json_cursor cursor = {
		.text = (const char *)body,
		.len = body_len,
		.offset = 0U,
	};
	bool seen[TASK_MUTATION_KEY_COUNT] = { false };
	bool has_id = false, has_source = false, has_ir = false;
	bool has_schema = false, has_action = false;
	bool has_revision = false, has_catalog = false;
	char schema[TASK_JSON_SCHEMA_MAX];
	char action[TASK_JSON_ACTION_MAX];
	char revision[TASK_JSON_REVISION_MAX];
	char catalog[TASK_JSON_DIGEST_MAX];
	size_t value_len = 0U;

	if (body == NULL || mutation == NULL || body_len == 0U ||
	    body_len > LINKR_DEBUGGER_TASK_MUTATION_MAX_SIZE) {
		return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
	}
	memset(mutation, 0, sizeof(*mutation));
	if (!task_json_take(&cursor, '{')) {
		return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
	}
	if (!task_json_take(&cursor, '}')) {
		for (;;) {
			char key[TASK_JSON_KEY_MAX];
			size_t key_len = 0U;
			size_t index;

			if (!task_json_string(&cursor, key, sizeof(key), &key_len)) {
				return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
			}
			if (strcmp(key, "schema") == 0) {
				index = TASK_MUTATION_KEY_SCHEMA;
			} else if (strcmp(key, "action") == 0) {
				index = TASK_MUTATION_KEY_ACTION;
			} else if (strcmp(key, "expected_revision") == 0) {
				index = TASK_MUTATION_KEY_REVISION;
			} else if (strcmp(key, "catalog_fingerprint") == 0) {
				index = TASK_MUTATION_KEY_CATALOG;
			} else if (strcmp(key, "id") == 0) {
				index = TASK_MUTATION_KEY_ID;
			} else if (strcmp(key, "source_base64") == 0) {
				index = TASK_MUTATION_KEY_SOURCE;
			} else if (strcmp(key, "ir_base64") == 0) {
				index = TASK_MUTATION_KEY_IR;
			} else {
				return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
			}
			if (seen[index] || !task_json_take(&cursor, ':')) {
				return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
			}
			seen[index] = true;
			switch (index) {
			case TASK_MUTATION_KEY_SCHEMA:
				if (!task_json_string(&cursor, schema, sizeof(schema),
						     &value_len)) {
					return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
				}
				has_schema = true;
				break;
			case TASK_MUTATION_KEY_ACTION:
				if (!task_json_string(&cursor, action, sizeof(action),
						     &value_len)) {
					return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
				}
				has_action = true;
				break;
			case TASK_MUTATION_KEY_REVISION:
				if (!task_json_string(&cursor, revision,
						      sizeof(revision), &value_len)) {
					return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
				}
				has_revision = true;
				break;
			case TASK_MUTATION_KEY_CATALOG:
				if (!task_json_string(&cursor, catalog, sizeof(catalog),
						     &value_len)) {
					return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
				}
				has_catalog = true;
				break;
			case TASK_MUTATION_KEY_ID:
				if (!task_json_string(&cursor, mutation->id,
						      sizeof(mutation->id),
						      &value_len)) {
					return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
				}
				has_id = true;
				break;
			case TASK_MUTATION_KEY_SOURCE: {
				char *source;
				enum task_base64_result failure = TASK_BASE64_OK;

				if (!task_json_base64_inplace(
					    &cursor,
					    LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE,
					    &source, &value_len, &failure)) {
					return failure == TASK_BASE64_TOO_LARGE ?
						LINKR_DEBUGGER_TASK_BODY_TOO_LARGE :
						LINKR_DEBUGGER_TASK_INVALID_MUTATION;
				}
				mutation->source = (uint8_t *)source;
				mutation->source_len = value_len;
				has_source = true;
				break;
			}
			case TASK_MUTATION_KEY_IR: {
				char *ir;
				enum task_base64_result failure = TASK_BASE64_OK;

				if (!task_json_base64_inplace(
					    &cursor,
					    LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE,
					    &ir, &value_len, &failure)) {
					return failure == TASK_BASE64_TOO_LARGE ?
						LINKR_DEBUGGER_TASK_BODY_TOO_LARGE :
						LINKR_DEBUGGER_TASK_INVALID_MUTATION;
				}
				mutation->ir = ir;
				mutation->ir_len = value_len;
				has_ir = true;
				break;
			}
			default:
				return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
			}
			if (!task_json_take(&cursor, ',')) {
				if (!task_json_take(&cursor, '}')) {
					return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
				}
				break;
			}
		}
	}
	task_json_space(&cursor);
	if (cursor.offset != cursor.len || !has_schema || !has_action ||
	    !has_revision || !has_catalog) {
		return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
	}
	if (strcmp(schema, "radxa-linkr-debugger.v1") != 0) {
		return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
	}
	if (strcmp(action, "upsert") == 0) {
		mutation->opcode = LINKR_DEBUGGER_TASK_MUTATION_UPSERT;
	} else if (strcmp(action, "delete") == 0) {
		mutation->opcode = LINKR_DEBUGGER_TASK_MUTATION_DELETE;
	} else if (strcmp(action, "clear") == 0) {
		mutation->opcode = LINKR_DEBUGGER_TASK_MUTATION_CLEAR;
	} else if (strcmp(action, "import") == 0) {
		mutation->opcode = LINKR_DEBUGGER_TASK_MUTATION_IMPORT;
	} else {
		return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
	}
	if (!task_parse_revision(revision, strlen(revision),
				 &mutation->expected_revision) ||
	    !task_parse_digest(catalog, strlen(catalog),
			       mutation->expected_catalog) ||
	    !task_mutation_semantics(mutation, has_id, has_source, has_ir)) {
		return LINKR_DEBUGGER_TASK_INVALID_MUTATION;
	}
	return LINKR_DEBUGGER_TASK_OK;
}

static enum linkr_debugger_task_result task_mutation_apply(
	const struct linkr_debugger_task_store_mutation *mutation,
	struct linkr_debugger_task_mutation_outcome *outcome)
{
	enum linkr_debugger_task_result result;

	if (!linkr_debugger_capture_arbiter_try_acquire(
		    LINKR_DEBUGGER_CAPTURE_OWNER_TASK)) {
		return LINKR_DEBUGGER_TASK_BUSY;
	}
	if (!linkr_debugger_flash_arbiter_try_acquire(
		    LINKR_DEBUGGER_FLASH_OWNER_TASK)) {
		(void)linkr_debugger_capture_arbiter_release(
			LINKR_DEBUGGER_CAPTURE_OWNER_TASK);
		return LINKR_DEBUGGER_TASK_BUSY;
	}
	result = linkr_debugger_task_store_apply(mutation, outcome);
	(void)linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_TASK);
	(void)linkr_debugger_capture_arbiter_release(
		LINKR_DEBUGGER_CAPTURE_OWNER_TASK);
	return result;
}

enum linkr_debugger_task_result linkr_debugger_task_mutation_execute(
	uint8_t *body, size_t body_len,
	struct linkr_debugger_task_mutation_outcome *outcome,
	enum linkr_debugger_task_mutation_opcode *opcode)
{
	struct linkr_debugger_task_store_mutation mutation;
	enum linkr_debugger_task_result result;

	result = task_mutation_parse(body, body_len, &mutation);
	if (result != LINKR_DEBUGGER_TASK_OK) {
		return result;
	}
	if (opcode != NULL) {
		*opcode = mutation.opcode;
	}
	return task_mutation_apply(&mutation, outcome);
}

enum linkr_debugger_task_result linkr_debugger_task_mutation_clear(void)
{
	enum linkr_debugger_task_result result;

	if (!linkr_debugger_capture_arbiter_try_acquire(
		    LINKR_DEBUGGER_CAPTURE_OWNER_TASK)) {
		return LINKR_DEBUGGER_TASK_BUSY;
	}
	if (!linkr_debugger_flash_arbiter_try_acquire(
		    LINKR_DEBUGGER_FLASH_OWNER_TASK)) {
		(void)linkr_debugger_capture_arbiter_release(
			LINKR_DEBUGGER_CAPTURE_OWNER_TASK);
		return LINKR_DEBUGGER_TASK_BUSY;
	}

	result = linkr_debugger_task_store_clear();
	(void)linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_TASK);
	(void)linkr_debugger_capture_arbiter_release(
		LINKR_DEBUGGER_CAPTURE_OWNER_TASK);
	return result;
}

const char *linkr_debugger_task_mutation_action(
	enum linkr_debugger_task_mutation_opcode opcode)
{
	switch (opcode) {
	case LINKR_DEBUGGER_TASK_MUTATION_UPSERT: return "upsert";
	case LINKR_DEBUGGER_TASK_MUTATION_DELETE: return "delete";
	case LINKR_DEBUGGER_TASK_MUTATION_CLEAR: return "clear";
	case LINKR_DEBUGGER_TASK_MUTATION_IMPORT: return "import";
	default: return "unknown";
	}
}
