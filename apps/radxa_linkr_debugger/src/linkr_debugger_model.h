/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
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
	bool always_on;
};

enum linkr_debugger_adc_kind {
	LINKR_DEBUGGER_ADC_KIND_CURRENT = 0,
	LINKR_DEBUGGER_ADC_KIND_VOLTAGE = 1,
};

struct linkr_debugger_current_desc {
	const char *name;
	const char *signal;
	const char *sensor;
	enum linkr_debugger_adc_kind kind;
	const char *unit;
	size_t adc_index;
};

struct linkr_debugger_safe_gpio_desc {
	uint8_t pin;
	bool output_capable;
	const char *note;
	const char *layout_group;
	const char *layout_label;
	uint8_t layout_row;
	uint8_t layout_column;
};

struct linkr_debugger_heartbeat_state {
	uint32_t ticks;
	bool active;
};

enum linkr_debugger_vin_route {
	LINKR_DEBUGGER_VIN_ROUTE_1V8 = 0,
	LINKR_DEBUGGER_VIN_ROUTE_3V3 = 1,
};

enum linkr_debugger_target_recovery_mode {
	LINKR_DEBUGGER_TARGET_RECOVERY_QUALCOMM_EDL = 0,
	LINKR_DEBUGGER_TARGET_RECOVERY_ROCKCHIP_MASKROM = 1,
};

enum linkr_debugger_tf_wp_route {
	LINKR_DEBUGGER_TF_WP_ROUTE_WRITABLE = 0,
	LINKR_DEBUGGER_TF_WP_ROUTE_PROTECTED = 1,
};

#define LINKR_DEBUGGER_GPIO_NAME_BUFSZ 5
#define LINKR_DEBUGGER_VIN_1V8_UV 1800000
#define LINKR_DEBUGGER_VIN_3V3_UV 3300000
#define LINKR_DEBUGGER_ADC_TELEMETRY_CHANNEL_COUNT 4U
#define LINKR_DEBUGGER_CURRENT_SENSOR_COUNT 3U

extern const struct linkr_debugger_rail_desc linkr_debugger_rails[];
extern const size_t linkr_debugger_rail_count;

extern const struct linkr_debugger_current_desc linkr_debugger_currents[];
extern const size_t linkr_debugger_adc_count;
extern const size_t linkr_debugger_current_count;

extern const struct linkr_debugger_safe_gpio_desc linkr_debugger_safe_gpios[];
extern const size_t linkr_debugger_safe_gpio_count;

bool linkr_debugger_parse_bool_arg(const char *arg, bool *value);
bool linkr_debugger_parse_gpio_pin(const char *arg, uint8_t *pin);
bool linkr_debugger_format_gpio_name(uint8_t pin, char *buf, size_t len);
bool linkr_debugger_parse_vin_route(const char *arg, enum linkr_debugger_vin_route *route);
const char *linkr_debugger_vin_route_to_string(enum linkr_debugger_vin_route route);
bool linkr_debugger_parse_tf_wp_route(const char *arg, enum linkr_debugger_tf_wp_route *route);
const char *linkr_debugger_tf_wp_route_to_string(enum linkr_debugger_tf_wp_route route);
int linkr_debugger_vin_route_microvolt(enum linkr_debugger_vin_route route);
bool linkr_debugger_vin_route_from_microvolt(int32_t microvolt,
					    enum linkr_debugger_vin_route *route);
bool linkr_debugger_parse_target_recovery_mode(
	const char *arg, enum linkr_debugger_target_recovery_mode *mode);
const char *linkr_debugger_target_recovery_mode_to_string(
	enum linkr_debugger_target_recovery_mode mode);
bool linkr_debugger_target_recovery_active_level(
	enum linkr_debugger_target_recovery_mode mode);
bool linkr_debugger_target_recovery_rail_allowed(
	const struct linkr_debugger_rail_desc *rail);
bool linkr_debugger_rail_initial_enabled(const struct linkr_debugger_rail_desc *rail);
bool linkr_debugger_rail_state_allowed(const struct linkr_debugger_rail_desc *rail,
					      bool enabled);
bool linkr_debugger_heartbeat_step(struct linkr_debugger_heartbeat_state *state,
					 bool feed_success,
					 uint32_t ticks_per_toggle);

const struct linkr_debugger_rail_desc *linkr_debugger_find_rail(const char *name);
const struct linkr_debugger_current_desc *linkr_debugger_find_adc(const char *name);
const struct linkr_debugger_current_desc *linkr_debugger_find_current(const char *name);
const struct linkr_debugger_safe_gpio_desc *linkr_debugger_find_safe_gpio_by_pin(uint8_t pin);
const struct linkr_debugger_safe_gpio_desc *linkr_debugger_find_safe_gpio_by_identifier(const char *identifier);

#endif /* RADXA_LINKR_DEBUGGER_MODEL_H_ */
