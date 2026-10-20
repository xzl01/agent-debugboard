/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LINKR_DEBUGGER_PD_HOST_TEST 1

/* Exercise the real driver and policy together. Including the sources lets
 * each case reset private state without adding a firmware-only test API. */
#include "../../src/wch_ch224a.c"
#include "../../src/linkr_debugger_pd.c"

#define SENSOR_STEP_CAP 128U
#define I2C_READ_RESULT_CAP 16U

struct sensor_step {
	int fetch_ret;
	int get_ret;
	int64_t millivolts;
};

int64_t linkr_pd_test_uptime_ms;

static bool sensor_ready;
static bool ch224a_initialized;
static bool ch224a_ready;
static int ch224a_init_result;
static unsigned int device_init_calls;
static unsigned int driver_init_calls;
static struct sensor_step sensor_steps[SENSOR_STEP_CAP];
static size_t sensor_step_count;
static size_t sensor_step_index;
static struct sensor_step sensor_default_step;
static struct sensor_step sensor_active_step;
static unsigned int sensor_fetches;
static unsigned int sensor_gets;
static unsigned int assert_no_i2c_through_fetch;

static uint8_t registers[256];
static unsigned int reads[256];
static unsigned int writes[256];
static unsigned int i2c_transactions;
static int failed_register;
static unsigned int read_failures_remaining;
static unsigned int write_failures_remaining;
static int i2c_read_results[I2C_READ_RESULT_CAP];
static size_t i2c_read_result_count;
static size_t i2c_read_result_index;
static uint8_t status_read_values[I2C_READ_RESULT_CAP];
static size_t status_read_value_count;
static size_t status_read_value_index;
static int i2c_recovery_result;
static unsigned int i2c_recoveries;
static unsigned int unknown_status_warnings;
static uint8_t unknown_status_warning_raw;
static uint8_t unknown_status_warning_bits;

static struct wch_ch224a_data test_driver_data;
const struct device linkr_pd_test_sensor_device = {0};
static const struct wch_ch224a_config test_driver_config = {
	.i2c = { .bus = &linkr_pd_test_device, .addr = CH224A_ADDR_LOW },
	.input_voltage_sensor = &linkr_pd_test_sensor_device,
	.maximum_request_mv = 20000U,
	.input_present_threshold_mv = 2800U,
	.input_absent_threshold_mv = 2500U,
	.input_present_stable_samples = 3U,
	.input_sample_interval_ms = 5U,
};

const struct device linkr_pd_test_device = {
	.config = &test_driver_config,
	.data = &test_driver_data,
	.api = &wch_ch224a_api,
};

void linkr_pd_test_log_warning(const char *format, ...)
{
	static const char unknown_status_format[] =
		"CH224A status 0x%02x has unknown/reserved bits 0x%02x; ignoring them";
	va_list args;

	if (strcmp(format, unknown_status_format) != 0) {
		return;
	}

	va_start(args, format);
	unknown_status_warning_raw = (uint8_t)va_arg(args, unsigned int);
	unknown_status_warning_bits = (uint8_t)va_arg(args, unsigned int);
	va_end(args);
	unknown_status_warnings++;
}

bool linkr_pd_test_device_is_ready(const struct device *dev)
{
	if (dev == &linkr_pd_test_sensor_device) {
		return sensor_ready;
	}
	return dev == &linkr_pd_test_device && ch224a_ready;
}

int device_init(const struct device *dev)
{
	int ret;

	assert(dev == &linkr_pd_test_device);
	device_init_calls++;
	if (ch224a_initialized) {
		return -EALREADY;
	}

	ch224a_initialized = true;
	driver_init_calls++;
	if (ch224a_init_result < 0) {
		return ch224a_init_result;
	}

	ret = wch_ch224a_init(dev);
	if (ret == 0) {
		ch224a_ready = true;
	}
	return ret;
}

static struct sensor_step sensor_step_at(size_t index)
{
	return index < sensor_step_count ? sensor_steps[index] : sensor_default_step;
}

int sensor_sample_fetch(const struct device *dev)
{
	assert(dev == &linkr_pd_test_sensor_device);
	assert(test_driver_data.lock.locked);
	sensor_active_step = sensor_step_at(sensor_step_index++);
	sensor_fetches++;
	if (assert_no_i2c_through_fetch != 0U &&
	    sensor_fetches <= assert_no_i2c_through_fetch) {
		assert(i2c_transactions == 0U);
	}
	return sensor_active_step.fetch_ret;
}

int sensor_channel_get(const struct device *dev, enum sensor_channel chan,
		       struct sensor_value *value)
{
	int64_t remainder;

	assert(dev == &linkr_pd_test_sensor_device);
	assert(chan == SENSOR_CHAN_VOLTAGE);
	assert(test_driver_data.lock.locked);
	sensor_gets++;
	if (sensor_active_step.get_ret < 0) {
		return sensor_active_step.get_ret;
	}
	value->val1 = (int32_t)(sensor_active_step.millivolts / 1000LL);
	remainder = sensor_active_step.millivolts % 1000LL;
	value->val2 = (int32_t)(remainder * 1000LL);
	return 0;
}

int i2c_reg_read_byte_dt(const struct i2c_dt_spec *spec, uint8_t reg,
			 uint8_t *value)
{
	assert(spec->addr == CH224A_ADDR_LOW || spec->addr == CH224A_ADDR_HIGH);
	reads[reg]++;
	i2c_transactions++;
	if (i2c_read_result_index < i2c_read_result_count) {
		int ret = i2c_read_results[i2c_read_result_index++];

		if (ret < 0) {
			return ret;
		}
	}
	if (reg == failed_register || read_failures_remaining > 0U) {
		if (read_failures_remaining > 0U) {
			read_failures_remaining--;
		}
		return -EIO;
	}
	if (reg == CH224A_REG_STATUS &&
	    status_read_value_index < status_read_value_count) {
		*value = status_read_values[status_read_value_index++];
		return 0;
	}
	*value = registers[reg];
	return 0;
}

int i2c_recover_bus(const struct device *dev)
{
	assert(dev == test_driver_config.i2c.bus);
	i2c_recoveries++;
	return i2c_recovery_result;
}

