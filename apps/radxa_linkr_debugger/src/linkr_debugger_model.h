/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RADXA_LINKR_DEBUGGER_MODEL_H_
#define RADXA_LINKR_DEBUGGER_MODEL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct linkr_debugger_rail_desc {
	const char *name;
	const char *signal;
	uint8_t pin;
	bool controllable;
};

struct linkr_debugger_current_desc {
	const char *name;
	const char *signal;
	const char *sensor;
	size_t adc_index;
};

struct linkr_debugger_safe_gpio_desc {
	uint8_t pin;
	const char *note;
};

#define LINKR_DEBUGGER_GPIO_NAME_BUFSZ 5

extern const struct linkr_debugger_rail_desc linkr_debugger_rails[];
extern const size_t linkr_debugger_rail_count;

extern const struct linkr_debugger_current_desc linkr_debugger_currents[];
extern const size_t linkr_debugger_current_count;

extern const struct linkr_debugger_safe_gpio_desc linkr_debugger_safe_gpios[];
extern const size_t linkr_debugger_safe_gpio_count;

bool linkr_debugger_parse_bool_arg(const char *arg, bool *value);
bool linkr_debugger_parse_gpio_pin(const char *arg, uint8_t *pin);
bool linkr_debugger_format_gpio_name(uint8_t pin, char *buf, size_t len);

const struct linkr_debugger_rail_desc *linkr_debugger_find_rail(const char *name);
const struct linkr_debugger_current_desc *linkr_debugger_find_current(const char *name);
const struct linkr_debugger_safe_gpio_desc *linkr_debugger_find_safe_gpio_by_pin(uint8_t pin);
const struct linkr_debugger_safe_gpio_desc *linkr_debugger_find_safe_gpio_by_identifier(const char *identifier);

#endif /* RADXA_LINKR_DEBUGGER_MODEL_H_ */
