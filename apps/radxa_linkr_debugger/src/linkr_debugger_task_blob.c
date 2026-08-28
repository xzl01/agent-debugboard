/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 *
 * Parses the bounded line-based NDJSON task document into task summaries.
 * This module is deliberately independent from the settings-backed task
 * storage so each side can be tested without sharing the storage lock.
 */

#include "linkr_debugger_task_blob.h"

#include "linkr_debugger_json_cursor.h"
#include "linkr_debugger_task_parse.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LINKR_DEBUGGER_TASK_MARKER_VERSION "# linkr-task.v1"
#define LINKR_DEBUGGER_TASK_MARKER_TASK "# task "
#define LINKR_DEBUGGER_TASK_MAX_LINE LINKR_DEBUGGER_TASK_MAX_REQUEST_LINE

struct linkr_debugger_task_scratch {
	char id[LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN + 1U];
	char name[LINKR_DEBUGGER_TASK_MAX_TASK_NAME_LEN + 1U];
	size_t request_count;
};

static bool task_id_valid(const char *id)
{
	size_t len;

	if (id == NULL) {
		return false;
	}
	len = strlen(id);
	if (len == 0U || len > LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN) {
		return false;
	}
	for (const char *p = id; *p != '\0'; p++) {
		if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '#') {
			return false;
		}
	}
	return true;
}

static bool task_id_seen(const char *id, size_t task_count,
			 const struct linkr_debugger_task_status *result)
{
	for (size_t index = 0U; index < task_count; index++) {
		if (strcmp(result->tasks[index].id, id) == 0) {
			return true;
		}
	}
	return false;
}

static void task_summary_update(size_t index,
				const struct linkr_debugger_task_scratch *task,
				struct linkr_debugger_task_status *result)
{
	if (index >= LINKR_DEBUGGER_TASK_MAX_TASKS) {
		return;
	}
	snprintf(result->tasks[index].id, sizeof(result->tasks[index].id),
		 "%s", task->id);
	snprintf(result->tasks[index].name, sizeof(result->tasks[index].name),
		 "%s", task->name);
	result->tasks[index].request_count = task->request_count;
}

/* Commits the open task from the scratch buffer into the summaries and
 * advances the task index.
 */
static bool task_commit_task(size_t *task_index,
			     struct linkr_debugger_task_scratch *task,
			     struct linkr_debugger_task_status *result)
{
	if (*task_index >= LINKR_DEBUGGER_TASK_MAX_TASKS) {
		return false;
	}
	if (task->name[0] == '\0') {
		snprintf(task->name, sizeof(task->name), "%s", task->id);
	}
	task_summary_update(*task_index, task, result);
	(*task_index)++;
	result->task_count = *task_index;
	return true;
}

bool linkr_debugger_task_blob_parse(
	const char *text, size_t len,
	struct linkr_debugger_task_status *result)
{
	const char *line_start;
	const char *end;
	size_t task_index = 0U;
	bool have_version = false;
	bool in_task = false;
	struct linkr_debugger_task_scratch task;

	if (text == NULL || len == 0U || result == NULL ||
	    len > LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE) {
		return false;
	}
	memset(result, 0, sizeof(*result));
	if (!linkr_debugger_json_utf8_valid(text, len)) {
		return false;
	}
	line_start = text;
	end = text + len;

	/* Raw C0 control bytes other than the supported tab/newline/carriage-return
	 * forms cannot be re-encoded by the GET JSON encoder; rejecting them here
	 * keeps every accepted blob exactly retrievable.
	 */
	for (size_t i = 0U; i < len; i++) {
		unsigned char ch = (unsigned char)text[i];

		if (ch < 0x20U && ch != '\t' && ch != '\n' && ch != '\r') {
			return false;
		}
	}

	while (line_start < end) {
		const char *line_end = line_start;
		char line[LINKR_DEBUGGER_TASK_MAX_LINE + 1];
		size_t line_len;
		struct linkr_debugger_task_request_fields parsed_request;

		while (line_end < end && *line_end != '\n') {
			line_end++;
		}
		line_len = (size_t)(line_end - line_start);
		if (line_len > LINKR_DEBUGGER_TASK_MAX_LINE) {
			return false;
		}
		memcpy(line, line_start, line_len);
		while (line_len > 0U && line[line_len - 1U] == '\r') {
			line_len--;
		}
		line[line_len] = '\0';
		line_start = line_end < end ? line_end + 1U : line_end;

		if (line[0] == '\0') {
			continue;
		}
		if (!have_version && strcmp(line, LINKR_DEBUGGER_TASK_MARKER_VERSION) != 0) {
			return false;
		}
		if (line[0] == '#') {
			if (strncmp(line, LINKR_DEBUGGER_TASK_MARKER_TASK,
				    sizeof(LINKR_DEBUGGER_TASK_MARKER_TASK) - 1U) == 0) {
				const char *id = line +
					sizeof(LINKR_DEBUGGER_TASK_MARKER_TASK) - 1U;

				if (!have_version) {
					return false;
				}
				if (in_task && !task_commit_task(&task_index, &task, result)) {
					return false;
				}
				if (!task_id_valid(id) ||
				    task_id_seen(id, task_index, result)) {
					return false;
				}
				memset(&task, 0, sizeof(task));
				memcpy(task.id, id, strlen(id) + 1U);
				in_task = true;
			} else if (strcmp(line, LINKR_DEBUGGER_TASK_MARKER_VERSION) == 0) {
				if (have_version) {
					return false;
				}
				have_version = true;
			}
			continue;
		}
		if (!have_version || !in_task) {
			return false;
		}

		if (!linkr_debugger_task_parse_request(line, &parsed_request)) {
			return false;
		}
		if (task.request_count >= LINKR_DEBUGGER_TASK_MAX_REQUESTS) {
			return false;
		}
		(void)parsed_request;
		task.request_count++;
	}

	if (in_task && !task_commit_task(&task_index, &task, result)) {
		return false;
	}
	return have_version && task_index > 0U;
}
