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

#include "linkr_debugger_control.h"
#include "linkr_debugger_power_dependency.h"

static void assert_str_eq(const char *actual, const char *expected)
{
	assert(strcmp(actual, expected) == 0);
}

static void test_rail_table_matches_schematic(void)
{
	const struct linkr_debugger_rail_desc *rail;
	const struct linkr_debugger_rail_desc *vdd_rail;

	assert(linkr_debugger_rail_count == 5);

	rail = linkr_debugger_find_rail("12v_out");
	assert(rail != NULL);
	assert(rail->pin == 2);
	assert(rail->controllable);
	assert(!rail->always_on);
	assert_str_eq(rail->signal, "GP02_12V_EN");

	vdd_rail = linkr_debugger_find_rail("GP01_5V_WS_EN");
	assert(vdd_rail != NULL);
	assert_str_eq(vdd_rail->name, "vdd_5v");
	assert(vdd_rail->pin == 1);
	assert(!vdd_rail->always_on);
	assert(vdd_rail->controllable);

	rail = linkr_debugger_find_rail("20v_out");
	assert(rail != NULL);
	assert(rail->pin == 3);
	assert_str_eq(rail->signal, "GP03_20V_EN");
	assert(!rail->always_on);

	assert(linkr_debugger_find_rail("5V_FIN") == NULL);

	rail = linkr_debugger_find_rail("5v_usb_hub");
	assert(rail != NULL);
	assert(rail->pin == 24U);
	assert(rail->controllable);
	assert(!rail->always_on);
	assert_str_eq(rail->signal, "GPIO24_USB_CRL");
	assert_str_eq(rail->parent, "vdd_5v");
}

static void test_vdd_5v_state_contract_matches_board_revision(void)
{
	const struct linkr_debugger_rail_desc *rail = linkr_debugger_find_rail("vdd_5v");

	assert(rail != NULL);
	assert(linkr_debugger_rail_state_allowed(rail, true));
	assert(linkr_debugger_rail_state_allowed(rail, false));
	assert(!linkr_debugger_rail_initial_enabled(rail));
}

static void test_current_table(void)
{
	const struct linkr_debugger_current_desc *current;
	const struct linkr_debugger_current_desc *five_volt;

	assert(linkr_debugger_current_count == 4);
	assert(LINKR_DEBUGGER_CURRENT_SENSOR_COUNT == 4U);

	five_volt = linkr_debugger_find_current("5v_out");
	assert(five_volt != NULL);
	assert_str_eq(five_volt->signal, "INA226_5V");
	assert_str_eq(five_volt->sensor, "INA226");
	assert(five_volt->kind == LINKR_DEBUGGER_ADC_KIND_CURRENT);
	assert_str_eq(five_volt->unit, "uA");
	assert(five_volt->sensor_index == 0);

	current = linkr_debugger_find_current("INA226_12V");
	assert(current != NULL);
	assert_str_eq(current->name, "12v_out");
	assert_str_eq(current->sensor, "INA226");
	assert(current->kind == LINKR_DEBUGGER_ADC_KIND_CURRENT);
	assert_str_eq(current->unit, "uA");
	assert(current->sensor_index == 1);

	for (size_t i = 0; i < linkr_debugger_current_count; i++) {
		current = &linkr_debugger_currents[i];
		assert(current->kind == LINKR_DEBUGGER_ADC_KIND_CURRENT);
		assert_str_eq(current->sensor, "INA226");
		assert_str_eq(current->unit, "uA");
		assert(current->sensor_index == i);
	}
	assert(linkr_debugger_find_current("adc3") == NULL);

	assert(linkr_debugger_find_current("5V_FIN") == NULL);
}

