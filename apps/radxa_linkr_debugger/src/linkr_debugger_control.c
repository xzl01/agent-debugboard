/*
 * SPDX-License-Identifier: Apache-2.0
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
#include <zephyr/sys/util.h>
#include <string.h>

LOG_MODULE_REGISTER(linkr_debugger_control, LOG_LEVEL_INF);

#if defined(CONFIG_SOC_SERIES_RP2040) || defined(CONFIG_SOC_SERIES_RP2350)
#include <hardware/regs/watchdog.h>
#include <hardware/watchdog.h>
#include <hardware/gpio.h>
#include <pico/bootrom.h>
#endif

#define GPIO0_NODE DT_NODELABEL(gpio0)
#define ADC_INPUTS_NODE DT_PATH(zephyr_user)
#define REGULATOR_12V_OUT_NODE DT_NODELABEL(reg_12v_out)
#define REGULATOR_5V_OUT_NODE DT_NODELABEL(reg_5v_out)
#define REGULATOR_5V_WS_NODE DT_NODELABEL(reg_5v_ws)
#define REGULATOR_20V_OUT_NODE DT_NODELABEL(reg_20v_out)
#define CURRENT_5V_OUT_NODE DT_NODELABEL(sense_5v_out)
#define CURRENT_12V_OUT_NODE DT_NODELABEL(sense_12v_out)
#define CURRENT_20V_OUT_NODE DT_NODELABEL(sense_20v_out)
#define WATCHDOG_NODE DT_NODELABEL(wdt0)

#define LINKR_DEBUGGER_JSON_SCHEMA "radxa-linkr-debugger.v1"
#define LINKR_DEBUGGER_USB_MODE "ncm-http"
#define LINKR_DEBUGGER_RESERVED_GPIOS "GP02 GP03 GP05 GP06 GP09 GP10 GP26-GP28"

#define LINKR_DEBUGGER_GPIO_DIR_INPUT 1
#define LINKR_DEBUGGER_GPIO_DIR_OUTPUT 2

#define REGULATOR_STATE_CAPACITY 8U
#define LINKR_DEBUGGER_WATCHDOG_TIMEOUT_MS 5000U
#define LINKR_DEBUGGER_WATCHDOG_SUPERVISOR_PERIOD_MS 250U
#define LINKR_DEBUGGER_WATCHDOG_STARTUP_GRACE_MS 15000U
#define LINKR_DEBUGGER_WATCHDOG_CORE_STALE_MS 1500U
#define LINKR_DEBUGGER_WATCHDOG_CMDLINE_STALE_MS 2000U
#define LINKR_DEBUGGER_WATCHDOG_WS_STALE_MS 1500U
#define LINKR_DEBUGGER_WATCHDOG_DIAGNOSTIC_PERIOD_MS 500U
#define LINKR_DEBUGGER_WATCHDOG_SCRATCH_INDEX 0U
#define LINKR_DEBUGGER_WATCHDOG_BOOTSEL_MARKER 0xadb00751U
#define LINKR_DEBUGGER_WATCHDOG_SOURCE_SCRATCH 1U
#define LINKR_DEBUGGER_BOOTLOADER_SOURCE_EXPLICIT 0xbfeed001U
#define LINKR_DEBUGGER_BOOTLOADER_SOURCE_WATCHDOG  0xbfeed002U

#define ADC_SPEC_AND_COMMA(node_id, prop, idx) \
	COND_CODE_1(DT_PHA_HAS_CELL_AT_IDX(node_id, prop, idx, input), \
			(ADC_DT_SPEC_GET_BY_IDX(node_id, idx),), ())

BUILD_ASSERT(DT_NODE_HAS_STATUS(REGULATOR_12V_OUT_NODE, okay));
BUILD_ASSERT(DT_NODE_HAS_STATUS(REGULATOR_5V_OUT_NODE, okay));
BUILD_ASSERT(DT_NODE_HAS_STATUS(REGULATOR_5V_WS_NODE, okay));
BUILD_ASSERT(DT_NODE_HAS_STATUS(REGULATOR_20V_OUT_NODE, okay));
BUILD_ASSERT(DT_NODE_HAS_STATUS(CURRENT_5V_OUT_NODE, okay));
BUILD_ASSERT(DT_NODE_HAS_STATUS(CURRENT_12V_OUT_NODE, okay));
BUILD_ASSERT(DT_NODE_HAS_STATUS(CURRENT_20V_OUT_NODE, okay));

static const struct device *const gpio0 = DEVICE_DT_GET(GPIO0_NODE);
static const struct device *const watchdog_dev = DEVICE_DT_GET_OR_NULL(WATCHDOG_NODE);
static bool regulator_states[REGULATOR_STATE_CAPACITY];
static enum linkr_debugger_sd_route linkr_debugger_sd_route = LINKR_DEBUGGER_SD_ROUTE_TARGET;
static enum linkr_debugger_usb_route linkr_debugger_usb_route = LINKR_DEBUGGER_USB_ROUTE_TARGET;
static uint8_t linkr_debugger_gpio_directions[16];
static bool linkr_debugger_gpio_output_levels[16];
static struct k_mutex linkr_debugger_control_lock;
static bool linkr_debugger_watchdog_armed;
static int linkr_debugger_watchdog_channel = -1;
static bool linkr_debugger_watchdog_supervisor_started;
static bool linkr_debugger_watchdog_ws_client_active;
static int64_t linkr_debugger_watchdog_core_alive_ms;
static int64_t linkr_debugger_watchdog_cmdline_alive_ms;
static int64_t linkr_debugger_watchdog_ws_alive_ms;
static int64_t linkr_debugger_watchdog_started_ms;
static int64_t linkr_debugger_watchdog_last_diagnostic_ms;
static int64_t linkr_debugger_watchdog_last_feed_ms;
static const char *linkr_debugger_watchdog_failing_service = NULL;
static const char *linkr_debugger_watchdog_last_reported_service;
static K_THREAD_STACK_DEFINE(linkr_debugger_watchdog_supervisor_stack, 1536);
static struct k_thread linkr_debugger_watchdog_supervisor_thread;

static const struct device *const regulators[] = {
	DEVICE_DT_GET(REGULATOR_12V_OUT_NODE),
	DEVICE_DT_GET(REGULATOR_5V_OUT_NODE),
	DEVICE_DT_GET(REGULATOR_5V_WS_NODE),
	DEVICE_DT_GET(REGULATOR_20V_OUT_NODE),
};

static const struct device *const current_sensors[] = {
	DEVICE_DT_GET(CURRENT_5V_OUT_NODE),
	DEVICE_DT_GET(CURRENT_12V_OUT_NODE),
	DEVICE_DT_GET(CURRENT_20V_OUT_NODE),
};

static const struct adc_dt_spec adc_channels[] = {
	DT_FOREACH_PROP_ELEM(ADC_INPUTS_NODE, io_channels, ADC_SPEC_AND_COMMA)
};

BUILD_ASSERT(ARRAY_SIZE(regulators) <= REGULATOR_STATE_CAPACITY);

static size_t regulator_index(const struct linkr_debugger_rail_desc *rail)
{
	return (size_t)(rail - linkr_debugger_rails);
}

static size_t safe_gpio_index(const struct linkr_debugger_safe_gpio_desc *desc)
{
	return (size_t)(desc - linkr_debugger_safe_gpios);
}

const char *linkr_debugger_safe_gpio_direction_name(size_t index)
{
	if (index >= linkr_debugger_safe_gpio_count) {
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
	if (current == NULL || current->adc_index >= ARRAY_SIZE(current_sensors)) {
		return NULL;
	}

	return current_sensors[current->adc_index];
}

static int32_t sensor_value_to_microamps(const struct sensor_value *value)
{
	int64_t microamps;

	microamps = ((int64_t)value->val1 * 1000000) + value->val2;
	return (int32_t)microamps;
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

		set_regulator_state(&linkr_debugger_rails[i], false);
	}

	return 0;
}

static int configure_sd_default(void)
{
	linkr_debugger_sd_route = LINKR_DEBUGGER_SD_ROUTE_TARGET;
	return gpio_pin_configure(gpio0, 6, GPIO_OUTPUT_INACTIVE);
}

static int configure_usb_mux_default(void)
{
	linkr_debugger_usb_route = LINKR_DEBUGGER_USB_ROUTE_TARGET;
	return gpio_pin_configure(gpio0, 3, GPIO_OUTPUT_ACTIVE);
}

static void linkr_debugger_gpio_apply_safe_drive_strength(const struct linkr_debugger_safe_gpio_desc *desc)
{
	ARG_UNUSED(desc);

#if defined(CONFIG_SOC_SERIES_RP2040) || defined(CONFIG_SOC_SERIES_RP2350)
	if (desc != NULL) {
		gpio_set_drive_strength(desc->pin, GPIO_DRIVE_STRENGTH_4MA);
	}
#endif
}

static int setup_current_sensors(void)
{
	if (linkr_debugger_current_count != ARRAY_SIZE(current_sensors)) {
		return -EINVAL;
	}

	if (linkr_debugger_current_count != ARRAY_SIZE(adc_channels)) {
		return -EINVAL;
	}

	for (size_t i = 0; i < ARRAY_SIZE(current_sensors); i++) {
		if (!device_is_ready(current_sensors[i])) {
			return -ENODEV;
		}

		if (!adc_is_ready_dt(&adc_channels[i])) {
			return -ENODEV;
		}

		if (adc_channel_setup_dt(&adc_channels[i]) < 0) {
			return -ENODEV;
		}
	}

	return 0;
}

static void linkr_debugger_watchdog_marker_set(void)
{
#if defined(CONFIG_SOC_SERIES_RP2040) || defined(CONFIG_SOC_SERIES_RP2350)
	watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_SCRATCH_INDEX] = LINKR_DEBUGGER_WATCHDOG_BOOTSEL_MARKER;
#endif
}

static void linkr_debugger_watchdog_marker_clear(void)
{
#if defined(CONFIG_SOC_SERIES_RP2040) || defined(CONFIG_SOC_SERIES_RP2350)
	watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_SCRATCH_INDEX] = 0U;
#endif
}

static void linkr_debugger_watchdog_force_disable_locked(void)
{
#if defined(CONFIG_SOC_SERIES_RP2040) || defined(CONFIG_SOC_SERIES_RP2350)
	hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
#endif
	linkr_debugger_watchdog_armed = false;
	linkr_debugger_watchdog_channel = -1;
}

static void linkr_debugger_watchdog_set_failing_service_locked(const char *service)
{
	linkr_debugger_watchdog_failing_service = service;
}

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

	if (linkr_debugger_watchdog_core_alive_ms == 0 ||
	    (now - linkr_debugger_watchdog_core_alive_ms) > LINKR_DEBUGGER_WATCHDOG_CORE_STALE_MS) {
		linkr_debugger_watchdog_set_failing_service_locked("core");
		return false;
	}

	if (linkr_debugger_watchdog_started_ms != 0 &&
	    (now - linkr_debugger_watchdog_started_ms) < LINKR_DEBUGGER_WATCHDOG_STARTUP_GRACE_MS) {
		linkr_debugger_watchdog_set_failing_service_locked(NULL);
		return true;
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
		int feed_ret = 0;

		diagnostic_due = linkr_debugger_watchdog_diagnostic_due(now);

		k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
		healthy = linkr_debugger_watchdog_services_healthy_locked();
		core_age_ms = linkr_debugger_watchdog_age_ms(now, linkr_debugger_watchdog_core_alive_ms);
		cmdline_age_ms = linkr_debugger_watchdog_age_ms(now, linkr_debugger_watchdog_cmdline_alive_ms);
		grace_remaining_ms = linkr_debugger_watchdog_started_ms != 0 ?
			(LINKR_DEBUGGER_WATCHDOG_STARTUP_GRACE_MS - (now - linkr_debugger_watchdog_started_ms)) : -1;
		if (grace_remaining_ms < 0) {
			grace_remaining_ms = 0;
		}
		listener_fd = linkr_debugger_http_listener_fd();
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
#if defined(CONFIG_SOC_SERIES_RP2040) || defined(CONFIG_SOC_SERIES_RP2350)
			watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_SOURCE_SCRATCH] =
				LINKR_DEBUGGER_BOOTLOADER_SOURCE_WATCHDOG;
#endif
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
		armed = linkr_debugger_watchdog_armed;
		k_mutex_unlock(&linkr_debugger_control_lock);

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
#if defined(CONFIG_SOC_SERIES_RP2040) || defined(CONFIG_SOC_SERIES_RP2350)
	if ((watchdog_hw->reason & WATCHDOG_REASON_TIMER_BITS) != 0U &&
	    watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_SCRATCH_INDEX] ==
	    LINKR_DEBUGGER_WATCHDOG_BOOTSEL_MARKER) {
		uint32_t source = watchdog_hw->scratch[LINKR_DEBUGGER_WATCHDOG_SOURCE_SCRATCH];
		const char *desc;

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
#endif
}

static int linkr_debugger_watchdog_arm_locked(void)
{
	struct wdt_timeout_cfg config = {
		.window = {
			.min = 0U,
			.max = LINKR_DEBUGGER_WATCHDOG_TIMEOUT_MS,
		},
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int ret;

	if (watchdog_dev == NULL || !device_is_ready(watchdog_dev)) {
		return -ENODEV;
	}

	if (linkr_debugger_watchdog_armed) {
		return 0;
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
	return 0;
}

static int read_current_adc_debug(const struct linkr_debugger_current_desc *current,
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
		return ret;
	}

	ret = adc_read_dt(spec, &sequence);
	if (ret < 0) {
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

	k_mutex_init(&linkr_debugger_control_lock);

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

	ret = setup_current_sensors();
	if (ret < 0) {
		return ret;
	}

	for (size_t i = 0; i < MIN(linkr_debugger_safe_gpio_count, ARRAY_SIZE(linkr_debugger_gpio_directions)); i++) {
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

int linkr_debugger_sd_route_set(enum linkr_debugger_sd_route route)
{
	int ret;

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	ret = gpio_pin_set(gpio0, 6, route == LINKR_DEBUGGER_SD_ROUTE_USB_READER ? 1 : 0);
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
	int ret;

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	ret = gpio_pin_set(gpio0, 3, route == LINKR_DEBUGGER_USB_ROUTE_TARGET ? 1 : 0);
	if (ret < 0) {
		k_mutex_unlock(&linkr_debugger_control_lock);
		return ret;
	}

	linkr_debugger_usb_route = route;

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

	if (sample == NULL) {
		return -EINVAL;
	}

	sample->raw_available = false;
	sample->raw = 0;
	sample->mv = 0;
	sample->current_ua = 0;
	sample->value.val1 = 0;
	sample->value.val2 = 0;

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

	ret = read_current_adc_debug(current, sample);
	if (ret < 0) {
		k_mutex_unlock(&linkr_debugger_control_lock);
		return ret;
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
	sample->current_ua = sensor_value_to_microamps(&value);
	k_mutex_unlock(&linkr_debugger_control_lock);

	return 0;
}

int linkr_debugger_gpio_get(const struct linkr_debugger_safe_gpio_desc *desc, int *value)
{
	size_t index;

	if (desc == NULL || value == NULL) {
		return -EINVAL;
	}

	index = safe_gpio_index(desc);

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	if (index < linkr_debugger_safe_gpio_count &&
	    linkr_debugger_gpio_directions[index] == LINKR_DEBUGGER_GPIO_DIR_OUTPUT) {
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

	if (desc == NULL) {
		return -EINVAL;
	}

	index = safe_gpio_index(desc);

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	ret = gpio_pin_configure(gpio0, desc->pin,
				 value ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
	if (ret >= 0 && index < linkr_debugger_safe_gpio_count) {
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

	if (desc == NULL) {
		return -EINVAL;
	}

	index = safe_gpio_index(desc);

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	ret = gpio_pin_configure(gpio0, desc->pin, GPIO_INPUT);
	if (ret >= 0 && index < linkr_debugger_safe_gpio_count) {
		linkr_debugger_gpio_apply_safe_drive_strength(desc);
		linkr_debugger_gpio_directions[index] = LINKR_DEBUGGER_GPIO_DIR_INPUT;
	}
	k_mutex_unlock(&linkr_debugger_control_lock);
	return ret;
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
	status->timeout_ms = LINKR_DEBUGGER_WATCHDOG_TIMEOUT_MS;
	status->bootloader_on_timeout = status->supported;
	status->failing_service = linkr_debugger_watchdog_failing_service;
	k_mutex_unlock(&linkr_debugger_control_lock);
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
	linkr_debugger_watchdog_supervisor_started = true;
	k_mutex_unlock(&linkr_debugger_control_lock);

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

int linkr_debugger_bootloader_now(void)
{
#if defined(CONFIG_SOC_SERIES_RP2040) || defined(CONFIG_SOC_SERIES_RP2350)
	int ret = 0;

	k_mutex_lock(&linkr_debugger_control_lock, K_FOREVER);
	if (linkr_debugger_watchdog_armed && watchdog_dev != NULL && device_is_ready(watchdog_dev)) {
		(void)wdt_feed(watchdog_dev, linkr_debugger_watchdog_channel);
	}
	linkr_debugger_watchdog_marker_clear();
	if (linkr_debugger_watchdog_armed && watchdog_dev != NULL && device_is_ready(watchdog_dev)) {
		ret = wdt_disable(watchdog_dev);
	}
	ARG_UNUSED(ret);
	linkr_debugger_watchdog_force_disable_locked();
	k_mutex_unlock(&linkr_debugger_control_lock);
	printk("explicit BOOTSEL entry\n");
	reset_usb_boot(0, 0);
	return 0;
#else
	return -ENOTSUP;
#endif
}
