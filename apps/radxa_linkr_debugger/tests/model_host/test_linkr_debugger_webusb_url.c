/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_webusb_url.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

static struct linkr_debugger_net_identity valid_identity(uint32_t network)
{
	struct linkr_debugger_net_identity identity = {
		.mac = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 },
		.network = network,
		.origin = LINKR_DEBUGGER_NET_ORIGIN_MANUAL,
	};

	return identity;
}

static void assert_url_descriptor(uint32_t network, const char *expected_url)
{
	struct linkr_debugger_net_identity identity = valid_identity(network);
	uint8_t descriptor[LINKR_DEBUGGER_WEBUSB_URL_DESCRIPTOR_MAX_LEN];
	size_t descriptor_len = 0U;
	size_t url_len = strlen(expected_url);

	memset(descriptor, 0xA5, sizeof(descriptor));
	assert(linkr_debugger_webusb_url_encode(&identity, descriptor,
						 sizeof(descriptor), &descriptor_len) == 0);
	assert(descriptor_len == url_len + 3U);
	assert(descriptor[0] == descriptor_len);
	assert(descriptor[1] == 0x03U);
	assert(descriptor[2] == 0x01U);
	assert(memcmp(&descriptor[3], expected_url, url_len) == 0);
	assert(memcmp(&descriptor[3], "https://", strlen("https://")) != 0);
}

static void test_rfc1918_urls(void)
{
	assert_url_descriptor(0x0A000000U,
			      "xzl01.github.io/agent-debugboard/connect.html#device=10.0.0.1");
	assert_url_descriptor(0xAC100000U,
			      "xzl01.github.io/agent-debugboard/connect.html#device=172.16.0.1");
	assert_url_descriptor(0xC0A8FFFCU,
			      "xzl01.github.io/agent-debugboard/connect.html#device=192.168.255.253");
}

static void test_capacity_boundary(void)
{
	struct linkr_debugger_net_identity identity = valid_identity(0xC0A8FFFCU);
	uint8_t descriptor[LINKR_DEBUGGER_WEBUSB_URL_DESCRIPTOR_MAX_LEN];
	size_t descriptor_len = 123U;

	memset(descriptor, 0xA5, sizeof(descriptor));
	assert(linkr_debugger_webusb_url_encode(&identity, descriptor,
						 sizeof(descriptor) - 1U,
						 &descriptor_len) == -ENOSPC);
	assert(descriptor_len == 0U);
	for (size_t i = 0U; i < sizeof(descriptor); i++) {
		assert(descriptor[i] == 0xA5U);
	}

	assert(linkr_debugger_webusb_url_encode(&identity, descriptor,
						 sizeof(descriptor), &descriptor_len) == 0);
	assert(descriptor_len == sizeof(descriptor));
}

static void test_invalid_arguments(void)
{
	struct linkr_debugger_net_identity identity = valid_identity(0x0A000000U);
	uint8_t descriptor[LINKR_DEBUGGER_WEBUSB_URL_DESCRIPTOR_MAX_LEN];
	size_t descriptor_len = 123U;

	assert(linkr_debugger_webusb_url_encode(NULL, descriptor, sizeof(descriptor),
						 &descriptor_len) == -EINVAL);
	assert(descriptor_len == 0U);
	assert(linkr_debugger_webusb_url_encode(&identity, NULL, sizeof(descriptor),
						 &descriptor_len) == -EINVAL);
	assert(linkr_debugger_webusb_url_encode(&identity, descriptor, sizeof(descriptor),
						 NULL) == -EINVAL);

	identity.network = 0x08080808U;
	assert(linkr_debugger_webusb_url_encode(&identity, descriptor, sizeof(descriptor),
						 &descriptor_len) == -EINVAL);
	identity = valid_identity(0x0A000000U);
	identity.mac[0] = 0x00U;
	assert(linkr_debugger_webusb_url_encode(&identity, descriptor, sizeof(descriptor),
						 &descriptor_len) == -EINVAL);
	identity = valid_identity(0x0A000000U);
	identity.origin = (enum linkr_debugger_net_origin)99;
	assert(linkr_debugger_webusb_url_encode(&identity, descriptor, sizeof(descriptor),
						 &descriptor_len) == -EINVAL);
}

int main(void)
{
	test_rfc1918_urls();
	test_capacity_boundary();
	test_invalid_arguments();
	return 0;
}
