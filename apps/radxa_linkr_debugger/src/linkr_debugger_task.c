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
#include "linkr_debugger_task_blob.h"

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

struct linkr_debugger_task_runtime {
	bool backend_available;
	bool blob_present;
	char blob[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
	size_t blob_len;
	struct linkr_debugger_task_status summaries;
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
	memset(&task_runtime.summaries, 0, sizeof(task_runtime.summaries));
}

enum linkr_debugger_task_result linkr_debugger_task_tasks_store(const char *blob, size_t len)
{
	struct linkr_debugger_task_status parsed;
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
	if (!linkr_debugger_task_blob_parse(blob, len, &parsed)) {
		task_runtime_unlock();
		return LINKR_DEBUGGER_TASK_INVALID_BLOB;
	}

	ret = task_backend_save_one(NULL, LINKR_DEBUGGER_TASK_TASKS_KEY, blob, len);
	if (ret < 0) {
		task_runtime_unlock();
		return LINKR_DEBUGGER_TASK_STORAGE_ERROR;
	}

	memcpy(task_runtime.blob, blob, len);
	task_runtime.blob_len = len;
	task_runtime.blob_present = true;
	task_runtime.summaries = parsed;
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
	struct linkr_debugger_task_status parsed;
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
	if (!linkr_debugger_task_blob_parse(
		    task_runtime.blob, task_runtime.blob_len, &parsed)) {
		task_runtime.blob_present = false;
		task_runtime.blob_len = 0U;
		task_summaries_reset();
	} else {
		task_runtime.summaries = parsed;
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
	*status = task_runtime.summaries;
	status->backend_available = task_runtime.backend_available;
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
