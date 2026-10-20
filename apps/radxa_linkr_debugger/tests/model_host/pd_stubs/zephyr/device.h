/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef LINKR_PD_TEST_DEVICE_H_
#define LINKR_PD_TEST_DEVICE_H_

#include <stdbool.h>
#include <stddef.h>

struct device {
	const void *config;
	void *data;
	const void *api;
};

extern const struct device linkr_pd_test_device;
extern const struct device linkr_pd_test_sensor_device;

bool linkr_pd_test_device_is_ready(const struct device *dev);
int device_init(const struct device *dev);

#define DEVICE_DT_GET(node) (&linkr_pd_test_device)
#define DT_INST_FOREACH_STATUS_OKAY(fn)

static inline bool device_is_ready(const struct device *dev)
{
	return linkr_pd_test_device_is_ready(dev);
}

#endif
