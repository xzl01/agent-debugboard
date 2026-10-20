/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_board_model.h"

#define ARRAY_SIZE_LOCAL(array) (sizeof(array) / sizeof((array)[0]))

#if defined(__ZEPHYR__)

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#define LINKR_GPIO_LAYOUT_NODE DT_CHOSEN(radxa_linkr_gpio_layout)

/* DT_CAT*() paste their arguments directly, so values produced by DT_PROP()
 * must pass through an outer macro expansion before token concatenation.
 */
#define LINKR_CAT2(a, b) LINKR_CAT2_(a, b)
#define LINKR_CAT2_(a, b) a##b
#define LINKR_CAT4(a, b, c, d) LINKR_CAT4_(a, b, c, d)
#define LINKR_CAT4_(a, b, c, d) a##b##c##d
#define LINKR_CAT6(a, b, c, d, e, f) LINKR_CAT6_(a, b, c, d, e, f)
#define LINKR_CAT6_(a, b, c, d, e, f) a##b##c##d##e##f

BUILD_ASSERT(DT_NODE_EXISTS(LINKR_GPIO_LAYOUT_NODE),
	     "missing radxa,linkr-gpio-layout chosen node");
BUILD_ASSERT(DT_NODE_HAS_COMPAT(LINKR_GPIO_LAYOUT_NODE, radxa_linkr_gpio_layout),
	     "chosen GPIO layout has the wrong compatible");

#define LINKR_LAYOUT_GROUP_INIT(group_node)                                      \
	{                                                                         \
		.name = DT_PROP(group_node, connector_name),                       \
		.edge = DT_PROP(group_node, edge),                                 \
		.order = DT_PROP(group_node, order),                               \
		.rows = DT_PROP(group_node, rows),                                 \
		.columns = DT_PROP(group_node, columns),                           \
	},

#define LINKR_LAYOUT_CELL_INIT(cell_node, group_node)                            \
	{                                                                         \
		.group_order = DT_PROP(group_node, order),                         \
		.pin_number = DT_PROP(cell_node, pin_number),                      \
		.row = DT_PROP(cell_node, row),                                    \
		.column = DT_PROP(cell_node, column),                              \
		.label = DT_PROP(cell_node, display_label),                        \
		.signal = DT_PROP(cell_node, signal),                              \
		.role = DT_PROP(cell_node, role),                                  \
		.controllable = DT_PROP(cell_node, controllable),                  \
		.output_capable = DT_PROP(cell_node, output_capable),              \
		.gpio_pin = DT_PROP_OR(cell_node, gpio_pin, 0),                    \
	},

#define LINKR_LAYOUT_GROUP_CELLS(group_node) \
	DT_FOREACH_CHILD_VARGS(group_node, LINKR_LAYOUT_CELL_INIT, group_node)

#define LINKR_SAFE_CELL_INIT_0(cell_node, group_node)
#define LINKR_SAFE_CELL_INIT_1(cell_node, group_node)                            \
	{                                                                         \
		.pin = DT_PROP_OR(cell_node, gpio_pin, 0),                          \
		.output_capable = DT_PROP(cell_node, output_capable),              \
		.note = DT_PROP_OR(cell_node, control_alias, NULL),                 \
		.layout_group = DT_PROP(group_node, connector_name),               \
		.layout_label = DT_PROP(cell_node, display_label),                 \
		.layout_row = DT_PROP(cell_node, row),                             \
		.layout_column = DT_PROP(cell_node, column),                       \
	},
#define LINKR_SAFE_CELL_INIT(cell_node, group_node)                              \
	LINKR_CAT2(LINKR_SAFE_CELL_INIT_, DT_PROP(cell_node, controllable))(       \
		cell_node, group_node)
#define LINKR_SAFE_GROUP_CELLS(group_node) \
	DT_FOREACH_CHILD_VARGS(group_node, LINKR_SAFE_CELL_INIT, group_node)

const char linkr_debugger_gpio_layout_view[] = DT_PROP(LINKR_GPIO_LAYOUT_NODE, view);

const struct linkr_debugger_gpio_layout_group_desc
	linkr_debugger_gpio_layout_groups[] = {
	DT_FOREACH_CHILD(LINKR_GPIO_LAYOUT_NODE, LINKR_LAYOUT_GROUP_INIT)
};

const size_t linkr_debugger_gpio_layout_group_count =
	ARRAY_SIZE_LOCAL(linkr_debugger_gpio_layout_groups);

const struct linkr_debugger_gpio_layout_cell_desc linkr_debugger_gpio_layout_cells[] = {
	DT_FOREACH_CHILD(LINKR_GPIO_LAYOUT_NODE, LINKR_LAYOUT_GROUP_CELLS)
};

const size_t linkr_debugger_gpio_layout_cell_count =
	ARRAY_SIZE_LOCAL(linkr_debugger_gpio_layout_cells);

