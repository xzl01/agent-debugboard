/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "linkr_debugger_model.h"

static void assert_str_eq(const char *actual, const char *expected)
{
	assert(strcmp(actual, expected) == 0);
}

static void test_rail_table_matches_schematic(void)
{
	const struct linkr_debugger_rail_desc *rail;
	const struct linkr_debugger_rail_desc *ws_rail;

	assert(linkr_debugger_rail_count == 4);

	rail = linkr_debugger_find_rail("12v_out");
	assert(rail != NULL);
	assert(rail->pin == 2);
	assert(rail->controllable);
	assert(!rail->always_on);
	assert_str_eq(rail->signal, "GP02_12V_EN");

	ws_rail = linkr_debugger_find_rail("GP09_5V_WS_EN");
	assert(ws_rail != NULL);
	assert_str_eq(ws_rail->name, "5v_ws");
#if defined(CONFIG_SOC_SERIES_RP2350)
	assert(ws_rail->pin == 1);
	assert(ws_rail->always_on);
#else
	assert(ws_rail->pin == 9);
	assert(!ws_rail->always_on);
#endif
	assert(ws_rail->controllable);

	rail = linkr_debugger_find_rail("20v_out");
	assert(rail != NULL);
#if defined(CONFIG_SOC_SERIES_RP2350)
	assert(rail->pin == 3);
#else
	assert(rail->pin == 10);
#endif
	assert_str_eq(rail->signal, "GP10_20V_EN");
	assert(!rail->always_on);

	assert(linkr_debugger_find_rail("5V_FIN") == NULL);
}

static void test_5v_ws_state_contract_matches_board_revision(void)
{
	const struct linkr_debugger_rail_desc *rail = linkr_debugger_find_rail("5v_ws");

	assert(rail != NULL);
	assert(linkr_debugger_rail_state_allowed(rail, true));
#if defined(CONFIG_SOC_SERIES_RP2350)
	assert(linkr_debugger_rail_initial_enabled(rail));
	assert(!linkr_debugger_rail_state_allowed(rail, false));
#else
	assert(!linkr_debugger_rail_initial_enabled(rail));
	assert(linkr_debugger_rail_state_allowed(rail, false));
#endif
}

static void test_current_table(void)
{
	const struct linkr_debugger_current_desc *current;
	const struct linkr_debugger_current_desc *five_volt;

	assert(linkr_debugger_current_count == 3);

	five_volt = linkr_debugger_find_current("5v_out");
	assert(five_volt != NULL);
	assert_str_eq(five_volt->signal, "S_C_5V");
	assert_str_eq(five_volt->sensor, "INA139");
	assert(five_volt->adc_index == 0);

	current = linkr_debugger_find_current("S_C_12V");
	assert(current != NULL);
	assert_str_eq(current->name, "12v_out");
	assert_str_eq(current->sensor, "INA139");
	assert(current->adc_index == 1);

	for (size_t i = 0; i < linkr_debugger_current_count; i++) {
		assert_str_eq(linkr_debugger_currents[i].sensor, "INA139");
	}

	assert(linkr_debugger_find_current("5V_FIN") == NULL);
}

static void test_safe_gpio_allowlist(void)
{
	const struct linkr_debugger_safe_gpio_desc *gpio;
	char name[LINKR_DEBUGGER_GPIO_NAME_BUFSZ];

#if defined(CONFIG_SOC_SERIES_RP2350)
	assert(linkr_debugger_safe_gpio_count == 15);

	gpio = linkr_debugger_find_safe_gpio_by_pin(7);
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "CON_MAS");

	gpio = linkr_debugger_find_safe_gpio_by_pin(8);
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "CON_REST");

	gpio = linkr_debugger_find_safe_gpio_by_pin(9);
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "CON_USER");

	gpio = linkr_debugger_find_safe_gpio_by_pin(10);
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "J16_PIN1");

	gpio = linkr_debugger_find_safe_gpio_by_pin(15);
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "J16_PIN11");

	gpio = linkr_debugger_find_safe_gpio_by_pin(20);
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "J16_PIN10");

	gpio = linkr_debugger_find_safe_gpio_by_pin(29);
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "J16_PIN12");
	assert(linkr_debugger_format_gpio_name(gpio->pin, name, sizeof(name)));
	assert_str_eq(name, "GP29");

	assert(linkr_debugger_find_safe_gpio_by_pin(6) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(21) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(22) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(23) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(24) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(25) == NULL);

	gpio = linkr_debugger_find_safe_gpio_by_identifier("GP7");
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "CON_MAS");

	gpio = linkr_debugger_find_safe_gpio_by_identifier("J16_PIN12");
	assert(gpio != NULL);
	assert(gpio->pin == 29);
