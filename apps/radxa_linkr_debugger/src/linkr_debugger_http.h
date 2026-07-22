/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_HTTP_H_
#define RADXA_LINKR_DEBUGGER_HTTP_H_

#include <stdint.h>

void linkr_debugger_http_init(void);
void linkr_debugger_http_publish_state_change(void);
int linkr_debugger_http_listener_fd(void);
uint16_t linkr_debugger_http_listener_port(void);
const char *linkr_debugger_http_listener_host(void);
void linkr_debugger_http_reap_stale_holders(void);

#endif /* RADXA_LINKR_DEBUGGER_HTTP_H_ */
