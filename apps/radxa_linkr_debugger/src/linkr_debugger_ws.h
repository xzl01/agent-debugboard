/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RADXA_LINKR_DEBUGGER_WS_H_
#define RADXA_LINKR_DEBUGGER_WS_H_

#include <zephyr/net/http/server.h>

#include <stdbool.h>
#include <stdint.h>

#define LINKR_DEBUGGER_WS_MAX_CLIENTS 4
#define LINKR_DEBUGGER_WS_RECV_BUFFER_SIZE 512

struct linkr_debugger_ws_session_info {
	uint8_t slot;
	bool active;
	bool connected;
	uint32_t session_id;
	char ws_path[64];
};

int linkr_debugger_ws_init(void);
void linkr_debugger_ws_publish_state_change(void);
void linkr_debugger_ws_publish_sample(void);
int linkr_debugger_ws_setup(int ws_socket, struct http_request_ctx *request_ctx, void *user_data);
int linkr_debugger_ws_session_create(struct linkr_debugger_ws_session_info *info);
int linkr_debugger_ws_session_delete(uint32_t session_id);
int linkr_debugger_ws_session_lookup(uint32_t session_id, struct linkr_debugger_ws_session_info *info);

#endif /* RADXA_LINKR_DEBUGGER_WS_H_ */
