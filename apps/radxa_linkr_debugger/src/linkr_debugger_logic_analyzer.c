/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_logic_analyzer.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(BIT)
#define BIT(n) (1UL << (n))
#endif

#if !defined(LINKR_DEBUGGER_LA_HOST_TEST)
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(linkr_debugger_la, CONFIG_LINKR_DEBUGGER_LOG_LEVEL);
#endif

#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
#include <zephyr/device.h>

static const struct device *const la_pio_dev = DEVICE_DT_GET(DT_NODELABEL(pio2));
static const struct device *const la_dma_dev = DEVICE_DT_GET(DT_NODELABEL(dma));

#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <zephyr/dt-bindings/dma/rpi-pico-dma-rp2350.h>

#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/pio_instructions.h>
#endif

#define LINKR_DEBUGGER_LA_RAW_SAMPLE_BYTES sizeof(uint32_t)
#define LINKR_DEBUGGER_LA_DEFAULT_CLK_SYS_HZ 125000000U
#define LINKR_DEBUGGER_LA_MIN_DIV256 256U
#define LINKR_DEBUGGER_LA_MAX_DIV256 ((65535U * 256U) + 255U)
#define LINKR_DEBUGGER_LA_PIO_JMP_PIN_BITS 0x00c0U

static uint8_t la_active_pin_count(const struct linkr_debugger_la_config *config)
{
	return config->selected_pin_count > 0U ? config->selected_pin_count : config->pin_count;
}

static uint8_t la_pin_at(const struct linkr_debugger_la_config *config, uint8_t index)
{
	if (config->selected_pin_count > 0U) {
		return config->selected_pins[index];
	}

	return (uint8_t)(config->pin_base + index);
}

static bool la_pin_is_safe(uint8_t pin)
{
	return (pin >= 7U && pin <= 20U) || pin == 29U;
}

static uint32_t la_rate_from_clock(uint32_t requested_rate, uint32_t clk_sys_hz)
{
	uint64_t div256;

	if (requested_rate < LINKR_DEBUGGER_LA_MIN_SAMPLE_RATE_HZ ||
	    requested_rate > LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ || clk_sys_hz == 0U) {
		return 0U;
	}

	div256 = (((uint64_t)clk_sys_hz * 256ULL) + ((uint64_t)requested_rate / 2ULL)) /
		 requested_rate;
	if (div256 < LINKR_DEBUGGER_LA_MIN_DIV256) {
		div256 = LINKR_DEBUGGER_LA_MIN_DIV256;
	}
	if (div256 > LINKR_DEBUGGER_LA_MAX_DIV256) {
		return 0U;
	}

	return (uint32_t)(((uint64_t)clk_sys_hz * 256ULL) / div256);
}

uint32_t linkr_debugger_logic_analyzer_max_samples(uint8_t pin_count, uint32_t buffer_size)
{
	if (pin_count == 0U || pin_count > LINKR_DEBUGGER_LA_MAX_CHANNELS) {
		return 0U;
	}

	return buffer_size / LINKR_DEBUGGER_LA_RAW_SAMPLE_BYTES;
}

uint32_t linkr_debugger_logic_analyzer_actual_rate(uint32_t requested_rate)
{
#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
	return la_rate_from_clock(requested_rate, clock_get_hz(clk_sys));
#else
	return la_rate_from_clock(requested_rate, LINKR_DEBUGGER_LA_DEFAULT_CLK_SYS_HZ);
#endif
}

uint64_t linkr_debugger_logic_analyzer_sample_period_ps(uint32_t actual_rate_hz)
{
	if (actual_rate_hz == 0U) {
		return 0U;
	}

	return 1000000000000ULL / actual_rate_hz;
}

uint32_t linkr_debugger_logic_analyzer_dma_block_size(uint32_t sample_count)
{
	if (sample_count == 0U || sample_count > LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES) {
		return 0U;
	}

	return sample_count * LINKR_DEBUGGER_LA_RAW_SAMPLE_BYTES;
}

uint16_t linkr_debugger_logic_analyzer_compress_raw_sample(
	uint32_t raw, const struct linkr_debugger_la_config *config)
{
	uint16_t values = 0U;
	uint8_t active_pin_count;

	if (config == NULL) {
		return 0U;
	}

	active_pin_count = la_active_pin_count(config);
	for (uint8_t i = 0U; i < active_pin_count && i < LINKR_DEBUGGER_LA_MAX_CHANNELS; i++) {
		uint8_t pin = la_pin_at(config, i);

		if ((raw & BIT(pin)) != 0U) {
			values |= (uint16_t)BIT(i);
		}
	}

	return values;
}

int linkr_debugger_logic_analyzer_validate_config(
	const struct linkr_debugger_la_config *config, uint32_t capacity_samples)
{
	uint8_t active_pin_count;
	uint32_t total_samples;

	if (config == NULL) {
		return -EINVAL;
	}

	active_pin_count = la_active_pin_count(config);
	if (active_pin_count == 0U || active_pin_count > LINKR_DEBUGGER_LA_MAX_CHANNELS) {
		return -EINVAL;
	}
	if (config->selected_pin_count > LINKR_DEBUGGER_LA_MAX_CHANNELS) {
		return -EINVAL;
	}
	if (config->trigger > LINKR_DEBUGGER_LA_TRIGGER_EITHER) {
		return -EINVAL;
	}
	if (config->trigger != LINKR_DEBUGGER_LA_TRIGGER_NONE &&
	    config->trigger_pin >= active_pin_count) {
		return -EINVAL;
	}
	if (config->trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE && config->pre_samples > 0U) {
		return -EINVAL;
	}
	if (config->pre_samples > 0U &&
	    config->sample_rate_hz > LINKR_DEBUGGER_LA_MAX_STREAM_RATE_HZ) {
		return -EINVAL;
	}
	if (linkr_debugger_logic_analyzer_actual_rate(config->sample_rate_hz) == 0U) {
		return -EINVAL;
	}

	total_samples = config->pre_samples + config->post_samples;
	if (total_samples == 0U || total_samples > capacity_samples ||
	    total_samples > LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES) {
		return -EINVAL;
	}

	for (uint8_t i = 0U; i < active_pin_count; i++) {
		uint8_t pin = la_pin_at(config, i);

		if (!la_pin_is_safe(pin)) {
			return -EINVAL;
		}
		for (uint8_t j = 0U; j < i; j++) {
			if (pin == la_pin_at(config, j)) {
				return -EINVAL;
			}
		}
	}

	return 0;
}

