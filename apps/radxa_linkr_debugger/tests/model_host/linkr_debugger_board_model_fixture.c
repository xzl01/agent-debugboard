/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "linkr_debugger_board_model.h"

#define ARRAY_SIZE_LOCAL(array) (sizeof(array) / sizeof((array)[0]))
#define CELL(group_, pin_, row_, column_, label_, signal_, role_)               \
	{ group_, pin_, row_, column_, label_, signal_, role_, false, false, 0 }
#define GPIO_CELL(group_, pin_, row_, column_, label_, signal_, output_, gp_)    \
	{ group_, pin_, row_, column_, label_, signal_, "gpio", true, output_, gp_ }

const char linkr_debugger_gpio_layout_view[] = "component-top";

const struct linkr_debugger_gpio_layout_group_desc
	linkr_debugger_gpio_layout_groups[] = {
	{ .name = "J8", .edge = "right", .order = 0, .rows = 10, .columns = 2 },
	{ .name = "J13", .edge = "right", .order = 1, .rows = 2, .columns = 2 },
};

const size_t linkr_debugger_gpio_layout_group_count =
	ARRAY_SIZE_LOCAL(linkr_debugger_gpio_layout_groups);

const struct linkr_debugger_gpio_layout_cell_desc linkr_debugger_gpio_layout_cells[] = {
	CELL(0, 10, 0, 0, "GP21 / SDA", "RP2354_SDA", "i2c"),
	CELL(0, 20, 0, 1, "5V USB HUB", "5V_USB_HUB", "power"),
	CELL(0, 9, 1, 0, "GP20 / SCL", "RP2354_SCL", "i2c"),
	CELL(0, 19, 1, 1, "USB DM (hub)", "U3_D3M_HUB_CON", "usb"),
	CELL(0, 8, 2, 0, "GP27 / ADC1", "GPIO27_ADC1", "adc"),
	CELL(0, 18, 2, 1, "USB DP (hub)", "U3_D3P_HUB_CON", "usb"),
	CELL(0, 7, 3, 0, "GP28 / ADC2", "GPIO28_ADC2", "adc"),
	CELL(0, 17, 3, 1, "GND", "GND", "ground"),
	{ 0, 6, 4, 0, "GP29 / ADC3", "GPIO29_ADC3", "adc", true, false, 29 },
	GPIO_CELL(0, 16, 4, 1, "GP5", "GPIO5_CON", true, 5),
	GPIO_CELL(0, 5, 5, 0, "GP16", "GPIO16", true, 16),
	GPIO_CELL(0, 15, 5, 1, "GP6", "GPIO6_CON", true, 6),
	GPIO_CELL(0, 4, 6, 0, "GP15", "GPIO15", true, 15),
	CELL(0, 14, 6, 1, "GND", "GND", "ground"),
	GPIO_CELL(0, 3, 7, 0, "GP14", "GPIO14", true, 14),
	CELL(0, 13, 7, 1, "USB DM", "MTT_USB2_DM", "usb"),
	GPIO_CELL(0, 2, 8, 0, "GP13", "GPIO13", true, 13),
	CELL(0, 12, 8, 1, "USB DP", "MTT_USB2_DP", "usb"),
	GPIO_CELL(0, 1, 9, 0, "GP12", "GPIO12", true, 12),
	CELL(0, 11, 9, 1, "GND", "GND", "ground"),
	GPIO_CELL(1, 2, 0, 0, "GP9 / USER", "GP09_USER", true, 9),
	GPIO_CELL(1, 3, 0, 1, "GP8 / RSET", "GP08_REST", true, 8),
	GPIO_CELL(1, 1, 1, 0, "GP7 / MASKROM", "GP07_MAS", true, 7),
	CELL(1, 4, 1, 1, "GND", "GND", "ground"),
};

const size_t linkr_debugger_gpio_layout_cell_count =
	ARRAY_SIZE_LOCAL(linkr_debugger_gpio_layout_cells);

const struct linkr_debugger_safe_gpio_desc linkr_debugger_safe_gpios[] = {
	{ 29, false, "J8_PIN6", "J8", "GP29 / ADC3", 4, 0 },
	{ 5, true, "J8_PIN16", "J8", "GP5", 4, 1 },
	{ 16, true, "J8_PIN5", "J8", "GP16", 5, 0 },
	{ 6, true, "J8_PIN15", "J8", "GP6", 5, 1 },
	{ 15, true, "J8_PIN4", "J8", "GP15", 6, 0 },
	{ 14, true, "J8_PIN3", "J8", "GP14", 7, 0 },
	{ 13, true, "J8_PIN2", "J8", "GP13", 8, 0 },
	{ 12, true, "J8_PIN1", "J8", "GP12", 9, 0 },
	{ 9, true, "CON_USER", "J13", "GP9 / USER", 0, 0 },
	{ 8, true, "CON_REST", "J13", "GP8 / RSET", 0, 1 },
	{ 7, true, "CON_MAS", "J13", "GP7 / MASKROM", 1, 0 },
};

const size_t linkr_debugger_safe_gpio_count =
	ARRAY_SIZE_LOCAL(linkr_debugger_safe_gpios);

_Static_assert(ARRAY_SIZE_LOCAL(linkr_debugger_gpio_layout_cells) == 24U,
	       "host layout fixture must cover every contact");
_Static_assert(ARRAY_SIZE_LOCAL(linkr_debugger_safe_gpios) == 11U,
	       "host safe-GPIO fixture changed");
