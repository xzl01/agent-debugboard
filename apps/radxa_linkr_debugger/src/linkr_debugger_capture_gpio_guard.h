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
	uint32_t capture_pin_mask,
	const struct linkr_debugger_safe_gpio_desc *safe_gpios,
	const uint8_t *gpio_directions,
	size_t safe_gpio_count,
	uint8_t *conflict_pin);

/* Atomic with GPIO setters under control_lock. Caller lock order is
 * la_mutex -> control_lock; no worker/J4 waits while control_lock is held. */
int linkr_debugger_capture_gpio_claim(uint32_t pin_mask, uint32_t generation);
void linkr_debugger_capture_gpio_release(uint32_t generation);

#endif /* RADXA_LINKR_DEBUGGER_CAPTURE_GPIO_GUARD_H_ */
