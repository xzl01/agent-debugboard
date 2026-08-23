/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_task_shell.h"

#include "linkr_debugger_task.h"
#include "linkr_debugger_task_mutation.h"

#include <errno.h>
#include <string.h>

#include <zephyr/shell/shell.h>

int linkr_debugger_task_shell_show(const struct shell *sh, size_t argc, char **argv)
{
	struct linkr_debugger_task_status status;
	enum linkr_debugger_task_result result;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	result = linkr_debugger_task_status_get(&status);
	if (result != LINKR_DEBUGGER_TASK_OK) {
		shell_error(sh, "task show error=status_unavailable");
		return -EIO;
	}

	shell_print(sh, "task show available=%s task_count=%u",
		    status.backend_available ? "true" : "false",
		    (unsigned int)status.task_count);
	return 0;
}

int linkr_debugger_task_shell_clear(const struct shell *sh, size_t argc, char **argv)
{
	enum linkr_debugger_task_result result;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	result = linkr_debugger_task_mutation_clear();
	if (result == LINKR_DEBUGGER_TASK_BUSY) {
		shell_error(sh, "task clear error=busy");
		return -EBUSY;
	}
	if (result != LINKR_DEBUGGER_TASK_OK) {
		shell_error(sh, "task clear error=storage_error");
		return -EIO;
	}
	shell_print(sh, "task clear ok");
	return 0;
}
