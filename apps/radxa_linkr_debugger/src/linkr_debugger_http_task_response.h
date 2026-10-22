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
#include "linkr_debugger_task_store.h"

#define LINKR_DEBUGGER_TASK_HTTP_RESPONSE_CAP LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE
#define LINKR_DEBUGGER_TASK_HTTP_PATH "/api/v1/tasks"
#define LINKR_DEBUGGER_TASK_IR_HTTP_PATH "/api/v1/tasks/ir"
#define LINKR_DEBUGGER_TASK_MUTATIONS_HTTP_PATH "/api/v1/tasks/mutations"
#define LINKR_DEBUGGER_TASK_SOURCE_0_HTTP_PATH "/api/v1/tasks/slots/0/source"
#define LINKR_DEBUGGER_TASK_SOURCE_1_HTTP_PATH "/api/v1/tasks/slots/1/source"
#define LINKR_DEBUGGER_TASK_SOURCE_2_HTTP_PATH "/api/v1/tasks/slots/2/source"
#define LINKR_DEBUGGER_TASK_SOURCE_3_HTTP_PATH "/api/v1/tasks/slots/3/source"
#define LINKR_DEBUGGER_TASK_IR_CONTENT_TYPE \
	"application/vnd.radxa.linkr-task-ir.v1"
#define LINKR_DEBUGGER_TASK_SOURCE_CONTENT_TYPE \
	"application/vnd.radxa.linkr-test.v1+ndjson"

int linkr_debugger_http_task_list_response(char *buf, size_t cap,
	const struct linkr_debugger_task_status *status);
int linkr_debugger_http_task_mutation_response(char *buf, size_t cap,
	const struct linkr_debugger_task_mutation_outcome *outcome,
	enum linkr_debugger_task_mutation_opcode opcode);
int linkr_debugger_http_task_error_response(char *buf, size_t cap,
	const char *code, const char *message,
	const struct linkr_debugger_task_status *current);

#endif /* RADXA_LINKR_DEBUGGER_HTTP_TASK_RESPONSE_H_ */
