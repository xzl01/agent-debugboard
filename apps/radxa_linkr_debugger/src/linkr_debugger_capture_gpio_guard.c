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
	uint32_t capture_pin_mask,
	const struct linkr_debugger_safe_gpio_desc *safe_gpios,
	const uint8_t *gpio_directions,
	size_t safe_gpio_count,
	uint8_t *conflict_pin)
{
	if (safe_gpio_count > 0U && (safe_gpios == NULL || gpio_directions == NULL)) {
		return false;
	}

	for (size_t i = 0U; i < safe_gpio_count; i++) {
		uint8_t pin = safe_gpios[i].pin;

		if (pin < 32U && (capture_pin_mask & (UINT32_C(1) << pin)) &&
		    gpio_directions[i] == LINKR_DEBUGGER_GPIO_DIR_OUTPUT) {
			if (conflict_pin != NULL) {
				*conflict_pin = pin;
			}
			return true;
		}
	}

	return false;
}
