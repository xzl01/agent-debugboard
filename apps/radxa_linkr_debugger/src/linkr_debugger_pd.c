/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_pd.h"
#include "wch_ch224a.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(linkr_debugger_pd, LOG_LEVEL_INF);

static const struct device *const ch224a_dev =
	DEVICE_DT_GET(DT_NODELABEL(ch224a));

K_MUTEX_DEFINE(linkr_debugger_pd_lock);

static bool pd_available;
static enum linkr_debugger_pd_mode pd_mode = LINKR_DEBUGGER_PD_MODE_FIXED;
static uint8_t pd_gear;
static uint16_t pd_pps_mv;
static int pd_last_error;
static bool pd_requested_voltage_valid;
static struct wch_ch224a_capabilities pd_capabilities;
static int64_t pd_probe_retry_after_ms;
static int64_t pd_capability_retry_after_ms;
static int64_t pd_capability_valid_until_ms;

#define PD_PROBE_RETRY_MSEC      5000
#define PD_CAPABILITY_RETRY_MSEC 5000
/* Bound stale source data without reading all 48 registers on every poll. */
#define PD_CAPABILITY_TTL_MSEC   5000

static const uint16_t pd_voltages[] = {
	LINKR_DEBUGGER_PD_VOLTAGE_5V,
	LINKR_DEBUGGER_PD_VOLTAGE_9V,
	LINKR_DEBUGGER_PD_VOLTAGE_12V,
	LINKR_DEBUGGER_PD_VOLTAGE_15V,
	LINKR_DEBUGGER_PD_VOLTAGE_20V,
};

static void pd_clear_capabilities_locked(void)
{
	memset(&pd_capabilities, 0, sizeof(pd_capabilities));
	pd_capability_valid_until_ms = 0;
	pd_capability_retry_after_ms = 0;
}

static void pd_mark_unavailable_locked(int error)
{
	pd_available = false;
	pd_last_error = error;
	pd_probe_retry_after_ms = k_uptime_get() + PD_PROBE_RETRY_MSEC;
	pd_clear_capabilities_locked();
}

static int pd_probe_locked(void)
{
	int ret;

	if (k_uptime_get() < pd_probe_retry_after_ms) return pd_last_error;
	ret = wch_ch224a_probe(ch224a_dev);
	if (ret < 0) {
		pd_mark_unavailable_locked(ret);
		return ret;
	}
	pd_available = true;
	pd_last_error = 0;
	pd_probe_retry_after_ms = 0;
	return 0;
}

static enum linkr_debugger_pd_protocol pd_protocol(
	const struct wch_ch224a_status *status)
{
	if (status->epr) return LINKR_DEBUGGER_PD_PROTOCOL_EPR;
	if (status->pd_active) return LINKR_DEBUGGER_PD_PROTOCOL_PD;
	if (status->qc3) return LINKR_DEBUGGER_PD_PROTOCOL_QC3;
	if (status->qc2) return LINKR_DEBUGGER_PD_PROTOCOL_QC2;
	if (status->bc) return LINKR_DEBUGGER_PD_PROTOCOL_BC;
	return LINKR_DEBUGGER_PD_PROTOCOL_NONE;
}

static void pd_copy_capabilities(struct linkr_debugger_pd_status *status)
{
	const uint16_t board_max_mv = LINKR_DEBUGGER_PD_VOLTAGE_20V;

	for (size_t i = 0; i < pd_capabilities.fixed_count; i++) {
		const struct wch_ch224a_fixed_profile *source = &pd_capabilities.fixed[i];
		struct linkr_debugger_pd_fixed_profile *dest;

		if (source->voltage_mv > board_max_mv ||
		    status->fixed_count >= LINKR_DEBUGGER_PD_MAX_SOURCE_PROFILES) continue;
		dest = &status->fixed[status->fixed_count++];
		dest->voltage_mv = source->voltage_mv;
		dest->max_current_ma = source->max_current_ma;
	}
	for (size_t i = 0; i < pd_capabilities.pps_count; i++) {
		const struct wch_ch224a_programmable_range *source = &pd_capabilities.pps[i];
		struct linkr_debugger_pd_programmable_range *dest;
		uint16_t max_mv = MIN(source->max_voltage_mv, board_max_mv);

		if (source->min_voltage_mv > max_mv ||
		    status->pps_count >= LINKR_DEBUGGER_PD_MAX_SOURCE_PROFILES) continue;
		dest = &status->pps[status->pps_count++];
		dest->min_voltage_mv = source->min_voltage_mv;
		dest->max_voltage_mv = max_mv;
		dest->step_mv = source->step_mv;
		dest->max_current_ma = source->max_current_ma;
	}
	for (size_t i = 0; i < pd_capabilities.avs_count; i++) {
		const struct wch_ch224a_programmable_range *source = &pd_capabilities.avs[i];
		struct linkr_debugger_pd_programmable_range *dest;
		uint16_t max_mv = MIN(source->max_voltage_mv, board_max_mv);

		if (source->min_voltage_mv > max_mv ||
		    status->avs_count >= LINKR_DEBUGGER_PD_MAX_SOURCE_PROFILES) continue;
		dest = &status->avs_ranges[status->avs_count++];
		*dest = (struct linkr_debugger_pd_programmable_range) {
			.min_voltage_mv = source->min_voltage_mv,
			.max_voltage_mv = max_mv,
			.step_mv = source->step_mv,
			.max_power_w = source->max_power_w,
		};
	}
	status->capabilities_valid = status->fixed_count > 0U ||
		status->pps_count > 0U || status->avs_count > 0U;
}