#else
	assert(linkr_debugger_safe_gpio_count == 15);

	gpio = linkr_debugger_find_safe_gpio_by_pin(4);
	assert(gpio != NULL);
	assert(gpio->pin == 4);
	assert_str_eq(gpio->note, "CON_MAS");

	gpio = linkr_debugger_find_safe_gpio_by_pin(13);
	assert(gpio != NULL);
	assert(gpio->pin == 13);
	assert_str_eq(gpio->note, "J17_PIN1");
	assert(linkr_debugger_format_gpio_name(gpio->pin, name, sizeof(name)));
	assert_str_eq(name, "GP13");

	gpio = linkr_debugger_find_safe_gpio_by_pin(24);
	assert(gpio != NULL);
	assert(gpio->pin == 24);
	assert_str_eq(gpio->note, "J17_PIN12");
	assert(linkr_debugger_format_gpio_name(gpio->pin, name, sizeof(name)));
	assert_str_eq(name, "GP24");

	gpio = linkr_debugger_find_safe_gpio_by_pin(16);
	assert(gpio != NULL);
	assert(gpio->pin == 16);
	assert_str_eq(gpio->note, "J17_PIN7");

	gpio = linkr_debugger_find_safe_gpio_by_pin(21);
	assert(gpio != NULL);
	assert(gpio->pin == 21);
	assert_str_eq(gpio->note, "J17_PIN6");

	assert(linkr_debugger_find_safe_gpio_by_pin(10) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(26) == NULL);

	gpio = linkr_debugger_find_safe_gpio_by_identifier("GP4");
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "CON_MAS");

	gpio = linkr_debugger_find_safe_gpio_by_identifier("4");
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "CON_MAS");

	gpio = linkr_debugger_find_safe_gpio_by_identifier("CON_MAS");
	assert(gpio != NULL);
	assert(gpio->pin == 4);

	gpio = linkr_debugger_find_safe_gpio_by_identifier("J17_PIN1");
	assert(gpio != NULL);
	assert(gpio->pin == 13);
#endif

	assert(linkr_debugger_find_safe_gpio_by_identifier("not-a-gpio") == NULL);
}

static void test_gpio_name_formatter(void)
{
	char name[LINKR_DEBUGGER_GPIO_NAME_BUFSZ];
	char too_short[4];

	assert(linkr_debugger_format_gpio_name(0, name, sizeof(name)));
	assert_str_eq(name, "GP0");

	assert(linkr_debugger_format_gpio_name(29, name, sizeof(name)));
	assert_str_eq(name, "GP29");

	assert(!linkr_debugger_format_gpio_name(13, too_short, sizeof(too_short)));
	assert(!linkr_debugger_format_gpio_name(13, NULL, sizeof(name)));
	assert(!linkr_debugger_format_gpio_name(13, name, 0));
}

static void test_bool_parser(void)
{
	bool value = false;

	assert(linkr_debugger_parse_bool_arg("on", &value));
	assert(value);

	assert(linkr_debugger_parse_bool_arg("enabled", &value));
	assert(value);

	assert(linkr_debugger_parse_bool_arg("0", &value));
	assert(!value);

	assert(linkr_debugger_parse_bool_arg("disable", &value));
	assert(!value);

	assert(!linkr_debugger_parse_bool_arg("true", &value));
	assert(!linkr_debugger_parse_bool_arg(NULL, &value));
	assert(!linkr_debugger_parse_bool_arg("on", NULL));
}

