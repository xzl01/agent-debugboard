/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RADXA_LINKR_DEBUGGER_CONTROL_H_
#define RADXA_LINKR_DEBUGGER_CONTROL_H_

#include "linkr_debugger_model.h"

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

int linkr_debugger_current_read(const struct linkr_debugger_current_desc *current,
				   struct linkr_debugger_current_sample *sample);

int linkr_debugger_gpio_get(const struct linkr_debugger_safe_gpio_desc *desc, int *value);
int linkr_debugger_gpio_set_output(const struct linkr_debugger_safe_gpio_desc *desc, bool value);
int linkr_debugger_gpio_set_input(const struct linkr_debugger_safe_gpio_desc *desc);
const char *linkr_debugger_safe_gpio_direction_name(size_t index);

void linkr_debugger_watchdog_status_get(struct linkr_debugger_watchdog_status *status);
void linkr_debugger_watchdog_note_core_alive(void);
void linkr_debugger_watchdog_note_cmdline_alive(void);
void linkr_debugger_watchdog_note_ws_alive(void);
void linkr_debugger_watchdog_note_ws_client_active(bool active);

int linkr_debugger_bootloader_now(void);

#endif /* RADXA_LINKR_DEBUGGER_CONTROL_H_ */
