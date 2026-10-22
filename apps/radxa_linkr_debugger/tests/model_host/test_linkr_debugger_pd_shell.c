/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>

#ifndef BIT
#define BIT(bit) (1UL << (bit))
#endif

/*
 * Exercise the real shell parser behind `pd`, including its rejection paths,
 * without pulling the unrelated Zephyr shell subsystem or firmware services
 * into the host build.
 */
#define RADXA_LINKR_DEBUGGER_BUILD_INFO_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_SHELL_H_
#define RADXA_LINKR_DEBUGGER_CONTROL_H_
#define RADXA_LINKR_DEBUGGER_NETWORK_H_
#define RADXA_LINKR_DEBUGGER_SHELL_H_
#define RADXA_LINKR_DEBUGGER_TASK_SHELL_H_

#include "linkr_debugger_model.h"
#include "linkr_debugger_net_settings.h"
#include "linkr_debugger_pd.h"

int linkr_debugger_bootloader_now(void);
void linkr_debugger_watchdog_note_cmdline_alive(void);
const char *linkr_debugger_mcu_name(void);
const char *linkr_debugger_tf_wp_route_name(void);
bool linkr_debugger_parse_tf_wp_route(const char *arg,
				      enum linkr_debugger_tf_wp_route *route);
int linkr_debugger_tf_wp_route_set(enum linkr_debugger_tf_wp_route route);
const char *linkr_debugger_build_sysname(void);
const char *linkr_debugger_build_nodename(void);
const char *linkr_debugger_build_release(void);
const char *linkr_debugger_build_version(void);
const char *linkr_debugger_build_machine(void);
const char *linkr_debugger_build_processor(void);
const char *linkr_debugger_build_platform(void);
int linkr_debugger_network_identity(struct linkr_debugger_net_identity *identity);
int linkr_debugger_net_format_mac(const uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN],
				  char *buf, size_t cap);
int linkr_debugger_net_format_network(uint32_t network, char *buf, size_t cap);
int linkr_debugger_net_format_ipv4(uint32_t network, uint8_t host,
				   char *buf, size_t cap);
int linkr_debugger_net_format_url(uint32_t network, char *buf, size_t cap);
int linkr_debugger_net_parse_mac(const char *text,
				 uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN]);
int linkr_debugger_net_parse_network(const char *text, uint32_t *network);
int linkr_debugger_network_set_identity_with_result(
	const struct linkr_debugger_net_identity *identity, bool *reconnect_required);
int linkr_debugger_config_shell_show(const struct shell *sh, size_t argc,
				     char **argv);
int linkr_debugger_config_shell_save(const struct shell *sh, size_t argc,
				     char **argv);
int linkr_debugger_config_shell_clear(const struct shell *sh, size_t argc,
				      char **argv);
int linkr_debugger_task_shell_show(const struct shell *sh, size_t argc,
				   char **argv);
int linkr_debugger_task_shell_clear(const struct shell *sh, size_t argc,
				    char **argv);

#include "../../src/linkr_debugger_shell.c"

#define OUTPUT_CAP 8U
#define OUTPUT_LINE_CAP 256U

enum output_kind {
	OUTPUT_PRINT,
	OUTPUT_ERROR,
};

struct fake_output {
	enum output_kind kinds[OUTPUT_CAP];
	char lines[OUTPUT_CAP][OUTPUT_LINE_CAP];
	size_t line_count;
	size_t print_calls;
	size_t error_calls;
};

static struct fake_output output;
static const struct shell test_shell;
static struct linkr_debugger_pd_status fake_pd_status;
static unsigned int pd_get_status_calls;
static int pd_get_status_result;
static unsigned int pd_set_fixed_calls;
static unsigned int pd_set_pps_calls;
static uint16_t pd_set_fixed_mv;
static uint16_t pd_set_pps_mv;

static void fake_output_write(enum output_kind kind, const char *format,
			      va_list arguments)
{
	int written;

	assert(output.line_count < OUTPUT_CAP);
	written = vsnprintf(output.lines[output.line_count], OUTPUT_LINE_CAP,
			    format, arguments);
	assert(written >= 0);
	assert((size_t)written < OUTPUT_LINE_CAP);
	output.kinds[output.line_count++] = kind;
	if (kind == OUTPUT_PRINT) {
		output.print_calls++;
	} else {
		output.error_calls++;
	}
}