const char *linkr_debugger_logic_analyzer_backend(void)
{
#if defined(CONFIG_SOC_SERIES_RP2350)
	return "rp2350-pio2-dma";
#else
	return "unsupported";
#endif
}

int linkr_debugger_logic_analyzer_build_either_trigger_program(
	uint8_t offset, uint16_t *instructions, size_t instruction_count)
{
	if (instructions == NULL || instruction_count < 5U || offset > 27U) {
		return -EINVAL;
	}

	instructions[0] = (uint16_t)(LINKR_DEBUGGER_LA_PIO_JMP_PIN_BITS | (uint16_t)(offset + 3U));
	instructions[2] = (uint16_t)(offset + 4U);
	return 0;
}

#if !defined(LINKR_DEBUGGER_LA_HOST_TEST)

static K_MUTEX_DEFINE(la_mutex);

static struct linkr_debugger_la_capture la_capture;
static bool la_initialized;

#if defined(CONFIG_SOC_SERIES_RP2350)
static struct linkr_debugger_la_sample la_samples[LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES];
static uint32_t la_raw_samples[LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES] __aligned(4);
static struct k_work la_finalize_work;
static struct dma_config la_dma_config;
static struct dma_block_config la_dma_block;
static size_t la_pio_sm;
static int la_pio_offset = -1;
static int la_dma_channel = -1;
static bool la_program_loaded;
static bool la_sm_claimed;
static bool la_capture_active;
static volatile int la_dma_status;
static volatile uint32_t la_generation;
static volatile uint32_t la_done_generation;
static uint8_t la_configured_pins[LINKR_DEBUGGER_LA_MAX_CHANNELS];
static uint8_t la_configured_pin_count;
static uint16_t la_program_instructions[5];
static struct pio_program la_program = {
	.instructions = la_program_instructions,
	.origin = -1,
};

static uint16_t la_stream_buf_a[LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES] __aligned(4);
static uint16_t la_stream_buf_b[LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES] __aligned(4);
static uint32_t la_stream_raw_a[LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES] __aligned(4);
static uint32_t la_stream_raw_b[LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES] __aligned(4);
static volatile bool la_stream_active;
static volatile bool la_stream_buf_a_ready;
static volatile bool la_stream_buf_b_ready;
static volatile bool la_stream_use_buf_a;
static volatile uint32_t la_stream_sequence;
static linkr_debugger_la_stream_callback_t la_stream_callback;
static void *la_stream_user_data;
static uint32_t la_stream_emit_div;
static uint32_t la_stream_block_index;

#define LINKR_DEBUGGER_LA_STREAM_EMIT_TARGET_HZ 200U
static struct linkr_debugger_la_config la_stream_config;
static struct k_work la_stream_work;
static struct dma_config la_stream_dma_config;
static struct dma_block_config la_stream_dma_block;

static uint16_t la_pre_trigger_ring[LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES];
static uint32_t la_pre_trigger_write_index;
static volatile bool la_pre_trigger_active;
static volatile bool la_pre_trigger_triggered;
static volatile uint32_t la_pre_trigger_post_remaining;
static uint8_t la_pre_trigger_prev_level;
static bool la_pre_trigger_have_prev;
static struct linkr_debugger_la_config la_pre_trigger_config;
static struct k_work la_pre_trigger_finalize_work;
static volatile uint32_t la_stream_irq_count;
static volatile uint32_t la_stream_chunk_count;
static volatile uint16_t la_stream_values_or;
static volatile uint16_t la_stream_values_and;

static int la_arm_pre_trigger_locked(const struct linkr_debugger_la_config *config);

static void la_stream_teardown_locked(void)
{
	la_stream_active = false;
	la_stream_callback = NULL;
	la_stream_user_data = NULL;
	la_pre_trigger_active = false;
	la_pre_trigger_triggered = false;
	la_pre_trigger_have_prev = false;
}

static uint32_t la_actual_rate_from_hw(uint32_t requested_rate)
{
	return linkr_debugger_logic_analyzer_actual_rate(requested_rate);
}

static void la_restore_configured_pins(void)
{
	const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));

	for (uint8_t i = 0U; i < la_configured_pin_count; i++) {
		uint8_t pin = la_configured_pins[i];

		gpio_set_function(pin, GPIO_FUNC_SIO);
		gpio_set_dir(pin, false);
		(void)gpio_pin_configure(gpio_dev, pin, GPIO_INPUT);
	}
	la_configured_pin_count = 0U;
}

