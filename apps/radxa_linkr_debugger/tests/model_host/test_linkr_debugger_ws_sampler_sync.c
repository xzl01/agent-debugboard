/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_ws_sampler_sync.h"

#include <assert.h>
#include <stdint.h>

static void test_old_resume_does_not_release_new_pause(void)
{
	struct linkr_debugger_ws_sampler_sync sync = {0};
	uint32_t first = linkr_debugger_ws_sampler_sync_request_pause(&sync);
	uint32_t second;

	assert(!linkr_debugger_ws_sampler_sync_is_resumed(&sync, first));
	linkr_debugger_ws_sampler_sync_resume_current(&sync);
	assert(linkr_debugger_ws_sampler_sync_is_resumed(&sync, first));

	second = linkr_debugger_ws_sampler_sync_request_pause(&sync);
	assert(linkr_debugger_ws_sampler_sync_is_resumed(&sync, first));
	assert(!linkr_debugger_ws_sampler_sync_is_resumed(&sync, second));

	linkr_debugger_ws_sampler_sync_resume_current(&sync);
	assert(linkr_debugger_ws_sampler_sync_is_resumed(&sync, second));
}

static void test_pause_generation_never_uses_zero(void)
{
	struct linkr_debugger_ws_sampler_sync sync = {
		.pause_generation = UINT32_MAX,
	};

	assert(linkr_debugger_ws_sampler_sync_request_pause(&sync) == 1U);
}

int main(void)
{
	test_old_resume_does_not_release_new_pause();
	test_pause_generation_never_uses_zero();
	return 0;
}