void shell_print(const struct shell *sh, const char *format, ...)
{
	va_list arguments;

	(void)sh;
	va_start(arguments, format);
	fake_output_write(OUTPUT_PRINT, format, arguments);
	va_end(arguments);
}

void shell_error(const struct shell *sh, const char *format, ...)
{
	va_list arguments;

	(void)sh;
	va_start(arguments, format);
	fake_output_write(OUTPUT_ERROR, format, arguments);
	va_end(arguments);
}

void shell_fprintf(const struct shell *sh, int level, const char *format, ...)
{
	va_list arguments;

	(void)sh;
	(void)level;
	va_start(arguments, format);
	fake_output_write(OUTPUT_PRINT, format, arguments);
	va_end(arguments);
}

int linkr_debugger_reboot_schedule(void)
{
	return 0;
}

int linkr_debugger_pd_get_status(struct linkr_debugger_pd_status *status)
{
	pd_get_status_calls++;
	*status = fake_pd_status;
	return pd_get_status_result;
}

int linkr_debugger_pd_set_fixed(uint16_t voltage_mv)
{
	pd_set_fixed_calls++;
	pd_set_fixed_mv = voltage_mv;
	fake_pd_status.voltage_mv = voltage_mv;
	return 0;
}

int linkr_debugger_pd_set_pps(uint16_t voltage_mv)
{
	pd_set_pps_calls++;
	pd_set_pps_mv = voltage_mv;
	return 0;
}

int linkr_debugger_bootloader_now(void)
{
	return 0;
}

void linkr_debugger_watchdog_note_cmdline_alive(void)
{
}

const char *linkr_debugger_mcu_name(void)
{
	return "pd-shell-test";
}

const char *linkr_debugger_tf_wp_route_name(void)
{
	return "writable";
}

bool linkr_debugger_parse_tf_wp_route(const char *arg,
				      enum linkr_debugger_tf_wp_route *route)
{
	(void)arg;
	(void)route;
	return false;
}

int linkr_debugger_tf_wp_route_set(enum linkr_debugger_tf_wp_route route)
{
	(void)route;
	return 0;
}

const char *linkr_debugger_build_sysname(void)
{
	return "Zephyr";
}

const char *linkr_debugger_build_nodename(void)
{
	return "pd-shell-test";
}

const char *linkr_debugger_build_release(void)
{
	return "test";
}

const char *linkr_debugger_build_version(void)
{
	return "test";
}

const char *linkr_debugger_build_machine(void)
{
	return "test";
}

const char *linkr_debugger_build_processor(void)
{
	return "test";
}

const char *linkr_debugger_build_platform(void)
{
	return "test";
}

int linkr_debugger_network_identity(struct linkr_debugger_net_identity *identity)
{
	(void)identity;
	return -ENODEV;
}

int linkr_debugger_net_format_mac(const uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN],
				  char *buf, size_t cap)
{
	(void)mac;
	(void)buf;
	(void)cap;
	return -ENODEV;
}

int linkr_debugger_net_format_network(uint32_t network, char *buf, size_t cap)
{
	(void)network;
	(void)buf;
	(void)cap;
	return -ENODEV;
}

int linkr_debugger_net_format_ipv4(uint32_t network, uint8_t host,
				   char *buf, size_t cap)
{
	(void)network;
	(void)host;
	(void)buf;
	(void)cap;
	return -ENODEV;
}

int linkr_debugger_net_format_url(uint32_t network, char *buf, size_t cap)
{
	(void)network;
	(void)buf;
	(void)cap;
	return -ENODEV;
}

int linkr_debugger_net_parse_mac(const char *text,
				 uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN])
{
	(void)text;
	(void)mac;
	return -EINVAL;
}

int linkr_debugger_net_parse_network(const char *text, uint32_t *network)
{
	(void)text;
	(void)network;
	return -EINVAL;
}

int linkr_debugger_network_set_identity_with_result(
	const struct linkr_debugger_net_identity *identity, bool *reconnect_required)
{
	(void)identity;
	if (reconnect_required != NULL) {
		*reconnect_required = false;
	}
	return -ENODEV;
}