static void la_cleanup_locked(void)
{
	PIO pio;

	if (!device_is_ready(la_pio_dev)) {
		return;
	}

	pio = pio_rpi_pico_get_pio(la_pio_dev);
	if (la_sm_claimed) {
		pio_sm_set_enabled(pio, (uint)la_pio_sm, false);
		pio_sm_clear_fifos(pio, (uint)la_pio_sm);
	}
	if (la_dma_channel >= 0 && device_is_ready(la_dma_dev)) {
		(void)dma_stop(la_dma_dev, (uint32_t)la_dma_channel);
		dma_release_channel(la_dma_dev, (uint32_t)la_dma_channel);
		la_dma_channel = -1;
	}
	if (la_program_loaded && la_pio_offset >= 0) {
		pio_remove_program(pio, &la_program, (uint)la_pio_offset);
		la_program_loaded = false;
		la_pio_offset = -1;
	}
	if (la_sm_claimed) {
		pio_sm_unclaim(pio, (uint)la_pio_sm);
		la_sm_claimed = false;
	}
	la_restore_configured_pins();
	la_capture_active = false;
}

static void la_finalize_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&la_mutex, K_FOREVER);
	if (!la_capture_active || la_done_generation != la_generation) {
		k_mutex_unlock(&la_mutex);
		return;
	}

	if (la_dma_status < 0) {
		la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
	} else {
		uint64_t period_ps = la_capture.sample_period_ps;

		for (uint32_t i = 0U; i < la_capture.sample_count; i++) {
			la_samples[i].timestamp_us = (uint32_t)((period_ps * i) / 1000000ULL);
			la_samples[i].values = linkr_debugger_logic_analyzer_compress_raw_sample(
				la_raw_samples[i], &la_capture.config);
			la_samples[i].reserved = 0U;
		}
		la_capture.samples = la_samples;
		la_capture.state = LINKR_DEBUGGER_LA_STATE_DONE;
	}

	la_cleanup_locked();
	k_mutex_unlock(&la_mutex);
}

static void la_dma_callback(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel);

	uint32_t done_generation = (uint32_t)(uintptr_t)user_data;

	if (done_generation != la_generation) {
		return;
	}

	la_done_generation = done_generation;
	la_dma_status = status;
	(void)k_work_submit(&la_finalize_work);
}

static uint8_t la_program_length_for_trigger(enum linkr_debugger_la_trigger_type trigger)
{
	if (trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE) {
		return 1U;
	}
	if (trigger == LINKR_DEBUGGER_LA_TRIGGER_EITHER) {
		return 5U;
	}

	return 3U;
}

static int la_find_program_offset(PIO pio, uint8_t length)
{
	la_program.length = length;
	for (uint8_t offset = 0U; offset <= 32U - length; offset++) {
		if (pio_can_add_program_at_offset(pio, &la_program, offset)) {
			return offset;
		}
	}

	return -EBUSY;
}

static void la_build_program(const struct linkr_debugger_la_config *config, uint8_t offset)
{
	uint8_t trigger_pin = la_pin_at(config, config->trigger_pin);

	if (config->trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE) {
		la_program_instructions[0] = (uint16_t)pio_encode_in(pio_pins, 32U);
		la_program.length = 1U;
		return;
	}

	if (config->trigger == LINKR_DEBUGGER_LA_TRIGGER_RISING) {
		la_program_instructions[0] = (uint16_t)pio_encode_wait_pin(false, trigger_pin);
		la_program_instructions[1] = (uint16_t)pio_encode_wait_pin(true, trigger_pin);
		la_program_instructions[2] = (uint16_t)pio_encode_in(pio_pins, 32U);
		la_program.length = 3U;
		return;
	}

	if (config->trigger == LINKR_DEBUGGER_LA_TRIGGER_FALLING) {
		la_program_instructions[0] = (uint16_t)pio_encode_wait_pin(true, trigger_pin);
		la_program_instructions[1] = (uint16_t)pio_encode_wait_pin(false, trigger_pin);
		la_program_instructions[2] = (uint16_t)pio_encode_in(pio_pins, 32U);
		la_program.length = 3U;
		return;
	}

	(void)linkr_debugger_logic_analyzer_build_either_trigger_program(
		offset, la_program_instructions, ARRAY_SIZE(la_program_instructions));
	la_program_instructions[1] = (uint16_t)pio_encode_wait_pin(true, trigger_pin);
	la_program_instructions[3] = (uint16_t)pio_encode_wait_pin(false, trigger_pin);
	la_program_instructions[4] = (uint16_t)pio_encode_in(pio_pins, 32U);
	la_program.length = 5U;
}

