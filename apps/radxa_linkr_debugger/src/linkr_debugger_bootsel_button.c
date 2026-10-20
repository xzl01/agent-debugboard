/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_bootsel_button.h"

#include <stdbool.h>
#include <stddef.h>

void linkr_debugger_bootsel_button_state_init(
	struct linkr_debugger_bootsel_button_state *state)
{
	if (state == NULL) {
		return;
	}

	state->id = LINKR_DEBUGGER_BOOTSEL_STATE_RELEASED;
	state->consecutive_samples = 0U;
	state->pressed_since_ms = 0;
}

static uint32_t linkr_debugger_bootsel_debounce_samples(
	const struct linkr_debugger_bootsel_button_config *config)
{
	return config->debounce_samples == 0U ? 1U : config->debounce_samples;
}

static void linkr_debugger_bootsel_confirm_press(
	struct linkr_debugger_bootsel_button_state *state,
	const struct linkr_debugger_bootsel_button_config *config,
	int64_t now_ms)
{
	state->consecutive_samples = 0U;
	state->id = now_ms - state->pressed_since_ms >= config->long_press_ms ?
		LINKR_DEBUGGER_BOOTSEL_STATE_WAIT_RELEASE :
		LINKR_DEBUGGER_BOOTSEL_STATE_PRESSED;
}

enum linkr_debugger_bootsel_button_action
linkr_debugger_bootsel_button_state_step(
	struct linkr_debugger_bootsel_button_state *state,
	const struct linkr_debugger_bootsel_button_config *config,
	enum linkr_debugger_bootsel_button_sample sample,
	int64_t now_ms)
{
	uint32_t debounce_samples;

	if (state == NULL || config == NULL) {
		return LINKR_DEBUGGER_BOOTSEL_ACTION_NONE;
	}
	if (sample == LINKR_DEBUGGER_BOOTSEL_SAMPLE_UNAVAILABLE) {
		/* Never infer a long press across an interval where flash ownership
		 * prevented observation; cancel the gesture and require a fresh hold.
		 */
		linkr_debugger_bootsel_button_state_init(state);
		return LINKR_DEBUGGER_BOOTSEL_ACTION_NONE;
	}

	debounce_samples = linkr_debugger_bootsel_debounce_samples(config);

	switch (state->id) {
	case LINKR_DEBUGGER_BOOTSEL_STATE_RELEASED:
		if (sample == LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED) {
			state->pressed_since_ms = now_ms;
			state->consecutive_samples = 1U;
			if (debounce_samples == 1U) {
				linkr_debugger_bootsel_confirm_press(state, config, now_ms);
			} else {
				state->id = LINKR_DEBUGGER_BOOTSEL_STATE_DEBOUNCE_PRESS;
			}
		}
		break;

	case LINKR_DEBUGGER_BOOTSEL_STATE_DEBOUNCE_PRESS:
		if (sample == LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED) {
			linkr_debugger_bootsel_button_state_init(state);
			break;
		}
		state->consecutive_samples++;
		if (state->consecutive_samples >= debounce_samples) {
			linkr_debugger_bootsel_confirm_press(state, config, now_ms);
		}
		break;

	case LINKR_DEBUGGER_BOOTSEL_STATE_PRESSED:
		if (now_ms - state->pressed_since_ms >= config->long_press_ms) {
			state->id = LINKR_DEBUGGER_BOOTSEL_STATE_WAIT_RELEASE;
			state->consecutive_samples =
				sample == LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED ? 1U : 0U;
			if (state->consecutive_samples >= debounce_samples) {
				linkr_debugger_bootsel_button_state_init(state);
				return LINKR_DEBUGGER_BOOTSEL_ACTION_REBOOT;
			}
			break;
		}
		if (sample == LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED) {
			state->consecutive_samples = 0U;
			break;
		}
		state->consecutive_samples++;
		if (state->consecutive_samples >= debounce_samples) {
			linkr_debugger_bootsel_button_state_init(state);
		}
		break;

	case LINKR_DEBUGGER_BOOTSEL_STATE_WAIT_RELEASE:
		if (sample == LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED) {
			state->consecutive_samples = 0U;
			break;
		}
		state->consecutive_samples++;
		if (state->consecutive_samples >= debounce_samples) {
			linkr_debugger_bootsel_button_state_init(state);
			return LINKR_DEBUGGER_BOOTSEL_ACTION_REBOOT;
		}
		break;
	}

	return LINKR_DEBUGGER_BOOTSEL_ACTION_NONE;
}