int i2c_reg_write_byte_dt(const struct i2c_dt_spec *spec, uint8_t reg,
			  uint8_t value)
{
	assert(spec->addr == CH224A_ADDR_LOW || spec->addr == CH224A_ADDR_HIGH);
	writes[reg]++;
	i2c_transactions++;
	if (write_failures_remaining > 0U) {
		write_failures_remaining--;
		return -EIO;
	}
	registers[reg] = value;
	return 0;
}

static unsigned int write_count(void)
{
	unsigned int total = 0U;

	for (size_t i = 0U; i < ARRAY_SIZE(writes); i++) {
		total += writes[i];
	}
	return total;
}

static void set_sensor_default(int64_t millivolts)
{
	sensor_default_step = (struct sensor_step) {
		.millivolts = millivolts,
	};
	sensor_step_count = 0U;
	sensor_step_index = 0U;
}

static void set_sensor_steps(const struct sensor_step *steps, size_t count)
{
	assert(count <= ARRAY_SIZE(sensor_steps));
	memcpy(sensor_steps, steps, count * sizeof(*steps));
	sensor_step_count = count;
	sensor_step_index = 0U;
}

static void set_sensor_mv_steps(const int64_t *millivolts, size_t count)
{
	assert(count <= ARRAY_SIZE(sensor_steps));
	for (size_t i = 0U; i < count; i++) {
		sensor_steps[i] = (struct sensor_step) {
			.millivolts = millivolts[i],
		};
	}
	sensor_step_count = count;
	sensor_step_index = 0U;
}

static void set_i2c_read_results(const int *results, size_t count)
{
	assert(count <= ARRAY_SIZE(i2c_read_results));
	memcpy(i2c_read_results, results, count * sizeof(*results));
	i2c_read_result_count = count;
	i2c_read_result_index = 0U;
}

static void set_status_read_values(const uint8_t *values, size_t count)
{
	assert(count <= ARRAY_SIZE(status_read_values));
	memcpy(status_read_values, values, count * sizeof(*values));
	status_read_value_count = count;
	status_read_value_index = 0U;
}

static void put_pdo(size_t offset, uint32_t pdo)
{
	for (size_t i = 0; i < 4U; i++) {
		registers[CH224A_REG_SOURCE_CAPABILITIES + offset + i] =
			pdo >> (i * 8U);
	}
}

static void set_source(uint16_t fixed_mv, uint16_t pps_max_mv)
{
	memset(&registers[CH224A_REG_SOURCE_CAPABILITIES], 0,
	       CH224A_SOURCE_CAPABILITIES_LEN);
	/* Three PDOs: fixed 5 V, a second fixed voltage, and PPS 3.3 V..max. */
	registers[CH224A_REG_SOURCE_CAPABILITIES + 1U] = 0x30U;
	put_pdo(2U, ((5000U / 50U) << 10) | 300U);
	put_pdo(6U, ((uint32_t)(fixed_mv / 50U) << 10) | 300U);
	put_pdo(10U, (3U << 30) | ((uint32_t)(pps_max_mv / 100U) << 17) |
		     (33U << 8) | 60U);
}

static void reset_fixture(void)
{
	memset(registers, 0, sizeof(registers));
	memset(reads, 0, sizeof(reads));
	memset(writes, 0, sizeof(writes));
	memset(sensor_steps, 0, sizeof(sensor_steps));
	memset(i2c_read_results, 0, sizeof(i2c_read_results));
	memset(status_read_values, 0, sizeof(status_read_values));
	i2c_transactions = 0U;
	failed_register = -1;
	read_failures_remaining = 0U;
	write_failures_remaining = 0U;
	i2c_read_result_count = 0U;
	i2c_read_result_index = 0U;
	status_read_value_count = 0U;
	status_read_value_index = 0U;
	i2c_recovery_result = 0;
	i2c_recoveries = 0U;
	unknown_status_warnings = 0U;
	unknown_status_warning_raw = 0U;
	unknown_status_warning_bits = 0U;
	sensor_ready = true;
	ch224a_initialized = true;
	ch224a_ready = true;
	ch224a_init_result = 0;
	device_init_calls = 0U;
	driver_init_calls = 0U;
	sensor_fetches = 0U;
	sensor_gets = 0U;
	assert_no_i2c_through_fetch = 0U;
	set_sensor_default(5000);
	linkr_pd_test_uptime_ms = 1000;
	registers[CH224A_REG_STATUS] = CH224A_STATUS_PD;
	registers[CH224A_REG_CURRENT] = 60U;
	set_source(9000U, 11000U);
	assert(wch_ch224a_init(&linkr_pd_test_device) == 0);
	k_mutex_init(&linkr_debugger_pd_lock);
	pd_available = false;
	pd_mode = LINKR_DEBUGGER_PD_MODE_FIXED;
	pd_gear = 0U;
	pd_pps_mv = 0U;
	pd_last_error = 0;
	pd_requested_voltage_valid = false;
	pd_probe_retry_after_ms = 0;
	pd_capability_retry_after_ms = 0;
	pd_capability_valid_until_ms = 0;
	pd_input_present = false;
	pd_service_init_state = PD_SERVICE_UNINITIALIZED;
	pd_service_init_error = 0;
	pd_monitor_started = false;
	memset(&pd_capabilities, 0, sizeof(pd_capabilities));
}

static void defer_driver_init(void)
{
	memset(&test_driver_data, 0, sizeof(test_driver_data));
	ch224a_initialized = false;
	ch224a_ready = false;
	device_init_calls = 0U;
	driver_init_calls = 0U;
}

static void establish_driver_presence(void)
{
	assert(wch_ch224a_probe(&linkr_pd_test_device) == 0);
	assert(test_driver_data.input_present);
	assert(test_driver_data.present);
}

static void start_pd(void)
{
	reset_fixture();
	assert(linkr_debugger_pd_init() == 0);
	assert(device_init_calls == 0U);
	assert(driver_init_calls == 0U);
	assert(linkr_debugger_pd_available());
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 1U);
	assert(write_count() == 0U);
}

