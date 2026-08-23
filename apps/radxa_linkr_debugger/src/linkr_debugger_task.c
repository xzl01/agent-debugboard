/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 *
 * Firmware-owned task request store.
 *
 * A task blob is a bounded line-based NDJSON document:
 *
 *   # linkr-task.v1
 *   # task <id>
 *   {"method":"PUT","path":"/api/v1/...","body":"{...}","wait_ms":0}
 *   ...
 *
 * Each request is the same method/path/body tuple used by the public HTTP API.
 * wait_ms is validated task metadata; clients apply it after dispatching a
 * request through the public HTTP API.
 */

#include "linkr_debugger_task.h"
#include "linkr_debugger_json_cursor.h"
#include "linkr_debugger_task_parse.h"

#ifndef LINKR_DEBUGGER_TASK_HOST_TEST
#include <zephyr/kernel.h>
#else
#include <pthread.h>
#endif

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef LINKR_DEBUGGER_TASK_HOST_TEST
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(linkr_debugger_task, LOG_LEVEL_INF);

#endif

#define LINKR_DEBUGGER_TASK_MARKER_VERSION "# linkr-task.v1"
#define LINKR_DEBUGGER_TASK_MARKER_TASK "# task "

#define LINKR_DEBUGGER_TASK_MAX_LINE LINKR_DEBUGGER_TASK_MAX_REQUEST_LINE

struct linkr_debugger_task_runtime {
	bool backend_available;
	bool blob_present;
	char blob[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
	size_t blob_len;
	size_t task_count;
	struct {
		char id[LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN + 1];
		char name[LINKR_DEBUGGER_TASK_MAX_TASK_NAME_LEN + 1];
		size_t request_count;
	} tasks[LINKR_DEBUGGER_TASK_MAX_TASKS];
};

static struct linkr_debugger_task_runtime task_runtime;

#ifdef LINKR_DEBUGGER_TASK_HOST_TEST

static pthread_mutex_t task_runtime_lock = PTHREAD_MUTEX_INITIALIZER;

#define task_runtime_lock() pthread_mutex_lock(&task_runtime_lock)
#define task_runtime_unlock() pthread_mutex_unlock(&task_runtime_lock)

#else

K_MUTEX_DEFINE(task_runtime_lock);

#define task_runtime_lock() k_mutex_lock(&task_runtime_lock, K_FOREVER)
#define task_runtime_unlock() k_mutex_unlock(&task_runtime_lock)

#endif

struct linkr_debugger_task_scratch {
	char id[LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN + 1U];
	char name[LINKR_DEBUGGER_TASK_MAX_TASK_NAME_LEN + 1U];
	size_t request_count;
};

static struct linkr_debugger_task_scratch task_scratch_task;

/* FIXME(review-20260821): Parsing and settings-backed task storage remain in
 * one module. Consequence: changes to either must preserve this lock contract.
 * Remove when independently tested parser and storage modules share one runtime API.
 */

#ifndef LINKR_DEBUGGER_TASK_HOST_TEST

static int task_backend_load_one(void *context, const char *name, void *value,
				 size_t value_size)
{
	(void)context;
	return (int)settings_load_one(name, value, value_size);
}

static int task_backend_save_one(void *context, const char *name, const void *value,
				 size_t value_size)
{
	(void)context;
	return settings_save_one(name, value, value_size);
}

static int task_backend_delete_one(void *context, const char *name)
{
	(void)context;
	return settings_delete(name);
}

#else /* LINKR_DEBUGGER_TASK_HOST_TEST */

static int (*test_load_one)(void *context, const char *name, void *value,
			    size_t value_size);
static int (*test_save_one)(void *context, const char *name, const void *value,
			    size_t value_size);
static int (*test_delete_one)(void *context, const char *name);
static void *test_backend_context;

void linkr_debugger_task_test_set_backend(
	const struct linkr_debugger_task_backend_ops *ops, void *context)
{
	test_backend_context = context;
	if (ops == NULL) {
		test_load_one = NULL;
		test_save_one = NULL;
		test_delete_one = NULL;
		return;
	}
	test_load_one = ops->load_one;
	test_save_one = ops->save_one;
	test_delete_one = ops->delete_one;
}

static int task_backend_load_one(void *context, const char *name, void *value,
				 size_t value_size)
{
	(void)context;
	return test_load_one != NULL ? test_load_one(test_backend_context, name,
						     value, value_size) : -ENODEV;
}

static int task_backend_save_one(void *context, const char *name, const void *value,
				 size_t value_size)
{
	(void)context;
	return test_save_one != NULL ? test_save_one(test_backend_context, name,
						     value, value_size) : -ENODEV;
}

static int task_backend_delete_one(void *context, const char *name)
{
	(void)context;
	return test_delete_one != NULL ? test_delete_one(test_backend_context, name)
				       : -ENODEV;
}

#endif /* LINKR_DEBUGGER_TASK_HOST_TEST */

static void task_runtime_reset(void)
{
	memset(&task_runtime, 0, sizeof(task_runtime));
}

static void task_summaries_reset(void)
{
	for (size_t i = 0; i < LINKR_DEBUGGER_TASK_MAX_TASKS; i++) {
		task_runtime.tasks[i].id[0] = '\0';
		task_runtime.tasks[i].name[0] = '\0';
		task_runtime.tasks[i].request_count = 0U;
	}
	task_runtime.task_count = 0U;
}

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

static bool task_id_seen(const char *id, size_t task_count)
{
	for (size_t index = 0U; index < task_count; index++) {
		if (strcmp(task_runtime.tasks[index].id, id) == 0) {
			return true;
		}
	}
	return false;
}

static void task_summary_update(size_t index,
				const struct linkr_debugger_task_scratch *task)
{
	if (index >= LINKR_DEBUGGER_TASK_MAX_TASKS) {
		return;
	}
	snprintf(task_runtime.tasks[index].id, sizeof(task_runtime.tasks[index].id),
		 "%s", task->id);
	snprintf(task_runtime.tasks[index].name, sizeof(task_runtime.tasks[index].name),
		 "%s", task->name);
	task_runtime.tasks[index].request_count = task->request_count;
}

/* Commits the open task from the scratch buffer into the summaries and
 * advances the task index.
 */
static bool task_commit_task(size_t *task_index)
{
	if (*task_index >= LINKR_DEBUGGER_TASK_MAX_TASKS) {
		return false;
	}
	if (task_scratch_task.name[0] == '\0') {
		snprintf(task_scratch_task.name, sizeof(task_scratch_task.name),
			 "%s", task_scratch_task.id);
	}
	task_summary_update(*task_index, &task_scratch_task);
	(*task_index)++;
	task_runtime.task_count = *task_index;
	return true;
}

/* Parses the stored blob text and rebuilds the RAM task summaries. Returns
 * OK when every task is valid; on error the summaries are left untouched.
 */
static enum linkr_debugger_task_result task_parse_blob(const char *text, size_t len)
{
	const char *line_start;
	const char *end;
	size_t task_index = 0U;
	bool have_version = false;
	bool in_task = false;