static int la_configure_pio_locked(const struct linkr_debugger_la_config *config)
{
	const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	uint8_t active_pin_count = la_active_pin_count(config);
	uint32_t actual_rate = la_actual_rate_from_hw(config->sample_rate_hz);
	uint64_t div256;
	pio_sm_config sm_config;
	PIO pio;
	int ret;

	if (!device_is_ready(gpio_dev) || !device_is_ready(la_pio_dev) || !device_is_ready(la_dma_dev)) {
		return -ENODEV;
	}

	pio = pio_rpi_pico_get_pio(la_pio_dev);
	ret = pio_rpi_pico_allocate_sm(la_pio_dev, &la_pio_sm);
	if (ret < 0) {
		return ret;
	}
	la_sm_claimed = true;

	la_pio_offset = la_find_program_offset(pio, la_program_length_for_trigger(config->trigger));
	if (la_pio_offset < 0) {
		return la_pio_offset;
	}
	la_build_program(config, (uint8_t)la_pio_offset);
	ret = pio_add_program_at_offset(pio, &la_program, (uint)la_pio_offset);
	if (ret < 0) {
		return ret;
	}
	la_pio_offset = ret;
	la_program_loaded = true;

	for (uint8_t i = 0U; i < active_pin_count; i++) {
		uint8_t pin = la_pin_at(config, i);

		ret = gpio_pin_configure(gpio_dev, pin, GPIO_INPUT);
		if (ret < 0) {
			return ret;
		}
		pio_gpio_init(pio, pin);
		pio_sm_set_consecutive_pindirs(pio, (uint)la_pio_sm, pin, 1U, false);
		la_configured_pins[la_configured_pin_count++] = pin;
	}

	div256 = (((uint64_t)clock_get_hz(clk_sys) * 256ULL) +
		 ((uint64_t)config->sample_rate_hz / 2ULL)) / config->sample_rate_hz;
	if (div256 < LINKR_DEBUGGER_LA_MIN_DIV256) {
		div256 = LINKR_DEBUGGER_LA_MIN_DIV256;
	}

	sm_config = pio_get_default_sm_config();
	sm_config_set_clkdiv_int_frac8(&sm_config, (uint32_t)(div256 / 256ULL),
		(uint8_t)(div256 % 256ULL));
	/* in_base=0 makes WAIT PIN and IN PINS use raw GPIO numbers; a 32-bit IN preserves
	 * GPIO N at raw sample bit N with right-shift autopush.
	 */
	sm_config_set_in_pins(&sm_config, 0U);
	sm_config_set_in_pin_count(&sm_config, 32U);
	sm_config_set_jmp_pin(&sm_config, la_pin_at(config, config->trigger_pin));
	sm_config_set_in_shift(&sm_config, true, true, 32U);
	sm_config_set_fifo_join(&sm_config, PIO_FIFO_JOIN_RX);
	sm_config_set_wrap(&sm_config, (uint)la_pio_offset + la_program.length - 1U,
		(uint)la_pio_offset + la_program.length - 1U);

	ret = pio_sm_init(pio, (uint)la_pio_sm, (uint)la_pio_offset, &sm_config);
	if (ret < 0) {
		return ret;
	}
	pio_sm_clear_fifos(pio, (uint)la_pio_sm);
	pio_sm_restart(pio, (uint)la_pio_sm);
	pio_sm_clkdiv_restart(pio, (uint)la_pio_sm);
	la_capture.actual_sample_rate_hz = actual_rate;
	la_capture.sample_period_ps = linkr_debugger_logic_analyzer_sample_period_ps(actual_rate);

	return 0;
}

static int la_configure_dma_locked(uint32_t sample_count)
{
	PIO pio = pio_rpi_pico_get_pio(la_pio_dev);
	uint32_t block_size = linkr_debugger_logic_analyzer_dma_block_size(sample_count);
	int ret;

	la_dma_channel = dma_request_channel(la_dma_dev, NULL);
	if (la_dma_channel < 0) {
		return la_dma_channel;
	}

	memset(&la_dma_config, 0, sizeof(la_dma_config));
	memset(&la_dma_block, 0, sizeof(la_dma_block));

	la_dma_block.source_address = (uint32_t)&pio->rxf[la_pio_sm];
	la_dma_block.dest_address = (uint32_t)la_raw_samples;
	la_dma_block.block_size = block_size;
	la_dma_block.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	la_dma_block.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

	la_dma_config.dma_slot = RPI_PICO_DMA_DREQ_TO_SLOT(pio_get_dreq(pio, (uint)la_pio_sm, false));
	la_dma_config.channel_direction = PERIPHERAL_TO_MEMORY;
	la_dma_config.source_data_size = LINKR_DEBUGGER_LA_RAW_SAMPLE_BYTES;
	la_dma_config.dest_data_size = LINKR_DEBUGGER_LA_RAW_SAMPLE_BYTES;
	la_dma_config.source_burst_length = 1U;
	la_dma_config.dest_burst_length = 1U;
	la_dma_config.block_count = 1U;
	la_dma_config.head_block = &la_dma_block;
	la_dma_config.user_data = (void *)(uintptr_t)la_generation;
	la_dma_config.dma_callback = la_dma_callback;
	la_dma_config.complete_callback_en = 1U;

	ret = dma_config(la_dma_dev, (uint32_t)la_dma_channel, &la_dma_config);
	if (ret < 0) {
		dma_release_channel(la_dma_dev, (uint32_t)la_dma_channel);
		la_dma_channel = -1;
		return ret;
	}

	ret = dma_start(la_dma_dev, (uint32_t)la_dma_channel);
	if (ret < 0) {
		dma_release_channel(la_dma_dev, (uint32_t)la_dma_channel);
		la_dma_channel = -1;
	}

	return ret;
}

#endif
#else

struct k_mutex { uint8_t unused; };

#define K_FOREVER 0
#define ARG_UNUSED(x) (void)(x)

static void k_mutex_lock(struct k_mutex *mutex, int timeout)
{
	ARG_UNUSED(mutex);
	ARG_UNUSED(timeout);
}

static void k_mutex_unlock(struct k_mutex *mutex)
{
	ARG_UNUSED(mutex);
}

static struct k_mutex la_mutex;
static struct linkr_debugger_la_capture la_capture;
static bool la_initialized;
static struct linkr_debugger_la_sample la_samples[LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES];

int linkr_debugger_logic_analyzer_host_set_capture(
	const struct linkr_debugger_la_capture *capture,
	const struct linkr_debugger_la_sample *samples,
	size_t sample_count)
{
	if (capture == NULL || (sample_count > 0U && samples == NULL) ||
	    sample_count > LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES) {
		return -EINVAL;
	}

	k_mutex_lock(&la_mutex, K_FOREVER);
	la_capture = *capture;
	la_capture.sample_count = (uint32_t)sample_count;
	la_capture.samples = sample_count > 0U ? la_samples : NULL;
	memcpy(la_samples, samples, sample_count * sizeof(la_samples[0]));
	k_mutex_unlock(&la_mutex);
	return 0;
}

