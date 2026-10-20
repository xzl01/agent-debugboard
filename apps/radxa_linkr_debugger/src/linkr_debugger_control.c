/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_control.h"
#include "linkr_debugger_http.h"
#include "linkr_debugger_monitoring.h"

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(linkr_debugger_control, LOG_LEVEL_INF);

#if defined(CONFIG_SOC_SERIES_RP2350)
#include <hardware/regs/watchdog.h>
#include <hardware/watchdog.h>
#include <hardware/gpio.h>
#include <pico/bootrom.h>
#endif

#define GPIO0_NODE DT_NODELABEL(gpio0)
#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)
#define ADC_INPUTS_NODE ZEPHYR_USER_NODE
#define REGULATOR_12V_OUT_NODE DT_NODELABEL(reg_12v_out)
#define REGULATOR_5V_OUT_NODE DT_NODELABEL(reg_5v_out)
#define REGULATOR_VDD_5V_NODE DT_NODELABEL(reg_vdd_5v)
#define REGULATOR_20V_OUT_NODE DT_NODELABEL(reg_20v_out)
#define CURRENT_5V_OUT_NODE DT_NODELABEL(ina226_5v)
#define CURRENT_12V_OUT_NODE DT_NODELABEL(ina226_12v)
#define CURRENT_20V_OUT_NODE DT_NODELABEL(ina226_20v)
#define CURRENT_VDD_5V_NODE DT_NODELABEL(ina226_vdd_5v)
#define WATCHDOG_NODE DT_NODELABEL(wdt0)
#define HEARTBEAT_LED_NODE DT_CHOSEN(zephyr_heartbeat_led)

#define LINKR_DEBUGGER_JSON_SCHEMA "radxa-linkr-debugger.v1"
#define LINKR_DEBUGGER_POWER_CAPTURE_PROTOCOL "host-stream-v1"
#define LINKR_DEBUGGER_USB_MODE "ncm-http"
#define LINKR_DEBUGGER_MCU_NAME "rp2350"
#define LINKR_DEBUGGER_RESERVED_GPIOS "GP00-GP06 GP10-GP11 GP19 GP22-GP25 GP26-GP28"

#define LINKR_DEBUGGER_GPIO_DIR_INPUT 1
#define LINKR_DEBUGGER_GPIO_DIR_OUTPUT 2

#define REGULATOR_STATE_CAPACITY 8U
#define SAFE_GPIO_STATE_CAPACITY 32U
#define LINKR_DEBUGGER_WATCHDOG_TIMEOUT_MS 5000U
#define LINKR_DEBUGGER_WATCHDOG_BOOT_TIMEOUT_MS 15000U
#define LINKR_DEBUGGER_WATCHDOG_SUPERVISOR_PERIOD_MS 250U
#define LINKR_DEBUGGER_WATCHDOG_STARTUP_GRACE_MS 15000U
#define LINKR_DEBUGGER_WATCHDOG_CORE_STALE_MS 1500U
#define LINKR_DEBUGGER_WATCHDOG_CMDLINE_STALE_MS 2000U
#define LINKR_DEBUGGER_WATCHDOG_WS_STALE_MS 1500U
#define LINKR_DEBUGGER_WATCHDOG_DIAGNOSTIC_PERIOD_MS 500U
#define LINKR_DEBUGGER_HEARTBEAT_TICKS_PER_TOGGLE 2U
#if defined(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION)
#define LINKR_DEBUGGER_WATCHDOG_FAULT_INJECTION_TIMEOUT_MS 10000U
#endif
#define LINKR_DEBUGGER_WATCHDOG_SCRATCH_INDEX 0U
#define LINKR_DEBUGGER_WATCHDOG_BOOTSEL_MARKER 0xadb00751U
#define LINKR_DEBUGGER_WATCHDOG_SOURCE_SCRATCH 1U
#define LINKR_DEBUGGER_WATCHDOG_OTA_TEST_SCRATCH 2U
#define LINKR_DEBUGGER_BOOTLOADER_SOURCE_EXPLICIT 0xbfeed001U
#define LINKR_DEBUGGER_BOOTLOADER_SOURCE_WATCHDOG  0xbfeed002U
#define LINKR_DEBUGGER_OTA_TEST_MARKER 0x07a7e571U

#define ADC_SPEC_AND_COMMA(node_id, prop, idx) \
	COND_CODE_1(DT_PHA_HAS_CELL_AT_IDX(node_id, prop, idx, input), \
			(ADC_DT_SPEC_GET_BY_IDX(node_id, idx),), ())

#if DT_NODE_HAS_STATUS(REGULATOR_12V_OUT_NODE, okay) && \
    DT_NODE_HAS_STATUS(REGULATOR_5V_OUT_NODE, okay) && \
    DT_NODE_HAS_STATUS(REGULATOR_VDD_5V_NODE, okay) && \
    DT_NODE_HAS_STATUS(REGULATOR_20V_OUT_NODE, okay)
#define HAS_REGULATORS 1
BUILD_ASSERT(DT_NODE_HAS_STATUS(REGULATOR_12V_OUT_NODE, okay));
BUILD_ASSERT(DT_NODE_HAS_STATUS(REGULATOR_5V_OUT_NODE, okay));
BUILD_ASSERT(DT_NODE_HAS_STATUS(REGULATOR_VDD_5V_NODE, okay));
BUILD_ASSERT(DT_NODE_HAS_STATUS(REGULATOR_20V_OUT_NODE, okay));
#else
#define HAS_REGULATORS 0
#endif

#if DT_NODE_HAS_STATUS(CURRENT_5V_OUT_NODE, okay) && \
    DT_NODE_HAS_STATUS(CURRENT_12V_OUT_NODE, okay) && \
    DT_NODE_HAS_STATUS(CURRENT_20V_OUT_NODE, okay) && \
    DT_NODE_HAS_STATUS(CURRENT_VDD_5V_NODE, okay)
#define HAS_CURRENT_SENSE 1
BUILD_ASSERT(DT_NODE_HAS_STATUS(CURRENT_5V_OUT_NODE, okay));
BUILD_ASSERT(DT_NODE_HAS_STATUS(CURRENT_12V_OUT_NODE, okay));
BUILD_ASSERT(DT_NODE_HAS_STATUS(CURRENT_20V_OUT_NODE, okay));
BUILD_ASSERT(DT_NODE_HAS_STATUS(CURRENT_VDD_5V_NODE, okay));
#else
#define HAS_CURRENT_SENSE 0
#endif

#if DT_HAS_CHOSEN(zephyr_heartbeat_led) && \
    DT_NODE_HAS_STATUS(HEARTBEAT_LED_NODE, okay) && \
    DT_NODE_HAS_PROP(HEARTBEAT_LED_NODE, gpios)
#define HAS_HEARTBEAT_LED 1
#else
#define HAS_HEARTBEAT_LED 0
#endif