static void test_adc_table(void)
{
	const struct linkr_debugger_current_desc *adc;
	const struct linkr_debugger_current_desc *adc0;
	const struct linkr_debugger_current_desc *adc3;
	size_t current_descriptor_count = 0U;
	static const char *const expected_names[] = {
		"5v_out", "12v_out", "20v_out", "vdd_5v",
		"adc0", "adc1", "adc2", "adc3",
	};
	static const char *const expected_adc_signals[] = {
		"GPIO26_ADC0", "GPIO27_ADC1", "GPIO28_ADC2", "GPIO29_ADC3",
	};

	assert(linkr_debugger_adc_count == LINKR_DEBUGGER_ADC_TELEMETRY_CHANNEL_COUNT);
	assert(linkr_debugger_adc_count == 8U);
	for (size_t i = 0; i < linkr_debugger_adc_count; i++) {
		adc = &linkr_debugger_currents[i];
		assert_str_eq(adc->name, expected_names[i]);
		switch (adc->kind) {
		case LINKR_DEBUGGER_ADC_KIND_CURRENT:
			assert_str_eq(adc->sensor, "INA226");
			assert_str_eq(adc->unit, "uA");
			current_descriptor_count++;
			break;
		case LINKR_DEBUGGER_ADC_KIND_VOLTAGE:
			assert_str_eq(adc->sensor, "ADC");
			assert_str_eq(adc->unit, "uV");
			assert(adc->adc_index == i - LINKR_DEBUGGER_CURRENT_SENSOR_COUNT);
			assert_str_eq(adc->signal,
				expected_adc_signals[i - LINKR_DEBUGGER_CURRENT_SENSOR_COUNT]);
			break;
		default:
			assert(false);
		}
	}
	assert(current_descriptor_count == LINKR_DEBUGGER_CURRENT_SENSOR_COUNT);
	adc0 = linkr_debugger_find_adc("adc0");
	assert(adc0 != NULL);
	assert_str_eq(adc0->signal, "GPIO26_ADC0");
	assert_str_eq(adc0->note, "PD input divider; ADC pad voltage");
	assert(adc0->adc_index == 0U);
	assert(linkr_debugger_find_current("adc0") == NULL);

	adc3 = linkr_debugger_find_adc("adc3");
	assert(adc3 != NULL);
	assert_str_eq(adc3->signal, "GPIO29_ADC3");
	assert_str_eq(adc3->sensor, "ADC");
	assert(adc3->kind == LINKR_DEBUGGER_ADC_KIND_VOLTAGE);
	assert_str_eq(adc3->unit, "uV");
	assert(adc3->adc_index == 3);
	assert(adc3->note == NULL);

	assert(linkr_debugger_find_adc("5V_FIN") == NULL);
	assert(linkr_debugger_find_adc("5v_usb_hub") == NULL);
	assert(linkr_debugger_find_current("5v_usb_hub") == NULL);
}

static void test_adc_microvolt_conversion_fallback(void)
{
	int32_t microvolts = 1234567;

	assert(linkr_debugger_adc_resolve_microvolts(0, 2048, 12U,
						       &microvolts) == 0);
	assert(microvolts == 1234567);

	assert(linkr_debugger_adc_resolve_microvolts(-ENOTSUP, 2048, 12U,
						       &microvolts) == 0);
	assert(microvolts == 1650402);
	assert(linkr_debugger_adc_resolve_microvolts(-ENOTSUP, 4095, 12U,
						       &microvolts) == 0);
	assert(microvolts == 3300000);
	assert(linkr_debugger_adc_resolve_microvolts(-ENOTSUP, -2048, 12U,
						       &microvolts) == 0);
	assert(microvolts == -1650402);

	assert(linkr_debugger_adc_resolve_microvolts(-ENOTSUP, INT32_MAX, 1U,
						       &microvolts) == 0);
	assert(microvolts == INT32_MAX);
	assert(linkr_debugger_adc_resolve_microvolts(-ENOTSUP, INT32_MIN, 1U,
						       &microvolts) == 0);
	assert(microvolts == INT32_MIN);
	assert(linkr_debugger_adc_resolve_microvolts(-ENOTSUP, 2048, 33U,
						       &microvolts) == -ERANGE);
	assert(linkr_debugger_adc_resolve_microvolts(-ENOTSUP, 2048, 12U,
						       NULL) == -EINVAL);
}