int linkr_debugger_logic_analyzer_start_stream(
	const struct linkr_debugger_la_config *config,
	linkr_debugger_la_stream_callback_t callback,
	void *user_data)
{
	ARG_UNUSED(config);
	ARG_UNUSED(callback);
	ARG_UNUSED(user_data);
	return -ENOTSUP;
}

int linkr_debugger_logic_analyzer_stop_stream(void)
{
	return 0;
}

bool linkr_debugger_logic_analyzer_is_streaming(void)
{
	return false;
}

void linkr_debugger_logic_analyzer_get_debug(struct linkr_debugger_la_debug *out)
{
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
}

#endif

int linkr_debugger_logic_analyzer_init(void)
{
	k_mutex_lock(&la_mutex, K_FOREVER);
	if (la_initialized) {
		k_mutex_unlock(&la_mutex);
		return 0;
	}

	memset(&la_capture, 0, sizeof(la_capture));
	la_capture.state = LINKR_DEBUGGER_LA_STATE_IDLE;
	la_capture.backend = linkr_debugger_logic_analyzer_backend();
#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
	k_work_init(&la_finalize_work, la_finalize_work_handler);
#endif
	la_initialized = true;
	k_mutex_unlock(&la_mutex);
	return 0;
}

int linkr_debugger_logic_analyzer_arm(const struct linkr_debugger_la_config *config)
{
	int ret;

	ret = linkr_debugger_logic_analyzer_validate_config(config,
		LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES);
	if (ret < 0) {
		return ret;
	}
#if !defined(CONFIG_SOC_SERIES_RP2350) || defined(LINKR_DEBUGGER_LA_HOST_TEST)
	return -ENOTSUP;
#else
	struct linkr_debugger_la_config normalized;
	uint8_t active_pin_count;
	uint32_t total_samples;

	active_pin_count = la_active_pin_count(config);
	total_samples = config->pre_samples + config->post_samples;
	normalized = *config;
	normalized.pin_count = active_pin_count;
	if (normalized.selected_pin_count == 0U) {
		for (uint8_t i = 0U; i < active_pin_count; i++) {
			normalized.selected_pins[i] = (uint8_t)(config->pin_base + i);
		}
		normalized.selected_pin_count = active_pin_count;
	}

	if (normalized.trigger != LINKR_DEBUGGER_LA_TRIGGER_NONE && normalized.pre_samples > 0U) {
		return la_arm_pre_trigger_locked(&normalized);
	}

	k_mutex_lock(&la_mutex, K_FOREVER);
	if (la_capture.state != LINKR_DEBUGGER_LA_STATE_IDLE &&
	    la_capture.state != LINKR_DEBUGGER_LA_STATE_DONE &&
	    la_capture.state != LINKR_DEBUGGER_LA_STATE_ERROR) {
		k_mutex_unlock(&la_mutex);
		return -EBUSY;
	}

	la_stream_teardown_locked();
	la_cleanup_locked();
	la_generation++;
	memset(la_raw_samples, 0, sizeof(la_raw_samples));
	memset(la_samples, 0, sizeof(la_samples));
	la_capture.config = normalized;
	la_capture.sample_count = total_samples;
	la_capture.trigger_index = config->pre_samples;
	la_capture.requested_sample_rate_hz = config->sample_rate_hz;
	la_capture.actual_sample_rate_hz = la_actual_rate_from_hw(config->sample_rate_hz);
	la_capture.config.sample_rate_hz = la_capture.actual_sample_rate_hz;
	la_capture.sample_period_ps = linkr_debugger_logic_analyzer_sample_period_ps(
		la_capture.actual_sample_rate_hz);
	la_capture.backend = linkr_debugger_logic_analyzer_backend();
	la_capture.samples = NULL;
	la_dma_status = 0;

	ret = la_configure_pio_locked(&normalized);
	if (ret < 0) {
		la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
		la_cleanup_locked();
		k_mutex_unlock(&la_mutex);
		return ret;
	}

	ret = la_configure_dma_locked(total_samples);
	if (ret < 0) {
		la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
		la_cleanup_locked();
		k_mutex_unlock(&la_mutex);
		return ret;
	}

	la_capture.state = (normalized.trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE) ?
		LINKR_DEBUGGER_LA_STATE_CAPTURING : LINKR_DEBUGGER_LA_STATE_ARMED;
	la_capture_active = true;
	pio_sm_set_enabled(pio_rpi_pico_get_pio(la_pio_dev), (uint)la_pio_sm, true);
	k_mutex_unlock(&la_mutex);
	return 0;
#endif
}

int linkr_debugger_logic_analyzer_cancel(void)
{
	k_mutex_lock(&la_mutex, K_FOREVER);
#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
	la_generation++;
	la_stream_teardown_locked();
	la_cleanup_locked();
#endif
	la_capture.state = LINKR_DEBUGGER_LA_STATE_IDLE;
	la_capture.samples = NULL;
	k_mutex_unlock(&la_mutex);
	return 0;
}

#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)

static void la_pre_trigger_finalize_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&la_mutex, K_FOREVER);
	if (!la_pre_trigger_active) {
		k_mutex_unlock(&la_mutex);
		return;
	}

	uint32_t pre_count = la_pre_trigger_config.pre_samples;
	uint32_t post_count = la_pre_trigger_config.post_samples;
	uint32_t total = pre_count + post_count;
	uint32_t start = (la_pre_trigger_write_index +
		LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES - total) %
		LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES;

	for (uint32_t i = 0U; i < total; i++) {
		uint32_t src_index = (start + i) % LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES;

		la_samples[i].timestamp_us = (uint32_t)((la_capture.sample_period_ps * i) / 1000000ULL);
		la_samples[i].values = la_pre_trigger_ring[src_index];
		la_samples[i].reserved = 0U;
	}

	la_capture.samples = la_samples;
	la_capture.sample_count = total;
	la_capture.state = LINKR_DEBUGGER_LA_STATE_DONE;
	la_capture.trigger_index = pre_count;

	la_stream_teardown_locked();
	la_cleanup_locked();
	k_mutex_unlock(&la_mutex);
}

