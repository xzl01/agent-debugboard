/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_WS_SAMPLER_SYNC_H_
#define RADXA_LINKR_DEBUGGER_WS_SAMPLER_SYNC_H_

#include <stdbool.h>
#include <stdint.h>

struct linkr_debugger_ws_sampler_sync {
	uint32_t pause_generation;
	uint32_t resumed_generation;
};

static inline uint32_t linkr_debugger_ws_sampler_sync_request_pause(
	struct linkr_debugger_ws_sampler_sync *sync)
{
	sync->pause_generation++;
	if (sync->pause_generation == 0U) {
		sync->pause_generation++;
	}
	return sync->pause_generation;
}

static inline void linkr_debugger_ws_sampler_sync_resume_current(
	struct linkr_debugger_ws_sampler_sync *sync)
{
	sync->resumed_generation = sync->pause_generation;
}

static inline bool linkr_debugger_ws_sampler_sync_is_resumed(
	const struct linkr_debugger_ws_sampler_sync *sync,
	uint32_t pause_generation)
{
	return pause_generation != 0U &&
		sync->resumed_generation == pause_generation;
}

#endif
