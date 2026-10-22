/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_TASK_SHA256_H_
#define RADXA_LINKR_DEBUGGER_TASK_SHA256_H_

#include <stddef.h>
#include <stdint.h>

#define LINKR_DEBUGGER_TASK_SHA256_DIGEST_SIZE 32U

struct linkr_debugger_task_sha256 {
	uint32_t state[8];
	uint64_t bit_count;
	uint8_t block[64];
	size_t block_len;
};

void linkr_debugger_task_sha256_init(struct linkr_debugger_task_sha256 *context);
void linkr_debugger_task_sha256_update(struct linkr_debugger_task_sha256 *context,
	const void *data, size_t len);
void linkr_debugger_task_sha256_final(struct linkr_debugger_task_sha256 *context,
	uint8_t digest[LINKR_DEBUGGER_TASK_SHA256_DIGEST_SIZE]);
void linkr_debugger_task_sha256_digest(const void *data, size_t len,
	uint8_t digest[LINKR_DEBUGGER_TASK_SHA256_DIGEST_SIZE]);

#endif /* RADXA_LINKR_DEBUGGER_TASK_SHA256_H_ */
