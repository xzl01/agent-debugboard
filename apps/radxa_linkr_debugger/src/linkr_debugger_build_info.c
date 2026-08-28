/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_build_info.h"

#include <zephyr/app_version.h>
#include <zephyr/version.h>

#define LINKR_DEBUGGER_BUILD_INFO_STRINGIFY_INNER(x) #x
#define LINKR_DEBUGGER_BUILD_INFO_STRINGIFY(x) \
	LINKR_DEBUGGER_BUILD_INFO_STRINGIFY_INNER(x)

#define LINKR_DEBUGGER_BUILD_ID_STRING \
	LINKR_DEBUGGER_BUILD_INFO_STRINGIFY(BUILD_VERSION)

const char *linkr_debugger_build_sysname(void)
{
	return "Zephyr";
}

const char *linkr_debugger_build_nodename(void)
{
	return "linkr-debugger";
}

const char *linkr_debugger_build_release(void)
{
	return APP_VERSION_STRING;
}

const char *linkr_debugger_build_version(void)
{
	return LINKR_DEBUGGER_BUILD_ID_STRING " (" __DATE__ " " __TIME__ ")";
}

const char *linkr_debugger_build_machine(void)
{
	return CONFIG_ARCH;
}

const char *linkr_debugger_build_processor(void)
{
	return CONFIG_SOC;
}

const char *linkr_debugger_build_platform(void)
{
	return CONFIG_BOARD_TARGET;
}

const char *linkr_debugger_build_id(void)
{
	return LINKR_DEBUGGER_BUILD_ID_STRING;
}

const char *linkr_debugger_build_time(void)
{
	return __DATE__ " " __TIME__;
}

const char *linkr_debugger_build_profile(void)
{
#ifdef CONFIG_LINKR_DEBUGGER_FAULT_INJECTION
	return "fault-injection";
#else
	return "production";
#endif
}

const char *linkr_debugger_build_image_version(void)
{
#ifdef CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
	return CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION;
#else
	return "unknown";
#endif
}
