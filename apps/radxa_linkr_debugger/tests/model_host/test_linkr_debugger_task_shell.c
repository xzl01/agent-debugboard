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

#define ENTRY_COUNT 64U

enum fake_shell_line_kind {
	FAKE_SHELL_LINE_PRINT,
	FAKE_SHELL_LINE_ERROR,
};

static struct {
	enum fake_shell_line_kind kind;
	char line[160];
	size_t count;
} shell_output;

void shell_print(const struct shell *sh, const char *format, ...)
{
	va_list args;

	(void)sh;
	va_start(args, format);
	assert(vsnprintf(shell_output.line, sizeof(shell_output.line), format, args) > 0);
	va_end(args);
	shell_output.kind = FAKE_SHELL_LINE_PRINT;
	shell_output.count++;
}

void shell_error(const struct shell *sh, const char *format, ...)
{
	va_list args;

	(void)sh;
	va_start(args, format);
	assert(vsnprintf(shell_output.line, sizeof(shell_output.line), format, args) > 0);
	va_end(args);
	shell_output.kind = FAKE_SHELL_LINE_ERROR;
	shell_output.count++;
}

struct memory_entry {
	bool present;
	size_t len;
	uint8_t data[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE];
	char name[48];
};

static struct memory_entry entries[ENTRY_COUNT];

static struct memory_entry *memory_find(const char *name)
{
	for (size_t i = 0U; i < ENTRY_COUNT; i++) {
		if (entries[i].present && strcmp(entries[i].name, name) == 0) {
			return &entries[i];
		}
	}
	return NULL;
}

static int memory_subsys_init(void *context)
{
	(void)context;
	return 0;
}

static int memory_load_one(void *context, const char *name, void *value, size_t cap)
{
	struct memory_entry *entry;
	size_t copied;

	(void)context;
	entry = memory_find(name);
	if (entry == NULL) {
		return 0;
	}
	copied = entry->len < cap ? entry->len : cap;
	memcpy(value, entry->data, copied);
	return (int)entry->len;
}

static int memory_save_one(void *context, const char *name, const void *value,
			   size_t len)
{
	struct memory_entry *entry;

	(void)context;
	assert(len <= LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE);
	entry = memory_find(name);
	if (entry == NULL) {
		for (size_t i = 0U; i < ENTRY_COUNT; i++) {
			if (!entries[i].present) {
				entry = &entries[i];
				break;
			}
		}
		assert(entry != NULL);
		(void)snprintf(entry->name, sizeof(entry->name), "%s", name);
		entry->present = true;
	}
	memcpy(entry->data, value, len);
	entry->len = len;
	return 0;
}

static int memory_delete_one(void *context, const char *name)
{
	struct memory_entry *entry;

	(void)context;
	entry = memory_find(name);
	if (entry != NULL) {
		entry->present = false;
		entry->len = 0U;
	}
	return 0;
}

static void memory_random(void *context, void *value, size_t len)
{
	(void)context;
	memset(value, 0x5a, len);
}

static const struct linkr_debugger_task_backend_ops memory_backend = {
	.subsys_init = memory_subsys_init,
	.load_one = memory_load_one,
	.save_one = memory_save_one,
	.delete_one = memory_delete_one,
	.random = memory_random,
};

static const struct shell test_shell;

static void reset_all(void)
{
	memset(entries, 0, sizeof(entries));
	memset(&shell_output, 0, sizeof(shell_output));
	linkr_debugger_capture_arbiter_reset();
	linkr_debugger_flash_arbiter_reset();
	linkr_debugger_task_test_set_backend(&memory_backend, NULL);
	linkr_debugger_task_init();
}

static void assert_line(enum fake_shell_line_kind kind, const char *text)
{
	assert(shell_output.count == 1U);
	assert(shell_output.kind == kind);
	assert(strcmp(shell_output.line, text) == 0);
}

static void test_clear_commits_new_revision_and_releases_owners(void)
{
	struct linkr_debugger_task_status status;

	reset_all();
	assert(linkr_debugger_task_shell_clear(&test_shell, 0U, NULL) == 0);
	assert_line(FAKE_SHELL_LINE_PRINT, "task clear ok");
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	assert(status.revision == 1U && status.task_count == 0U);
	assert(linkr_debugger_capture_arbiter_owner() == LINKR_DEBUGGER_CAPTURE_OWNER_NONE);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_NONE);
}

static void test_clear_busy_has_zero_writes(void)
{
	struct linkr_debugger_task_status status;

	reset_all();
	assert(linkr_debugger_flash_arbiter_try_acquire(LINKR_DEBUGGER_FLASH_OWNER_OTA));
	assert(linkr_debugger_task_shell_clear(&test_shell, 0U, NULL) == -EBUSY);
	assert_line(FAKE_SHELL_LINE_ERROR, "task clear error=busy");
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	assert(status.revision == 0U);
	assert(linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_OTA));
}

static void test_show_reports_v2_status(void)
{
	reset_all();
	assert(linkr_debugger_task_shell_show(&test_shell, 0U, NULL) == 0);
	assert_line(FAKE_SHELL_LINE_PRINT, "task show available=true task_count=0");
}

int main(void)
{
	test_clear_commits_new_revision_and_releases_owners();
	test_clear_busy_has_zero_writes();
	test_show_reports_v2_status();
	puts("linkr_debugger_task_shell: all tests passed");
	return 0;
}
