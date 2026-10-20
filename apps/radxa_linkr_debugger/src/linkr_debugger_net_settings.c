/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_net_settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef LINKR_DEBUGGER_NET_SETTINGS_HOST_TEST
#include <zephyr/settings/settings.h>
#endif

bool linkr_debugger_net_subnet_valid(uint8_t subnet)
{
	return subnet >= LINKR_DEBUGGER_NET_SUBNET_MIN &&
	       subnet <= LINKR_DEBUGGER_NET_SUBNET_MAX;
}

int linkr_debugger_net_parse_subnet(const char *text, uint8_t *subnet)
{
	const unsigned char *cursor;
	unsigned long value;
	char *end;

	if (text == NULL || subnet == NULL || text[0] == '\0') {
		return -EINVAL;
	}

	/* strtoul accepts signs and leading whitespace; the shell grammar does not. */
	for (cursor = (const unsigned char *)text; *cursor != '\0'; cursor++) {
		if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9') {
			return -EINVAL;
		}
	}

	errno = 0;
	value = strtoul(text, &end, 10);
	if (end == text || *end != '\0' || errno == ERANGE ||
	    value < LINKR_DEBUGGER_NET_SUBNET_MIN ||
	    value > LINKR_DEBUGGER_NET_SUBNET_MAX) {
		return -EINVAL;
	}

	*subnet = (uint8_t)value;
	return 0;
}

int linkr_debugger_net_format_ipv4(uint8_t subnet, uint8_t host, char *buf,
				   size_t cap)
{
	int written;

	if (buf == NULL || cap == 0U || !linkr_debugger_net_subnet_valid(subnet)) {
		return -EINVAL;
	}

	written = snprintf(buf, cap, "172.29.%u.%u", (unsigned int)subnet,
			   (unsigned int)host);
	if (written < 0) {
		return -EINVAL;
	}
	if ((size_t)written >= cap) {
		return -ENOSPC;
	}

	return 0;
}

int linkr_debugger_net_format_captive_uri(uint8_t subnet, char *buf,
					  size_t cap)
{
	char ip[16];
	int written;
	int ret;

	ret = linkr_debugger_net_format_ipv4(subnet, LINKR_DEBUGGER_NET_DEVICE_HOST,
					     ip, sizeof(ip));
	if (ret < 0) {
		return ret;
	}

	written = snprintf(buf, cap, "http://%s/", ip);
	if (written < 0) {
		return -EINVAL;
	}
	if ((size_t)written >= cap) {
		return -ENOSPC;
	}

	return 0;
}

#ifndef LINKR_DEBUGGER_NET_SETTINGS_HOST_TEST
uint8_t linkr_debugger_net_subnet_load(void)
{
	uint8_t subnet = LINKR_DEBUGGER_NET_SUBNET_DEFAULT;
	ssize_t len;

	len = settings_load_one(LINKR_DEBUGGER_NET_SETTINGS_KEY, &subnet,
				sizeof(subnet));
	if (len != (ssize_t)sizeof(subnet) ||
	    !linkr_debugger_net_subnet_valid(subnet)) {
		return LINKR_DEBUGGER_NET_SUBNET_DEFAULT;
	}

	return subnet;
}

int linkr_debugger_net_subnet_save(uint8_t subnet)
{
	if (!linkr_debugger_net_subnet_valid(subnet)) {
		return -EINVAL;
	}

	return (int)settings_save_one(LINKR_DEBUGGER_NET_SETTINGS_KEY, &subnet,
				      sizeof(subnet));
}
#else
uint8_t linkr_debugger_net_subnet_load(void)
{
	return LINKR_DEBUGGER_NET_SUBNET_DEFAULT;
}

int linkr_debugger_net_subnet_save(uint8_t subnet)
{
	return linkr_debugger_net_subnet_valid(subnet) ? 0 : -EINVAL;
}
#endif
