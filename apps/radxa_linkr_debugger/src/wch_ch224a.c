/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#define DT_DRV_COMPAT wch_ch224a

#include "wch_ch224a.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(wch_ch224a, LOG_LEVEL_INF);

#define CH224A_REG_STATUS  0x09U
#define CH224A_REG_VOLTAGE 0x0AU
#define CH224A_REG_CURRENT 0x50U
#define CH224A_REG_PPS     0x53U
#define CH224A_REG_SOURCE_CAPABILITIES 0x60U
#define CH224A_SOURCE_CAPABILITIES_LEN 48U

#define CH224A_ADDR_LOW  0x22U
#define CH224A_ADDR_HIGH 0x23U

#define CH224A_MODE_PPS 6U

#define CH224A_STATUS_BC  BIT(0)
#define CH224A_STATUS_QC2 BIT(1)
#define CH224A_STATUS_QC3 BIT(2)
#define CH224A_STATUS_PD  BIT(3)
#define CH224A_STATUS_EPR BIT(4)

/* Decode only named protocol flags. Presence depends on a successful I2C byte
 * read, never on the returned value; unknown/reserved bits remain forward-
 * compatible and are reported once rather than rejected.
 */
#define CH224A_STATUS_EPR_EXIST BIT(5)
#define CH224A_STATUS_AVS_EXIST BIT(6)
#define CH224A_STATUS_KNOWN_MASK (CH224A_STATUS_BC | CH224A_STATUS_QC2 | \
	CH224A_STATUS_QC3 | CH224A_STATUS_PD | CH224A_STATUS_EPR | \
	CH224A_STATUS_EPR_EXIST | CH224A_STATUS_AVS_EXIST)

#define CH224A_IO_ATTEMPTS       3
#define CH224A_RETRY_DELAY_MSEC 10

struct wch_ch224a_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec cfg1;
	const struct device *input_voltage_sensor;
	uint16_t maximum_request_mv;
	uint16_t input_present_threshold_mv;
	uint16_t input_absent_threshold_mv;
	uint8_t input_present_stable_samples;
	uint16_t input_sample_interval_ms;
};

struct wch_ch224a_data {
	struct i2c_dt_spec active;
	struct k_mutex lock;
	bool present;
	bool input_present;
	bool status_unknown_bits_warned;
};

static const uint16_t ch224a_fixed_voltage_mv[] = {
	5000U, 9000U, 12000U, 15000U, 20000U, 28000U,
};

static uint16_t wch_ch224a_other_address(uint16_t addr)
{
	return addr == CH224A_ADDR_LOW ? CH224A_ADDR_HIGH : CH224A_ADDR_LOW;
}

static bool wch_ch224a_probe_error_needs_recovery(int error)
{
	/* The PIO controller reports a clean address/data NAK as -EIO. Only
	 * timeout or busy/stuck-line classes may trigger the bounded bus-clear.
	 */
	return error == -ETIMEDOUT || error == -EBUSY;
}

static void wch_ch224a_note_status_bits_locked(struct wch_ch224a_data *data,
					       uint8_t status)
{
	uint8_t unknown_bits = status & (uint8_t)~CH224A_STATUS_KNOWN_MASK;

	if (unknown_bits != 0U && !data->status_unknown_bits_warned) {
		data->status_unknown_bits_warned = true;
		LOG_WRN("CH224A status 0x%02x has unknown/reserved bits 0x%02x; ignoring them",
			(unsigned int)status, (unsigned int)unknown_bits);
	}
}

static void wch_ch224a_clear_input_locked(struct wch_ch224a_data *data)
{
	data->input_present = false;
	data->present = false;
}