static const struct device *const gpio0 = DEVICE_DT_GET(GPIO0_NODE);
static const struct device *const watchdog_dev = DEVICE_DT_GET_OR_NULL(WATCHDOG_NODE);
static bool regulator_states[REGULATOR_STATE_CAPACITY];
static enum linkr_debugger_sd_route linkr_debugger_sd_route = LINKR_DEBUGGER_SD_ROUTE_TARGET;
static enum linkr_debugger_usb_route linkr_debugger_usb_route = LINKR_DEBUGGER_USB_ROUTE_TARGET;
static enum linkr_debugger_tf_wp_route linkr_debugger_tf_wp_route = LINKR_DEBUGGER_TF_WP_ROUTE_WRITABLE;
static uint8_t linkr_debugger_gpio_directions[SAFE_GPIO_STATE_CAPACITY];
static bool linkr_debugger_gpio_output_levels[SAFE_GPIO_STATE_CAPACITY];
K_MUTEX_DEFINE(linkr_debugger_control_lock);
static bool linkr_debugger_watchdog_armed;
static bool linkr_debugger_watchdog_boot_timeout_active;
static bool linkr_debugger_watchdog_planned_reboot_pending;
static int linkr_debugger_watchdog_channel = -1;
static bool linkr_debugger_watchdog_supervisor_started;
static bool linkr_debugger_watchdog_ws_client_active;
static int64_t linkr_debugger_watchdog_core_alive_ms;
static int64_t linkr_debugger_watchdog_cmdline_alive_ms;
static int64_t linkr_debugger_watchdog_ws_alive_ms;
static int64_t linkr_debugger_watchdog_started_ms;
static int64_t linkr_debugger_watchdog_last_diagnostic_ms;
static int64_t linkr_debugger_watchdog_last_feed_ms;
#if defined(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION)
static bool linkr_debugger_watchdog_fault_injection_armed_state;
static int64_t linkr_debugger_watchdog_fault_injection_expires_ms;
static bool linkr_debugger_watchdog_fault_injection_confirm_pending;
#endif
static const char *linkr_debugger_watchdog_failing_service = NULL;
static const char *linkr_debugger_watchdog_last_reported_service;
static struct linkr_debugger_heartbeat_state linkr_debugger_watchdog_heartbeat;
static K_THREAD_STACK_DEFINE(linkr_debugger_watchdog_supervisor_stack, 1024);
static struct k_thread linkr_debugger_watchdog_supervisor_thread;

#if HAS_REGULATORS
static const struct device *const regulators[] = {
	DEVICE_DT_GET(REGULATOR_12V_OUT_NODE),
	DEVICE_DT_GET(REGULATOR_5V_OUT_NODE),
	DEVICE_DT_GET(REGULATOR_VDD_5V_NODE),
	DEVICE_DT_GET(REGULATOR_20V_OUT_NODE),
};
#else
static const struct device *const regulators[] = {};
#endif

#if HAS_CURRENT_SENSE
static const struct device *const current_sensors[] = {
	DEVICE_DT_GET(CURRENT_5V_OUT_NODE),
	DEVICE_DT_GET(CURRENT_12V_OUT_NODE),
	DEVICE_DT_GET(CURRENT_20V_OUT_NODE),
	DEVICE_DT_GET(CURRENT_VDD_5V_NODE),
};
BUILD_ASSERT(ARRAY_SIZE(current_sensors) == LINKR_DEBUGGER_CURRENT_SENSOR_COUNT);
#else
static const struct device *const current_sensors[] = {};
#endif

#if DT_NODE_HAS_STATUS(DT_PATH(zephyr_user), okay) && \
    DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
static const struct adc_dt_spec adc_channels[] = {
	DT_FOREACH_PROP_ELEM(ADC_INPUTS_NODE, io_channels, ADC_SPEC_AND_COMMA)
};
#else
static const struct adc_dt_spec adc_channels[] = {};
#endif

#if HAS_HEARTBEAT_LED
static const struct gpio_dt_spec heartbeat_led = GPIO_DT_SPEC_GET(HEARTBEAT_LED_NODE, gpios);
#endif

BUILD_ASSERT(ARRAY_SIZE(regulators) <= REGULATOR_STATE_CAPACITY);

static size_t regulator_index(const struct linkr_debugger_rail_desc *rail)
{
	return (size_t)(rail - linkr_debugger_rails);
}

static bool safe_gpio_index_valid(const struct linkr_debugger_safe_gpio_desc *desc, size_t *index)
{
	if (desc == NULL) {
		return false;
	}

	if (linkr_debugger_safe_gpio_count > ARRAY_SIZE(linkr_debugger_gpio_directions)) {
		return false;
	}

	for (size_t i = 0; i < linkr_debugger_safe_gpio_count; i++) {
		if (&linkr_debugger_safe_gpios[i] == desc) {
			if (index != NULL) {
				*index = i;
			}
			return true;
		}
	}

	return false;
}

const char *linkr_debugger_safe_gpio_direction_name(size_t index)
{
	if (index >= linkr_debugger_safe_gpio_count ||
	    index >= ARRAY_SIZE(linkr_debugger_gpio_directions)) {
		return "unknown";
	}
	switch (linkr_debugger_gpio_directions[index]) {
	case LINKR_DEBUGGER_GPIO_DIR_OUTPUT:
		return "output";
	case LINKR_DEBUGGER_GPIO_DIR_INPUT:
		return "input";
	default:
		return "unknown";
	}
}

const char *linkr_debugger_safe_gpio_direction(const struct linkr_debugger_safe_gpio_desc *desc)
{
	size_t index;

	if (!safe_gpio_index_valid(desc, &index)) {
		return "unknown";
	}

	return linkr_debugger_safe_gpio_direction_name(index);
}

static const struct device *linkr_debugger_regulator_device(const struct linkr_debugger_rail_desc *rail)
{
	size_t index;

	if (rail == NULL || !rail->controllable) {
		return NULL;
	}

	index = regulator_index(rail);
	if (index >= ARRAY_SIZE(regulators)) {
		return NULL;
	}

	return regulators[index];
}

static const struct device *linkr_debugger_current_sensor(const struct linkr_debugger_current_desc *current)
{
	if (current == NULL || current->kind != LINKR_DEBUGGER_ADC_KIND_CURRENT ||
	    current->sensor_index >= ARRAY_SIZE(current_sensors)) {
		return NULL;
	}

	return current_sensors[current->sensor_index];
}

static int32_t sensor_value_to_micro_units(const struct sensor_value *value)
{
	int64_t micro_units;

	micro_units = ((int64_t)value->val1 * 1000000) + value->val2;
	return (int32_t)micro_units;
}

static void set_regulator_state(const struct linkr_debugger_rail_desc *rail, bool enabled)
{
	size_t index;

	if (rail == NULL || !rail->controllable) {
		return;
	}

	index = regulator_index(rail);
	if (index >= linkr_debugger_rail_count || index >= ARRAY_SIZE(regulator_states)) {
		return;
	}

	regulator_states[index] = enabled;
}

static int configure_regulator_defaults(void)
{
	const struct device *regulator;

	if (ARRAY_SIZE(regulators) == 0) {
		return 0;
	}

	if (linkr_debugger_rail_count != ARRAY_SIZE(regulators)) {
		return -EINVAL;
	}

	for (size_t i = 0; i < linkr_debugger_rail_count; i++) {
		if (!linkr_debugger_rails[i].controllable) {
			continue;
		}

		regulator = linkr_debugger_regulator_device(&linkr_debugger_rails[i]);
		if (regulator == NULL || !device_is_ready(regulator)) {
			return -ENODEV;
		}

		set_regulator_state(&linkr_debugger_rails[i],
				    regulator_is_enabled(regulator));
	}

	return 0;
}

static int configure_sd_default(void)
{
	linkr_debugger_sd_route = LINKR_DEBUGGER_SD_ROUTE_TARGET;
	return gpio_pin_configure(gpio0, 4, GPIO_OUTPUT_ACTIVE);
}

static int configure_usb_mux_default(void)
{
	linkr_debugger_usb_route = LINKR_DEBUGGER_USB_ROUTE_TARGET;
	return gpio_pin_configure(gpio0, 5, GPIO_OUTPUT_ACTIVE);
}

static int configure_tf_wp_default(void)
{
	/* Writable = GPIO22 high: Q12 pulls GL3224 SD_WP low, which the reader
	 * treats as writable. Released SD_WP is treated as read-only. */
	linkr_debugger_tf_wp_route = LINKR_DEBUGGER_TF_WP_ROUTE_WRITABLE;
	return gpio_pin_configure(gpio0, 22, GPIO_OUTPUT_ACTIVE);
}

static void linkr_debugger_gpio_apply_safe_drive_strength(const struct linkr_debugger_safe_gpio_desc *desc)
{
	if (desc != NULL) {
		gpio_set_drive_strength(desc->pin, GPIO_DRIVE_STRENGTH_4MA);
	}
}

