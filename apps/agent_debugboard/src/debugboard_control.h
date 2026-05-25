/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AGENT_DEBUGBOARD_CONTROL_H_
#define AGENT_DEBUGBOARD_CONTROL_H_

#include "debugboard_model.h"

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/sensor.h>

enum debugboard_sd_route {
	DEBUGBOARD_SD_ROUTE_TARGET = 0,
	DEBUGBOARD_SD_ROUTE_USB_READER = 1,
};

enum debugboard_usb_route {
	DEBUGBOARD_USB_ROUTE_PC = 0,
	DEBUGBOARD_USB_ROUTE_TARGET = 1,
};

struct debugboard_current_sample {
	bool rail_enabled;
	bool raw_available;
	int32_t raw;
	int32_t mv;
	int32_t current_ua;
	struct sensor_value value;
};

struct debugboard_watchdog_status {
	bool supported;
	bool armed;
	bool automatic;
	bool healthy;
	uint32_t timeout_ms;
	bool bootloader_on_timeout;
	const char *failing_service;
};

void debugboard_watchdog_boot_check(void);
int debugboard_control_init(void);
int debugboard_watchdog_supervisor_start(void);

const char *debugboard_json_schema(void);
const char *debugboard_reserved_gpios(void);
const char *debugboard_usb_mode(void);

bool debugboard_power_output_enabled(const struct debugboard_rail_desc *rail);
int debugboard_power_output_set(const struct debugboard_rail_desc *rail, bool enabled);

enum debugboard_sd_route debugboard_sd_route_get(void);
const char *debugboard_sd_route_name(void);
int debugboard_sd_route_set(enum debugboard_sd_route route);

enum debugboard_usb_route debugboard_usb_route_get(void);
const char *debugboard_usb_route_name(void);
int debugboard_usb_route_set(enum debugboard_usb_route route);

int debugboard_current_read(const struct debugboard_current_desc *current,
				   struct debugboard_current_sample *sample);

int debugboard_gpio_get(const struct debugboard_safe_gpio_desc *desc, int *value);
int debugboard_gpio_set_output(const struct debugboard_safe_gpio_desc *desc, bool value);
int debugboard_gpio_set_input(const struct debugboard_safe_gpio_desc *desc);
const char *debugboard_safe_gpio_direction_name(size_t index);

void debugboard_watchdog_status_get(struct debugboard_watchdog_status *status);
void debugboard_watchdog_note_core_alive(void);
void debugboard_watchdog_note_cmdline_alive(void);
void debugboard_watchdog_note_ws_alive(void);
void debugboard_watchdog_note_ws_client_active(bool active);

int debugboard_bootloader_now(void);

#endif /* AGENT_DEBUGBOARD_CONTROL_H_ */
