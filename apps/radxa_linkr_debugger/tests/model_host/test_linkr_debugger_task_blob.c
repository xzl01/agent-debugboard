/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "../../src/linkr_debugger_task_blob.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_valid_blob(void)
{
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task recovery\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":0}\n"
		"# task alternate\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/power/12v_out\",\"body\":\"{\\\"state\\\":\\\"off\\\"}\",\"wait_ms\":60000}\n";
	struct linkr_debugger_task_status result;

	assert(linkr_debugger_task_blob_parse(blob, sizeof(blob) - 1U, &result));
	assert(result.task_count == 2U);
	assert(strcmp(result.tasks[0].id, "recovery") == 0);
	assert(strcmp(result.tasks[0].name, "recovery") == 0);
	assert(result.tasks[0].request_count == 1U);
	assert(strcmp(result.tasks[1].id, "alternate") == 0);
	assert(result.tasks[1].request_count == 1U);
}

static void test_invalid_blob(void)
{
	static const char missing_version[] =
		"# task recovery\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":0}\n";
	static const char duplicate_task[] =
		"# linkr-task.v1\n"
		"# task recovery\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":0}\n"
		"# task recovery\n";
	static const char bad_request[] =
		"# linkr-task.v1\n"
		"# task recovery\n"
		"not-a-request\n";
	struct linkr_debugger_task_status result;

	assert(!linkr_debugger_task_blob_parse(
		missing_version, sizeof(missing_version) - 1U, &result));
	assert(!linkr_debugger_task_blob_parse(
		duplicate_task, sizeof(duplicate_task) - 1U, &result));
	assert(!linkr_debugger_task_blob_parse(
		bad_request, sizeof(bad_request) - 1U, &result));
	assert(!linkr_debugger_task_blob_parse(NULL, 0U, &result));
	assert(!linkr_debugger_task_blob_parse(missing_version, 0U, NULL));
}

static void test_delete_task(void)
{
	static const char blob[] =
		"# linkr-task.v1\n"
		"# task recovery\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":0}\n"
		"# task alternate\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/power/12v_out\",\"body\":\"{\\\"state\\\":\\\"off\\\"}\",\"wait_ms\":60000}\n";
	static const char only_recovery[] =
		"# linkr-task.v1\n"
		"# task recovery\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":0}\n";
	static const char drop_first[] =
		"# linkr-task.v1\n"
		"# task alternate\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/power/12v_out\",\"body\":\"{\\\"state\\\":\\\"off\\\"}\",\"wait_ms\":60000}\n";
	static const char drop_second[] =
		"# linkr-task.v1\n"
		"# task recovery\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":0}\n";
	static const char marker_only[] = "# linkr-task.v1\n";
	char out[512];
	size_t out_len = 999U;
	size_t remaining = 999U;
	enum linkr_debugger_task_blob_edit_result edit;

	/* Deleting the first task keeps the second byte-for-byte. */
	edit = linkr_debugger_task_blob_delete_task(
		blob, sizeof(blob) - 1U, "recovery", out, sizeof(out), &out_len, &remaining);
	assert(edit == LINKR_DEBUGGER_TASK_BLOB_EDIT_OK);
	assert(remaining == 1U);
	assert(out_len == sizeof(drop_first) - 1U);
	assert(memcmp(out, drop_first, out_len) == 0);

	/* Deleting the second task keeps the first byte-for-byte. */
	edit = linkr_debugger_task_blob_delete_task(
		blob, sizeof(blob) - 1U, "alternate", out, sizeof(out), &out_len, &remaining);
	assert(edit == LINKR_DEBUGGER_TASK_BLOB_EDIT_OK);
	assert(remaining == 1U);
	assert(out_len == sizeof(drop_second) - 1U);
	assert(memcmp(out, drop_second, out_len) == 0);

	/* Deleting the only task leaves just the marker and zero tasks. */
	edit = linkr_debugger_task_blob_delete_task(
		only_recovery, sizeof(only_recovery) - 1U, "recovery",
		out, sizeof(out), &out_len, &remaining);
	assert(edit == LINKR_DEBUGGER_TASK_BLOB_EDIT_OK);
	assert(remaining == 0U);
	assert(out_len == sizeof(marker_only) - 1U);
	assert(memcmp(out, marker_only, out_len) == 0);

	/* An absent id is reported as not found without touching the output. */
	edit = linkr_debugger_task_blob_delete_task(
		blob, sizeof(blob) - 1U, "absent", out, sizeof(out), &out_len, &remaining);
	assert(edit == LINKR_DEBUGGER_TASK_BLOB_EDIT_NOT_FOUND);

	/* Invalid ids are rejected outright. */
	edit = linkr_debugger_task_blob_delete_task(
		blob, sizeof(blob) - 1U, "has space", out, sizeof(out), &out_len, &remaining);
	assert(edit == LINKR_DEBUGGER_TASK_BLOB_EDIT_INVALID);
	edit = linkr_debugger_task_blob_delete_task(
		blob, sizeof(blob) - 1U, "", out, sizeof(out), &out_len, &remaining);
	assert(edit == LINKR_DEBUGGER_TASK_BLOB_EDIT_INVALID);

	/* A too-small output buffer is reported without overrunning. */
	edit = linkr_debugger_task_blob_delete_task(
		blob, sizeof(blob) - 1U, "recovery", out, 4U, &out_len, &remaining);
	assert(edit == LINKR_DEBUGGER_TASK_BLOB_EDIT_TOO_SMALL);
}

static void test_rejects_exact_dot_ids_for_parse_and_delete(void)
{
	static const char *const ids[] = { ".", ".." };
	static const char existing[] = "# linkr-task.v1\n# task valid\n";
	struct linkr_debugger_task_status result;
	char blob[64];
	char out[64];

	for (size_t i = 0U; i < sizeof(ids) / sizeof(ids[0]); i++) {
		size_t out_len = 999U;
		size_t remaining = 999U;
		int written = snprintf(blob, sizeof(blob), "# linkr-task.v1\n# task %s\n", ids[i]);

		assert(written > 0 && (size_t)written < sizeof(blob));
		assert(!linkr_debugger_task_blob_parse(blob, (size_t)written, &result));
		memset(out, 0xa5, sizeof(out));
		assert(linkr_debugger_task_blob_delete_task(
			       existing, sizeof(existing) - 1U, ids[i], out, sizeof(out),
			       &out_len, &remaining) == LINKR_DEBUGGER_TASK_BLOB_EDIT_INVALID);
		assert(out_len == 999U);
		assert(remaining == 999U);
		assert((unsigned char)out[0] == 0xa5U);
	}
}

int main(void)
{
	test_valid_blob();
	test_invalid_blob();
	test_delete_task();
	test_rejects_exact_dot_ids_for_parse_and_delete();
	puts("linkr_debugger_task_blob: all tests passed");
	return 0;
}