#ifndef LINKR_DEBUGGER_BOOTSEL_BUTTON_HOST_TEST

#define DT_DRV_COMPAT radxa_linkr_bootsel_button

#include "linkr_debugger_flash_arbiter.h"
#include "linkr_debugger_reboot.h"

#include <hardware/regs/io_qspi.h>
#include <hardware/regs/sio.h>
#include <hardware/structs/io_qspi.h>
#include <hardware/structs/sio.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

LOG_MODULE_REGISTER(linkr_debugger_bootsel_button,
			CONFIG_LINKR_DEBUGGER_LOG_LEVEL);

#define LINKR_DEBUGGER_BOOTSEL_NODE \
	DT_COMPAT_GET_ANY_STATUS_OKAY(DT_DRV_COMPAT)
#define LINKR_DEBUGGER_BOOTSEL_SAMPLE_INTERVAL_MS \
	DT_PROP(LINKR_DEBUGGER_BOOTSEL_NODE, sample_interval_ms)
#define LINKR_DEBUGGER_BOOTSEL_DEBOUNCE_SAMPLES \
	DT_PROP(LINKR_DEBUGGER_BOOTSEL_NODE, debounce_samples)
#define LINKR_DEBUGGER_BOOTSEL_LONG_PRESS_MS \
	DT_PROP(LINKR_DEBUGGER_BOOTSEL_NODE, long_press_ms)
#define LINKR_DEBUGGER_BOOTSEL_STACK_SIZE 1024U
#define LINKR_DEBUGGER_BOOTSEL_THREAD_PRIORITY K_PRIO_PREEMPT(10)
#define LINKR_DEBUGGER_BOOTSEL_QSPI_SS_INDEX 1U
#define LINKR_DEBUGGER_BOOTSEL_SETTLE_CYCLES 1000U

BUILD_ASSERT(DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT),
	     "BOOTSEL button devicetree node is required");
BUILD_ASSERT(CONFIG_MP_MAX_NUM_CPUS == 1,
	     "BOOTSEL flash-CS sampling requires a single-core build");
BUILD_ASSERT(LINKR_DEBUGGER_BOOTSEL_SAMPLE_INTERVAL_MS > 0U);
BUILD_ASSERT(LINKR_DEBUGGER_BOOTSEL_DEBOUNCE_SAMPLES > 0U);
BUILD_ASSERT(LINKR_DEBUGGER_BOOTSEL_LONG_PRESS_MS > 0U);

static __ramfunc bool linkr_debugger_bootsel_button_raw_pressed(void)
{
	unsigned int key = irq_lock();
	uint32_t saved_ctrl =
		io_qspi_hw->io[LINKR_DEBUGGER_BOOTSEL_QSPI_SS_INDEX].ctrl;
	bool pressed;

	io_qspi_hw->io[LINKR_DEBUGGER_BOOTSEL_QSPI_SS_INDEX].ctrl =
		(saved_ctrl & ~IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS) |
		(IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_VALUE_DISABLE
		 << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB);
	barrier_dsync_fence_full();

	for (volatile uint32_t i = 0U;
	     i < LINKR_DEBUGGER_BOOTSEL_SETTLE_CYCLES; ++i) {
		/* The flash CS pin needs a short all-RAM settling delay. */
	}

	pressed = (sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS) == 0U;

	io_qspi_hw->io[LINKR_DEBUGGER_BOOTSEL_QSPI_SS_INDEX].ctrl = saved_ctrl;
	barrier_dsync_fence_full();
	irq_unlock(key);

	return pressed;
}

