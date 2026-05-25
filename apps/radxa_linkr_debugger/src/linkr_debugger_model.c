/*
 * SPDX-License-Identifier: Apache-2.0
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
		.pin = 5,
		.controllable = true,
	},
	{
		.name = "5v_ws",
		.signal = "GP09_5V_WS_EN",
		.pin = 9,
		.controllable = true,
	},
	{
		.name = "20v_out",
		.signal = "GP10_20V_EN",
		.pin = 10,
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
