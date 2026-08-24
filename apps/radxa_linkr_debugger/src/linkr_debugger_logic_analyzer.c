/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_logic_analyzer.h"

#include "linkr_debugger_capture_arena.h"

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
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(linkr_debugger_la, CONFIG_LINKR_DEBUGGER_LOG_LEVEL);
#endif

#if !defined(K_PRIO_PREEMPT)
#define K_PRIO_PREEMPT(prio) (prio)
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
#include <hardware/dma.h>
#endif

#define LINKR_DEBUGGER_LA_RAW_SAMPLE_BYTES sizeof(uint32_t)
#define LINKR_DEBUGGER_LA_DEFAULT_CLK_SYS_HZ 125000000U
#define LINKR_DEBUGGER_LA_MIN_DIV256 256U
#define LINKR_DEBUGGER_LA_MAX_DIV256 ((65535U * 256U) + 255U)
#define LINKR_DEBUGGER_LA_PIO_IN_PINS_1 0x4001U
#define LINKR_DEBUGGER_LA_PIO_IN_PINS_11 0x400bU
#define LINKR_DEBUGGER_LA_PIO_IN_PINS_32 0x4000U
#define LINKR_DEBUGGER_LA_PIO_WAIT_IRQ0 0x20c0U
#define LINKR_DEBUGGER_LA_STREAM_WAIT_PRODUCER BIT(0)
#define LINKR_DEBUGGER_LA_STREAM_WAIT_CONSUMER BIT(1)
#define LINKR_DEBUGGER_LA_DMA_ABORT_TIMEOUT_US 1000U
#define LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE 2048U
/* Must outrank CONFIG_NET_TCP_WORKER_PRIO=2: at 1 MHz the usable ring window
 * is 7.168 ms. The producer only polls DMA/trigger state and wakes the lower
 * priority consumer; websocket_send_msg is not called from it.
 */
#define LINKR_DEBUGGER_LA_STREAM_THREAD_PRIORITY K_PRIO_PREEMPT(1)
#define LINKR_DEBUGGER_LA_STREAM_CONSUMER_SINK_PRIORITY K_PRIO_PREEMPT(7)
#define LINKR_DEBUGGER_LA_STREAM_CONSUMER_PRIORITY K_PRIO_PREEMPT(8)
#define LINKR_DEBUGGER_LA_WIDE11_BURST_CHUNK_SAMPLES 1024U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_DMA_A_DONE BIT(0)
#define LINKR_DEBUGGER_LA_WIDE11_BURST_DMA_B_DONE BIT(1)

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
	return pin >= 7U && pin <= 20U;
}

static bool la_is_packed_physical_plan_supported(
	const struct linkr_debugger_la_config *config);
static bool la_is_high_rate_packed_burst_request(
	const struct linkr_debugger_la_config *config);

static bool la_is_single_plan(const struct linkr_debugger_la_config *config)
{
	return config != NULL && la_active_pin_count(config) == 1U;
}

static bool la_is_fast8_plan(const struct linkr_debugger_la_config *config)
{
	uint8_t active_pin_count;

	if (config == NULL) {
		return false;
	}
	active_pin_count = la_active_pin_count(config);
	if (active_pin_count == 0U || active_pin_count > 8U) {
		return false;
	}
	for (uint8_t i = 0U; i < active_pin_count; i++) {
		uint8_t pin = la_pin_at(config, i);

		if (pin < 10U || pin > 17U) {
			return false;
		}
	}
	return true;
}

static bool la_is_wide11_plan(const struct linkr_debugger_la_config *config)
{
	uint8_t active_pin_count;

	if (config == NULL) {
		return false;
	}
	active_pin_count = la_active_pin_count(config);
	if (active_pin_count == 0U || active_pin_count > 11U) {
		return false;
	}
	for (uint8_t i = 0U; i < active_pin_count; i++) {
		uint8_t pin = la_pin_at(config, i);

		if (!(pin >= 10U && pin <= 20U)) {
			return false;
		}
	}
	return true;
}

static bool la_pins_are_valid_unique_safe(const struct linkr_debugger_la_config *config)
{
	uint8_t active_pin_count;

	if (config == NULL) {
		return false;
	}
	active_pin_count = la_active_pin_count(config);
	if (active_pin_count == 0U || active_pin_count > LINKR_DEBUGGER_LA_MAX_CHANNELS ||
	    config->selected_pin_count > LINKR_DEBUGGER_LA_MAX_CHANNELS) {
		return false;
	}
	for (uint8_t i = 0U; i < active_pin_count; i++) {
		uint8_t pin = la_pin_at(config, i);

		if (!la_pin_is_safe(pin)) {
			return false;
		}
		for (uint8_t j = 0U; j < i; j++) {
			if (pin == la_pin_at(config, j)) {
				return false;
			}
		}
	}
	return true;
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

int linkr_debugger_logic_analyzer_bounded_sample_target(
	uint32_t pre_samples,
	uint32_t post_samples,
	uint32_t *target_samples)
{
	if (target_samples == NULL || pre_samples > UINT32_MAX - post_samples) {
		return -EOVERFLOW;
	}

	*target_samples = pre_samples + post_samples;
	return *target_samples == 0U ? -EINVAL : 0;
}

bool linkr_debugger_logic_analyzer_pre_trigger_supported(
	enum linkr_debugger_la_trigger_type trigger,
	uint32_t requested_rate_hz,
	uint32_t pre_samples,
	uint32_t post_samples)
{
	uint32_t total_samples;

	return trigger >= LINKR_DEBUGGER_LA_TRIGGER_RISING &&
		trigger <= LINKR_DEBUGGER_LA_TRIGGER_EITHER &&
		requested_rate_hz >= LINKR_DEBUGGER_LA_MIN_PRE_TRIGGER_SAMPLE_RATE_HZ &&
		requested_rate_hz <= LINKR_DEBUGGER_LA_MAX_PRE_TRIGGER_SAMPLE_RATE_HZ &&
		pre_samples > 0U && post_samples > 0U &&
		linkr_debugger_logic_analyzer_bounded_sample_target(pre_samples, post_samples,
			&total_samples) == 0 &&
		total_samples <= LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES;
}

bool linkr_debugger_logic_analyzer_packed_rate_limit_supported(
	uint32_t requested_rate_hz,
	uint32_t actual_rate_hz,
	uint32_t requested_limit_hz)
{
	return actual_rate_hz != 0U && requested_rate_hz <= requested_limit_hz;
}

uint64_t linkr_debugger_logic_analyzer_sample_period_ps(uint32_t actual_rate_hz)
{
	if (actual_rate_hz == 0U) {
		return 0U;
	}

	return 1000000000000ULL / actual_rate_hz;
}

uint32_t linkr_debugger_logic_analyzer_ring_delta_samples(
	uint32_t last_hw_index, uint32_t hw_index, uint32_t ring_samples)
{
	if (ring_samples == 0U || last_hw_index >= ring_samples || hw_index >= ring_samples) {
		return 0U;
	}

	if (hw_index >= last_hw_index) {
		return hw_index - last_hw_index;
	}

	return (ring_samples - last_hw_index) + hw_index;
}

bool linkr_debugger_logic_analyzer_ring_window_may_overrun(
	uint64_t elapsed_us, uint32_t actual_rate_hz,
	uint32_t ring_samples, uint32_t safety_margin)
{
	uint32_t usable_samples;
	uint64_t elapsed_samples;

	if (actual_rate_hz == 0U || ring_samples == 0U || safety_margin >= ring_samples) {
		return true;
	}

	usable_samples = ring_samples - safety_margin;
	elapsed_samples = (elapsed_us * actual_rate_hz + 999999ULL) / 1000000ULL;
	return elapsed_samples >= usable_samples;
}

bool linkr_debugger_logic_analyzer_ring_seq_overran(
	uint64_t writer_seq, uint64_t reader_seq,
	uint32_t ring_samples, uint32_t safety_margin)
{
	if (ring_samples == 0U || safety_margin >= ring_samples || writer_seq < reader_seq) {
		return true;
	}

	return (writer_seq - reader_seq) > (uint64_t)(ring_samples - safety_margin);
}

int linkr_debugger_logic_analyzer_pre_trigger_minimum_retention_samples(
	uint32_t actual_rate_hz,
	uint32_t *required_samples)
{
	uint64_t samples_per_minimum_poll;
	uint64_t retention_samples;

	if (actual_rate_hz == 0U || required_samples == NULL) {
		return -EINVAL;
	}
	samples_per_minimum_poll = ((uint64_t)actual_rate_hz + 999ULL) / 1000ULL;
	retention_samples = 2ULL * samples_per_minimum_poll;
	if (retention_samples > UINT32_MAX) {
		return -EOVERFLOW;
	}

	*required_samples = (uint32_t)retention_samples;
	return 0;
}

bool linkr_debugger_logic_analyzer_pre_trigger_plan_feasible(
	const struct linkr_debugger_la_packed_ring_plan *plan,
	uint32_t actual_rate_hz)
{
	uint32_t required_samples;

	return plan != NULL && plan->sample_capacity > plan->safety_samples &&
		plan->usable_sample_capacity == plan->sample_capacity - plan->safety_samples &&
		linkr_debugger_logic_analyzer_pre_trigger_minimum_retention_samples(
			actual_rate_hz, &required_samples) == 0 &&
		plan->usable_sample_capacity >= required_samples;
}

uint64_t linkr_debugger_logic_analyzer_pre_trigger_scan_start(
	uint64_t writer_sequence,
	uint32_t pre_samples)
{
	uint64_t retained_start = writer_sequence > pre_samples ?
		writer_sequence - pre_samples : 0U;

	return retained_start > pre_samples ? retained_start : pre_samples;
}

bool linkr_debugger_logic_analyzer_pre_trigger_edge_matches(
	enum linkr_debugger_la_trigger_type trigger,
	uint8_t previous_level,
	uint8_t current_level)
{
	switch (trigger) {
	case LINKR_DEBUGGER_LA_TRIGGER_RISING:
		return previous_level == 0U && current_level != 0U;
	case LINKR_DEBUGGER_LA_TRIGGER_FALLING:
		return previous_level != 0U && current_level == 0U;
	case LINKR_DEBUGGER_LA_TRIGGER_EITHER:
		return previous_level != current_level;
	default:
		return false;
	}
}

bool linkr_debugger_logic_analyzer_pre_trigger_scan_source_overrun(
	uint64_t writer_sequence,
	uint64_t first_sequence,
	uint32_t ring_samples,
	uint32_t safety_margin)
{
	return linkr_debugger_logic_analyzer_ring_seq_overran(writer_sequence,
		first_sequence, ring_samples, safety_margin);
}

int linkr_debugger_logic_analyzer_pre_trigger_window(
	const struct linkr_debugger_la_config *config,
	uint64_t writer_sequence,
	uint64_t trigger_sequence,
	uint32_t ring_samples,
	uint32_t safety_margin,
	struct linkr_debugger_la_pre_trigger_window *window)
{
	uint32_t target_samples;
	uint64_t first_sequence;
	uint64_t end_sequence;

	if (config == NULL || window == NULL ||
	    !linkr_debugger_logic_analyzer_pre_trigger_supported(config->trigger,
		config->sample_rate_hz, config->pre_samples, config->post_samples) ||
	    ring_samples == 0U || safety_margin >= ring_samples) {
		return -EINVAL;
	}
	if (trigger_sequence < config->pre_samples || trigger_sequence >= writer_sequence) {
		return -ENODATA;
	}
	if (trigger_sequence > UINT64_MAX - config->post_samples ||
	    linkr_debugger_logic_analyzer_bounded_sample_target(config->pre_samples,
		config->post_samples, &target_samples) < 0) {
		return -EOVERFLOW;
	}

	first_sequence = trigger_sequence - config->pre_samples;
	end_sequence = trigger_sequence + config->post_samples;
	if (linkr_debugger_logic_analyzer_ring_seq_overran(writer_sequence, first_sequence,
	    ring_samples, safety_margin)) {
		return -EOVERFLOW;
	}

	window->first_sequence = first_sequence;
	window->end_sequence = end_sequence;
	window->sample_count = target_samples;
	window->trigger_index = config->pre_samples;
	return 0;
}

bool linkr_debugger_logic_analyzer_ring_should_freeze_before_overwrite(
	uint64_t writer_seq, uint64_t reader_seq, uint32_t produced_samples,
	uint32_t ring_samples, uint32_t safety_margin,
	uint32_t *retained_samples)
{
	uint64_t available_after;
	uint32_t usable_samples;

	if (retained_samples != NULL) {
		*retained_samples = 0U;
	}
	if (ring_samples == 0U || safety_margin >= ring_samples ||
	    reader_seq > writer_seq + produced_samples) {
		return true;
	}

	available_after = (writer_seq + produced_samples) - reader_seq;
	if (retained_samples != NULL) {
		*retained_samples = available_after > ring_samples ? ring_samples :
			(uint32_t)available_after;
	}
	usable_samples = ring_samples - safety_margin;
	return available_after > usable_samples;
}

uint32_t linkr_debugger_logic_analyzer_ring_poll_interval_ms(
	uint32_t actual_rate_hz, uint32_t ring_samples, uint32_t safety_margin)
{
	uint32_t usable_samples;
	uint64_t quarter_usable_us;
	uint32_t interval_ms;

	if (actual_rate_hz == 0U || ring_samples == 0U || safety_margin >= ring_samples) {
		return LINKR_DEBUGGER_LA_RING_POLL_INTERVAL_MIN_MS;
	}

	usable_samples = ring_samples - safety_margin;
	quarter_usable_us = ((uint64_t)usable_samples * 1000000ULL) /
		((uint64_t)actual_rate_hz * 4ULL);
	interval_ms = (uint32_t)(quarter_usable_us / 1000ULL);

	if (interval_ms < LINKR_DEBUGGER_LA_RING_POLL_INTERVAL_MIN_MS) {
		return LINKR_DEBUGGER_LA_RING_POLL_INTERVAL_MIN_MS;
	}
	if (interval_ms > LINKR_DEBUGGER_LA_RING_POLL_INTERVAL_MAX_MS) {
		return LINKR_DEBUGGER_LA_RING_POLL_INTERVAL_MAX_MS;
	}

	return interval_ms;
}

enum linkr_debugger_la_ring_poll_result linkr_debugger_logic_analyzer_ring_observe(
	struct linkr_debugger_la_ring_progress *progress,
	uint32_t hw_index,
	uint64_t now_us,
	uint32_t actual_rate_hz,
	uint32_t consumed_samples,
	uint32_t ring_samples,
	uint32_t safety_margin,
	uint32_t *produced_samples)
{
	uint32_t produced = 0U;

	if (produced_samples != NULL) {
		*produced_samples = 0U;
	}
	if (progress == NULL || ring_samples == 0U || safety_margin >= ring_samples ||
	    hw_index >= ring_samples) {
		return LINKR_DEBUGGER_LA_RING_POLL_POSSIBLE_OVERRUN;
	}

	if (!progress->initialized) {
		progress->last_hw_index = hw_index;
		progress->last_poll_time_us = now_us;
		progress->initialized = true;
		if (consumed_samples > 0U) {
			progress->reader_seq += consumed_samples;
		}
		return LINKR_DEBUGGER_LA_RING_POLL_OK;
	}

	if (now_us < progress->last_poll_time_us ||
	    linkr_debugger_logic_analyzer_ring_window_may_overrun(
		now_us - progress->last_poll_time_us, actual_rate_hz,
		ring_samples, safety_margin)) {
		return LINKR_DEBUGGER_LA_RING_POLL_POSSIBLE_OVERRUN;
	}

	produced = linkr_debugger_logic_analyzer_ring_delta_samples(
		progress->last_hw_index, hw_index, ring_samples);
	progress->last_hw_index = hw_index;
	progress->last_poll_time_us = now_us;
	progress->writer_seq += produced;
	progress->reader_seq += consumed_samples;
	if (produced_samples != NULL) {
		*produced_samples = produced;
	}

	if (linkr_debugger_logic_analyzer_ring_should_freeze_before_overwrite(
	    progress->writer_seq - produced, progress->reader_seq, produced,
	    ring_samples, safety_margin, NULL)) {
		return LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN;
	}

	return LINKR_DEBUGGER_LA_RING_POLL_OK;
}

enum linkr_debugger_la_ring_poll_result linkr_debugger_logic_analyzer_packed_ring_observe(
	struct linkr_debugger_la_ring_progress *progress,
	uint32_t lane_last_hw_indices[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES],
	uint64_t lane_writer_seqs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES],
	const uint32_t hw_word_indices[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES],
	uint64_t now_us,
	uint32_t actual_rate_hz,
	uint32_t consumed_samples,
	const struct linkr_debugger_la_packed_ring_plan *plan,
	uint32_t *produced_samples,
	uint32_t *skew_samples)
{
	uint64_t prev_writer_seq;
	uint64_t min_writer_seq = UINT64_MAX;
	uint64_t max_writer_seq = 0U;

	if (produced_samples != NULL) {
		*produced_samples = 0U;
	}
	if (skew_samples != NULL) {
		*skew_samples = 0U;
	}
	if (progress == NULL || lane_last_hw_indices == NULL || lane_writer_seqs == NULL ||
	    hw_word_indices == NULL || plan == NULL || plan->lane_count == 0U ||
	    plan->lane_count > LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES ||
	    plan->sample_capacity == 0U || plan->usable_sample_capacity == 0U ||
	    plan->safety_samples >= plan->sample_capacity) {
		return LINKR_DEBUGGER_LA_RING_POLL_POSSIBLE_OVERRUN;
	}

	for (uint8_t lane = 0U; lane < plan->lane_count; lane++) {
		if (plan->lanes[lane].word_count == 0U ||
		    hw_word_indices[lane] >= plan->lanes[lane].word_count) {
			return LINKR_DEBUGGER_LA_RING_POLL_POSSIBLE_OVERRUN;
		}
	}

	if (!progress->initialized) {
		for (uint8_t lane = 0U; lane < plan->lane_count; lane++) {
			lane_last_hw_indices[lane] = hw_word_indices[lane];
			lane_writer_seqs[lane] = 0U;
		}
		progress->last_poll_time_us = now_us;
		progress->initialized = true;
		if (consumed_samples > 0U) {
			progress->reader_seq += consumed_samples;
		}
		return LINKR_DEBUGGER_LA_RING_POLL_OK;
	}

	if (now_us < progress->last_poll_time_us ||
	    linkr_debugger_logic_analyzer_ring_window_may_overrun(
		    now_us - progress->last_poll_time_us, actual_rate_hz,
		    plan->sample_capacity, plan->safety_samples)) {
		return LINKR_DEBUGGER_LA_RING_POLL_POSSIBLE_OVERRUN;
	}

	prev_writer_seq = progress->writer_seq;
	for (uint8_t lane = 0U; lane < plan->lane_count; lane++) {
		uint32_t delta_words = linkr_debugger_logic_analyzer_ring_delta_samples(
			lane_last_hw_indices[lane], hw_word_indices[lane], plan->lanes[lane].word_count);

		lane_last_hw_indices[lane] = hw_word_indices[lane];
		lane_writer_seqs[lane] +=
			(uint64_t)delta_words * plan->lanes[lane].samples_per_word;
		if (lane_writer_seqs[lane] < min_writer_seq) {
			min_writer_seq = lane_writer_seqs[lane];
		}
		if (lane_writer_seqs[lane] > max_writer_seq) {
			max_writer_seq = lane_writer_seqs[lane];
		}
	}

	progress->last_poll_time_us = now_us;
	progress->writer_seq = min_writer_seq;
	progress->reader_seq += consumed_samples;
	if (produced_samples != NULL) {
		*produced_samples = min_writer_seq < prev_writer_seq ? 0U :
			(uint32_t)(min_writer_seq - prev_writer_seq);
	}
	if (skew_samples != NULL) {
		*skew_samples = (uint32_t)(max_writer_seq - min_writer_seq);
	}
	if (max_writer_seq - min_writer_seq > LINKR_DEBUGGER_LA_RING_MAX_SKEW_SAMPLES) {
		return LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN;
	}
	if (linkr_debugger_logic_analyzer_ring_should_freeze_before_overwrite(
		    prev_writer_seq, progress->reader_seq,
		    produced_samples != NULL ? *produced_samples :
		    (uint32_t)(min_writer_seq - prev_writer_seq),
		    plan->sample_capacity, plan->safety_samples, NULL)) {
		return LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN;
	}

	return LINKR_DEBUGGER_LA_RING_POLL_OK;
}

uint32_t linkr_debugger_logic_analyzer_ring_next_emit_count(
	uint64_t available_samples, uint32_t remaining_samples, uint32_t chunk_samples)
{
	uint32_t target;

	if (chunk_samples == 0U || available_samples == 0U) {
		return 0U;
	}
	if (remaining_samples == 0U) {
		return available_samples >= chunk_samples ? chunk_samples : 0U;
	}

	target = remaining_samples > chunk_samples ? chunk_samples : remaining_samples;
	return available_samples >= target ? target : 0U;
}

uint32_t linkr_debugger_logic_analyzer_ring_drainable_samples(
	uint64_t available_samples, uint32_t remaining_samples, uint32_t chunk_samples)
{
	uint64_t full_chunks;
	uint32_t full_samples;
	uint32_t remainder;

	if (chunk_samples == 0U || available_samples == 0U) {
		return 0U;
	}
	if (remaining_samples == 0U) {
		full_chunks = available_samples / chunk_samples;
		if (full_chunks > UINT32_MAX / chunk_samples) {
			return UINT32_MAX - (UINT32_MAX % chunk_samples);
		}
		return (uint32_t)full_chunks * chunk_samples;
	}

	if (available_samples > remaining_samples) {
		available_samples = remaining_samples;
	}
	full_samples = (uint32_t)(available_samples - (available_samples % chunk_samples));
	remainder = remaining_samples % chunk_samples;
	if (remainder > 0U && available_samples >= remaining_samples) {
		return full_samples + remainder;
	}

	return full_samples;
}

uint32_t linkr_debugger_logic_analyzer_ring_terminal_emit_count(
	uint64_t available_samples, uint32_t remaining_samples,
	uint32_t chunk_samples, bool terminal_pending)
{
	uint64_t target;

	if (!terminal_pending) {
		return linkr_debugger_logic_analyzer_ring_next_emit_count(available_samples,
			remaining_samples, chunk_samples);
	}
	if (available_samples == 0U || chunk_samples == 0U) {
		return 0U;
	}

	target = available_samples;
	if (remaining_samples > 0U && target > remaining_samples) {
		target = remaining_samples;
	}
	if (target > chunk_samples) {
		target = chunk_samples;
	}
	return (uint32_t)target;
}

void linkr_debugger_logic_analyzer_ring_metrics_update(
	struct linkr_debugger_la_ring_metrics *metrics,
	uint64_t poll_gap_us, uint64_t unread_samples, uint64_t emit_us)
{
	if (metrics == NULL) {
		return;
	}
	if (poll_gap_us > metrics->max_poll_gap_us) {
		metrics->max_poll_gap_us = poll_gap_us;
	}
	if (unread_samples > metrics->max_unread_samples) {
		metrics->max_unread_samples = unread_samples;
	}
	if (emit_us > metrics->max_emit_us) {
		metrics->max_emit_us = emit_us;
	}
}

void linkr_debugger_logic_analyzer_ring_metrics_update_consume(
	struct linkr_debugger_la_ring_metrics *metrics,
	uint64_t compact_us, uint64_t callback_us, uint64_t total_us)
{
	if (metrics == NULL) {
		return;
	}
	if (compact_us > metrics->max_compact_us) {
		metrics->max_compact_us = compact_us;
	}
	if (callback_us > metrics->max_callback_us) {
		metrics->max_callback_us = callback_us;
	}
	if (total_us > metrics->max_emit_us) {
		metrics->max_emit_us = total_us;
	}
	metrics->total_compact_us += compact_us;
	metrics->total_callback_us += callback_us;
	metrics->total_consume_us += total_us;
	metrics->consume_chunk_count++;
}

void linkr_debugger_logic_analyzer_ring_metrics_clear_consumer_gap(
	struct linkr_debugger_la_ring_metrics *metrics)
{
	if (metrics != NULL) {
		metrics->consumer_gap_armed = false;
		metrics->last_consume_complete_us = 0U;
	}
}

void linkr_debugger_logic_analyzer_ring_metrics_update_inter_chunk_gap(
	struct linkr_debugger_la_ring_metrics *metrics,
	uint64_t next_consume_start_us)
{
	uint64_t gap_us;

	if (metrics == NULL || !metrics->consumer_gap_armed ||
	    next_consume_start_us < metrics->last_consume_complete_us) {
		return;
	}

	gap_us = next_consume_start_us - metrics->last_consume_complete_us;
	if (gap_us > metrics->max_consumer_inter_chunk_gap_us) {
		metrics->max_consumer_inter_chunk_gap_us = gap_us;
	}
}

void linkr_debugger_logic_analyzer_ring_metrics_mark_chunk_complete(
	struct linkr_debugger_la_ring_metrics *metrics,
	uint64_t complete_us)
{
	if (metrics == NULL) {
		return;
	}
	metrics->last_consume_complete_us = complete_us;
	metrics->consumer_gap_armed = true;
}

void linkr_debugger_logic_analyzer_ring_metrics_update_yield_resume(
	struct linkr_debugger_la_ring_metrics *metrics,
	uint64_t duration_us, bool sink_handoff)
{
	if (metrics == NULL) {
		return;
	}
	if (duration_us > metrics->max_consumer_yield_resume_us) {
		metrics->max_consumer_yield_resume_us = duration_us;
	}
	if (!sink_handoff) {
		metrics->legacy_yield_count++;
	}
}

void linkr_debugger_logic_analyzer_ring_metrics_update_sink_handoff(
	struct linkr_debugger_la_ring_metrics *metrics,
	bool requested, bool executed)
{
	if (metrics == NULL || !requested) {
		return;
	}
	metrics->sink_handoff_requested_count++;
	if (executed) {
		metrics->sink_handoff_executed_count++;
	}
}

bool linkr_debugger_logic_analyzer_stream_copy_complete_advance_reader(
	struct linkr_debugger_la_ring_progress *progress,
	struct linkr_debugger_la_ring_metrics *metrics,
	bool generation_current,
	uint32_t operation_generation,
	uint64_t start_reader_seq,
	uint32_t copied_samples)
{
	if (progress == NULL || !generation_current || copied_samples == 0U ||
	    progress->generation != operation_generation ||
	    progress->reader_seq != start_reader_seq) {
		return false;
	}

	progress->reader_seq += copied_samples;
	linkr_debugger_logic_analyzer_ring_metrics_update(metrics, 0U,
		progress->writer_seq - progress->reader_seq, 0U);
	return true;
}

bool linkr_debugger_logic_analyzer_stream_callback_allows_protocol_update(
	bool generation_current, uint32_t emitted_samples)
{
	return generation_current && emitted_samples > 0U;
}

