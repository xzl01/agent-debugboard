/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_control.h"
#include "linkr_debugger_reboot.h"

#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>

/*
 * Exercise the real reboot state machine with the Zephyr scheduling, atomic,
 * watchdog, and reboot dependencies replaced by the reboot_stubs shim.
 */
#include "../../src/linkr_debugger_reboot.c"

_Static_assert(LINKR_DEBUGGER_REBOOT_DELAY_MS == 250U,
	       "scheduled reboot delay changed");

static int prepare_calls;
static int prepare_result;
static int prepare_observed_schedule_calls;
static void (*prepare_hook)(void);
static int schedule_calls;
static int schedule_result;
static k_timeout_t schedule_delay;
static int schedule_observed_prepare_calls;
static int schedule_observed_state;
static struct k_work_delayable *scheduled_work;
static int reboot_calls;
static int reboot_type;
static int bootsel_calls;
static jmp_buf reboot_jmp;

int linkr_debugger_watchdog_prepare_planned_reboot(void)
{
	prepare_calls++;
	prepare_observed_schedule_calls = schedule_calls;

	if (prepare_result == 0 && prepare_hook != NULL) {
		prepare_hook();
	}

	return prepare_result;
}

int k_work_schedule(struct k_work_delayable *work, k_timeout_t delay)
{
	schedule_calls++;
	schedule_delay = delay;
	schedule_observed_prepare_calls = prepare_calls;
	schedule_observed_state = atomic_get(&linkr_debugger_reboot_state);
	scheduled_work = work;
	return schedule_result;
}

void sys_reboot(int type)
{
	reboot_calls++;
	reboot_type = type;
	longjmp(reboot_jmp, 1);
}

int linkr_debugger_bootloader_now(void)
{
	bootsel_calls++;
	return 0;
}

static void harness_reset(void)
{
	atomic_set(&linkr_debugger_reboot_state, LINKR_DEBUGGER_REBOOT_IDLE);

	prepare_calls = 0;
	prepare_result = 0;
	prepare_observed_schedule_calls = -1;
	prepare_hook = NULL;
	schedule_calls = 0;
	schedule_result = 0;
	schedule_delay = -1;
	schedule_observed_prepare_calls = -1;
	schedule_observed_state = -1;
	scheduled_work = NULL;
	reboot_calls = 0;
	reboot_type = -1;
	bootsel_calls = 0;
}

static void assert_no_bootsel(void)
{
	assert(bootsel_calls == 0);
}

static bool run_work_reboots(void)
{
	assert(scheduled_work == &linkr_debugger_reboot_work);

	if (setjmp(reboot_jmp) == 0) {
		linkr_debugger_reboot_work_handler(
			&linkr_debugger_reboot_work.work);
		return false;
	}

	return true;
}

static bool schedule_reboots(void)
{
	if (setjmp(reboot_jmp) == 0) {
		(void)linkr_debugger_reboot_schedule();
		return false;
	}

	return true;
}

static bool reboot_now_reboots(void)
{
	if (setjmp(reboot_jmp) == 0) {
		(void)linkr_debugger_reboot_now();
		return false;
	}

	return true;
}

static void test_schedule_prepares_before_arming_delay(void)
{
	harness_reset();

	assert(linkr_debugger_reboot_schedule() == 0);
	assert(prepare_calls == 1);
	assert(prepare_observed_schedule_calls == 0);
	assert(schedule_calls == 1);
	assert(schedule_observed_prepare_calls == 1);
	assert(schedule_observed_state == LINKR_DEBUGGER_REBOOT_SCHEDULED);
	assert(schedule_delay == (k_timeout_t)LINKR_DEBUGGER_REBOOT_DELAY_MS);
	assert(schedule_delay == 250);
	assert(scheduled_work == &linkr_debugger_reboot_work);
	assert(reboot_calls == 0);
	assert(atomic_get(&linkr_debugger_reboot_state) ==
	       LINKR_DEBUGGER_REBOOT_SCHEDULED);
	assert_no_bootsel();
}

