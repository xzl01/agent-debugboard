/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef LINKR_PD_TEST_I2C_H_
#define LINKR_PD_TEST_I2C_H_

#include <zephyr/device.h>
#include <stdint.h>

struct i2c_dt_spec {
	const struct device *bus;
	uint16_t addr;
};

static inline bool i2c_is_ready_dt(const struct i2c_dt_spec *spec)
{
	return spec->bus != NULL;
}

int i2c_reg_read_byte_dt(const struct i2c_dt_spec *spec, uint8_t reg, uint8_t *value);
int i2c_reg_write_byte_dt(const struct i2c_dt_spec *spec, uint8_t reg, uint8_t value);
int i2c_recover_bus(const struct device *dev);

#endif
