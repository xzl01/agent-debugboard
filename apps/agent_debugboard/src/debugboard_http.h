/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AGENT_DEBUGBOARD_HTTP_H_
#define AGENT_DEBUGBOARD_HTTP_H_

#include <stdint.h>

void debugboard_http_init(void);
void debugboard_http_publish_state_change(void);
int debugboard_http_listener_fd(void);
uint16_t debugboard_http_listener_port(void);
const char *debugboard_http_listener_host(void);

#endif /* AGENT_DEBUGBOARD_HTTP_H_ */
