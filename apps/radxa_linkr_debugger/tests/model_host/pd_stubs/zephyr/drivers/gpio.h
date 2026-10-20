/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef LINKR_PD_TEST_GPIO_H_
#define LINKR_PD_TEST_GPIO_H_

#include <zephyr/device.h>

struct gpio_dt_spec {
	const struct device *port;
};

#define GPIO_INPUT 0

static inline bool gpio_is_ready_dt(const struct gpio_dt_spec *spec)
{
	return spec->port != NULL;
}

static inline int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, int flags)
{
	(void)spec;
	(void)flags;
	return 0;
}

#endif
