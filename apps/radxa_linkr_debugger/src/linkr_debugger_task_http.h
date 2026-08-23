/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_TASK_HTTP_H_
#define RADXA_LINKR_DEBUGGER_TASK_HTTP_H_

#ifndef LINKR_DEBUGGER_TASK_HTTP_HOST_TEST
#include <zephyr/net/http/server.h>
#endif

enum linkr_debugger_task_http_route {
	LINKR_DEBUGGER_TASK_HTTP_ROUTE_TASKS = 0,
	LINKR_DEBUGGER_TASK_HTTP_ROUTE_CATALOG,
};

int linkr_debugger_task_http_handle(
	struct http_client_ctx *client,
	enum http_transaction_status status,
	const struct http_request_ctx *request_ctx,
	struct http_response_ctx *response_ctx,
	void *user_data);

#endif