bool linkr_debugger_logic_analyzer_stream_sink_allows_protocol_update(
	bool generation_current, uint32_t committed_samples)
{
	return generation_current && committed_samples > 0U;
}

static void la_write_sample_le(uint16_t sample, uint8_t bytes_per_sample,
	uint8_t *dst)
{
	for (uint8_t byte = 0U; byte < bytes_per_sample; byte++) {
		dst[byte] = (uint8_t)((sample >> (byte * 8U)) & 0xffU);
	}
}

int linkr_debugger_logic_analyzer_stream_sink_validate(
	const struct linkr_debugger_la_config *config,
	const struct linkr_debugger_la_stream_sink *sink)
{
	uint8_t active_pin_count;
	uint8_t expected_bytes_per_sample;

	if (config == NULL || sink == NULL || sink->lease == NULL || sink->commit == NULL ||
	    sink->abort == NULL || sink->terminal == NULL) {
		return -EINVAL;
	}
	if (sink->format != LINKR_DEBUGGER_LA_STREAM_PAYLOAD_PACKED_LE_BYTES) {
		return -ENOTSUP;
	}

	active_pin_count = la_active_pin_count(config);
	if (active_pin_count == 0U || active_pin_count > LINKR_DEBUGGER_LA_MAX_CHANNELS) {
		return -EINVAL;
	}
	expected_bytes_per_sample = (uint8_t)((active_pin_count + 7U) / 8U);
	if (expected_bytes_per_sample == 0U || expected_bytes_per_sample > sizeof(uint16_t) ||
	    sink->bytes_per_sample != expected_bytes_per_sample) {
		return -EINVAL;
	}
	if (sink->max_chunk_samples > LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES) {
		return -EINVAL;
	}

	return 0;
}

int linkr_debugger_logic_analyzer_stream_sink_lease_payload(
	const struct linkr_debugger_la_stream_sink *sink,
	uint32_t sample_count,
	struct linkr_debugger_la_stream_sink_lease *lease)
{
	if (sink == NULL || sink->lease == NULL || lease == NULL || sample_count == 0U ||
	    sink->bytes_per_sample == 0U) {
		return -EINVAL;
	}

	memset(lease, 0, sizeof(*lease));
	return sink->lease(sample_count, sink->bytes_per_sample, sink->user_data, lease);
}

int linkr_debugger_logic_analyzer_stream_sink_write_packed_payload(
	const struct linkr_debugger_la_packed_ring_plan *plan,
	const uint32_t * const lane_words[],
	const uint32_t lane_word_counts[],
	uint64_t first_sample,
	uint32_t sample_count,
	uint8_t bytes_per_sample,
	uint8_t *payload,
	size_t payload_capacity,
	uint16_t *values_or,
	uint16_t *values_and)
{
	uint16_t or_acc = 0U;
	uint16_t and_acc = 0xffffU;
	size_t needed;
	int ret;

	if (values_or != NULL) {
		*values_or = 0U;
	}
	if (values_and != NULL) {
		*values_and = 0xffffU;
	}
	if (plan == NULL || lane_words == NULL || lane_word_counts == NULL || payload == NULL ||
	    sample_count == 0U || bytes_per_sample == 0U ||
	    bytes_per_sample > sizeof(uint16_t) || bytes_per_sample != plan->bytes_per_sample) {
		return -EINVAL;
	}
	needed = (size_t)sample_count * bytes_per_sample;
	if (needed > payload_capacity) {
		return -ENOSPC;
	}

	ret = linkr_debugger_logic_analyzer_decode_packed_ring_span(plan, lane_words,
		lane_word_counts, first_sample, payload, needed, sample_count);
	if (ret < 0) {
		return ret;
	}

	for (uint32_t i = 0U; i < sample_count; i++) {
		uint8_t *src = &payload[(size_t)i * bytes_per_sample];
		uint16_t sample = src[0];

		if (bytes_per_sample > 1U) {
			sample |= (uint16_t)src[1] << 8U;
		}

		or_acc |= sample;
		and_acc &= sample;
	}

	if (values_or != NULL) {
		*values_or = or_acc;
	}
	if (values_and != NULL) {
		*values_and = and_acc;
	}
	return 0;
}

int linkr_debugger_logic_analyzer_stream_sink_commit_payload(
	const struct linkr_debugger_la_stream_sink *sink,
	const struct linkr_debugger_la_stream_sink_commit *commit)
{
	if (sink == NULL || sink->commit == NULL || commit == NULL || commit->token == NULL ||
	    commit->sample_count == 0U || commit->bytes_per_sample == 0U ||
	    commit->payload_len == 0U) {
		return -EINVAL;
	}

	return sink->commit(commit, sink->user_data);
}

void linkr_debugger_logic_analyzer_stream_sink_abort_payload(
	const struct linkr_debugger_la_stream_sink *sink,
	struct linkr_debugger_la_stream_sink_lease *lease)
{
	if (sink == NULL || sink->abort == NULL || lease == NULL || lease->token == NULL) {
		return;
	}

	sink->abort(lease->token, sink->user_data);
	lease->payload = NULL;
	lease->capacity = 0U;
	lease->token = NULL;
}

void linkr_debugger_logic_analyzer_stream_sink_notify_terminal(
	const struct linkr_debugger_la_stream_sink *sink,
	enum linkr_debugger_la_ring_poll_result status,
	uint32_t sequence)
{
	if (sink == NULL || sink->terminal == NULL) {
		return;
	}

	sink->terminal(status, sequence, sink->user_data);
}

bool linkr_debugger_logic_analyzer_stream_generation_current(
	bool active, uint32_t stream_generation, uint32_t current_generation)
{
	return active && stream_generation == current_generation;
}

bool linkr_debugger_logic_analyzer_stream_start_must_wait_idle(
	bool thread_started, bool thread_busy)
{
	return thread_started && thread_busy;
}

uint8_t linkr_debugger_logic_analyzer_stream_idle_wait_mask(
	bool producer_started, bool producer_busy,
	bool consumer_started, bool consumer_busy)
{
	uint8_t mask = 0U;

	if (linkr_debugger_logic_analyzer_stream_start_must_wait_idle(
	    producer_started, producer_busy)) {
		mask |= LINKR_DEBUGGER_LA_STREAM_WAIT_PRODUCER;
	}
	if (linkr_debugger_logic_analyzer_stream_start_must_wait_idle(
	    consumer_started, consumer_busy)) {
		mask |= LINKR_DEBUGGER_LA_STREAM_WAIT_CONSUMER;
	}

	return mask;
}

