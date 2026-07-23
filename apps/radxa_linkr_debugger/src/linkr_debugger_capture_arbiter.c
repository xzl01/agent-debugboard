/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_capture_arbiter.h"

#ifndef LINKR_DEBUGGER_CAPTURE_ARBITER_HOST_TEST
#include <zephyr/irq.h>
#endif

static enum linkr_debugger_capture_owner linkr_debugger_capture_arbiter_current_owner =
	LINKR_DEBUGGER_CAPTURE_OWNER_NONE;

static unsigned int linkr_debugger_capture_arbiter_lock(void)
{
#ifdef LINKR_DEBUGGER_CAPTURE_ARBITER_HOST_TEST
	return 0U;
#else
	return irq_lock();
#endif
}

static void linkr_debugger_capture_arbiter_unlock(unsigned int key)
{
#ifndef LINKR_DEBUGGER_CAPTURE_ARBITER_HOST_TEST
	irq_unlock(key);
#else
	(void)key;
#endif
}

void linkr_debugger_capture_arbiter_reset(void)
{
	unsigned int key = linkr_debugger_capture_arbiter_lock();

	linkr_debugger_capture_arbiter_current_owner = LINKR_DEBUGGER_CAPTURE_OWNER_NONE;
	linkr_debugger_capture_arbiter_unlock(key);
}

bool linkr_debugger_capture_arbiter_try_acquire(enum linkr_debugger_capture_owner owner)
{
	bool acquired = false;
	unsigned int key;

	if (owner == LINKR_DEBUGGER_CAPTURE_OWNER_NONE) {
		return false;
	}

	key = linkr_debugger_capture_arbiter_lock();
	if (linkr_debugger_capture_arbiter_current_owner == LINKR_DEBUGGER_CAPTURE_OWNER_NONE) {
		linkr_debugger_capture_arbiter_current_owner = owner;
		acquired = true;
	}
	linkr_debugger_capture_arbiter_unlock(key);
	return acquired;
}

bool linkr_debugger_capture_arbiter_release(enum linkr_debugger_capture_owner owner)
{
	bool released = false;
	unsigned int key;

	if (owner == LINKR_DEBUGGER_CAPTURE_OWNER_NONE) {
		return false;
	}

	key = linkr_debugger_capture_arbiter_lock();
	if (linkr_debugger_capture_arbiter_current_owner == owner) {
		linkr_debugger_capture_arbiter_current_owner = LINKR_DEBUGGER_CAPTURE_OWNER_NONE;
		released = true;
	}
	linkr_debugger_capture_arbiter_unlock(key);
	return released;
}

enum linkr_debugger_capture_owner linkr_debugger_capture_arbiter_owner(void)
{
	enum linkr_debugger_capture_owner owner;
	unsigned int key = linkr_debugger_capture_arbiter_lock();

	owner = linkr_debugger_capture_arbiter_current_owner;
	linkr_debugger_capture_arbiter_unlock(key);
	return owner;
}
