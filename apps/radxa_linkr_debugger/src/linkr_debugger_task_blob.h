/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_TASK_BLOB_H_
#define RADXA_LINKR_DEBUGGER_TASK_BLOB_H_

#include "linkr_debugger_task.h"

#define LINKR_DEBUGGER_TASK_BLOB_HEADER "# linkr-task.v1\n"
#define LINKR_DEBUGGER_TASK_BLOB_HEADER_LEN (sizeof(LINKR_DEBUGGER_TASK_BLOB_HEADER) - 1U)

struct linkr_debugger_task_blob_entry {
	char id[LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN + 1U];
	size_t request_count;
	size_t section_start;
	size_t section_end;
	size_t digest_end;
};

struct linkr_debugger_task_blob_layout {
	size_t task_count;
	struct linkr_debugger_task_blob_entry tasks[LINKR_DEBUGGER_TASK_MAX_TASKS];
};

bool linkr_debugger_task_id_valid(const char *id, size_t len);
bool linkr_debugger_task_blob_inspect(const char *text, size_t len, bool allow_empty,
	struct linkr_debugger_task_blob_layout *layout);
bool linkr_debugger_task_blob_parse(const char *text, size_t len,
	struct linkr_debugger_task_status *result);

#endif /* RADXA_LINKR_DEBUGGER_TASK_BLOB_H_ */