static void test_absent_blocks_all_pd_paths_without_i2c(void)
{
	struct linkr_debugger_pd_status status;

	reset_fixture();
	set_sensor_default(0);
	assert(linkr_debugger_pd_init() == -ENOLINK);
	assert(i2c_transactions == 0U);
	assert(!test_driver_data.input_present && !test_driver_data.present);
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(!status.available && status.last_error == -ENOLINK);
	assert(status.voltage_mv == 0U && status.gear == 0U && status.pps_mv == 0U);
	assert(linkr_debugger_pd_set_fixed(5000U) == -ENOLINK);
	assert(linkr_debugger_pd_set_fixed(20000U) == -ENOLINK);
	assert(linkr_debugger_pd_set_pps(3300U) == -ENOLINK);
	assert(i2c_transactions == 0U);
}

static void test_monitor_recovers_insert_without_client_polling(void)
{
	unsigned int before;
	unsigned int status_reads;

	reset_fixture();
	set_sensor_default(0);
	assert(linkr_debugger_pd_init() == -ENOLINK);
	assert(pd_monitor_started);
	before = i2c_transactions;
	linkr_pd_test_uptime_ms += PD_INPUT_MONITOR_INTERVAL_MSEC;
	assert(pd_monitor_step() == -ENOLINK);
	assert(i2c_transactions == before);

	set_sensor_default(5000);
	assert(pd_monitor_step() == 0);
	assert(linkr_debugger_pd_available());
	assert(pd_input_present);
	assert(reads[CH224A_REG_STATUS] == 2U);
	assert(reads[CH224A_REG_CURRENT] == 1U);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 1U);

	before = i2c_transactions;
	status_reads = reads[CH224A_REG_STATUS];
	linkr_pd_test_uptime_ms += PD_INPUT_MONITOR_INTERVAL_MSEC;
	assert(pd_monitor_step() == 0);
	assert(i2c_transactions == before);
	assert(reads[CH224A_REG_STATUS] == status_reads);
}

static void test_monitor_remove_and_fast_reinsert(void)
{
	unsigned int before;
	unsigned int writes_before;
	int64_t retry_after;

	start_pd();
	assert(linkr_debugger_pd_set_fixed(9000U) == 0);
	assert(pd_requested_voltage_valid && pd_gear == 1U);
	set_sensor_default(2400);
	before = i2c_transactions;
	assert(pd_monitor_step() == -ENOLINK);
	assert(i2c_transactions == before);
	assert(!linkr_debugger_pd_available());
	assert(!pd_input_present && !pd_requested_voltage_valid);
	assert(pd_gear == 0U && pd_pps_mv == 0U && !pd_capabilities.valid);
	retry_after = pd_probe_retry_after_ms;
	assert(retry_after > linkr_pd_test_uptime_ms);

	writes_before = write_count();
	linkr_pd_test_uptime_ms += PD_INPUT_MONITOR_INTERVAL_MSEC;
	set_sensor_default(5000);
	assert(pd_monitor_step() == 0);
	assert(linkr_debugger_pd_available());
	assert(pd_probe_retry_after_ms == 0);
	assert(!pd_requested_voltage_valid && pd_gear == 0U && pd_pps_mv == 0U);
	assert(write_count() == writes_before);
}

static void test_monitor_adc_error_clears_without_i2c(void)
{
	const struct sensor_step error = { .fetch_ret = -EAGAIN };
	unsigned int before;

	start_pd();
	set_sensor_steps(&error, 1U);
	before = i2c_transactions;
	assert(pd_monitor_step() == -EAGAIN);
	assert(i2c_transactions == before);
	assert(!linkr_debugger_pd_available());
	assert(!pd_input_present && pd_last_error == -EAGAIN);
	assert(!pd_requested_voltage_valid && !pd_capabilities.valid);
}

static void test_monitor_honors_driver_edge_after_client_detects_removal(void)
{
	struct linkr_debugger_pd_status status;
	unsigned int before;

	start_pd();
	set_sensor_default(2400);
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(!status.available && !linkr_debugger_pd_available());
	assert(pd_input_present);
	assert(!test_driver_data.input_present);
	before = i2c_transactions;

	linkr_pd_test_uptime_ms += PD_INPUT_MONITOR_INTERVAL_MSEC;
	set_sensor_default(5000);
	assert(pd_monitor_step() == 0);
	assert(linkr_debugger_pd_available());
	assert(i2c_transactions > before);
	assert(pd_probe_retry_after_ms == 0);
}

static void test_driver_init_is_sensor_independent_and_bus_silent(void)
{
	reset_fixture();
	sensor_ready = false;
	defer_driver_init();
	assert(device_init(&linkr_pd_test_device) == 0);
	assert(device_init(&linkr_pd_test_device) == -EALREADY);
	assert(device_init_calls == 2U);
	assert(driver_init_calls == 1U);
	assert(ch224a_ready);
	assert(sensor_fetches == 0U);
	assert(i2c_transactions == 0U);
}

static void test_pd_initializes_deferred_driver_once(void)
{
	unsigned int before;

	reset_fixture();
	defer_driver_init();
	assert(linkr_debugger_pd_init() == 0);
	assert(ch224a_ready);
	assert(pd_monitor_started);
	assert(device_init_calls == 1U);
	assert(driver_init_calls == 1U);
	assert(i2c_transactions > 0U);

	before = i2c_transactions;
	assert(linkr_debugger_pd_init() == 0);
	assert(device_init_calls == 1U);
	assert(driver_init_calls == 1U);
	assert(i2c_transactions == before);
}

