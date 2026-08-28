/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_TASK_H_
#define RADXA_LINKR_DEBUGGER_TASK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINKR_DEBUGGER_TASK_SCHEMA "linkr-task.v1"
#define LINKR_DEBUGGER_TASK_MAX_TASKS 4U
#define LINKR_DEBUGGER_TASK_MAX_REQUESTS 32U
#define LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN 31U
#define LINKR_DEBUGGER_TASK_MAX_TASK_NAME_LEN 63U
#define LINKR_DEBUGGER_TASK_MAX_REQUEST_LINE 256U
#define LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE 4096U
#define LINKR_DEBUGGER_TASK_MAX_WAIT_MS 60000U
#define LINKR_DEBUGGER_TASK_TASKS_KEY "linkr/task/tasks"

struct linkr_debugger_task_request {
	char json[LINKR_DEBUGGER_TASK_MAX_REQUEST_LINE + 1];
	int32_t wait_ms;
};

struct linkr_debugger_task {
	char id[LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN + 1];
	char name[LINKR_DEBUGGER_TASK_MAX_TASK_NAME_LEN + 1];
	size_t request_count;
	struct linkr_debugger_task_request requests[LINKR_DEBUGGER_TASK_MAX_REQUESTS];
};

struct linkr_debugger_task_summary {
	char id[LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN + 1];
	char name[LINKR_DEBUGGER_TASK_MAX_TASK_NAME_LEN + 1];
	size_t request_count;
};

struct linkr_debugger_task_status {
	bool backend_available;
	size_t task_count;
	struct linkr_debugger_task_summary tasks[LINKR_DEBUGGER_TASK_MAX_TASKS];
};

enum linkr_debugger_task_result {
	LINKR_DEBUGGER_TASK_OK = 0,
	LINKR_DEBUGGER_TASK_INVALID_ARGUMENT,
	LINKR_DEBUGGER_TASK_BACKEND_UNAVAILABLE,
	LINKR_DEBUGGER_TASK_INVALID_BLOB,
	LINKR_DEBUGGER_TASK_BUSY,
	LINKR_DEBUGGER_TASK_STORAGE_ERROR,
};

typedef void (*linkr_debugger_task_blob_visitor)(const char *blob, size_t blob_len,
						  void *context);

void linkr_debugger_task_init(void);
enum linkr_debugger_task_result linkr_debugger_task_status_get(
	struct linkr_debugger_task_status *status);
enum linkr_debugger_task_result linkr_debugger_task_blob_snapshot(
	char *blob, size_t blob_cap, size_t *blob_len);
enum linkr_debugger_task_result linkr_debugger_task_blob_visit(
	linkr_debugger_task_blob_visitor visitor, void *context);
enum linkr_debugger_task_result linkr_debugger_task_tasks_store(const char *blob, size_t len);
enum linkr_debugger_task_result linkr_debugger_task_tasks_clear(void);

#ifdef LINKR_DEBUGGER_TASK_HOST_TEST

struct linkr_debugger_task_backend_ops {
	int (*load_one)(void *context, const char *name, void *value, size_t value_size);
	int (*save_one)(void *context, const char *name, const void *value, size_t value_size);
	int (*delete_one)(void *context, const char *name);
};

void linkr_debugger_task_test_set_backend(
	const struct linkr_debugger_task_backend_ops *ops, void *context);

#endif /* LINKR_DEBUGGER_TASK_HOST_TEST */

#endif /* RADXA_LINKR_DEBUGGER_TASK_H_ */