	if (text == NULL || len == 0U || len > LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE) {
		return LINKR_DEBUGGER_TASK_INVALID_BLOB;
	}
	if (!linkr_debugger_json_utf8_valid(text, len)) {
		return LINKR_DEBUGGER_TASK_INVALID_BLOB;
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
			return LINKR_DEBUGGER_TASK_INVALID_BLOB;
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
			return LINKR_DEBUGGER_TASK_INVALID_BLOB;
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
			return LINKR_DEBUGGER_TASK_INVALID_BLOB;
		}
		if (line[0] == '#') {
			if (strncmp(line, LINKR_DEBUGGER_TASK_MARKER_TASK,
				    sizeof(LINKR_DEBUGGER_TASK_MARKER_TASK) - 1U) == 0) {
				const char *id = line +
					sizeof(LINKR_DEBUGGER_TASK_MARKER_TASK) - 1U;

				if (!have_version) {
					return LINKR_DEBUGGER_TASK_INVALID_BLOB;
				}
				if (in_task && !task_commit_task(&task_index)) {
					return LINKR_DEBUGGER_TASK_INVALID_BLOB;
				}
				if (!task_id_valid(id)) {
					return LINKR_DEBUGGER_TASK_INVALID_BLOB;
				}
				if (task_id_seen(id, task_index)) {
					return LINKR_DEBUGGER_TASK_INVALID_BLOB;
				}
				memset(&task_scratch_task, 0,
				       sizeof(task_scratch_task));
				memcpy(task_scratch_task.id, id, strlen(id) + 1U);
				in_task = true;
			} else if (strcmp(line, LINKR_DEBUGGER_TASK_MARKER_VERSION) == 0) {
				if (have_version) {
					return LINKR_DEBUGGER_TASK_INVALID_BLOB;
				}
				have_version = true;
			}
			continue;
		}
		if (!have_version) {
			return LINKR_DEBUGGER_TASK_INVALID_BLOB;
		}
		if (!in_task) {
			return LINKR_DEBUGGER_TASK_INVALID_BLOB;
		}

		if (!linkr_debugger_task_parse_request(line, &parsed_request)) {
			return LINKR_DEBUGGER_TASK_INVALID_BLOB;
		}

		if (task_scratch_task.request_count >= LINKR_DEBUGGER_TASK_MAX_REQUESTS) {
			return LINKR_DEBUGGER_TASK_INVALID_BLOB;
		}
		(void)parsed_request;
		task_scratch_task.request_count++;
	}

	if (in_task && !task_commit_task(&task_index)) {
		return LINKR_DEBUGGER_TASK_INVALID_BLOB;
	}
	if (!have_version || task_index == 0U) {
		return LINKR_DEBUGGER_TASK_INVALID_BLOB;
	}
	return LINKR_DEBUGGER_TASK_OK;
}

enum linkr_debugger_task_result linkr_debugger_task_tasks_store(const char *blob, size_t len)
{
	enum linkr_debugger_task_result result;
	int ret;

	if (blob == NULL) {
		return LINKR_DEBUGGER_TASK_INVALID_ARGUMENT;
	}
	if (len == 0U || len > LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE) {
		return LINKR_DEBUGGER_TASK_INVALID_BLOB;
	}