static int setup_adc_channels(void)
{
	if (ARRAY_SIZE(adc_channels) == 0) {
		return 0;
	}

	for (size_t i = 0; i < ARRAY_SIZE(adc_channels); i++) {
		if (!adc_is_ready_dt(&adc_channels[i])) {
			LOG_WRN("ADC channel %zu not ready; voltage telemetry degraded", i);
			continue;
		}

		if (adc_channel_setup_dt(&adc_channels[i]) < 0) {
			LOG_WRN("ADC channel %zu setup failed; voltage telemetry degraded", i);
		}
	}

	for (size_t i = 0; i < linkr_debugger_adc_count; i++) {
		const struct linkr_debugger_current_desc *current = &linkr_debugger_currents[i];

		switch (current->kind) {
		case LINKR_DEBUGGER_ADC_KIND_CURRENT: {
			const struct device *sensor = linkr_debugger_current_sensor(current);

			if (sensor == NULL) {
				LOG_WRN("current sensor %s missing from devicetree", current->name);
				continue;
			}
			if (!device_is_ready(sensor)) {
				LOG_WRN("current sensor %s not ready; rail telemetry degraded",
					current->name);
				continue;
			}
			break;
		}
		case LINKR_DEBUGGER_ADC_KIND_VOLTAGE:
			if (current->adc_index >= ARRAY_SIZE(adc_channels)) {
				return -EINVAL;
			}
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

static int configure_heartbeat_led(void)
{
#if HAS_HEARTBEAT_LED
	if (!gpio_is_ready_dt(&heartbeat_led)) {
		return -ENODEV;
	}

	return gpio_pin_configure_dt(&heartbeat_led, GPIO_OUTPUT_INACTIVE);
#else
	return 0;
#endif
}

static void linkr_debugger_heartbeat_led_set(bool active)
{
#if HAS_HEARTBEAT_LED
	int ret = gpio_pin_set_dt(&heartbeat_led, active ? 1 : 0);

	if (ret < 0) {
		LOG_WRN("heartbeat LED set failed: %d", ret);
	}
#else
	ARG_UNUSED(active);
#endif
}

static bool linkr_debugger_watchdog_heartbeat_step_locked(bool feed_success)
{
	return linkr_debugger_heartbeat_step(&linkr_debugger_watchdog_heartbeat,
					    feed_success,
					    LINKR_DEBUGGER_HEARTBEAT_TICKS_PER_TOGGLE);
}

static void linkr_debugger_watchdog_marker_set(void)
{
	watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_SCRATCH_INDEX] = LINKR_DEBUGGER_WATCHDOG_BOOTSEL_MARKER;
	watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_SOURCE_SCRATCH] = 0U;
}

static void linkr_debugger_watchdog_marker_clear(void)
{
	watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_SCRATCH_INDEX] = 0U;
}

bool linkr_debugger_watchdog_ota_test_marker_present(void)
{
#if defined(CONFIG_SOC_SERIES_RP2350)
	return watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_OTA_TEST_SCRATCH] ==
		LINKR_DEBUGGER_OTA_TEST_MARKER;
#else
	return false;
#endif
}

void linkr_debugger_watchdog_ota_test_marker_set(void)
{
#if defined(CONFIG_SOC_SERIES_RP2350)
	watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_OTA_TEST_SCRATCH] =
		LINKR_DEBUGGER_OTA_TEST_MARKER;
#endif
}

void linkr_debugger_watchdog_ota_test_marker_clear(void)
{
#if defined(CONFIG_SOC_SERIES_RP2350)
	watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_OTA_TEST_SCRATCH] = 0U;
#endif
}

static void linkr_debugger_watchdog_force_disable_locked(void)
{
	hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
	linkr_debugger_watchdog_armed = false;
	linkr_debugger_watchdog_boot_timeout_active = false;
	linkr_debugger_watchdog_channel = -1;
}

int linkr_debugger_watchdog_prepare_planned_reboot(void)
{
	int ret = 0;

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	linkr_debugger_watchdog_planned_reboot_pending = true;
	if (linkr_debugger_watchdog_armed && watchdog_dev != NULL && device_is_ready(watchdog_dev)) {
		(void)wdt_feed(watchdog_dev, linkr_debugger_watchdog_channel);
		ret = wdt_disable(watchdog_dev);
		if (ret < 0) {
			LOG_WRN("watchdog disable before planned reboot failed: %d", ret);
		}
	}
	hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
	if ((watchdog_hw->ctrl & WATCHDOG_CTRL_ENABLE_BITS) != 0U) {
		linkr_debugger_watchdog_planned_reboot_pending = false;
		k_mutex_unlock(&linkr_debugger_control_lock);
		return ret < 0 ? ret : -EIO;
	}
	linkr_debugger_watchdog_force_disable_locked();
	linkr_debugger_watchdog_marker_clear();
	watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_SOURCE_SCRATCH] = 0U;
	k_mutex_unlock(&linkr_debugger_control_lock);
	return 0;
}

static void linkr_debugger_watchdog_set_failing_service_locked(const char *service)
{
	linkr_debugger_watchdog_failing_service = service;
}

#if defined(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION)
static void linkr_debugger_watchdog_fault_injection_expire_locked(int64_t now)
{
	if (linkr_debugger_watchdog_fault_injection_armed_state &&
	    now >= linkr_debugger_watchdog_fault_injection_expires_ms) {
		linkr_debugger_watchdog_fault_injection_armed_state = false;
		linkr_debugger_watchdog_fault_injection_expires_ms = 0;
	}
}
#endif

static int linkr_debugger_watchdog_arm_locked(void);

static int64_t linkr_debugger_watchdog_age_ms(int64_t now, int64_t last_ms)
{
	if (last_ms == 0) {
		return -1;
	}

	return now - last_ms;
}

static bool linkr_debugger_watchdog_services_healthy_locked(void)
{
	int64_t now = k_uptime_get();

#if defined(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION)
	if (linkr_debugger_watchdog_fault_injection_armed_state) {
		linkr_debugger_watchdog_fault_injection_expire_locked(now);
		if (linkr_debugger_watchdog_fault_injection_armed_state) {
			linkr_debugger_watchdog_set_failing_service_locked("fault_injection");
			return false;
		}
	}
#endif

	/*
	 * The supervisor is now started before main(). Bypass the runtime
	 * liveness checks for the whole boot grace, otherwise a normal but
	 * blocking init would look like a dead core and stop the feeds.
	 */
	if (linkr_debugger_watchdog_started_ms != 0 &&
	    (now - linkr_debugger_watchdog_started_ms) < LINKR_DEBUGGER_WATCHDOG_STARTUP_GRACE_MS) {
		linkr_debugger_watchdog_set_failing_service_locked(NULL);
		return true;
	}

	if (linkr_debugger_watchdog_core_alive_ms == 0 ||
	    (now - linkr_debugger_watchdog_core_alive_ms) > LINKR_DEBUGGER_WATCHDOG_CORE_STALE_MS) {
		linkr_debugger_watchdog_set_failing_service_locked("core");
		return false;
	}

	if (linkr_debugger_http_listener_fd() < 0) {
		linkr_debugger_watchdog_set_failing_service_locked("api");
		return false;
	}

	if (linkr_debugger_watchdog_cmdline_alive_ms == 0 ||
	    (now - linkr_debugger_watchdog_cmdline_alive_ms) > LINKR_DEBUGGER_WATCHDOG_CMDLINE_STALE_MS) {
		linkr_debugger_watchdog_set_failing_service_locked("cmdline");
		return false;
	}

	linkr_debugger_watchdog_set_failing_service_locked(NULL);
	return true;
}