struct linkr_debugger_la_ring_freeze_policy
	linkr_debugger_logic_analyzer_ring_freeze_policy(uint8_t lane_count)
{
	struct linkr_debugger_la_ring_freeze_policy policy = {
		.stop_sampler_sm_a = true,
		.stop_trigger_sm = true,
		.abort_dma_a = true,
	};

	if (lane_count > 1U) {
		policy.stop_sampler_sm_b = true;
		policy.abort_dma_b = true;
	}
	return policy;
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
		uint8_t pin = la_pin_at(config, i) - config->pin_base;

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
	if (linkr_debugger_logic_analyzer_actual_rate(config->sample_rate_hz) == 0U) {
		return -EINVAL;
	}

	if (linkr_debugger_logic_analyzer_bounded_sample_target(config->pre_samples,
	    config->post_samples, &total_samples) < 0 ||
	    (config->pre_samples > 0U &&
	     !linkr_debugger_logic_analyzer_pre_trigger_plan_supported(config)) ||
	    total_samples > capacity_samples ||
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

int linkr_debugger_logic_analyzer_validate_stream_config(
	const struct linkr_debugger_la_config *config)
{
	uint8_t active_pin_count;

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
	if (config->pre_samples != 0U &&
	    !linkr_debugger_logic_analyzer_pre_trigger_plan_supported(config)) {
		return -EINVAL;
	}
	if (config->post_samples > UINT16_MAX && config->post_samples > 0U &&
	    !la_is_packed_physical_plan_supported(config)) {
		return -EINVAL;
	}
	if (linkr_debugger_logic_analyzer_actual_rate(config->sample_rate_hz) == 0U) {
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

bool linkr_debugger_logic_analyzer_wide11_burst_exact_eligible(
	const struct linkr_debugger_la_config *config)
{
	static const uint8_t expected_pins[] = {
		10U, 11U, 12U, 13U, 14U, 15U,
		16U, 17U, 18U, 19U, 20U,
	};

	if (config == NULL || config->trigger > LINKR_DEBUGGER_LA_TRIGGER_EITHER ||
	    config->trigger_pin >= 11U || config->pre_samples != 0U ||
	    config->post_samples != LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES ||
	    config->sample_rate_hz != LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ ||
	    config->selected_pin_count != 11U || config->pin_base != 10U ||
	    config->pin_count != 11U) {
		return false;
	}

	for (size_t i = 0U; i < sizeof(expected_pins) / sizeof(expected_pins[0]); i++) {
		if (config->selected_pins[i] != expected_pins[i]) {
			return false;
		}
	}

	return true;
}

static uint32_t la_aligned_sample_count(uint32_t sample_count, uint32_t alignment)
{
	uint32_t remainder;

	if (alignment == 0U) {
		return 0U;
	}
	remainder = sample_count % alignment;
	return remainder == 0U ? sample_count : sample_count + (alignment - remainder);
}

static uint32_t la_packed_burst_max_chunk_samples(uint8_t bytes_per_sample)
{
	uint32_t payload_bytes = LINKR_DEBUGGER_LA_WIDE11_BURST_CHUNK_SAMPLES *
		LINKR_DEBUGGER_LA_WIDE11_BURST_PACKED_SAMPLE_BYTES;

	return bytes_per_sample == 0U ? 0U : payload_bytes / bytes_per_sample;
}

static int la_packed_ring_size_bits(uint32_t byte_count)
{
	switch (byte_count) {
	case 8192U:
		return 13;
	case 16384U:
		return 14;
	case 32768U:
		return 15;
	default:
		return -EINVAL;
	}
}

static bool la_is_packed_physical_plan_supported(const struct linkr_debugger_la_config *config)
{
	uint32_t actual_rate;

	if (config == NULL || config->pre_samples != 0U ||
	    config->trigger > LINKR_DEBUGGER_LA_TRIGGER_EITHER ||
	    config->post_samples > LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES ||
	    !la_pins_are_valid_unique_safe(config)) {
		return false;
	}
	actual_rate = linkr_debugger_logic_analyzer_actual_rate(config->sample_rate_hz);
	if (actual_rate == 0U) {
		return false;
	}
	if (la_is_single_plan(config) || la_is_fast8_plan(config)) {
		return linkr_debugger_logic_analyzer_packed_rate_limit_supported(
			config->sample_rate_hz, actual_rate,
			LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ);
	}
	if (la_is_wide11_plan(config)) {
		return linkr_debugger_logic_analyzer_packed_rate_limit_supported(
			config->sample_rate_hz, actual_rate,
			LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ);
	}
	return false;
}

static bool la_is_high_rate_packed_burst_request(const struct linkr_debugger_la_config *config)
{
	if (config == NULL || config->post_samples != 0U ||
	    !la_is_packed_physical_plan_supported(config)) {
		return false;
	}
	if (config->sample_rate_hz == LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ) {
		return la_is_single_plan(config) || la_is_fast8_plan(config) || la_is_wide11_plan(config);
	}
	if (config->sample_rate_hz == LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ) {
		return la_is_single_plan(config) || la_is_fast8_plan(config);
	}
	return false;
}

int linkr_debugger_logic_analyzer_packed_burst_plan(
	const struct linkr_debugger_la_config *config,
	struct linkr_debugger_la_packed_burst_plan *plan)
{
	struct linkr_debugger_la_packed_burst_plan computed;
	uint32_t requested;
	uint32_t alignment = 0U;
	uint32_t source_samples;
	uint8_t active_pin_count;

	if (config == NULL || plan == NULL ||
	    !(config->post_samples > 0U ? la_is_packed_physical_plan_supported(config) :
	      la_is_high_rate_packed_burst_request(config))) {
		return -EINVAL;
	}

	memset(&computed, 0, sizeof(computed));
	active_pin_count = la_active_pin_count(config);
	requested = config->post_samples == 0U ?
		LINKR_DEBUGGER_LA_PACKED_BURST_CONTINUOUS_SAMPLES : config->post_samples;
	if (requested == 0U || requested > LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES) {
		return -EINVAL;
	}
	computed.requested_sample_count = config->post_samples;
	computed.emitted_sample_count = requested;
	computed.bytes_per_sample = (uint8_t)((active_pin_count + 7U) / 8U);
	computed.selected_pin_count = active_pin_count;
	computed.continuous_until_capacity = config->post_samples == 0U;
	for (uint8_t i = 0U; i < active_pin_count; i++) {
		computed.selected_pins[i] = la_pin_at(config, i);
	}

	if (la_is_single_plan(config)) {
		computed.kind = LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED;
		computed.lane_count = 1U;
		computed.lanes[0].pin_base = la_pin_at(config, 0U);
		computed.lanes[0].pin_count = 1U;
		computed.lanes[0].bits_per_sample = 1U;
		computed.lanes[0].autopush_bits = 32U;
		computed.lanes[0].samples_per_word = 32U;
		computed.lanes[0].arena_offset = LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_A_OFFSET;
		alignment = 32U;
	} else if (la_is_fast8_plan(config)) {
		computed.kind = LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED;
		computed.lane_count = 1U;
		computed.lanes[0].pin_base = 10U;
		computed.lanes[0].pin_count = 8U;
		computed.lanes[0].bits_per_sample = 8U;
		computed.lanes[0].autopush_bits = 32U;
		computed.lanes[0].samples_per_word = 4U;
		computed.lanes[0].arena_offset = LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_A_OFFSET;
		alignment = 4U;
	} else if (la_is_wide11_plan(config)) {
		computed.kind = LINKR_DEBUGGER_LA_HARDWARE_PLAN_WIDE11_SPLIT_PACKED;
		computed.lane_count = 2U;
		computed.lanes[0].pin_base = 10U;
		computed.lanes[0].pin_count = 8U;
		computed.lanes[0].bits_per_sample = LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_BITS_PER_SAMPLE;
		computed.lanes[0].autopush_bits = LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_AUTOPUSH_BITS;
		computed.lanes[0].samples_per_word = LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_SAMPLES_PER_WORD;
		computed.lanes[0].arena_offset = LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_A_OFFSET;
		computed.lanes[1].pin_base = 18U;
		computed.lanes[1].pin_count = 3U;
		computed.lanes[1].bits_per_sample = LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_BITS_PER_SAMPLE;
		computed.lanes[1].autopush_bits = LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_AUTOPUSH_BITS;
		computed.lanes[1].samples_per_word = LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_SAMPLES_PER_WORD;
		computed.lanes[1].arena_offset = LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_B_OFFSET;
		alignment = LINKR_DEBUGGER_LA_WIDE11_BURST_SAMPLE_ALIGNMENT;
	} else {
		return -EINVAL;
	}
	source_samples = la_aligned_sample_count(requested, alignment);
	if (source_samples == 0U) {
		return -EOVERFLOW;
	}
	computed.source_sample_count = source_samples;
	computed.sample_alignment = alignment;

	for (uint8_t lane = 0U; lane < computed.lane_count; lane++) {
		struct linkr_debugger_la_packed_burst_lane *l = &computed.lanes[lane];

		if (l->samples_per_word == 0U || source_samples % l->samples_per_word != 0U) {
			return -EINVAL;
		}
		l->word_count = source_samples / l->samples_per_word;
		if (l->word_count > UINT32_MAX / (uint32_t)sizeof(uint32_t)) {
			return -EOVERFLOW;
		}
		l->byte_count = l->word_count * (uint32_t)sizeof(uint32_t);
		if (UINT32_MAX - computed.total_byte_count < l->byte_count) {
			return -EOVERFLOW;
		}
		computed.total_byte_count += l->byte_count;
	}
	if (computed.total_byte_count > LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_TOTAL_BYTES) {
		return -ENOSPC;
	}

	*plan = computed;
	return 0;
}

int linkr_debugger_logic_analyzer_packed_ring_plan(
	const struct linkr_debugger_la_config *config,
	struct linkr_debugger_la_packed_ring_plan *plan)
{
	struct linkr_debugger_la_packed_ring_plan computed;
	uint8_t active_pin_count;
	int ring_size_bits;

	if (config == NULL || plan == NULL ||
	    (config->pre_samples != 0U &&
	     !linkr_debugger_logic_analyzer_pre_trigger_supported(config->trigger,
		config->sample_rate_hz, config->pre_samples, config->post_samples)) ||
	    config->trigger > LINKR_DEBUGGER_LA_TRIGGER_EITHER ||
	    !la_pins_are_valid_unique_safe(config)) {
		return -EINVAL;
	}

	memset(&computed, 0, sizeof(computed));
	active_pin_count = la_active_pin_count(config);
	computed.bytes_per_sample = (uint8_t)((active_pin_count + 7U) / 8U);
	computed.selected_pin_count = active_pin_count;
	computed.safety_samples = LINKR_DEBUGGER_LA_RING_SAFETY_SAMPLES;
	for (uint8_t i = 0U; i < active_pin_count; i++) {
		computed.selected_pins[i] = la_pin_at(config, i);
	}

	if (la_is_single_plan(config)) {
		computed.kind = LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED;
		computed.chunk_samples = 2048U;
		computed.lane_count = 1U;
		computed.lanes[0].pin_base = la_pin_at(config, 0U);
		computed.lanes[0].pin_count = 1U;
		computed.lanes[0].bits_per_sample = 1U;
		computed.lanes[0].autopush_bits = 32U;
		computed.lanes[0].samples_per_word = 32U;
		computed.lanes[0].byte_count = LINKR_DEBUGGER_LA_RING_BUFFER_BYTES;
		computed.lanes[0].word_count = LINKR_DEBUGGER_LA_RING_SAMPLES;
		computed.lanes[0].arena_offset = LINKR_DEBUGGER_CAPTURE_ARENA_LA_PACKED_RING_OFFSET;
	} else if (la_is_fast8_plan(config)) {
		computed.kind = LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED;
		computed.chunk_samples = 2048U;
		computed.lane_count = 1U;
		computed.lanes[0].pin_base = 10U;
		computed.lanes[0].pin_count = 8U;
		computed.lanes[0].bits_per_sample = 8U;
		computed.lanes[0].autopush_bits = 32U;
		computed.lanes[0].samples_per_word = 4U;
		computed.lanes[0].byte_count = LINKR_DEBUGGER_LA_RING_BUFFER_BYTES;
		computed.lanes[0].word_count = LINKR_DEBUGGER_LA_RING_SAMPLES;
		computed.lanes[0].arena_offset = LINKR_DEBUGGER_CAPTURE_ARENA_LA_PACKED_RING_OFFSET;
	} else if (la_is_wide11_plan(config)) {
		computed.kind = LINKR_DEBUGGER_LA_HARDWARE_PLAN_WIDE11_SPLIT_PACKED;
		computed.chunk_samples = LINKR_DEBUGGER_LA_WIDE11_BURST_CHUNK_SAMPLES;
		computed.lane_count = 2U;
		computed.lanes[0].pin_base = 10U;
		computed.lanes[0].pin_count = 8U;
		computed.lanes[0].bits_per_sample = LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_BITS_PER_SAMPLE;
		computed.lanes[0].autopush_bits = LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_AUTOPUSH_BITS;
		computed.lanes[0].samples_per_word = LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_SAMPLES_PER_WORD;
		computed.lanes[0].byte_count = 16384U;
		computed.lanes[0].word_count = 4096U;
		computed.lanes[0].arena_offset = LINKR_DEBUGGER_CAPTURE_ARENA_LA_PACKED_RING_OFFSET;
		computed.lanes[1].pin_base = 18U;
		computed.lanes[1].pin_count = 3U;
		computed.lanes[1].bits_per_sample = LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_BITS_PER_SAMPLE;
		computed.lanes[1].autopush_bits = LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_AUTOPUSH_BITS;
		computed.lanes[1].samples_per_word = LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_SAMPLES_PER_WORD;
		computed.lanes[1].byte_count = 8192U;
		computed.lanes[1].word_count = 2048U;
		computed.lanes[1].arena_offset = LINKR_DEBUGGER_CAPTURE_ARENA_LA_PACKED_RING_OFFSET + 16384U;
	} else {
		return -EINVAL;
	}

	computed.sample_capacity = UINT32_MAX;
	for (uint8_t lane = 0U; lane < computed.lane_count; lane++) {
		struct linkr_debugger_la_packed_ring_lane *l = &computed.lanes[lane];

		ring_size_bits = la_packed_ring_size_bits(l->byte_count);
		if (ring_size_bits < 0 || l->samples_per_word == 0U) {
			return -EINVAL;
		}
		l->ring_size_bits = (uint8_t)ring_size_bits;
		l->sample_capacity = l->word_count * l->samples_per_word;
		if (l->sample_capacity < computed.sample_capacity) {
			computed.sample_capacity = l->sample_capacity;
		}
	}
	if (computed.sample_capacity == UINT32_MAX ||
	    computed.sample_capacity <= computed.safety_samples) {
		return -EINVAL;
	}
	computed.usable_sample_capacity = computed.sample_capacity - computed.safety_samples;
	*plan = computed;
	return 0;
}

bool linkr_debugger_logic_analyzer_pre_trigger_plan_supported(
	const struct linkr_debugger_la_config *config)
{
	struct linkr_debugger_la_packed_ring_plan plan;
	uint32_t actual_rate_hz;

	if (config == NULL || !linkr_debugger_logic_analyzer_pre_trigger_supported(
		config->trigger, config->sample_rate_hz, config->pre_samples,
		config->post_samples)) {
		return false;
	}
	actual_rate_hz = linkr_debugger_logic_analyzer_actual_rate(config->sample_rate_hz);
	if (actual_rate_hz == 0U ||
	    linkr_debugger_logic_analyzer_packed_ring_plan(config, &plan) < 0) {
		return false;
	}

	return linkr_debugger_logic_analyzer_pre_trigger_plan_feasible(&plan, actual_rate_hz);
}

int linkr_debugger_logic_analyzer_session_contract(
	const struct linkr_debugger_la_config *config,
	struct linkr_debugger_la_session_contract *contract)
{
	if (config == NULL || contract == NULL ||
	    config->trigger > LINKR_DEBUGGER_LA_TRIGGER_EITHER) {
		return -EINVAL;
	}

	memset(contract, 0, sizeof(*contract));
	contract->trigger_gate = config->trigger;
	contract->pre_samples = config->pre_samples;
	contract->post_samples = config->post_samples;
	if (config->post_samples == 0U) {
		contract->stop_policy = LINKR_DEBUGGER_LA_STOP_POLICY_CONTINUOUS;
	} else {
		contract->stop_policy = LINKR_DEBUGGER_LA_STOP_POLICY_BOUNDED;
	}
	return 0;
}

const char *linkr_debugger_logic_analyzer_stop_reason_name(
	enum linkr_debugger_la_stop_reason reason)
{
	switch (reason) {
	case LINKR_DEBUGGER_LA_STOP_REASON_COMPLETE:
		return "complete";
	case LINKR_DEBUGGER_LA_STOP_REASON_HOST_STOP:
		return "host_stop";
	case LINKR_DEBUGGER_LA_STOP_REASON_FREEZE_BEFORE_OVERWRITE:
		return "freeze_before_overwrite";
	case LINKR_DEBUGGER_LA_STOP_REASON_TRANSPORT_PRESSURE:
		return "transport_pressure";
	case LINKR_DEBUGGER_LA_STOP_REASON_DMA_ERROR:
		return "dma_error";
	case LINKR_DEBUGGER_LA_STOP_REASON_UNSUPPORTED:
		return "unsupported";
	default:
		return "unknown";
	}
}

const char *linkr_debugger_logic_analyzer_hardware_plan_name(
	enum linkr_debugger_la_hardware_plan_kind kind)
{
	switch (kind) {
	case LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED:
		return "single_packed";
	case LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED:
		return "fast8_packed";
	case LINKR_DEBUGGER_LA_HARDWARE_PLAN_WIDE11_SPLIT_PACKED:
		return "wide11_split_packed";
	default:
		return "unsupported";
	}
}

struct la_plan_selector_row {
	enum linkr_debugger_la_hardware_plan_kind kind;
	enum linkr_debugger_la_pipeline_family pipeline_family;
	uint8_t bytes_per_sample;
	uint8_t max_channels;
	uint32_t max_sample_rate_hz;
	bool (*matches)(const struct linkr_debugger_la_config *config);
};

static const struct la_plan_selector_row la_plan_selector_rows[] = {
	{
		.kind = LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED,
		.pipeline_family = LINKR_DEBUGGER_LA_PIPELINE_FAMILY_COMMON_PACKED,
		.bytes_per_sample = 1U,
		.max_channels = 1U,
		.max_sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ,
		.matches = la_is_single_plan,
	},
	{
		.kind = LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED,
		.pipeline_family = LINKR_DEBUGGER_LA_PIPELINE_FAMILY_COMMON_PACKED,
		.bytes_per_sample = 1U,
		.max_channels = 8U,
		.max_sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ,
		.matches = la_is_fast8_plan,
	},
	{
		.kind = LINKR_DEBUGGER_LA_HARDWARE_PLAN_WIDE11_SPLIT_PACKED,
		.pipeline_family = LINKR_DEBUGGER_LA_PIPELINE_FAMILY_COMMON_PACKED,
		.bytes_per_sample = LINKR_DEBUGGER_LA_WIDE11_BURST_PACKED_SAMPLE_BYTES,
		.max_channels = 11U,
		.max_sample_rate_hz = LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ,
		.matches = la_is_wide11_plan,
	},
};

int linkr_debugger_logic_analyzer_select_hardware_plan(
	const struct linkr_debugger_la_config *config,
	bool config_v2,
	struct linkr_debugger_la_hardware_plan *plan)
{
	struct linkr_debugger_la_hardware_plan selected;
	uint8_t active_pin_count;
	bool pre_trigger;

	if (config == NULL || plan == NULL) {
		return -EINVAL;
	}

	memset(&selected, 0, sizeof(selected));
	selected.kind = LINKR_DEBUGGER_LA_HARDWARE_PLAN_UNSUPPORTED;
	selected.pipeline_family = LINKR_DEBUGGER_LA_PIPELINE_FAMILY_UNSUPPORTED;
	selected.unsupported_reason = LINKR_DEBUGGER_LA_STOP_REASON_UNSUPPORTED;
	selected.reason = "unsupported";
	active_pin_count = la_active_pin_count(config);
	pre_trigger = linkr_debugger_logic_analyzer_pre_trigger_plan_supported(config);

	if (config->trigger > LINKR_DEBUGGER_LA_TRIGGER_EITHER || active_pin_count == 0U ||
	    active_pin_count > LINKR_DEBUGGER_LA_MAX_CHANNELS ||
	    (pre_trigger && config->trigger != LINKR_DEBUGGER_LA_TRIGGER_NONE &&
	     config->trigger_pin >= active_pin_count) ||
	    (config->pre_samples != 0U && !pre_trigger) ||
	    linkr_debugger_logic_analyzer_actual_rate(config->sample_rate_hz) == 0U ||
	    !la_pins_are_valid_unique_safe(config)) {
		*plan = selected;
		return 0;
	}
	for (size_t i = 0U; i < sizeof(la_plan_selector_rows) /
	     sizeof(la_plan_selector_rows[0]); i++) {
		const struct la_plan_selector_row *row = &la_plan_selector_rows[i];

		if (!row->matches(config)) {
			continue;
		}
		if (config->sample_rate_hz > row->max_sample_rate_hz) {
			continue;
		}
		selected.kind = row->kind;
		selected.pipeline_family = row->pipeline_family;
		selected.bytes_per_sample = row->bytes_per_sample;
		selected.channel_count = active_pin_count;
		selected.max_sample_rate_hz = row->max_sample_rate_hz;
		selected.reason = "supported";
		if (pre_trigger) {
			selected.supported = true;
			selected.legacy_adapter = LINKR_DEBUGGER_LA_LEGACY_ADAPTER_RING_STREAM;
			*plan = selected;
			return 0;
		}
		if (config->post_samples > LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES) {
			selected.supported = false;
			selected.legacy_adapter = LINKR_DEBUGGER_LA_LEGACY_ADAPTER_NONE;
			selected.reason = "post_samples_too_large";
			*plan = selected;
			return 0;
		}
		if (config->post_samples > UINT16_MAX && !config_v2) {
			selected.supported = false;
			selected.legacy_adapter = LINKR_DEBUGGER_LA_LEGACY_ADAPTER_NONE;
			selected.reason = "config_v2_required";
			*plan = selected;
			return 0;
		}
		if (config->post_samples > 0U &&
		    la_is_packed_physical_plan_supported(config)) {
			selected.supported = true;
			selected.legacy_adapter = LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST;
			selected.reason = "supported";
		} else if (la_is_high_rate_packed_burst_request(config)) {
			selected.supported = true;
			selected.legacy_adapter = LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST;
			selected.reason = "continuous_until_capacity";
		} else {
			selected.supported = true;
			selected.legacy_adapter = LINKR_DEBUGGER_LA_LEGACY_ADAPTER_RING_STREAM;
		}
		*plan = selected;
		return 0;
	}

	selected.reason = config->post_samples > UINT16_MAX && !config_v2 ?
		"config_v2_required" : "unsupported_pin_plan";
	*plan = selected;
	return 0;
}

bool linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(
	const struct linkr_debugger_la_config *config)
{
	return config != NULL &&
		config->pre_samples == 0U &&
		config->trigger > LINKR_DEBUGGER_LA_TRIGGER_NONE &&
		config->trigger <= LINKR_DEBUGGER_LA_TRIGGER_EITHER;
}

enum linkr_debugger_la_trigger_type linkr_debugger_logic_analyzer_effective_trigger(
	enum linkr_debugger_la_trigger_type trigger,
	bool current_level_high)
{
	if (trigger == LINKR_DEBUGGER_LA_TRIGGER_EITHER) {
		return current_level_high ? LINKR_DEBUGGER_LA_TRIGGER_FALLING :
			LINKR_DEBUGGER_LA_TRIGGER_RISING;
	}

	return trigger;
}

static int la_build_single_instruction_program(uint16_t instruction,
	uint8_t offset, uint16_t *instructions, size_t instruction_count,
	struct linkr_debugger_la_pio_program_layout *layout)
{
	if (instructions == NULL || layout == NULL || instruction_count < 1U || offset > 31U) {
		return -EINVAL;
	}

	memset(layout, 0, sizeof(*layout));
	instructions[0] = instruction;
	layout->length = 1U;
	return 0;
}

int linkr_debugger_logic_analyzer_build_capture_program(
	bool wait_for_trigger_irq,
	uint8_t offset,
	uint16_t *instructions,
	size_t instruction_count,
	struct linkr_debugger_la_pio_program_layout *layout)
{
	uint8_t length = wait_for_trigger_irq ? 2U : 1U;

	if (instructions == NULL || layout == NULL || instruction_count < length ||
	    offset > 32U - length) {
		return -EINVAL;
	}

	memset(layout, 0, sizeof(*layout));
	if (wait_for_trigger_irq) {
		instructions[0] = LINKR_DEBUGGER_LA_PIO_WAIT_IRQ0;
		instructions[1] = LINKR_DEBUGGER_LA_PIO_IN_PINS_32;
		layout->wrap_target = 1U;
		layout->wrap = 1U;
	} else {
		instructions[0] = LINKR_DEBUGGER_LA_PIO_IN_PINS_32;
		layout->wrap_target = 0U;
		layout->wrap = 0U;
	}
	layout->length = length;
	return 0;
}

int linkr_debugger_logic_analyzer_build_wide11_sm_a_program(
	uint8_t offset,
	uint16_t *instructions,
	size_t instruction_count,
	struct linkr_debugger_la_pio_program_layout *layout)
{
	return la_build_single_instruction_program((uint16_t)(0x4000U |
		LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_BITS_PER_SAMPLE),
		offset, instructions, instruction_count, layout);
}

int linkr_debugger_logic_analyzer_build_wide11_sm_b_program(
	uint8_t offset,
	uint16_t *instructions,
	size_t instruction_count,
	struct linkr_debugger_la_pio_program_layout *layout)
{
	return la_build_single_instruction_program((uint16_t)(0x4000U |
		LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_BITS_PER_SAMPLE),
		offset, instructions, instruction_count, layout);
}

int linkr_debugger_logic_analyzer_wide11_burst_plan(
	uint32_t sample_count,
	struct linkr_debugger_la_wide11_burst_plan *plan)
{
	struct linkr_debugger_la_wide11_burst_plan computed;

	if (plan == NULL) {
		return -EINVAL;
	}
	if (sample_count == 0U ||
	    (sample_count % LINKR_DEBUGGER_LA_WIDE11_BURST_SAMPLE_ALIGNMENT) != 0U) {
		return -EINVAL;
	}

	memset(&computed, 0, sizeof(computed));
	computed.sample_count = sample_count;
	computed.lane_a_word_count = sample_count /
		LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_SAMPLES_PER_WORD;
	computed.lane_b_word_count = sample_count /
		LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_SAMPLES_PER_WORD;
	if (computed.lane_a_word_count > UINT32_MAX / (uint32_t)sizeof(uint32_t) ||
	    computed.lane_b_word_count > UINT32_MAX / (uint32_t)sizeof(uint32_t)) {
		return -EOVERFLOW;
	}

	computed.lane_a_byte_count = computed.lane_a_word_count * (uint32_t)sizeof(uint32_t);
	computed.lane_b_byte_count = computed.lane_b_word_count * (uint32_t)sizeof(uint32_t);
	if (UINT32_MAX - computed.lane_a_byte_count < computed.lane_b_byte_count) {
		return -EOVERFLOW;
	}
	computed.total_byte_count = computed.lane_a_byte_count + computed.lane_b_byte_count;

	*plan = computed;
	return 0;
}

static uint16_t la_decode_wide11_burst_sample(
	const uint32_t *lane_a_words, const uint32_t *lane_b_words, uint32_t sample_index)
{
	uint32_t lane_a_word = lane_a_words[sample_index /
		LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_SAMPLES_PER_WORD];
	uint32_t lane_b_word = lane_b_words[sample_index /
		LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_SAMPLES_PER_WORD];
	uint8_t lane_a_shift = (uint8_t)((sample_index %
		LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_SAMPLES_PER_WORD) *
		LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_BITS_PER_SAMPLE);
	uint8_t lane_b_shift = (uint8_t)((32U - LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_AUTOPUSH_BITS) +
		((sample_index % LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_SAMPLES_PER_WORD) *
		 LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_BITS_PER_SAMPLE));
	uint16_t gp10_17 = (uint16_t)((lane_a_word >> lane_a_shift) & 0x00ffU);
	uint16_t gp18_20 = (uint16_t)(((lane_b_word >> lane_b_shift) & 0x07U) << 8U);

	return (uint16_t)(gp10_17 | gp18_20);
}

static bool la_packed_burst_lane_contains_pin(
	const struct linkr_debugger_la_packed_burst_lane *lane, uint8_t pin,
	uint8_t *bit_index)
{
	if (lane == NULL || pin < lane->pin_base ||
	    pin >= (uint8_t)(lane->pin_base + lane->pin_count)) {
		return false;
	}
	if (bit_index != NULL) {
		*bit_index = (uint8_t)(pin - lane->pin_base);
	}
	return true;
}

static uint32_t la_decode_packed_burst_lane_value(
	const struct linkr_debugger_la_packed_burst_lane *lane,
	const uint32_t *words, uint32_t sample_index)
{
	uint32_t word;
	uint8_t in_word;
	uint8_t shift;
	uint32_t mask;

	if (lane == NULL || words == NULL || lane->samples_per_word == 0U ||
	    lane->bits_per_sample == 0U || lane->bits_per_sample > 31U ||
	    lane->autopush_bits == 0U || lane->autopush_bits > 32U) {
		return 0U;
	}
	word = words[sample_index / lane->samples_per_word];
	in_word = (uint8_t)(sample_index % lane->samples_per_word);
	shift = (uint8_t)((32U - lane->autopush_bits) +
		((uint32_t)in_word * lane->bits_per_sample));
	mask = (1UL << lane->bits_per_sample) - 1UL;
	return (word >> shift) & mask;
}

int linkr_debugger_logic_analyzer_decode_packed_burst_span(
	const struct linkr_debugger_la_packed_burst_plan *plan,
	const uint32_t * const lane_words[],
	const uint32_t lane_word_counts[],
	uint32_t first_sample,
	uint8_t *packed_le,
	size_t packed_len,
	uint32_t sample_count)
{
	size_t needed_len;

	if (plan == NULL || lane_words == NULL || lane_word_counts == NULL || packed_le == NULL ||
	    sample_count == 0U || plan->lane_count == 0U ||
	    plan->lane_count > LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES ||
	    plan->bytes_per_sample == 0U || plan->bytes_per_sample > sizeof(uint16_t)) {
		return -EINVAL;
	}
	if (UINT32_MAX - first_sample < sample_count ||
	    first_sample + sample_count > plan->source_sample_count ||
	    first_sample + sample_count > plan->emitted_sample_count) {
		return -EINVAL;
	}
	for (uint8_t lane = 0U; lane < plan->lane_count; lane++) {
		if (lane_words[lane] == NULL ||
		    lane_word_counts[lane] != plan->lanes[lane].word_count) {
			return -EINVAL;
		}
	}
#if SIZE_MAX < UINT32_MAX
	if (sample_count > SIZE_MAX / plan->bytes_per_sample) {
		return -EOVERFLOW;
	}
#endif
	needed_len = (size_t)sample_count * plan->bytes_per_sample;
	if (packed_len < needed_len) {
		return -EMSGSIZE;
	}

	for (uint32_t i = 0U; i < sample_count; i++) {
		uint16_t sample = 0U;
		uint8_t *dst = &packed_le[(size_t)i * plan->bytes_per_sample];

		for (uint8_t selected = 0U; selected < plan->selected_pin_count; selected++) {
			uint8_t pin = plan->selected_pins[selected];

			for (uint8_t lane = 0U; lane < plan->lane_count; lane++) {
				uint8_t bit_index;

				if (la_packed_burst_lane_contains_pin(&plan->lanes[lane], pin,
				    &bit_index)) {
					uint32_t lane_value = la_decode_packed_burst_lane_value(
						&plan->lanes[lane], lane_words[lane], first_sample + i);

					if ((lane_value & BIT(bit_index)) != 0U) {
						sample |= (uint16_t)BIT(selected);
					}
					break;
				}
			}
		}
		for (uint8_t byte = 0U; byte < plan->bytes_per_sample; byte++) {
			dst[byte] = (uint8_t)((sample >> (byte * 8U)) & 0xffU);
		}
	}

	return 0;
}

static uint16_t la_decode_packed_ring_sample(
	const struct linkr_debugger_la_packed_ring_plan *plan,
	const uint32_t * const lane_words[], uint64_t sample_index)
{
	uint16_t sample = 0U;

	for (uint8_t selected = 0U; selected < plan->selected_pin_count; selected++) {
		uint8_t pin = plan->selected_pins[selected];

		for (uint8_t lane = 0U; lane < plan->lane_count; lane++) {
			const struct linkr_debugger_la_packed_ring_lane *ring_lane = &plan->lanes[lane];
			struct linkr_debugger_la_packed_burst_lane burst_lane = {
				.pin_base = ring_lane->pin_base,
				.pin_count = ring_lane->pin_count,
				.bits_per_sample = ring_lane->bits_per_sample,
				.autopush_bits = ring_lane->autopush_bits,
				.samples_per_word = ring_lane->samples_per_word,
			};
			uint8_t bit_index;

			if (la_packed_burst_lane_contains_pin(&burst_lane, pin, &bit_index)) {
				uint32_t lane_sample = (uint32_t)(sample_index % ring_lane->sample_capacity);
				uint32_t lane_value = la_decode_packed_burst_lane_value(
					&burst_lane, lane_words[lane], lane_sample);

				if ((lane_value & BIT(bit_index)) != 0U) {
					sample |= (uint16_t)BIT(selected);
				}
				break;
			}
		}
	}

	return sample;
}

struct la_packed_ring_trigger_geometry {
	uint32_t sample_capacity;
	uint32_t word_count;
	uint8_t lane_index;
	uint8_t bits_per_sample;
	uint8_t samples_per_word;
	uint8_t shift_base;
};

static int la_packed_ring_trigger_geometry_build(
	const struct linkr_debugger_la_packed_ring_plan *plan,
	uint8_t trigger_pin,
	struct la_packed_ring_trigger_geometry *geometry)
{
	uint8_t pin;

	if (plan == NULL || geometry == NULL || trigger_pin >= plan->selected_pin_count ||
	    plan->lane_count == 0U || plan->lane_count > LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES) {
		return -EINVAL;
	}
	pin = plan->selected_pins[trigger_pin];
	for (uint8_t lane_index = 0U; lane_index < plan->lane_count; lane_index++) {
		const struct linkr_debugger_la_packed_ring_lane *lane = &plan->lanes[lane_index];
		uint8_t bit_index;
		uint32_t last_shift;

		if (pin < lane->pin_base || pin >= (uint8_t)(lane->pin_base + lane->pin_count)) {
			continue;
		}
		bit_index = (uint8_t)(pin - lane->pin_base);
		if (lane->sample_capacity == 0U || lane->word_count == 0U ||
		    lane->samples_per_word == 0U || lane->bits_per_sample == 0U ||
		    lane->autopush_bits == 0U || lane->autopush_bits > 32U ||
		    bit_index >= lane->bits_per_sample ||
		    lane->sample_capacity % lane->samples_per_word != 0U ||
		    lane->word_count != lane->sample_capacity / lane->samples_per_word) {
			return -EINVAL;
		}
		last_shift = (32U - lane->autopush_bits) + bit_index +
			((uint32_t)lane->samples_per_word - 1U) * lane->bits_per_sample;
		if (last_shift >= 32U) {
			return -EINVAL;
		}
		geometry->sample_capacity = lane->sample_capacity;
		geometry->word_count = lane->word_count;
		geometry->lane_index = lane_index;
		geometry->bits_per_sample = lane->bits_per_sample;
		geometry->samples_per_word = lane->samples_per_word;
		geometry->shift_base = (uint8_t)((32U - lane->autopush_bits) + bit_index);
		return 0;
	}

	return -EINVAL;
}

#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
static int la_packed_ring_trigger_level(
	const struct la_packed_ring_trigger_geometry *geometry,
	const uint32_t *lane_words,
	uint64_t sequence,
	uint8_t *level)
{
	uint32_t ring_sample;
	uint32_t word_index;
	uint8_t in_word;
	uint8_t shift;

	if (geometry == NULL || lane_words == NULL || level == NULL ||
	    geometry->sample_capacity == 0U || geometry->samples_per_word == 0U) {
		return -EINVAL;
	}
	ring_sample = (uint32_t)(sequence % geometry->sample_capacity);
	word_index = ring_sample / geometry->samples_per_word;
	in_word = (uint8_t)(ring_sample % geometry->samples_per_word);
	if (word_index >= geometry->word_count) {
		return -EINVAL;
	}
	shift = (uint8_t)(geometry->shift_base +
		((uint32_t)in_word * geometry->bits_per_sample));
	*level = (uint8_t)((lane_words[word_index] >> shift) & 1U);
	return 0;
}
#endif

static uint8_t la_packed_ring_first_set_bit(uint32_t bits)
{
	uint8_t bit = 0U;

	while ((bits & 1U) == 0U) {
		bits >>= 1U;
		bit++;
	}
	return bit;
}

static int la_packed_ring_trigger_scan(
	const struct la_packed_ring_trigger_geometry *geometry,
	const uint32_t *lane_words,
	enum linkr_debugger_la_trigger_type trigger,
	uint64_t first_sequence,
	uint64_t end_sequence,
	uint8_t previous_level,
	uint64_t *edge_sequence,
	uint8_t *last_level,
	bool *edge_found)
{
	uint64_t sequence = first_sequence;

	if (geometry == NULL || lane_words == NULL || edge_sequence == NULL ||
	    last_level == NULL || edge_found == NULL || first_sequence > end_sequence ||
	    trigger < LINKR_DEBUGGER_LA_TRIGGER_RISING ||
	    trigger > LINKR_DEBUGGER_LA_TRIGGER_EITHER) {
		return -EINVAL;
	}
	*edge_sequence = 0U;
	*last_level = previous_level;
	*edge_found = false;
	while (sequence < end_sequence) {
		uint32_t ring_sample = (uint32_t)(sequence % geometry->sample_capacity);
		uint32_t word_index = ring_sample / geometry->samples_per_word;
		uint8_t in_word = (uint8_t)(ring_sample % geometry->samples_per_word);
		uint32_t samples_in_word = geometry->samples_per_word - in_word;
		uint64_t remaining = end_sequence - sequence;
		uint32_t sample_count = remaining < samples_in_word ? (uint32_t)remaining :
			samples_in_word;
		uint32_t word;

		if (word_index >= geometry->word_count) {
			return -EINVAL;
		}
		word = lane_words[word_index];
		if (geometry->bits_per_sample == 1U && geometry->shift_base == 0U &&
		    geometry->samples_per_word == 32U) {
			uint32_t active_mask = sample_count == 32U ? UINT32_MAX :
				(((uint32_t)1U << sample_count) - 1U) << in_word;
			uint32_t first_mask = (uint32_t)1U << in_word;
			uint8_t first_level = (uint8_t)((word >> in_word) & 1U);
			uint32_t transitions;

			switch (trigger) {
			case LINKR_DEBUGGER_LA_TRIGGER_RISING:
				transitions = word & ~(word << 1U);
				break;
			case LINKR_DEBUGGER_LA_TRIGGER_FALLING:
				transitions = ~word & (word << 1U);
				break;
			case LINKR_DEBUGGER_LA_TRIGGER_EITHER:
				transitions = word ^ (word << 1U);
				break;
			default:
				return -EINVAL;
			}
			transitions &= ~first_mask;
			if (linkr_debugger_logic_analyzer_pre_trigger_edge_matches(trigger,
			    *last_level, first_level)) {
				transitions |= first_mask;
			}
			transitions &= active_mask;
			if (transitions != 0U) {
				uint8_t edge_bit = la_packed_ring_first_set_bit(transitions);

				*edge_sequence = sequence + edge_bit - in_word;
				*last_level = (uint8_t)((word >> edge_bit) & 1U);
				*edge_found = true;
				return 0;
			}
			*last_level = (uint8_t)((word >> (in_word + sample_count - 1U)) & 1U);
			sequence += sample_count;
			continue;
		}
		for (uint32_t index = 0U; index < sample_count; index++) {
			uint8_t shift = (uint8_t)(geometry->shift_base +
				((uint32_t)(in_word + index) * geometry->bits_per_sample));
			uint8_t current_level = (uint8_t)((word >> shift) & 1U);

			if (linkr_debugger_logic_analyzer_pre_trigger_edge_matches(trigger,
			    *last_level, current_level)) {
				*edge_sequence = sequence + index;
				*last_level = current_level;
				*edge_found = true;
				return 0;
			}
			*last_level = current_level;
		}
		sequence += sample_count;
	}

	return 0;
}

int linkr_debugger_logic_analyzer_pre_trigger_scan_packed_ring(
	const struct linkr_debugger_la_packed_ring_plan *plan,
	const uint32_t * const lane_words[],
	const uint32_t lane_word_counts[],
	uint8_t trigger_pin,
	enum linkr_debugger_la_trigger_type trigger,
	uint64_t first_sequence,
	uint64_t end_sequence,
	uint8_t previous_level,
	uint64_t *edge_sequence,
	uint8_t *last_level,
	bool *edge_found)
{
	struct la_packed_ring_trigger_geometry geometry;
	int ret;

	if (lane_words == NULL || lane_word_counts == NULL) {
		return -EINVAL;
	}
	ret = la_packed_ring_trigger_geometry_build(plan, trigger_pin, &geometry);
	if (ret < 0 || lane_words[geometry.lane_index] == NULL ||
	    lane_word_counts[geometry.lane_index] != geometry.word_count) {
		return ret < 0 ? ret : -EINVAL;
	}
	return la_packed_ring_trigger_scan(&geometry, lane_words[geometry.lane_index], trigger,
		first_sequence, end_sequence, previous_level, edge_sequence, last_level, edge_found);
}

int linkr_debugger_logic_analyzer_decode_packed_ring_span(
	const struct linkr_debugger_la_packed_ring_plan *plan,
	const uint32_t * const lane_words[],
	const uint32_t lane_word_counts[],
	uint64_t first_sample,
	uint8_t *packed_le,
	size_t packed_len,
	uint32_t sample_count)
{
	size_t needed_len;

	if (plan == NULL || lane_words == NULL || lane_word_counts == NULL || packed_le == NULL ||
	    sample_count == 0U || plan->lane_count == 0U ||
	    plan->lane_count > LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES ||
	    plan->bytes_per_sample == 0U || plan->bytes_per_sample > sizeof(uint16_t) ||
	    plan->sample_capacity == 0U) {
		return -EINVAL;
	}
	for (uint8_t lane = 0U; lane < plan->lane_count; lane++) {
		if (lane_words[lane] == NULL ||
		    lane_word_counts[lane] != plan->lanes[lane].word_count) {
			return -EINVAL;
		}
	}
#if SIZE_MAX < UINT32_MAX
	if (sample_count > SIZE_MAX / plan->bytes_per_sample) {
		return -EOVERFLOW;
	}
#endif
	needed_len = (size_t)sample_count * plan->bytes_per_sample;
	if (packed_len < needed_len) {
		return -EMSGSIZE;
	}

	uint64_t sequence = first_sample;

	for (uint32_t i = 0U; i < sample_count; i++) {
		uint16_t sample = la_decode_packed_ring_sample(plan, lane_words, sequence);
		uint8_t *dst = &packed_le[(size_t)i * plan->bytes_per_sample];

		la_write_sample_le(sample, plan->bytes_per_sample, dst);
		if (sequence == UINT64_MAX) {
			sequence = 0U;
		} else {
			sequence++;
		}
	}

	return 0;
}

int linkr_debugger_logic_analyzer_decode_wide11_burst(
	const uint32_t *lane_a_words,
	uint32_t lane_a_word_count,
	const uint32_t *lane_b_words,
	uint32_t lane_b_word_count,
	uint16_t *samples,
	uint32_t sample_count)
{
	struct linkr_debugger_la_wide11_burst_plan plan;
	int ret;

	if (lane_a_words == NULL || lane_b_words == NULL || samples == NULL) {
		return -EINVAL;
	}

	ret = linkr_debugger_logic_analyzer_wide11_burst_plan(sample_count, &plan);
	if (ret < 0) {
		return ret;
	}
	if (lane_a_word_count != plan.lane_a_word_count ||
	    lane_b_word_count != plan.lane_b_word_count) {
		return -EINVAL;
	}

	/*
	 * Reference full-buffer decoder matching the runtime span decoder. Lane A
	 * stores GP10..GP17 as four packed 8-bit samples per 32-bit word. Lane B
	 * stores GP18..GP20 as ten packed 3-bit samples in bits 2..31 of each word
	 * under the 30-bit autopush plan.
	 */
	for (uint32_t i = 0U; i < sample_count; i++) {
		samples[i] = la_decode_wide11_burst_sample(lane_a_words, lane_b_words, i);
	}

	return 0;
}

int linkr_debugger_logic_analyzer_decode_wide11_burst_span(
	const uint32_t *lane_a_words,
	uint32_t lane_a_word_count,
	const uint32_t *lane_b_words,
	uint32_t lane_b_word_count,
	uint32_t source_sample_count,
	uint32_t first_sample,
	uint8_t *packed_le,
	size_t packed_len,
	uint32_t sample_count)
{
	struct linkr_debugger_la_wide11_burst_plan plan;
	size_t needed_len;
	int ret;

	if (lane_a_words == NULL || lane_b_words == NULL || packed_le == NULL || sample_count == 0U) {
		return -EINVAL;
	}
	if (UINT32_MAX - first_sample < sample_count) {
		return -EOVERFLOW;
	}

	ret = linkr_debugger_logic_analyzer_wide11_burst_plan(source_sample_count, &plan);
	if (ret < 0) {
		return ret;
	}
	if (lane_a_word_count != plan.lane_a_word_count ||
	    lane_b_word_count != plan.lane_b_word_count ||
	    first_sample + sample_count > source_sample_count) {
		return -EINVAL;
	}
#if SIZE_MAX < UINT32_MAX
	if (sample_count > SIZE_MAX /
	    LINKR_DEBUGGER_LA_WIDE11_BURST_PACKED_SAMPLE_BYTES) {
		return -EOVERFLOW;
	}
#endif
	needed_len = (size_t)sample_count * LINKR_DEBUGGER_LA_WIDE11_BURST_PACKED_SAMPLE_BYTES;
	if (packed_len < needed_len) {
		return -EMSGSIZE;
	}

	for (uint32_t i = 0U; i < sample_count; i++) {
		uint16_t sample = la_decode_wide11_burst_sample(lane_a_words, lane_b_words,
			first_sample + i);

	packed_le[(size_t)i * 2U] = (uint8_t)(sample & 0xffU);
	packed_le[((size_t)i * 2U) + 1U] = (uint8_t)((sample >> 8) & 0xffU);
	}

	return 0;
}

uint8_t linkr_debugger_logic_analyzer_wide11_burst_completion_mask_update(
	uint32_t active_generation,
	uint32_t done_generation,
	uint8_t current_mask,
	uint8_t done_bit,
	bool *complete)
{
	const uint8_t both = LINKR_DEBUGGER_LA_WIDE11_BURST_DMA_A_DONE |
		LINKR_DEBUGGER_LA_WIDE11_BURST_DMA_B_DONE;
	uint8_t next = current_mask;

	if (complete != NULL) {
		*complete = false;
	}
	if (active_generation == 0U || done_generation != active_generation ||
	    (done_bit & (uint8_t)~both) != 0U || done_bit == 0U) {
		return next;
	}
	next |= done_bit;
	if (complete != NULL) {
		*complete = (next & both) == both;
	}
	return next;
}

uint8_t linkr_debugger_logic_analyzer_wide11_burst_configured_pin_count_model(
	const struct linkr_debugger_la_config *config)
{
	return linkr_debugger_logic_analyzer_wide11_burst_exact_eligible(config) ? 11U : 0U;
}
#if !defined(LINKR_DEBUGGER_LA_HOST_TEST)

static K_MUTEX_DEFINE(la_mutex);

static struct linkr_debugger_la_capture la_capture;
static bool la_initialized;

#if defined(CONFIG_SOC_SERIES_RP2350)
#define la_stream_packed_ring_words linkr_debugger_capture_arena_la_packed_ring()
#define la_samples \
	((struct linkr_debugger_la_sample *)linkr_debugger_capture_arena_la_finite_samples())
#define la_stream_scratch linkr_debugger_capture_arena_la_scratch()
#define la_stream_ring_value_scratch \
	((uint16_t *)linkr_debugger_capture_arena_la_finite_samples())
#define la_pre_trigger_ring linkr_debugger_capture_arena_la_pre_trigger()

BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_LA_PACKED_RING_BYTES ==
	LINKR_DEBUGGER_LA_RING_SAMPLES * sizeof(uint32_t),
	"LA stream packed ring arena slice size mismatch");
BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_LA_FINITE_SAMPLES_BYTES ==
	LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES * sizeof(struct linkr_debugger_la_sample),
	"LA finite exported samples arena slice size mismatch");
BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_LA_FINITE_SAMPLES_BYTES >=
	LINKR_DEBUGGER_LA_RING_HALF_SAMPLES * sizeof(uint16_t),
	"LA finite samples slice must cover packed ring callback scratch");
BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_LA_FINITE_SAMPLES_BYTES >=
	LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES * sizeof(uint16_t),
	"LA finite samples slice must cover packed ring stream values");
BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_LA_SCRATCH_BYTES ==
	LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES * sizeof(uint16_t),
	"LA stream scratch arena slice size mismatch");
BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_LA_PRE_TRIGGER_BYTES ==
	LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES * sizeof(uint16_t),
	"LA pre-trigger arena slice size mismatch");
BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_A_OFFSET == 0U,
	"Packed burst lane A must start at arena base");
BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_A_BYTES ==
	LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_LANE_A_BYTES,
	"Packed burst lane A byte count mismatch");
BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_B_OFFSET ==
	LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_A_BYTES,
	"Packed burst lane B offset must follow lane A");
BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_B_BYTES ==
	LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_LANE_B_BYTES,
	"Packed burst lane B byte count mismatch");
BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_BURST_SOURCE_CANARY_OFFSET ==
	LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_TOTAL_BYTES,
	"Packed burst source canary must follow capture lanes");
BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TOTAL_BYTES <=
	LINKR_DEBUGGER_CAPTURE_ARENA_BYTES,
	"Packed burst arena must fit inside shared arena");

static struct k_work la_packed_burst_work;
static volatile uint32_t la_generation;
static uint8_t la_configured_pins[LINKR_DEBUGGER_LA_MAX_CHANNELS];
static uint8_t la_configured_pin_count;

static size_t la_trigger_sm;
static uint16_t la_trigger_instructions[5];
static struct pio_program la_trigger_program = {
	.instructions = la_trigger_instructions,
	.origin = -1,
};
static bool la_trigger_sm_claimed;
static bool la_trigger_program_loaded;
static int la_trigger_offset = -1;

static struct dma_config la_packed_dma_a_config;
static struct dma_config la_packed_dma_b_config;
static struct dma_block_config la_packed_dma_a_block;
static struct dma_block_config la_packed_dma_b_block;
static uint16_t la_packed_sm_a_instructions[2];
static uint16_t la_packed_sm_b_instructions[2];
static struct pio_program la_packed_sm_a_program = {
	.instructions = la_packed_sm_a_instructions,
	.origin = -1,
};
static struct pio_program la_packed_sm_b_program = {
	.instructions = la_packed_sm_b_instructions,
	.origin = -1,
};
static size_t la_packed_sm_a;
static size_t la_packed_sm_b;
static int la_packed_sm_a_offset = -1;
static int la_packed_sm_b_offset = -1;
static int la_packed_dma_a_channel = -1;
static int la_packed_dma_b_channel = -1;
static bool la_packed_sm_a_claimed;
static bool la_packed_sm_b_claimed;
static bool la_packed_sm_a_program_loaded;
static bool la_packed_sm_b_program_loaded;
static bool la_packed_burst_active;
static bool la_packed_burst_hw_configured;
static bool la_packed_burst_triggered;
static struct linkr_debugger_la_packed_burst_plan la_packed_burst_plan_active;
static volatile uint32_t la_packed_burst_done_generation;
static volatile uint8_t la_packed_burst_done_mask;
static volatile int la_packed_burst_dma_status;
static struct linkr_debugger_la_stream_sink la_packed_burst_sink;
static bool la_packed_burst_sink_bound;
static struct linkr_debugger_capture_arena_lease la_packed_burst_arena_lease;
static bool la_packed_burst_arena_held;

static volatile bool la_stream_active;
static volatile uint32_t la_stream_sequence;
static linkr_debugger_la_stream_callback_t la_stream_callback;
static void *la_stream_user_data;
static struct linkr_debugger_la_stream_sink la_stream_sink;
static bool la_stream_sink_active;
static struct linkr_debugger_la_config la_stream_config;

#define la_stream_ring_values la_stream_ring_value_scratch
static struct linkr_debugger_la_ring_progress la_stream_ring_progress;
static struct linkr_debugger_la_ring_metrics la_stream_ring_metrics;
static struct k_spinlock la_stream_ring_progress_lock;
static struct linkr_debugger_la_packed_ring_plan la_stream_ring_plan;
static uint32_t la_stream_ring_lane_last_hw_index[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES];
static uint64_t la_stream_ring_lane_writer_seq[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES];
static bool la_stream_ring_active;
static uint32_t la_stream_ring_error_count;
static uint32_t la_stream_ring_emitted_samples;
static bool la_stream_ring_terminal_pending;
static bool la_stream_ring_terminal_emitted;
static bool la_stream_ring_acquisition_frozen;
static enum linkr_debugger_la_ring_poll_result la_stream_ring_terminal_status;
struct la_stream_pre_trigger_state {
	bool enabled;
	bool scan_initialized;
	bool edge_found;
	uint8_t previous_level;
	uint64_t scan_sequence;
	uint64_t edge_sequence;
	struct la_packed_ring_trigger_geometry trigger_geometry;
	struct linkr_debugger_la_pre_trigger_window window;
};
static struct la_stream_pre_trigger_state la_stream_pre_trigger;
/*
 * Stream lifecycle invariants:
 * - la_mutex owns configuration, DMA/PIO cleanup, callback pointer replacement,
 *   active flags, and la_generation changes.
 * - The ring producer thread is the only owner of DMA/trigger polling and
 *   writer progress. The lower-priority consumer compresses/emits chunks and
 *   invokes callbacks. Neither worker holds la_mutex or the progress lock while
 *   invoking a callback.
 * - stop/cancel/release bump la_generation, mark inactive, wake the producer,
 *   wait for the idle semaphore without la_mutex, then clear DMA/ring/callback
 *   state. A callback is allowed only when active && generation still matches,
 *   and that condition is checked again after every callback returns.
 * - start waits for any busy old producer before resetting idle_sem or
 *   configuring a new generation, then revalidates its start preconditions.
 *   Only one start caller may wait for that old idle acknowledgement; other
 *   concurrent start callers fail with -EBUSY instead of racing for one token.
 */
static K_THREAD_STACK_DEFINE(la_stream_ring_thread_stack,
	LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE);
static K_THREAD_STACK_DEFINE(la_stream_ring_consumer_thread_stack,
	LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE);
static struct k_thread la_stream_ring_thread;
static struct k_thread la_stream_ring_consumer_thread;
static struct k_sem la_stream_ring_wake_sem;
static struct k_sem la_stream_ring_consumer_wake_sem;
static struct k_sem la_stream_ring_idle_sem;
static struct k_sem la_stream_ring_consumer_idle_sem;
static bool la_stream_ring_thread_started;
static bool la_stream_ring_consumer_thread_started;
static bool la_stream_ring_thread_busy;
static bool la_stream_ring_consumer_thread_busy;
static bool la_stream_ring_start_waiter;

#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST) && \
	defined(DMA_CH0_TRANS_COUNT_MODE_VALUE_ENDLESS) && \
	defined(DMA_CH0_TRANS_COUNT_MODE_LSB)
#define LINKR_DEBUGGER_LA_HAS_RING_DMA_ENDLESS 1

static uint32_t la_ring_transfer_count(void)
{
	return dma_encode_endless_transfer_count();
}
#endif

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
static volatile bool la_stream_triggered;
static volatile uint16_t la_stream_values_or;
static volatile uint16_t la_stream_values_and;

static int la_arm_pre_trigger_locked(const struct linkr_debugger_la_config *config);
static void la_cleanup_locked(void);
static void la_packed_burst_cleanup_locked(void);
static uint16_t la_pio_in_pins_instruction(uint8_t bits)
{
	return bits == 32U ? LINKR_DEBUGGER_LA_PIO_IN_PINS_32 :
		(uint16_t)(0x4000U | bits);
}
static void la_stream_ring_thread_fn(void *p1, void *p2, void *p3);
static void la_stream_ring_consumer_thread_fn(void *p1, void *p2, void *p3);
static void la_packed_burst_work_handler(struct k_work *work);
static int la_start_stream_ring_dma_locked(void);
static int la_start_stream_common(
	const struct linkr_debugger_la_config *config,
	linkr_debugger_la_stream_callback_t callback,
	void *user_data,
	const struct linkr_debugger_la_stream_sink *sink);

static void la_stream_teardown_locked(void)
{
	la_stream_active = false;
	la_stream_ring_active = false;
	la_stream_ring_acquisition_frozen = false;
	la_pre_trigger_active = false;
	la_pre_trigger_triggered = false;
	la_pre_trigger_have_prev = false;
	memset(&la_stream_pre_trigger, 0, sizeof(la_stream_pre_trigger));
}

static void la_stream_clear_callback_locked(void)
{
	la_stream_callback = NULL;
	la_stream_user_data = NULL;
	memset(&la_stream_sink, 0, sizeof(la_stream_sink));
	la_stream_sink_active = false;
}

static void la_stream_request_inactive_locked(void)
{
	la_generation++;
	la_stream_teardown_locked();
	if (la_stream_ring_thread_started) {
		k_sem_give(&la_stream_ring_wake_sem);
	}
	if (la_stream_ring_consumer_thread_started) {
		k_sem_give(&la_stream_ring_consumer_wake_sem);
	}
}

static void la_stream_ring_signal_idle_locked(void)
{
	la_stream_ring_thread_busy = false;
	k_sem_give(&la_stream_ring_idle_sem);
}

static void la_stream_ring_consumer_signal_idle_locked(void)
{
	la_stream_ring_consumer_thread_busy = false;
	k_sem_give(&la_stream_ring_consumer_idle_sem);
}

static void la_stream_ring_consumer_set_priority(bool sink_session)
{
	if (la_stream_ring_consumer_thread_started) {
		k_thread_priority_set(&la_stream_ring_consumer_thread,
			linkr_debugger_logic_analyzer_stream_consumer_priority(sink_session));
	}
}

#define LA_STREAM_RING_IDLE_WAIT_TIMEOUT_MS 2000U

static void la_stream_ring_wait_idle_if_needed(void)
{
	uint8_t wait_mask;

	wait_mask = linkr_debugger_logic_analyzer_stream_idle_wait_mask(
		la_stream_ring_thread_started && k_current_get() != &la_stream_ring_thread,
		la_stream_ring_thread_busy,
		la_stream_ring_consumer_thread_started &&
			k_current_get() != &la_stream_ring_consumer_thread,
		la_stream_ring_consumer_thread_busy);
	/* The idle wait must stay bounded: a stuck producer/consumer must not turn
	 * a capture error into a permanent thread block.  The generation bump in
	 * la_stream_request_inactive_locked() already detached the old capture and
	 * la_cleanup_locked() tears the hardware down regardless, so timing out and
	 * continuing is safe and keeps the system recoverable.
	 */
	if ((wait_mask & LINKR_DEBUGGER_LA_STREAM_WAIT_PRODUCER) != 0U &&
	    k_sem_take(&la_stream_ring_idle_sem,
		K_MSEC(LA_STREAM_RING_IDLE_WAIT_TIMEOUT_MS)) != 0) {
		LOG_ERR("la ring producer idle wait timed out; forcing recovery");
		printk("la ring producer idle wait timeout; forcing recovery\n");
	}
	if ((wait_mask & LINKR_DEBUGGER_LA_STREAM_WAIT_CONSUMER) != 0U &&
	    k_sem_take(&la_stream_ring_consumer_idle_sem,
		K_MSEC(LA_STREAM_RING_IDLE_WAIT_TIMEOUT_MS)) != 0) {
		LOG_ERR("la ring consumer idle wait timed out; forcing recovery");
		printk("la ring consumer idle wait timeout; forcing recovery\n");
	}
}

static void la_stream_stop_and_cleanup(enum linkr_debugger_la_state state)
{
	k_mutex_lock(&la_mutex, K_FOREVER);
	la_stream_request_inactive_locked();
	k_mutex_unlock(&la_mutex);

	la_stream_ring_wait_idle_if_needed();
	la_stream_ring_consumer_set_priority(false);

	k_mutex_lock(&la_mutex, K_FOREVER);
	la_cleanup_locked();
	la_stream_clear_callback_locked();
	la_capture.state = state;
	la_capture.samples = NULL;
	k_mutex_unlock(&la_mutex);
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
	la_packed_burst_cleanup_locked();
	pio = pio_rpi_pico_get_pio(la_pio_dev);
	pio_interrupt_clear(pio, 0U);
	if (la_trigger_sm_claimed) {
		pio_sm_set_enabled(pio, (uint)la_trigger_sm, false);
		pio_interrupt_clear(pio, 0U);
	}
	if (la_trigger_program_loaded) {
		pio_remove_program(pio, &la_trigger_program, (uint)la_trigger_offset);
		la_trigger_program_loaded = false;
		la_trigger_offset = -1;
	}
	if (la_trigger_sm_claimed) {
		pio_sm_unclaim(pio, (uint)la_trigger_sm);
		la_trigger_sm_claimed = false;
	}
	la_restore_configured_pins();
	la_stream_ring_active = false;
}

static void la_packed_burst_release_dma_locked(int *channel)
{
	if (channel == NULL || *channel < 0 || !device_is_ready(la_dma_dev)) {
		return;
	}
	dma_channel_set_irq0_enabled((uint)*channel, false);
#if defined(DMA_CH0_CTRL_TRIG_EN_BITS)
	hw_clear_bits(&dma_hw->ch[*channel].ctrl_trig, DMA_CH0_CTRL_TRIG_EN_BITS);
#endif
	dma_channel_abort((uint)*channel);
	dma_hw->ints0 = BIT(*channel);
	dma_release_channel(la_dma_dev, (uint32_t)*channel);
	*channel = -1;
}

static void la_packed_burst_cleanup_locked(void)
{
	PIO pio;

	if (!device_is_ready(la_pio_dev)) {
		return;
	}
	pio = pio_rpi_pico_get_pio(la_pio_dev);
	if (la_packed_sm_a_claimed) {
		pio_sm_set_enabled(pio, (uint)la_packed_sm_a, false);
		pio_sm_clear_fifos(pio, (uint)la_packed_sm_a);
	}
	if (la_packed_sm_b_claimed) {
		pio_sm_set_enabled(pio, (uint)la_packed_sm_b, false);
		pio_sm_clear_fifos(pio, (uint)la_packed_sm_b);
	}
	if (la_trigger_sm_claimed) {
		pio_sm_set_enabled(pio, (uint)la_trigger_sm, false);
		pio_interrupt_clear(pio, 0U);
	}
	la_packed_burst_release_dma_locked(&la_packed_dma_a_channel);
	la_packed_burst_release_dma_locked(&la_packed_dma_b_channel);
	if (la_packed_sm_a_program_loaded && la_packed_sm_a_offset >= 0) {
		pio_remove_program(pio, &la_packed_sm_a_program, (uint)la_packed_sm_a_offset);
		la_packed_sm_a_program_loaded = false;
		la_packed_sm_a_offset = -1;
	}
	if (la_packed_sm_b_program_loaded && la_packed_sm_b_offset >= 0) {
		pio_remove_program(pio, &la_packed_sm_b_program, (uint)la_packed_sm_b_offset);
		la_packed_sm_b_program_loaded = false;
		la_packed_sm_b_offset = -1;
	}
	if (la_packed_sm_a_claimed) {
		pio_sm_unclaim(pio, (uint)la_packed_sm_a);
		la_packed_sm_a_claimed = false;
	}
	if (la_packed_sm_b_claimed) {
		pio_sm_unclaim(pio, (uint)la_packed_sm_b);
		la_packed_sm_b_claimed = false;
	}
	if (la_packed_burst_arena_held) {
		(void)linkr_debugger_capture_arena_release(&la_packed_burst_arena_lease);
		la_packed_burst_arena_held = false;
		memset(&la_packed_burst_arena_lease, 0, sizeof(la_packed_burst_arena_lease));
	}
	memset(&la_packed_burst_sink, 0, sizeof(la_packed_burst_sink));
	la_packed_burst_sink_bound = false;
	la_packed_burst_active = false;
	la_packed_burst_hw_configured = false;
	la_packed_burst_triggered = false;
	la_packed_burst_done_mask = 0U;
	la_packed_burst_dma_status = 0;
}

static void la_packed_burst_dma_callback(const struct device *dev, void *user_data,
	uint32_t channel, int status)
{
	uint32_t done_generation = (uint32_t)(uintptr_t)user_data;
	uint8_t done_bit = 0U;
	uint8_t expected_done = LINKR_DEBUGGER_LA_WIDE11_BURST_DMA_A_DONE;

	ARG_UNUSED(dev);

	if (done_generation != la_generation) {
		return;
	}
	if ((int)channel == la_packed_dma_a_channel) {
		done_bit = LINKR_DEBUGGER_LA_WIDE11_BURST_DMA_A_DONE;
	} else if ((int)channel == la_packed_dma_b_channel) {
		done_bit = LINKR_DEBUGGER_LA_WIDE11_BURST_DMA_B_DONE;
	} else {
		return;
	}
	la_packed_burst_done_generation = done_generation;
	if (status < 0 && la_packed_burst_dma_status == 0) {
		la_packed_burst_dma_status = status;
	}
	bool complete;

	if (la_packed_burst_plan_active.lane_count > 1U) {
		expected_done |= LINKR_DEBUGGER_LA_WIDE11_BURST_DMA_B_DONE;
	}
	if ((done_bit & expected_done) == 0U) {
		return;
	}
	la_packed_burst_done_mask |= done_bit;
	complete = (la_packed_burst_done_mask & expected_done) == expected_done;
	if (complete) {
		(void)k_work_submit(&la_packed_burst_work);
	}
}

