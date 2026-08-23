/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_TASK_SHELL_H_
#define RADXA_LINKR_DEBUGGER_TASK_SHELL_H_

#include <stddef.h>

struct shell;

int linkr_debugger_task_shell_show(const struct shell *sh, size_t argc, char **argv);
int linkr_debugger_task_shell_clear(const struct shell *sh, size_t argc, char **argv);

#endif /* RADXA_LINKR_DEBUGGER_TASK_SHELL_H_ */