static void test_safe_gpio_allowlist(void)
{
	const struct linkr_debugger_safe_gpio_desc *gpio;
	static const uint8_t expected_order[] = {
		29, 5, 16, 6, 15, 14, 13, 12, 9, 8, 7,
	};
	static const struct {
		uint8_t pin;
		const char *note;
		const char *group;
		uint8_t row;
		uint8_t column;
	} expected_layout[] = {
		{ 12, "J8_PIN1", "J8", 9, 0 },
		{ 13, "J8_PIN2", "J8", 8, 0 },
		{ 14, "J8_PIN3", "J8", 7, 0 },
		{ 15, "J8_PIN4", "J8", 6, 0 },
		{ 16, "J8_PIN5", "J8", 5, 0 },
		{ 5, "J8_PIN16", "J8", 4, 1 },
		{ 6, "J8_PIN15", "J8", 5, 1 },
		{ 29, "J8_PIN6", "J8", 4, 0 },
		{ 9, "CON_USER", "J13", 0, 0 },
		{ 8, "CON_REST", "J13", 0, 1 },
		{ 7, "CON_MAS", "J13", 1, 0 },
	};
	char name[LINKR_DEBUGGER_GPIO_NAME_BUFSZ];

	assert(linkr_debugger_gpio_layout_group_count == 2U);
	assert_str_eq(linkr_debugger_gpio_layout_groups[0].name, "J8");
	assert_str_eq(linkr_debugger_gpio_layout_groups[0].edge, "right");
	assert(linkr_debugger_gpio_layout_groups[0].order == 0U);
	assert(linkr_debugger_gpio_layout_groups[0].rows == 10U);
	assert(linkr_debugger_gpio_layout_groups[0].columns == 2U);
	assert_str_eq(linkr_debugger_gpio_layout_groups[1].name, "J13");
	assert_str_eq(linkr_debugger_gpio_layout_groups[1].edge, "right");
	assert(linkr_debugger_gpio_layout_groups[1].order == 1U);
	assert(linkr_debugger_gpio_layout_groups[1].rows == 2U);
	assert(linkr_debugger_gpio_layout_groups[1].columns == 2U);

	assert(linkr_debugger_safe_gpio_count == 11);
	for (size_t i = 0; i < linkr_debugger_safe_gpio_count; i++) {
		assert(linkr_debugger_safe_gpios[i].pin == expected_order[i]);
		assert(linkr_debugger_safe_gpios[i].output_capable ==
		       (linkr_debugger_safe_gpios[i].pin != 29U));
	}
	for (size_t i = 0; i < sizeof(expected_layout) / sizeof(expected_layout[0]); i++) {
		gpio = linkr_debugger_find_safe_gpio_by_pin(expected_layout[i].pin);
		assert(gpio != NULL);
		assert_str_eq(gpio->note, expected_layout[i].note);
		assert_str_eq(gpio->layout_group, expected_layout[i].group);
		assert(gpio->layout_row == expected_layout[i].row);
		assert(gpio->layout_column == expected_layout[i].column);
	}

	gpio = linkr_debugger_find_safe_gpio_by_pin(7);
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "CON_MAS");
	assert_str_eq(gpio->layout_group, "J13");
	assert_str_eq(gpio->layout_label, "GP7 / MASKROM");
	assert(gpio->layout_row == 1);
	assert(gpio->layout_column == 0);

	gpio = linkr_debugger_find_safe_gpio_by_pin(8);
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "CON_REST");
	assert_str_eq(gpio->layout_label, "GP8 / RSET");
	assert(gpio->layout_row == 0);
	assert(gpio->layout_column == 1);

	gpio = linkr_debugger_find_safe_gpio_by_pin(9);
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "CON_USER");
	assert_str_eq(gpio->layout_label, "GP9 / USER");
	assert(gpio->layout_row == 0);
	assert(gpio->layout_column == 0);

	assert(linkr_debugger_find_safe_gpio_by_pin(10) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(11) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(19) == NULL);

	assert(linkr_debugger_find_safe_gpio_by_pin(20) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(21) == NULL);

	gpio = linkr_debugger_find_safe_gpio_by_pin(5);
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "J8_PIN16");
	assert_str_eq(gpio->layout_label, "GP5");
	assert(gpio->layout_row == 4);
	assert(gpio->layout_column == 1);
	assert(gpio->output_capable);

	gpio = linkr_debugger_find_safe_gpio_by_pin(6);
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "J8_PIN15");
	assert_str_eq(gpio->layout_label, "GP6");
	assert(gpio->layout_row == 5);
	assert(gpio->layout_column == 1);
	assert(gpio->output_capable);

	gpio = linkr_debugger_find_safe_gpio_by_pin(15);
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "J8_PIN4");
	assert(gpio->layout_row == 6);
	assert(gpio->layout_column == 0);

	gpio = linkr_debugger_find_safe_gpio_by_pin(29);
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "J8_PIN6");
	assert_str_eq(gpio->layout_label, "GP29 / ADC3");
	assert(gpio->layout_row == 4);
	assert(gpio->layout_column == 0);
	assert(!gpio->output_capable);
	assert(linkr_debugger_format_gpio_name(gpio->pin, name, sizeof(name)));
	assert_str_eq(name, "GP29");

	assert(linkr_debugger_find_safe_gpio_by_pin(17) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(18) == NULL);

	assert(linkr_debugger_find_safe_gpio_by_pin(22) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(23) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(24) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(25) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(26) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(27) == NULL);
	assert(linkr_debugger_find_safe_gpio_by_pin(28) == NULL);

	gpio = linkr_debugger_find_safe_gpio_by_identifier("GP7");
	assert(gpio != NULL);
	assert_str_eq(gpio->note, "CON_MAS");

	gpio = linkr_debugger_find_safe_gpio_by_identifier("J8_PIN6");
	assert(gpio != NULL);
	assert(gpio->pin == 29);
	assert(linkr_debugger_find_safe_gpio_by_identifier("J8_PIN12") == NULL);
	gpio = linkr_debugger_find_safe_gpio_by_identifier("J8_PIN16");
	assert(gpio != NULL && gpio->pin == 5U);
	gpio = linkr_debugger_find_safe_gpio_by_identifier("J8_PIN15");
	assert(gpio != NULL && gpio->pin == 6U);

	assert(linkr_debugger_find_safe_gpio_by_identifier("not-a-gpio") == NULL);
}

