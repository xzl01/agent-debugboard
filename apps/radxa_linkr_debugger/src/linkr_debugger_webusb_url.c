/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_webusb_url.h"

#include <errno.h>
#include <string.h>

#define WEBUSB_URL_DESCRIPTOR_TYPE 0x03U
#define WEBUSB_URL_SCHEME_HTTPS 0x01U

int linkr_debugger_webusb_url_encode(const struct linkr_debugger_net_identity *identity,
				     uint8_t *descriptor, size_t capacity,
				     size_t *descriptor_len)
{
	char ipv4[LINKR_DEBUGGER_NET_IPV4_STR_LEN];
	size_t ipv4_len;
	size_t required;
	int ret;

	if (descriptor_len != NULL) {
		*descriptor_len = 0U;
	}

	if (descriptor == NULL || descriptor_len == NULL ||
	    !linkr_debugger_net_identity_valid(identity)) {
		return -EINVAL;
	}

	ret = linkr_debugger_net_format_ipv4(identity->network,
					    LINKR_DEBUGGER_NET_DEVICE_HOST,
					    ipv4, sizeof(ipv4));
	if (ret < 0) {
		return ret;
	}

	ipv4_len = strlen(ipv4);
	required = 3U + sizeof(LINKR_DEBUGGER_WEBUSB_URL_PREFIX) - 1U + ipv4_len;
	if (capacity < required) {
		return -ENOSPC;
	}

	descriptor[0] = (uint8_t)required;
	descriptor[1] = WEBUSB_URL_DESCRIPTOR_TYPE;
	descriptor[2] = WEBUSB_URL_SCHEME_HTTPS;
	memcpy(&descriptor[3], LINKR_DEBUGGER_WEBUSB_URL_PREFIX,
	       sizeof(LINKR_DEBUGGER_WEBUSB_URL_PREFIX) - 1U);
	memcpy(&descriptor[2U + sizeof(LINKR_DEBUGGER_WEBUSB_URL_PREFIX)], ipv4, ipv4_len);
	*descriptor_len = required;

	return 0;
}
