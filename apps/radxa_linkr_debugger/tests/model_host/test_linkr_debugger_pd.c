/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include <assert.h>
#include <stdio.h>

/* Exercise the real driver and policy together. Including the sources lets
 * each case reset private state without adding a firmware-only test API. */
#include "../../src/wch_ch224a.c"
#include "../../src/linkr_debugger_pd.c"

int64_t linkr_pd_test_uptime_ms;

static struct wch_ch224a_data test_driver_data;
static const struct wch_ch224a_config test_driver_config = {
	.i2c = { .bus = &linkr_pd_test_device, .addr = CH224A_ADDR_LOW },
	.maximum_request_mv = 20000U,
};

const struct device linkr_pd_test_device = {
	.config = &test_driver_config,
	.data = &test_driver_data,
	.api = &wch_ch224a_api,
};

static uint8_t registers[256];
static unsigned int reads[256];
static unsigned int writes;
static int failed_register;

int i2c_reg_read_byte_dt(const struct i2c_dt_spec *spec, uint8_t reg, uint8_t *value)
{
	assert(spec->addr == CH224A_ADDR_LOW);
	reads[reg]++;
	if (reg == failed_register) return -EIO;
	*value = registers[reg];
	return 0;
}

int i2c_reg_write_byte_dt(const struct i2c_dt_spec *spec, uint8_t reg, uint8_t value)
{
	assert(spec->addr == CH224A_ADDR_LOW);
	writes++;
	registers[reg] = value;
	return 0;
}

static void put_pdo(size_t offset, uint32_t pdo)
{
	for (size_t i = 0; i < 4U; i++) {
		registers[CH224A_REG_SOURCE_CAPABILITIES + offset + i] = pdo >> (i * 8U);
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
	writes = 0;
	failed_register = -1;
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
	memset(&pd_capabilities, 0, sizeof(pd_capabilities));
}

static void start_pd(void)
{
	reset_fixture();
	assert(linkr_debugger_pd_init() == 0);
	assert(linkr_debugger_pd_available());
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 1U);
	assert(writes == 0U);
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
	assert(status.capabilities_valid);
	assert(status.fixed[1].voltage_mv == 9000U);
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
	assert(writes == 0U);
	assert(linkr_debugger_pd_set_fixed(12000U) == 0);
	assert(writes == 1U);
	linkr_pd_test_uptime_ms += 5000;
	assert(linkr_debugger_pd_set_fixed(5000U) == 0);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 2U);
	assert(writes == 2U);

	start_pd();
	set_source(12000U, 9000U);
	linkr_pd_test_uptime_ms += 5000;
	assert(linkr_debugger_pd_set_pps(10000U) == -ENOTSUP);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 2U);
	assert(writes == 0U);
	assert(linkr_debugger_pd_set_pps(9000U) == 0);
	assert(writes == 2U);
}

static void test_expired_capability_failure_blocks_requests_and_retries(void)
{
	struct linkr_debugger_pd_status status;
	unsigned int capability_reads;
	int64_t failed_at;

	start_pd();
	linkr_pd_test_uptime_ms += 5000;
	failed_register = CH224A_REG_SOURCE_CAPABILITIES;
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(status.available && !status.capabilities_valid);
	capability_reads = reads[CH224A_REG_SOURCE_CAPABILITIES];
	assert(capability_reads == 1U + CH224A_IO_ATTEMPTS);
	failed_at = linkr_pd_test_uptime_ms;
	assert(linkr_debugger_pd_set_fixed(9000U) == -ENOTSUP);
	assert(linkr_debugger_pd_set_pps(10000U) == -ENOTSUP);
	assert(linkr_debugger_pd_set_fixed(5000U) == 0);
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(!status.capabilities_valid && status.fixed_count == 0U);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == capability_reads);
	assert(writes == 1U);
	failed_register = -1;
	set_source(12000U, 9000U);
	linkr_pd_test_uptime_ms = failed_at + 4999;
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(!status.capabilities_valid);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == capability_reads);
	linkr_pd_test_uptime_ms++;
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(status.capabilities_valid && status.fixed[1].voltage_mv == 12000U);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == capability_reads + 1U);

	start_pd();
	linkr_pd_test_uptime_ms += 5000;
	failed_register = CH224A_REG_SOURCE_CAPABILITIES;
	assert(linkr_debugger_pd_set_pps(10000U) == -ENOTSUP);
	assert(linkr_debugger_pd_set_fixed(9000U) == -ENOTSUP);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 1U + CH224A_IO_ATTEMPTS);
	assert(writes == 0U);
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
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 1U);
	registers[CH224A_REG_STATUS] = CH224A_STATUS_PD;
	set_source(12000U, 9000U);
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(!status.capabilities_valid);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 1U);
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
	failed_register = CH224A_REG_CURRENT;
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(!status.available && status.last_error == -EIO);
	assert(!status.capabilities_valid && !linkr_debugger_pd_available());
	status_reads = reads[CH224A_REG_STATUS];
	current_reads = reads[CH224A_REG_CURRENT];
	assert(current_reads == 1U + CH224A_IO_ATTEMPTS);
	failed_at = linkr_pd_test_uptime_ms;
	failed_register = -1;
	linkr_pd_test_uptime_ms = failed_at + 4999;
	assert(linkr_debugger_pd_get_status(&status) == 0);
	assert(!status.available && status.last_error == -EIO);
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(linkr_debugger_pd_set_fixed(9000U) == -EIO);
	assert(linkr_debugger_pd_set_pps(10000U) == -EIO);
	assert(reads[CH224A_REG_STATUS] == status_reads);
	assert(reads[CH224A_REG_CURRENT] == current_reads);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 1U);
	assert(writes == 0U);
	linkr_pd_test_uptime_ms++;
	assert(linkr_debugger_pd_get_status_short(&status) == 0);
	assert(status.available && status.last_error == 0 && !status.capabilities_valid);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 1U);
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
	assert(reads[CH224A_REG_STATUS] == status_reads);
	assert(reads[CH224A_REG_SOURCE_CAPABILITIES] == 0U);
}

int main(void)
{
	test_full_refresh_and_cache_only_short_status();
	test_explicit_requests_refresh_expired_capabilities();
	test_expired_capability_failure_blocks_requests_and_retries();
	test_inactive_pd_clears_capabilities_and_zero_status_is_valid();
	test_current_read_failure_propagates_and_probe_backs_off();
	test_initial_current_read_failure_backs_off();
	puts("PD/CH224A host tests passed");
	return 0;
}