static void test_full_gpio_layout_catalog(void)
{
	const struct linkr_debugger_gpio_layout_cell_desc *cell;
	size_t controllable = 0U;
	size_t output_capable = 0U;

	assert_str_eq(linkr_debugger_gpio_layout_view, "component-top");
	assert(linkr_debugger_gpio_layout_cell_count == 24U);
	for (size_t i = 0U; i < linkr_debugger_gpio_layout_cell_count; i++) {
		cell = &linkr_debugger_gpio_layout_cells[i];
		if (cell->controllable) {
			controllable++;
			assert(cell->gpio_pin <= 29U);
		}
		if (cell->output_capable) {
			output_capable++;
		}
		assert(!cell->output_capable || cell->controllable);
	}
	assert(controllable == 11U);
	assert(output_capable == 10U);
	assert(linkr_debugger_gpio_layout_cell_count - controllable == 13U);

	cell = &linkr_debugger_gpio_layout_cells[0];
	assert(cell->group_order == 0U);
	assert(cell->pin_number == 10U);
	assert(cell->row == 0U && cell->column == 0U);
	assert_str_eq(cell->label, "GP21 / SDA");
	assert_str_eq(cell->signal, "RP2354_SDA");
	assert_str_eq(cell->role, "i2c");
	assert(!cell->controllable);

	cell = &linkr_debugger_gpio_layout_cells[8];
	assert(cell->pin_number == 6U);
	assert_str_eq(cell->signal, "GPIO29_ADC3");
	assert_str_eq(cell->role, "adc");
	assert(cell->controllable);
	assert(!cell->output_capable);
	assert(cell->gpio_pin == 29U);

	cell = &linkr_debugger_gpio_layout_cells[9];
	assert(cell->pin_number == 16U);
	assert_str_eq(cell->label, "GP5");
	assert_str_eq(cell->signal, "GPIO5_CON");
	assert_str_eq(cell->role, "gpio");
	assert(cell->controllable && cell->output_capable);
	assert(cell->gpio_pin == 5U);

	cell = &linkr_debugger_gpio_layout_cells[11];
	assert(cell->pin_number == 15U);
	assert_str_eq(cell->label, "GP6");
	assert_str_eq(cell->signal, "GPIO6_CON");
	assert_str_eq(cell->role, "gpio");
	assert(cell->controllable && cell->output_capable);
	assert(cell->gpio_pin == 6U);

	cell = &linkr_debugger_gpio_layout_cells[20];
	assert(cell->group_order == 1U);
	assert(cell->pin_number == 2U);
	assert_str_eq(cell->label, "GP9 / USER");
	assert_str_eq(cell->signal, "GP09_USER");
	assert(cell->controllable && cell->output_capable);
	assert(cell->gpio_pin == 9U);

	cell = &linkr_debugger_gpio_layout_cells[23];
	assert(cell->pin_number == 4U);
	assert_str_eq(cell->signal, "GND");
	assert_str_eq(cell->role, "ground");
	assert(!cell->controllable);
}