static enum linkr_debugger_la_ring_poll_result la_packed_burst_emit_sink(
	const struct linkr_debugger_la_stream_sink *sink)
{
	const uint32_t *lane_words[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {
		(const uint32_t *)linkr_debugger_capture_arena_burst_lane_a(),
		(const uint32_t *)linkr_debugger_capture_arena_burst_lane_b(),
	};
	uint32_t lane_word_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	const struct linkr_debugger_la_packed_burst_plan *plan = &la_packed_burst_plan_active;
	uint32_t emitted = 0U;
	uint32_t sequence = 0U;

	for (uint8_t lane = 0U; lane < plan->lane_count; lane++) {
		lane_word_counts[lane] = plan->lanes[lane].word_count;
	}
	while (emitted < plan->emitted_sample_count) {
		uint32_t chunk = plan->emitted_sample_count - emitted;
		struct linkr_debugger_la_stream_sink_lease lease;
		struct linkr_debugger_la_stream_sink_commit commit;
		int ret;

		uint32_t max_chunk = la_packed_burst_max_chunk_samples(plan->bytes_per_sample);

		if (max_chunk == 0U) {
			return LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN;
		}
		if (chunk > max_chunk) {
			chunk = max_chunk;
		}
		ret = linkr_debugger_logic_analyzer_stream_sink_lease_payload(sink,
			chunk, &lease);
		if (ret < 0) {
			return LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN;
		}
		ret = linkr_debugger_logic_analyzer_decode_packed_burst_span(plan,
			lane_words, lane_word_counts, emitted, lease.payload, lease.capacity,
			chunk);
		if (ret < 0) {
			linkr_debugger_logic_analyzer_stream_sink_abort_payload(sink, &lease);
			return LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN;
		}
		memset(&commit, 0, sizeof(commit));
		commit.token = lease.token;
		commit.sequence = sequence;
		commit.sample_count = chunk;
		commit.timestamp_us = (uint64_t)emitted * la_capture.sample_period_ps / 1000000ULL;
		commit.bytes_per_sample = plan->bytes_per_sample;
		commit.payload_len = (size_t)chunk * plan->bytes_per_sample;
		ret = linkr_debugger_logic_analyzer_stream_sink_commit_payload(sink, &commit);
		if (ret < 0) {
			linkr_debugger_logic_analyzer_stream_sink_abort_payload(sink, &lease);
			return LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN;
		}
		emitted += chunk;
		sequence++;
	}

	return LINKR_DEBUGGER_LA_RING_POLL_OK;
}

static int la_decode_packed_snapshot_into_capture(void)
{
	const struct linkr_debugger_la_packed_burst_plan *plan = &la_packed_burst_plan_active;
	const uint32_t *lane_words[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {
		(const uint32_t *)linkr_debugger_capture_arena_burst_lane_a(),
		(const uint32_t *)linkr_debugger_capture_arena_burst_lane_b(),
	};
	uint32_t lane_word_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint8_t *packed = linkr_debugger_capture_arena_burst_tx_slot(0U);
	uint64_t period_ps = la_capture.sample_period_ps;
	uint32_t sample_count = plan->requested_sample_count;
	int ret;

	if (packed == NULL || sample_count == 0U ||
	    sample_count > LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES) {
		return -EINVAL;
	}
	for (uint8_t lane = 0U; lane < plan->lane_count; lane++) {
		lane_word_counts[lane] = plan->lanes[lane].word_count;
	}
	ret = linkr_debugger_logic_analyzer_decode_packed_burst_span(plan, lane_words,
		lane_word_counts, 0U, packed,
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TX_SLOT_BYTES, sample_count);
	if (ret < 0) {
		return ret;
	}
	for (uint32_t i = 0U; i < sample_count; i++) {
		uint16_t values = packed[(size_t)i * plan->bytes_per_sample];

		if (plan->bytes_per_sample > 1U) {
			values |= (uint16_t)packed[((size_t)i * plan->bytes_per_sample) + 1U] << 8U;
		}
		la_samples[i].timestamp_us = (uint32_t)((period_ps * i) / 1000000ULL);
		la_samples[i].values = values;
		la_samples[i].reserved = 0U;
	}
	la_capture.sample_count = sample_count;
	la_capture.samples = la_samples;
	return 0;
}

static void la_packed_burst_work_handler(struct k_work *work)
{
	struct linkr_debugger_la_stream_sink sink;
	enum linkr_debugger_la_ring_poll_result status;
	struct linkr_debugger_capture_arena_lease lease;
	bool have_sink;
	bool have_lease;

	ARG_UNUSED(work);

	k_mutex_lock(&la_mutex, K_FOREVER);
	if (!la_packed_burst_active || la_packed_burst_done_generation != la_generation) {
		k_mutex_unlock(&la_mutex);
		return;
	}
	if (la_packed_sm_a_claimed) {
		pio_sm_set_enabled(pio_rpi_pico_get_pio(la_pio_dev), (uint)la_packed_sm_a, false);
	}
	if (la_packed_sm_b_claimed) {
		pio_sm_set_enabled(pio_rpi_pico_get_pio(la_pio_dev), (uint)la_packed_sm_b, false);
	}
	if (la_trigger_sm_claimed) {
		pio_sm_set_enabled(pio_rpi_pico_get_pio(la_pio_dev), (uint)la_trigger_sm, false);
	}
	status = la_packed_burst_dma_status == 0 && linkr_debugger_capture_arena_canaries_ok() ?
		LINKR_DEBUGGER_LA_RING_POLL_OK : LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN;
	have_sink = la_packed_burst_sink_bound;
	sink = la_packed_burst_sink;
	have_lease = la_packed_burst_arena_held;
	lease = la_packed_burst_arena_lease;
	if (have_lease && status == LINKR_DEBUGGER_LA_RING_POLL_OK &&
	    linkr_debugger_capture_arena_mark_postprocess(&lease) < 0) {
		status = LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN;
	}
	la_capture.state = status == LINKR_DEBUGGER_LA_RING_POLL_OK ?
		LINKR_DEBUGGER_LA_STATE_DONE : LINKR_DEBUGGER_LA_STATE_ERROR;
	la_packed_burst_active = false;
	k_mutex_unlock(&la_mutex);

	if (have_sink && status == LINKR_DEBUGGER_LA_RING_POLL_OK) {
		status = la_packed_burst_emit_sink(&sink);
	} else if (!have_sink && status == LINKR_DEBUGGER_LA_RING_POLL_OK) {
		if (la_decode_packed_snapshot_into_capture() < 0) {
			status = LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN;
		}
	}
	if (have_sink) {
		linkr_debugger_logic_analyzer_stream_sink_notify_terminal(&sink, status, 0U);
	}
	if (have_lease) {
		if (have_sink && status == LINKR_DEBUGGER_LA_RING_POLL_OK) {
			(void)linkr_debugger_capture_arena_mark_network_send(&lease);
		}
		if (!have_sink) {
			k_mutex_lock(&la_mutex, K_FOREVER);
			la_capture.state = status == LINKR_DEBUGGER_LA_RING_POLL_OK ?
				LINKR_DEBUGGER_LA_STATE_DONE : LINKR_DEBUGGER_LA_STATE_ERROR;
			la_cleanup_locked();
			k_mutex_unlock(&la_mutex);
		} else {
			/* Arena release is deferred to stop_stream(), which WS invokes only after
			 * the burst data and terminal slots have drained.
			 */
		}
	} else if (!have_sink) {
		k_mutex_lock(&la_mutex, K_FOREVER);
		la_capture.state = status == LINKR_DEBUGGER_LA_RING_POLL_OK ?
			LINKR_DEBUGGER_LA_STATE_DONE : LINKR_DEBUGGER_LA_STATE_ERROR;
		la_cleanup_locked();
		k_mutex_unlock(&la_mutex);
	}
}

static uint8_t la_trigger_program_length(enum linkr_debugger_la_trigger_type trigger)
{
	if (trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE) {
		return 0U;
	}

	return 3U;
}

static int la_packed_find_program_offset(PIO pio,
	const struct pio_program *program)
{
	for (uint8_t offset = 0U; offset <= 32U - program->length; offset++) {
		if (pio_can_add_program_at_offset(pio, program, offset)) {
			return offset;
		}
	}
	return -EBUSY;
}

static int la_packed_burst_configure_trigger_locked(
	const struct linkr_debugger_la_config *config,
	uint32_t div_int,
	uint8_t div_frac)
{
	uint8_t trig_pin = la_pin_at(config, config->trigger_pin);
	enum linkr_debugger_la_trigger_type effective_trigger = config->trigger;
	pio_sm_config trigger_cfg;
	PIO pio = pio_rpi_pico_get_pio(la_pio_dev);
	int ret;

	if (config->trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE) {
		return 0;
	}
	if (config->trigger == LINKR_DEBUGGER_LA_TRIGGER_EITHER) {
		effective_trigger = linkr_debugger_logic_analyzer_effective_trigger(
			config->trigger, gpio_get(trig_pin));
	}
	if (effective_trigger == LINKR_DEBUGGER_LA_TRIGGER_RISING) {
		la_trigger_instructions[0] = (uint16_t)pio_encode_wait_gpio(false, trig_pin);
		la_trigger_instructions[1] = (uint16_t)pio_encode_wait_gpio(true, trig_pin);
	} else if (effective_trigger == LINKR_DEBUGGER_LA_TRIGGER_FALLING) {
		la_trigger_instructions[0] = (uint16_t)pio_encode_wait_gpio(true, trig_pin);
		la_trigger_instructions[1] = (uint16_t)pio_encode_wait_gpio(false, trig_pin);
	} else {
		return -EINVAL;
	}
	la_trigger_instructions[2] = (uint16_t)pio_encode_irq_set(false, 0U);
	la_trigger_program.length = la_trigger_program_length(config->trigger);

	ret = pio_rpi_pico_allocate_sm(la_pio_dev, &la_trigger_sm);
	if (ret < 0) {
		return ret;
	}
	la_trigger_sm_claimed = true;
	la_trigger_offset = la_packed_find_program_offset(pio, &la_trigger_program);
	if (la_trigger_offset < 0) {
		return la_trigger_offset;
	}
	ret = pio_add_program_at_offset(pio, &la_trigger_program,
		(uint)la_trigger_offset);
	if (ret < 0) {
		return ret;
	}
	la_trigger_offset = ret;
	la_trigger_program_loaded = true;
	trigger_cfg = pio_get_default_sm_config();
	sm_config_set_clkdiv_int_frac8(&trigger_cfg, div_int, div_frac);
	sm_config_set_wrap(&trigger_cfg,
		(uint)la_trigger_offset + la_trigger_program.length - 1U,
		(uint)la_trigger_offset + la_trigger_program.length - 1U);
	pio_interrupt_clear(pio, 0U);
	ret = pio_sm_init(pio, (uint)la_trigger_sm, (uint)la_trigger_offset,
		&trigger_cfg);
	if (ret < 0) {
		return ret;
	}
	pio_sm_clear_fifos(pio, (uint)la_trigger_sm);
	pio_sm_restart(pio, (uint)la_trigger_sm);
	return 0;
}

static int la_packed_burst_configure_dma_locked(void)
{
	PIO pio = pio_rpi_pico_get_pio(la_pio_dev);
	int ret;

	la_packed_dma_a_channel = dma_request_channel(la_dma_dev, NULL);
	if (la_packed_dma_a_channel < 0) {
		return la_packed_dma_a_channel;
	}
	if (la_packed_burst_plan_active.lane_count > 1U) {
		la_packed_dma_b_channel = dma_request_channel(la_dma_dev, NULL);
		if (la_packed_dma_b_channel < 0) {
			ret = la_packed_dma_b_channel;
			la_packed_burst_release_dma_locked(&la_packed_dma_a_channel);
			return ret;
		}
	}

	memset(&la_packed_dma_a_config, 0, sizeof(la_packed_dma_a_config));
	memset(&la_packed_dma_b_config, 0, sizeof(la_packed_dma_b_config));
	memset(&la_packed_dma_a_block, 0, sizeof(la_packed_dma_a_block));
	memset(&la_packed_dma_b_block, 0, sizeof(la_packed_dma_b_block));

	la_packed_dma_a_block.source_address = (uint32_t)&pio->rxf[la_packed_sm_a];
	la_packed_dma_a_block.dest_address = (uint32_t)linkr_debugger_capture_arena_burst_lane_a();
	la_packed_dma_a_block.block_size = la_packed_burst_plan_active.lanes[0].byte_count;
	la_packed_dma_a_block.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	la_packed_dma_a_block.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	la_packed_dma_a_config.dma_slot = RPI_PICO_DMA_DREQ_TO_SLOT(
		pio_get_dreq(pio, (uint)la_packed_sm_a, false));
	la_packed_dma_a_config.channel_direction = PERIPHERAL_TO_MEMORY;
	la_packed_dma_a_config.source_data_size = sizeof(uint32_t);
	la_packed_dma_a_config.dest_data_size = sizeof(uint32_t);
	la_packed_dma_a_config.source_burst_length = 1U;
	la_packed_dma_a_config.dest_burst_length = 1U;
	la_packed_dma_a_config.block_count = 1U;
	la_packed_dma_a_config.head_block = &la_packed_dma_a_block;
	la_packed_dma_a_config.user_data = (void *)(uintptr_t)la_generation;
	la_packed_dma_a_config.dma_callback = la_packed_burst_dma_callback;
	la_packed_dma_a_config.complete_callback_en = 1U;

	if (la_packed_burst_plan_active.lane_count > 1U) {
		la_packed_dma_b_block.source_address = (uint32_t)&pio->rxf[la_packed_sm_b];
		la_packed_dma_b_block.dest_address = (uint32_t)linkr_debugger_capture_arena_burst_lane_b();
		la_packed_dma_b_block.block_size = la_packed_burst_plan_active.lanes[1].byte_count;
		la_packed_dma_b_block.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
		la_packed_dma_b_block.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
		la_packed_dma_b_config.dma_slot = RPI_PICO_DMA_DREQ_TO_SLOT(
			pio_get_dreq(pio, (uint)la_packed_sm_b, false));
		la_packed_dma_b_config.channel_direction = PERIPHERAL_TO_MEMORY;
		la_packed_dma_b_config.source_data_size = sizeof(uint32_t);
		la_packed_dma_b_config.dest_data_size = sizeof(uint32_t);
		la_packed_dma_b_config.source_burst_length = 1U;
		la_packed_dma_b_config.dest_burst_length = 1U;
		la_packed_dma_b_config.block_count = 1U;
		la_packed_dma_b_config.head_block = &la_packed_dma_b_block;
		la_packed_dma_b_config.user_data = (void *)(uintptr_t)la_generation;
		la_packed_dma_b_config.dma_callback = la_packed_burst_dma_callback;
		la_packed_dma_b_config.complete_callback_en = 1U;
	}

	ret = dma_config(la_dma_dev, (uint32_t)la_packed_dma_a_channel,
		&la_packed_dma_a_config);
	if (ret < 0) {
		return ret;
	}
	if (la_packed_burst_plan_active.lane_count > 1U) {
		ret = dma_config(la_dma_dev, (uint32_t)la_packed_dma_b_channel,
			&la_packed_dma_b_config);
		if (ret < 0) {
			return ret;
		}
	}
	ret = dma_start(la_dma_dev, (uint32_t)la_packed_dma_a_channel);
	if (ret < 0) {
		return ret;
	}
	if (la_packed_burst_plan_active.lane_count > 1U) {
		ret = dma_start(la_dma_dev, (uint32_t)la_packed_dma_b_channel);
		if (ret < 0) {
			return ret;
		}
	}
	return 0;
}

static int la_packed_burst_configure_locked(
	const struct linkr_debugger_la_start_prepare *prepare)
{
	const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	PIO pio;
	uint64_t div256;
	pio_sm_config sm_a_cfg;
	pio_sm_config sm_b_cfg;
	uint8_t sm_a_len;
	uint8_t sm_b_len;
	const struct linkr_debugger_la_packed_burst_plan *plan = &la_packed_burst_plan_active;
	int ret;

	if (!device_is_ready(gpio_dev) || !device_is_ready(la_pio_dev) || !device_is_ready(la_dma_dev)) {
		return -ENODEV;
	}
	pio = pio_rpi_pico_get_pio(la_pio_dev);
	ret = pio_rpi_pico_allocate_sm(la_pio_dev, &la_packed_sm_a);
	if (ret < 0) {
		return ret;
	}
	la_packed_sm_a_claimed = true;
	if (plan->lane_count > 1U) {
		ret = pio_rpi_pico_allocate_sm(la_pio_dev, &la_packed_sm_b);
		if (ret < 0) {
			return ret;
		}
		la_packed_sm_b_claimed = true;
	}

	la_packed_burst_triggered = prepare->config.trigger != LINKR_DEBUGGER_LA_TRIGGER_NONE;
	if (la_packed_burst_triggered) {
		la_packed_sm_a_instructions[0] = LINKR_DEBUGGER_LA_PIO_WAIT_IRQ0;
		la_packed_sm_a_instructions[1] = la_pio_in_pins_instruction(
			plan->lanes[0].bits_per_sample);
		sm_a_len = 2U;
		if (plan->lane_count > 1U) {
			la_packed_sm_b_instructions[0] = LINKR_DEBUGGER_LA_PIO_WAIT_IRQ0;
			la_packed_sm_b_instructions[1] = la_pio_in_pins_instruction(
				plan->lanes[1].bits_per_sample);
			sm_b_len = 2U;
		} else {
			sm_b_len = 0U;
		}
	} else {
		la_packed_sm_a_instructions[0] = la_pio_in_pins_instruction(
			plan->lanes[0].bits_per_sample);
		sm_a_len = 1U;
		if (plan->lane_count > 1U) {
			la_packed_sm_b_instructions[0] = la_pio_in_pins_instruction(
				plan->lanes[1].bits_per_sample);
			sm_b_len = 1U;
		} else {
			sm_b_len = 0U;
		}
	}
	la_packed_sm_a_program.length = sm_a_len;
	if (plan->lane_count > 1U) {
		la_packed_sm_b_program.length = sm_b_len;
	}
	la_packed_sm_a_offset = la_packed_find_program_offset(pio, &la_packed_sm_a_program);
	if (la_packed_sm_a_offset < 0) {
		return la_packed_sm_a_offset;
	}
	ret = pio_add_program_at_offset(pio, &la_packed_sm_a_program,
		(uint)la_packed_sm_a_offset);
	if (ret < 0) {
		return ret;
	}
	la_packed_sm_a_offset = ret;
	la_packed_sm_a_program_loaded = true;
	if (plan->lane_count > 1U) {
		la_packed_sm_b_offset = la_packed_find_program_offset(pio, &la_packed_sm_b_program);
		if (la_packed_sm_b_offset < 0) {
			return la_packed_sm_b_offset;
		}
		ret = pio_add_program_at_offset(pio, &la_packed_sm_b_program,
			(uint)la_packed_sm_b_offset);
		if (ret < 0) {
			return ret;
		}
		la_packed_sm_b_offset = ret;
		la_packed_sm_b_program_loaded = true;
	}

	for (uint8_t pin = plan->lanes[0].pin_base;
	     pin < (uint8_t)(plan->lanes[0].pin_base + plan->lanes[0].pin_count); pin++) {
		ret = gpio_pin_configure(gpio_dev, pin, GPIO_INPUT);
		if (ret < 0) {
			return ret;
		}
		pio_gpio_init(pio, pin);
		pio_sm_set_consecutive_pindirs(pio, (uint)la_packed_sm_a, pin, 1U, false);
		la_configured_pins[la_configured_pin_count++] = pin;
	}
	if (plan->lane_count > 1U) {
		for (uint8_t pin = plan->lanes[1].pin_base;
		     pin < (uint8_t)(plan->lanes[1].pin_base + plan->lanes[1].pin_count); pin++) {
			ret = gpio_pin_configure(gpio_dev, pin, GPIO_INPUT);
			if (ret < 0) {
				return ret;
			}
			pio_gpio_init(pio, pin);
			pio_sm_set_consecutive_pindirs(pio, (uint)la_packed_sm_b, pin, 1U, false);
			la_configured_pins[la_configured_pin_count++] = pin;
		}
	}

	div256 = (((uint64_t)clock_get_hz(clk_sys) * 256ULL) +
		 ((uint64_t)prepare->config.sample_rate_hz / 2ULL)) /
		 prepare->config.sample_rate_hz;
	sm_a_cfg = pio_get_default_sm_config();
	sm_config_set_clkdiv_int_frac8(&sm_a_cfg, (uint32_t)(div256 / 256ULL),
		(uint8_t)(div256 % 256ULL));
	sm_config_set_in_pins(&sm_a_cfg, plan->lanes[0].pin_base);
	sm_config_set_in_pin_count(&sm_a_cfg, plan->lanes[0].pin_count);
	sm_config_set_in_shift(&sm_a_cfg, true, true,
		plan->lanes[0].autopush_bits);
	sm_config_set_fifo_join(&sm_a_cfg, PIO_FIFO_JOIN_RX);
	sm_config_set_wrap(&sm_a_cfg, (uint)la_packed_sm_a_offset + (sm_a_len - 1U),
		(uint)la_packed_sm_a_offset + (sm_a_len - 1U));
	ret = pio_sm_init(pio, (uint)la_packed_sm_a, (uint)la_packed_sm_a_offset,
		&sm_a_cfg);
	if (ret < 0) {
		return ret;
	}

	if (plan->lane_count > 1U) {
		sm_b_cfg = pio_get_default_sm_config();
		sm_config_set_clkdiv_int_frac8(&sm_b_cfg, (uint32_t)(div256 / 256ULL),
			(uint8_t)(div256 % 256ULL));
		sm_config_set_in_pins(&sm_b_cfg, plan->lanes[1].pin_base);
		sm_config_set_in_pin_count(&sm_b_cfg, plan->lanes[1].pin_count);
		sm_config_set_in_shift(&sm_b_cfg, true, true,
			plan->lanes[1].autopush_bits);
		sm_config_set_fifo_join(&sm_b_cfg, PIO_FIFO_JOIN_RX);
		sm_config_set_wrap(&sm_b_cfg, (uint)la_packed_sm_b_offset + (sm_b_len - 1U),
			(uint)la_packed_sm_b_offset + (sm_b_len - 1U));
		ret = pio_sm_init(pio, (uint)la_packed_sm_b, (uint)la_packed_sm_b_offset,
			&sm_b_cfg);
		if (ret < 0) {
			return ret;
		}
		pio_sm_clear_fifos(pio, (uint)la_packed_sm_b);
		pio_sm_restart(pio, (uint)la_packed_sm_b);
		pio_sm_clkdiv_restart(pio, (uint)la_packed_sm_b);
	}
	pio_sm_clear_fifos(pio, (uint)la_packed_sm_a);
	pio_sm_restart(pio, (uint)la_packed_sm_a);
	pio_sm_clkdiv_restart(pio, (uint)la_packed_sm_a);

	if (la_packed_burst_triggered) {
		ret = la_packed_burst_configure_trigger_locked(&prepare->config,
			(uint32_t)(div256 / 256ULL), (uint8_t)(div256 % 256ULL));
		if (ret < 0) {
			return ret;
		}
	}

	return la_packed_burst_configure_dma_locked();
}

static int la_packed_ring_configure_locked(const struct linkr_debugger_la_config *config)
{
	const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	PIO pio;
	uint64_t div256;
	pio_sm_config sm_a_cfg;
	pio_sm_config sm_b_cfg;
	uint8_t sm_a_len;
	uint8_t sm_b_len = 0U;
	int ret;

	if (!device_is_ready(gpio_dev) || !device_is_ready(la_pio_dev) || !device_is_ready(la_dma_dev)) {
		return -ENODEV;
	}
	pio = pio_rpi_pico_get_pio(la_pio_dev);
	ret = pio_rpi_pico_allocate_sm(la_pio_dev, &la_packed_sm_a);
	if (ret < 0) {
		return ret;
	}
	la_packed_sm_a_claimed = true;
	if (la_stream_ring_plan.lane_count > 1U) {
		ret = pio_rpi_pico_allocate_sm(la_pio_dev, &la_packed_sm_b);
		if (ret < 0) {
			return ret;
		}
		la_packed_sm_b_claimed = true;
	}

	if (config->trigger != LINKR_DEBUGGER_LA_TRIGGER_NONE && config->pre_samples == 0U) {
		la_packed_sm_a_instructions[0] = LINKR_DEBUGGER_LA_PIO_WAIT_IRQ0;
		la_packed_sm_a_instructions[1] = la_pio_in_pins_instruction(
			la_stream_ring_plan.lanes[0].bits_per_sample);
		sm_a_len = 2U;
		if (la_stream_ring_plan.lane_count > 1U) {
			la_packed_sm_b_instructions[0] = LINKR_DEBUGGER_LA_PIO_WAIT_IRQ0;
			la_packed_sm_b_instructions[1] = la_pio_in_pins_instruction(
				la_stream_ring_plan.lanes[1].bits_per_sample);
			sm_b_len = 2U;
		}
	} else {
		la_packed_sm_a_instructions[0] = la_pio_in_pins_instruction(
			la_stream_ring_plan.lanes[0].bits_per_sample);
		sm_a_len = 1U;
		if (la_stream_ring_plan.lane_count > 1U) {
			la_packed_sm_b_instructions[0] = la_pio_in_pins_instruction(
				la_stream_ring_plan.lanes[1].bits_per_sample);
			sm_b_len = 1U;
		}
	}

	la_packed_sm_a_program.length = sm_a_len;
	la_packed_sm_a_offset = la_packed_find_program_offset(pio, &la_packed_sm_a_program);
	if (la_packed_sm_a_offset < 0) {
		return la_packed_sm_a_offset;
	}
	ret = pio_add_program_at_offset(pio, &la_packed_sm_a_program,
		(uint)la_packed_sm_a_offset);
	if (ret < 0) {
		return ret;
	}
	la_packed_sm_a_offset = ret;
	la_packed_sm_a_program_loaded = true;

	if (la_stream_ring_plan.lane_count > 1U) {
		la_packed_sm_b_program.length = sm_b_len;
		la_packed_sm_b_offset = la_packed_find_program_offset(pio, &la_packed_sm_b_program);
		if (la_packed_sm_b_offset < 0) {
			return la_packed_sm_b_offset;
		}
		ret = pio_add_program_at_offset(pio, &la_packed_sm_b_program,
			(uint)la_packed_sm_b_offset);
		if (ret < 0) {
			return ret;
		}
		la_packed_sm_b_offset = ret;
		la_packed_sm_b_program_loaded = true;
	}

	for (uint8_t lane = 0U; lane < la_stream_ring_plan.lane_count; lane++) {
		size_t sm = lane == 0U ? la_packed_sm_a : la_packed_sm_b;

		for (uint8_t pin = la_stream_ring_plan.lanes[lane].pin_base;
		     pin < (uint8_t)(la_stream_ring_plan.lanes[lane].pin_base +
		     la_stream_ring_plan.lanes[lane].pin_count); pin++) {
			ret = gpio_pin_configure(gpio_dev, pin, GPIO_INPUT);
			if (ret < 0) {
				return ret;
			}
			pio_gpio_init(pio, pin);
			pio_sm_set_consecutive_pindirs(pio, (uint)sm, pin, 1U, false);
			la_configured_pins[la_configured_pin_count++] = pin;
		}
	}

	div256 = (((uint64_t)clock_get_hz(clk_sys) * 256ULL) +
		 ((uint64_t)config->sample_rate_hz / 2ULL)) / config->sample_rate_hz;
	sm_a_cfg = pio_get_default_sm_config();
	sm_config_set_clkdiv_int_frac8(&sm_a_cfg, (uint32_t)(div256 / 256ULL),
		(uint8_t)(div256 % 256ULL));
	sm_config_set_in_pins(&sm_a_cfg, la_stream_ring_plan.lanes[0].pin_base);
	sm_config_set_in_pin_count(&sm_a_cfg, la_stream_ring_plan.lanes[0].pin_count);
	sm_config_set_in_shift(&sm_a_cfg, true, true,
		la_stream_ring_plan.lanes[0].autopush_bits);
	sm_config_set_fifo_join(&sm_a_cfg, PIO_FIFO_JOIN_RX);
	sm_config_set_wrap(&sm_a_cfg, (uint)la_packed_sm_a_offset + (sm_a_len - 1U),
		(uint)la_packed_sm_a_offset + (sm_a_len - 1U));
	ret = pio_sm_init(pio, (uint)la_packed_sm_a, (uint)la_packed_sm_a_offset, &sm_a_cfg);
	if (ret < 0) {
		return ret;
	}
	pio_sm_clear_fifos(pio, (uint)la_packed_sm_a);
	pio_sm_restart(pio, (uint)la_packed_sm_a);
	pio_sm_clkdiv_restart(pio, (uint)la_packed_sm_a);

	if (la_stream_ring_plan.lane_count > 1U) {
		sm_b_cfg = pio_get_default_sm_config();
		sm_config_set_clkdiv_int_frac8(&sm_b_cfg, (uint32_t)(div256 / 256ULL),
			(uint8_t)(div256 % 256ULL));
		sm_config_set_in_pins(&sm_b_cfg, la_stream_ring_plan.lanes[1].pin_base);
		sm_config_set_in_pin_count(&sm_b_cfg, la_stream_ring_plan.lanes[1].pin_count);
		sm_config_set_in_shift(&sm_b_cfg, true, true,
			la_stream_ring_plan.lanes[1].autopush_bits);
		sm_config_set_fifo_join(&sm_b_cfg, PIO_FIFO_JOIN_RX);
		sm_config_set_wrap(&sm_b_cfg, (uint)la_packed_sm_b_offset + (sm_b_len - 1U),
			(uint)la_packed_sm_b_offset + (sm_b_len - 1U));
		ret = pio_sm_init(pio, (uint)la_packed_sm_b, (uint)la_packed_sm_b_offset,
			&sm_b_cfg);
		if (ret < 0) {
			return ret;
		}
		pio_sm_clear_fifos(pio, (uint)la_packed_sm_b);
		pio_sm_restart(pio, (uint)la_packed_sm_b);
		pio_sm_clkdiv_restart(pio, (uint)la_packed_sm_b);
	}

	if (config->trigger != LINKR_DEBUGGER_LA_TRIGGER_NONE && config->pre_samples == 0U) {
		ret = la_packed_burst_configure_trigger_locked(config,
			(uint32_t)(div256 / 256ULL), (uint8_t)(div256 % 256ULL));
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
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

static void la_cleanup_locked(void)
{
}

static void la_stream_teardown_locked(void)
{
}

static void la_stream_clear_callback_locked(void)
{
}

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

int linkr_debugger_logic_analyzer_start_stream_sink(
	const struct linkr_debugger_la_config *config,
	const struct linkr_debugger_la_stream_sink *sink)
{
	ARG_UNUSED(config);
	ARG_UNUSED(sink);
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

bool linkr_debugger_logic_analyzer_stream_sink_should_yield_for_handoff(
	bool handoff_requested, uint64_t unread_samples)
{
	return handoff_requested &&
		unread_samples <= LINKR_DEBUGGER_LA_STREAM_HANDOFF_UNREAD_SAMPLES;
}

static struct linkr_debugger_la_start_prepare la_start_prepare_active;
static uint32_t la_start_prepare_next_generation;

static bool la_state_allows_start_prepare(enum linkr_debugger_la_state state)
{
	return state == LINKR_DEBUGGER_LA_STATE_IDLE ||
		state == LINKR_DEBUGGER_LA_STATE_DONE ||
		state == LINKR_DEBUGGER_LA_STATE_ERROR;
}

static bool la_start_prepare_is_active(void)
{
	return la_start_prepare_active.state >= LINKR_DEBUGGER_LA_START_PREPARE_READY &&
		la_start_prepare_active.state <=
		LINKR_DEBUGGER_LA_START_PREPARE_ARMED_EVENT_SENT;
}

static bool la_start_prepare_state_is_terminal(
	enum linkr_debugger_la_start_prepare_state state)
{
	return state == LINKR_DEBUGGER_LA_START_PREPARE_GO_FAILED ||
		state == LINKR_DEBUGGER_LA_START_PREPARE_CANCELLED;
}

static uint32_t la_start_prepare_allocate_generation(void)
{
	la_start_prepare_next_generation++;
	if (la_start_prepare_next_generation == 0U) {
		la_start_prepare_next_generation = 1U;
	}

	return la_start_prepare_next_generation;
}

static int la_start_prepare_fill_common(struct linkr_debugger_la_start_prepare *next,
	const struct linkr_debugger_la_config *config, bool config_v2,
	const struct linkr_debugger_la_stream_sink *sink)
{
	int ret;

	if (next == NULL || config == NULL) {
		return -EINVAL;
	}

	memset(next, 0, sizeof(*next));
	next->generation = la_start_prepare_allocate_generation();
	next->state = LINKR_DEBUGGER_LA_START_PREPARE_READY;
	next->config = *config;
	next->config_v2 = config_v2;
	ret = linkr_debugger_logic_analyzer_session_contract(config, &next->contract);
	if (ret < 0) {
		return ret;
	}
	ret = linkr_debugger_logic_analyzer_select_hardware_plan(config, config_v2,
		&next->plan);
	if (ret < 0) {
		return ret;
	}
	if (!next->plan.supported || next->plan.pipeline_family !=
	    LINKR_DEBUGGER_LA_PIPELINE_FAMILY_COMMON_PACKED) {
		return -ENOTSUP;
	}
	next->requested_sample_rate_hz = config->sample_rate_hz;
	next->actual_sample_rate_hz = linkr_debugger_logic_analyzer_actual_rate(
		config->sample_rate_hz);
	next->sample_period_ps = linkr_debugger_logic_analyzer_sample_period_ps(
		next->actual_sample_rate_hz);
	next->backend = linkr_debugger_logic_analyzer_backend();
	if (sink != NULL) {
		next->sink = *sink;
		next->sink_bound = true;
	}
	return 0;
}

static void la_start_prepare_clear_active_locked(void)
{
	memset(&la_start_prepare_active, 0, sizeof(la_start_prepare_active));
}

static int la_start_prepare_validate_active_locked(
	const struct linkr_debugger_la_start_prepare *prepare)
{
	if (prepare == NULL || prepare->generation == 0U ||
	    prepare->state == LINKR_DEBUGGER_LA_START_PREPARE_EMPTY) {
		return -EINVAL;
	}
	if (la_start_prepare_state_is_terminal(prepare->state)) {
		return -EALREADY;
	}
	if (!la_start_prepare_is_active()) {
		return -EINVAL;
	}
	if (prepare->generation != la_start_prepare_active.generation) {
		return -ESTALE;
	}
	if (prepare->state != la_start_prepare_active.state) {
		return -ESTALE;
	}

	return 0;
}

static void la_start_prepare_copy_active_to_token_locked(
	struct linkr_debugger_la_start_prepare *prepare)
{
	*prepare = la_start_prepare_active;
}

int linkr_debugger_logic_analyzer_prepare_wide11_burst_start(
	const struct linkr_debugger_la_config *config,
	struct linkr_debugger_la_start_prepare *prepare)
{
	return linkr_debugger_logic_analyzer_prepare_wide11_burst_start_sink(config,
		NULL, NULL, prepare);
}

int linkr_debugger_logic_analyzer_prepare_wide11_burst_start_sink(
	const struct linkr_debugger_la_config *config,
	const struct linkr_debugger_capture_arena_lease *arena_lease,
	const struct linkr_debugger_la_stream_sink *sink,
	struct linkr_debugger_la_start_prepare *prepare)
{
	struct linkr_debugger_la_start_prepare next;
	struct linkr_debugger_la_packed_burst_plan burst_plan;
	int ret;

	if (prepare == NULL ||
	    linkr_debugger_logic_analyzer_packed_burst_plan(config, &burst_plan) < 0) {
		return -EINVAL;
	}
	if ((arena_lease == NULL) != (sink == NULL)) {
		return -EINVAL;
	}
	if (sink != NULL) {
		ret = linkr_debugger_logic_analyzer_stream_sink_validate(config, sink);
		if (ret < 0) {
			return ret;
		}
		if (sink->bytes_per_sample != burst_plan.bytes_per_sample ||
		    sink->max_chunk_samples == 0U ||
		    sink->max_chunk_samples >
		    la_packed_burst_max_chunk_samples(burst_plan.bytes_per_sample)) {
			return -EINVAL;
		}
	}

	k_mutex_lock(&la_mutex, K_FOREVER);
	if (la_start_prepare_is_active() || !la_state_allows_start_prepare(la_capture.state)) {
		k_mutex_unlock(&la_mutex);
		return -EBUSY;
	}

	ret = la_start_prepare_fill_common(&next, config, true, sink);
	if (ret < 0) {
		k_mutex_unlock(&la_mutex);
		return ret;
	}
	if (arena_lease != NULL) {
		next.arena_held = true;
		next.arena_lease = *arena_lease;
	}
#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
	if (next.sink_bound) {
		la_stream_teardown_locked();
		la_cleanup_locked();
		la_stream_clear_callback_locked();
		la_generation++;
		la_capture.config = next.config;
		la_capture.sample_count = burst_plan.emitted_sample_count;
		la_capture.trigger_index = 0U;
		la_capture.requested_sample_rate_hz = next.requested_sample_rate_hz;
		la_capture.actual_sample_rate_hz = next.actual_sample_rate_hz;
		la_capture.config.sample_rate_hz = next.actual_sample_rate_hz;
		la_capture.sample_period_ps = next.sample_period_ps;
		la_capture.backend = next.backend;
		la_capture.samples = NULL;
		la_capture.state = next.config.trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE ?
			LINKR_DEBUGGER_LA_STATE_CAPTURING : LINKR_DEBUGGER_LA_STATE_ARMED;
		la_packed_burst_sink = next.sink;
		la_packed_burst_sink_bound = true;
		la_packed_burst_arena_lease = next.arena_lease;
		la_packed_burst_arena_held = true;
		la_packed_burst_plan_active = burst_plan;
		la_packed_burst_done_generation = 0U;
		la_packed_burst_done_mask = 0U;
		la_packed_burst_dma_status = 0;
		ret = la_packed_burst_configure_locked(&next);
		if (ret < 0) {
			la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
			la_cleanup_locked();
			k_mutex_unlock(&la_mutex);
			return ret;
		}
		la_packed_burst_hw_configured = true;
		next.hardware_prepared = true;
	}
#endif
	if (next.plan.legacy_adapter ==
	    LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST) {
		next.hardware_prepared = true;
	}
	la_start_prepare_active = next;
	la_start_prepare_copy_active_to_token_locked(prepare);
	k_mutex_unlock(&la_mutex);
	return 0;
}

#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
static int la_prepare_stream_common_locked(
	const struct linkr_debugger_la_start_prepare *prepare,
	linkr_debugger_la_stream_callback_t callback,
	void *user_data)
{
	const struct linkr_debugger_la_config *config;
	const struct linkr_debugger_la_stream_sink *sink;
	int ret;

	if (prepare == NULL || (!prepare->sink_bound && callback == NULL)) {
		return -EINVAL;
	}

	config = &prepare->config;
	sink = prepare->sink_bound ? &prepare->sink : NULL;
	while (true) {
		if (la_stream_active) {
			return -EBUSY;
		}
		if (la_capture.state != LINKR_DEBUGGER_LA_STATE_IDLE &&
		    la_capture.state != LINKR_DEBUGGER_LA_STATE_DONE &&
		    la_capture.state != LINKR_DEBUGGER_LA_STATE_ERROR) {
			return -EBUSY;
		}
		if (!linkr_debugger_logic_analyzer_stream_start_must_wait_idle(
		    la_stream_ring_thread_started, la_stream_ring_thread_busy) &&
		    !linkr_debugger_logic_analyzer_stream_start_must_wait_idle(
		    la_stream_ring_consumer_thread_started, la_stream_ring_consumer_thread_busy)) {
			break;
		}
		if (k_current_get() == &la_stream_ring_thread ||
		    k_current_get() == &la_stream_ring_consumer_thread || la_stream_ring_start_waiter) {
			return -EBUSY;
		}

		la_stream_ring_start_waiter = true;
		k_mutex_unlock(&la_mutex);
		la_stream_ring_wait_idle_if_needed();
		k_mutex_lock(&la_mutex, K_FOREVER);
		la_stream_ring_start_waiter = false;
	}

	la_stream_ring_consumer_set_priority(sink != NULL);
	k_sem_reset(&la_stream_ring_idle_sem);
	k_sem_reset(&la_stream_ring_consumer_idle_sem);
	la_stream_teardown_locked();
	la_cleanup_locked();
	la_stream_clear_callback_locked();
	la_generation++;
	la_stream_config = *config;
	la_stream_config.pin_count = la_active_pin_count(config);
	if (la_stream_config.selected_pin_count == 0U) {
		for (uint8_t i = 0U; i < la_stream_config.pin_count; i++) {
		la_stream_config.selected_pins[i] = (uint8_t)(config->pin_base + i);
		}
		la_stream_config.selected_pin_count = la_stream_config.pin_count;
	}
	la_stream_pre_trigger.enabled = config->pre_samples > 0U;
	ret = linkr_debugger_logic_analyzer_packed_ring_plan(&la_stream_config,
		&la_stream_ring_plan);
	if (ret < 0) {
		la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
		la_stream_teardown_locked();
		la_cleanup_locked();
		la_stream_clear_callback_locked();
		return ret;
	}

	if (sink != NULL) {
		la_stream_sink = *sink;
		la_stream_sink_active = true;
	} else {
		la_stream_callback = callback;
		la_stream_user_data = user_data;
	}
	la_stream_sequence = 0U;
	la_stream_triggered = (config->trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE);
	la_stream_values_or = 0U;
	la_stream_values_and = 0xffffU;
	la_stream_ring_emitted_samples = 0U;
	la_stream_ring_error_count = 0U;
	memset(&la_stream_ring_metrics, 0, sizeof(la_stream_ring_metrics));
	la_stream_ring_terminal_pending = false;
	la_stream_ring_terminal_emitted = false;
	la_stream_ring_acquisition_frozen = false;
	la_stream_ring_terminal_status = LINKR_DEBUGGER_LA_RING_POLL_OK;
	memset(la_stream_ring_values, 0,
		LINKR_DEBUGGER_LA_RING_HALF_SAMPLES * sizeof(la_stream_ring_values[0]));

	ret = la_packed_ring_configure_locked(&la_stream_config);
	if (ret < 0) {
		la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
		la_stream_teardown_locked();
		la_cleanup_locked();
		la_stream_clear_callback_locked();
		return ret;
	}

	la_capture.state = LINKR_DEBUGGER_LA_STATE_STREAMING;
	la_capture.config = la_stream_config;
	la_capture.sample_count = 0U;
	la_capture.trigger_index = config->pre_samples;
	la_capture.requested_sample_rate_hz = config->sample_rate_hz;
	la_capture.actual_sample_rate_hz = la_actual_rate_from_hw(config->sample_rate_hz);
	la_capture.sample_period_ps = linkr_debugger_logic_analyzer_sample_period_ps(
		la_capture.actual_sample_rate_hz);
	la_capture.backend = linkr_debugger_logic_analyzer_backend();
	la_stream_config.sample_rate_hz = la_capture.actual_sample_rate_hz;

	ret = la_start_stream_ring_dma_locked();
	if (ret < 0) {
		LOG_ERR("la stream packed ring setup failed ret=%d", ret);
		la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
		la_stream_teardown_locked();
		la_cleanup_locked();
		la_stream_clear_callback_locked();
		return ret;
	}
	la_stream_active = true;
	return 0;
}

static void la_start_prepared_stream_go_locked(void)
{
	PIO pio = pio_rpi_pico_get_pio(la_pio_dev);
	uint32_t sampler_mask = BIT(la_packed_sm_a);

	if (la_stream_ring_plan.lane_count > 1U) {
		sampler_mask |= BIT(la_packed_sm_b);
	}
	pio_enable_sm_mask_in_sync(pio, sampler_mask);
	if (la_trigger_sm_claimed) {
		pio_sm_set_enabled(pio, (uint)la_trigger_sm, true);
	}
	k_sem_give(&la_stream_ring_wake_sem);
}
#endif

int linkr_debugger_logic_analyzer_prepare_stream_start_sink(
	const struct linkr_debugger_la_config *config,
	bool config_v2,
	const struct linkr_debugger_la_stream_sink *sink,
	struct linkr_debugger_la_start_prepare *prepare)
{
	struct linkr_debugger_la_start_prepare next;
	int ret;

	if (prepare == NULL || sink == NULL) {
		return -EINVAL;
	}
	ret = linkr_debugger_logic_analyzer_validate_stream_config(config);
	if (ret < 0) {
		return ret;
	}
	ret = linkr_debugger_logic_analyzer_stream_sink_validate(config, sink);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&la_mutex, K_FOREVER);
	if (la_start_prepare_is_active() || !la_state_allows_start_prepare(la_capture.state)) {
		k_mutex_unlock(&la_mutex);
		return -EBUSY;
	}
	ret = la_start_prepare_fill_common(&next, config, config_v2, sink);
	if (ret < 0) {
		k_mutex_unlock(&la_mutex);
		return ret;
	}
	if (next.plan.legacy_adapter ==
	    LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST) {
		k_mutex_unlock(&la_mutex);
		return -EINVAL;
	}
	la_start_prepare_active = next;
#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
	ret = la_prepare_stream_common_locked(&la_start_prepare_active, NULL, NULL);
	if (ret < 0) {
		la_start_prepare_clear_active_locked();
		k_mutex_unlock(&la_mutex);
		return ret;
	}
	la_start_prepare_active.hardware_prepared = true;
#else
	la_start_prepare_active.hardware_prepared = true;
#endif
	la_start_prepare_copy_active_to_token_locked(prepare);
	k_mutex_unlock(&la_mutex);
	return 0;
}

int linkr_debugger_logic_analyzer_start_prepare_cancel(
	struct linkr_debugger_la_start_prepare *prepare)
{
	int ret;

	if (prepare == NULL || prepare->generation == 0U) {
		return -EINVAL;
	}
	if (la_start_prepare_state_is_terminal(prepare->state)) {
		return 0;
	}

	k_mutex_lock(&la_mutex, K_FOREVER);
	ret = la_start_prepare_validate_active_locked(prepare);
	if (ret == 0) {
		if (prepare->sink_bound) {
			la_stream_teardown_locked();
			la_cleanup_locked();
			la_stream_clear_callback_locked();
			prepare->arena_held = false;
			prepare->sink_bound = false;
			prepare->hardware_prepared = false;
		}
		prepare->state = LINKR_DEBUGGER_LA_START_PREPARE_CANCELLED;
		la_start_prepare_clear_active_locked();
	}
	k_mutex_unlock(&la_mutex);
	return ret;
}

int linkr_debugger_logic_analyzer_start_prepare_mark_response_sent(
	struct linkr_debugger_la_start_prepare *prepare)
{
	int ret;

	k_mutex_lock(&la_mutex, K_FOREVER);
	ret = la_start_prepare_validate_active_locked(prepare);
	if (ret == 0) {
		if (prepare->state != LINKR_DEBUGGER_LA_START_PREPARE_READY) {
			ret = -EPROTO;
		} else {
			la_start_prepare_active.state =
				LINKR_DEBUGGER_LA_START_PREPARE_RESPONSE_SENT;
			la_start_prepare_copy_active_to_token_locked(prepare);
		}
	}
	k_mutex_unlock(&la_mutex);
	return ret;
}

int linkr_debugger_logic_analyzer_start_prepare_mark_armed_event_sent(
	struct linkr_debugger_la_start_prepare *prepare)
{
	int ret;

	k_mutex_lock(&la_mutex, K_FOREVER);
	ret = la_start_prepare_validate_active_locked(prepare);
	if (ret == 0) {
		if (prepare->state != LINKR_DEBUGGER_LA_START_PREPARE_RESPONSE_SENT) {
			ret = -EPROTO;
		} else {
			la_start_prepare_active.state =
				LINKR_DEBUGGER_LA_START_PREPARE_ARMED_EVENT_SENT;
			la_start_prepare_copy_active_to_token_locked(prepare);
		}
	}
	k_mutex_unlock(&la_mutex);
	return ret;
}

int linkr_debugger_logic_analyzer_start_prepare_go(
	struct linkr_debugger_la_start_prepare *prepare)
{
	int ret;

	k_mutex_lock(&la_mutex, K_FOREVER);
	ret = la_start_prepare_validate_active_locked(prepare);
	if (ret == 0) {
		if ((prepare->config.trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE &&
		     prepare->state != LINKR_DEBUGGER_LA_START_PREPARE_RESPONSE_SENT) ||
		    (prepare->config.trigger != LINKR_DEBUGGER_LA_TRIGGER_NONE &&
		     prepare->state != LINKR_DEBUGGER_LA_START_PREPARE_ARMED_EVENT_SENT)) {
			ret = -EPROTO;
		} else {
			if (!prepare->sink_bound) {
				prepare->state = LINKR_DEBUGGER_LA_START_PREPARE_GO_FAILED;
				la_start_prepare_clear_active_locked();
				ret = -ENOTSUP;
			} else if (!prepare->hardware_prepared) {
				prepare->state = LINKR_DEBUGGER_LA_START_PREPARE_GO_FAILED;
				la_start_prepare_clear_active_locked();
				ret = -EPROTO;
			} else {
#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
				if (prepare->plan.legacy_adapter !=
				    LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST) {
					la_start_prepared_stream_go_locked();
					la_start_prepare_clear_active_locked();
					ret = 0;
				} else {
					PIO pio = pio_rpi_pico_get_pio(la_pio_dev);

					ret = linkr_debugger_capture_arena_mark_dma_active(
						&prepare->arena_lease);
					if (ret == 0) {
						uint32_t sampler_mask = BIT(la_packed_sm_a);

						if (la_packed_burst_plan_active.lane_count > 1U) {
							sampler_mask |= BIT(la_packed_sm_b);
						}

						la_packed_burst_active = true;
						la_packed_burst_done_generation = la_generation;
						pio_enable_sm_mask_in_sync(pio, sampler_mask);
						if (prepare->config.trigger != LINKR_DEBUGGER_LA_TRIGGER_NONE &&
						    la_trigger_sm_claimed) {
							pio_sm_set_enabled(pio, (uint)la_trigger_sm, true);
						}
						la_start_prepare_clear_active_locked();
					} else {
						prepare->state = LINKR_DEBUGGER_LA_START_PREPARE_GO_FAILED;
						la_cleanup_locked();
						la_start_prepare_clear_active_locked();
					}
				}
#else
				la_start_prepare_clear_active_locked();
				ret = 0;
#endif
			}
		}
	}
	k_mutex_unlock(&la_mutex);
	return ret;
}

bool linkr_debugger_logic_analyzer_stream_sink_should_explicit_yield(
	bool handoff_requested, uint64_t unread_samples)
{
	return linkr_debugger_logic_analyzer_stream_sink_should_yield_for_handoff(
		handoff_requested, unread_samples);
}

int linkr_debugger_logic_analyzer_stream_consumer_priority(bool sink_session)
{
	return sink_session ? LINKR_DEBUGGER_LA_STREAM_CONSUMER_SINK_PRIORITY :
		LINKR_DEBUGGER_LA_STREAM_CONSUMER_PRIORITY;
}

bool linkr_debugger_logic_analyzer_stream_callback_should_yield_after_chunk(
	bool generation_current, uint32_t emitted_samples)
{
	return linkr_debugger_logic_analyzer_stream_callback_allows_protocol_update(
		generation_current, emitted_samples);
}

uint32_t linkr_debugger_logic_analyzer_capture_actual_rate(void)
{
	return la_capture.actual_sample_rate_hz;
}

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
	memset(&la_start_prepare_active, 0, sizeof(la_start_prepare_active));
#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
	k_work_init(&la_packed_burst_work, la_packed_burst_work_handler);
	k_sem_init(&la_stream_ring_wake_sem, 0, 1);
	k_sem_init(&la_stream_ring_consumer_wake_sem, 0, 1);
	k_sem_init(&la_stream_ring_idle_sem, 0, 1);
	k_sem_init(&la_stream_ring_consumer_idle_sem, 0, 1);
	la_stream_ring_thread_busy = false;
	la_stream_ring_consumer_thread_busy = false;
	if (!la_stream_ring_thread_started) {
		k_thread_create(&la_stream_ring_thread, la_stream_ring_thread_stack,
			K_THREAD_STACK_SIZEOF(la_stream_ring_thread_stack),
			la_stream_ring_thread_fn, NULL, NULL, NULL,
			LINKR_DEBUGGER_LA_STREAM_THREAD_PRIORITY, 0, K_NO_WAIT);
		k_thread_name_set(&la_stream_ring_thread, "la_ring");
		la_stream_ring_thread_started = true;
	}
	if (!la_stream_ring_consumer_thread_started) {
		k_thread_create(&la_stream_ring_consumer_thread,
			la_stream_ring_consumer_thread_stack,
			K_THREAD_STACK_SIZEOF(la_stream_ring_consumer_thread_stack),
			la_stream_ring_consumer_thread_fn, NULL, NULL, NULL,
			LINKR_DEBUGGER_LA_STREAM_CONSUMER_PRIORITY, 0, K_NO_WAIT);
		k_thread_name_set(&la_stream_ring_consumer_thread, "la_ring_cons");
		la_stream_ring_consumer_thread_started = true;
	}
	la_stream_ring_consumer_set_priority(false);
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
	struct linkr_debugger_la_packed_burst_plan burst_plan;
	struct linkr_debugger_capture_arena_lease arena_lease;
	struct linkr_debugger_la_start_prepare packed_prepare;
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
	la_stream_clear_callback_locked();
	la_generation++;
	memset(la_samples, 0,
		LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES * sizeof(la_samples[0]));
	la_capture.config = normalized;
	la_capture.sample_count = total_samples;
	la_capture.trigger_index = config->pre_samples;
	la_capture.requested_sample_rate_hz = config->sample_rate_hz;
	la_capture.actual_sample_rate_hz = la_actual_rate_from_hw(config->sample_rate_hz);
	la_capture.config.sample_rate_hz = la_capture.actual_sample_rate_hz;
	la_capture.sample_period_ps = linkr_debugger_logic_analyzer_sample_period_ps(
		la_capture.actual_sample_rate_hz);
	la_capture.backend = linkr_debugger_logic_analyzer_backend();
	la_stream_config.sample_rate_hz = la_capture.actual_sample_rate_hz;
	la_capture.samples = NULL;
	memset(&arena_lease, 0, sizeof(arena_lease));
	memset(&packed_prepare, 0, sizeof(packed_prepare));
	ret = linkr_debugger_logic_analyzer_packed_burst_plan(&normalized, &burst_plan);
	if (ret < 0 || burst_plan.requested_sample_count != total_samples) {
		la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
		k_mutex_unlock(&la_mutex);
		return ret < 0 ? ret : -ENOTSUP;
	}
	ret = linkr_debugger_capture_arena_try_acquire_wide11_quiesced(la_generation, 0,
		&arena_lease);
	if (ret < 0) {
		la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
		k_mutex_unlock(&la_mutex);
		return ret;
	}
	ret = linkr_debugger_capture_arena_mark_armed(&arena_lease);
	if (ret < 0) {
		(void)linkr_debugger_capture_arena_release(&arena_lease);
		la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
		k_mutex_unlock(&la_mutex);
		return ret;
	}
	la_packed_burst_plan_active = burst_plan;
	la_packed_burst_sink_bound = false;
	memset(&la_packed_burst_sink, 0, sizeof(la_packed_burst_sink));
	la_packed_burst_arena_lease = arena_lease;
	la_packed_burst_arena_held = true;
	la_packed_burst_done_generation = 0U;
	la_packed_burst_done_mask = 0U;
	la_packed_burst_dma_status = 0;
	packed_prepare.config = normalized;
	ret = la_packed_burst_configure_locked(&packed_prepare);
	if (ret < 0) {
		la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
		la_cleanup_locked();
		k_mutex_unlock(&la_mutex);
		return ret;
	}
	ret = linkr_debugger_capture_arena_mark_dma_active(&arena_lease);
	if (ret < 0) {
		la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
		la_cleanup_locked();
		k_mutex_unlock(&la_mutex);
		return ret;
	}
	la_capture.state = (normalized.trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE) ?
		LINKR_DEBUGGER_LA_STATE_CAPTURING : LINKR_DEBUGGER_LA_STATE_ARMED;
	la_packed_burst_active = true;
	la_packed_burst_done_generation = la_generation;
	{
		PIO pio = pio_rpi_pico_get_pio(la_pio_dev);
		uint32_t sampler_mask = BIT(la_packed_sm_a);

		if (la_packed_burst_plan_active.lane_count > 1U) {
			sampler_mask |= BIT(la_packed_sm_b);
		}
		pio_enable_sm_mask_in_sync(pio, sampler_mask);
		if (normalized.trigger != LINKR_DEBUGGER_LA_TRIGGER_NONE &&
		    la_trigger_sm_claimed) {
			pio_sm_set_enabled(pio, (uint)la_trigger_sm, true);
		}
	}
	k_mutex_unlock(&la_mutex);
	return 0;
#endif
}

int linkr_debugger_logic_analyzer_cancel(void)
{
#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
	la_stream_stop_and_cleanup(LINKR_DEBUGGER_LA_STATE_IDLE);
	k_mutex_lock(&la_mutex, K_FOREVER);
	la_start_prepare_clear_active_locked();
	k_mutex_unlock(&la_mutex);
#else
	k_mutex_lock(&la_mutex, K_FOREVER);
	la_start_prepare_clear_active_locked();
	la_capture.state = LINKR_DEBUGGER_LA_STATE_IDLE;
	la_capture.samples = NULL;
	k_mutex_unlock(&la_mutex);
#endif
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

	la_stream_request_inactive_locked();
	k_mutex_unlock(&la_mutex);

	la_stream_ring_wait_idle_if_needed();

	k_mutex_lock(&la_mutex, K_FOREVER);
	la_cleanup_locked();
	la_stream_clear_callback_locked();
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
	memset(la_pre_trigger_ring, 0,
		LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES * sizeof(la_pre_trigger_ring[0]));

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
		la_stream_clear_callback_locked();
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
#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
	la_stream_stop_and_cleanup(LINKR_DEBUGGER_LA_STATE_IDLE);
#else
	k_mutex_lock(&la_mutex, K_FOREVER);
	la_capture.state = LINKR_DEBUGGER_LA_STATE_IDLE;
	la_capture.samples = NULL;
	k_mutex_unlock(&la_mutex);
#endif
}

#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)

static bool la_stream_ring_current(uint32_t generation)
{
	bool current;

	k_mutex_lock(&la_mutex, K_FOREVER);
	current = linkr_debugger_logic_analyzer_stream_generation_current(
		la_stream_ring_active, generation, la_generation);
	k_mutex_unlock(&la_mutex);
	return current;
}

static void la_stream_ring_progress_snapshot(uint64_t *writer_seq, uint64_t *reader_seq,
					     uint32_t *last_hw_index,
					     struct linkr_debugger_la_ring_metrics *metrics)
{
	k_spinlock_key_t key = k_spin_lock(&la_stream_ring_progress_lock);

	if (writer_seq != NULL) {
		*writer_seq = la_stream_ring_progress.writer_seq;
	}
	if (reader_seq != NULL) {
		*reader_seq = la_stream_ring_progress.reader_seq;
	}
	if (last_hw_index != NULL) {
		*last_hw_index = la_stream_ring_progress.last_hw_index;
	}
	if (metrics != NULL) {
		*metrics = la_stream_ring_metrics;
	}
	k_spin_unlock(&la_stream_ring_progress_lock, key);
}

static void la_stream_ring_freeze_acquisition_locked(void)
{
	struct linkr_debugger_la_ring_freeze_policy policy;
	PIO pio;

	if (la_stream_ring_acquisition_frozen) {
		return;
	}
	la_stream_ring_acquisition_frozen = true;
	policy = linkr_debugger_logic_analyzer_ring_freeze_policy(
		la_stream_ring_plan.lane_count);
	if (device_is_ready(la_pio_dev)) {
		pio = pio_rpi_pico_get_pio(la_pio_dev);
		if (policy.stop_sampler_sm_a && la_packed_sm_a_claimed) {
			pio_sm_set_enabled(pio, (uint)la_packed_sm_a, false);
			pio_sm_clear_fifos(pio, (uint)la_packed_sm_a);
		}
		if (policy.stop_sampler_sm_b && la_packed_sm_b_claimed) {
			pio_sm_set_enabled(pio, (uint)la_packed_sm_b, false);
			pio_sm_clear_fifos(pio, (uint)la_packed_sm_b);
		}
		if (policy.stop_trigger_sm && la_trigger_sm_claimed) {
			pio_sm_set_enabled(pio, (uint)la_trigger_sm, false);
			pio_interrupt_clear(pio, 0U);
		}
	}
	if (device_is_ready(la_dma_dev)) {
		if (policy.abort_dma_a && la_packed_dma_a_channel >= 0) {
			dma_channel_set_irq0_enabled((uint)la_packed_dma_a_channel, false);
#if defined(DMA_CH0_CTRL_TRIG_EN_BITS)
			hw_clear_bits(&dma_hw->ch[la_packed_dma_a_channel].ctrl_trig,
				DMA_CH0_CTRL_TRIG_EN_BITS);
#endif
			dma_channel_abort((uint)la_packed_dma_a_channel);
			dma_hw->ints0 = BIT(la_packed_dma_a_channel);
		}
		if (policy.abort_dma_b && la_packed_dma_b_channel >= 0) {
			dma_channel_set_irq0_enabled((uint)la_packed_dma_b_channel, false);
#if defined(DMA_CH0_CTRL_TRIG_EN_BITS)
			hw_clear_bits(&dma_hw->ch[la_packed_dma_b_channel].ctrl_trig,
				DMA_CH0_CTRL_TRIG_EN_BITS);
#endif
			dma_channel_abort((uint)la_packed_dma_b_channel);
			dma_hw->ints0 = BIT(la_packed_dma_b_channel);
		}
	}
}

static void la_stream_ring_request_terminal(enum linkr_debugger_la_ring_poll_result status,
					    uint32_t generation)
{
	k_mutex_lock(&la_mutex, K_FOREVER);
	if (linkr_debugger_logic_analyzer_stream_generation_current(
	    la_stream_ring_active, generation, la_generation) &&
	    !la_stream_ring_terminal_pending && !la_stream_ring_terminal_emitted) {
		la_stream_ring_terminal_status = status;
		la_stream_ring_terminal_pending = true;
		la_stream_ring_freeze_acquisition_locked();
		if (status != LINKR_DEBUGGER_LA_RING_POLL_OK) {
			la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
		}
		k_sem_give(&la_stream_ring_consumer_wake_sem);
	}
	k_mutex_unlock(&la_mutex);
}

static bool la_stream_ring_take_terminal(uint32_t generation,
					 enum linkr_debugger_la_ring_poll_result *status)
{
	bool taken = false;

	k_mutex_lock(&la_mutex, K_FOREVER);
	if (linkr_debugger_logic_analyzer_stream_generation_current(
	    la_stream_ring_active, generation, la_generation) &&
	    la_stream_ring_terminal_pending && !la_stream_ring_terminal_emitted) {
		*status = la_stream_ring_terminal_status;
		la_stream_ring_terminal_pending = false;
		la_stream_ring_terminal_emitted = true;
		taken = true;
	}
	k_mutex_unlock(&la_mutex);
	return taken;
}

static bool la_stream_ring_terminal_waiting(uint32_t generation)
{
	bool waiting;

	k_mutex_lock(&la_mutex, K_FOREVER);
	waiting = linkr_debugger_logic_analyzer_stream_generation_current(
		la_stream_ring_active, generation, la_generation) &&
		la_stream_ring_terminal_pending && !la_stream_ring_terminal_emitted;
	k_mutex_unlock(&la_mutex);
	return waiting;
}

static void la_stream_emit_terminal(enum linkr_debugger_la_ring_poll_result status,
					    uint32_t generation)
{
	struct linkr_debugger_la_stream_chunk chunk;
	linkr_debugger_la_stream_callback_t callback;
	struct linkr_debugger_la_stream_sink sink;
	void *user_data;
	bool sink_active;
	uint64_t writer_seq;
	uint64_t reader_seq;
	uint32_t last_hw_index;
	struct linkr_debugger_la_ring_metrics metrics;

	if (!la_stream_ring_current(generation)) {
		return;
	}
	la_stream_ring_progress_snapshot(&writer_seq, &reader_seq, &last_hw_index, &metrics);

	LOG_WRN("la ring terminal: status=%d seq=%u active=%d gen=%u cur_gen=%u dma=%d "
		"hw=%u writer=%llu reader=%llu emitted=%u errors=%u max_gap_us=%llu "
		"max_unread=%llu max_emit_us=%llu fill_compact_max_us=%llu "
		"fill_compact_total_us=%llu commit_callback_max_us=%llu "
		"commit_callback_total_us=%llu consume_total_us=%llu consume_chunks=%u "
		"consumer_gap_max_us=%llu yield_resume_max_us=%llu legacy_yields=%u "
		"sink_handoff_req=%u sink_handoff_exec=%u sink_handoff_skip_backlog=%u "
		"sink=%d",
		status, la_stream_sequence, la_stream_ring_active,
		la_stream_ring_progress.generation, la_generation, la_packed_dma_a_channel,
		last_hw_index, (unsigned long long)writer_seq,
		(unsigned long long)reader_seq, la_stream_ring_emitted_samples,
		la_stream_ring_error_count,
		(unsigned long long)metrics.max_poll_gap_us,
		(unsigned long long)metrics.max_unread_samples,
		(unsigned long long)metrics.max_emit_us,
		(unsigned long long)metrics.max_compact_us,
		(unsigned long long)metrics.total_compact_us,
		(unsigned long long)metrics.max_callback_us,
		(unsigned long long)metrics.total_callback_us,
		(unsigned long long)metrics.total_consume_us,
		metrics.consume_chunk_count,
		(unsigned long long)metrics.max_consumer_inter_chunk_gap_us,
		(unsigned long long)metrics.max_consumer_yield_resume_us,
		metrics.legacy_yield_count,
		metrics.sink_handoff_requested_count,
		metrics.sink_handoff_executed_count,
		metrics.sink_handoff_skipped_backlog_count, la_stream_sink_active);

	memset(&chunk, 0, sizeof(chunk));
	chunk.sequence = la_stream_sequence++;
	chunk.status = status;
	k_mutex_lock(&la_mutex, K_FOREVER);
	callback = la_stream_callback;
	user_data = la_stream_user_data;
	sink = la_stream_sink;
	sink_active = la_stream_sink_active;
	k_mutex_unlock(&la_mutex);
	if (sink_active && la_stream_ring_current(generation)) {
		linkr_debugger_logic_analyzer_stream_sink_notify_terminal(&sink, status,
			chunk.sequence);
	} else if (callback != NULL && la_stream_ring_current(generation)) {
		callback(&chunk, user_data);
	}
}

static void la_stream_ring_emit_terminal_and_stop(uint32_t generation,
	enum linkr_debugger_la_ring_poll_result terminal_status)
{
	la_stream_emit_terminal(terminal_status, generation);
	k_mutex_lock(&la_mutex, K_FOREVER);
	if (generation == la_generation) {
		la_stream_ring_active = false;
		la_stream_active = false;
		la_cleanup_locked();
		la_stream_clear_callback_locked();
		k_sem_give(&la_stream_ring_wake_sem);
	}
	k_mutex_unlock(&la_mutex);
}

static uint32_t *la_stream_ring_lane_words(uint8_t lane)
{
	uint8_t *base = (uint8_t *)la_stream_packed_ring_words;

	if (lane >= la_stream_ring_plan.lane_count) {
		return NULL;
	}
	return (uint32_t *)(base + la_stream_ring_plan.lanes[lane].arena_offset);
}

static void la_stream_ring_hw_indices(uint32_t hw_word_indices[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES])
{
	for (uint8_t lane = 0U; lane < la_stream_ring_plan.lane_count; lane++) {
		uintptr_t base = (uintptr_t)la_stream_ring_lane_words(lane);
		uintptr_t write_addr = (uintptr_t)(lane == 0U ?
			dma_hw->ch[la_packed_dma_a_channel].write_addr :
			dma_hw->ch[la_packed_dma_b_channel].write_addr);

		hw_word_indices[lane] = (uint32_t)(((write_addr - base) &
			(la_stream_ring_plan.lanes[lane].byte_count - 1U)) /
			LINKR_DEBUGGER_LA_RAW_SAMPLE_BYTES);
	}
}

static enum linkr_debugger_la_ring_poll_result la_stream_ring_refresh_progress(
	uint64_t *writer_sequence)
{
	uint32_t hw_word_indices[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t produced = 0U;
	uint32_t skew_samples = 0U;
	uint64_t now_us;
	uint64_t poll_gap_us = 0U;
	uint64_t unread_samples;
	enum linkr_debugger_la_ring_poll_result result;
	k_spinlock_key_t key;

	la_stream_ring_hw_indices(hw_word_indices);
	now_us = k_ticks_to_us_floor64(k_uptime_ticks());
	key = k_spin_lock(&la_stream_ring_progress_lock);
	if (la_stream_ring_progress.initialized &&
	    now_us >= la_stream_ring_progress.last_poll_time_us) {
		poll_gap_us = now_us - la_stream_ring_progress.last_poll_time_us;
	}
	result = linkr_debugger_logic_analyzer_packed_ring_observe(&la_stream_ring_progress,
		la_stream_ring_lane_last_hw_index, la_stream_ring_lane_writer_seq,
		hw_word_indices, now_us, la_capture.actual_sample_rate_hz, 0U,
		&la_stream_ring_plan, &produced, &skew_samples);
	unread_samples = la_stream_ring_progress.writer_seq - la_stream_ring_progress.reader_seq;
	linkr_debugger_logic_analyzer_ring_metrics_update(&la_stream_ring_metrics,
		poll_gap_us, unread_samples, 0U);
	if (writer_sequence != NULL) {
		*writer_sequence = la_stream_ring_progress.writer_seq;
	}
	k_spin_unlock(&la_stream_ring_progress_lock, key);
	return result;
}

static uint32_t la_stream_ring_bounded_target(void)
{
	uint32_t target_samples;

	if (la_stream_config.post_samples == 0U ||
	    linkr_debugger_logic_analyzer_bounded_sample_target(la_stream_config.pre_samples,
		la_stream_config.post_samples, &target_samples) < 0) {
		return 0U;
	}

	return target_samples;
}

static int la_stream_pre_trigger_read_level(uint64_t sequence,
	uint64_t *writer_sequence,
	uint8_t *level)
{
	const uint32_t *lane_words;
	int ret;

	if (writer_sequence == NULL || level == NULL) {
		return -EINVAL;
	}
	if (linkr_debugger_logic_analyzer_pre_trigger_scan_source_overrun(
		*writer_sequence, sequence, la_stream_ring_plan.sample_capacity,
		la_stream_ring_plan.safety_samples)) {
		return -EOVERFLOW;
	}
	lane_words = la_stream_ring_lane_words(la_stream_pre_trigger.trigger_geometry.lane_index);
	ret = la_packed_ring_trigger_level(&la_stream_pre_trigger.trigger_geometry, lane_words,
		sequence, level);
	if (ret < 0 || la_stream_ring_refresh_progress(writer_sequence) !=
	    LINKR_DEBUGGER_LA_RING_POLL_OK) {
		return ret < 0 ? ret : -EOVERFLOW;
	}
	return linkr_debugger_logic_analyzer_pre_trigger_scan_source_overrun(
		*writer_sequence, sequence, la_stream_ring_plan.sample_capacity,
		la_stream_ring_plan.safety_samples) ? -EOVERFLOW : 0;
}

#define LA_STREAM_PRE_TRIGGER_SCAN_CHUNK_SAMPLES 4096U

static int la_stream_pre_trigger_find_edge(uint64_t first_sequence,
	uint64_t end_sequence,
	uint64_t *writer_sequence)
{
	const uint32_t *lane_words;
	uint64_t sequence = first_sequence;

	if (writer_sequence == NULL || first_sequence > end_sequence) {
		return -EINVAL;
	}
	lane_words = la_stream_ring_lane_words(la_stream_pre_trigger.trigger_geometry.lane_index);
	if (lane_words == NULL) {
		return -EINVAL;
	}
	while (sequence < end_sequence) {
		uint64_t remaining = end_sequence - sequence;
		uint32_t sample_count = remaining > LA_STREAM_PRE_TRIGGER_SCAN_CHUNK_SAMPLES ?
			LA_STREAM_PRE_TRIGGER_SCAN_CHUNK_SAMPLES : (uint32_t)remaining;
		uint64_t scan_end_sequence = sequence + sample_count;
		uint64_t edge_sequence;
		uint8_t last_level;
		bool edge_found;
		int ret;

		if (linkr_debugger_logic_analyzer_pre_trigger_scan_source_overrun(
			end_sequence, sequence, la_stream_ring_plan.sample_capacity,
			la_stream_ring_plan.safety_samples)) {
			return -EOVERFLOW;
		}
		ret = la_packed_ring_trigger_scan(&la_stream_pre_trigger.trigger_geometry, lane_words,
			la_stream_config.trigger, sequence, scan_end_sequence,
			la_stream_pre_trigger.previous_level, &edge_sequence, &last_level, &edge_found);
		if (ret < 0 || la_stream_ring_refresh_progress(writer_sequence) !=
		    LINKR_DEBUGGER_LA_RING_POLL_OK ||
		    linkr_debugger_logic_analyzer_pre_trigger_scan_source_overrun(
			*writer_sequence, sequence, la_stream_ring_plan.sample_capacity,
			la_stream_ring_plan.safety_samples)) {
			return ret < 0 ? ret : -EOVERFLOW;
		}
		la_stream_pre_trigger.previous_level = last_level;
		if (edge_found) {
			la_stream_pre_trigger.edge_found = true;
			la_stream_pre_trigger.edge_sequence = edge_sequence;
			la_stream_pre_trigger.scan_sequence = edge_sequence + 1U;
			return 0;
		}
		sequence = scan_end_sequence;
		la_stream_pre_trigger.scan_sequence = sequence;
	}

	return 0;
}

static void la_stream_pre_trigger_retain_recent_samples(uint64_t scan_sequence)
{
	uint64_t retained_first_sequence = scan_sequence - la_stream_config.pre_samples;
	k_spinlock_key_t key = k_spin_lock(&la_stream_ring_progress_lock);

	if (la_stream_ring_progress.reader_seq < retained_first_sequence) {
		la_stream_ring_progress.reader_seq = retained_first_sequence;
	}
	k_spin_unlock(&la_stream_ring_progress_lock, key);
}

static int la_stream_pre_trigger_process(uint32_t generation)
{
	uint64_t writer_sequence;
	uint64_t reader_sequence;
	uint64_t scan_start_sequence;
	k_spinlock_key_t key;
	int ret;

	if (!la_stream_pre_trigger.enabled) {
		return 0;
	}
	if (la_stream_ring_refresh_progress(&writer_sequence) !=
	    LINKR_DEBUGGER_LA_RING_POLL_OK) {
		return -EOVERFLOW;
	}
	if (writer_sequence < la_stream_config.pre_samples) {
		return 0;
	}
	if (!la_stream_pre_trigger.scan_initialized) {
		scan_start_sequence = linkr_debugger_logic_analyzer_pre_trigger_scan_start(
			writer_sequence, la_stream_config.pre_samples);
		ret = la_packed_ring_trigger_geometry_build(&la_stream_ring_plan,
			la_stream_config.trigger_pin, &la_stream_pre_trigger.trigger_geometry);
		if (ret < 0) {
			return ret;
		}
		ret = la_stream_pre_trigger_read_level(scan_start_sequence - 1U,
			&writer_sequence, &la_stream_pre_trigger.previous_level);
		if (ret < 0) {
			return ret;
		}
		la_stream_pre_trigger.scan_initialized = true;
		la_stream_pre_trigger.scan_sequence = scan_start_sequence;
	}
	if (!la_stream_pre_trigger.edge_found) {
		if (la_stream_pre_trigger.scan_sequence > writer_sequence) {
			return -EIO;
		}
		if (la_stream_pre_trigger.scan_sequence < writer_sequence) {
			ret = la_stream_pre_trigger_find_edge(la_stream_pre_trigger.scan_sequence,
				writer_sequence, &writer_sequence);
			if (ret < 0) {
				return ret;
			}
		}
	}
	if (!la_stream_pre_trigger.edge_found) {
		la_stream_pre_trigger_retain_recent_samples(la_stream_pre_trigger.scan_sequence);
		return 0;
	}

	key = k_spin_lock(&la_stream_ring_progress_lock);
	reader_sequence = la_stream_ring_progress.reader_seq;
	k_spin_unlock(&la_stream_ring_progress_lock, key);
	ret = linkr_debugger_logic_analyzer_pre_trigger_window(&la_stream_config,
		writer_sequence, la_stream_pre_trigger.edge_sequence,
		la_stream_ring_plan.sample_capacity, la_stream_ring_plan.safety_samples,
		&la_stream_pre_trigger.window);
	if (ret < 0 || la_stream_pre_trigger.window.first_sequence < reader_sequence) {
		return ret < 0 ? ret : -EOVERFLOW;
	}
	if (writer_sequence < la_stream_pre_trigger.window.end_sequence) {
		return 0;
	}

	key = k_spin_lock(&la_stream_ring_progress_lock);
	if (la_stream_ring_progress.reader_seq > la_stream_pre_trigger.window.first_sequence) {
		k_spin_unlock(&la_stream_ring_progress_lock, key);
		return -EOVERFLOW;
	}
	la_stream_ring_progress.reader_seq = la_stream_pre_trigger.window.first_sequence;
	k_spin_unlock(&la_stream_ring_progress_lock, key);
	k_mutex_lock(&la_mutex, K_FOREVER);
	if (linkr_debugger_logic_analyzer_stream_generation_current(
	    la_stream_ring_active, generation, la_generation)) {
		la_capture.sample_count = la_stream_pre_trigger.window.sample_count;
		la_capture.trigger_index = la_stream_pre_trigger.window.trigger_index;
		la_stream_triggered = true;
	}
	k_mutex_unlock(&la_mutex);
	la_stream_ring_request_terminal(LINKR_DEBUGGER_LA_RING_POLL_OK, generation);
	return 0;
}

static bool la_stream_ring_prepare_emit(uint64_t first_seq, uint32_t count,
							 uint32_t generation,
							 struct linkr_debugger_la_stream_chunk *chunk,
							 uint64_t *emit_start_us)
{
	const uint32_t *lane_words[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_word_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint64_t period_ps = la_stream_config.sample_rate_hz > 0U
		? (1000000000000ULL / la_stream_config.sample_rate_hz)
		: 1000000ULL;
	uint16_t or_acc = 0U;
	uint16_t and_acc = 0xffffU;

	if (chunk == NULL || emit_start_us == NULL || count == 0U ||
	    count > LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES ||
	    !la_stream_ring_current(generation)) {
		return false;
	}
	for (uint8_t lane = 0U; lane < la_stream_ring_plan.lane_count; lane++) {
		lane_words[lane] = la_stream_ring_lane_words(lane);
		lane_word_counts[lane] = la_stream_ring_plan.lanes[lane].word_count;
	}

	if (la_stream_ring_plan.bytes_per_sample > 1U) {
		if (linkr_debugger_logic_analyzer_decode_packed_ring_span(&la_stream_ring_plan,
		    lane_words, lane_word_counts, first_seq,
		    (uint8_t *)la_stream_ring_values,
		    (size_t)count * sizeof(la_stream_ring_values[0]), count) < 0) {
			return false;
		}
	} else {
		if (linkr_debugger_logic_analyzer_decode_packed_ring_span(&la_stream_ring_plan,
		    lane_words, lane_word_counts, first_seq, (uint8_t *)la_stream_scratch,
		    count, count) < 0) {
			return false;
		}
		for (uint32_t i = 0U; i < count; i++) {
			la_stream_ring_values[i] = la_stream_scratch[i];
		}
	}

	for (uint32_t i = 0U; i < count; i++) {
		or_acc |= la_stream_ring_values[i];
		and_acc &= la_stream_ring_values[i];
	}

	la_stream_values_or |= or_acc;
	la_stream_values_and &= and_acc;
	chunk->sequence = la_stream_sequence++;
	chunk->sample_count = count;
	chunk->timestamp_us = (first_seq * period_ps) / 1000000ULL;
	chunk->values = la_stream_ring_values;
	chunk->status = LINKR_DEBUGGER_LA_RING_POLL_OK;
	la_stream_chunk_count++;
	return true;
}

static void la_stream_ring_record_consume_elapsed(uint64_t compact_us,
						  uint64_t callback_us,
						  uint64_t total_us)
{
	k_spinlock_key_t key = k_spin_lock(&la_stream_ring_progress_lock);

	linkr_debugger_logic_analyzer_ring_metrics_update_consume(
		&la_stream_ring_metrics, compact_us, callback_us, total_us);
	k_spin_unlock(&la_stream_ring_progress_lock, key);
}

enum la_stream_ring_consume_action {
	LA_STREAM_RING_CONSUME_STOP = 0,
	LA_STREAM_RING_CONSUME_CONTINUE,
	LA_STREAM_RING_CONSUME_YIELD_CONTINUE,
};

struct la_stream_ring_consume_result {
	enum la_stream_ring_consume_action action;
	bool sink_handoff;
};

static struct la_stream_ring_consume_result la_stream_ring_consume_result(
	enum la_stream_ring_consume_action action)
{
	struct la_stream_ring_consume_result result = { .action = action };

	return result;
}

static struct la_stream_ring_consume_result la_stream_ring_consume_yield_result(
	bool sink_handoff)
{
	struct la_stream_ring_consume_result result = {
		.action = LA_STREAM_RING_CONSUME_YIELD_CONTINUE,
		.sink_handoff = sink_handoff,
	};

	return result;
}

static void la_stream_ring_record_consume_start(uint64_t start_us)
{
	k_spinlock_key_t key = k_spin_lock(&la_stream_ring_progress_lock);

	linkr_debugger_logic_analyzer_ring_metrics_update_inter_chunk_gap(
		&la_stream_ring_metrics, start_us);
	k_spin_unlock(&la_stream_ring_progress_lock, key);
}

static void la_stream_ring_record_chunk_complete(uint64_t complete_us)
{
	k_spinlock_key_t key = k_spin_lock(&la_stream_ring_progress_lock);

	linkr_debugger_logic_analyzer_ring_metrics_mark_chunk_complete(
		&la_stream_ring_metrics, complete_us);
	k_spin_unlock(&la_stream_ring_progress_lock, key);
}

static void la_stream_ring_clear_consumer_gap(void)
{
	k_spinlock_key_t key = k_spin_lock(&la_stream_ring_progress_lock);

	linkr_debugger_logic_analyzer_ring_metrics_clear_consumer_gap(&la_stream_ring_metrics);
	k_spin_unlock(&la_stream_ring_progress_lock, key);
}

static void la_stream_ring_record_yield_resume(uint64_t duration_us, bool sink_handoff)
{
	k_spinlock_key_t key = k_spin_lock(&la_stream_ring_progress_lock);

	linkr_debugger_logic_analyzer_ring_metrics_update_yield_resume(
		&la_stream_ring_metrics, duration_us, sink_handoff);
	k_spin_unlock(&la_stream_ring_progress_lock, key);
}

static void la_stream_ring_record_sink_handoff(bool requested, bool executed)
{
	k_spinlock_key_t key = k_spin_lock(&la_stream_ring_progress_lock);

	linkr_debugger_logic_analyzer_ring_metrics_update_sink_handoff(
		&la_stream_ring_metrics, requested, executed);
	k_spin_unlock(&la_stream_ring_progress_lock, key);
}

static void la_stream_ring_abort_lease_once(
	const struct linkr_debugger_la_stream_sink *sink,
	struct linkr_debugger_la_stream_sink_lease *lease,
	bool *lease_active)
{
	if (lease_active == NULL || !*lease_active) {
		return;
	}

	linkr_debugger_logic_analyzer_stream_sink_abort_payload(sink, lease);
	*lease_active = false;
}

static struct la_stream_ring_consume_result la_stream_ring_consume_sink_once(uint32_t generation,
									     uint64_t reader_seq, uint32_t emit_count);

/* After an overrun terminal is requested, the consumer must keep offering the
 * remaining ring data to the sink, but it must never hot-spin: the transport
 * thread that frees sink queue space runs at a lower priority, so a
 * no-sleep retry loop would starve exactly the thread that can unblock it
 * (observed on hardware as a full service freeze after a ring overrun
 * terminal).  Failures therefore sleep one tick between retries, and once the
 * sink has refused data for LA_STREAM_RING_SINK_STALL_ABANDON_US the remaining
 * ring data is abandoned - it is already declared lost by the overrun
 * semantics - so the pending terminal can be emitted and the capture can
 * finish instead of retrying forever.
 */
#define LA_STREAM_RING_SINK_STALL_ABANDON_US 1000000ULL

static uint64_t la_stream_ring_sink_stall_since_us;

static struct la_stream_ring_consume_result la_stream_ring_consume_sink_failure(
	uint32_t generation)
{
	uint64_t now_us = k_ticks_to_us_floor64(k_uptime_ticks());

	la_stream_ring_request_terminal(LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN,
		generation);
	if (la_stream_ring_sink_stall_since_us == 0U) {
		la_stream_ring_sink_stall_since_us = now_us;
	} else if (now_us >= la_stream_ring_sink_stall_since_us &&
		   now_us - la_stream_ring_sink_stall_since_us >=
		   LA_STREAM_RING_SINK_STALL_ABANDON_US) {
		k_spinlock_key_t key = k_spin_lock(&la_stream_ring_progress_lock);

		la_stream_ring_progress.reader_seq = la_stream_ring_progress.writer_seq;
		k_spin_unlock(&la_stream_ring_progress_lock, key);
		la_stream_ring_sink_stall_since_us = 0U;
		LOG_WRN("la ring sink stall abandon: dropped remaining ring data gen=%u",
			generation);
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_CONTINUE);
	}
	k_msleep(1);
	return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_CONTINUE);
}

static struct la_stream_ring_consume_result la_stream_ring_consume_sink_once(uint32_t generation,
									     uint64_t reader_seq, uint32_t emit_count)
{
	struct linkr_debugger_la_stream_sink sink;
	struct linkr_debugger_la_stream_sink_lease lease;
	struct linkr_debugger_la_stream_sink_commit commit;
	const uint32_t *lane_words[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_word_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint64_t period_ps = la_stream_config.sample_rate_hz > 0U
		? (1000000000000ULL / la_stream_config.sample_rate_hz)
		: 1000000ULL;
	uint64_t consume_start_us;
	uint64_t fill_end_us;
	uint64_t commit_start_us;
	uint64_t commit_end_us;
	uint16_t values_or = 0U;
	uint16_t values_and = 0xffffU;
	k_spinlock_key_t key;
	bool current;
	bool reader_advanced;
	bool lease_active = false;
	bool handoff_requested;
	bool explicit_yield;
	uint64_t unread_samples;
	int ret;

	k_mutex_lock(&la_mutex, K_FOREVER);
	if (!linkr_debugger_logic_analyzer_stream_generation_current(
	    la_stream_ring_active, generation, la_generation) || !la_stream_sink_active) {
		k_mutex_unlock(&la_mutex);
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_STOP);
	}
	sink = la_stream_sink;
	k_mutex_unlock(&la_mutex);

	consume_start_us = k_ticks_to_us_floor64(k_uptime_ticks());
	la_stream_ring_record_consume_start(consume_start_us);
	ret = linkr_debugger_logic_analyzer_stream_sink_lease_payload(&sink,
		emit_count, &lease);
	if (ret < 0) {
		fill_end_us = k_ticks_to_us_floor64(k_uptime_ticks());
		la_stream_ring_record_consume_elapsed(
			fill_end_us >= consume_start_us ? fill_end_us - consume_start_us : 0U,
			0U, fill_end_us >= consume_start_us ? fill_end_us - consume_start_us : 0U);
		return la_stream_ring_consume_sink_failure(generation);
	}
	lease_active = true;
	for (uint8_t lane = 0U; lane < la_stream_ring_plan.lane_count; lane++) {
		lane_words[lane] = la_stream_ring_lane_words(lane);
		lane_word_counts[lane] = la_stream_ring_plan.lanes[lane].word_count;
	}

	ret = linkr_debugger_logic_analyzer_stream_sink_write_packed_payload(
		&la_stream_ring_plan, lane_words, lane_word_counts,
		reader_seq, emit_count, sink.bytes_per_sample, lease.payload,
		lease.capacity, &values_or, &values_and);
	fill_end_us = k_ticks_to_us_floor64(k_uptime_ticks());
	if (ret < 0) {
		la_stream_ring_abort_lease_once(&sink, &lease, &lease_active);
		la_stream_ring_record_consume_elapsed(
			fill_end_us >= consume_start_us ? fill_end_us - consume_start_us : 0U,
			0U, fill_end_us >= consume_start_us ? fill_end_us - consume_start_us : 0U);
		return la_stream_ring_consume_sink_failure(generation);
	}

	k_mutex_lock(&la_mutex, K_FOREVER);
	current = linkr_debugger_logic_analyzer_stream_generation_current(
		la_stream_ring_active, generation, la_generation);
	key = k_spin_lock(&la_stream_ring_progress_lock);
	reader_advanced = linkr_debugger_logic_analyzer_stream_copy_complete_advance_reader(
		&la_stream_ring_progress, &la_stream_ring_metrics, current, generation,
		reader_seq, emit_count);
	k_spin_unlock(&la_stream_ring_progress_lock, key);
	k_mutex_unlock(&la_mutex);
	if (!reader_advanced) {
		la_stream_ring_abort_lease_once(&sink, &lease, &lease_active);
		la_stream_ring_record_consume_elapsed(
			fill_end_us >= consume_start_us ? fill_end_us - consume_start_us : 0U,
			0U, fill_end_us >= consume_start_us ? fill_end_us - consume_start_us : 0U);
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_STOP);
	}

	memset(&commit, 0, sizeof(commit));
	commit.token = lease.token;
	commit.sequence = la_stream_sequence;
	commit.sample_count = emit_count;
	commit.timestamp_us = (reader_seq * period_ps) / 1000000ULL;
	commit.bytes_per_sample = sink.bytes_per_sample;
	commit.payload_len = (size_t)emit_count * sink.bytes_per_sample;
	commit_start_us = k_ticks_to_us_floor64(k_uptime_ticks());
	ret = linkr_debugger_logic_analyzer_stream_sink_commit_payload(&sink, &commit);
	commit_end_us = k_ticks_to_us_floor64(k_uptime_ticks());
	if (ret < 0) {
		la_stream_ring_abort_lease_once(&sink, &lease, &lease_active);
		la_stream_ring_record_consume_elapsed(
			fill_end_us >= consume_start_us ? fill_end_us - consume_start_us : 0U,
			commit_end_us >= commit_start_us ? commit_end_us - commit_start_us : 0U,
			commit_end_us >= consume_start_us ? commit_end_us - consume_start_us : 0U);
		return la_stream_ring_consume_sink_failure(generation);
	}
	handoff_requested = ret > 0;
	lease_active = false;
	la_stream_ring_sink_stall_since_us = 0U;
	key = k_spin_lock(&la_stream_ring_progress_lock);
	unread_samples = la_stream_ring_progress.writer_seq - la_stream_ring_progress.reader_seq;
	k_spin_unlock(&la_stream_ring_progress_lock, key);

	la_stream_ring_record_consume_elapsed(
		fill_end_us >= consume_start_us ? fill_end_us - consume_start_us : 0U,
		commit_end_us >= commit_start_us ? commit_end_us - commit_start_us : 0U,
		commit_end_us >= consume_start_us ? commit_end_us - consume_start_us : 0U);

	current = la_stream_ring_current(generation);
	if (!linkr_debugger_logic_analyzer_stream_sink_allows_protocol_update(
	    current, emit_count)) {
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_STOP);
	}

	la_stream_values_or |= values_or;
	la_stream_values_and &= values_and;
	la_stream_chunk_count++;
	la_stream_sequence++;
	la_stream_ring_emitted_samples += emit_count;
	if (la_stream_ring_bounded_target() > 0U &&
	    la_stream_ring_emitted_samples >= la_stream_ring_bounded_target()) {
		la_stream_ring_request_terminal(LINKR_DEBUGGER_LA_RING_POLL_OK, generation);
	}
	explicit_yield = linkr_debugger_logic_analyzer_stream_sink_should_explicit_yield(
		handoff_requested, unread_samples);
	la_stream_ring_record_sink_handoff(handoff_requested, explicit_yield);
	la_stream_ring_record_chunk_complete(commit_end_us);
	if (explicit_yield) {
		return la_stream_ring_consume_yield_result(true);
	}
	return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_CONTINUE);
}

