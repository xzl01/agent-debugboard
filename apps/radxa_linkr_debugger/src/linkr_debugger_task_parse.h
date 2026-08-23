/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_TASK_PARSE_H_
#define RADXA_LINKR_DEBUGGER_TASK_PARSE_H_

#include <stdbool.h>
#include <stdint.h>

#define LINKR_DEBUGGER_TASK_MAX_METHOD_LEN 8U
#define LINKR_DEBUGGER_TASK_MAX_PATH_LEN 96U
#define LINKR_DEBUGGER_TASK_MAX_BODY_LEN 192U
#define LINKR_DEBUGGER_TASK_MAX_JSON_DEPTH 16U

struct linkr_debugger_task_request_fields {
	char method[LINKR_DEBUGGER_TASK_MAX_METHOD_LEN + 1U];
	char path[LINKR_DEBUGGER_TASK_MAX_PATH_LEN + 1U];
	char body[LINKR_DEBUGGER_TASK_MAX_BODY_LEN + 1U];
	int32_t wait_ms;
};

bool linkr_debugger_task_parse_request(
	const char *line, struct linkr_debugger_task_request_fields *request);

#endif