int linkr_debugger_pd_voltage_to_gear(uint16_t voltage_mv, uint8_t *gear)
{
	if (gear == NULL) {
		return -EINVAL;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(pd_voltages); i++) {
		if (pd_voltages[i] == voltage_mv) {
			*gear = (uint8_t)i;
			return 0;
		}
	}

	return -EINVAL;
}

int linkr_debugger_pd_gear_to_voltage(uint8_t gear, uint16_t *voltage_mv)
{
	if (voltage_mv == NULL || gear >= ARRAY_SIZE(pd_voltages)) {
		return -EINVAL;
	}

	*voltage_mv = pd_voltages[gear];
	return 0;
}

static bool pd_pps_valid(uint16_t voltage_mv)
{
	return voltage_mv >= LINKR_DEBUGGER_PD_PPS_MIN_MV &&
	       voltage_mv <= LINKR_DEBUGGER_PD_PPS_MAX_MV &&
	       (voltage_mv % LINKR_DEBUGGER_PD_PPS_STEP_MV) == 0U;
}

static uint16_t pd_current_voltage_mv(void)
{
	if (!pd_requested_voltage_valid) return 0U;
	if (pd_mode == LINKR_DEBUGGER_PD_MODE_PPS) {
		return pd_pps_mv;
	}

	return pd_gear < ARRAY_SIZE(pd_voltages) ? pd_voltages[pd_gear] : 0U;
}

static int pd_refresh_capabilities_locked(void)
{
	int ret;

	if (pd_capabilities.valid &&
	    k_uptime_get() < pd_capability_valid_until_ms) return 0;
	if (k_uptime_get() < pd_capability_retry_after_ms) return -EAGAIN;
	ret = wch_ch224a_get_capabilities(ch224a_dev, &pd_capabilities);
	if (ret < 0) {
		pd_clear_capabilities_locked();
		pd_capability_retry_after_ms = k_uptime_get() + PD_CAPABILITY_RETRY_MSEC;
	} else {
		pd_capability_retry_after_ms = 0;
		pd_capability_valid_until_ms = k_uptime_get() + PD_CAPABILITY_TTL_MSEC;
	}
	return ret;
}

static bool pd_fixed_is_advertised(uint16_t voltage_mv)
{
	for (size_t i = 0; i < pd_capabilities.fixed_count; i++) {
		if (pd_capabilities.fixed[i].voltage_mv == voltage_mv) return true;
	}
	return false;
}

static bool pd_pps_is_advertised(uint16_t voltage_mv)
{
	for (size_t i = 0; i < pd_capabilities.pps_count; i++) {
		const struct wch_ch224a_programmable_range *range = &pd_capabilities.pps[i];

		if (range->step_mv != 0U && voltage_mv >= range->min_voltage_mv &&
		    voltage_mv <= range->max_voltage_mv &&
		    ((voltage_mv - range->min_voltage_mv) % range->step_mv) == 0U) return true;
	}
	return false;
}

static int pd_apply_fixed_locked(uint8_t gear)
{
	int ret;

	if (gear >= ARRAY_SIZE(pd_voltages)) {
		return -EINVAL;
	}
	if (!pd_available) {
		ret = pd_probe_locked();
		if (ret < 0) {
			return ret;
		}
	}
	/* 5 V is the fail-safe request and must remain available even when the
	 * optional Source_Capabilities window cannot be refreshed. Higher fixed
	 * requests require a fresh matching PDO from the connected source. */
	if (gear != 0U && (pd_refresh_capabilities_locked() < 0 ||
	    !pd_fixed_is_advertised(pd_voltages[gear]))) return -ENOTSUP;

	ret = wch_ch224a_set_fixed(ch224a_dev, gear);
	if (ret < 0) {
		pd_mark_unavailable_locked(ret);
		return ret;
	}

	pd_mode = LINKR_DEBUGGER_PD_MODE_FIXED;
	pd_gear = gear;
	pd_pps_mv = 0U;
	pd_last_error = 0;
	pd_requested_voltage_valid = true;
	return 0;
}

static int pd_apply_pps_locked(uint16_t voltage_mv)
{
	int ret;

	if (!pd_pps_valid(voltage_mv)) {
		return -EINVAL;
	}
	if (!pd_available) {
		ret = pd_probe_locked();
		if (ret < 0) {
			return ret;
		}
	}
	if (pd_refresh_capabilities_locked() < 0 || !pd_pps_is_advertised(voltage_mv)) {
		return -ENOTSUP;
	}

	ret = wch_ch224a_set_pps(ch224a_dev, voltage_mv);
	if (ret < 0) {
		pd_mark_unavailable_locked(ret);
		return ret;
	}

	pd_mode = LINKR_DEBUGGER_PD_MODE_PPS;
	pd_gear = 0U;
	pd_pps_mv = voltage_mv;
	pd_last_error = 0;
	pd_requested_voltage_valid = true;
	return 0;
}

