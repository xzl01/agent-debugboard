/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_DNS_H_
#define RADXA_LINKR_DEBUGGER_DNS_H_

#include <stddef.h>
#include <stdint.h>

#define LINKR_DEBUGGER_DNS_PORT 53U
#define LINKR_DEBUGGER_DNS_TTL_SECONDS 30U
#define LINKR_DEBUGGER_DNS_RESPONSE_MAX_SIZE 512U

int linkr_debugger_dns_build_response(const uint8_t *query, size_t query_len,
				      uint8_t *response, size_t response_cap,
				      size_t *response_len);

#if !defined(LINKR_DEBUGGER_DNS_HOST_TEST)
int linkr_debugger_dns_start(void);
#endif

#endif /* RADXA_LINKR_DEBUGGER_DNS_H_ */
