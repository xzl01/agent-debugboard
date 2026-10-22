/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_TASK_MUTATION_H_
#define RADXA_LINKR_DEBUGGER_TASK_MUTATION_H_

#include "linkr_debugger_task_store.h"

/* Stable text/JSON mutation contract. The body is a UTF-8 JSON object with
 * the string fields schema, action, expected_revision, catalog_fingerprint
 * and the action-scoped optional fields id, source_base64 and ir_base64.
 * The base64 fields are strict canonical RFC 4648 strings (standard
 * alphabet, exact zero-pad-bit padding, no JSON escapes or whitespace)
 * carrying the exact source/IR bytes; every other encoding is rejected. */
#define LINKR_DEBUGGER_TASK_MUTATION_CONTENT_TYPE "application/json"

enum linkr_debugger_task_result linkr_debugger_task_mutation_execute(
	uint8_t *body, size_t body_len,
	struct linkr_debugger_task_mutation_outcome *outcome,
	enum linkr_debugger_task_mutation_opcode *opcode);
enum linkr_debugger_task_result linkr_debugger_task_mutation_clear(void);
const char *linkr_debugger_task_mutation_action(
	enum linkr_debugger_task_mutation_opcode opcode);

#endif /* RADXA_LINKR_DEBUGGER_TASK_MUTATION_H_ */