static void la_pre_trigger_stream_callback(
	const struct linkr_debugger_la_stream_chunk *chunk, void *user_data)
{
	ARG_UNUSED(user_data);

	if (!la_pre_trigger_active) {
		return;
	}

	/* Compressed samples: bit i mirrors selected_pins[i]. */
	const uint8_t trigger_bit = la_pre_trigger_config.trigger_pin;
	const uint32_t post_total = la_pre_trigger_config.post_samples;

	for (uint32_t i = 0U; i < chunk->sample_count; i++) {
		uint16_t sample = chunk->values[i];
		uint8_t level = (uint8_t)((sample >> trigger_bit) & 1U);

		la_pre_trigger_ring[la_pre_trigger_write_index] = sample;
		la_pre_trigger_write_index = (la_pre_trigger_write_index + 1U) %
			LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES;

		if (!la_pre_trigger_triggered) {
			bool edge = false;

			if (la_pre_trigger_have_prev) {
				switch (la_pre_trigger_config.trigger) {
				case LINKR_DEBUGGER_LA_TRIGGER_RISING:
					edge = la_pre_trigger_prev_level == 0U && level != 0U;
					break;
				case LINKR_DEBUGGER_LA_TRIGGER_FALLING:
					edge = la_pre_trigger_prev_level != 0U && level == 0U;
					break;
				case LINKR_DEBUGGER_LA_TRIGGER_EITHER:
					edge = la_pre_trigger_prev_level != level;
					break;
				default:
					break;
				}
			}
			la_pre_trigger_prev_level = level;
			la_pre_trigger_have_prev = true;
			if (edge) {
				la_pre_trigger_triggered = true;
				la_pre_trigger_post_remaining = post_total;
			}
		}

		if (la_pre_trigger_triggered) {
			if (la_pre_trigger_post_remaining > 0U) {
				la_pre_trigger_post_remaining--;
			}
			if (la_pre_trigger_post_remaining == 0U) {
				(void)k_work_submit(&la_pre_trigger_finalize_work);
				return;
			}
		}
	}
}

static int la_arm_pre_trigger_locked(const struct linkr_debugger_la_config *config)
{
	int ret;

	k_mutex_lock(&la_mutex, K_FOREVER);
	if (la_pre_trigger_active) {
		k_mutex_unlock(&la_mutex);
		return -EBUSY;
	}
	if (la_capture.state != LINKR_DEBUGGER_LA_STATE_IDLE &&
	    la_capture.state != LINKR_DEBUGGER_LA_STATE_DONE &&
	    la_capture.state != LINKR_DEBUGGER_LA_STATE_ERROR) {
		k_mutex_unlock(&la_mutex);
		return -EBUSY;
	}

	la_pre_trigger_triggered = false;
	la_pre_trigger_have_prev = false;
	la_pre_trigger_prev_level = 0U;
	la_pre_trigger_post_remaining = 0U;
	la_pre_trigger_write_index = 0U;
	la_pre_trigger_config = *config;
	memset(la_pre_trigger_ring, 0, sizeof(la_pre_trigger_ring));

	la_capture.config = *config;
	la_capture.sample_count = 0U;
	la_capture.trigger_index = config->pre_samples;
	la_capture.requested_sample_rate_hz = config->sample_rate_hz;
	la_capture.actual_sample_rate_hz = la_actual_rate_from_hw(config->sample_rate_hz);
	la_capture.config.sample_rate_hz = la_capture.actual_sample_rate_hz;
	la_capture.sample_period_ps = linkr_debugger_logic_analyzer_sample_period_ps(
		la_capture.actual_sample_rate_hz);
	la_capture.backend = linkr_debugger_logic_analyzer_backend();
	la_capture.samples = NULL;
	la_capture.state = LINKR_DEBUGGER_LA_STATE_IDLE;

	k_work_init(&la_pre_trigger_finalize_work, la_pre_trigger_finalize_handler);

	struct linkr_debugger_la_config stream_config = *config;
	stream_config.pre_samples = 0U;
	stream_config.post_samples = LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES;
	stream_config.trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;

	k_mutex_unlock(&la_mutex);

	ret = linkr_debugger_logic_analyzer_start_stream(&stream_config,
		la_pre_trigger_stream_callback, NULL);
	if (ret < 0) {
		k_mutex_lock(&la_mutex, K_FOREVER);
		la_stream_teardown_locked();
		k_mutex_unlock(&la_mutex);
		return ret;
	}

	k_mutex_lock(&la_mutex, K_FOREVER);
	la_pre_trigger_active = true;
	la_capture.state = LINKR_DEBUGGER_LA_STATE_ARMED;
	k_mutex_unlock(&la_mutex);
	return 0;
}

#endif

enum linkr_debugger_la_state linkr_debugger_logic_analyzer_get_state(void)
{
	enum linkr_debugger_la_state state;

	k_mutex_lock(&la_mutex, K_FOREVER);
	state = la_capture.state;
	k_mutex_unlock(&la_mutex);
	return state;
}

