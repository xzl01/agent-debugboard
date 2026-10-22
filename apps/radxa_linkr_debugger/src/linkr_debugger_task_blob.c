/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 *
 * Canonical linkr-task.v1 validator and byte-span inspector.
 */

#include "linkr_debugger_task_blob.h"

#include "linkr_debugger_json_cursor.h"
#include "linkr_debugger_task_parse.h"

#include <string.h>

#define TASK_MARKER "# task "
#define TASK_MARKER_LEN (sizeof(TASK_MARKER) - 1U)

bool linkr_debugger_task_id_valid(const char *id, size_t len)
{
	if (id == NULL || len == 0U || len > LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN ||
	    !linkr_debugger_json_utf8_valid(id, len)) {
		return false;
	}
	for (size_t i = 0U; i < len; i++) {
		uint8_t ch = (uint8_t)id[i];

		if (ch <= 0x20U || ch == 0x7fU || ch == '#') {
			return false;
		}
	}
	return true;
}

static bool task_id_seen(const struct linkr_debugger_task_blob_layout *layout,
			 const char *id, size_t len)
{
	for (size_t i = 0U; i < layout->task_count; i++) {
		if (strlen(layout->tasks[i].id) == len &&
		    memcmp(layout->tasks[i].id, id, len) == 0) {
			return true;
		}
	}
	return false;
}

static bool task_blob_bytes_canonical(const char *text, size_t len)
{
	if (text == NULL || len < LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN ||
	    len > LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE ||
	    text[len - 1U] != '\n' ||
	    memcmp(text, LINKR_DEBUGGER_TASK_BLOB_HEADER,
		   LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN) != 0 ||
	    !linkr_debugger_json_utf8_valid(text, len)) {
		return false;
	}
	for (size_t i = 0U; i < len; i++) {
		uint8_t ch = (uint8_t)text[i];

		if ((ch < 0x20U && ch != '\n') || ch == 0x7fU) {
			return false;
		}
	}
	return true;
}

bool linkr_debugger_task_blob_inspect(const char *text, size_t len, bool allow_empty,
	struct linkr_debugger_task_blob_layout *layout)
{
	size_t offset = LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN;
	struct linkr_debugger_task_blob_entry *current = NULL;

	if (layout == NULL || !task_blob_bytes_canonical(text, len)) {
		return false;
	}
	memset(layout, 0, sizeof(*layout));
	while (offset < len) {
		size_t line_start = offset;
		size_t line_len;
		char line[LINKR_DEBUGGER_TASK_MAX_REQUEST_LINE + 1U];
		struct linkr_debugger_task_request_fields request;

		while (offset < len && text[offset] != '\n') {
			offset++;
		}
		if (offset >= len) {
			return false;
		}
		line_len = offset - line_start;
		offset++;
		if (line_len > LINKR_DEBUGGER_TASK_MAX_REQUEST_LINE) {
			return false;
		}
		memcpy(line, text + line_start, line_len);
		line[line_len] = '\0';
		if (line_len == 0U) {
			continue;
		}
		if (line[0] == '#') {
			if (strcmp(line, "# linkr-task.v1") == 0) {
				return false;
			}
			if (line_len >= TASK_MARKER_LEN &&
			    memcmp(line, TASK_MARKER, TASK_MARKER_LEN) == 0) {
				const char *id = line + TASK_MARKER_LEN;
				size_t id_len = line_len - TASK_MARKER_LEN;

				if (layout->task_count >= LINKR_DEBUGGER_TASK_MAX_TASKS ||
				    !linkr_debugger_task_id_valid(id, id_len) ||
				    task_id_seen(layout, id, id_len)) {
					return false;
				}
				if (current != NULL) {
					current->section_end = line_start;
				}
				current = &layout->tasks[layout->task_count++];
				memcpy(current->id, id, id_len);
				current->id[id_len] = '\0';
				current->section_start = line_start;
				current->digest_end = offset;
			}
			continue;
		}
		if (current == NULL ||
		    !linkr_debugger_task_parse_request(line, &request) ||
		    current->request_count >= LINKR_DEBUGGER_TASK_MAX_REQUESTS) {
			return false;
		}
		current->request_count++;
		current->digest_end = offset;
	}
	if (current != NULL) {
		current->section_end = len;
	}
	return allow_empty || layout->task_count > 0U;
}

bool linkr_debugger_task_blob_parse(const char *text, size_t len,
	struct linkr_debugger_task_status *result)
{
	struct linkr_debugger_task_blob_layout layout;

	if (result == NULL ||
	    !linkr_debugger_task_blob_inspect(text, len, false, &layout)) {
		return false;
	}
	memset(result, 0, sizeof(*result));
	result->task_count = layout.task_count;
	for (size_t i = 0U; i < layout.task_count; i++) {
		result->tasks[i].slot = (uint8_t)i;
		memcpy(result->tasks[i].id, layout.tasks[i].id,
		       strlen(layout.tasks[i].id) + 1U);
		result->tasks[i].request_count = layout.tasks[i].request_count;
	}
	return true;
}
