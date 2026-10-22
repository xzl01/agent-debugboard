/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_POWER_JSON_H_
#define RADXA_LINKR_DEBUGGER_POWER_JSON_H_

#include <stdbool.h>
#include <stddef.h>

/* Power-output JSON shared by the HTTP status/power endpoints and the WebSocket
 * status snapshot, so both transports report one contract:
 *
 * - Ordinary rails: {"name","state","value"} plus metadata at FULL detail,
 *   where "state" is "on"/"off"/"locked" and "value" is the enable level.
 * - Port VBUS rails (desired != NULL): "state" mirrors the effective state
 *   ("on"/"off"/"blocked") verbatim and "value" is null - a released port is
 *   never presented as a measured 0 V. FULL detail adds "role":"port_vbus",
 *   "measured":false and the separated desired/requested/effective/backfeed
 *   fields. Backfeed strings are "possible"/"unknown"/"detected", never
 *   "none", because the switch body diode can backfeed the parent rail.
 */
enum linkr_debugger_power_json_detail {
	LINKR_DEBUGGER_POWER_JSON_BRIEF = 0,
	LINKR_DEBUGGER_POWER_JSON_FULL = 1,
};

struct linkr_debugger_power_output_view {
	const char *name;
	const char *signal;
	unsigned int gp;
	bool controllable;
	bool locked;
	bool on;
	/* Non-NULL marks a bidirectional port VBUS rail; strings must be the
	 * model helpers' names ("on"/"off", "on"/"off"/"blocked",
	 * "possible"/"unknown"/"detected").
	 */
	const char *desired;
	const char *requested;
	const char *effective;
	const char *backfeed;
};

struct linkr_debugger_power_json_writer {
	void *context;
	int (*append)(void *context, const char *text);
};

#define LINKR_DEBUGGER_POWER_JSON_NAME_MAX 24U
#define LINKR_DEBUGGER_POWER_JSON_SIGNAL_MAX 32U

int linkr_debugger_power_outputs_json_write(
	const struct linkr_debugger_power_output_view *views, size_t count,
	enum linkr_debugger_power_json_detail detail,
	struct linkr_debugger_power_json_writer writer);

/* Upper bound on the serialized array size for `count` outputs; used by
 * tests to keep both transport budgets (HTTP status workspace and the WS
 * send buffer) provably large enough.
 */
size_t linkr_debugger_power_outputs_json_worst_case_size(
	size_t count, enum linkr_debugger_power_json_detail detail);

#endif /* RADXA_LINKR_DEBUGGER_POWER_JSON_H_ */