const struct linkr_debugger_safe_gpio_desc linkr_debugger_safe_gpios[] = {
	DT_FOREACH_CHILD(LINKR_GPIO_LAYOUT_NODE, LINKR_SAFE_GROUP_CELLS)
};

const size_t linkr_debugger_safe_gpio_count =
	ARRAY_SIZE_LOCAL(linkr_debugger_safe_gpios);

#define LINKR_ASSERT_GPIO_UNIQUE_PRESENT(cell_node)                              \
	struct LINKR_CAT2(linkr_layout_gpio_pin_, DT_PROP(cell_node, gpio_pin)) {  \
		uint8_t duplicate_gpio_pin;                                        \
	};
#define LINKR_ASSERT_GPIO_UNIQUE(cell_node)                                      \
	COND_CODE_1(DT_NODE_HAS_PROP(cell_node, gpio_pin),                        \
		    (LINKR_ASSERT_GPIO_UNIQUE_PRESENT(cell_node)), ())

#define LINKR_ASSERT_GPIO_RANGE_PRESENT(cell_node)                               \
	BUILD_ASSERT(DT_PROP(cell_node, gpio_pin) <= 29,                           \
		     "GPIO pin exceeds the RP2350 GPIO range");
#define LINKR_ASSERT_GPIO_RANGE(cell_node)                                       \
	COND_CODE_1(DT_NODE_HAS_PROP(cell_node, gpio_pin),                        \
		    (LINKR_ASSERT_GPIO_RANGE_PRESENT(cell_node)), ())

#define LINKR_ASSERT_CONTROL_FIELDS_PRESENT(cell_node)                           \
	BUILD_ASSERT(DT_NODE_HAS_PROP(cell_node, gpio_pin) &&                     \
		     DT_NODE_HAS_PROP(cell_node, control_alias),                    \
		     "controllable contact needs gpio-pin and control-alias");
#define LINKR_ASSERT_CONTROL_FIELDS(cell_node)                                   \
	COND_CODE_1(DT_PROP(cell_node, controllable),                             \
		    (LINKR_ASSERT_CONTROL_FIELDS_PRESENT(cell_node)), ())

#define LINKR_ASSERT_OUTPUT_CONTROL_PRESENT(cell_node)                           \
	BUILD_ASSERT(DT_PROP(cell_node, controllable),                             \
		     "output-capable contact must be controllable");
#define LINKR_ASSERT_OUTPUT_CONTROL(cell_node)                                   \
	COND_CODE_1(DT_PROP(cell_node, output_capable),                           \
		    (LINKR_ASSERT_OUTPUT_CONTROL_PRESENT(cell_node)), ())

#define LINKR_ASSERT_LAYOUT_CELL(cell_node, group_node)                          \
	BUILD_ASSERT(DT_PROP(cell_node, row) < DT_PROP(group_node, rows),          \
		     "GPIO layout row is outside the connector");                 \
	BUILD_ASSERT(DT_PROP(cell_node, column) < DT_PROP(group_node, columns),    \
		     "GPIO layout column is outside the connector");              \
	LINKR_ASSERT_CONTROL_FIELDS(cell_node)                                     \
	LINKR_ASSERT_OUTPUT_CONTROL(cell_node)                                     \
	BUILD_ASSERT(DT_PROP(cell_node, pin_number) <= 29,                         \
		     "physical pin number exceeds the catalog limit");             \
	LINKR_ASSERT_GPIO_RANGE(cell_node)                                         \
	struct LINKR_CAT6(linkr_layout_coordinate_, DT_PROP(group_node, order), _, \
			  DT_PROP(cell_node, row), _,                            \
			  DT_PROP(cell_node, column)) {                          \
		uint8_t duplicate_coordinate;                                      \
	};                                                                        \
	struct LINKR_CAT4(linkr_layout_physical_pin_, DT_PROP(group_node, order),  \
			  _, DT_PROP(cell_node, pin_number)) {                    \
		uint8_t duplicate_physical_pin;                                    \
	};                                                                        \
	LINKR_ASSERT_GPIO_UNIQUE(cell_node)

#define LINKR_ASSERT_LAYOUT_GROUP(group_node)                                   \
	BUILD_ASSERT(DT_CHILD_NUM(group_node) ==                                  \
		     DT_PROP(group_node, rows) * DT_PROP(group_node, columns),       \
		     "GPIO layout must describe every connector contact");         \
	struct LINKR_CAT2(linkr_layout_group_order_, DT_PROP(group_node, order)) { \
		uint8_t duplicate_group_order;                                     \
	};                                                                        \
	DT_FOREACH_CHILD_VARGS(group_node, LINKR_ASSERT_LAYOUT_CELL, group_node)

DT_FOREACH_CHILD(LINKR_GPIO_LAYOUT_NODE, LINKR_ASSERT_LAYOUT_GROUP)

#endif