static void test_pd_propagates_deferred_init_failure(void)
{
	struct linkr_debugger_pd_status status;
	unsigned int init_calls;
	unsigned int fetches;
	unsigned int transactions;

	reset_fixture();
	defer_driver_init();
	ch224a_init_result = -EIO;
	assert(linkr_debugger_pd_init() == -EIO);
	assert(device_init_calls == 1U);
	assert(driver_init_calls == 1U);
	assert(!ch224a_ready);
	assert(!pd_monitor_started);
	assert(!linkr_debugger_pd_available());
	assert(pd_last_error == -EIO);
	assert(sensor_fetches == 0U);
	assert(i2c_transactions == 0U);
	assert(pd_service_init_state == PD_SERVICE_FAILED);
	assert(pd_service_init_error == -EIO);

	/* Poison the never-initialized CH224A mutex. Every service API must stop
	 * at the immutable init-failure gate without touching the driver.
	 */
	test_driver_data.lock.locked = true;
	init_calls = device_init_calls;
	fetches = sensor_fetches;
	transactions = i2c_transactions;
	linkr_pd_test_uptime_ms += PD_PROBE_RETRY_MSEC + 1;
	assert(linkr_debugger_pd_init() == -EIO);
	assert(pd_monitor_step() == -EIO);
	assert(linkr_debugger_pd_set_fixed(5000U) == -EIO);
	assert(linkr_debugger_pd_set_fixed(20000U) == -EIO);
	assert(linkr_debugger_pd_set_pps(3300U) == -EIO);
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(!status.available && status.last_error == -EIO);
	assert(status.voltage_mv == 0U && status.gear == 0U && status.pps_mv == 0U);
	assert(!status.capabilities_valid && status.fixed_count == 0U);
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(!status.available && status.last_error == -EIO);
	assert(status.voltage_mv == 0U && !status.capabilities_valid);
	assert(device_init_calls == init_calls);
	assert(sensor_fetches == fetches);
	assert(i2c_transactions == transactions);
	assert(!pd_monitor_started);
	test_driver_data.lock.locked = false;

	/* Pinned Zephyr marks an attempted init initialized even on failure, so
	 * another device_init() reports -EALREADY without rerunning the callback.
	 */
	assert(device_init(&linkr_pd_test_device) == -EALREADY);
	assert(device_init_calls == 2U);
	assert(driver_init_calls == 1U);
}

static void test_pd_rejects_previously_failed_deferred_device(void)
{
	reset_fixture();
	ch224a_ready = false;
	ch224a_initialized = true;
	memset(&test_driver_data, 0, sizeof(test_driver_data));
	assert(linkr_debugger_pd_init() == -EALREADY);
	assert(pd_service_init_state == PD_SERVICE_FAILED);
	assert(pd_service_init_error == -EALREADY);
	assert(device_init_calls == 1U);
	assert(driver_init_calls == 0U);
	assert(!pd_monitor_started && !linkr_debugger_pd_available());

	linkr_pd_test_uptime_ms += PD_PROBE_RETRY_MSEC + 1;
	assert(linkr_debugger_pd_init() == -EALREADY);
	assert(device_init_calls == 1U);
	assert(driver_init_calls == 0U);
	assert(i2c_transactions == 0U && sensor_fetches == 0U);
}

static void test_three_high_samples_precede_first_i2c(void)
{
	const int64_t high[] = { 3000, 3000, 3000 };
	const int64_t unstable[] = { 3000, 3000, 2700 };
	int64_t started;

	reset_fixture();
	set_sensor_mv_steps(unstable, ARRAY_SIZE(unstable));
	assert_no_i2c_through_fetch = 3U;
	assert(wch_ch224a_probe(&linkr_pd_test_device) == -ENOLINK);
	assert(i2c_transactions == 0U);

	reset_fixture();
	set_sensor_mv_steps(high, ARRAY_SIZE(high));
	assert_no_i2c_through_fetch = 3U;
	started = linkr_pd_test_uptime_ms;
	establish_driver_presence();
	assert(sensor_fetches == 3U);
	assert(i2c_transactions == 1U);
	assert(linkr_pd_test_uptime_ms - started >= 10);
}

static void test_presence_hysteresis(void)
{
	struct wch_ch224a_status status;
	unsigned int before;

	reset_fixture();
	establish_driver_presence();
	set_sensor_default(2600);
	assert(wch_ch224a_get_status(&linkr_pd_test_device, &status) == 0);
	assert(status.pd_active);
	assert(test_driver_data.input_present);

	before = i2c_transactions;
	set_sensor_default(2400);
	assert(wch_ch224a_set_fixed(&linkr_pd_test_device, 0U) == -ENOLINK);
	assert(i2c_transactions == before);
	assert(!test_driver_data.input_present && !test_driver_data.present);

	set_sensor_default(2600);
	assert(wch_ch224a_probe(&linkr_pd_test_device) == -ENOLINK);
	assert(i2c_transactions == before);
}

static void assert_adc_gate_error(struct sensor_step step, bool ready,
				  int expected)
{
	reset_fixture();
	sensor_ready = ready;
	set_sensor_steps(&step, 1U);
	assert(wch_ch224a_probe(&linkr_pd_test_device) == expected);
	assert(i2c_transactions == 0U);
	assert(!test_driver_data.input_present && !test_driver_data.present);
}

static void test_adc_errors_propagate_without_i2c(void)
{
	assert_adc_gate_error((struct sensor_step){ .millivolts = 5000 },
			      false, -ENODEV);
	assert_adc_gate_error((struct sensor_step){ .fetch_ret = -EAGAIN },
			      true, -EAGAIN);
	assert_adc_gate_error((struct sensor_step){ .get_ret = -EIO },
			      true, -EIO);
	assert_adc_gate_error((struct sensor_step){ .millivolts = -1 },
			      true, -ERANGE);
	assert_adc_gate_error(
		(struct sensor_step){ .millivolts = (int64_t)INT32_MAX + 1LL },
		true, -ERANGE);
}

static void test_low_between_i2c_retries_stops_remaining_attempts(void)
{
	struct wch_ch224a_status status;
	const int64_t sequence[] = { 3000, 2400 };
	unsigned int before;
	int64_t started;

	reset_fixture();
	establish_driver_presence();
	set_sensor_mv_steps(sequence, ARRAY_SIZE(sequence));
	read_failures_remaining = 1U;
	before = i2c_transactions;
	started = linkr_pd_test_uptime_ms;
	assert(wch_ch224a_get_status(&linkr_pd_test_device, &status) == -ENOLINK);
	assert(i2c_transactions == before + 1U);
	assert(linkr_pd_test_uptime_ms - started == CH224A_RETRY_DELAY_MSEC);
	assert(!test_driver_data.input_present && !test_driver_data.present);
	assert(!status.pd_active && status.max_current_ma == 0U);
}

