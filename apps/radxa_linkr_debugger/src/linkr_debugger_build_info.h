/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_BUILD_INFO_H_
#define RADXA_LINKR_DEBUGGER_BUILD_INFO_H_

#ifdef __cplusplus
extern "C" {
#endif

const char *linkr_debugger_build_sysname(void);
const char *linkr_debugger_build_nodename(void);
const char *linkr_debugger_build_release(void);
const char *linkr_debugger_build_version(void);
const char *linkr_debugger_build_machine(void);
const char *linkr_debugger_build_processor(void);
const char *linkr_debugger_build_platform(void);
const char *linkr_debugger_build_id(void);
const char *linkr_debugger_build_time(void);
const char *linkr_debugger_build_profile(void);
const char *linkr_debugger_build_image_version(void);

#ifdef __cplusplus
}
#endif

#endif /* RADXA_LINKR_DEBUGGER_BUILD_INFO_H_ */
