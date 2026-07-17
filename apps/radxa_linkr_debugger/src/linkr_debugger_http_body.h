/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_HTTP_BODY_H_
#define RADXA_LINKR_DEBUGGER_HTTP_BODY_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define LINKR_DEBUGGER_HTTP_BODY_CAP 1024U
#define LINKR_DEBUGGER_HTTP_BODY_SLOTS 4U

enum linkr_debugger_http_body_event {
	LINKR_DEBUGGER_HTTP_BODY_MORE,
	LINKR_DEBUGGER_HTTP_BODY_FINAL,
	LINKR_DEBUGGER_HTTP_BODY_ABORTED,
	LINKR_DEBUGGER_HTTP_BODY_COMPLETE,
};

enum linkr_debugger_http_body_result {
	LINKR_DEBUGGER_HTTP_BODY_WAITING = 0,
	LINKR_DEBUGGER_HTTP_BODY_READY = 1,
	LINKR_DEBUGGER_HTTP_BODY_CLEARED = 2,
	LINKR_DEBUGGER_HTTP_BODY_TOO_LARGE = -1,
	LINKR_DEBUGGER_HTTP_BODY_MISMATCH = -2,
	LINKR_DEBUGGER_HTTP_BODY_BAD_ARG = -3,
};

struct linkr_debugger_http_body_view {
	const uint8_t *data;
	size_t len;
};

bool linkr_debugger_http_body_should_handle(bool body_method,
					   enum linkr_debugger_http_body_event event);
void linkr_debugger_http_body_reset_all(void);
void linkr_debugger_http_body_clear(uintptr_t client_key, uint16_t method_id,
				    uint16_t route_id);
enum linkr_debugger_http_body_result linkr_debugger_http_body_accumulate(
	uintptr_t client_key, uint16_t method_id, uint16_t route_id,
	enum linkr_debugger_http_body_event event, const void *data, size_t len,
	struct linkr_debugger_http_body_view *view);

#endif /* RADXA_LINKR_DEBUGGER_HTTP_BODY_H_ */