static void test_work_only_cold_reboots(void)
{
	harness_reset();

	assert(linkr_debugger_reboot_schedule() == 0);
	assert(prepare_calls == 1);

	assert(run_work_reboots());
	assert(reboot_calls == 1);
	assert(reboot_type == SYS_REBOOT_COLD);
	assert(prepare_calls == 1);
	assert(atomic_get(&linkr_debugger_reboot_state) ==
	       LINKR_DEBUGGER_REBOOT_EXECUTING);
	assert_no_bootsel();

	/* The delayed handler is a one-shot transition to EXECUTING. */
	linkr_debugger_reboot_work_handler(&linkr_debugger_reboot_work.work);
	assert(reboot_calls == 1);
	assert_no_bootsel();
}

static void work_during_prepare_hook(void)
{
	assert(atomic_get(&linkr_debugger_reboot_state) ==
	       LINKR_DEBUGGER_REBOOT_PREPARING);
	assert(schedule_calls == 0);

	/* A work item that fires while the control lock is held must stay inert. */
	linkr_debugger_reboot_work_handler(&linkr_debugger_reboot_work.work);

	assert(reboot_calls == 0);
	assert(atomic_get(&linkr_debugger_reboot_state) ==
	       LINKR_DEBUGGER_REBOOT_PREPARING);
}

static void test_work_during_preparing_does_not_reboot(void)
{
	harness_reset();
	prepare_hook = work_during_prepare_hook;

	assert(linkr_debugger_reboot_schedule() == 0);
	assert(prepare_calls == 1);
	assert(prepare_observed_schedule_calls == 0);
	assert(schedule_calls == 1);
	assert(schedule_observed_prepare_calls == 1);
	assert(reboot_calls == 0);
	assert(atomic_get(&linkr_debugger_reboot_state) ==
	       LINKR_DEBUGGER_REBOOT_SCHEDULED);
	assert_no_bootsel();

	/* Only after preparation finishes may the armed work reboot. */
	assert(run_work_reboots());
	assert(reboot_calls == 1);
	assert(reboot_type == SYS_REBOOT_COLD);
	assert_no_bootsel();
}

static int nested_schedule_result;
static int nested_prepare_calls;
static int nested_schedule_calls;

static void duplicate_schedule_during_prepare_hook(void)
{
	nested_schedule_result = linkr_debugger_reboot_schedule();
	nested_prepare_calls = prepare_calls;
	nested_schedule_calls = schedule_calls;
}

static void test_duplicate_schedule_during_preparing_is_ealready(void)
{
	harness_reset();
	prepare_hook = duplicate_schedule_during_prepare_hook;

	assert(linkr_debugger_reboot_schedule() == 0);
	assert(nested_schedule_result == -EALREADY);
	assert(nested_prepare_calls == 1);
	assert(nested_schedule_calls == 0);
	assert(prepare_calls == 1);
	assert(schedule_calls == 1);
	assert(reboot_calls == 0);
	assert(atomic_get(&linkr_debugger_reboot_state) ==
	       LINKR_DEBUGGER_REBOOT_SCHEDULED);
	assert_no_bootsel();
}

static void test_duplicate_schedule_while_scheduled_is_ealready(void)
{
	harness_reset();

	assert(linkr_debugger_reboot_schedule() == 0);
	assert(linkr_debugger_reboot_schedule() == -EALREADY);
	assert(prepare_calls == 1);
	assert(schedule_calls == 1);
	assert(reboot_calls == 0);
	assert(atomic_get(&linkr_debugger_reboot_state) ==
	       LINKR_DEBUGGER_REBOOT_SCHEDULED);
	assert_no_bootsel();
}