int linkr_debugger_config_shell_show(const struct shell *sh, size_t argc,
				     char **argv)
{
	(void)sh;
	(void)argc;
	(void)argv;
	return 0;
}

int linkr_debugger_config_shell_save(const struct shell *sh, size_t argc,
				     char **argv)
{
	(void)sh;
	(void)argc;
	(void)argv;
	return 0;
}

int linkr_debugger_config_shell_clear(const struct shell *sh, size_t argc,
				      char **argv)
{
	(void)sh;
	(void)argc;
	(void)argv;
	return 0;
}

int linkr_debugger_task_shell_show(const struct shell *sh, size_t argc,
				   char **argv)
{
	(void)sh;
	(void)argc;
	(void)argv;
	return 0;
}

int linkr_debugger_task_shell_clear(const struct shell *sh, size_t argc,
				    char **argv)
{
	(void)sh;
	(void)argc;
	(void)argv;
	return 0;
}

static void reset_fixture(void)
{
	memset(&output, 0, sizeof(output));
	memset(&fake_pd_status, 0, sizeof(fake_pd_status));
	fake_pd_status.available = true;
	fake_pd_status.mode = LINKR_DEBUGGER_PD_MODE_FIXED;
	fake_pd_status.protocol = LINKR_DEBUGGER_PD_PROTOCOL_PD;
	fake_pd_status.pd_active = true;
	fake_pd_status.voltage_mv = 5000U;
	fake_pd_status.gear = 0U;
	pd_get_status_calls = 0U;
	pd_get_status_result = 0;
	pd_set_fixed_calls = 0U;
	pd_set_pps_calls = 0U;
	pd_set_fixed_mv = 0U;
	pd_set_pps_mv = 0U;
}

static int run_pd(const char *value, const char *voltage)
{
	char *argv[3] = { "pd", (char *)value, (char *)voltage };
	size_t argc = value == NULL ? 1U : voltage == NULL ? 2U : 3U;

	return cmd_pd(&test_shell, argc, argv);
}

static void assert_error_output(void)
{
	assert(output.print_calls == 0U);
	assert(output.error_calls == 1U);
	assert(output.line_count == 1U);
	assert(output.kinds[0] == OUTPUT_ERROR);
	assert(strstr(output.lines[0],
		      "usage: pd [status] | pd fixed <mv> | pd pps <mv>") != NULL);
}

static void assert_rejected(const char *value, const char *voltage)
{
	reset_fixture();
	assert(run_pd(value, voltage) == -EINVAL);
	assert(pd_set_fixed_calls == 0U);
	assert(pd_set_pps_calls == 0U);
	assert(pd_get_status_calls == 0U);
	assert_error_output();
}

static void assert_accepted(const char *value, const char *voltage,
			    uint16_t expected_mv)
{
	reset_fixture();
	assert(run_pd(value, voltage) == 0);
	assert(pd_set_fixed_calls == 1U);
	assert(pd_set_pps_calls == 0U);
	assert(pd_set_fixed_mv == expected_mv);
	assert(pd_get_status_calls == 1U);
	assert(output.error_calls == 0U);
	assert(output.print_calls == 1U);
	assert(strstr(output.lines[0], "voltage_mv=20000") != NULL);
}

static void test_pd_rejects_voltage_above_board_limit_before_setter(void)
{
	assert_rejected("fixed", "20001");
	assert_rejected("fixed", "85536");
	assert_rejected("fixed", "999999999999999999999");
}

static void test_pd_rejects_negative_and_malformed_voltage_before_setter(void)
{
	assert_rejected("fixed", "-45536");
	assert_rejected("fixed", "20x");
	assert_rejected("fixed", "");
}

static void test_pd_accepts_shorthand_and_explicit_20v(void)
{
	assert_accepted("20", NULL, 20000U);
	assert_accepted("fixed", "20", 20000U);
	assert_accepted("fixed", "20000", 20000U);
	assert_accepted("20000", NULL, 20000U);
}

int main(void)
{
	test_pd_rejects_voltage_above_board_limit_before_setter();
	test_pd_rejects_negative_and_malformed_voltage_before_setter();
	test_pd_accepts_shorthand_and_explicit_20v();
	return 0;
}