static void test_probe_gates_every_address_and_attempt(void)
{
	reset_fixture();
	failed_register = CH224A_REG_STATUS;
	assert(wch_ch224a_probe(&linkr_pd_test_device) == -EIO);
	assert(reads[CH224A_REG_STATUS] == 2U * CH224A_IO_ATTEMPTS);
	assert(sensor_fetches == 2U * CH224A_IO_ATTEMPTS + 2U);
	assert(i2c_recoveries == 0U);
	assert(linkr_pd_test_uptime_ms ==
	       1000 + 10 + (CH224A_IO_ATTEMPTS - 1) * CH224A_RETRY_DELAY_MSEC);
}

static void test_probe_recovery_is_bounded_and_classified(void)
{
	const int busy_then_success[] = { -EBUSY, 0 };
	const int two_recoverable_errors[] = { -EBUSY, -ETIMEDOUT, 0 };

	assert(wch_ch224a_probe_error_needs_recovery(-EBUSY));
	assert(wch_ch224a_probe_error_needs_recovery(-ETIMEDOUT));
	assert(!wch_ch224a_probe_error_needs_recovery(-EIO));
	assert(!wch_ch224a_probe_error_needs_recovery(-ENXIO));

	reset_fixture();
	set_i2c_read_results(busy_then_success, ARRAY_SIZE(busy_then_success));
	assert(wch_ch224a_probe(&linkr_pd_test_device) == 0);
	assert(i2c_recoveries == 1U);
	assert(reads[CH224A_REG_STATUS] == 2U);

	reset_fixture();
	set_i2c_read_results(two_recoverable_errors,
			     ARRAY_SIZE(two_recoverable_errors));
	assert(wch_ch224a_probe(&linkr_pd_test_device) == 0);
	assert(i2c_recoveries == 1U);
	assert(reads[CH224A_REG_STATUS] == 3U);
}

static void test_probe_recovery_obeys_gate_and_failure_state(void)
{
	const int busy[] = { -EBUSY };
	unsigned int before;

	reset_fixture();
	set_sensor_default(0);
	set_i2c_read_results(busy, ARRAY_SIZE(busy));
	assert(wch_ch224a_probe(&linkr_pd_test_device) == -ENOLINK);
	assert(i2c_transactions == 0U && i2c_recoveries == 0U);
	assert(!test_driver_data.present);

	reset_fixture();
	set_i2c_read_results(busy, ARRAY_SIZE(busy));
	i2c_recovery_result = -EBUSY;
	before = i2c_transactions;
	assert(wch_ch224a_probe(&linkr_pd_test_device) == -EBUSY);
	assert(i2c_transactions == before + 1U);
	assert(i2c_recoveries == 1U);
	assert(!test_driver_data.present);

	reset_fixture();
	set_i2c_read_results(busy, ARRAY_SIZE(busy));
	i2c_recovery_result = -EBUSY;
	assert(linkr_debugger_pd_init() == -EBUSY);
	assert(!linkr_debugger_pd_available());
	assert(pd_last_error == -EBUSY);
	assert(pd_probe_retry_after_ms > linkr_pd_test_uptime_ms);
	assert(i2c_recoveries == 1U);
	before = i2c_transactions;
	linkr_pd_test_uptime_ms += PD_INPUT_MONITOR_INTERVAL_MSEC;
	assert(pd_monitor_step() == -EBUSY);
	assert(i2c_transactions == before && i2c_recoveries == 1U);
}

static void test_status_ack_success_and_reserved_ignore_policy(void)
{
	struct wch_ch224a_status status;
	unsigned int before;

	assert(CH224A_STATUS_KNOWN_MASK == 0x7fU);

	reset_fixture();
	set_sensor_default(3300);
	failed_register = CH224A_REG_STATUS;
	assert(wch_ch224a_probe(&linkr_pd_test_device) == -EIO);
	assert(!test_driver_data.present);
	assert(i2c_recoveries == 0U);

	reset_fixture();
	set_sensor_default(3300);
	registers[CH224A_REG_STATUS] = BIT(7);
	assert(wch_ch224a_probe(&linkr_pd_test_device) == 0);
	assert(test_driver_data.present);
	assert(i2c_recoveries == 0U);
	assert(unknown_status_warnings == 1U);
	assert(unknown_status_warning_raw == 0x80U);
	assert(unknown_status_warning_bits == BIT(7));
	assert(test_driver_data.status_unknown_bits_warned);
	assert(wch_ch224a_get_status(&linkr_pd_test_device, &status) == 0);
	assert(!status.bc && !status.qc2 && !status.qc3 && !status.pd_active);
	assert(!status.epr && !status.epr_exist && !status.avs);
	assert(status.max_current_ma == 0U);
	assert(unknown_status_warnings == 1U);

	reset_fixture();
	set_sensor_default(3300);
	registers[CH224A_REG_STATUS] = 0xffU;
	assert(wch_ch224a_probe(&linkr_pd_test_device) == 0);
	assert(wch_ch224a_get_status(&linkr_pd_test_device, &status) == 0);
	assert(status.bc && status.qc2 && status.qc3 && status.pd_active);
	assert(status.epr && status.epr_exist && status.avs);
	assert(status.max_current_ma == 3000U);
	assert(test_driver_data.present && i2c_recoveries == 0U);
	assert(unknown_status_warnings == 1U);
	assert(unknown_status_warning_raw == 0xffU);
	assert(unknown_status_warning_bits == BIT(7));

	reset_fixture();
	set_sensor_default(400);
	before = i2c_transactions;
	assert(wch_ch224a_probe(&linkr_pd_test_device) == -ENOLINK);
	assert(i2c_transactions == before && !test_driver_data.present);
}