int linkr_debugger_logic_analyzer_get_capture(
	struct linkr_debugger_la_capture *capture,
	struct linkr_debugger_la_sample *samples,
	size_t sample_capacity)
{
	if (capture == NULL || samples == NULL || sample_capacity == 0U ||
	    sample_capacity > LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES) {
		return -EINVAL;
	}

	k_mutex_lock(&la_mutex, K_FOREVER);
	if (la_capture.state != LINKR_DEBUGGER_LA_STATE_DONE) {
		k_mutex_unlock(&la_mutex);
		return -ENODATA;
	}
	if (la_capture.samples == NULL || la_capture.sample_count > sample_capacity) {
		k_mutex_unlock(&la_mutex);
		return -ENOSPC;
	}
	*capture = la_capture;
	memcpy(samples, la_capture.samples, la_capture.sample_count * sizeof(samples[0]));
	capture->samples = samples;
	k_mutex_unlock(&la_mutex);
	return 0;
}

void linkr_debugger_logic_analyzer_release(void)
{
	k_mutex_lock(&la_mutex, K_FOREVER);
#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
	la_generation++;
	la_stream_teardown_locked();
	la_cleanup_locked();
#endif
	la_capture.state = LINKR_DEBUGGER_LA_STATE_IDLE;
	la_capture.samples = NULL;
	k_mutex_unlock(&la_mutex);
}

#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)

static void la_stream_compress_and_emit(uint32_t *raw_buf, uint16_t *out_buf,
					 uint32_t count, uint64_t base_timestamp)
{
	struct linkr_debugger_la_stream_chunk chunk;

	uint16_t or_acc = 0U;
	uint16_t and_acc = 0xffffU;

	for (uint32_t i = 0U; i < count; i++) {
		out_buf[i] = linkr_debugger_logic_analyzer_compress_raw_sample(
			raw_buf[i], &la_stream_config);
		or_acc |= out_buf[i];
		and_acc &= out_buf[i];
	}
	la_stream_values_or |= or_acc;
	la_stream_values_and &= and_acc;

	chunk.sequence = la_stream_sequence++;
	chunk.sample_count = count;
	chunk.timestamp_us = base_timestamp;
	chunk.values = out_buf;
	la_stream_chunk_count++;

	if (la_stream_callback != NULL) {
		la_stream_callback(&chunk, la_stream_user_data);
	}
}

static void la_stream_process_block(uint32_t *raw_buf, uint16_t *out_buf)
{
	uint32_t block_index = la_stream_block_index++;

	/* The WS delivery budget is fixed; at multi-MHz rates formatting and
	 * compressing every block would saturate the system workqueue, so most
	 * blocks are dropped here before doing any expensive work.
	 */
	if (la_stream_emit_div > 1U && (block_index % la_stream_emit_div) != 0U) {
		return;
	}

	la_stream_compress_and_emit(raw_buf, out_buf,
		LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES,
		(uint64_t)block_index * LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES);
}

static void la_stream_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!la_stream_active) {
		return;
	}

	if (la_stream_buf_a_ready) {
		la_stream_buf_a_ready = false;
		la_stream_process_block(la_stream_raw_a, la_stream_buf_a);
	}

	if (la_stream_buf_b_ready) {
		la_stream_buf_b_ready = false;
		la_stream_process_block(la_stream_raw_b, la_stream_buf_b);
	}
}

static void la_stream_dma_callback(const struct device *dev, void *user_data,
				   uint32_t channel, int status)
{
	uint32_t *next_dest;

	ARG_UNUSED(user_data);

	if (!la_stream_active || status < 0) {
		return;
	}

	la_stream_irq_count++;

	if (la_stream_use_buf_a) {
		la_stream_buf_a_ready = true;
	} else {
		la_stream_buf_b_ready = true;
	}
	la_stream_use_buf_a = !la_stream_use_buf_a;

	/* The rpi_pico DMA driver stops after each block and disables the
	 * channel IRQ before invoking this callback, so the next block must be
	 * re-armed into the idle half-buffer to keep the ping-pong stream
	 * running.
	 */
	next_dest = la_stream_use_buf_a ? la_stream_raw_a : la_stream_raw_b;
	la_stream_dma_block.dest_address = (uint32_t)next_dest;
	if (dma_config(dev, channel, &la_stream_dma_config) < 0 ||
	    dma_start(dev, channel) < 0) {
		la_stream_active = false;
		la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
		return;
	}

	(void)k_work_submit(&la_stream_work);
}

static int la_start_stream_dma_locked(void)
{
	PIO pio = pio_rpi_pico_get_pio(la_pio_dev);
	uint32_t *dest = la_stream_use_buf_a ? la_stream_raw_a : la_stream_raw_b;
	uint32_t block_size = LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES * sizeof(uint32_t);
	int ret;

	la_dma_channel = dma_request_channel(la_dma_dev, NULL);
	if (la_dma_channel < 0) {
		return la_dma_channel;
	}

	memset(&la_stream_dma_config, 0, sizeof(la_stream_dma_config));
	memset(&la_stream_dma_block, 0, sizeof(la_stream_dma_block));

	la_stream_dma_block.source_address = (uint32_t)&pio->rxf[la_pio_sm];
	la_stream_dma_block.dest_address = (uint32_t)dest;
	la_stream_dma_block.block_size = block_size;
	la_stream_dma_block.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	la_stream_dma_block.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

	la_stream_dma_config.dma_slot = RPI_PICO_DMA_DREQ_TO_SLOT(
		pio_get_dreq(pio, (uint)la_pio_sm, false));
	la_stream_dma_config.channel_direction = PERIPHERAL_TO_MEMORY;
	la_stream_dma_config.source_data_size = LINKR_DEBUGGER_LA_RAW_SAMPLE_BYTES;
	la_stream_dma_config.dest_data_size = LINKR_DEBUGGER_LA_RAW_SAMPLE_BYTES;
	la_stream_dma_config.source_burst_length = 1U;
	la_stream_dma_config.dest_burst_length = 1U;
	la_stream_dma_config.block_count = 1U;
	la_stream_dma_config.head_block = &la_stream_dma_block;
	la_stream_dma_config.user_data = NULL;
	la_stream_dma_config.dma_callback = la_stream_dma_callback;
	la_stream_dma_config.complete_callback_en = 1U;

	ret = dma_config(la_dma_dev, (uint32_t)la_dma_channel, &la_stream_dma_config);
	if (ret < 0) {
		dma_release_channel(la_dma_dev, (uint32_t)la_dma_channel);
		la_dma_channel = -1;
		return ret;
	}

	ret = dma_start(la_dma_dev, (uint32_t)la_dma_channel);
	if (ret < 0) {
		dma_release_channel(la_dma_dev, (uint32_t)la_dma_channel);
		la_dma_channel = -1;
	}

	return ret;
}

