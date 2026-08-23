/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_task_mutation.h"

#include "linkr_debugger_capture_arbiter.h"
#include "linkr_debugger_flash_arbiter.h"

enum linkr_debugger_task_result linkr_debugger_task_mutation_acquire(void)
{
	if (!linkr_debugger_capture_arbiter_try_acquire(
	    LINKR_DEBUGGER_CAPTURE_OWNER_TASK)) {
		return LINKR_DEBUGGER_TASK_BUSY;
	}
	if (!linkr_debugger_flash_arbiter_try_acquire(
	    LINKR_DEBUGGER_FLASH_OWNER_TASK)) {
		(void)linkr_debugger_capture_arbiter_release(
			LINKR_DEBUGGER_CAPTURE_OWNER_TASK);
		return LINKR_DEBUGGER_TASK_BUSY;
	}
	return LINKR_DEBUGGER_TASK_OK;
}

void linkr_debugger_task_mutation_release(void)
{
	(void)linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_TASK);
	(void)linkr_debugger_capture_arbiter_release(
		LINKR_DEBUGGER_CAPTURE_OWNER_TASK);
}

enum linkr_debugger_task_result linkr_debugger_task_mutation_clear(void)
{
	enum linkr_debugger_task_result result = linkr_debugger_task_mutation_acquire();

	if (result != LINKR_DEBUGGER_TASK_OK) {
		return result;
	}
	result = linkr_debugger_task_tasks_clear();
	linkr_debugger_task_mutation_release();
	return result;
}
