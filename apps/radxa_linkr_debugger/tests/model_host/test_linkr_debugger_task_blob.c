/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "../../src/linkr_debugger_task_blob.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char valid_blob[] =
	"# linkr-task.v1\n"
	"# retained comment\n"
	"# task caf\xc3\xa9/one\n"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP7\",\"body\":\"{}\"}\n"
	"# trailing section comment\n"
	"# task two\n"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\","
	"\"body\":\"{\\\"state\\\":\\\"off\\\"}\",\"wait_ms\":1}\n";

static void test_layout_tracks_exact_section_and_digest_boundaries(void)
{
	struct linkr_debugger_task_blob_layout layout;
	struct linkr_debugger_task_status status;
	const char *second = strstr(valid_blob, "# task two\n");
	const char *trailing = strstr(valid_blob, "# trailing section comment\n");

	assert(linkr_debugger_task_blob_inspect(valid_blob, sizeof(valid_blob) - 1U,
						false, &layout));
	assert(layout.task_count == 2U);
	assert(strcmp(layout.tasks[0].id, "caf\xc3\xa9/one") == 0);
	assert(layout.tasks[0].request_count == 1U);
	assert(layout.tasks[0].section_end == (size_t)(second - valid_blob));
	assert(layout.tasks[0].digest_end == (size_t)(trailing - valid_blob));
	assert(strcmp(layout.tasks[1].id, "two") == 0);
	assert(layout.tasks[1].section_end == sizeof(valid_blob) - 1U);
	assert(linkr_debugger_task_blob_parse(valid_blob, sizeof(valid_blob) - 1U,
					      &status));
	assert(status.task_count == 2U && status.tasks[0].slot == 0U &&
	       status.tasks[1].slot == 1U);
}

static void test_empty_marker_is_explicitly_opt_in(void)
{
	struct linkr_debugger_task_blob_layout layout;
	static const char empty[] = LINKR_DEBUGGER_TASK_BLOB_HEADER;

	assert(linkr_debugger_task_blob_inspect(empty, sizeof(empty) - 1U, true, &layout));
	assert(layout.task_count == 0U);
	assert(!linkr_debugger_task_blob_inspect(empty, sizeof(empty) - 1U, false, &layout));
}

static void test_rejects_noncanonical_utf8_controls_ids_and_framing(void)
{
	static const char *const invalid[] = {
		"# linkr-task.v1\r\n# task A\r\n",
		"# linkr-task.v1\n# task has space\n",
		"# linkr-task.v1\n# task has#hash\n",
		"# linkr-task.v1\n# task A\n#\tcomment\n",
		"# linkr-task.v1\n# task A\n{\"method\":\"POST\",\"path\":\"/api/v1/gpio/GP7\",\"body\":\"{}\"}\n",
		"# linkr-task.v1\n# task A\n{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP7\",\"body\":\"{}\"}",
		"# linkr-task.v1\n# task A\n# linkr-task.v1\n",
	};
	static const char invalid_utf8[] =
		"# linkr-task.v1\n# task \xc0\x80\n";
	static const char invalid_control[] =
		"# linkr-task.v1\n# task A\n#\x01\n";
	struct linkr_debugger_task_blob_layout layout;

	for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		assert(!linkr_debugger_task_blob_inspect(invalid[i], strlen(invalid[i]),
						 false, &layout));
	}
	assert(!linkr_debugger_task_blob_inspect(invalid_utf8,
					 sizeof(invalid_utf8) - 1U, false, &layout));
	assert(!linkr_debugger_task_blob_inspect(invalid_control,
					 sizeof(invalid_control) - 1U, false, &layout));
	assert(!linkr_debugger_task_id_valid("", 0U));
	assert(!linkr_debugger_task_id_valid("\x7f", 1U));
}

static void test_exact_4000_byte_limit(void)
{
	char blob[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE + 1U];
	struct linkr_debugger_task_blob_layout layout;
	static const char prefix[] =
		"# linkr-task.v1\n# task max\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP7\",\"body\":\"{}\"}\n";
	size_t len = sizeof(prefix) - 1U;

	memcpy(blob, prefix, len);
	while (len < LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE) {
		size_t remaining = LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE - len;
		size_t line = remaining > 200U ? 200U : remaining;

		assert(line >= 2U);
		blob[len++] = '#';
		memset(blob + len, 'x', line - 2U);
		len += line - 2U;
		blob[len++] = '\n';
	}
	assert(linkr_debugger_task_blob_inspect(blob, len, false, &layout));
	blob[len] = '\n';
	assert(!linkr_debugger_task_blob_inspect(blob, len + 1U, false, &layout));
}

int main(void)
{
	test_layout_tracks_exact_section_and_digest_boundaries();
	test_empty_marker_is_explicitly_opt_in();
	test_rejects_noncanonical_utf8_controls_ids_and_framing();
	test_exact_4000_byte_limit();
	puts("linkr_debugger_task_blob: all tests passed");
	return 0;
}
