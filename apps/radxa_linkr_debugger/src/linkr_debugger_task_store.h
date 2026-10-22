/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_TASK_STORE_H_
#define RADXA_LINKR_DEBUGGER_TASK_STORE_H_

#include "linkr_debugger_task.h"

enum linkr_debugger_task_mutation_opcode {
	LINKR_DEBUGGER_TASK_MUTATION_UPSERT = 1,
	LINKR_DEBUGGER_TASK_MUTATION_DELETE = 2,
	LINKR_DEBUGGER_TASK_MUTATION_CLEAR = 3,
	LINKR_DEBUGGER_TASK_MUTATION_IMPORT = 4,
};

struct linkr_debugger_task_store_mutation {
	enum linkr_debugger_task_mutation_opcode opcode;
	uint64_t expected_revision;
	uint8_t expected_catalog[LINKR_DEBUGGER_TASK_SHA256_SIZE];
	char id[LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN + 1U];
	/* Exact decoded source/IR bytes; both point into the caller's JSON
	 * mutation body buffer (strict canonical base64, decoded in place). */
	uint8_t *source;
	size_t source_len;
	const char *ir;
	size_t ir_len;
};

struct linkr_debugger_task_mutation_outcome {
	struct linkr_debugger_task_status status;
	bool affected_task_present;
	uint8_t affected_slot;
};

enum linkr_debugger_task_result linkr_debugger_task_store_apply(
	const struct linkr_debugger_task_store_mutation *mutation,
	struct linkr_debugger_task_mutation_outcome *outcome);

#endif /* RADXA_LINKR_DEBUGGER_TASK_STORE_H_ */