static int linkr_debugger_watchdog_hw_feed_locked(void)
{
	int ret;

	/*
	 * The early arm covers the pre-main boot window with a long timeout.
	 * Once the supervisor gets scheduled, drop to the runtime timeout.
	 */
	if (linkr_debugger_watchdog_boot_timeout_active &&
	    linkr_debugger_watchdog_armed) {
		int disable_ret = wdt_disable(watchdog_dev);

		if (disable_ret < 0) {
			LOG_WRN("watchdog disable for runtime re-arm failed: %d", disable_ret);
		}
		linkr_debugger_watchdog_armed = false;
		linkr_debugger_watchdog_channel = -1;
		linkr_debugger_watchdog_boot_timeout_active = false;
	}

	ret = linkr_debugger_watchdog_arm_locked();
	if (ret < 0) {
		LOG_ERR("watchdog arm failed: %d", ret);
		return ret;
	}

	ret = wdt_feed(watchdog_dev, linkr_debugger_watchdog_channel);
	if (ret < 0) {
		LOG_ERR("watchdog feed failed: %d", ret);
		linkr_debugger_watchdog_marker_clear();
		linkr_debugger_watchdog_force_disable_locked();
	} else {
		linkr_debugger_watchdog_last_feed_ms = k_uptime_get();
	}

	return ret;
}

static bool linkr_debugger_watchdog_diagnostic_due(int64_t now)
{
	if (linkr_debugger_watchdog_last_diagnostic_ms != 0 &&
	    (now - linkr_debugger_watchdog_last_diagnostic_ms) < LINKR_DEBUGGER_WATCHDOG_DIAGNOSTIC_PERIOD_MS) {
		return false;
	}
	linkr_debugger_watchdog_last_diagnostic_ms = now;
	return true;
}

static void linkr_debugger_watchdog_log_diagnostics(int64_t now, const char *feed_status,
						bool healthy, const char *blocker,
						bool armed, int listener_fd,
						int64_t core_age_ms,
						int64_t cmdline_age_ms,
						int64_t grace_remaining_ms)
{
	struct linkr_debugger_monitoring_diagnostics diagnostics;

	linkr_debugger_monitoring_diagnostics_get(&diagnostics);
	if (diagnostics.heap.available) {
		LOG_INF("memory diagnostics: heap_allocated=%zu heap_free=%zu heap_total=%zu heap_peak=%zu uptime_ms=%lld",
			diagnostics.heap.allocated_bytes,
			diagnostics.heap.free_bytes,
			diagnostics.heap.total_bytes,
			diagnostics.heap.max_allocated_bytes,
			(long long)now);
	} else {
		LOG_INF("memory diagnostics: heap_unavailable=%s heap_error=%d uptime_ms=%lld",
			diagnostics.heap.reason,
			diagnostics.heap.error,
			(long long)now);
	}

	if (diagnostics.runtime.available) {
		LOG_INF("runtime diagnostics: uptime_ms=%lld uptime_seconds=%llu",
			(long long)diagnostics.runtime.uptime_ms,
			(unsigned long long)diagnostics.runtime.uptime_seconds);
	}

	LOG_INF("watchdog trace: supervisor=alive feed=%s healthy=%d blocker=%s armed=%d listener_fd=%d core_age_ms=%lld cmd_age_ms=%lld grace_remaining_ms=%lld uptime_ms=%lld",
		feed_status,
		healthy ? 1 : 0,
		blocker != NULL ? blocker : "none",
		armed ? 1 : 0,
		listener_fd,
		(long long)core_age_ms,
		(long long)cmdline_age_ms,
		(long long)grace_remaining_ms,
		(long long)now);
}

static void linkr_debugger_watchdog_supervisor_thread_main(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		int64_t now = k_uptime_get();
		int64_t core_age_ms;
		int64_t cmdline_age_ms;
		int64_t grace_remaining_ms;
		int listener_fd;
		bool diagnostic_due;
		bool healthy;
		bool armed;
		const char *failing_service;
		const char *feed_status;
		bool heartbeat_active;
		int feed_ret = 0;

		diagnostic_due = linkr_debugger_watchdog_diagnostic_due(now);

		k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
		if (linkr_debugger_watchdog_planned_reboot_pending) {
			k_mutex_unlock(&linkr_debugger_control_lock);
			k_sleep(K_MSEC(LINKR_DEBUGGER_WATCHDOG_SUPERVISOR_PERIOD_MS));
			continue;
		}
		healthy = linkr_debugger_watchdog_services_healthy_locked();
		core_age_ms = linkr_debugger_watchdog_age_ms(now, linkr_debugger_watchdog_core_alive_ms);
		cmdline_age_ms = linkr_debugger_watchdog_age_ms(now, linkr_debugger_watchdog_cmdline_alive_ms);
		grace_remaining_ms = linkr_debugger_watchdog_started_ms != 0 ?
			(LINKR_DEBUGGER_WATCHDOG_STARTUP_GRACE_MS - (now - linkr_debugger_watchdog_started_ms)) : -1;
		if (grace_remaining_ms < 0) {
			grace_remaining_ms = 0;
		}
		listener_fd = linkr_debugger_http_listener_fd();
		linkr_debugger_http_reap_stale_holders();
		failing_service = linkr_debugger_watchdog_failing_service;
		if (linkr_debugger_watchdog_failing_service != linkr_debugger_watchdog_last_reported_service) {
			linkr_debugger_watchdog_last_reported_service = linkr_debugger_watchdog_failing_service;
			if (linkr_debugger_watchdog_failing_service != NULL) {
				LOG_ERR("watchdog unhealthy: service=%s listener_fd=%d core_age_ms=%lld cmd_age_ms=%lld uptime_ms=%lld",
					linkr_debugger_watchdog_failing_service,
					listener_fd,
					(long long)core_age_ms,
					(long long)cmdline_age_ms,
					(long long)now);
			} else {
				LOG_INF("watchdog healthy again: listener_fd=%d core_age_ms=%lld cmd_age_ms=%lld uptime_ms=%lld",
					listener_fd,
					(long long)core_age_ms,
					(long long)cmdline_age_ms,
					(long long)now);
			}
		}
		if (!healthy) {
			watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_SOURCE_SCRATCH] =
				LINKR_DEBUGGER_BOOTLOADER_SOURCE_WATCHDOG;
			{
				int64_t since_feed_ms = linkr_debugger_watchdog_last_feed_ms != 0 ?
					(now - linkr_debugger_watchdog_last_feed_ms) : -1;
				int64_t reset_remaining_ms = since_feed_ms >= 0 ?
					(int64_t)LINKR_DEBUGGER_WATCHDOG_TIMEOUT_MS - since_feed_ms : -1;

				if (reset_remaining_ms > 0 && reset_remaining_ms <= 500) {
					LOG_ERR("watchdog reset imminent: remaining=%lldms since_feed=%lldms blocker=%s",
						(long long)reset_remaining_ms,
						(long long)since_feed_ms,
						linkr_debugger_watchdog_failing_service != NULL ?
							linkr_debugger_watchdog_failing_service : "?");
				}
			}
		}
		if (healthy) {
			feed_ret = linkr_debugger_watchdog_hw_feed_locked();
			feed_status = feed_ret < 0 ? "failed" : "ok";
		} else {
			feed_status = "skipped";
		}
		heartbeat_active = linkr_debugger_watchdog_heartbeat_step_locked(healthy && feed_ret == 0);
		armed = linkr_debugger_watchdog_armed;
		k_mutex_unlock(&linkr_debugger_control_lock);

		linkr_debugger_heartbeat_led_set(heartbeat_active);

		if (diagnostic_due) {
			linkr_debugger_watchdog_log_diagnostics(now, feed_status, healthy, failing_service,
								armed, listener_fd, core_age_ms,
								cmdline_age_ms,
								grace_remaining_ms);
		}
		k_sleep(K_MSEC(LINKR_DEBUGGER_WATCHDOG_SUPERVISOR_PERIOD_MS));
	}
}

