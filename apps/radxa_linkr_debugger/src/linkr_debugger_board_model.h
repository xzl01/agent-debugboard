/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_BOARD_MODEL_H_
#define RADXA_LINKR_DEBUGGER_BOARD_MODEL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct linkr_debugger_safe_gpio_desc {
	uint8_t pin;
	bool output_capable;
	const char *note;
	const char *layout_group;
	const char *layout_label;
	uint8_t layout_row;
	uint8_t layout_column;
};

struct linkr_debugger_gpio_layout_group_desc {
	const char *name;
	const char *edge;
	uint8_t order;
	uint8_t rows;
	uint8_t columns;
};

struct linkr_debugger_gpio_layout_cell_desc {
	uint8_t group_order;
	uint8_t pin_number;
	uint8_t row;
	uint8_t column;
	const char *label;
	const char *signal;
	const char *role;
	bool controllable;
	bool output_capable;
	uint8_t gpio_pin;
};

extern const char linkr_debugger_gpio_layout_view[];
extern const struct linkr_debugger_gpio_layout_group_desc
	linkr_debugger_gpio_layout_groups[];
extern const size_t linkr_debugger_gpio_layout_group_count;
extern const struct linkr_debugger_gpio_layout_cell_desc
	linkr_debugger_gpio_layout_cells[];
extern const size_t linkr_debugger_gpio_layout_cell_count;
extern const struct linkr_debugger_safe_gpio_desc linkr_debugger_safe_gpios[];
extern const size_t linkr_debugger_safe_gpio_count;

#endif /* RADXA_LINKR_DEBUGGER_BOARD_MODEL_H_ */
