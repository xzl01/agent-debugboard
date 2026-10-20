/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) xzl <xiangzelong@radxa.com>
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_model.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE_LOCAL(array) (sizeof(array) / sizeof((array)[0]))
#define INA226_SENSOR "INA226"
#define ADC_SENSOR "ADC"
#define CURRENT_UNIT "uA"
#define VOLTAGE_UNIT "uV"

const struct linkr_debugger_rail_desc linkr_debugger_rails[] = {
	{
		.name = "12v_out",
		.signal = "GP02_12V_EN",
		.pin = 2,
		.controllable = true,
	},
	{
		.name = "5v_out",
		.signal = "GP0_5V_EN",
		.pin = 0,
		.controllable = true,
	},
	{
		.name = "vdd_5v",
		.signal = "GP01_5V_WS_EN",
		.pin = 1,
		.controllable = true,
	},
	{
		.name = "20v_out",
		.signal = "GP03_20V_EN",
		.pin = 3,
		.controllable = true,
	},
};

const size_t linkr_debugger_rail_count = ARRAY_SIZE_LOCAL(linkr_debugger_rails);

const struct linkr_debugger_current_desc linkr_debugger_currents[] = {
	{
		.name = "5v_out",
		.signal = "INA226_5V",
		.sensor = INA226_SENSOR,
		.kind = LINKR_DEBUGGER_ADC_KIND_CURRENT,
		.unit = CURRENT_UNIT,
		.sensor_index = 0,
	},
	{
		.name = "12v_out",
		.signal = "INA226_12V",
		.sensor = INA226_SENSOR,
		.kind = LINKR_DEBUGGER_ADC_KIND_CURRENT,
		.unit = CURRENT_UNIT,
		.sensor_index = 1,
	},
	{
		.name = "20v_out",
		.signal = "INA226_20V",
		.sensor = INA226_SENSOR,
		.kind = LINKR_DEBUGGER_ADC_KIND_CURRENT,
		.unit = CURRENT_UNIT,
		.sensor_index = 2,
	},
	{
		.name = "vdd_5v",
		.signal = "INA226_VDD_5V",
		.sensor = INA226_SENSOR,
		.kind = LINKR_DEBUGGER_ADC_KIND_CURRENT,
		.unit = CURRENT_UNIT,
		.sensor_index = 3,
	},
	{
		.name = "adc3",
		.signal = "GPIO29_ADC3",
		.sensor = ADC_SENSOR,
		.kind = LINKR_DEBUGGER_ADC_KIND_VOLTAGE,
		.unit = VOLTAGE_UNIT,
		.adc_index = 3,
	},
};

const size_t linkr_debugger_adc_count = ARRAY_SIZE_LOCAL(linkr_debugger_currents);
const size_t linkr_debugger_current_count = LINKR_DEBUGGER_CURRENT_SENSOR_COUNT;

_Static_assert(ARRAY_SIZE_LOCAL(linkr_debugger_currents) ==
	       LINKR_DEBUGGER_ADC_TELEMETRY_CHANNEL_COUNT,
	       "ADC telemetry descriptor count changed");
_Static_assert(ARRAY_SIZE_LOCAL(linkr_debugger_currents) ==
	       LINKR_DEBUGGER_CURRENT_SENSOR_COUNT + 1U,
	       "current sensor descriptor count changed");

const struct linkr_debugger_safe_gpio_desc linkr_debugger_safe_gpios[] = {
	{ .pin = 8,  .output_capable = true, .note = "CON_REST", .layout_group = "J13", .layout_label = "RSET",    .layout_row = 0, .layout_column = 1 },
	{ .pin = 9,  .output_capable = true, .note = "CON_USER", .layout_group = "J13", .layout_label = "USER",    .layout_row = 0, .layout_column = 0 },
	{ .pin = 7,  .output_capable = true, .note = "CON_MAS",  .layout_group = "J13", .layout_label = "MASKROM", .layout_row = 1, .layout_column = 0 },
	{ .pin = 15, .output_capable = true, .note = "J8_PIN4", .layout_group = "J8", .layout_label = "GP15", .layout_row = 6, .layout_column = 0 },
	{ .pin = 29, .output_capable = false, .note = "J8_PIN6", .layout_group = "J8", .layout_label = "ADC3", .layout_row = 4, .layout_column = 0 },
	{ .pin = 14, .output_capable = true, .note = "J8_PIN3", .layout_group = "J8", .layout_label = "GP14", .layout_row = 7, .layout_column = 0 },
	{ .pin = 13, .output_capable = true, .note = "J8_PIN2", .layout_group = "J8", .layout_label = "GP13", .layout_row = 8, .layout_column = 0 },
	{ .pin = 12, .output_capable = true, .note = "J8_PIN1", .layout_group = "J8", .layout_label = "GP12", .layout_row = 9, .layout_column = 0 },
	{ .pin = 18, .output_capable = true, .note = "GP18", .layout_label = "GP18" },
	{ .pin = 17, .output_capable = true, .note = "GP17", .layout_label = "GP17" },
	{ .pin = 16, .output_capable = true, .note = "J8_PIN5", .layout_group = "J8", .layout_label = "GP16", .layout_row = 5, .layout_column = 0 },
};

const size_t linkr_debugger_safe_gpio_count = ARRAY_SIZE_LOCAL(linkr_debugger_safe_gpios);

const struct linkr_debugger_gpio_layout_group_desc linkr_debugger_gpio_layout_groups[] = {
	{ .name = "J8", .edge = "right", .order = 0 },
	{ .name = "J13", .edge = "right", .order = 1 },
};

const size_t linkr_debugger_gpio_layout_group_count =
	ARRAY_SIZE_LOCAL(linkr_debugger_gpio_layout_groups);

