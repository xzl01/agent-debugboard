/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_CAPTURE_GPIO_GUARD_H_
#define RADXA_LINKR_DEBUGGER_CAPTURE_GPIO_GUARD_H_

#include "linkr_debugger_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool linkr_debugger_capture_gpio_conflicts_with_output(
	const uint8_t *capture_pins,
	size_t capture_pin_count,
	const struct linkr_debugger_safe_gpio_desc *safe_gpios,
	const uint8_t *gpio_directions,
	size_t safe_gpio_count,
	uint8_t *conflict_pin);

int linkr_debugger_capture_validate_gpio_pins(const uint8_t *pins, size_t pin_count);

#endif /* RADXA_LINKR_DEBUGGER_CAPTURE_GPIO_GUARD_H_ */