static uint32_t la_stream_ring_poll_once(uint32_t generation)
{
	uint32_t produced = 0U;
	uint32_t hw_word_indices[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t skew_samples = 0U;
	uint64_t now_us;
	uint64_t poll_gap_us = 0U;
	uint64_t unread_samples;
	enum linkr_debugger_la_ring_poll_result result;
	k_spinlock_key_t key;

	if (!la_stream_ring_current(generation) || la_packed_dma_a_channel < 0 ||
	    la_stream_ring_acquisition_frozen) {
		return 0U;
	}

	if (!la_stream_pre_trigger.enabled && !la_stream_triggered) {
		PIO pio = pio_rpi_pico_get_pio(la_pio_dev);

		if (pio_interrupt_get(pio, 0U)) {
			la_stream_triggered = true;
			pio_interrupt_clear(pio, 0U);
		}
	}

	la_stream_ring_hw_indices(hw_word_indices);
	now_us = k_ticks_to_us_floor64(k_uptime_ticks());
	key = k_spin_lock(&la_stream_ring_progress_lock);
	if (la_stream_ring_progress.initialized &&
	    now_us >= la_stream_ring_progress.last_poll_time_us) {
		poll_gap_us = now_us - la_stream_ring_progress.last_poll_time_us;
	}
	result = linkr_debugger_logic_analyzer_packed_ring_observe(&la_stream_ring_progress,
		la_stream_ring_lane_last_hw_index, la_stream_ring_lane_writer_seq,
		hw_word_indices, now_us, la_capture.actual_sample_rate_hz, 0U,
		&la_stream_ring_plan, &produced, &skew_samples);
	if (result == LINKR_DEBUGGER_LA_RING_POLL_OK && !la_stream_pre_trigger.enabled &&
	    !la_stream_triggered) {
		la_stream_ring_progress.reader_seq = la_stream_ring_progress.writer_seq;
	}
	unread_samples = la_stream_ring_progress.writer_seq - la_stream_ring_progress.reader_seq;
	linkr_debugger_logic_analyzer_ring_metrics_update(&la_stream_ring_metrics,
		poll_gap_us, unread_samples, 0U);
	k_spin_unlock(&la_stream_ring_progress_lock, key);
	if (result != LINKR_DEBUGGER_LA_RING_POLL_OK) {
		LOG_WRN("la ring observe terminal: result=%d gen=%u cur_gen=%u dma_a=%d dma_b=%d "
			"hw_a=%u hw_b=%u produced=%u skew=%u emitted=%u errors=%u",
			result, generation, la_generation, la_packed_dma_a_channel,
			la_packed_dma_b_channel, hw_word_indices[0], hw_word_indices[1], produced,
			skew_samples,
			la_stream_ring_emitted_samples, la_stream_ring_error_count + 1U);
		la_stream_ring_error_count++;
		la_stream_ring_request_terminal(result, generation);
		return 0U;
	}
	if (la_stream_pre_trigger.enabled) {
		int ret = la_stream_pre_trigger_process(generation);

		if (ret < 0) {
			LOG_WRN("la pre-trigger terminal: ret=%d gen=%u writer=%llu scan=%llu edge=%llu",
				ret, generation, (unsigned long long)la_stream_ring_progress.writer_seq,
				(unsigned long long)la_stream_pre_trigger.scan_sequence,
				(unsigned long long)la_stream_pre_trigger.edge_sequence);
			la_stream_ring_error_count++;
			la_stream_ring_request_terminal(LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN,
				generation);
			return 0U;
		}
	}

	if (produced > 0U || la_stream_triggered) {
		k_sem_give(&la_stream_ring_consumer_wake_sem);
	}

	if (la_stream_ring_current(generation)) {
		return linkr_debugger_logic_analyzer_ring_poll_interval_ms(
			la_capture.actual_sample_rate_hz, la_stream_ring_plan.sample_capacity,
			la_stream_ring_plan.safety_samples);
	}

	return 0U;
}

static struct la_stream_ring_consume_result la_stream_ring_consume_once(uint32_t generation)
{
	uint64_t writer_seq;
	uint64_t reader_seq;
	uint64_t available;
	uint32_t emit_count;
	uint32_t chunk_limit;
	uint32_t remaining = 0U;
	uint32_t target_samples;
	enum linkr_debugger_la_ring_poll_result terminal_status;
	k_spinlock_key_t key;
	bool current;
	bool sink_active_for_emit;
	bool terminal_pending;

	if (!la_stream_triggered || !la_stream_ring_current(generation)) {
		if (la_stream_ring_take_terminal(generation, &terminal_status)) {
			la_stream_ring_emit_terminal_and_stop(generation, terminal_status);
		}
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_STOP);
	}

	key = k_spin_lock(&la_stream_ring_progress_lock);
	writer_seq = la_stream_ring_progress.writer_seq;
	reader_seq = la_stream_ring_progress.reader_seq;
	k_spin_unlock(&la_stream_ring_progress_lock, key);
	available = writer_seq - reader_seq;
	terminal_pending = la_stream_ring_terminal_waiting(generation);
	if (terminal_pending && available == 0U) {
		if (la_stream_ring_take_terminal(generation, &terminal_status)) {
			la_stream_ring_emit_terminal_and_stop(generation, terminal_status);
		}
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_STOP);
	}
	target_samples = la_stream_ring_bounded_target();
	if (target_samples > 0U) {
		remaining = la_stream_ring_emitted_samples >= target_samples ? 0U :
			target_samples - la_stream_ring_emitted_samples;
	}
	if (target_samples > 0U && remaining == 0U) {
		if (!terminal_pending) {
			la_stream_ring_request_terminal(LINKR_DEBUGGER_LA_RING_POLL_OK,
				generation);
		}
		if (la_stream_ring_take_terminal(generation, &terminal_status)) {
			la_stream_ring_emit_terminal_and_stop(generation, terminal_status);
		}
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_STOP);
	}
	k_mutex_lock(&la_mutex, K_FOREVER);
	current = linkr_debugger_logic_analyzer_stream_generation_current(
		la_stream_ring_active, generation, la_generation);
	sink_active_for_emit = current && la_stream_sink_active;
	chunk_limit = la_stream_ring_plan.chunk_samples;
	if (sink_active_for_emit && la_stream_sink.max_chunk_samples > 0U &&
	    la_stream_sink.max_chunk_samples < chunk_limit) {
		chunk_limit = la_stream_sink.max_chunk_samples;
	}
	k_mutex_unlock(&la_mutex);
	emit_count = linkr_debugger_logic_analyzer_ring_terminal_emit_count(available,
		remaining, chunk_limit, terminal_pending);
	if (emit_count == 0U) {
		if (terminal_pending && la_stream_ring_take_terminal(generation,
		    &terminal_status)) {
			la_stream_ring_emit_terminal_and_stop(generation, terminal_status);
		}
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_STOP);
	}

	if (sink_active_for_emit) {
		return la_stream_ring_consume_sink_once(generation, reader_seq, emit_count);
	}

	struct linkr_debugger_la_stream_chunk chunk;
	linkr_debugger_la_stream_callback_t callback;
	void *user_data;
	uint64_t consume_start_us;
	uint64_t compact_end_us;
	uint64_t callback_start_us;
	uint64_t callback_end_us;
	bool reader_advanced;

	consume_start_us = k_ticks_to_us_floor64(k_uptime_ticks());
	la_stream_ring_record_consume_start(consume_start_us);
	if (!la_stream_ring_prepare_emit(reader_seq, emit_count, generation, &chunk,
	    &consume_start_us)) {
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_STOP);
	}
	compact_end_us = k_ticks_to_us_floor64(k_uptime_ticks());

	k_mutex_lock(&la_mutex, K_FOREVER);
	current = linkr_debugger_logic_analyzer_stream_generation_current(
		la_stream_ring_active, generation, la_generation);
	key = k_spin_lock(&la_stream_ring_progress_lock);
	reader_advanced = linkr_debugger_logic_analyzer_stream_copy_complete_advance_reader(
		&la_stream_ring_progress, &la_stream_ring_metrics, current, generation,
		reader_seq, emit_count);
	k_spin_unlock(&la_stream_ring_progress_lock, key);
	k_mutex_unlock(&la_mutex);
	if (!reader_advanced) {
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_STOP);
	}

	k_mutex_lock(&la_mutex, K_FOREVER);
	current = linkr_debugger_logic_analyzer_stream_generation_current(
		la_stream_ring_active, generation, la_generation);
	callback = la_stream_callback;
	user_data = la_stream_user_data;
	k_mutex_unlock(&la_mutex);
	callback_start_us = k_ticks_to_us_floor64(k_uptime_ticks());
	if (callback != NULL && current) {
		callback(&chunk, user_data);
	}
	callback_end_us = k_ticks_to_us_floor64(k_uptime_ticks());
	la_stream_ring_record_consume_elapsed(
		compact_end_us >= consume_start_us ? compact_end_us - consume_start_us : 0U,
		callback_end_us >= callback_start_us ? callback_end_us - callback_start_us : 0U,
		callback_end_us >= consume_start_us ? callback_end_us - consume_start_us : 0U);

	current = la_stream_ring_current(generation);
	if (!linkr_debugger_logic_analyzer_stream_callback_should_yield_after_chunk(
	    current, emit_count)) {
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_STOP);
	}

	la_stream_ring_emitted_samples += emit_count;
	if (la_stream_ring_bounded_target() > 0U &&
	    la_stream_ring_emitted_samples >= la_stream_ring_bounded_target()) {
		la_stream_ring_request_terminal(LINKR_DEBUGGER_LA_RING_POLL_OK, generation);
	}
	la_stream_ring_record_chunk_complete(callback_end_us);
	return la_stream_ring_consume_yield_result(false);
}