static int wch_ch224a_sample_input_mv_locked(const struct device *dev,
					      int32_t *input_mv)
{
	const struct wch_ch224a_config *config = dev->config;
	struct wch_ch224a_data *data = dev->data;
	struct sensor_value value;
	int64_t milli;
	int ret;

	if (!device_is_ready(config->input_voltage_sensor)) {
		wch_ch224a_clear_input_locked(data);
		return -ENODEV;
	}
	ret = sensor_sample_fetch(config->input_voltage_sensor);
	if (ret < 0) {
		wch_ch224a_clear_input_locked(data);
		return ret;
	}
	ret = sensor_channel_get(config->input_voltage_sensor,
				 SENSOR_CHAN_VOLTAGE, &value);
	if (ret < 0) {
		wch_ch224a_clear_input_locked(data);
		return ret;
	}
	milli = sensor_value_to_milli(&value);
	if (milli < 0 || milli > INT32_MAX) {
		wch_ch224a_clear_input_locked(data);
		return -ERANGE;
	}

	*input_mv = (int32_t)milli;
	return 0;
}

static int wch_ch224a_check_input_locked(const struct device *dev)
{
	const struct wch_ch224a_config *config = dev->config;
	struct wch_ch224a_data *data = dev->data;
	uint8_t samples = data->input_present ? 1U :
		config->input_present_stable_samples;

	/* This check prevents the next I2C transaction from starting after a low
	 * observation. It cannot close the ADC-to-I2C TOCTOU window or cancel an
	 * already-running transaction, and it is not powered-off Ioff isolation.
	 * The private voltage-divider sensor is sampled only while data->lock is
	 * held; the ADC driver independently serializes channel access.
	 */
	for (uint8_t i = 0U; i < samples; i++) {
		int32_t input_mv;
		int ret = wch_ch224a_sample_input_mv_locked(dev, &input_mv);

		if (ret < 0) {
			return ret;
		}
		if (data->input_present) {
			if (input_mv < config->input_absent_threshold_mv) {
				wch_ch224a_clear_input_locked(data);
				return -ENOLINK;
			}
			return 0;
		}
		if (input_mv < config->input_present_threshold_mv) {
			wch_ch224a_clear_input_locked(data);
			return -ENOLINK;
		}
		if (i + 1U < samples) {
			k_msleep(config->input_sample_interval_ms);
		}
	}

	data->input_present = true;
	return 0;
}

static int wch_ch224a_probe_locked(const struct device *dev)
{
	const struct wch_ch224a_config *config = dev->config;
	struct wch_ch224a_data *data = dev->data;
	uint16_t candidates[] = {
		data->active.addr,
		config->i2c.addr,
		wch_ch224a_other_address(config->i2c.addr),
	};
	bool recovery_attempted = false;
	uint8_t status;
	int ret = -ENODEV;
	int recovery_ret;

	data->present = false;

	for (int attempt = 0; attempt < CH224A_IO_ATTEMPTS; attempt++) {
		for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
			bool duplicate = false;

			for (size_t n = 0; n < i; n++) {
				if (candidates[n] == candidates[i]) {
					duplicate = true;
					break;
				}
			}
			if (duplicate) {
				continue;
			}

			data->active = config->i2c;
			data->active.addr = candidates[i];
			ret = wch_ch224a_check_input_locked(dev);
			if (ret < 0) {
				goto fail;
			}
			ret = i2c_reg_read_byte_dt(&data->active, CH224A_REG_STATUS, &status);
			if (ret == 0) {
				wch_ch224a_note_status_bits_locked(data, status);
				data->present = true;
				LOG_INF("CH224A ready at 0x%02x", data->active.addr);
				return 0;
			}
			if (recovery_attempted ||
			    !wch_ch224a_probe_error_needs_recovery(ret)) {
				continue;
			}

			/* Recovery emits SCL pulses, so gate it as strictly as an I2C
			 * transaction. The PIO driver's recover_bus callback takes the
			 * shared bus lock and preserves its fail-closed open-drain rules.
			 */
			recovery_attempted = true;
			ret = wch_ch224a_check_input_locked(dev);
			if (ret < 0) {
				goto fail;
			}
			recovery_ret = i2c_recover_bus(config->i2c.bus);
			if (recovery_ret < 0) {
				ret = recovery_ret;
				goto fail;
			}

			/* Re-sample after recovery before the one immediate retry. */
			ret = wch_ch224a_check_input_locked(dev);
			if (ret < 0) {
				goto fail;
			}
			ret = i2c_reg_read_byte_dt(&data->active, CH224A_REG_STATUS,
						   &status);
			if (ret == 0) {
				wch_ch224a_note_status_bits_locked(data, status);
				data->present = true;
				LOG_INF("CH224A ready at 0x%02x after bus recovery",
					data->active.addr);
				return 0;
			}
		}

		if (attempt + 1 < CH224A_IO_ATTEMPTS) {
			k_msleep(CH224A_RETRY_DELAY_MSEC);
		}
	}

