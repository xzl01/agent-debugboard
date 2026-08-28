/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_build_info.h"

#include <assert.h>
#include <string.h>

static void test_build_identity_fields(void)
{
	assert(strcmp(linkr_debugger_build_sysname(), "Zephyr") == 0);
	assert(strcmp(linkr_debugger_build_nodename(), "linkr-debugger") == 0);
	assert(strcmp(linkr_debugger_build_release(), "0.3.0-test") == 0);
	assert(strcmp(linkr_debugger_build_id(), "unit-test-build-1") == 0);
	assert(strcmp(linkr_debugger_build_machine(), "arm-test") == 0);
	assert(strcmp(linkr_debugger_build_processor(), "rp2350a-test") == 0);
	assert(strcmp(linkr_debugger_build_platform(), "test-board-target") == 0);
	assert(strcmp(linkr_debugger_build_profile(), "fault-injection") == 0);
	assert(strcmp(linkr_debugger_build_image_version(), "test-image") == 0);
}

static void test_build_version_contains_id_and_compile_time(void)
{
	const char *version = linkr_debugger_build_version();

	assert(version != NULL);
	assert(strstr(version, "unit-test-build-1") != NULL);
	assert(strstr(version, "(") != NULL);
	assert(strstr(version, ")") != NULL);
}

int main(void)
{
	test_build_identity_fields();
	test_build_version_contains_id_and_compile_time();
	return 0;
}
