/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AGENT_DEBUGBOARD_WS_H_
#define AGENT_DEBUGBOARD_WS_H_

#include <zephyr/net/http/server.h>

#include <stdbool.h>
#include <stdint.h>

#define DEBUGBOARD_WS_MAX_CLIENTS 4
#define DEBUGBOARD_WS_RECV_BUFFER_SIZE 512

struct debugboard_ws_session_info {
	uint8_t slot;
	bool active;
	bool connected;
	uint32_t session_id;
	char ws_path[64];
};

int debugboard_ws_init(void);
void debugboard_ws_publish_state_change(void);
void debugboard_ws_publish_sample(void);
int debugboard_ws_setup(int ws_socket, struct http_request_ctx *request_ctx, void *user_data);
int debugboard_ws_session_create(struct debugboard_ws_session_info *info);
int debugboard_ws_session_delete(uint32_t session_id);
int debugboard_ws_session_lookup(uint32_t session_id, struct debugboard_ws_session_info *info);

#endif /* AGENT_DEBUGBOARD_WS_H_ */
