/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_TASK_MUTATION_H_
#define RADXA_LINKR_DEBUGGER_TASK_MUTATION_H_

#include "linkr_debugger_task.h"

enum linkr_debugger_task_result linkr_debugger_task_mutation_acquire(void);
void linkr_debugger_task_mutation_release(void);
enum linkr_debugger_task_result linkr_debugger_task_mutation_clear(void);

#endif
