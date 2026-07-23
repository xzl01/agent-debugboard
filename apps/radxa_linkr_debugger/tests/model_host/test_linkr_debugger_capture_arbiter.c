/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_capture_arbiter.h"

#include <assert.h>

static void test_initial_owner_and_reset(void)
{
	linkr_debugger_capture_arbiter_reset();
	assert(linkr_debugger_capture_arbiter_owner() == LINKR_DEBUGGER_CAPTURE_OWNER_NONE);
	assert(!linkr_debugger_capture_arbiter_release(LINKR_DEBUGGER_CAPTURE_OWNER_LOGIC_ANALYZER));
}

static void test_exclusive_acquire(void)
{
	linkr_debugger_capture_arbiter_reset();
	assert(linkr_debugger_capture_arbiter_try_acquire(
		LINKR_DEBUGGER_CAPTURE_OWNER_LOGIC_ANALYZER));
	assert(linkr_debugger_capture_arbiter_owner() ==
		LINKR_DEBUGGER_CAPTURE_OWNER_LOGIC_ANALYZER);
	assert(!linkr_debugger_capture_arbiter_try_acquire(
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR));
	assert(!linkr_debugger_capture_arbiter_try_acquire(
		LINKR_DEBUGGER_CAPTURE_OWNER_LOGIC_ANALYZER));
}

static void test_release_by_owner_only(void)
{
	linkr_debugger_capture_arbiter_reset();
	assert(linkr_debugger_capture_arbiter_try_acquire(
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR));
	assert(!linkr_debugger_capture_arbiter_release(
		LINKR_DEBUGGER_CAPTURE_OWNER_LOGIC_ANALYZER));
	assert(linkr_debugger_capture_arbiter_owner() ==
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR);
	assert(linkr_debugger_capture_arbiter_release(
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR));
	assert(linkr_debugger_capture_arbiter_owner() == LINKR_DEBUGGER_CAPTURE_OWNER_NONE);
	
	assert(linkr_debugger_capture_arbiter_try_acquire(
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR));
	assert(linkr_debugger_capture_arbiter_release(
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR));
	assert(linkr_debugger_capture_arbiter_try_acquire(
		LINKR_DEBUGGER_CAPTURE_OWNER_LOGIC_ANALYZER));
}

int main(void)
{
	test_initial_owner_and_reset();
	test_exclusive_acquire();
	test_release_by_owner_only();
	return 0;
}
