/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_HTTP_TASK_RESPONSE_H_
#define RADXA_LINKR_DEBUGGER_HTTP_TASK_RESPONSE_H_

#include <stddef.h>

#include "linkr_debugger_task.h"

#define LINKR_DEBUGGER_TASK_HTTP_RESPONSE_CAP \
	((2U * LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE) + \
	 (2U * LINKR_DEBUGGER_TASK_MAX_TASKS * \
	  (LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN + LINKR_DEBUGGER_TASK_MAX_TASK_NAME_LEN)) + 512U)
#define LINKR_DEBUGGER_TASK_HTTP_PATH "/api/v1/tasks"

enum linkr_debugger_task_http_action {
	LINKR_DEBUGGER_TASK_HTTP_ACTION_LIST,
	LINKR_DEBUGGER_TASK_HTTP_ACTION_STORE,
	LINKR_DEBUGGER_TASK_HTTP_ACTION_CLEAR,
	LINKR_DEBUGGER_TASK_HTTP_ACTION_METHOD_NOT_ALLOWED,
};

bool linkr_debugger_http_task_path_is_supported(const char *path);
enum linkr_debugger_task_http_action linkr_debugger_http_task_action_for_method(unsigned int method);
int linkr_debugger_http_task_list_response(char *buf, size_t cap,
	const struct linkr_debugger_task_status *status, const char *blob, size_t blob_len);
int linkr_debugger_http_task_store_response(char *buf, size_t cap);
int linkr_debugger_http_task_clear_response(char *buf, size_t cap);

#endif