	task_runtime_lock();
	if (!task_runtime.backend_available) {
		task_runtime_unlock();
		return LINKR_DEBUGGER_TASK_BACKEND_UNAVAILABLE;
	}
	result = task_parse_blob(blob, len);
	if (result != LINKR_DEBUGGER_TASK_OK) {
		task_summaries_reset();
		if (task_runtime.blob_present) {
			(void)task_parse_blob(task_runtime.blob, task_runtime.blob_len);
		}
		task_runtime_unlock();
		return result;
	}

	ret = task_backend_save_one(NULL, LINKR_DEBUGGER_TASK_TASKS_KEY, blob, len);
	if (ret < 0) {
		task_summaries_reset();
		if (task_runtime.blob_present) {
			(void)task_parse_blob(task_runtime.blob, task_runtime.blob_len);
		}
		task_runtime_unlock();
		return LINKR_DEBUGGER_TASK_STORAGE_ERROR;
	}

	memcpy(task_runtime.blob, blob, len);
	task_runtime.blob_len = len;
	task_runtime.blob_present = true;
	task_summaries_reset();
	(void)task_parse_blob(task_runtime.blob, task_runtime.blob_len);
	task_runtime_unlock();
	return LINKR_DEBUGGER_TASK_OK;
}

enum linkr_debugger_task_result linkr_debugger_task_tasks_clear(void)
{
	int ret;

	task_runtime_lock();
	if (!task_runtime.backend_available) {
		task_runtime_unlock();
		return LINKR_DEBUGGER_TASK_BACKEND_UNAVAILABLE;
	}
	ret = task_backend_delete_one(NULL, LINKR_DEBUGGER_TASK_TASKS_KEY);
	if (ret < 0) {
		task_runtime_unlock();
		return LINKR_DEBUGGER_TASK_STORAGE_ERROR;
	}
	task_runtime.blob_present = false;
	task_runtime.blob_len = 0U;
	task_summaries_reset();
	task_runtime_unlock();
	return LINKR_DEBUGGER_TASK_OK;
}

void linkr_debugger_task_init(void)
{
	int ret;

	task_runtime_lock();
	task_runtime_reset();
	ret = task_backend_load_one(NULL, LINKR_DEBUGGER_TASK_TASKS_KEY,
			    task_runtime.blob, sizeof(task_runtime.blob));
	if (ret < 0) {
		task_runtime.backend_available = ret == -ENOENT;
		task_runtime_unlock();
		return;
	}
	task_runtime.backend_available = true;
	task_runtime.blob_present = true;
	task_runtime.blob_len = (size_t)ret;
	if (task_parse_blob(task_runtime.blob, task_runtime.blob_len) !=
	    LINKR_DEBUGGER_TASK_OK) {
		task_runtime.blob_present = false;
		task_runtime.blob_len = 0U;
		task_summaries_reset();
	}
	task_runtime_unlock();
}

enum linkr_debugger_task_result linkr_debugger_task_status_get(
	struct linkr_debugger_task_status *status)
{
	if (status == NULL) {
		return LINKR_DEBUGGER_TASK_INVALID_ARGUMENT;
	}
	task_runtime_lock();
	memset(status, 0, sizeof(*status));
	status->backend_available = task_runtime.backend_available;
	status->task_count = task_runtime.task_count;
	for (size_t i = 0U; i < task_runtime.task_count; i++) {
		snprintf(status->tasks[i].id, sizeof(status->tasks[i].id), "%s",
			 task_runtime.tasks[i].id);
		snprintf(status->tasks[i].name, sizeof(status->tasks[i].name), "%s",
			 task_runtime.tasks[i].name);
		status->tasks[i].request_count = task_runtime.tasks[i].request_count;
	}
	task_runtime_unlock();
	return LINKR_DEBUGGER_TASK_OK;
}

enum linkr_debugger_task_result linkr_debugger_task_blob_snapshot(
	char *blob, size_t blob_cap, size_t *blob_len)
{
	if (blob == NULL || blob_len == NULL) {
		return LINKR_DEBUGGER_TASK_INVALID_ARGUMENT;
	}
	task_runtime_lock();
	if (blob_cap < task_runtime.blob_len) {
		task_runtime_unlock();
		return LINKR_DEBUGGER_TASK_INVALID_ARGUMENT;
	}
	if (task_runtime.blob_len > 0U) {
		memcpy(blob, task_runtime.blob, task_runtime.blob_len);
	}
	*blob_len = task_runtime.blob_len;
	task_runtime_unlock();
	return LINKR_DEBUGGER_TASK_OK;
}

enum linkr_debugger_task_result linkr_debugger_task_blob_visit(
	linkr_debugger_task_blob_visitor visitor, void *context)
{
	if (visitor == NULL) {
		return LINKR_DEBUGGER_TASK_INVALID_ARGUMENT;
	}
	task_runtime_lock();
	visitor(task_runtime.blob, task_runtime.blob_len, context);
	task_runtime_unlock();
	return LINKR_DEBUGGER_TASK_OK;
}