struct power_dependency_fixture {
	bool parent;
	bool child;
	int fail_call;
	int calls;
	struct {
		bool child;
		bool enabled;
	} trace[4];
};

static bool dependency_enabled(void *context, bool child)
{
	struct power_dependency_fixture *fixture = context;

	return child ? fixture->child : fixture->parent;
}

static int dependency_set(void *context, bool child, bool enabled)
{
	struct power_dependency_fixture *fixture = context;
	int call = fixture->calls++;

	fixture->trace[call].child = child;
	fixture->trace[call].enabled = enabled;
	if (call == fixture->fail_call) {
		return -EIO;
	}
	if (child) {
		fixture->child = enabled;
	} else {
		fixture->parent = enabled;
	}
	return 0;
}

static void test_power_dependency_order_and_rollback(void)
{
	static const struct linkr_debugger_power_dependency_ops ops = {
		.is_enabled = dependency_enabled,
		.set_enabled = dependency_set,
	};
	struct power_dependency_fixture fixture = { .fail_call = -1 };

	assert(linkr_debugger_power_dependency_apply(
		       LINKR_DEBUGGER_POWER_DEPENDENCY_CHILD, true, &ops, &fixture) == 0);
	assert(fixture.parent && fixture.child);
	assert(fixture.calls == 2);
	assert(!fixture.trace[0].child && fixture.trace[0].enabled);
	assert(fixture.trace[1].child && fixture.trace[1].enabled);

	fixture = (struct power_dependency_fixture){
		.parent = true, .child = true, .fail_call = -1,
	};
	assert(linkr_debugger_power_dependency_apply(
		       LINKR_DEBUGGER_POWER_DEPENDENCY_PARENT, false, &ops, &fixture) == 0);
	assert(!fixture.parent && !fixture.child);
	assert(fixture.calls == 2);
	assert(fixture.trace[0].child && !fixture.trace[0].enabled);
	assert(!fixture.trace[1].child && !fixture.trace[1].enabled);

	fixture = (struct power_dependency_fixture){
		.parent = true, .child = true, .fail_call = -1,
	};
	assert(linkr_debugger_power_dependency_apply(
		       LINKR_DEBUGGER_POWER_DEPENDENCY_CHILD, false, &ops, &fixture) == 0);
	assert(fixture.parent && !fixture.child);
	assert(fixture.calls == 1);

	fixture = (struct power_dependency_fixture){ .fail_call = 1 };
	assert(linkr_debugger_power_dependency_apply(
		       LINKR_DEBUGGER_POWER_DEPENDENCY_CHILD, true, &ops, &fixture) == -EIO);
	assert(!fixture.parent && !fixture.child);
	assert(fixture.calls == 3);
	assert(!fixture.trace[2].child && !fixture.trace[2].enabled);

	fixture = (struct power_dependency_fixture){
		.parent = true, .child = true, .fail_call = 1,
	};
	assert(linkr_debugger_power_dependency_apply(
		       LINKR_DEBUGGER_POWER_DEPENDENCY_PARENT, false, &ops, &fixture) == -EIO);
	assert(fixture.parent && fixture.child);
	assert(fixture.calls == 3);
	assert(fixture.trace[2].child && fixture.trace[2].enabled);

	fixture = (struct power_dependency_fixture){ .fail_call = 0 };
	assert(linkr_debugger_power_dependency_apply(
		       LINKR_DEBUGGER_POWER_DEPENDENCY_CHILD, true, &ops, &fixture) == -EIO);
	assert(!fixture.parent && !fixture.child);
	assert(fixture.calls == 1);

	fixture = (struct power_dependency_fixture){
		.parent = true, .child = true, .fail_call = 0,
	};
	assert(linkr_debugger_power_dependency_apply(
		       LINKR_DEBUGGER_POWER_DEPENDENCY_PARENT, false, &ops, &fixture) == -EIO);
	assert(fixture.parent && fixture.child);
	assert(fixture.calls == 1);
}

