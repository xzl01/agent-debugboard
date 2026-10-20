/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_TASK_BLOB_H_
#define RADXA_LINKR_DEBUGGER_TASK_BLOB_H_

#include "linkr_debugger_task.h"

bool linkr_debugger_task_blob_parse(
	const char *text, size_t len,
	struct linkr_debugger_task_status *result);

enum linkr_debugger_task_blob_edit_result {
	LINKR_DEBUGGER_TASK_BLOB_EDIT_OK = 0,
	LINKR_DEBUGGER_TASK_BLOB_EDIT_NOT_FOUND,
	LINKR_DEBUGGER_TASK_BLOB_EDIT_INVALID,
	LINKR_DEBUGGER_TASK_BLOB_EDIT_TOO_SMALL,
};

/* Copies the blob text with the `# task <task_id>` section (its header line and
 * every following line up to the next task header or end) removed. Request lines
 * of all other tasks are preserved byte-for-byte. `remaining_tasks` receives the
 * number of task sections left; when it reaches 0 the output carries just the
 * version marker and callers must clear the backing key instead of storing a
 * task-free blob. Returns NOT_FOUND when no section matches task_id.
 */
enum linkr_debugger_task_blob_edit_result linkr_debugger_task_blob_delete_task(
	const char *text, size_t len,
	const char *task_id,
	char *out, size_t out_cap, size_t *out_len, size_t *remaining_tasks);

#endif