static void test_prepare_failure_returns_idle_and_allows_retry(void)
{
	harness_reset();

	prepare_result = -EIO;
	assert(linkr_debugger_reboot_schedule() == -EIO);
	assert(schedule_calls == 0);
	assert(prepare_calls == 1);
	assert(reboot_calls == 0);
	assert(atomic_get(&linkr_debugger_reboot_state) ==
	       LINKR_DEBUGGER_REBOOT_IDLE);
	assert_no_bootsel();

	prepare_result = 0;
	assert(linkr_debugger_reboot_schedule() == 0);
	assert(schedule_calls == 1);
	assert(prepare_calls == 2);
	assert(atomic_get(&linkr_debugger_reboot_state) ==
	       LINKR_DEBUGGER_REBOOT_SCHEDULED);
	assert_no_bootsel();
}

static void test_schedule_failure_fails_closed_with_cold_reboot(void)
{
	harness_reset();

	schedule_result = -EBUSY;
	assert(schedule_reboots());
	assert(prepare_calls == 1);
	assert(schedule_calls == 1);
	assert(schedule_observed_prepare_calls == 1);
	assert(schedule_observed_state == LINKR_DEBUGGER_REBOOT_SCHEDULED);
	assert(reboot_calls == 1);
	assert(reboot_type == SYS_REBOOT_COLD);
	assert(atomic_get(&linkr_debugger_reboot_state) ==
	       LINKR_DEBUGGER_REBOOT_EXECUTING);
	assert_no_bootsel();
}

static void test_reboot_now_from_idle_cold_reboots(void)
{
	harness_reset();

	assert(reboot_now_reboots());
	assert(prepare_calls == 1);
	assert(schedule_calls == 0);
	assert(reboot_calls == 1);
	assert(reboot_type == SYS_REBOOT_COLD);
	assert_no_bootsel();
}

static void test_reboot_now_rejects_pending_states(void)
{
	harness_reset();
	assert(linkr_debugger_reboot_schedule() == 0);
	assert(linkr_debugger_reboot_now() == -EALREADY);
	assert(prepare_calls == 1);
	assert(reboot_calls == 0);
	assert(atomic_get(&linkr_debugger_reboot_state) ==
	       LINKR_DEBUGGER_REBOOT_SCHEDULED);
	assert_no_bootsel();

	harness_reset();
	atomic_set(&linkr_debugger_reboot_state,
		   LINKR_DEBUGGER_REBOOT_PREPARING);
	assert(linkr_debugger_reboot_now() == -EALREADY);
	assert(prepare_calls == 0);
	assert(reboot_calls == 0);
	assert_no_bootsel();

	harness_reset();
	atomic_set(&linkr_debugger_reboot_state,
		   LINKR_DEBUGGER_REBOOT_EXECUTING);
	assert(linkr_debugger_reboot_now() == -EALREADY);
	assert(prepare_calls == 0);
	assert(reboot_calls == 0);
	assert_no_bootsel();
}

static void test_reboot_now_prepare_failure_returns_to_idle(void)
{
	harness_reset();

	prepare_result = -EIO;
	assert(linkr_debugger_reboot_now() == -EIO);
	assert(prepare_calls == 1);
	assert(reboot_calls == 0);
	assert(atomic_get(&linkr_debugger_reboot_state) ==
	       LINKR_DEBUGGER_REBOOT_IDLE);
	assert_no_bootsel();

	prepare_result = 0;
	assert(reboot_now_reboots());
	assert(prepare_calls == 2);
	assert(reboot_type == SYS_REBOOT_COLD);
	assert_no_bootsel();
}

int main(void)
{
	test_schedule_prepares_before_arming_delay();
	test_work_only_cold_reboots();
	test_work_during_preparing_does_not_reboot();
	test_duplicate_schedule_during_preparing_is_ealready();
	test_duplicate_schedule_while_scheduled_is_ealready();
	test_prepare_failure_returns_idle_and_allows_retry();
	test_schedule_failure_fails_closed_with_cold_reboot();
	test_reboot_now_from_idle_cold_reboots();
	test_reboot_now_rejects_pending_states();
	test_reboot_now_prepare_failure_returns_to_idle();

	puts("linkr_debugger_reboot tests passed");
	return 0;
}