static enum linkr_debugger_bootsel_button_sample
linkr_debugger_bootsel_button_sample(void)
{
	enum linkr_debugger_bootsel_button_sample sample =
		LINKR_DEBUGGER_BOOTSEL_SAMPLE_UNAVAILABLE;
	unsigned int key = irq_lock();

	/* Keep the NONE check and RAM sampler atomic against flash-owner threads. */
	if (linkr_debugger_flash_arbiter_owner() ==
	    LINKR_DEBUGGER_FLASH_OWNER_NONE) {
		sample = linkr_debugger_bootsel_button_raw_pressed() ?
			LINKR_DEBUGGER_BOOTSEL_SAMPLE_PRESSED :
			LINKR_DEBUGGER_BOOTSEL_SAMPLE_RELEASED;
	}

	irq_unlock(key);
	return sample;
}

static const char *linkr_debugger_bootsel_button_state_name(
	enum linkr_debugger_bootsel_button_state_id state)
{
	switch (state) {
	case LINKR_DEBUGGER_BOOTSEL_STATE_RELEASED:
		return "released";
	case LINKR_DEBUGGER_BOOTSEL_STATE_DEBOUNCE_PRESS:
		return "debounce-press";
	case LINKR_DEBUGGER_BOOTSEL_STATE_PRESSED:
		return "pressed";
	case LINKR_DEBUGGER_BOOTSEL_STATE_WAIT_RELEASE:
		return "wait-release";
	default:
		return "unknown";
	}
}

static void linkr_debugger_bootsel_button_thread(void *arg1, void *arg2,
						 void *arg3)
{
	const struct linkr_debugger_bootsel_button_config config = {
		.debounce_samples = LINKR_DEBUGGER_BOOTSEL_DEBOUNCE_SAMPLES,
		.long_press_ms = LINKR_DEBUGGER_BOOTSEL_LONG_PRESS_MS,
	};
	struct linkr_debugger_bootsel_button_state state;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	linkr_debugger_bootsel_button_state_init(&state);

	while (true) {
		enum linkr_debugger_bootsel_button_state_id previous = state.id;
		enum linkr_debugger_bootsel_button_action action;
		enum linkr_debugger_bootsel_button_sample sample;

		sample = linkr_debugger_bootsel_button_sample();
		action = linkr_debugger_bootsel_button_state_step(
			&state, &config, sample, k_uptime_get());
		if (state.id != previous) {
			LOG_INF("BOOTSEL button state: %s -> %s",
				linkr_debugger_bootsel_button_state_name(previous),
				linkr_debugger_bootsel_button_state_name(state.id));
		}
		if (action == LINKR_DEBUGGER_BOOTSEL_ACTION_REBOOT) {
			int ret = linkr_debugger_reboot_now();

			if (ret < 0) {
				LOG_ERR("BOOTSEL release reboot failed: %d", ret);
			}
		}

		k_sleep(K_MSEC(LINKR_DEBUGGER_BOOTSEL_SAMPLE_INTERVAL_MS));
	}
}

/*
 * Polling is deliberately thread-based, never timer-ISR-based. It cannot run
 * when the core is completely wedged; the existing hardware watchdog remains
 * the fallback for that failure mode.
 */
K_THREAD_DEFINE(linkr_debugger_bootsel_button_thread_id,
		LINKR_DEBUGGER_BOOTSEL_STACK_SIZE,
		linkr_debugger_bootsel_button_thread,
		NULL, NULL, NULL,
		LINKR_DEBUGGER_BOOTSEL_THREAD_PRIORITY, 0, 0);

#endif