static void test_runtime_status_accepts_reserved_bit_after_transport_retry(void)
{
	struct wch_ch224a_status status;
	const int retry_then_read[] = { -EIO, 0 };
	unsigned int before;

	reset_fixture();
	establish_driver_presence();
	registers[CH224A_REG_STATUS] = BIT(7) | CH224A_STATUS_PD;
	set_i2c_read_results(retry_then_read, ARRAY_SIZE(retry_then_read));
	before = reads[CH224A_REG_STATUS];
	assert(wch_ch224a_get_status(&linkr_pd_test_device, &status) == 0);
	assert(reads[CH224A_REG_STATUS] == before + 2U);
	assert(test_driver_data.present);
	assert(status.pd_active && status.max_current_ma == 3000U);
	assert(i2c_recoveries == 0U);
	assert(unknown_status_warnings == 1U);
	assert(unknown_status_warning_raw == 0x88U);
	assert(wch_ch224a_get_status(&linkr_pd_test_device, &status) == 0);
	assert(status.pd_active && test_driver_data.present);
	assert(unknown_status_warnings == 1U);

	/* Zero has no protocol flags but is still a valid CH224A status. */
	reset_fixture();
	registers[CH224A_REG_STATUS] = 0U;
	assert(wch_ch224a_probe(&linkr_pd_test_device) == 0);
	assert(wch_ch224a_get_status(&linkr_pd_test_device, &status) == 0);
	assert(!status.pd_active && status.max_current_ma == 0U);
}

static void test_initial_probe_and_status_accept_reserved_bit(void)
{
	const uint8_t sequence[] = {
		BIT(7) | CH224A_STATUS_PD,
		BIT(7) | CH224A_STATUS_PD,
	};

	reset_fixture();
	set_status_read_values(sequence, ARRAY_SIZE(sequence));
	assert(linkr_debugger_pd_init() == 0);
	assert(linkr_debugger_pd_available());
	assert(pd_last_error == 0);
	assert(test_driver_data.present);
	assert(!pd_requested_voltage_valid && pd_capabilities.valid);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 1U);
	assert(i2c_recoveries == 0U);
	assert(unknown_status_warnings == 1U);
	assert(unknown_status_warning_raw == 0x88U);
}

static void test_runtime_reserved_status_preserves_cached_pd_state(void)
{
	struct linkr_debugger_pd_status status;
	int64_t capability_valid_until;

	reset_fixture();
	set_source(20000U, 11000U);
	assert(linkr_debugger_pd_init() == 0);
	assert(linkr_debugger_pd_set_fixed(20000U) == 0);
	assert(pd_requested_voltage_valid && pd_gear == 4U);
	assert(pd_capabilities.valid);
	capability_valid_until = pd_capability_valid_until_ms;

	registers[CH224A_REG_STATUS] = BIT(7) | CH224A_STATUS_PD;
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(status.available && status.last_error == 0);
	assert(status.pd_active && status.voltage_mv == 20000U);
	assert(status.gear == 4U && status.pps_mv == 0U);
	assert(status.capabilities_valid && status.fixed_count > 0U);
	assert(linkr_debugger_pd_available());
	assert(pd_requested_voltage_valid && pd_gear == 4U && pd_pps_mv == 0U);
	assert(pd_capabilities.valid && test_driver_data.present);
	assert(pd_probe_retry_after_ms == 0);
	assert(pd_capability_valid_until_ms == capability_valid_until);
	assert(i2c_recoveries == 0U);
	assert(unknown_status_warnings == 1U);
	assert(unknown_status_warning_raw == 0x88U);
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(status.available && status.voltage_mv == 20000U);
	assert(status.capabilities_valid && pd_probe_retry_after_ms == 0);
	assert(linkr_debugger_pd_set_fixed(20000U) == 0);
	assert(linkr_debugger_pd_set_pps(3300U) == 0);
	assert(linkr_debugger_pd_available());
	assert(unknown_status_warnings == 1U);
}

static void test_capability_read_stops_at_low_sample(void)
{
	struct wch_ch224a_capabilities capabilities;
	int64_t sequence[8];
	const size_t fail_byte = 7U;
	unsigned int before;

	reset_fixture();
	establish_driver_presence();
	for (size_t i = 0U; i < fail_byte - 1U; i++) {
		sequence[i] = 3000;
	}
	sequence[fail_byte - 1U] = 2400;
	set_sensor_mv_steps(sequence, fail_byte);
	before = i2c_transactions;
	assert(wch_ch224a_get_capabilities(&linkr_pd_test_device,
					   &capabilities) == -ENOLINK);
	assert(i2c_transactions == before + fail_byte - 1U);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES + fail_byte - 1U] == 0U);
	assert(!capabilities.valid && capabilities.fixed_count == 0U);
	assert(capabilities.pps_count == 0U && capabilities.avs_count == 0U);
	assert(!test_driver_data.input_present && !test_driver_data.present);
}

static void test_status_and_capability_endpoint_checks(void)
{
	struct wch_ch224a_status status;
	struct wch_ch224a_capabilities capabilities;
	int64_t status_sequence[] = { 3000, 3000, 2400 };
	int64_t capability_sequence[CH224A_SOURCE_CAPABILITIES_LEN + 1U];
	unsigned int before;

	reset_fixture();
	establish_driver_presence();
	set_sensor_mv_steps(status_sequence, ARRAY_SIZE(status_sequence));
	before = i2c_transactions;
	assert(wch_ch224a_get_status(&linkr_pd_test_device, &status) == -ENOLINK);
	assert(i2c_transactions == before + 2U);
	assert(!status.pd_active && status.max_current_ma == 0U);

	reset_fixture();
	establish_driver_presence();
	for (size_t i = 0U; i < CH224A_SOURCE_CAPABILITIES_LEN; i++) {
		capability_sequence[i] = 3000;
	}
	capability_sequence[CH224A_SOURCE_CAPABILITIES_LEN] = 2400;
	set_sensor_mv_steps(capability_sequence,
			    ARRAY_SIZE(capability_sequence));
	before = i2c_transactions;
	assert(wch_ch224a_get_capabilities(&linkr_pd_test_device,
					   &capabilities) == -ENOLINK);
	assert(i2c_transactions == before + CH224A_SOURCE_CAPABILITIES_LEN);
	assert(!capabilities.valid && capabilities.fixed_count == 0U);
	assert(capabilities.pps_count == 0U && capabilities.avs_count == 0U);
}

