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
#include <sys/types.h>

#define LINKR_DEBUGGER_TASK_SCHEMA "linkr-task.v1"
#define LINKR_DEBUGGER_TASK_MAX_TASKS 4U
#define LINKR_DEBUGGER_TASK_MAX_REQUESTS 32U
#define LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN 63U
#define LINKR_DEBUGGER_TASK_MAX_TASK_NAME_LEN 63U
#define LINKR_DEBUGGER_TASK_MAX_REQUEST_LINE 256U
#define LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE 4000U
#define LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE 4000U
#define LINKR_DEBUGGER_TASK_MAX_WAIT_MS 60000U
#define LINKR_DEBUGGER_TASK_SHA256_SIZE 32U
#define LINKR_DEBUGGER_TASK_STORE_ID_SIZE 16U
#define LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE (LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE + 1U)
/* JSON mutation bodies carry source/IR as strict canonical base64 strings:
 * each worst 4000-byte payload encodes to 5336 characters, so both fields
 * plus the fixed envelope keys fit one bounded 12288-byte arena. */
#define LINKR_DEBUGGER_TASK_MUTATION_MAX_SIZE 12288U
#define LINKR_DEBUGGER_TASK_STORAGE_RESERVE_BYTES 8192U

struct linkr_debugger_task_request {
	char json[LINKR_DEBUGGER_TASK_MAX_REQUEST_LINE + 1U];
	int32_t wait_ms;
};

struct linkr_debugger_task {
	char id[LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN + 1U];
	char name[LINKR_DEBUGGER_TASK_MAX_TASK_NAME_LEN + 1U];
	size_t request_count;
	struct linkr_debugger_task_request requests[LINKR_DEBUGGER_TASK_MAX_REQUESTS];
};

struct linkr_debugger_task_summary {
	uint8_t slot;
	char id[LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN + 1U];
	size_t request_count;
	bool source_present;
	uint16_t source_len;
	uint8_t source_digest[LINKR_DEBUGGER_TASK_SHA256_SIZE];
	uint16_t ir_len;
	uint8_t ir_digest[LINKR_DEBUGGER_TASK_SHA256_SIZE];
};

struct linkr_debugger_task_storage {
	uint32_t total_bytes;
	uint32_t used_bytes;
	uint32_t free_bytes;
	uint32_t task_bytes;
	uint32_t reserve_bytes;
	bool measured;
	bool save_allowed;
};

struct linkr_debugger_task_status {
	bool backend_available;
	uint8_t task_store_id[LINKR_DEBUGGER_TASK_STORE_ID_SIZE];
	uint64_t revision;
	uint8_t catalog_fingerprint[LINKR_DEBUGGER_TASK_SHA256_SIZE];
	uint16_t full_ir_len;
	uint8_t full_ir_digest[LINKR_DEBUGGER_TASK_SHA256_SIZE];
	size_t task_count;
	struct linkr_debugger_task_storage storage;
	struct linkr_debugger_task_summary tasks[LINKR_DEBUGGER_TASK_MAX_TASKS];
};

enum linkr_debugger_task_result {
	LINKR_DEBUGGER_TASK_OK = 0,
	LINKR_DEBUGGER_TASK_INVALID_ARGUMENT,
	LINKR_DEBUGGER_TASK_BACKEND_UNAVAILABLE,
	LINKR_DEBUGGER_TASK_INVALID_MUTATION,
	LINKR_DEBUGGER_TASK_INVALID_TASK,
	LINKR_DEBUGGER_TASK_BUSY,
	LINKR_DEBUGGER_TASK_REVISION_CONFLICT,
	LINKR_DEBUGGER_TASK_CATALOG_CONFLICT,
	LINKR_DEBUGGER_TASK_STORAGE_FULL,
	LINKR_DEBUGGER_TASK_READBACK_FAILED,
	LINKR_DEBUGGER_TASK_STORAGE_ERROR,
	LINKR_DEBUGGER_TASK_SOURCE_ABSENT,
	LINKR_DEBUGGER_TASK_BODY_TOO_LARGE,
};

void linkr_debugger_task_init(void);
enum linkr_debugger_task_result linkr_debugger_task_status_get(
	struct linkr_debugger_task_status *status);
/* Low-stack shell clear path: commits an empty manifest without staging an
 * outcome/status copy on the 2 KiB shell stack.
 */
enum linkr_debugger_task_result linkr_debugger_task_store_clear(void);
enum linkr_debugger_task_result linkr_debugger_task_ir_snapshot(
	uint8_t *ir, size_t ir_cap, size_t *ir_len);
enum linkr_debugger_task_result linkr_debugger_task_source_snapshot(
	uint8_t slot, uint8_t *source, size_t source_cap, size_t *source_len);

#ifdef LINKR_DEBUGGER_TASK_HOST_TEST

/* Settings-style key/value backend seam. load_one returns the stored value
 * length (0 means absent), save_one is atomic per key, delete_one removes a
 * key. All three return negative errno values on failure. */
struct linkr_debugger_task_backend_ops {
	int (*subsys_init)(void *context);
	int (*load_one)(void *context, const char *name, void *value, size_t value_size);
	int (*save_one)(void *context, const char *name, const void *value,
			size_t value_size);
	int (*delete_one)(void *context, const char *name);
	void (*random)(void *context, void *value, size_t value_size);
};

void linkr_debugger_task_test_set_backend(
	const struct linkr_debugger_task_backend_ops *ops, void *context);
void linkr_debugger_task_test_force_revision(uint64_t revision);
/* -1 restores the default host-model free-space calculation. */
void linkr_debugger_task_test_set_storage_free(ssize_t free_bytes);

#endif /* LINKR_DEBUGGER_TASK_HOST_TEST */

#endif /* RADXA_LINKR_DEBUGGER_TASK_H_ */
