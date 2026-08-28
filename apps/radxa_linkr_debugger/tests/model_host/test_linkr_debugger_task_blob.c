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

int main(void)
{
	test_valid_blob();
	test_invalid_blob();
	puts("linkr_debugger_task_blob: all tests passed");
	return 0;
}
