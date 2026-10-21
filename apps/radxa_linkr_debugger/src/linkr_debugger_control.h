/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_CONTROL_H_
#define RADXA_LINKR_DEBUGGER_CONTROL_H_

#include "linkr_debugger_config_codec.h"
#include "linkr_debugger_model.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/sensor.h>

enum linkr_debugger_sd_route {
	LINKR_DEBUGGER_SD_ROUTE_TARGET = 0,
	LINKR_DEBUGGER_SD_ROUTE_USB_READER = 1,
};

struct linkr_debugger_current_sample {
	bool rail_enabled;
	bool raw_available;
	bool voltage_valid;
	int32_t raw;
	int32_t mv;
	int32_t voltage_uv;
	int32_t current_ua;
	struct sensor_value value;
};

static inline int linkr_debugger_adc_resolve_microvolts(int conversion_result,
							 int32_t raw,
							 uint8_t resolution,
							 int32_t *microvolts)
{
	int64_t full_scale;
	int64_t scaled;

	if (microvolts == NULL) {
		return -EINVAL;
	}
	if (conversion_result >= 0) {
		return 0;
	}
	if (resolution == 0U || resolution > 32U) {
		return -ERANGE;
	}

	full_scale = (int64_t)((UINT64_C(1) << resolution) - 1U);
	scaled = ((int64_t)raw * INT64_C(3300000)) / full_scale;
	if (scaled > INT32_MAX) {
		*microvolts = INT32_MAX;
	} else if (scaled < INT32_MIN) {
		*microvolts = INT32_MIN;
	} else {
		*microvolts = (int32_t)scaled;
	}

	return 0;
}

struct linkr_debugger_control_item_state {
	uint8_t domain;
	uint8_t item_id;
	uint8_t value;
	bool available;
};

struct linkr_debugger_control_snapshot {
	size_t item_count;
	struct linkr_debugger_control_item_state items[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
};

#define LINKR_DEBUGGER_CURRENT_BATCH_MAX 20U

struct linkr_debugger_watchdog_status {
	bool supported;
	bool armed;
	bool automatic;
	bool healthy;
	uint32_t timeout_ms;
	bool bootloader_on_timeout;
	bool fault_injection_available;
	bool fault_injection_armed;
	const char *failing_service;
};

void linkr_debugger_watchdog_boot_check(void);
int linkr_debugger_control_init(void);
int linkr_debugger_control_snapshot_get(struct linkr_debugger_control_snapshot *snapshot);
int linkr_debugger_watchdog_early_arm(void);
int linkr_debugger_watchdog_supervisor_start(void);

const char *linkr_debugger_json_schema(void);
const char *linkr_debugger_power_capture_protocol(void);
const char *linkr_debugger_mcu_name(void);
const char *linkr_debugger_reserved_gpios(void);
const char *linkr_debugger_usb_mode(void);

bool linkr_debugger_power_output_enabled(const struct linkr_debugger_rail_desc *rail);
int linkr_debugger_power_output_set(const struct linkr_debugger_rail_desc *rail, bool enabled);

enum linkr_debugger_sd_route linkr_debugger_sd_route_get(void);
const char *linkr_debugger_sd_route_name(void);
int linkr_debugger_sd_route_set(enum linkr_debugger_sd_route route);

enum linkr_debugger_tf_wp_route linkr_debugger_tf_wp_route_get(void);
const char *linkr_debugger_tf_wp_route_name(void);
int linkr_debugger_tf_wp_route_set(enum linkr_debugger_tf_wp_route route);

int linkr_debugger_current_read(const struct linkr_debugger_current_desc *current,
				   struct linkr_debugger_current_sample *sample);
int linkr_debugger_current_read_all(struct linkr_debugger_current_sample *samples,
				       size_t sample_count);
int linkr_debugger_current_read_batch(struct linkr_debugger_current_sample *samples,
					 size_t batch_count,
					 size_t channel_count,
					 int64_t *timestamps_us,
					 uint32_t interval_us);

int linkr_debugger_gpio_get(const struct linkr_debugger_safe_gpio_desc *desc, int *value);
int linkr_debugger_gpio_set_output(const struct linkr_debugger_safe_gpio_desc *desc, bool value);
int linkr_debugger_gpio_set_input(const struct linkr_debugger_safe_gpio_desc *desc);
const char *linkr_debugger_safe_gpio_direction_name(size_t index);
const char *linkr_debugger_safe_gpio_direction(const struct linkr_debugger_safe_gpio_desc *desc);

void linkr_debugger_watchdog_status_get(struct linkr_debugger_watchdog_status *status);
void linkr_debugger_watchdog_note_core_alive(void);
void linkr_debugger_watchdog_note_cmdline_alive(void);
void linkr_debugger_watchdog_note_ws_alive(void);
void linkr_debugger_watchdog_note_ws_client_active(bool active);
bool linkr_debugger_watchdog_ota_test_marker_present(void);
void linkr_debugger_watchdog_ota_test_marker_set(void);
void linkr_debugger_watchdog_ota_test_marker_clear(void);
int linkr_debugger_watchdog_prepare_planned_reboot(void);
bool linkr_debugger_watchdog_fault_injection_available(void);
bool linkr_debugger_watchdog_fault_injection_armed(void);
int linkr_debugger_watchdog_fault_injection_arm(void);
int linkr_debugger_watchdog_fault_injection_disarm(void);
bool linkr_debugger_watchdog_fault_injection_confirm_begin(void);
void linkr_debugger_watchdog_fault_injection_confirm_end(void);

int linkr_debugger_bootloader_now(void);

#endif /* RADXA_LINKR_DEBUGGER_CONTROL_H_ */
