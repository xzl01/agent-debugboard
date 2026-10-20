/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_reboot.h"

#include "linkr_debugger_control.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>

enum linkr_debugger_reboot_state {
	LINKR_DEBUGGER_REBOOT_IDLE = 0,
	LINKR_DEBUGGER_REBOOT_PREPARING,
	LINKR_DEBUGGER_REBOOT_SCHEDULED,
	LINKR_DEBUGGER_REBOOT_EXECUTING,
};

static atomic_t linkr_debugger_reboot_state =
	ATOMIC_INIT(LINKR_DEBUGGER_REBOOT_IDLE);

static int linkr_debugger_reboot_perform(void)
{
	int ret = linkr_debugger_watchdog_prepare_planned_reboot();

	if (ret < 0) {
		atomic_set(&linkr_debugger_reboot_state, LINKR_DEBUGGER_REBOOT_IDLE);
		return ret;
	}

	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

static void linkr_debugger_reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!atomic_cas(&linkr_debugger_reboot_state,
			LINKR_DEBUGGER_REBOOT_SCHEDULED,
			LINKR_DEBUGGER_REBOOT_EXECUTING)) {
		return;
	}

	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

static K_WORK_DELAYABLE_DEFINE(linkr_debugger_reboot_work,
				linkr_debugger_reboot_work_handler);

int linkr_debugger_reboot_now(void)
{
	if (!atomic_cas(&linkr_debugger_reboot_state,
			LINKR_DEBUGGER_REBOOT_IDLE,
			LINKR_DEBUGGER_REBOOT_EXECUTING)) {
		return -EALREADY;
	}

	return linkr_debugger_reboot_perform();
}

int linkr_debugger_reboot_schedule(void)
{
	int ret;

	if (!atomic_cas(&linkr_debugger_reboot_state,
			LINKR_DEBUGGER_REBOOT_IDLE,
			LINKR_DEBUGGER_REBOOT_PREPARING)) {
		return -EALREADY;
	}

	/* Complete watchdog/marker preparation before starting the response grace
	 * period. A slow control lock can therefore never let the work race ahead
	 * of preparation or client acknowledgement.
	 */
	ret = linkr_debugger_watchdog_prepare_planned_reboot();
	if (ret < 0) {
		atomic_set(&linkr_debugger_reboot_state, LINKR_DEBUGGER_REBOOT_IDLE);
		return ret;
	}

	atomic_set(&linkr_debugger_reboot_state, LINKR_DEBUGGER_REBOOT_SCHEDULED);
	ret = k_work_schedule(&linkr_debugger_reboot_work,
			      K_MSEC(LINKR_DEBUGGER_REBOOT_DELAY_MS));
	if (ret < 0) {
		/* Preparation has disabled the recovery watchdog. Failing closed means
		 * rebooting immediately rather than continuing without supervision.
		 */
		atomic_set(&linkr_debugger_reboot_state, LINKR_DEBUGGER_REBOOT_EXECUTING);
		sys_reboot(SYS_REBOOT_COLD);
		CODE_UNREACHABLE;
	}

	return 0;
}
