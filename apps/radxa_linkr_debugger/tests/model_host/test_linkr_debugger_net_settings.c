/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_net_settings.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static void test_subnet_validity(void)
{
	assert(!linkr_debugger_net_subnet_valid(0U));
	assert(linkr_debugger_net_subnet_valid(1U));
	assert(linkr_debugger_net_subnet_valid(42U));
	assert(linkr_debugger_net_subnet_valid(203U));
	assert(linkr_debugger_net_subnet_valid(254U));
	assert(!linkr_debugger_net_subnet_valid(255U));
}

static void test_parse_shell_subnet(void)
{
	uint8_t subnet = 0U;

	assert(linkr_debugger_net_parse_subnet("1", &subnet) == 0);
	assert(subnet == 1U);
	assert(linkr_debugger_net_parse_subnet("204", &subnet) == 0);
	assert(subnet == 204U);
	assert(linkr_debugger_net_parse_subnet("254", &subnet) == 0);
	assert(subnet == 254U);

	assert(linkr_debugger_net_parse_subnet("0", &subnet) == -EINVAL);
	assert(linkr_debugger_net_parse_subnet("255", &subnet) == -EINVAL);
	assert(linkr_debugger_net_parse_subnet("-1", &subnet) == -EINVAL);
	assert(linkr_debugger_net_parse_subnet("-18446744073709551616", &subnet) == -EINVAL);
	assert(linkr_debugger_net_parse_subnet("+1", &subnet) == -EINVAL);
	assert(linkr_debugger_net_parse_subnet(" 1", &subnet) == -EINVAL);
	assert(linkr_debugger_net_parse_subnet("1 ", &subnet) == -EINVAL);
	assert(linkr_debugger_net_parse_subnet("1\t", &subnet) == -EINVAL);
	assert(linkr_debugger_net_parse_subnet("1.5", &subnet) == -EINVAL);
	assert(linkr_debugger_net_parse_subnet("abc", &subnet) == -EINVAL);
	assert(linkr_debugger_net_parse_subnet("", &subnet) == -EINVAL);
	assert(linkr_debugger_net_parse_subnet(NULL, &subnet) == -EINVAL);
	assert(linkr_debugger_net_parse_subnet("203", NULL) == -EINVAL);
}

static void test_format_ipv4(void)
{
	char buf[16];

	assert(linkr_debugger_net_format_ipv4(203U, 1U, buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "172.29.203.1") == 0);

	assert(linkr_debugger_net_format_ipv4(1U, 10U, buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "172.29.1.10") == 0);

	assert(linkr_debugger_net_format_ipv4(254U, 254U, buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "172.29.254.254") == 0);
}

static void test_format_ipv4_rejects_invalid_input(void)
{
	char buf[16] = "unchanged";

	assert(linkr_debugger_net_format_ipv4(0U, 1U, buf, sizeof(buf)) == -EINVAL);
	assert(strcmp(buf, "unchanged") == 0);
	assert(linkr_debugger_net_format_ipv4(255U, 1U, buf, sizeof(buf)) == -EINVAL);
	assert(linkr_debugger_net_format_ipv4(203U, 1U, NULL, sizeof(buf)) == -EINVAL);
	assert(linkr_debugger_net_format_ipv4(203U, 1U, buf, 0U) == -EINVAL);

	assert(linkr_debugger_net_format_ipv4(203U, 1U, buf, 8U) == -ENOSPC);
}

static void test_format_captive_uri(void)
{
	char buf[48];

	assert(linkr_debugger_net_format_captive_uri(203U, buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "http://172.29.203.1/") == 0);

	assert(linkr_debugger_net_format_captive_uri(42U, buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "http://172.29.42.1/") == 0);

	assert(linkr_debugger_net_format_captive_uri(0U, buf, sizeof(buf)) == -EINVAL);
	assert(linkr_debugger_net_format_captive_uri(203U, buf, 8U) == -ENOSPC);
}

int main(void)
{
	test_subnet_validity();
	test_parse_shell_subnet();
	test_format_ipv4();
	test_format_ipv4_rejects_invalid_input();
	test_format_captive_uri();
	return 0;
}
