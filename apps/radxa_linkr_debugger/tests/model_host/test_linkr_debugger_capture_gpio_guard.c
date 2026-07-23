/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_capture_gpio_guard.h"

#include <assert.h>
#include <string.h>

#ifndef LINKR_DEBUGGER_GPIO_DIR_INPUT
#define LINKR_DEBUGGER_GPIO_DIR_INPUT 1U
#endif

#ifndef LINKR_DEBUGGER_GPIO_DIR_OUTPUT
#define LINKR_DEBUGGER_GPIO_DIR_OUTPUT 2U
#endif

static void test_no_conflict_for_input_pins(void)
{
	const struct linkr_debugger_safe_gpio_desc safe_gpios[] = {
		{ .pin = 10U },
		{ .pin = 13U },
		{ .pin = 29U },
	};
	const uint8_t capture_pins[] = { 10U, 11U, 29U };
	const uint8_t directions[] = {
		LINKR_DEBUGGER_GPIO_DIR_INPUT,
		LINKR_DEBUGGER_GPIO_DIR_OUTPUT,
		LINKR_DEBUGGER_GPIO_DIR_INPUT,
	};

	assert(!linkr_debugger_capture_gpio_conflicts_with_output(
		capture_pins, 3U, safe_gpios, directions, 3U, NULL));
}

static void test_conflict_reports_first_output_pin(void)
{
	const struct linkr_debugger_safe_gpio_desc safe_gpios[] = {
		{ .pin = 10U },
		{ .pin = 13U },
		{ .pin = 29U },
	};
	const uint8_t capture_pins[] = { 13U, 29U };
	const uint8_t directions[] = {
		LINKR_DEBUGGER_GPIO_DIR_INPUT,
		LINKR_DEBUGGER_GPIO_DIR_OUTPUT,
		LINKR_DEBUGGER_GPIO_DIR_OUTPUT,
	};
	uint8_t conflict_pin = 0U;

	assert(linkr_debugger_capture_gpio_conflicts_with_output(
		capture_pins, 2U, safe_gpios, directions, 3U, &conflict_pin));
	assert(conflict_pin == 13U);
}

static void test_duplicate_capture_pins_and_empty_inputs(void)
{
	const struct linkr_debugger_safe_gpio_desc safe_gpios[] = {
		{ .pin = 13U },
	};
	const uint8_t capture_pins[] = { 13U, 13U };
	const uint8_t directions[] = { LINKR_DEBUGGER_GPIO_DIR_OUTPUT };

	assert(!linkr_debugger_capture_gpio_conflicts_with_output(
		NULL, 0U, safe_gpios, directions, 1U, NULL));
	assert(linkr_debugger_capture_gpio_conflicts_with_output(
		capture_pins, 2U, safe_gpios, directions, 1U, NULL));
}

int main(void)
{
	test_no_conflict_for_input_pins();
	test_conflict_reports_first_output_pin();
	test_duplicate_capture_pins_and_empty_inputs();
	return 0;
}