void linkr_debugger_watchdog_boot_check(void)
{
	if ((watchdog_hw->reason & WATCHDOG_REASON_TIMER_BITS) != 0U &&
	    watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_SCRATCH_INDEX] ==
	    LINKR_DEBUGGER_WATCHDOG_BOOTSEL_MARKER) {
		uint32_t source = watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_SOURCE_SCRATCH];
		const char *desc;

		if (linkr_debugger_watchdog_ota_test_marker_present()) {
			printk("watchdog reset after OTA test: allowing MCUboot rollback path\n");
			linkr_debugger_watchdog_marker_clear();
			return;
		}

		if (source == LINKR_DEBUGGER_BOOTLOADER_SOURCE_EXPLICIT) {
			desc = "explicit";
		} else if (source == LINKR_DEBUGGER_BOOTLOADER_SOURCE_WATCHDOG) {
			desc = "unhealthy";
		} else {
			desc = "unknown";
		}
		printk("boot to BOOTSEL: reason=timer marker=match source=%s(0x%08x)\n",
		       desc, source);
		linkr_debugger_watchdog_marker_clear();
		reset_usb_boot(0, 0);
	}
}

static int linkr_debugger_watchdog_install_locked(uint32_t timeout_ms)
{
	struct wdt_timeout_cfg config = {
		.window = {
			.min = 0U,
			.max = timeout_ms,
		},
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int ret;

	if (watchdog_dev == NULL || !device_is_ready(watchdog_dev)) {
		return -ENODEV;
	}

	linkr_debugger_watchdog_marker_set();
	ret = wdt_install_timeout(watchdog_dev, &config);
	if (ret < 0) {
		linkr_debugger_watchdog_marker_clear();
		return ret;
	}

	linkr_debugger_watchdog_channel = ret;
	ret = wdt_setup(watchdog_dev, 0U);
	if (ret < 0) {
		linkr_debugger_watchdog_marker_clear();
		linkr_debugger_watchdog_channel = -1;
		return ret;
	}

	linkr_debugger_watchdog_armed = true;
	linkr_debugger_watchdog_boot_timeout_active =
		(timeout_ms != LINKR_DEBUGGER_WATCHDOG_TIMEOUT_MS);
	return 0;
}

static int linkr_debugger_watchdog_arm_locked(void)
{
	if (linkr_debugger_watchdog_armed) {
		return 0;
	}

	return linkr_debugger_watchdog_install_locked(LINKR_DEBUGGER_WATCHDOG_TIMEOUT_MS);
}

int linkr_debugger_watchdog_early_arm(void)
{
	int ret;

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	if (linkr_debugger_watchdog_armed) {
		k_mutex_unlock(&linkr_debugger_control_lock);
		return 0;
	}
	ret = linkr_debugger_watchdog_install_locked(LINKR_DEBUGGER_WATCHDOG_BOOT_TIMEOUT_MS);
	k_mutex_unlock(&linkr_debugger_control_lock);

	return ret;
}

static int read_adc_debug(const struct linkr_debugger_current_desc *current,
			  struct linkr_debugger_current_sample *sample)
{
	const struct adc_dt_spec *spec;
	struct adc_sequence sequence = {
		.buffer = &sample->raw,
		.buffer_size = sizeof(sample->raw),
	};
	int32_t mv;
	int ret;

	if (current == NULL || sample == NULL || current->adc_index >= ARRAY_SIZE(adc_channels)) {
		return -EINVAL;
	}

	spec = &adc_channels[current->adc_index];
	ret = adc_sequence_init_dt(spec, &sequence);
	if (ret < 0) {
		LOG_ERR("%s: adc_sequence_init_dt failed: %d", current->name, ret);
		return ret;
	}

	ret = adc_read_dt(spec, &sequence);
	if (ret < 0) {
		LOG_ERR("%s: adc_read_dt failed: %d", current->name, ret);
		return ret;
	}

	mv = sample->raw;
	ret = adc_raw_to_millivolts_dt(spec, &mv);
	if (ret < 0) {
		uint8_t resolution = spec->resolution != 0U ? spec->resolution : 12U;

		mv = (sample->raw * 3300) / ((1 << resolution) - 1);
	}

	sample->raw_available = true;
	sample->mv = mv;

	return 0;
}

const char *linkr_debugger_json_schema(void)
{
	return LINKR_DEBUGGER_JSON_SCHEMA;
}

const char *linkr_debugger_power_capture_protocol(void)
{
	return LINKR_DEBUGGER_POWER_CAPTURE_PROTOCOL;
}

const char *linkr_debugger_mcu_name(void)
{
	return LINKR_DEBUGGER_MCU_NAME;
}

const char *linkr_debugger_reserved_gpios(void)
{
	return LINKR_DEBUGGER_RESERVED_GPIOS;
}

const char *linkr_debugger_usb_mode(void)
{
	return LINKR_DEBUGGER_USB_MODE;
}

int linkr_debugger_control_init(void)
{
	int ret;

	if (!device_is_ready(gpio0)) {
		return -ENODEV;
	}

	ret = configure_regulator_defaults();
	if (ret < 0) {
		return ret;
	}

	ret = configure_sd_default();
	if (ret < 0) {
		return ret;
	}

	ret = configure_usb_mux_default();
	if (ret < 0) {
		return ret;
	}

	ret = configure_tf_wp_default();
	if (ret < 0) {
		return ret;
	}

	ret = setup_adc_channels();
	if (ret < 0) {
		return ret;
	}

	ret = configure_heartbeat_led();
	if (ret < 0) {
		return ret;
	}

	if (linkr_debugger_safe_gpio_count > ARRAY_SIZE(linkr_debugger_gpio_directions)) {
		return -ERANGE;
	}

	for (size_t i = 0; i < linkr_debugger_safe_gpio_count; i++) {
		linkr_debugger_gpio_directions[i] = LINKR_DEBUGGER_GPIO_DIR_INPUT;
	}

	return 0;
}

bool linkr_debugger_power_output_enabled(const struct linkr_debugger_rail_desc *rail)
{
	size_t index;

	if (rail == NULL || !rail->controllable) {
		return false;
	}
	if (linkr_debugger_rail_initial_enabled(rail)) {
		return true;
	}

	index = regulator_index(rail);
	if (index >= linkr_debugger_rail_count || index >= ARRAY_SIZE(regulator_states)) {
		return false;
	}

	return regulator_states[index];
}

int linkr_debugger_power_output_set(const struct linkr_debugger_rail_desc *rail, bool enabled)
{
	const struct device *regulator;
	int ret;

	if (rail == NULL) {
		return -EINVAL;
	}

	if (!rail->controllable) {
		return -EPERM;
	}
	if (!linkr_debugger_rail_state_allowed(rail, enabled)) {
		return -EPERM;
	}
	if (linkr_debugger_rail_initial_enabled(rail)) {
		return enabled ? 0 : -EPERM;
	}

	regulator = linkr_debugger_regulator_device(rail);
	if (regulator == NULL || !device_is_ready(regulator)) {
		return -ENODEV;
	}

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);

	if (linkr_debugger_power_output_enabled(rail) == enabled) {
		k_mutex_unlock(&linkr_debugger_control_lock);
		return 0;
	}

	ret = enabled ? regulator_enable(regulator) : regulator_disable(regulator);
	if (ret < 0) {
		k_mutex_unlock(&linkr_debugger_control_lock);
		return ret;
	}

	set_regulator_state(rail, enabled);
	k_mutex_unlock(&linkr_debugger_control_lock);
	return 0;
}

enum linkr_debugger_sd_route linkr_debugger_sd_route_get(void)
{
	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	enum linkr_debugger_sd_route route = linkr_debugger_sd_route;
	k_mutex_unlock(&linkr_debugger_control_lock);
	return route;
}

const char *linkr_debugger_sd_route_name(void)
{
	return linkr_debugger_sd_route_get() == LINKR_DEBUGGER_SD_ROUTE_USB_READER ?
	       "usb-reader" : "target";
}

enum linkr_debugger_usb_route linkr_debugger_usb_route_get(void)
{
	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	enum linkr_debugger_usb_route route = linkr_debugger_usb_route;
	k_mutex_unlock(&linkr_debugger_control_lock);
	return route;
}

const char *linkr_debugger_usb_route_name(void)
{
	return linkr_debugger_usb_route_get() == LINKR_DEBUGGER_USB_ROUTE_TARGET ?
	       "target" : "pc";
}

