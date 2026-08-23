/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "stubs/zephyr/shell/shell.h"

#include "../../src/linkr_debugger_capture_arbiter.h"
#include "../../src/linkr_debugger_flash_arbiter.h"
#include "../../src/linkr_debugger_task.h"
#include "../../src/linkr_debugger_task_shell.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SHELL_OUTPUT_CAP 8U
#define SHELL_OUTPUT_LINE_CAP 160U

enum fake_shell_line_kind {
	FAKE_SHELL_LINE_PRINT,
	FAKE_SHELL_LINE_ERROR,
};

static struct {
	enum fake_shell_line_kind kinds[SHELL_OUTPUT_CAP];
	char lines[SHELL_OUTPUT_CAP][SHELL_OUTPUT_LINE_CAP];
	size_t line_count;
} shell_output;

static void shell_capture(enum fake_shell_line_kind kind, const char *format, va_list args)
{
	int written;

	assert(shell_output.line_count < SHELL_OUTPUT_CAP);
	shell_output.kinds[shell_output.line_count] = kind;
	written = vsnprintf(shell_output.lines[shell_output.line_count],
			    SHELL_OUTPUT_LINE_CAP, format, args);
	assert(written >= 0 && (size_t)written < SHELL_OUTPUT_LINE_CAP);
	shell_output.line_count++;
}

void shell_print(const struct shell *sh, const char *format, ...)
{
	va_list args;

	(void)sh;
	va_start(args, format);
	shell_capture(FAKE_SHELL_LINE_PRINT, format, args);
	va_end(args);
}

void shell_error(const struct shell *sh, const char *format, ...)
{
	va_list args;

	(void)sh;
	va_start(args, format);
	shell_capture(FAKE_SHELL_LINE_ERROR, format, args);
	va_end(args);
}

/* In-memory task storage backend. */
static char store_buf[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
static size_t store_len;
static bool store_present;

static int fake_load_one(void *context, const char *name, void *value, size_t value_size)
{
	(void)context;
	assert(name != NULL);
	if (!store_present) {
		return -ENOENT;
	}
	assert(value_size >= store_len);
	memcpy(value, store_buf, store_len);
	return (int)store_len;
}

static int fake_save_one(void *context, const char *name, const void *value, size_t value_size)
{
	(void)context;
	assert(name != NULL);
	if (value_size > sizeof(store_buf)) {
		return -EIO;
	}
	memcpy(store_buf, value, value_size);
	store_len = value_size;
	store_present = true;
	return 0;
}

static int fake_delete_one(void *context, const char *name)
{
	(void)context;
	assert(name != NULL);
	store_present = false;
	store_len = 0U;
	return 0;
}

static const struct linkr_debugger_task_backend_ops fake_backend = {
	.load_one = fake_load_one,
	.save_one = fake_save_one,
	.delete_one = fake_delete_one,
};

static const struct shell test_shell;

static void reset_all(void)
{
	shell_output.line_count = 0U;
	store_len = 0U;
	store_present = false;
	linkr_debugger_capture_arbiter_reset();
	linkr_debugger_flash_arbiter_reset();
	linkr_debugger_task_test_set_backend(&fake_backend, NULL);
	linkr_debugger_task_init();
}

static void store_valid_blob(void)
{
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task guard\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":\"{\\\"state\\\":\\\"off\\\"}\"}\n";

	assert(linkr_debugger_task_tasks_store(blob, sizeof(blob) - 1U) ==
	       LINKR_DEBUGGER_TASK_OK);
	assert(store_present);
}

static void assert_blob_still_stored(void)
{
	struct linkr_debugger_task_status status;

	assert(store_present);
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	assert(status.task_count == 1U);
}

static void assert_single_shell_line(enum fake_shell_line_kind kind, const char *expected)
{
	assert(shell_output.line_count == 1U);
	assert(shell_output.kinds[0] == kind);
	assert(strcmp(shell_output.lines[0], expected) == 0);
}

static void test_clear_succeeds_when_owners_free(void)
{
	int ret;

	reset_all();
	store_valid_blob();
	ret = linkr_debugger_task_shell_clear(&test_shell, 0U, NULL);
	assert(ret == 0);
	assert_single_shell_line(FAKE_SHELL_LINE_PRINT, "task clear ok");
	assert(!store_present);
	assert(linkr_debugger_capture_arbiter_owner() == LINKR_DEBUGGER_CAPTURE_OWNER_NONE);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_NONE);
}

static void test_clear_rejected_while_capture_owner_held(void)
{
	int ret;

	reset_all();
	store_valid_blob();
	assert(linkr_debugger_capture_arbiter_try_acquire(
		LINKR_DEBUGGER_CAPTURE_OWNER_LOGIC_ANALYZER));
	ret = linkr_debugger_task_shell_clear(&test_shell, 0U, NULL);
	assert(ret == -EBUSY);
	assert_single_shell_line(FAKE_SHELL_LINE_ERROR, "task clear error=busy");
	assert_blob_still_stored();
	assert(linkr_debugger_capture_arbiter_owner() ==
	       LINKR_DEBUGGER_CAPTURE_OWNER_LOGIC_ANALYZER);
	assert(linkr_debugger_capture_arbiter_release(
		LINKR_DEBUGGER_CAPTURE_OWNER_LOGIC_ANALYZER));
}

static void test_clear_rejected_while_flash_owner_held(void)
{
	int ret;

	reset_all();
	store_valid_blob();
	assert(linkr_debugger_flash_arbiter_try_acquire(LINKR_DEBUGGER_FLASH_OWNER_OTA));
	ret = linkr_debugger_task_shell_clear(&test_shell, 0U, NULL);
	assert(ret == -EBUSY);
	assert_single_shell_line(FAKE_SHELL_LINE_ERROR, "task clear error=busy");
	assert_blob_still_stored();
	assert(linkr_debugger_capture_arbiter_owner() == LINKR_DEBUGGER_CAPTURE_OWNER_NONE);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_OTA);
	assert(linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_OTA));
}

static void test_clear_after_owner_release_succeeds(void)
{
	int ret;

	reset_all();
	store_valid_blob();
	assert(linkr_debugger_capture_arbiter_try_acquire(
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR));
	ret = linkr_debugger_task_shell_clear(&test_shell, 0U, NULL);
	assert(ret == -EBUSY);
	assert_blob_still_stored();
	assert(linkr_debugger_capture_arbiter_release(
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR));
	shell_output.line_count = 0U;
	ret = linkr_debugger_task_shell_clear(&test_shell, 0U, NULL);
	assert(ret == 0);
	assert_single_shell_line(FAKE_SHELL_LINE_PRINT, "task clear ok");
	assert(!store_present);
}

static void test_show_reports_status(void)
{
	int ret;

	reset_all();
	store_valid_blob();
	ret = linkr_debugger_task_shell_show(&test_shell, 0U, NULL);
	assert(ret == 0);
	assert_single_shell_line(FAKE_SHELL_LINE_PRINT,
				 "task show available=true task_count=1");
	assert(linkr_debugger_capture_arbiter_owner() == LINKR_DEBUGGER_CAPTURE_OWNER_NONE);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_NONE);
}

int main(void)
{
	test_clear_succeeds_when_owners_free();
	test_clear_rejected_while_capture_owner_held();
	test_clear_rejected_while_flash_owner_held();
	test_clear_after_owner_release_succeeds();
	test_show_reports_status();
	puts("linkr_debugger_task_shell: all tests passed");
	return 0;
}