fail:
	data->active = config->i2c;
	data->present = false;
	return ret;
}

static int wch_ch224a_ensure_present_locked(const struct device *dev)
{
	struct wch_ch224a_data *data = dev->data;

	return data->present ? 0 : wch_ch224a_probe_locked(dev);
}

static int wch_ch224a_read_retry_locked(const struct device *dev,
					uint8_t reg, uint8_t *value)
{
	struct wch_ch224a_data *data = dev->data;
	int ret = -EIO;

	for (int attempt = 0; attempt < CH224A_IO_ATTEMPTS; attempt++) {
		ret = wch_ch224a_check_input_locked(dev);
		if (ret < 0) {
			return ret;
		}
		ret = i2c_reg_read_byte_dt(&data->active, reg, value);
		if (ret == 0) {
			return 0;
		}
		if (attempt + 1 < CH224A_IO_ATTEMPTS) {
			k_msleep(CH224A_RETRY_DELAY_MSEC);
		}
	}

	data->present = false;
	return ret;
}

static int wch_ch224a_read_status_retry_locked(const struct device *dev,
						uint8_t *status)
{
	struct wch_ch224a_data *data = dev->data;
	int ret = -EIO;

	for (int attempt = 0; attempt < CH224A_IO_ATTEMPTS; attempt++) {
		ret = wch_ch224a_check_input_locked(dev);
		if (ret < 0) {
			return ret;
		}
		ret = i2c_reg_read_byte_dt(&data->active, CH224A_REG_STATUS, status);
		if (ret == 0) {
			wch_ch224a_note_status_bits_locked(data, *status);
			return 0;
		}
		if (attempt + 1 < CH224A_IO_ATTEMPTS) {
			k_msleep(CH224A_RETRY_DELAY_MSEC);
		}
	}

	return ret;
}

static int wch_ch224a_write_retry_locked(const struct device *dev,
					 uint8_t reg, uint8_t value)
{
	struct wch_ch224a_data *data = dev->data;
	int ret = -EIO;

	for (int attempt = 0; attempt < CH224A_IO_ATTEMPTS; attempt++) {
		ret = wch_ch224a_check_input_locked(dev);
		if (ret < 0) {
			return ret;
		}
		ret = i2c_reg_write_byte_dt(&data->active, reg, value);
		if (ret == 0) {
			return 0;
		}
		if (attempt + 1 < CH224A_IO_ATTEMPTS) {
			k_msleep(CH224A_RETRY_DELAY_MSEC);
		}
	}

	data->present = false;
	return ret;
}

static int wch_ch224a_block_read_retry_locked(const struct device *dev,
					      uint8_t reg, uint8_t *buf, size_t len)
{
	/* Source Capabilities occupy 0x60..0x8f.  The CH224A/Q datasheet does
	 * not specify register-address auto-increment or a block-read transaction
	 * shape, so read each documented address independently instead of relying
	 * on undocumented burst behavior. */
	for (size_t i = 0; i < len; i++) {
		int ret = wch_ch224a_read_retry_locked(dev, (uint8_t)(reg + i), &buf[i]);

		if (ret < 0) {
			return ret;
		}
	}
	return 0;
}

