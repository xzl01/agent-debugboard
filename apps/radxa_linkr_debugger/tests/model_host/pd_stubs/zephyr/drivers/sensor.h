/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef LINKR_PD_TEST_SENSOR_H_
#define LINKR_PD_TEST_SENSOR_H_

#include <zephyr/device.h>
#include <stdint.h>

enum sensor_channel {
	SENSOR_CHAN_VOLTAGE,
};

struct sensor_value {
	int32_t val1;
	int32_t val2;
};

int sensor_sample_fetch(const struct device *dev);
int sensor_channel_get(const struct device *dev, enum sensor_channel chan,
		       struct sensor_value *value);

static inline int64_t sensor_value_to_milli(const struct sensor_value *value)
{
	return (int64_t)value->val1 * 1000LL + value->val2 / 1000;
}

#endif /* LINKR_PD_TEST_SENSOR_H_ */