static void la_stream_ring_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		uint32_t generation;
		uint32_t poll_interval_ms;

		(void)k_sem_take(&la_stream_ring_wake_sem, K_FOREVER);
		while (true) {
			k_mutex_lock(&la_mutex, K_FOREVER);
			generation = la_stream_ring_progress.generation;
			if (!linkr_debugger_logic_analyzer_stream_generation_current(
			    la_stream_ring_active, generation, la_generation) ||
			    la_packed_dma_a_channel < 0) {
				la_stream_ring_signal_idle_locked();
				k_mutex_unlock(&la_mutex);
				break;
			}
			la_stream_ring_thread_busy = true;
			k_mutex_unlock(&la_mutex);

			poll_interval_ms = la_stream_ring_poll_once(generation);

			k_mutex_lock(&la_mutex, K_FOREVER);
			if (poll_interval_ms == 0U ||
			    !linkr_debugger_logic_analyzer_stream_generation_current(
			    la_stream_ring_active, generation, la_generation)) {
				la_stream_ring_signal_idle_locked();
				k_mutex_unlock(&la_mutex);
				break;
			}
			k_mutex_unlock(&la_mutex);

			(void)k_sem_take(&la_stream_ring_wake_sem, K_MSEC(poll_interval_ms));
		}
	}
}

