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

#endif
