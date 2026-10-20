/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_SERIAL_HTTP_H_
#define RADXA_LINKR_DEBUGGER_SERIAL_HTTP_H_

#include <zephyr/net/http/server.h>

#define LINKR_DEBUGGER_SERIAL_HTTP_STATUS_PATH "/api/v1/serial/j4"
#define LINKR_DEBUGGER_SERIAL_HTTP_CONFIG_PATH "/api/v1/serial/j4/config"

enum linkr_debugger_serial_http_route {
	LINKR_DEBUGGER_SERIAL_HTTP_ROUTE_STATUS = 0,
	LINKR_DEBUGGER_SERIAL_HTTP_ROUTE_CONFIG,
};

int linkr_debugger_serial_http_handle(
	struct http_client_ctx *client, enum http_transaction_status status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response, void *user_data);

#endif /* RADXA_LINKR_DEBUGGER_SERIAL_HTTP_H_ */