static void test_gpio_pin_parser(void)
{
	uint8_t pin = 0xff;

	assert(linkr_debugger_parse_gpio_pin("GP13", &pin));
	assert(pin == 13);

	assert(linkr_debugger_parse_gpio_pin("gp24", &pin));
	assert(pin == 24);

	assert(linkr_debugger_parse_gpio_pin("0", &pin));
	assert(pin == 0);

	assert(linkr_debugger_parse_gpio_pin("GP09", &pin));
	assert(pin == 9);

	assert(!linkr_debugger_parse_gpio_pin("", &pin));
	assert(!linkr_debugger_parse_gpio_pin("GP", &pin));
	assert(!linkr_debugger_parse_gpio_pin("GP30", &pin));
	assert(!linkr_debugger_parse_gpio_pin("-1", &pin));
	assert(!linkr_debugger_parse_gpio_pin("GP13x", &pin));
	assert(!linkr_debugger_parse_gpio_pin(NULL, &pin));
	assert(!linkr_debugger_parse_gpio_pin("GP13", NULL));
}

static void test_vin_route_parser(void)
{
	enum linkr_debugger_vin_route route = LINKR_DEBUGGER_VIN_ROUTE_3V3;

	assert(linkr_debugger_parse_vin_route("1.8v", &route));
	assert(route == LINKR_DEBUGGER_VIN_ROUTE_1V8);
	assert_str_eq(linkr_debugger_vin_route_to_string(route), "1.8v");

	assert(linkr_debugger_parse_vin_route("3.3v", &route));
	assert(route == LINKR_DEBUGGER_VIN_ROUTE_3V3);
	assert_str_eq(linkr_debugger_vin_route_to_string(route), "3.3v");
	assert(linkr_debugger_vin_route_microvolt(route) == LINKR_DEBUGGER_VIN_3V3_UV);
	assert(linkr_debugger_vin_route_from_microvolt(LINKR_DEBUGGER_VIN_1V8_UV, &route));
	assert(route == LINKR_DEBUGGER_VIN_ROUTE_1V8);
	assert(linkr_debugger_vin_route_from_microvolt(LINKR_DEBUGGER_VIN_3V3_UV, &route));
	assert(route == LINKR_DEBUGGER_VIN_ROUTE_3V3);

	assert(!linkr_debugger_parse_vin_route("1v8", &route));
	assert(!linkr_debugger_parse_vin_route("3v3", &route));
	assert(!linkr_debugger_parse_vin_route(NULL, &route));
	assert(!linkr_debugger_parse_vin_route("1.8v", NULL));
	assert(!linkr_debugger_vin_route_from_microvolt(2500000, &route));
	assert(!linkr_debugger_vin_route_from_microvolt(LINKR_DEBUGGER_VIN_3V3_UV, NULL));
}

static void test_heartbeat_state(void)
{
	struct linkr_debugger_heartbeat_state state = {0};

	assert(!linkr_debugger_heartbeat_step(&state, true, 2));
	assert(!state.active);
	assert(state.ticks == 1);

	assert(linkr_debugger_heartbeat_step(&state, true, 2));
	assert(state.active);
	assert(state.ticks == 0);

	assert(linkr_debugger_heartbeat_step(&state, true, 2));
	assert(state.active);
	assert(state.ticks == 1);

	assert(!linkr_debugger_heartbeat_step(&state, true, 2));
	assert(!state.active);
	assert(state.ticks == 0);

	assert(!linkr_debugger_heartbeat_step(&state, true, 2));
	assert(!state.active);
	assert(state.ticks == 1);
	assert(!linkr_debugger_heartbeat_step(&state, false, 2));
	assert(!state.active);
	assert(state.ticks == 0);

	assert(!linkr_debugger_heartbeat_step(&state, true, 2));
	assert(!state.active);
	assert(state.ticks == 1);
	assert(linkr_debugger_heartbeat_step(&state, true, 2));
	assert(state.active);

	assert(!linkr_debugger_heartbeat_step(&state, true, 0));
	assert(!state.active);
	assert(state.ticks == 0);
	assert(!linkr_debugger_heartbeat_step(NULL, true, 2));
}

int main(void)
{
	test_rail_table_matches_schematic();
	test_5v_ws_state_contract_matches_board_revision();
	test_current_table();
	test_safe_gpio_allowlist();
	test_gpio_name_formatter();
	test_bool_parser();
	test_gpio_pin_parser();
	test_vin_route_parser();
	test_heartbeat_state();

	puts("linkr_debugger_model: all tests passed");
	return 0;
}
