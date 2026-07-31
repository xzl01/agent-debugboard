/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_CONTROL_H_
#define RADXA_LINKR_DEBUGGER_CONTROL_H_

#include "linkr_debugger_model.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/sensor.h>

enum linkr_debugger_sd_route {
	LINKR_DEBUGGER_SD_ROUTE_TARGET = 0,
	LINKR_DEBUGGER_SD_ROUTE_USB_READER = 1,
};

enum linkr_debugger_usb_route {
	LINKR_DEBUGGER_USB_ROUTE_PC = 0,
	LINKR_DEBUGGER_USB_ROUTE_TARGET = 1,
};

struct linkr_debugger_current_sample {
	bool rail_enabled;
	bool raw_available;
	int32_t raw;
	int32_t mv;
	int32_t current_ua;
	struct sensor_value value;
};

#define LINKR_DEBUGGER_CURRENT_BATCH_MAX 20U
#define LINKR_DEBUGGER_TARGET_RECOVERY_OFF_MS 1000U
#define LINKR_DEBUGGER_TARGET_RECOVERY_SETUP_MS 20U
#define LINKR_DEBUGGER_TARGET_RECOVERY_HOLD_MS 500U

struct linkr_debugger_watchdog_status {
	bool supported;
	bool armed;
	bool automatic;
	bool healthy;
	uint32_t timeout_ms;
	bool bootloader_on_timeout;
	const char *failing_service;
};

void linkr_debugger_watchdog_boot_check(void);
int linkr_debugger_control_init(void);
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

enum linkr_debugger_usb_route linkr_debugger_usb_route_get(void);
const char *linkr_debugger_usb_route_name(void);
int linkr_debugger_usb_route_set(enum linkr_debugger_usb_route route);

bool linkr_debugger_vin_switch_available(void);
enum linkr_debugger_vin_route linkr_debugger_vin_route_get(void);
const char *linkr_debugger_vin_route_name(void);
int linkr_debugger_vin_route_set(enum linkr_debugger_vin_route route);

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

int linkr_debugger_target_recovery_enter(
	enum linkr_debugger_target_recovery_mode mode,
	const struct linkr_debugger_rail_desc *rail);

void linkr_debugger_watchdog_status_get(struct linkr_debugger_watchdog_status *status);
void linkr_debugger_watchdog_note_core_alive(void);
void linkr_debugger_watchdog_note_cmdline_alive(void);
void linkr_debugger_watchdog_note_ws_alive(void);
void linkr_debugger_watchdog_note_ws_client_active(bool active);
bool linkr_debugger_watchdog_ota_test_marker_present(void);
void linkr_debugger_watchdog_ota_test_marker_set(void);
void linkr_debugger_watchdog_ota_test_marker_clear(void);
int linkr_debugger_watchdog_prepare_planned_reboot(void);

int linkr_debugger_bootloader_now(void);

#endif /* RADXA_LINKR_DEBUGGER_CONTROL_H_ */
