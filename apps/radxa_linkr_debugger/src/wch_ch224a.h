/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_WCH_CH224A_H_
#define RADXA_LINKR_DEBUGGER_WCH_CH224A_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#define WCH_CH224A_MAX_SOURCE_PROFILES 10U

struct wch_ch224a_fixed_profile {
	uint16_t voltage_mv;
	uint16_t max_current_ma;
};

struct wch_ch224a_programmable_range {
	uint16_t min_voltage_mv;
	uint16_t max_voltage_mv;
	uint16_t step_mv;
	uint16_t max_current_ma;
	uint16_t max_power_w;
};

struct wch_ch224a_capabilities {
	bool valid;
	uint8_t fixed_count;
	uint8_t pps_count;
	uint8_t avs_count;
	struct wch_ch224a_fixed_profile fixed[WCH_CH224A_MAX_SOURCE_PROFILES];
	struct wch_ch224a_programmable_range pps[WCH_CH224A_MAX_SOURCE_PROFILES];
	struct wch_ch224a_programmable_range avs[WCH_CH224A_MAX_SOURCE_PROFILES];
};

struct wch_ch224a_status {
	bool bc;
	bool qc2;
	bool qc3;
	bool pd_active;
	bool epr;
	bool epr_exist;
	bool avs;
	uint16_t max_current_ma;
};

/* Every API operation that can issue I2C is source-voltage gated. It may
 * return -ENOLINK for an absent input, or propagate sensor/ADC errors before
 * any CH224A transaction starts.
 */
struct wch_ch224a_driver_api {
	int (*get_input_present)(const struct device *dev, bool *present,
				 bool *newly_present);
	int (*probe)(const struct device *dev);
	int (*get_status)(const struct device *dev,
			  struct wch_ch224a_status *status);
	int (*get_capabilities)(const struct device *dev,
				struct wch_ch224a_capabilities *capabilities);
	int (*set_fixed)(const struct device *dev, uint8_t gear);
	int (*set_pps)(const struct device *dev, uint16_t voltage_mv);
};

/* Sample the private input-voltage gate without issuing I2C. A successful low
 * sample is reported as present=false; sensor/ADC failures are returned.
 * newly_present reports a completed absent-to-present stable transition even
 * when another driver operation observed the preceding low sample.
 */
static inline int wch_ch224a_get_input_present(const struct device *dev,
					       bool *present,
					       bool *newly_present)
{
	const struct wch_ch224a_driver_api *api =
		(const struct wch_ch224a_driver_api *)dev->api;

	return api->get_input_present(dev, present, newly_present);
}

static inline int wch_ch224a_probe(const struct device *dev)
{
	const struct wch_ch224a_driver_api *api =
		(const struct wch_ch224a_driver_api *)dev->api;

	return api->probe(dev);
}

static inline int wch_ch224a_get_capabilities(
	const struct device *dev, struct wch_ch224a_capabilities *capabilities)
{
	const struct wch_ch224a_driver_api *api =
		(const struct wch_ch224a_driver_api *)dev->api;

	return api->get_capabilities(dev, capabilities);
}

static inline int wch_ch224a_get_status(const struct device *dev,
					struct wch_ch224a_status *status)
{
	const struct wch_ch224a_driver_api *api =
		(const struct wch_ch224a_driver_api *)dev->api;

	return api->get_status(dev, status);
}

static inline int wch_ch224a_set_fixed(const struct device *dev, uint8_t gear)
{
	const struct wch_ch224a_driver_api *api =
		(const struct wch_ch224a_driver_api *)dev->api;

	return api->set_fixed(dev, gear);
}

static inline int wch_ch224a_set_pps(const struct device *dev,
				     uint16_t voltage_mv)
{
	const struct wch_ch224a_driver_api *api =
		(const struct wch_ch224a_driver_api *)dev->api;

	return api->set_pps(dev, voltage_mv);
}

#endif /* RADXA_LINKR_DEBUGGER_WCH_CH224A_H_ */