static bool streq(const char *a, const char *b)
{
	return strcmp(a, b) == 0;
}

bool linkr_debugger_parse_bool_arg(const char *arg, bool *value)
{
	if (arg == NULL || value == NULL) {
		return false;
	}

	if (streq(arg, "1") || streq(arg, "on") || streq(arg, "enable") ||
	    streq(arg, "enabled")) {
		*value = true;
		return true;
	}

	if (streq(arg, "0") || streq(arg, "off") || streq(arg, "disable") ||
	    streq(arg, "disabled")) {
		*value = false;
		return true;
	}

	return false;
}

bool linkr_debugger_parse_gpio_pin(const char *arg, uint8_t *pin)
{
	const char *p;
	char *end = NULL;
	unsigned long parsed;

	if (arg == NULL || pin == NULL) {
		return false;
	}

	p = arg;
	if ((arg[0] == 'G' || arg[0] == 'g') && (arg[1] == 'P' || arg[1] == 'p')) {
		p = &arg[2];
	}

	if (*p == '\0') {
		return false;
	}

	for (const char *s = p; *s != '\0'; s++) {
		if (!isdigit((unsigned char)*s)) {
			return false;
		}
	}

	errno = 0;
	parsed = strtoul(p, &end, 10);
	if (errno != 0 || end == p || *end != '\0' || parsed > 29) {
		return false;
	}

	*pin = (uint8_t)parsed;
	return true;
}

bool linkr_debugger_format_gpio_name(uint8_t pin, char *buf, size_t len)
{
	int written;

	if (buf == NULL || len == 0) {
		return false;
	}

	written = snprintf(buf, len, "GP%u", (unsigned int)pin);
	return written > 0 && (size_t)written < len;
}

bool linkr_debugger_parse_tf_wp_route(const char *arg, enum linkr_debugger_tf_wp_route *route)
{
	if (arg == NULL || route == NULL) {
		return false;
	}

	if (streq(arg, "writable")) {
		*route = LINKR_DEBUGGER_TF_WP_ROUTE_WRITABLE;
		return true;
	}

	if (streq(arg, "protected")) {
		*route = LINKR_DEBUGGER_TF_WP_ROUTE_PROTECTED;
		return true;
	}

	return false;
}

const char *linkr_debugger_tf_wp_route_to_string(enum linkr_debugger_tf_wp_route route)
{
	return route == LINKR_DEBUGGER_TF_WP_ROUTE_PROTECTED ? "protected" : "writable";
}

bool linkr_debugger_rail_initial_enabled(const struct linkr_debugger_rail_desc *rail)
{
	return rail != NULL && rail->controllable && rail->always_on;
}

bool linkr_debugger_rail_state_allowed(const struct linkr_debugger_rail_desc *rail, bool enabled)
{
	if (rail == NULL || !rail->controllable) {
		return false;
	}

	return !rail->always_on || enabled;
}

bool linkr_debugger_heartbeat_step(struct linkr_debugger_heartbeat_state *state,
					 bool feed_success,
					 uint32_t ticks_per_toggle)
{
	if (state == NULL) {
		return false;
	}

	if (!feed_success || ticks_per_toggle == 0U) {
		state->ticks = 0U;
		state->active = false;
		return false;
	}

	state->ticks++;
	if (state->ticks >= ticks_per_toggle) {
		state->ticks = 0U;
		state->active = !state->active;
	}

	return state->active;
}

const struct linkr_debugger_rail_desc *linkr_debugger_find_rail(const char *name)
{
	if (name == NULL) {
		return NULL;
	}

	for (size_t i = 0; i < linkr_debugger_rail_count; i++) {
		if (streq(name, linkr_debugger_rails[i].name) ||
		    streq(name, linkr_debugger_rails[i].signal)) {
			return &linkr_debugger_rails[i];
		}
	}

	return NULL;
}

const struct linkr_debugger_current_desc *linkr_debugger_find_current(const char *name)
{
	if (name == NULL) {
		return NULL;
	}

	for (size_t i = 0; i < linkr_debugger_current_count; i++) {
		if (streq(name, linkr_debugger_currents[i].name) ||
		    streq(name, linkr_debugger_currents[i].signal)) {
			return &linkr_debugger_currents[i];
		}
	}

	return NULL;
}

const struct linkr_debugger_current_desc *linkr_debugger_find_adc(const char *name)
{
	if (name == NULL) {
		return NULL;
	}

	for (size_t i = 0; i < linkr_debugger_adc_count; i++) {
		if (streq(name, linkr_debugger_currents[i].name) ||
		    streq(name, linkr_debugger_currents[i].signal)) {
			return &linkr_debugger_currents[i];
		}
	}

	return NULL;
}

const struct linkr_debugger_safe_gpio_desc *linkr_debugger_find_safe_gpio_by_pin(uint8_t pin)
{
	for (size_t i = 0; i < linkr_debugger_safe_gpio_count; i++) {
		if (linkr_debugger_safe_gpios[i].pin == pin) {
			return &linkr_debugger_safe_gpios[i];
		}
	}

	return NULL;
}

const struct linkr_debugger_safe_gpio_desc *linkr_debugger_find_safe_gpio_by_identifier(const char *identifier)
{
	uint8_t pin;

	if (identifier == NULL) {
		return NULL;
	}

	if (linkr_debugger_parse_gpio_pin(identifier, &pin)) {
		return linkr_debugger_find_safe_gpio_by_pin(pin);
	}

	for (size_t i = 0; i < linkr_debugger_safe_gpio_count; i++) {
		if (streq(identifier, linkr_debugger_safe_gpios[i].note)) {
			return &linkr_debugger_safe_gpios[i];
		}
	}

	return NULL;
}