int linkr_debugger_logic_analyzer_start_stream(
	const struct linkr_debugger_la_config *config,
	linkr_debugger_la_stream_callback_t callback,
	void *user_data)
{
	int ret;

	if (config == NULL || callback == NULL) {
		return -EINVAL;
	}

	if (config->trigger != LINKR_DEBUGGER_LA_TRIGGER_NONE) {
		return -EINVAL;
	}

	if (config->sample_rate_hz > LINKR_DEBUGGER_LA_MAX_STREAM_RATE_HZ) {
		return -EINVAL;
	}

	ret = linkr_debugger_logic_analyzer_validate_config(config,
		LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&la_mutex, K_FOREVER);
	if (la_stream_active) {
		k_mutex_unlock(&la_mutex);
		return -EBUSY;
	}
	if (la_capture.state != LINKR_DEBUGGER_LA_STATE_IDLE &&
	    la_capture.state != LINKR_DEBUGGER_LA_STATE_DONE &&
	    la_capture.state != LINKR_DEBUGGER_LA_STATE_ERROR) {
		k_mutex_unlock(&la_mutex);
		return -EBUSY;
	}

	la_stream_teardown_locked();
	la_cleanup_locked();
	la_generation++;
	la_stream_config = *config;
	la_stream_config.pin_count = la_active_pin_count(config);
	if (la_stream_config.selected_pin_count == 0U) {
		for (uint8_t i = 0U; i < la_stream_config.pin_count; i++) {
			la_stream_config.selected_pins[i] = (uint8_t)(config->pin_base + i);
		}
		la_stream_config.selected_pin_count = la_stream_config.pin_count;
	}

	la_stream_callback = callback;
	la_stream_user_data = user_data;
	la_stream_sequence = 0U;
	la_stream_block_index = 0U;
	la_stream_emit_div = 1U;
	{
		uint32_t block_rate = config->sample_rate_hz / LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES;

		if (block_rate > LINKR_DEBUGGER_LA_STREAM_EMIT_TARGET_HZ) {
			la_stream_emit_div = DIV_ROUND_UP(block_rate,
				LINKR_DEBUGGER_LA_STREAM_EMIT_TARGET_HZ);
		}
	}
	la_stream_use_buf_a = true;
	la_stream_buf_a_ready = false;
	la_stream_buf_b_ready = false;
	la_stream_values_or = 0U;
	la_stream_values_and = 0xffffU;
	memset(la_stream_raw_a, 0, sizeof(la_stream_raw_a));
	memset(la_stream_raw_b, 0, sizeof(la_stream_raw_b));

	ret = la_configure_pio_locked(&la_stream_config);
	if (ret < 0) {
		k_mutex_unlock(&la_mutex);
		return ret;
	}

	k_work_init(&la_stream_work, la_stream_work_handler);

	ret = la_start_stream_dma_locked();
	if (ret < 0) {
		la_cleanup_locked();
		k_mutex_unlock(&la_mutex);
		return ret;
	}

	la_stream_active = true;
	la_capture.state = LINKR_DEBUGGER_LA_STATE_STREAMING;
	la_capture.requested_sample_rate_hz = config->sample_rate_hz;
	la_capture.actual_sample_rate_hz = la_actual_rate_from_hw(config->sample_rate_hz);
	la_capture.sample_period_ps = linkr_debugger_logic_analyzer_sample_period_ps(
		la_capture.actual_sample_rate_hz);
	la_capture.backend = linkr_debugger_logic_analyzer_backend();

	pio_sm_set_enabled(pio_rpi_pico_get_pio(la_pio_dev), (uint)la_pio_sm, true);
	k_mutex_unlock(&la_mutex);
	return 0;
}

int linkr_debugger_logic_analyzer_stop_stream(void)
{
	k_mutex_lock(&la_mutex, K_FOREVER);
	la_stream_teardown_locked();
	la_generation++;
	la_cleanup_locked();
	la_capture.state = LINKR_DEBUGGER_LA_STATE_IDLE;
	k_mutex_unlock(&la_mutex);
	return 0;
}

bool linkr_debugger_logic_analyzer_is_streaming(void)
{
	return la_stream_active;
}

void linkr_debugger_logic_analyzer_get_debug(struct linkr_debugger_la_debug *out)
{
	if (out == NULL) {
		return;
	}

	k_mutex_lock(&la_mutex, K_FOREVER);
	out->stream_irqs = la_stream_irq_count;
	out->stream_chunks = la_stream_chunk_count;
	out->pre_write_index = la_pre_trigger_write_index;
	out->pre_post_remaining = la_pre_trigger_post_remaining;
	out->stream_values_or = la_stream_values_or;
	out->stream_values_and = la_stream_values_and;
	out->pre_active = la_pre_trigger_active;
	out->pre_triggered = la_pre_trigger_triggered;
	out->stream_active = la_stream_active;
	k_mutex_unlock(&la_mutex);
}

#endif