static void test_pps_checks_between_both_writes(void)
{
	const int64_t drops_between_writes[] = { 3000, 2400 };
	unsigned int before;

	reset_fixture();
	establish_driver_presence();
	set_sensor_mv_steps(drops_between_writes,
			    ARRAY_SIZE(drops_between_writes));
	before = i2c_transactions;
	assert(wch_ch224a_set_pps(&linkr_pd_test_device, 3300U) == -ENOLINK);
	assert(i2c_transactions == before + 1U);
	assert(writes[CH224A_REG_PPS] == 1U);
	assert(writes[CH224A_REG_VOLTAGE] == 0U);
	assert(!test_driver_data.input_present && !test_driver_data.present);

	reset_fixture();
	establish_driver_presence();
	assert(wch_ch224a_set_pps(&linkr_pd_test_device, 3300U) == 0);
	assert(writes[CH224A_REG_PPS] == 1U);
	assert(writes[CH224A_REG_VOLTAGE] == 1U);
}

static void test_full_refresh_and_cache_only_short_status(void)
{
	struct linkr_debugger_pd_status status;

	start_pd();
	set_source(12000U, 9000U);
	linkr_pd_test_uptime_ms += 4999;
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(status.fixed[1].voltage_mv == 9000U);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 1U);
	linkr_pd_test_uptime_ms++;
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(!status.capabilities_valid);
	assert(status.fixed_count == 0U && status.pps_count == 0U);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 1U);
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(status.capabilities_valid);
	assert(status.fixed[1].voltage_mv == 12000U);
	assert(status.pps[0].max_voltage_mv == 9000U);
	assert(status.max_current_ma == 3000U);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 2U);
}

static void test_explicit_requests_refresh_expired_capabilities(void)
{
	start_pd();
	set_source(12000U, 9000U);
	linkr_pd_test_uptime_ms += 5000;
	assert(linkr_debugger_pd_set_fixed(9000U) == -ENOTSUP);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 2U);
	assert(write_count() == 0U);
	assert(linkr_debugger_pd_set_fixed(12000U) == 0);
	assert(write_count() == 1U);
	linkr_pd_test_uptime_ms += 5000;
	assert(linkr_debugger_pd_set_fixed(5000U) == 0);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 2U);
	assert(write_count() == 2U);

	start_pd();
	set_source(12000U, 9000U);
	linkr_pd_test_uptime_ms += 5000;
	assert(linkr_debugger_pd_set_pps(10000U) == -ENOTSUP);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 2U);
	assert(write_count() == 0U);
	assert(linkr_debugger_pd_set_pps(9000U) == 0);
	assert(write_count() == 2U);
}

static void test_capability_gate_error_marks_unavailable_and_backs_off(void)
{
	struct linkr_debugger_pd_status status;
	int64_t sequence[12];
	const size_t cap_reads_before_drop = 5U;
	unsigned int before;

	start_pd();
	assert(linkr_debugger_pd_set_fixed(9000U) == 0);
	linkr_pd_test_uptime_ms += 5000;
	/* Status, current and endpoint samples, then N capability samples. */
	for (size_t i = 0U; i < 3U + cap_reads_before_drop; i++) {
		sequence[i] = 3000;
	}
	sequence[3U + cap_reads_before_drop] = 2400;
	set_sensor_mv_steps(sequence, 4U + cap_reads_before_drop);
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(!status.available && status.last_error == -ENOLINK);
	assert(status.voltage_mv == 0U && status.gear == 0U && status.pps_mv == 0U);
	assert(!status.capabilities_valid && status.fixed_count == 0U);
	before = i2c_transactions;
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(linkr_debugger_pd_set_fixed(5000U) == -ENOLINK);
	assert(i2c_transactions == before);
}

static void test_capability_i2c_error_marks_unavailable(void)
{
	struct linkr_debugger_pd_status status;

	start_pd();
	linkr_pd_test_uptime_ms += 5000;
	failed_register = CH224A_REG_SOURCE_CAPABILITIES;
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(!status.available && status.last_error == -EIO);
	assert(!status.capabilities_valid && !linkr_debugger_pd_available());
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 1U + CH224A_IO_ATTEMPTS);
}

static void test_capability_adc_error_marks_unavailable(void)
{
	struct linkr_debugger_pd_status status;
	const struct sensor_step sequence[] = {
		{ .millivolts = 3000 },
		{ .millivolts = 3000 },
		{ .millivolts = 3000 },
		{ .fetch_ret = -EAGAIN },
	};
	unsigned int before;

	start_pd();
	linkr_pd_test_uptime_ms += 5000;
	set_sensor_steps(sequence, ARRAY_SIZE(sequence));
	before = i2c_transactions;
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(!status.available && status.last_error == -EAGAIN);
	assert(!status.capabilities_valid && !linkr_debugger_pd_available());
	/* Only status/current were read; endpoint is an ADC-only check. */
	assert(i2c_transactions == before + 2U);
	before = i2c_transactions;
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(i2c_transactions == before);
}

static void test_invalid_pdos_only_back_off_capabilities(void)
{
	struct linkr_debugger_pd_status status;
	unsigned int reads_after_failure;

	start_pd();
	memset(&registers[CH224A_REG_SOURCE_CAPABILITIES], 0,
	       CH224A_SOURCE_CAPABILITIES_LEN);
	linkr_pd_test_uptime_ms += 5000;
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(status.available && !status.capabilities_valid);
	assert(linkr_debugger_pd_available());
	reads_after_failure = reads[CH224A_REG_SOURCE_CAPABILITIES];
	assert(linkr_debugger_pd_set_fixed(5000U) == 0);
	assert(write_count() == 1U);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == reads_after_failure);
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(status.available && !status.capabilities_valid);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == reads_after_failure);
}