static void la_stream_ring_consumer_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		uint32_t generation;
		struct la_stream_ring_consume_result consume_result;

		(void)k_sem_take(&la_stream_ring_consumer_wake_sem, K_FOREVER);
		while (true) {
			k_mutex_lock(&la_mutex, K_FOREVER);
			generation = la_stream_ring_progress.generation;
			if (!linkr_debugger_logic_analyzer_stream_generation_current(
			    la_stream_ring_active, generation, la_generation)) {
				la_stream_ring_consumer_signal_idle_locked();
				k_mutex_unlock(&la_mutex);
				break;
			}
			la_stream_ring_consumer_thread_busy = true;
			k_mutex_unlock(&la_mutex);

			consume_result = la_stream_ring_consume_once(generation);
			if (consume_result.action == LA_STREAM_RING_CONSUME_STOP) {
				la_stream_ring_clear_consumer_gap();
				k_mutex_lock(&la_mutex, K_FOREVER);
				if (!linkr_debugger_logic_analyzer_stream_generation_current(
				    la_stream_ring_active, generation, la_generation)) {
					la_stream_ring_consumer_signal_idle_locked();
				}
				k_mutex_unlock(&la_mutex);
				break;
			}
			if (consume_result.action == LA_STREAM_RING_CONSUME_YIELD_CONTINUE) {
				int64_t yield_start_ticks = k_uptime_ticks();

				/* Equal-priority handoff point for legacy WS/TCP callback senders,
				 * and bounded handoff point for sink transports that explicitly ask.
				 */
				k_yield();
				la_stream_ring_record_yield_resume(
					k_ticks_to_us_floor64(k_uptime_ticks() - yield_start_ticks),
					consume_result.sink_handoff);
			}
		}
	}
}

static int la_start_stream_ring_dma_locked(void)
{
	PIO pio = pio_rpi_pico_get_pio(la_pio_dev);
	dma_channel_config dma_a_cfg;
	dma_channel_config dma_b_cfg;
	int ret;

#if !defined(DMA_CH0_CTRL_TRIG_EN_BITS) || \
	!defined(LINKR_DEBUGGER_LA_HAS_RING_DMA_ENDLESS)
	LOG_ERR("la ring setup failed: missing RP2350 DMA ring support ret=%d", -ENOTSUP);
	return -ENOTSUP;
#else
	la_packed_dma_a_channel = dma_request_channel(la_dma_dev, NULL);
	if (la_packed_dma_a_channel < 0) {
		LOG_ERR("la ring setup failed: dma_request_channel ret=%d", la_packed_dma_a_channel);
		return la_packed_dma_a_channel;
	}
	if (la_stream_ring_plan.lane_count > 1U) {
		la_packed_dma_b_channel = dma_request_channel(la_dma_dev, NULL);
		if (la_packed_dma_b_channel < 0) {
			ret = la_packed_dma_b_channel;
			la_packed_burst_release_dma_locked(&la_packed_dma_a_channel);
			return ret;
		}
	}

	memset(la_stream_packed_ring_words, 0,
		LINKR_DEBUGGER_LA_RING_SAMPLES * sizeof(la_stream_packed_ring_words[0]));
	la_stream_ring_sink_stall_since_us = 0U;
	{
		k_spinlock_key_t key = k_spin_lock(&la_stream_ring_progress_lock);

		memset(&la_stream_ring_progress, 0, sizeof(la_stream_ring_progress));
		la_stream_ring_progress.generation = la_generation;
		memset(la_stream_ring_lane_last_hw_index, 0, sizeof(la_stream_ring_lane_last_hw_index));
		memset(la_stream_ring_lane_writer_seq, 0, sizeof(la_stream_ring_lane_writer_seq));
		k_spin_unlock(&la_stream_ring_progress_lock, key);
	}

	if (((uintptr_t)la_stream_ring_lane_words(0U) &
	    (la_stream_ring_plan.lanes[0].byte_count - 1U)) != 0U) {
		ret = -EINVAL;
		goto fail;
	}
	dma_a_cfg = dma_channel_get_default_config((uint)la_packed_dma_a_channel);
	channel_config_set_transfer_data_size(&dma_a_cfg, DMA_SIZE_32);
	channel_config_set_read_increment(&dma_a_cfg, false);
	channel_config_set_write_increment(&dma_a_cfg, true);
	channel_config_set_ring(&dma_a_cfg, true, la_stream_ring_plan.lanes[0].ring_size_bits);
	channel_config_set_dreq(&dma_a_cfg, pio_get_dreq(pio, (uint)la_packed_sm_a, false));
	dma_channel_set_irq0_enabled((uint)la_packed_dma_a_channel, false);
	dma_channel_configure((uint)la_packed_dma_a_channel, &dma_a_cfg,
		la_stream_ring_lane_words(0U), &pio->rxf[la_packed_sm_a],
		la_ring_transfer_count(), false);

	if (la_stream_ring_plan.lane_count > 1U) {
		if (((uintptr_t)la_stream_ring_lane_words(1U) &
		    (la_stream_ring_plan.lanes[1].byte_count - 1U)) != 0U) {
			ret = -EINVAL;
			goto fail;
		}
		dma_b_cfg = dma_channel_get_default_config((uint)la_packed_dma_b_channel);
		channel_config_set_transfer_data_size(&dma_b_cfg, DMA_SIZE_32);
		channel_config_set_read_increment(&dma_b_cfg, false);
		channel_config_set_write_increment(&dma_b_cfg, true);
		channel_config_set_ring(&dma_b_cfg, true,
			la_stream_ring_plan.lanes[1].ring_size_bits);
		channel_config_set_dreq(&dma_b_cfg,
			pio_get_dreq(pio, (uint)la_packed_sm_b, false));
		dma_channel_set_irq0_enabled((uint)la_packed_dma_b_channel, false);
		dma_channel_configure((uint)la_packed_dma_b_channel, &dma_b_cfg,
			la_stream_ring_lane_words(1U), &pio->rxf[la_packed_sm_b],
			la_ring_transfer_count(), false);
	}

	la_stream_ring_active = true;
	dma_channel_start((uint)la_packed_dma_a_channel);
	if (la_stream_ring_plan.lane_count > 1U) {
		dma_channel_start((uint)la_packed_dma_b_channel);
	}
	{
		k_spinlock_key_t key = k_spin_lock(&la_stream_ring_progress_lock);

		la_stream_ring_progress.initialized = true;
		la_stream_ring_progress.writer_seq = 0U;
		la_stream_ring_progress.reader_seq = 0U;
		la_stream_ring_progress.last_poll_time_us = k_ticks_to_us_floor64(k_uptime_ticks());
		k_spin_unlock(&la_stream_ring_progress_lock, key);
	}
	return 0;

fail:
	LOG_ERR("la ring setup failed: ret=%d", ret);
	la_packed_burst_release_dma_locked(&la_packed_dma_a_channel);
	la_packed_burst_release_dma_locked(&la_packed_dma_b_channel);
	return ret;
#endif
}

static int la_start_stream_common(
	const struct linkr_debugger_la_config *config,
	linkr_debugger_la_stream_callback_t callback,
	void *user_data,
	const struct linkr_debugger_la_stream_sink *sink)
{
	struct linkr_debugger_la_start_prepare next;
	int ret;

	if (config == NULL || (callback == NULL && sink == NULL)) {
		return -EINVAL;
	}

	ret = linkr_debugger_logic_analyzer_validate_stream_config(config);
	if (ret < 0) {
		return ret;
	}
	if (sink != NULL) {
		ret = linkr_debugger_logic_analyzer_stream_sink_validate(config, sink);
		if (ret < 0) {
			return ret;
		}
	}

	k_mutex_lock(&la_mutex, K_FOREVER);
	if (la_start_prepare_is_active()) {
		k_mutex_unlock(&la_mutex);
		return -EBUSY;
	}
	ret = la_start_prepare_fill_common(&next, config, false, sink);
	if (ret < 0) {
		k_mutex_unlock(&la_mutex);
		return ret;
	}
	if (next.plan.legacy_adapter ==
	    LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST) {
		k_mutex_unlock(&la_mutex);
		return -EINVAL;
	}
	la_start_prepare_active = next;
	ret = la_prepare_stream_common_locked(&la_start_prepare_active, callback, user_data);
	if (ret < 0) {
		la_start_prepare_clear_active_locked();
		k_mutex_unlock(&la_mutex);
		return ret;
	}
	la_start_prepare_active.hardware_prepared = true;
	la_start_prepared_stream_go_locked();
	la_start_prepare_clear_active_locked();
	k_mutex_unlock(&la_mutex);
	return 0;
}

int linkr_debugger_logic_analyzer_start_stream(
	const struct linkr_debugger_la_config *config,
	linkr_debugger_la_stream_callback_t callback,
	void *user_data)
{
	if (callback == NULL) {
		return -EINVAL;
	}

	return la_start_stream_common(config, callback, user_data, NULL);
}

int linkr_debugger_logic_analyzer_start_stream_sink(
	const struct linkr_debugger_la_config *config,
	const struct linkr_debugger_la_stream_sink *sink)
{
	if (sink == NULL) {
		return -EINVAL;
	}

	return la_start_stream_common(config, NULL, NULL, sink);
}

int linkr_debugger_logic_analyzer_stop_stream(void)
{
	la_stream_stop_and_cleanup(LINKR_DEBUGGER_LA_STATE_IDLE);
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
	{
		struct linkr_debugger_la_ring_metrics metrics;
		k_spinlock_key_t key = k_spin_lock(&la_stream_ring_progress_lock);

		metrics = la_stream_ring_metrics;
		k_spin_unlock(&la_stream_ring_progress_lock, key);
		out->stream_ring_max_poll_gap_us = metrics.max_poll_gap_us;
		out->stream_ring_max_unread_samples = metrics.max_unread_samples;
		out->stream_ring_max_emit_us = metrics.max_emit_us;
		out->stream_ring_max_compact_us = metrics.max_compact_us;
		out->stream_ring_total_compact_us = metrics.total_compact_us;
		out->stream_ring_max_callback_us = metrics.max_callback_us;
		out->stream_ring_total_callback_us = metrics.total_callback_us;
		out->stream_ring_total_consume_us = metrics.total_consume_us;
		out->stream_ring_consume_chunks = metrics.consume_chunk_count;
	}
	out->pre_write_index = la_pre_trigger_write_index;
	out->pre_post_remaining = la_pre_trigger_post_remaining;
	out->stream_values_or = la_stream_values_or;
	out->stream_values_and = la_stream_values_and;
	out->pre_active = la_pre_trigger_active;
	out->pre_triggered = la_pre_trigger_triggered;
	out->stream_active = la_stream_active;
	k_mutex_unlock(&la_mutex);
}

bool linkr_debugger_logic_analyzer_is_stream_triggered(void)
{
	return la_stream_triggered;
}

int linkr_debugger_logic_analyzer_stop_ring(void)
{
	return 0;
}

bool linkr_debugger_logic_analyzer_is_ring_active(void)
{
	return false;
}

bool linkr_debugger_logic_analyzer_is_ring_triggered(void)
{
	return false;
}

uint32_t linkr_debugger_logic_analyzer_ring_trigger_pos(void)
{
	return 0U;
}

#endif