static void test_usb_hub_regulator_dt_contract(void)
{
	char path[4096] = __FILE__;
	char source[32768];
	char *filename = strrchr(path, '/');
	char *regulator;
	char *regulator_end;
	size_t length;
	FILE *file;

	assert(filename != NULL);
	assert(snprintf(filename + 1, sizeof(path) - (size_t)(filename + 1 - path),
			"../../boards/rpi_pico2_rp2350a_m33.overlay") > 0);
	file = fopen(path, "rb");
	assert(file != NULL);
	length = fread(source, 1U, sizeof(source) - 1U, file);
	assert(feof(file));
	assert(fclose(file) == 0);
	source[length] = '\0';
	regulator = strstr(source, "reg_5v_usb_hub: regulator-5v-usb-hub");
	assert(regulator != NULL);
	regulator_end = strstr(regulator, "\n\t};");
	assert(regulator_end != NULL);
	*regulator_end = '\0';
	assert(strstr(regulator, "regulator-name = \"5v_usb_hub\"") != NULL);
	assert(strstr(regulator, "regulator-min-microvolt = <5000000>") != NULL);
	assert(strstr(regulator, "regulator-max-microvolt = <5000000>") != NULL);
	assert(strstr(regulator, "enable-gpios = <&gpio0 24 GPIO_ACTIVE_HIGH>") != NULL);
	assert(strstr(regulator, "regulator-boot-off;") != NULL);
	*regulator_end = '\n';
	assert(strstr(source, "&led0 {\n\tgpios = <&gpio0 25 GPIO_ACTIVE_HIGH>;") != NULL);
	assert(strstr(source, "&led0 {\n\tgpios = <&gpio0 24") == NULL);
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

static void test_tf_wp_route_parser(void)
{
	enum linkr_debugger_tf_wp_route route = LINKR_DEBUGGER_TF_WP_ROUTE_WRITABLE;

	assert(linkr_debugger_parse_tf_wp_route("writable", &route));
	assert(route == LINKR_DEBUGGER_TF_WP_ROUTE_WRITABLE);
	assert_str_eq(linkr_debugger_tf_wp_route_to_string(route), "writable");
	assert(linkr_debugger_parse_tf_wp_route("protected", &route));
	assert(route == LINKR_DEBUGGER_TF_WP_ROUTE_PROTECTED);
	assert_str_eq(linkr_debugger_tf_wp_route_to_string(route), "protected");
	assert(!linkr_debugger_parse_tf_wp_route("garbage", &route));
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
	test_vdd_5v_state_contract_matches_board_revision();
	test_current_table();
	test_adc_table();
	test_adc_microvolt_conversion_fallback();
	test_safe_gpio_allowlist();
	test_full_gpio_layout_catalog();
	test_power_dependency_order_and_rollback();
	test_usb_hub_regulator_dt_contract();
	test_gpio_name_formatter();
	test_bool_parser();
	test_gpio_pin_parser();
	test_tf_wp_route_parser();
	test_heartbeat_state();

	puts("linkr_debugger_model: all tests passed");
	return 0;
}
