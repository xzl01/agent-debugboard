/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include <assert.h>
#include <errno.h>
#include "linkr_debugger_model.h"

#define BIT(n) (UINT32_C(1) << (n))
#define K_FOREVER 0
#define LINKR_DEBUGGER_GPIO_DIR_INPUT 1U
#define LINKR_DEBUGGER_GPIO_DIR_OUTPUT 2U
#define GPIO_INPUT 0
#define GPIO_OUTPUT_INACTIVE 1
#define GPIO_OUTPUT_ACTIVE 2

static int linkr_debugger_control_lock;
static uint8_t linkr_debugger_gpio_directions[32];
static bool linkr_debugger_gpio_output_levels[32];
static const int gpio0;
static unsigned int gpio_configure_calls;
static int gpio_configure_result;

static void k_mutex_lock(int *lock, int timeout)
{
	(void)timeout;
	assert(*lock == 0);
	*lock = 1;
}
static void k_mutex_unlock(int *lock)
{
	assert(*lock == 1);
	*lock = 0;
}
static bool safe_gpio_index_valid(const struct linkr_debugger_safe_gpio_desc *desc,
	size_t *index)
{
	for (size_t i = 0U; i < linkr_debugger_safe_gpio_count; i++) {
		if (desc == &linkr_debugger_safe_gpios[i]) {
			*index = i;
			return true;
		}
	}
	return false;
}
static bool device_is_ready(int device) { (void)device; return true; }
static int gpio_pin_configure(int device, uint8_t pin, int flags)
{
	(void)device; (void)pin; (void)flags;
	assert(linkr_debugger_control_lock == 1);
	gpio_configure_calls++;
	return gpio_configure_result;
}
static int gpio_pin_get(int device, uint8_t pin)
{
	(void)device; (void)pin;
	assert(linkr_debugger_control_lock == 1);
	return 0;
}
static void linkr_debugger_gpio_apply_safe_drive_strength(
	const struct linkr_debugger_safe_gpio_desc *desc)
{
	(void)desc;
	assert(linkr_debugger_control_lock == 1);
}