static void test_inactive_pd_clears_capabilities_and_zero_status_is_valid(void)
{
	struct linkr_debugger_pd_status status;
	struct wch_ch224a_status driver_status;
	unsigned int current_reads;

	start_pd();
	current_reads = reads[CH224A_REG_CURRENT];
	registers[CH224A_REG_STATUS] = 0U;
	assert(wch_ch224a_probe(&linkr_pd_test_device) == 0);
	assert(wch_ch224a_get_status(&linkr_pd_test_device, &driver_status) == 0);
	assert(!driver_status.pd_active && driver_status.max_current_ma == 0U);
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(status.available && !status.pd_active && !status.capabilities_valid);
	assert(status.fixed_count == 0U && status.pps_count == 0U);
	assert(reads[CH224A_REG_CURRENT] == current_reads);
	registers[CH224A_REG_STATUS] = CH224A_STATUS_PD;
	set_source(12000U, 9000U);
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(!status.capabilities_valid);
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(status.capabilities_valid && status.fixed[1].voltage_mv == 12000U);
}

static void test_current_read_failure_propagates_and_probe_backs_off(void)
{
	struct linkr_debugger_pd_status status;
	struct wch_ch224a_status driver_status;
	unsigned int status_reads;
	unsigned int current_reads;
	int64_t failed_at;

	start_pd();
	failed_register = CH224A_REG_CURRENT;
	assert(wch_ch224a_get_status(&linkr_pd_test_device, &driver_status) == -EIO);

	start_pd();
	assert(linkr_debugger_pd_set_fixed(9000U) == 0);
	failed_register = CH224A_REG_CURRENT;
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(!status.available && status.last_error == -EIO);
	assert(status.voltage_mv == 0U && status.gear == 0U && status.pps_mv == 0U);
	assert(!status.capabilities_valid && !linkr_debugger_pd_available());
	status_reads = reads[CH224A_REG_STATUS];
	current_reads = reads[CH224A_REG_CURRENT];
	assert(current_reads == 1U + CH224A_IO_ATTEMPTS);
	failed_at = linkr_pd_test_uptime_ms;
	failed_register = -1;
	linkr_pd_test_uptime_ms = failed_at + 4999;
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(!status.available && status.last_error == -EIO);
	assert(linkr_debugger_pd_set_fixed(9000U) == -EIO);
	assert(linkr_debugger_pd_set_pps(10000U) == -EIO);
	assert(reads[CH224A_REG_STATUS] == status_reads);
	assert(reads[CH224A_REG_CURRENT] == current_reads);
	linkr_pd_test_uptime_ms++;
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(status.available && status.last_error == 0);
	assert(status.voltage_mv == 0U && !status.capabilities_valid);
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(status.capabilities_valid);
}

static void test_initial_current_read_failure_backs_off(void)
{
	struct linkr_debugger_pd_status status;
	unsigned int status_reads;

	reset_fixture();
	failed_register = CH224A_REG_CURRENT;
	assert(linkr_debugger_pd_init() == 0);
	assert(!linkr_debugger_pd_available());
	status_reads = reads[CH224A_REG_STATUS];
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(!status.available && status.last_error == -EIO);
	assert(status.voltage_mv == 0U && status.gear == 0U);
	assert(reads[CH224A_REG_STATUS] == status_reads);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 0U);
}

static void test_new_source_reprobe_does_not_restore_old_request(void)
{
	struct linkr_debugger_pd_status status;
	unsigned int before;

	start_pd();
	assert(linkr_debugger_pd_set_pps(9000U) == 0);
	assert(pd_requested_voltage_valid && pd_pps_mv == 9000U);
	set_sensor_default(2400);
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(!status.available && status.last_error == -ENOLINK);
	assert(status.voltage_mv == 0U && status.gear == 0U && status.pps_mv == 0U);
	assert(!pd_requested_voltage_valid && pd_pps_mv == 0U);
	before = i2c_transactions;
	set_sensor_default(5000);
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(!status.available);
	assert(i2c_transactions == before);
	linkr_pd_test_uptime_ms += PD_PROBE_RETRY_MSEC;
	set_source(12000U, 9000U);
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(status.available && status.last_error == 0);
	assert(status.voltage_mv == 0U && status.gear == 0U && status.pps_mv == 0U);
	assert(!status.capabilities_valid);
	assert(write_count() == 2U);
}

int main(void)
{
	test_absent_blocks_all_pd_paths_without_i2c();
	test_monitor_recovers_insert_without_client_polling();
	test_monitor_remove_and_fast_reinsert();
	test_monitor_adc_error_clears_without_i2c();
	test_monitor_honors_driver_edge_after_client_detects_removal();
	test_driver_init_is_sensor_independent_and_bus_silent();
	test_pd_initializes_deferred_driver_once();
	test_pd_propagates_deferred_init_failure();
	test_pd_rejects_previously_failed_deferred_device();
	test_three_high_samples_precede_first_i2c();
	test_presence_hysteresis();
	test_adc_errors_propagate_without_i2c();
	test_low_between_i2c_retries_stops_remaining_attempts();
	test_probe_gates_every_address_and_attempt();
	test_probe_recovery_is_bounded_and_classified();
	test_probe_recovery_obeys_gate_and_failure_state();
	test_status_ack_success_and_reserved_ignore_policy();
	test_runtime_status_accepts_reserved_bit_after_transport_retry();
	test_initial_probe_and_status_accept_reserved_bit();
	test_runtime_reserved_status_preserves_cached_pd_state();
	test_capability_read_stops_at_low_sample();
	test_status_and_capability_endpoint_checks();
	test_pps_checks_between_both_writes();
	test_full_refresh_and_cache_only_short_status();
	test_explicit_requests_refresh_expired_capabilities();
	test_capability_gate_error_marks_unavailable_and_backs_off();
	test_capability_i2c_error_marks_unavailable();
	test_capability_adc_error_marks_unavailable();
	test_invalid_pdos_only_back_off_capabilities();
	test_inactive_pd_clears_capabilities_and_zero_status_is_valid();
	test_current_read_failure_propagates_and_probe_backs_off();
	test_initial_current_read_failure_backs_off();
	test_new_source_reprobe_does_not_restore_old_request();
	puts("PD/CH224A ADC gate host tests passed");
	return 0;
}
