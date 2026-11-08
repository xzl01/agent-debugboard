/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_WEBUSB_URL_H_
#define RADXA_LINKR_DEBUGGER_WEBUSB_URL_H_

#include "linkr_debugger_net_settings.h"

#include <stddef.h>
#include <stdint.h>

#define LINKR_DEBUGGER_WEBUSB_URL_PREFIX \
	"xzl01.github.io/agent-debugboard/connect.html#device="
#define LINKR_DEBUGGER_WEBUSB_URL_DESCRIPTOR_MAX_LEN \
	(3U + sizeof(LINKR_DEBUGGER_WEBUSB_URL_PREFIX) - 1U + \
	 LINKR_DEBUGGER_NET_IPV4_STR_LEN - 1U)

int linkr_debugger_webusb_url_encode(const struct linkr_debugger_net_identity *identity,
				     uint8_t *descriptor, size_t capacity,
				     size_t *descriptor_len);

#endif /* RADXA_LINKR_DEBUGGER_WEBUSB_URL_H_ */
