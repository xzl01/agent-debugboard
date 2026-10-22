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

#include "linkr_debugger_board_model.h"

/* Board rail roles. A PORT_VBUS rail drives an externally accessible,
 * bidirectional connector VBUS through a backfeed-capable switch (Q10).
 */
enum linkr_debugger_rail_role {
	LINKR_DEBUGGER_RAIL_ROLE_SUPPLY = 0,
	LINKR_DEBUGGER_RAIL_ROLE_PORT_VBUS = 1,
};

struct linkr_debugger_rail_desc {
	const char *name;
	const char *signal;
	const char *parent;
	uint8_t pin;
	bool controllable;
	bool always_on;
	enum linkr_debugger_rail_role role;
	bool backfeed_capable;
};

/* Effective state of a bidirectional port VBUS rail. EFFECTIVE_OFF means the
 * enable switch is released; it never claims the connector is at 0 V because
 * an external source can backfeed through the switch body diode.
 */
enum linkr_debugger_rail_effective_state {
	LINKR_DEBUGGER_RAIL_EFFECTIVE_OFF = 0,
	LINKR_DEBUGGER_RAIL_EFFECTIVE_ON = 1,
	LINKR_DEBUGGER_RAIL_EFFECTIVE_BLOCKED = 2,
};

/* Backfeed report for a backfeed-capable rail. NONE is intentionally absent:
 * the Q10 body diode can backfeed VDD_5V from J3 even with the switch
 * released, so firmware must never claim "none".
 */
enum linkr_debugger_backfeed_state {
	LINKR_DEBUGGER_BACKFEED_UNKNOWN = 0,
	LINKR_DEBUGGER_BACKFEED_POSSIBLE = 1,
	LINKR_DEBUGGER_BACKFEED_DETECTED = 2,
};

/* Separated port state model (5v_otg):
 * - desired:   user/persist intent, survives parent loss, cleared by explicit
 *              parent off and by an explicit off.
 * - requested: the GPIO/regulator request actually asserted.
 * - effective: on only while requested and the parent is sourcing/qualified;
 *              off means released, not 0 V; blocked when desired but the
 *              parent is absent or backfeed is detected.
 * - backfeed:  never "none"; see enum above.
 */
struct linkr_debugger_port_state {
	bool desired;
	bool requested;
	enum linkr_debugger_rail_effective_state effective;
	enum linkr_debugger_backfeed_state backfeed;
};

enum linkr_debugger_adc_kind {
	LINKR_DEBUGGER_ADC_KIND_CURRENT = 0,
	LINKR_DEBUGGER_ADC_KIND_VOLTAGE = 1,
};

enum linkr_debugger_adc_role {
	LINKR_DEBUGGER_ADC_ROLE_NATIVE = 0,
	LINKR_DEBUGGER_ADC_ROLE_PD_INPUT = 1,
};

struct linkr_debugger_current_desc {
	const char *name;
	const char *signal;
	const char *sensor;
	const char *note;
	enum linkr_debugger_adc_kind kind;
	enum linkr_debugger_adc_role role;
	const char *unit;
	size_t adc_index;
	size_t sensor_index;
};

struct linkr_debugger_heartbeat_state {
	uint32_t ticks;
	bool active;
};

enum linkr_debugger_tf_wp_route {
	LINKR_DEBUGGER_TF_WP_ROUTE_WRITABLE = 0,
	LINKR_DEBUGGER_TF_WP_ROUTE_PROTECTED = 1,
};

#define LINKR_DEBUGGER_GPIO_NAME_BUFSZ 5
#define LINKR_DEBUGGER_ADC_TELEMETRY_CHANNEL_COUNT 8U
#define LINKR_DEBUGGER_CURRENT_SENSOR_COUNT 4U

extern const struct linkr_debugger_rail_desc linkr_debugger_rails[];
extern const size_t linkr_debugger_rail_count;

extern const struct linkr_debugger_current_desc linkr_debugger_currents[];
extern const size_t linkr_debugger_adc_count;
extern const size_t linkr_debugger_current_count;

static inline const char *linkr_debugger_adc_role_name(enum linkr_debugger_adc_role role)
{
	return role == LINKR_DEBUGGER_ADC_ROLE_PD_INPUT ? "pd_input" : "native";
}

bool linkr_debugger_parse_bool_arg(const char *arg, bool *value);
bool linkr_debugger_parse_gpio_pin(const char *arg, uint8_t *pin);
bool linkr_debugger_format_gpio_name(uint8_t pin, char *buf, size_t len);
bool linkr_debugger_parse_tf_wp_route(const char *arg, enum linkr_debugger_tf_wp_route *route);
const char *linkr_debugger_tf_wp_route_to_string(enum linkr_debugger_tf_wp_route route);
bool linkr_debugger_rail_is_port_vbus(const struct linkr_debugger_rail_desc *rail);
bool linkr_debugger_port_desired_cleared_by_parent_off(
	const struct linkr_debugger_rail_desc *rail);
const char *linkr_debugger_rail_effective_name(
	enum linkr_debugger_rail_effective_state effective);
const char *linkr_debugger_backfeed_state_name(enum linkr_debugger_backfeed_state backfeed);
bool linkr_debugger_port_request_allowed(bool desired, bool parent_sourcing,
					 bool backfeed_detected);
struct linkr_debugger_port_state linkr_debugger_port_state_resolve(
	bool desired, bool requested, bool parent_sourcing, bool backfeed_detected);
size_t linkr_debugger_rail_children(
	const struct linkr_debugger_rail_desc *parent,
	const struct linkr_debugger_rail_desc **children, size_t capacity);
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
