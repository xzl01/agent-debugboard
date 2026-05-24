/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "debugboard_model.h"

static void assert_str_eq(const char *actual, const char *expected)
{
	assert(strcmp(actual, expected) == 0);
}

static void test_rail_table_matches_schematic(void)
{
	const struct debugboard_rail_desc *rail;

	assert(debugboard_rail_count == 4);

	rail = debugboard_find_rail("12v_out");
	assert(rail != NULL);
	assert(rail->pin == 2);
	assert(rail->controllable);
	assert_str_eq(rail->signal, "GP02_12V_EN");

	rail = debugboard_find_rail("GP09_5V_WS_EN");
	assert(rail != NULL);
	assert_str_eq(rail->name, "5v_ws");
	assert(rail->pin == 9);

	rail = debugboard_find_rail("20v_out");
	assert(rail != NULL);
	assert(rail->pin == 10);
	assert_str_eq(rail->signal, "GP10_20V_EN");

	assert(debugboard_find_rail("5V_FIN") == NULL);
}

static void test_current_table(void)
{
	const struct debugboard_current_desc *current;
	const struct debugboard_current_desc *five_volt;

	assert(debugboard_current_count == 3);

	five_volt = debugboard_find_current("5v_out");
	assert(five_volt != NULL);
	assert_str_eq(five_volt->signal, "S_C_5V");
	assert_str_eq(five_volt->sensor, "INA139");
	assert(five_volt->adc_index == 0);

	current = debugboard_find_current("S_C_12V");
	assert(current != NULL);
	assert_str_eq(current->name, "12v_out");
	assert_str_eq(current->sensor, "INA139");
	assert(current->adc_index == 1);

	for (size_t i = 0; i < debugboard_current_count; i++) {
		assert_str_eq(debugboard_currents[i].sensor, "INA139");
	}

	assert(debugboard_find_current("5V_FIN") == NULL);
}

static void test_safe_gpio_allowlist(void)
{
	const struct debugboard_safe_gpio_desc *gpio;
	char name[DEBUGBOARD_GPIO_NAME_BUFSZ];

	assert(debugboard_safe_gpio_count == 15);

	gpio = debugboard_find_safe_gpio_by_pin(4);
	assert(gpio != NULL);
	assert(gpio->pin == 4);
	assert_str_eq(gpio->note, "CON_MAS");

	gpio = debugboard_find_safe_gpio_by_pin(13);
	assert(gpio != NULL);
	assert(gpio->pin == 13);
	assert_str_eq(gpio->note, "J17_PIN1");
	assert(debugboard_format_gpio_name(gpio->pin, name, sizeof(name)));
	assert_str_eq(name, "GP13");

	gpio = debugboard_find_safe_gpio_by_pin(24);
	assert(gpio != NULL);
	assert(gpio->pin == 24);
	assert_str_eq(gpio->note, "J17_PIN12");
	assert(debugboard_format_gpio_name(gpio->pin, name, sizeof(name)));
	assert_str_eq(name, "GP24");

	gpio = debugboard_find_safe_gpio_by_pin(16);
	assert(gpio != NULL);
	assert(gpio->pin == 16);
	assert_str_eq(gpio->note, "J17_PIN7");

	gpio = debugboard_find_safe_gpio_by_pin(21);
	assert(gpio != NULL);
	assert(gpio->pin == 21);
	assert_str_eq(gpio->note, "J17_PIN6");

	assert(debugboard_find_safe_gpio_by_pin(10) == NULL);
	assert(debugboard_find_safe_gpio_by_pin(26) == NULL);

	gpio = debugboard_find_safe_gpio_by_identifier("GP4");
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "CON_MAS");

	gpio = debugboard_find_safe_gpio_by_identifier("4");
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "CON_MAS");

	gpio = debugboard_find_safe_gpio_by_identifier("CON_MAS");
	assert(gpio != NULL);
	assert(gpio->pin == 4);

	gpio = debugboard_find_safe_gpio_by_identifier("J17_PIN1");
	assert(gpio != NULL);
	assert(gpio->pin == 13);

	assert(debugboard_find_safe_gpio_by_identifier("not-a-gpio") == NULL);
}

static void test_gpio_name_formatter(void)
{
	char name[DEBUGBOARD_GPIO_NAME_BUFSZ];
	char too_short[4];

	assert(debugboard_format_gpio_name(0, name, sizeof(name)));
	assert_str_eq(name, "GP0");

	assert(debugboard_format_gpio_name(29, name, sizeof(name)));
	assert_str_eq(name, "GP29");

	assert(!debugboard_format_gpio_name(13, too_short, sizeof(too_short)));
	assert(!debugboard_format_gpio_name(13, NULL, sizeof(name)));
	assert(!debugboard_format_gpio_name(13, name, 0));
}

static void test_bool_parser(void)
{
	bool value = false;

	assert(debugboard_parse_bool_arg("on", &value));
	assert(value);

	assert(debugboard_parse_bool_arg("enabled", &value));
	assert(value);

	assert(debugboard_parse_bool_arg("0", &value));
	assert(!value);

	assert(debugboard_parse_bool_arg("disable", &value));
	assert(!value);

	assert(!debugboard_parse_bool_arg("true", &value));
	assert(!debugboard_parse_bool_arg(NULL, &value));
	assert(!debugboard_parse_bool_arg("on", NULL));
}

static void test_gpio_pin_parser(void)
{
	uint8_t pin = 0xff;

	assert(debugboard_parse_gpio_pin("GP13", &pin));
	assert(pin == 13);

	assert(debugboard_parse_gpio_pin("gp24", &pin));
	assert(pin == 24);

	assert(debugboard_parse_gpio_pin("0", &pin));
	assert(pin == 0);

	assert(debugboard_parse_gpio_pin("GP09", &pin));
	assert(pin == 9);

	assert(!debugboard_parse_gpio_pin("", &pin));
	assert(!debugboard_parse_gpio_pin("GP", &pin));
	assert(!debugboard_parse_gpio_pin("GP30", &pin));
	assert(!debugboard_parse_gpio_pin("-1", &pin));
	assert(!debugboard_parse_gpio_pin("GP13x", &pin));
	assert(!debugboard_parse_gpio_pin(NULL, &pin));
	assert(!debugboard_parse_gpio_pin("GP13", NULL));
}

int main(void)
{
	test_rail_table_matches_schematic();
	test_current_table();
	test_safe_gpio_allowlist();
	test_gpio_name_formatter();
	test_bool_parser();
	test_gpio_pin_parser();

	puts("debugboard_model: all tests passed");
	return 0;
}
