/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_capture_gpio_guard.h"

#include <stddef.h>

#ifndef LINKR_DEBUGGER_GPIO_DIR_OUTPUT
#define LINKR_DEBUGGER_GPIO_DIR_OUTPUT 2U
#endif

bool linkr_debugger_capture_gpio_conflicts_with_output(
	const uint8_t *capture_pins,
	size_t capture_pin_count,
	const struct linkr_debugger_safe_gpio_desc *safe_gpios,
	const uint8_t *gpio_directions,
	size_t safe_gpio_count,
	uint8_t *conflict_pin)
{
	if ((capture_pin_count > 0U && capture_pins == NULL) ||
	    (safe_gpio_count > 0U && (safe_gpios == NULL || gpio_directions == NULL))) {
		return false;
	}

	for (size_t pin_index = 0U; pin_index < capture_pin_count; pin_index++) {
		for (size_t gpio_index = 0U; gpio_index < safe_gpio_count; gpio_index++) {
			if (capture_pins[pin_index] != safe_gpios[gpio_index].pin) {
				continue;
			}
			if (gpio_directions[gpio_index] != LINKR_DEBUGGER_GPIO_DIR_OUTPUT) {
				continue;
			}
			if (conflict_pin != NULL) {
				*conflict_pin = capture_pins[pin_index];
			}
			return true;
		}
	}

	return false;
}
