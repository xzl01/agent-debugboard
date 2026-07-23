/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_CAPTURE_ARBITER_H_
#define RADXA_LINKR_DEBUGGER_CAPTURE_ARBITER_H_

#include <stdbool.h>

enum linkr_debugger_capture_owner {
	LINKR_DEBUGGER_CAPTURE_OWNER_NONE = 0,
	LINKR_DEBUGGER_CAPTURE_OWNER_LOGIC_ANALYZER,
	LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR,
};

void linkr_debugger_capture_arbiter_reset(void);
bool linkr_debugger_capture_arbiter_try_acquire(enum linkr_debugger_capture_owner owner);
bool linkr_debugger_capture_arbiter_release(enum linkr_debugger_capture_owner owner);
enum linkr_debugger_capture_owner linkr_debugger_capture_arbiter_owner(void);

#endif /* RADXA_LINKR_DEBUGGER_CAPTURE_ARBITER_H_ */
