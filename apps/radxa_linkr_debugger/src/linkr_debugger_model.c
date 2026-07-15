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
#define INA139_SENSOR "INA139"

const struct linkr_debugger_rail_desc linkr_debugger_rails[] = {
	{
		.name = "12v_out",
		.signal = "GP02_12V_EN",
		.pin = 2,
		.controllable = true,
	},
	{
		.name = "5v_out",
		.signal = "GP05_5V_EN",
#if defined(CONFIG_SOC_SERIES_RP2350)
		.pin = 0,
#else
		.pin = 5,
#endif
		.controllable = true,
	},
	{
		.name = "5v_ws",
		.signal = "GP09_5V_WS_EN",
#if defined(CONFIG_SOC_SERIES_RP2350)
		.pin = 1,
		.always_on = true,
#else
		.pin = 9,
#endif
		.controllable = true,
	},
	{
		.name = "20v_out",
		.signal = "GP10_20V_EN",
#if defined(CONFIG_SOC_SERIES_RP2350)
		.pin = 3,
#else
		.pin = 10,
#endif
		.controllable = true,
	},
};

const size_t linkr_debugger_rail_count = ARRAY_SIZE_LOCAL(linkr_debugger_rails);

const struct linkr_debugger_current_desc linkr_debugger_currents[] = {
	{
		.name = "5v_out",
		.signal = "S_C_5V",
		.sensor = INA139_SENSOR,
		.adc_index = 0,
	},
	{
		.name = "12v_out",
		.signal = "S_C_12V",
		.sensor = INA139_SENSOR,
		.adc_index = 1,
	},
	{
		.name = "20v_out",
		.signal = "S_C_20V",
		.sensor = INA139_SENSOR,
		.adc_index = 2,
	},
};

const size_t linkr_debugger_current_count = ARRAY_SIZE_LOCAL(linkr_debugger_currents);

const struct linkr_debugger_safe_gpio_desc linkr_debugger_safe_gpios[] = {
#if defined(CONFIG_SOC_SERIES_RP2350)
	{ .pin = 7,  .note = "CON_MAS" },
	{ .pin = 8,  .note = "CON_REST" },
	{ .pin = 9,  .note = "CON_USER" },
	{ .pin = 10, .note = "J16_PIN1" },
	{ .pin = 11, .note = "J16_PIN3" },
	{ .pin = 12, .note = "J16_PIN5" },
	{ .pin = 13, .note = "J16_PIN7" },
	{ .pin = 14, .note = "J16_PIN9" },
	{ .pin = 15, .note = "J16_PIN11" },
	{ .pin = 16, .note = "J16_PIN2" },
	{ .pin = 17, .note = "J16_PIN4" },
	{ .pin = 18, .note = "J16_PIN6" },
	{ .pin = 19, .note = "J16_PIN8" },
	{ .pin = 20, .note = "J16_PIN10" },
	{ .pin = 29, .note = "J16_PIN12" },
#else
	{ .pin = 4,  .note = "CON_MAS" },
	{ .pin = 7,  .note = "CON_REST" },
	{ .pin = 8,  .note = "CON_USER" },
	{ .pin = 13, .note = "J17_PIN1" },
	{ .pin = 14, .note = "J17_PIN3" },
	{ .pin = 15, .note = "J17_PIN5" },
	{ .pin = 16, .note = "J17_PIN7" },
	{ .pin = 17, .note = "J17_PIN9" },
	{ .pin = 18, .note = "J17_PIN11" },
	{ .pin = 19, .note = "J17_PIN2" },
	{ .pin = 20, .note = "J17_PIN4" },
	{ .pin = 21, .note = "J17_PIN6" },
	{ .pin = 22, .note = "J17_PIN8" },
	{ .pin = 23, .note = "J17_PIN10" },
	{ .pin = 24, .note = "J17_PIN12" },
#endif
};

const size_t linkr_debugger_safe_gpio_count = ARRAY_SIZE_LOCAL(linkr_debugger_safe_gpios);

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

bool linkr_debugger_parse_vin_route(const char *arg, enum linkr_debugger_vin_route *route)
{
	if (arg == NULL || route == NULL) {
		return false;
	}

	if (streq(arg, "1.8v")) {
		*route = LINKR_DEBUGGER_VIN_ROUTE_1V8;
		return true;
	}

	if (streq(arg, "3.3v")) {
		*route = LINKR_DEBUGGER_VIN_ROUTE_3V3;
		return true;
	}

	return false;
}

const char *linkr_debugger_vin_route_to_string(enum linkr_debugger_vin_route route)
{
	return route == LINKR_DEBUGGER_VIN_ROUTE_1V8 ? "1.8v" : "3.3v";
}

int linkr_debugger_vin_route_microvolt(enum linkr_debugger_vin_route route)
{
	return route == LINKR_DEBUGGER_VIN_ROUTE_1V8 ?
	       LINKR_DEBUGGER_VIN_1V8_UV : LINKR_DEBUGGER_VIN_3V3_UV;
}

bool linkr_debugger_vin_route_from_microvolt(int32_t microvolt,
					    enum linkr_debugger_vin_route *route)
{
	if (route == NULL) {
		return false;
	}

	if (microvolt == LINKR_DEBUGGER_VIN_1V8_UV) {
		*route = LINKR_DEBUGGER_VIN_ROUTE_1V8;
		return true;
	}

	if (microvolt == LINKR_DEBUGGER_VIN_3V3_UV) {
		*route = LINKR_DEBUGGER_VIN_ROUTE_3V3;
		return true;
	}

	return false;
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
