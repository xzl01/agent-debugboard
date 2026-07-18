/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_RIGOL_H_
#define RADXA_LINKR_DEBUGGER_RIGOL_H_

#define LINKR_DEBUGGER_RIGOL_SERVER_PORT 80U
#define LINKR_DEBUGGER_RIGOL_BUFFER_SAMPLES 4096U

struct http_request_ctx;

void linkr_debugger_rigol_server_init(void);

int linkr_debugger_rigol_scpi_ws_setup(int ws_socket,
	struct http_request_ctx *request_ctx, void *user_data);

#endif /* RADXA_LINKR_DEBUGGER_RIGOL_H_ */