enum linkr_debugger_tf_wp_route linkr_debugger_tf_wp_route_get(void)
{
	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	enum linkr_debugger_tf_wp_route route = linkr_debugger_tf_wp_route;
	k_mutex_unlock(&linkr_debugger_control_lock);
	return route;
}

const char *linkr_debugger_tf_wp_route_name(void)
{
	return linkr_debugger_tf_wp_route_to_string(linkr_debugger_tf_wp_route_get());
}

int linkr_debugger_sd_route_set(enum linkr_debugger_sd_route route)
{
	int ret;

	if (!device_is_ready(gpio0)) {
		return -ENODEV;
	}

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	ret = gpio_pin_set(gpio0, 4, route == LINKR_DEBUGGER_SD_ROUTE_TARGET ? 1 : 0);
	if (ret < 0) {
		k_mutex_unlock(&linkr_debugger_control_lock);
		return ret;
	}

	linkr_debugger_sd_route = route;

	k_msleep(10);
	k_mutex_unlock(&linkr_debugger_control_lock);
	return 0;
}

int linkr_debugger_usb_route_set(enum linkr_debugger_usb_route route)
{
	const struct linkr_debugger_rail_desc *vdd_rail = linkr_debugger_find_rail("vdd_5v");
	bool vdd_enable = route == LINKR_DEBUGGER_USB_ROUTE_PC;
	int ret;

	if (!device_is_ready(gpio0)) {
		return -ENODEV;
	}

	/* The VDD_5V hub domain must be live before the mux attaches it. */
	if (vdd_enable) {
		ret = linkr_debugger_power_output_set(vdd_rail, true);
		if (ret < 0) {
			return ret;
		}
	}

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	ret = gpio_pin_set(gpio0, 5, route == LINKR_DEBUGGER_USB_ROUTE_TARGET ? 1 : 0);
	if (ret < 0) {
		k_mutex_unlock(&linkr_debugger_control_lock);
		return ret;
	}

	linkr_debugger_usb_route = route;

	k_msleep(10);
	k_mutex_unlock(&linkr_debugger_control_lock);

	/* Detach the mux first, then cut the VDD_5V hub domain. */
	if (!vdd_enable) {
		ret = linkr_debugger_power_output_set(vdd_rail, false);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

int linkr_debugger_tf_wp_route_set(enum linkr_debugger_tf_wp_route route)
{
	int ret;

	if (!device_is_ready(gpio0)) {
		return -ENODEV;
	}

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	ret = gpio_pin_set(gpio0, 22, route == LINKR_DEBUGGER_TF_WP_ROUTE_WRITABLE ? 1 : 0);
	if (ret < 0) {
		k_mutex_unlock(&linkr_debugger_control_lock);
		return ret;
	}

	linkr_debugger_tf_wp_route = route;

	k_msleep(10);
	k_mutex_unlock(&linkr_debugger_control_lock);
	return 0;
}

int linkr_debugger_current_read(const struct linkr_debugger_current_desc *current,
				    struct linkr_debugger_current_sample *sample)
{
	const struct linkr_debugger_rail_desc *rail;
	const struct device *sensor;
	struct sensor_value value;
	int ret;

	if (current == NULL || sample == NULL) {
		return -EINVAL;
	}

	sample->raw_available = false;
	sample->voltage_valid = false;
	sample->raw = 0;
	sample->mv = 0;
	sample->voltage_uv = 0;
	sample->current_ua = 0;
	sample->value.val1 = 0;
	sample->value.val2 = 0;

	switch (current->kind) {
	case LINKR_DEBUGGER_ADC_KIND_CURRENT:
		sensor = linkr_debugger_current_sensor(current);
		if (sensor == NULL || !device_is_ready(sensor)) {
			return -ENODEV;
		}

		k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
		rail = linkr_debugger_find_rail(current->name);
		sample->rail_enabled = true;
		if (rail != NULL && rail->controllable) {
			sample->rail_enabled = linkr_debugger_power_output_enabled(rail);
		}

		ret = sensor_sample_fetch_chan(sensor, SENSOR_CHAN_CURRENT);
		if (ret < 0) {
			k_mutex_unlock(&linkr_debugger_control_lock);
			return ret;
		}

		ret = sensor_channel_get(sensor, SENSOR_CHAN_CURRENT, &value);
		if (ret < 0) {
			k_mutex_unlock(&linkr_debugger_control_lock);
			return ret;
		}

		sample->value = value;
		sample->current_ua = sensor_value_to_micro_units(&value);

		/* INA226 also reports the bus voltage for this rail. */
		ret = sensor_sample_fetch_chan(sensor, SENSOR_CHAN_VOLTAGE);
		if (ret == 0) {
			ret = sensor_channel_get(sensor, SENSOR_CHAN_VOLTAGE, &value);
		}
		if (ret == 0) {
			sample->voltage_uv = sensor_value_to_micro_units(&value);
			sample->mv = sample->voltage_uv / 1000;
			sample->voltage_valid = true;
		}
		k_mutex_unlock(&linkr_debugger_control_lock);
		return 0;
	case LINKR_DEBUGGER_ADC_KIND_VOLTAGE: {
		int32_t microvolts;

		k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
		sample->rail_enabled = true;
		ret = read_adc_debug(current, sample);
		if (ret == 0) {
			microvolts = sample->raw;
			if (adc_raw_to_microvolts_dt(&adc_channels[current->adc_index],
					       &microvolts) < 0) {
				microvolts = sample->mv * 1000;
			}
			(void)sensor_value_from_micro(&sample->value, microvolts);
		}
		k_mutex_unlock(&linkr_debugger_control_lock);
		return ret;
	}
	default:
		return -EINVAL;
	}
}

int linkr_debugger_current_read_batch(struct linkr_debugger_current_sample *samples,
					 size_t batch_count,
					 size_t channel_count,
					 int64_t *timestamps_us,
					 uint32_t interval_us)
{
	int ret;

	if (samples == NULL || timestamps_us == NULL || batch_count == 0U ||
	    batch_count > LINKR_DEBUGGER_CURRENT_BATCH_MAX ||
	    (channel_count != linkr_debugger_adc_count &&
	     channel_count != linkr_debugger_current_count)) {
		return -EINVAL;
	}

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	for (size_t sample_index = 0; sample_index < batch_count; sample_index++) {
		timestamps_us[sample_index] = k_ticks_to_us_floor64(k_uptime_ticks());

		for (size_t i = 0; i < channel_count; i++) {
			struct linkr_debugger_current_sample *sample =
				&samples[sample_index * channel_count + i];
			const struct linkr_debugger_current_desc *current =
				&linkr_debugger_currents[i];
			const struct linkr_debugger_rail_desc *rail =
				linkr_debugger_find_rail(current->name);

			memset(sample, 0, sizeof(*sample));
			sample->rail_enabled = rail == NULL || !rail->controllable ||
				linkr_debugger_power_output_enabled(rail);

			if (current->kind == LINKR_DEBUGGER_ADC_KIND_CURRENT) {
				const struct device *sensor = linkr_debugger_current_sensor(current);
				struct sensor_value value;
				int sensor_ret;

				if (sensor == NULL || !device_is_ready(sensor)) {
					ret = -ENODEV;
					goto out;
				}
				sensor_ret = sensor_sample_fetch_chan(sensor, SENSOR_CHAN_CURRENT);
				if (sensor_ret < 0) {
					ret = sensor_ret;
					goto out;
				}
				sensor_ret = sensor_channel_get(sensor, SENSOR_CHAN_CURRENT, &value);
				if (sensor_ret < 0) {
					ret = sensor_ret;
					goto out;
				}
				sample->value = value;
				sample->current_ua = sensor_value_to_micro_units(&value);

				/* INA226 also reports the bus voltage for this rail. */
				sensor_ret = sensor_sample_fetch_chan(sensor, SENSOR_CHAN_VOLTAGE);
				if (sensor_ret == 0) {
					sensor_ret = sensor_channel_get(sensor, SENSOR_CHAN_VOLTAGE,
								&value);
				}
				if (sensor_ret == 0) {
					sample->voltage_uv = sensor_value_to_micro_units(&value);
					sample->mv = sample->voltage_uv / 1000;
					sample->voltage_valid = true;
				}
			} else if (current->kind == LINKR_DEBUGGER_ADC_KIND_VOLTAGE) {
				struct adc_sequence sequence = {
					.buffer = &sample->raw,
					.buffer_size = sizeof(sample->raw),
				};
				int32_t microvolts;

				if (current->adc_index >= ARRAY_SIZE(adc_channels)) {
					ret = -ERANGE;
					goto out;
				}
				ret = adc_sequence_init_dt(&adc_channels[current->adc_index],
							   &sequence);
				if (ret < 0) {
					goto out;
				}
				ret = adc_read_dt(&adc_channels[current->adc_index], &sequence);
				if (ret < 0) {
					goto out;
				}
				sample->raw_available = true;
				microvolts = sample->raw;
				if (adc_raw_to_microvolts_dt(&adc_channels[current->adc_index],
							     &microvolts) < 0) {
					uint8_t resolution =
						adc_channels[current->adc_index].resolution != 0U ?
						adc_channels[current->adc_index].resolution : 12U;

					microvolts = (sample->raw * 3300 * 1000) /
						((1 << resolution) - 1);
				}
				(void)sensor_value_from_micro(&sample->value, microvolts);
			} else {
				ret = -EINVAL;
				goto out;
			}
		}

		if (interval_us != 0U && sample_index + 1U < batch_count) {
			k_busy_wait(interval_us);
		}
	}
	ret = 0;

out:
	k_mutex_unlock(&linkr_debugger_control_lock);
	return ret;
}

int linkr_debugger_current_read_all(struct linkr_debugger_current_sample *samples,
				       size_t sample_count)
{
	int64_t timestamp_us;

	return linkr_debugger_current_read_batch(samples, 1U, sample_count,
					       &timestamp_us, 0U);
}

int linkr_debugger_gpio_get(const struct linkr_debugger_safe_gpio_desc *desc, int *value)
{
	size_t index;

	if (value == NULL) {
		return -EINVAL;
	}

	if (!safe_gpio_index_valid(desc, &index)) {
		return -ERANGE;
	}
	if (!device_is_ready(gpio0)) {
		return -ENODEV;
	}

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	if (linkr_debugger_gpio_directions[index] == LINKR_DEBUGGER_GPIO_DIR_OUTPUT) {
		*value = linkr_debugger_gpio_output_levels[index] ? 1 : 0;
	} else {
		*value = gpio_pin_get(gpio0, desc->pin);
		if (*value < 0) {
			k_mutex_unlock(&linkr_debugger_control_lock);
			return *value;
		}
	}
	k_mutex_unlock(&linkr_debugger_control_lock);

	return 0;
}

int linkr_debugger_gpio_set_output(const struct linkr_debugger_safe_gpio_desc *desc, bool value)
{
	size_t index;
	int ret;

	if (!safe_gpio_index_valid(desc, &index)) {
		return -ERANGE;
	}
	if (!desc->output_capable) {
		return -EPERM;
	}
	if (!device_is_ready(gpio0)) {
		return -ENODEV;
	}

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	ret = gpio_pin_configure(gpio0, desc->pin,
				 value ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
	if (ret >= 0) {
		linkr_debugger_gpio_apply_safe_drive_strength(desc);
		linkr_debugger_gpio_directions[index] = LINKR_DEBUGGER_GPIO_DIR_OUTPUT;
		linkr_debugger_gpio_output_levels[index] = value;
	}
	k_mutex_unlock(&linkr_debugger_control_lock);
	return ret;
}

int linkr_debugger_gpio_set_input(const struct linkr_debugger_safe_gpio_desc *desc)
{
	size_t index;
	int ret;

	if (!safe_gpio_index_valid(desc, &index)) {
		return -ERANGE;
	}
	if (!desc->output_capable) {
		return 0;
	}
	if (!device_is_ready(gpio0)) {
		return -ENODEV;
	}

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	ret = gpio_pin_configure(gpio0, desc->pin, GPIO_INPUT);
	if (ret >= 0) {
		linkr_debugger_gpio_apply_safe_drive_strength(desc);
		linkr_debugger_gpio_directions[index] = LINKR_DEBUGGER_GPIO_DIR_INPUT;
	}
	k_mutex_unlock(&linkr_debugger_control_lock);
	return ret;
}

int linkr_debugger_control_snapshot_get(struct linkr_debugger_control_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return -EINVAL;
	}
	memset(snapshot, 0, sizeof(*snapshot));

	if (linkr_debugger_config_item_count > LINKR_DEBUGGER_CONFIG_MAX_ENTRIES) {
		return -ERANGE;
	}

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	snapshot->item_count = linkr_debugger_config_item_count;
	for (size_t i = 0; i < linkr_debugger_config_item_count; i++) {
		const struct linkr_debugger_config_item_desc *item =
			&linkr_debugger_config_items[i];
		struct linkr_debugger_control_item_state *state = &snapshot->items[i];

		state->domain = item->domain;
		state->item_id = item->item_id;

		switch (item->domain) {
		case LINKR_DEBUGGER_CONFIG_DOMAIN_POWER: {
			const char *separator = strchr(item->id, '/');
			const struct linkr_debugger_rail_desc *rail = separator != NULL ?
				linkr_debugger_find_rail(separator + 1) : NULL;
			const struct device *regulator = linkr_debugger_regulator_device(rail);

			if (regulator != NULL && device_is_ready(regulator)) {
				state->available = true;
				state->value = linkr_debugger_power_output_enabled(rail) ?
					LINKR_DEBUGGER_CONFIG_POWER_ON :
					LINKR_DEBUGGER_CONFIG_POWER_OFF;
			}
			break;
		}
		case LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH:
			switch (item->item_id) {
			case LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID:
				if (device_is_ready(gpio0)) {
					state->available = true;
					state->value = linkr_debugger_sd_route ==
						LINKR_DEBUGGER_SD_ROUTE_USB_READER ?
						LINKR_DEBUGGER_CONFIG_SD_USB_READER :
						LINKR_DEBUGGER_CONFIG_SD_TARGET;
				}
				break;
			case LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID:
				if (device_is_ready(gpio0)) {
					state->available = true;
					state->value = linkr_debugger_usb_route ==
						LINKR_DEBUGGER_USB_ROUTE_PC ?
						LINKR_DEBUGGER_CONFIG_USB_PC :
						LINKR_DEBUGGER_CONFIG_USB_TARGET;
				}
				break;
			case LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID:
				if (device_is_ready(gpio0)) {
					state->available = true;
					state->value = linkr_debugger_tf_wp_route ==
						LINKR_DEBUGGER_TF_WP_ROUTE_PROTECTED ?
						LINKR_DEBUGGER_CONFIG_TF_WP_PROTECTED :
						LINKR_DEBUGGER_CONFIG_TF_WP_WRITABLE;
				}
				break;
			default:
				break;
			}
			break;
		case LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO: {
			const struct linkr_debugger_safe_gpio_desc *gpio =
				linkr_debugger_find_safe_gpio_by_pin(item->item_id);
			size_t index;

			if (device_is_ready(gpio0) && safe_gpio_index_valid(gpio, &index)) {
				state->available = true;
				if (linkr_debugger_gpio_directions[index] ==
				    LINKR_DEBUGGER_GPIO_DIR_OUTPUT) {
					state->value = LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT;
					if (linkr_debugger_gpio_output_levels[index]) {
						state->value |= LINKR_DEBUGGER_CONFIG_GPIO_LEVEL;
					}
				}
			}
			break;
		}
		default:
			break;
		}
	}
	k_mutex_unlock(&linkr_debugger_control_lock);

	return 0;
}

void linkr_debugger_watchdog_status_get(struct linkr_debugger_watchdog_status *status)
{
	if (status == NULL) {
		return;
	}

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	status->supported = watchdog_dev != NULL && device_is_ready(watchdog_dev);
	status->armed = linkr_debugger_watchdog_armed;
	status->automatic = true;
	status->healthy = linkr_debugger_watchdog_services_healthy_locked();
	status->timeout_ms = linkr_debugger_watchdog_boot_timeout_active ?
		LINKR_DEBUGGER_WATCHDOG_BOOT_TIMEOUT_MS : LINKR_DEBUGGER_WATCHDOG_TIMEOUT_MS;
	status->bootloader_on_timeout = status->supported;
#if defined(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION)
	status->fault_injection_available = true;
	linkr_debugger_watchdog_fault_injection_expire_locked(k_uptime_get());
	status->fault_injection_armed = linkr_debugger_watchdog_fault_injection_armed_state;
#else
	status->fault_injection_available = false;
	status->fault_injection_armed = false;
#endif
	status->failing_service = linkr_debugger_watchdog_failing_service;
	k_mutex_unlock(&linkr_debugger_control_lock);
}

bool linkr_debugger_watchdog_fault_injection_available(void)
{
	return IS_ENABLED(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION);
}

bool linkr_debugger_watchdog_fault_injection_armed(void)
{
#if defined(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION)
	bool armed;

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	linkr_debugger_watchdog_fault_injection_expire_locked(k_uptime_get());
	armed = linkr_debugger_watchdog_fault_injection_armed_state;
	k_mutex_unlock(&linkr_debugger_control_lock);
	return armed;
#else
	return false;
#endif
}

int linkr_debugger_watchdog_fault_injection_arm(void)
{
#if defined(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION)
	int64_t now;

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	if (linkr_debugger_watchdog_fault_injection_armed_state) {
		k_mutex_unlock(&linkr_debugger_control_lock);
		return -EALREADY;
	}
	if (linkr_debugger_watchdog_fault_injection_confirm_pending) {
		k_mutex_unlock(&linkr_debugger_control_lock);
		return -EBUSY;
	}
	if (!linkr_debugger_watchdog_ota_test_marker_present()) {
		k_mutex_unlock(&linkr_debugger_control_lock);
		return -EINVAL;
	}
	now = k_uptime_get();
	linkr_debugger_watchdog_fault_injection_armed_state = true;
	linkr_debugger_watchdog_fault_injection_expires_ms =
		now + LINKR_DEBUGGER_WATCHDOG_FAULT_INJECTION_TIMEOUT_MS;
	linkr_debugger_watchdog_last_feed_ms = now;
	k_mutex_unlock(&linkr_debugger_control_lock);
	LOG_WRN("watchdog fault injection armed for OTA rollback validation");
	return 0;
#else
	return -ENOTSUP;
#endif
}

bool linkr_debugger_watchdog_fault_injection_confirm_begin(void)
{
#if defined(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION)
	bool can_confirm;

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	linkr_debugger_watchdog_fault_injection_expire_locked(k_uptime_get());
	can_confirm = !linkr_debugger_watchdog_fault_injection_armed_state;
	if (can_confirm) {
		linkr_debugger_watchdog_fault_injection_confirm_pending = true;
	}
	k_mutex_unlock(&linkr_debugger_control_lock);
	return can_confirm;
#else
	return true;
#endif
}

void linkr_debugger_watchdog_fault_injection_confirm_end(void)
{
#if defined(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION)
	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	linkr_debugger_watchdog_fault_injection_confirm_pending = false;
	k_mutex_unlock(&linkr_debugger_control_lock);
#endif
}

int linkr_debugger_watchdog_fault_injection_disarm(void)
{
#if defined(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION)
	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	linkr_debugger_watchdog_fault_injection_armed_state = false;
	linkr_debugger_watchdog_fault_injection_expires_ms = 0;
	k_mutex_unlock(&linkr_debugger_control_lock);
	return 0;
#else
	return -ENOTSUP;
#endif
}

void linkr_debugger_watchdog_note_core_alive(void)
{
	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	linkr_debugger_watchdog_core_alive_ms = k_uptime_get();
	k_mutex_unlock(&linkr_debugger_control_lock);
}

void linkr_debugger_watchdog_note_cmdline_alive(void)
{
	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	linkr_debugger_watchdog_cmdline_alive_ms = k_uptime_get();
	k_mutex_unlock(&linkr_debugger_control_lock);
}

void linkr_debugger_watchdog_note_ws_alive(void)
{
	/* Websocket traffic no longer participates in watchdog health decisions. */
}

void linkr_debugger_watchdog_note_ws_client_active(bool active)
{
	ARG_UNUSED(active);
}

int linkr_debugger_watchdog_supervisor_start(void)
{
	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	if (linkr_debugger_watchdog_supervisor_started) {
		k_mutex_unlock(&linkr_debugger_control_lock);
		return 0;
	}
	linkr_debugger_watchdog_core_alive_ms = k_uptime_get();
	linkr_debugger_watchdog_cmdline_alive_ms = k_uptime_get();
	linkr_debugger_watchdog_started_ms = k_uptime_get();
	linkr_debugger_watchdog_ws_alive_ms = 0;
	linkr_debugger_watchdog_ws_client_active = false;
	linkr_debugger_watchdog_last_diagnostic_ms = 0;
	linkr_debugger_watchdog_last_feed_ms = 0;
	linkr_debugger_watchdog_failing_service = NULL;
	linkr_debugger_watchdog_last_reported_service = NULL;
#if defined(CONFIG_LINKR_DEBUGGER_FAULT_INJECTION)
	linkr_debugger_watchdog_fault_injection_armed_state = false;
	linkr_debugger_watchdog_fault_injection_expires_ms = 0;
	linkr_debugger_watchdog_fault_injection_confirm_pending = false;
#endif
	(void)linkr_debugger_watchdog_heartbeat_step_locked(false);
	linkr_debugger_watchdog_supervisor_started = true;
	k_mutex_unlock(&linkr_debugger_control_lock);

	linkr_debugger_heartbeat_led_set(false);

	k_thread_create(&linkr_debugger_watchdog_supervisor_thread,
				linkr_debugger_watchdog_supervisor_stack,
				K_THREAD_STACK_SIZEOF(linkr_debugger_watchdog_supervisor_stack),
				linkr_debugger_watchdog_supervisor_thread_main,
				NULL, NULL, NULL,
				K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	if (IS_ENABLED(CONFIG_THREAD_NAME)) {
		k_thread_name_set(&linkr_debugger_watchdog_supervisor_thread,
					  "linkr_debugger_wdt");
	}

	return 0;
}

/*
 * The INA226 driver probes I2C from SENSOR_INIT (priority 90), before main()
 * runs. Arm the watchdog and start its supervisor first (priority 80, after
 * the WDT device at PRE_KERNEL_1) so a probe that blocks the bus still trips
 * the hardware watchdog and the next boot takes the BOOTSEL marker path.
 */
static int linkr_debugger_watchdog_bootstrap(void)
{
	linkr_debugger_watchdog_boot_check();
	(void)linkr_debugger_watchdog_early_arm();
	return linkr_debugger_watchdog_supervisor_start();
}

SYS_INIT(linkr_debugger_watchdog_bootstrap, POST_KERNEL, 80);

int linkr_debugger_bootloader_now(void)
{
	int ret = 0;

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	if (linkr_debugger_watchdog_armed && watchdog_dev != NULL && device_is_ready(watchdog_dev)) {
		(void)wdt_feed(watchdog_dev, linkr_debugger_watchdog_channel);
	}
	linkr_debugger_watchdog_marker_clear();
	linkr_debugger_watchdog_ota_test_marker_clear();
	if (linkr_debugger_watchdog_armed && watchdog_dev != NULL && device_is_ready(watchdog_dev)) {
		ret = wdt_disable(watchdog_dev);
	}
	ARG_UNUSED(ret);
	linkr_debugger_watchdog_force_disable_locked();
	k_mutex_unlock(&linkr_debugger_control_lock);
	printk("explicit %s BOOTSEL entry\n", linkr_debugger_mcu_name());
	reset_usb_boot(0, 0);
	return 0;
}
