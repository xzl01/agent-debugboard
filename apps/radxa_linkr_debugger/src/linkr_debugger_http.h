/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RADXA_LINKR_DEBUGGER_HTTP_H_
#define RADXA_LINKR_DEBUGGER_HTTP_H_

#include <stdint.h>

void linkr_debugger_http_init(void);
void linkr_debugger_http_publish_state_change(void);
int linkr_debugger_http_listener_fd(void);
uint16_t linkr_debugger_http_listener_port(void);
const char *linkr_debugger_http_listener_host(void);

#endif /* RADXA_LINKR_DEBUGGER_HTTP_H_ */