static uint16_t wch_ch224a_get_le16(const uint8_t *buf)
{
	return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static uint32_t wch_ch224a_get_le32(const uint8_t *buf)
{
	return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
	       ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static void wch_ch224a_parse_capabilities(
	const uint8_t *buf, struct wch_ch224a_capabilities *capabilities)
{
	uint16_t header = wch_ch224a_get_le16(buf);
	size_t offset = 2U;
	size_t count = (header >> 12) & 0x7U;

	memset(capabilities, 0, sizeof(*capabilities));
	if ((header & BIT(15)) != 0U) {
		uint16_t extended_header = wch_ch224a_get_le16(&buf[2]);

		offset = 4U;
		count = (extended_header & 0x1ffU) / 4U;
	}
	count = MIN(count, (CH224A_SOURCE_CAPABILITIES_LEN - offset) / 4U);

	for (size_t i = 0; i < count; i++) {
		uint32_t pdo = wch_ch224a_get_le32(&buf[offset + i * 4U]);
		uint8_t supply_type = (pdo >> 30) & 0x3U;

		if (pdo == 0U) {
			continue;
		}
		if (supply_type == 0U &&
		    capabilities->fixed_count < WCH_CH224A_MAX_SOURCE_PROFILES) {
			struct wch_ch224a_fixed_profile *profile =
				&capabilities->fixed[capabilities->fixed_count++];

			profile->voltage_mv = (uint16_t)(((pdo >> 10) & 0x3ffU) * 50U);
			profile->max_current_ma = (uint16_t)((pdo & 0x3ffU) * 10U);
		} else if (supply_type == 3U) {
			uint8_t apdo_type = (pdo >> 28) & 0x3U;

			if (apdo_type == 0U &&
			    capabilities->pps_count < WCH_CH224A_MAX_SOURCE_PROFILES) {
				struct wch_ch224a_programmable_range *range =
					&capabilities->pps[capabilities->pps_count++];

				range->max_voltage_mv =
					(uint16_t)(((pdo >> 17) & 0xffU) * 100U);
				range->min_voltage_mv =
					(uint16_t)(((pdo >> 8) & 0xffU) * 100U);
				range->step_mv = 100U;
				range->max_current_ma = (uint16_t)((pdo & 0x7fU) * 50U);
			} else if (apdo_type == 1U &&
				   capabilities->avs_count < WCH_CH224A_MAX_SOURCE_PROFILES) {
				struct wch_ch224a_programmable_range *range =
					&capabilities->avs[capabilities->avs_count++];

				range->max_voltage_mv =
					(uint16_t)(((pdo >> 17) & 0x1ffU) * 100U);
				range->min_voltage_mv =
					(uint16_t)(((pdo >> 8) & 0xffU) * 100U);
				range->step_mv = 100U;
				range->max_power_w = (uint16_t)(pdo & 0xffU);
			}
		}
	}

	capabilities->valid = capabilities->fixed_count > 0U ||
		capabilities->pps_count > 0U || capabilities->avs_count > 0U;
}

static int wch_ch224a_get_input_present_impl(const struct device *dev,
					      bool *present,
					      bool *newly_present)
{
	struct wch_ch224a_data *data = dev->data;
	bool was_present;
	int ret;

	if (present == NULL || newly_present == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	was_present = data->input_present;
	ret = wch_ch224a_check_input_locked(dev);
	*present = ret == 0;
	*newly_present = !was_present && ret == 0;
	k_mutex_unlock(&data->lock);

	/* A low voltage is a valid observation for this query, while sensor and
	 * conversion failures must remain distinguishable to the PD monitor.
	 */
	return ret == -ENOLINK ? 0 : ret;
}

static int wch_ch224a_probe_impl(const struct device *dev)
{
	struct wch_ch224a_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = wch_ch224a_probe_locked(dev);
	k_mutex_unlock(&data->lock);
	return ret;
}

static int wch_ch224a_get_status_impl(const struct device *dev,
				      struct wch_ch224a_status *status)
{
	struct wch_ch224a_data *data = dev->data;
	uint8_t reg09;
	uint8_t reg50;
	int ret;

	if (status == NULL) {
		return -EINVAL;
	}

	memset(status, 0, sizeof(*status));
	k_mutex_lock(&data->lock, K_FOREVER);

	ret = wch_ch224a_ensure_present_locked(dev);
	if (ret < 0) {
		goto out;
	}

	ret = wch_ch224a_read_status_retry_locked(dev, &reg09);
	if (ret < 0) {
		goto out;
	}

	/* Register 0x50 is defined only while a PD contract is active. */
	if ((reg09 & CH224A_STATUS_PD) != 0U) {
		ret = wch_ch224a_read_retry_locked(dev, CH224A_REG_CURRENT, &reg50);
		if (ret < 0) {
			goto out;
		}
	}
	ret = wch_ch224a_check_input_locked(dev);
	if (ret < 0) {
		goto out;
	}

	status->bc = (reg09 & CH224A_STATUS_BC) != 0U;
	status->qc2 = (reg09 & CH224A_STATUS_QC2) != 0U;
	status->qc3 = (reg09 & CH224A_STATUS_QC3) != 0U;
	status->pd_active = (reg09 & CH224A_STATUS_PD) != 0U;
	status->epr = (reg09 & CH224A_STATUS_EPR) != 0U;
	status->epr_exist = (reg09 & CH224A_STATUS_EPR_EXIST) != 0U;
	status->avs = (reg09 & CH224A_STATUS_AVS_EXIST) != 0U;
	status->max_current_ma = status->pd_active ? (uint16_t)reg50 * 50U : 0U;

	ret = 0;

out:
	if (ret < 0) {
		memset(status, 0, sizeof(*status));
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int wch_ch224a_get_capabilities_impl(
	const struct device *dev, struct wch_ch224a_capabilities *capabilities)
{
	struct wch_ch224a_data *data = dev->data;
	uint8_t buf[CH224A_SOURCE_CAPABILITIES_LEN];
	int ret;

	if (capabilities == NULL) {
		return -EINVAL;
	}

	memset(capabilities, 0, sizeof(*capabilities));
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = wch_ch224a_ensure_present_locked(dev);
	if (ret == 0) {
		ret = wch_ch224a_block_read_retry_locked(
			dev, CH224A_REG_SOURCE_CAPABILITIES, buf, sizeof(buf));
	}
	if (ret == 0) {
		ret = wch_ch224a_check_input_locked(dev);
	}
	if (ret == 0) {
		wch_ch224a_parse_capabilities(buf, capabilities);
		if (!capabilities->valid) {
			ret = -ENODATA;
		}
	}
	if (ret < 0) {
		memset(capabilities, 0, sizeof(*capabilities));
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int wch_ch224a_set_fixed_impl(const struct device *dev, uint8_t gear)
{
	const struct wch_ch224a_config *config = dev->config;
	struct wch_ch224a_data *data = dev->data;
	int ret;

	if (gear >= ARRAY_SIZE(ch224a_fixed_voltage_mv)) {
		return -EINVAL;
	}
	if (ch224a_fixed_voltage_mv[gear] > config->maximum_request_mv) {
		return -ERANGE;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = wch_ch224a_ensure_present_locked(dev);
	if (ret == 0) {
		ret = wch_ch224a_write_retry_locked(dev, CH224A_REG_VOLTAGE, gear);
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int wch_ch224a_set_pps_impl(const struct device *dev,
				   uint16_t voltage_mv)
{
	const struct wch_ch224a_config *config = dev->config;
	struct wch_ch224a_data *data = dev->data;
	uint8_t pps_code;
	int ret;

	if (voltage_mv < 3300U || voltage_mv > 21000U ||
	    voltage_mv > config->maximum_request_mv ||
	    (voltage_mv % 100U) != 0U) {
		return -EINVAL;
	}

	pps_code = (uint8_t)(voltage_mv / 100U);
	k_mutex_lock(&data->lock, K_FOREVER);

	ret = wch_ch224a_ensure_present_locked(dev);
	if (ret == 0) {
		/* Datasheet order: configure PPS voltage first, then select PPS mode. */
		ret = wch_ch224a_write_retry_locked(dev, CH224A_REG_PPS, pps_code);
	}
	if (ret == 0) {
		ret = wch_ch224a_write_retry_locked(dev, CH224A_REG_VOLTAGE,
						     CH224A_MODE_PPS);
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

static int wch_ch224a_init(const struct device *dev)
{
	const struct wch_ch224a_config *config = dev->config;
	struct wch_ch224a_data *data = dev->data;
	int ret;

	if (!i2c_is_ready_dt(&config->i2c)) {
		return -ENODEV;
	}
	if (config->i2c.addr != CH224A_ADDR_LOW &&
	    config->i2c.addr != CH224A_ADDR_HIGH) {
		return -EINVAL;
	}

	/*
	 * CFG1 is selected by the external Rset in I2C mode. x1.2 connects an
	 * MCU GPIO through R97 only for optional board control; it must remain
	 * high impedance so it cannot override Rset or enter I/O level mode.
	 */
	if (config->cfg1.port != NULL) {
		if (!gpio_is_ready_dt(&config->cfg1)) {
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&config->cfg1, GPIO_INPUT);
		if (ret < 0) {
			return ret;
		}
	}

	/* Keep init bus-silent. The DT node defers this initializer until main calls
	 * device_init(), after the I2C controller and voltage-divider sensor have
	 * completed their normal initialization. Runtime readiness is still checked
	 * by every I2C path.
	 */
	data->active = config->i2c;
	data->present = false;
	data->input_present = false;
	data->status_unknown_bits_warned = false;
	k_mutex_init(&data->lock);
	LOG_INF("CH224A driver initialized at 0x%02x; CFG1 is high impedance",
		config->i2c.addr);
	return 0;
}

static const struct wch_ch224a_driver_api wch_ch224a_api = {
	.get_input_present = wch_ch224a_get_input_present_impl,
	.probe = wch_ch224a_probe_impl,
	.get_status = wch_ch224a_get_status_impl,
	.get_capabilities = wch_ch224a_get_capabilities_impl,
	.set_fixed = wch_ch224a_set_fixed_impl,
	.set_pps = wch_ch224a_set_pps_impl,
};

#define WCH_CH224A_INIT(inst)                                                    \
	BUILD_ASSERT(DT_INST_PROP(inst, input_absent_threshold_millivolt) > 0);    \
	BUILD_ASSERT(DT_INST_PROP(inst, input_absent_threshold_millivolt) <       \
		     DT_INST_PROP(inst, input_present_threshold_millivolt));        \
	BUILD_ASSERT(DT_INST_PROP(inst, input_present_threshold_millivolt) < 3300); \
	BUILD_ASSERT(DT_INST_PROP(inst, input_present_stable_samples) >= 2 &&     \
		     DT_INST_PROP(inst, input_present_stable_samples) <= 16);       \
	BUILD_ASSERT(DT_INST_PROP(inst, input_sample_interval_ms) > 0 &&          \
		     DT_INST_PROP(inst, input_sample_interval_ms) <= 1000);          \
	static const struct wch_ch224a_config wch_ch224a_config_##inst = {        \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                \
		.cfg1 = GPIO_DT_SPEC_INST_GET_OR(inst, cfg1_gpios, {0}),          \
		.input_voltage_sensor =                                           \
			DEVICE_DT_GET(DT_INST_PHANDLE(inst, input_voltage_sensor)),   \
		.maximum_request_mv = DT_INST_PROP(inst, maximum_request_millivolt), \
		.input_present_threshold_mv =                                     \
			DT_INST_PROP(inst, input_present_threshold_millivolt),        \
		.input_absent_threshold_mv =                                      \
			DT_INST_PROP(inst, input_absent_threshold_millivolt),         \
		.input_present_stable_samples =                                   \
			DT_INST_PROP(inst, input_present_stable_samples),             \
		.input_sample_interval_ms =                                       \
			DT_INST_PROP(inst, input_sample_interval_ms),                 \
	};                                                                        \
	static struct wch_ch224a_data wch_ch224a_data_##inst;                     \
	DEVICE_DT_INST_DEFINE(inst, wch_ch224a_init, NULL,                        \
			      &wch_ch224a_data_##inst,                            \
			      &wch_ch224a_config_##inst, POST_KERNEL, 51,          \
			      &wch_ch224a_api);

DT_INST_FOREACH_STATUS_OKAY(WCH_CH224A_INIT)