int linkr_debugger_pd_init(void)
{
	struct wch_ch224a_status status;
	int ret;

	if (!device_is_ready(ch224a_dev)) {
		pd_available = false;
		pd_last_error = -ENODEV;
		LOG_WRN("CH224A driver not ready");
		return -ENODEV;
	}

	k_mutex_lock(&linkr_debugger_pd_lock, K_FOREVER);

	ret = pd_probe_locked();
	if (ret < 0) {
		k_mutex_unlock(&linkr_debugger_pd_lock);
		LOG_WRN("CH224A not available at 0x22 or 0x23: %d", ret);
		return ret;
	}

	ret = wch_ch224a_get_status(ch224a_dev, &status);
	if (ret < 0) {
		pd_mark_unavailable_locked(ret);
		LOG_WRN("Failed to read initial PD status: %d", ret);
	} else if (status.pd_active) {
		(void)pd_refresh_capabilities_locked();
	} else {
		pd_clear_capabilities_locked();
	}

	k_mutex_unlock(&linkr_debugger_pd_lock);

	LOG_INF("PD discovery complete; startup voltage remains hardware-selected");
	return 0;
}

bool linkr_debugger_pd_available(void)
{
	return pd_available;
}

int linkr_debugger_pd_set_fixed(uint16_t voltage_mv)
{
	uint8_t gear;
	int ret;

	ret = linkr_debugger_pd_voltage_to_gear(voltage_mv, &gear);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&linkr_debugger_pd_lock, K_FOREVER);

	ret = pd_apply_fixed_locked(gear);
	if (ret < 0) {
		k_mutex_unlock(&linkr_debugger_pd_lock);
		return ret;
	}

	k_mutex_unlock(&linkr_debugger_pd_lock);
	return 0;
}

int linkr_debugger_pd_set_pps(uint16_t voltage_mv)
{
	int ret;

	if (!pd_pps_valid(voltage_mv)) {
		return -EINVAL;
	}

	k_mutex_lock(&linkr_debugger_pd_lock, K_FOREVER);

	ret = pd_apply_pps_locked(voltage_mv);
	if (ret < 0) {
		k_mutex_unlock(&linkr_debugger_pd_lock);
		return ret;
	}

	k_mutex_unlock(&linkr_debugger_pd_lock);
	return 0;
}

static int pd_get_status(struct linkr_debugger_pd_status *status,
			 bool refresh_capabilities)
{
	struct wch_ch224a_status drv_status;
	int ret;

	if (status == NULL) {
		return -EINVAL;
	}

	memset(status, 0, sizeof(*status));

	k_mutex_lock(&linkr_debugger_pd_lock, K_FOREVER);
	if (!pd_available && pd_probe_locked() < 0) {
		status->available = false;
		status->mode = pd_mode;
		status->gear = pd_gear;
		status->pps_mv = pd_mode == LINKR_DEBUGGER_PD_MODE_PPS ? pd_pps_mv : 0U;
		status->voltage_mv = pd_current_voltage_mv();
		status->last_error = pd_last_error;
		k_mutex_unlock(&linkr_debugger_pd_lock);
		return 0;
	}

	status->available = pd_available;
	status->mode = pd_mode;
	status->gear = pd_gear;
	status->pps_mv = pd_mode == LINKR_DEBUGGER_PD_MODE_PPS ? pd_pps_mv : 0U;
	status->voltage_mv = pd_current_voltage_mv();
	status->last_error = pd_last_error;

	ret = wch_ch224a_get_status(ch224a_dev, &drv_status);
	if (ret < 0) {
		pd_mark_unavailable_locked(ret);
		status->available = false;
		status->last_error = ret;
		k_mutex_unlock(&linkr_debugger_pd_lock);
		return 0;
	}

	status->bc = drv_status.bc;
	status->qc2 = drv_status.qc2;
	status->qc3 = drv_status.qc3;
	status->pd_active = drv_status.pd_active;
	status->epr = drv_status.epr;
	status->epr_exist = drv_status.epr_exist;
	status->avs = drv_status.avs;
	status->max_current_ma = drv_status.max_current_ma;
	status->protocol = pd_protocol(&drv_status);
	if (!drv_status.pd_active) {
		pd_clear_capabilities_locked();
	} else if (refresh_capabilities) {
		(void)pd_refresh_capabilities_locked();
	}
	if (pd_capabilities.valid) {
		pd_copy_capabilities(status);
	}
	status->last_error = 0;
	pd_last_error = 0;

	k_mutex_unlock(&linkr_debugger_pd_lock);
	return 0;
}

int linkr_debugger_pd_get_status_short(struct linkr_debugger_pd_status *status)
{
	return pd_get_status(status, false);
}

int linkr_debugger_pd_get_status(struct linkr_debugger_pd_status *status)
{
	return pd_get_status(status, true);
}
