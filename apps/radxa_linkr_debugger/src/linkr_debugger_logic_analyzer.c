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
#define LINKR_DEBUGGER_LA_PIO_IN_PINS_32 0x4000U
#define LINKR_DEBUGGER_LA_PIO_WAIT_IRQ0 0x20c0U
#define LINKR_DEBUGGER_LA_STREAM_WAIT_PRODUCER BIT(0)
#define LINKR_DEBUGGER_LA_STREAM_WAIT_CONSUMER BIT(1)
#define LINKR_DEBUGGER_LA_DMA_ABORT_TIMEOUT_US 1000U
#define LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE 3072U
/* Must outrank CONFIG_NET_TCP_WORKER_PRIO=2: at 1 MHz the usable ring window
 * is 7.168 ms. The producer only polls DMA/trigger state and wakes the lower
 * priority consumer; websocket_send_msg is not called from it.
 */
#define LINKR_DEBUGGER_LA_STREAM_THREAD_PRIORITY K_PRIO_PREEMPT(1)
#define LINKR_DEBUGGER_LA_STREAM_CONSUMER_SINK_PRIORITY K_PRIO_PREEMPT(7)
#define LINKR_DEBUGGER_LA_STREAM_CONSUMER_PRIORITY K_PRIO_PREEMPT(8)

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

	if (linkr_debugger_logic_analyzer_ring_seq_overran(progress->writer_seq,
	    progress->reader_seq, ring_samples, safety_margin)) {
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

static void la_stream_sink_write_single_raw_span(
	const uint32_t *raw,
	uint8_t *payload,
	uint32_t count,
	uint8_t bit_offset,
	uint16_t *or_acc,
	uint16_t *and_acc)
{
	for (uint32_t i = 0U; i < count; i++) {
		uint8_t sample = (uint8_t)((raw[i] >> bit_offset) & 0x01U);

		payload[i] = sample;
		*or_acc |= sample;
		*and_acc &= sample;
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

int linkr_debugger_logic_analyzer_stream_sink_write_raw_payload(
	const struct linkr_debugger_la_config *config,
	const uint32_t *raw_ring,
	uint32_t ring_samples,
	uint64_t first_seq,
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

	if (values_or != NULL) {
		*values_or = 0U;
	}
	if (values_and != NULL) {
		*values_and = 0xffffU;
	}
	if (config == NULL || raw_ring == NULL || payload == NULL || ring_samples == 0U ||
	    sample_count == 0U || bytes_per_sample == 0U ||
	    bytes_per_sample > sizeof(uint16_t)) {
		return -EINVAL;
	}
	needed = (size_t)sample_count * bytes_per_sample;
	if (needed > payload_capacity) {
		return -ENOSPC;
	}
	if (bytes_per_sample == 1U && la_active_pin_count(config) == 1U) {
		uint8_t selected_pin = la_pin_at(config, 0U);
		uint8_t bit_offset;
		uint32_t start_index;
		uint32_t first_count;
		uint32_t remaining;

		if (selected_pin < config->pin_base) {
			return -EINVAL;
		}
		bit_offset = (uint8_t)(selected_pin - config->pin_base);
		if (bit_offset >= 32U) {
			return -EINVAL;
		}

		start_index = (uint32_t)(first_seq % ring_samples);
		first_count = ring_samples - start_index;
		if (first_count > sample_count) {
			first_count = sample_count;
		}
		la_stream_sink_write_single_raw_span(&raw_ring[start_index], payload,
			first_count, bit_offset, &or_acc, &and_acc);
		remaining = sample_count - first_count;
		if (remaining > 0U) {
			la_stream_sink_write_single_raw_span(raw_ring, payload + first_count,
				remaining, bit_offset, &or_acc, &and_acc);
		}

		if (values_or != NULL) {
			*values_or = or_acc;
		}
		if (values_and != NULL) {
			*values_and = and_acc;
		}
		return 0;
	}

	for (uint32_t i = 0U; i < sample_count; i++) {
		uint32_t raw_index = (uint32_t)((first_seq + i) % ring_samples);
		uint16_t sample = linkr_debugger_logic_analyzer_compress_raw_sample(
			raw_ring[raw_index], config);
		uint8_t *dst = &payload[(size_t)i * bytes_per_sample];

		or_acc |= sample;
		and_acc &= sample;
		for (uint8_t byte = 0U; byte < bytes_per_sample; byte++) {
			dst[byte] = (uint8_t)((sample >> (byte * 8U)) & 0xffU);
		}
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
	if (config->pre_samples > 0U &&
	    config->sample_rate_hz > LINKR_DEBUGGER_LA_MAX_PRE_TRIGGER_SAMPLE_RATE_HZ) {
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
	if (config->pre_samples != 0U) {
		return -EINVAL;
	}
	if (config->post_samples > UINT16_MAX) {
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

bool linkr_debugger_logic_analyzer_finite_stream_eligible(
	const struct linkr_debugger_la_config *config)
{
	return config != NULL &&
		config->trigger <= LINKR_DEBUGGER_LA_TRIGGER_EITHER &&
		config->pre_samples == 0U &&
		config->post_samples > 0U &&
		config->post_samples <= LINKR_DEBUGGER_LA_FINITE_GATED_MAX_SAMPLES;
}

bool linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(
	const struct linkr_debugger_la_config *config)
{
	return config != NULL &&
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
#if !defined(LINKR_DEBUGGER_LA_HOST_TEST)

static K_MUTEX_DEFINE(la_mutex);

static struct linkr_debugger_la_capture la_capture;
static bool la_initialized;

#if defined(CONFIG_SOC_SERIES_RP2350)
static uint32_t la_stream_ring_raw[LINKR_DEBUGGER_LA_RING_SAMPLES]
	__aligned(LINKR_DEBUGGER_LA_RING_BUFFER_BYTES);
static uint32_t la_raw_samples[LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES] __aligned(4);
static struct linkr_debugger_la_sample la_samples[LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES];
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

static size_t la_trigger_sm;
static uint16_t la_trigger_instructions[5];
static struct pio_program la_trigger_program = {
	.instructions = la_trigger_instructions,
	.origin = -1,
};
static bool la_trigger_sm_claimed;
static bool la_trigger_program_loaded;
static int la_trigger_offset = -1;

static uint32_t *const la_stream_raw_a = &la_stream_ring_raw[0];
static uint32_t *const la_stream_raw_b =
	&la_stream_ring_raw[LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES];
static uint16_t la_stream_scratch[LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES] __aligned(4);
static uint16_t *const la_stream_buf_a = la_stream_scratch;
static uint16_t *const la_stream_buf_b = la_stream_scratch;
static volatile bool la_stream_active;
static volatile bool la_stream_buf_a_ready;
static volatile bool la_stream_buf_b_ready;
static volatile bool la_stream_use_buf_a;
static volatile uint32_t la_stream_sequence;
static linkr_debugger_la_stream_callback_t la_stream_callback;
static void *la_stream_user_data;
static struct linkr_debugger_la_stream_sink la_stream_sink;
static bool la_stream_sink_active;
static uint32_t la_stream_emit_div;
static uint32_t la_stream_block_index;

#define LINKR_DEBUGGER_LA_STREAM_EMIT_TARGET_HZ 200U
static struct linkr_debugger_la_config la_stream_config;
static struct k_work la_stream_work;
static struct dma_config la_stream_dma_config;
static struct dma_block_config la_stream_dma_block;

static uint16_t *const la_stream_ring_values = la_stream_scratch;
static struct linkr_debugger_la_ring_progress la_stream_ring_progress;
static struct linkr_debugger_la_ring_metrics la_stream_ring_metrics;
static struct k_spinlock la_stream_ring_progress_lock;
static bool la_stream_ring_active;
static bool la_stream_ring_backend;
static bool la_stream_legacy_backend;
static bool la_stream_finite_backend;
static uint32_t la_stream_ring_error_count;
static uint32_t la_stream_ring_emitted_samples;
static bool la_stream_ring_terminal_pending;
static bool la_stream_ring_terminal_emitted;
static enum linkr_debugger_la_ring_poll_result la_stream_ring_terminal_status;
static struct k_work la_stream_finite_work;
static volatile int la_stream_finite_dma_status;
static volatile uint32_t la_stream_finite_done_generation;
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
static volatile bool la_stream_triggered;
static volatile uint16_t la_stream_values_or;
static volatile uint16_t la_stream_values_and;

static int la_arm_pre_trigger_locked(const struct linkr_debugger_la_config *config);
static void la_cleanup_locked(void);
static void la_stream_ring_thread_fn(void *p1, void *p2, void *p3);
static void la_stream_ring_consumer_thread_fn(void *p1, void *p2, void *p3);
static void la_stream_finite_work_handler(struct k_work *work);

static void la_stream_teardown_locked(void)
{
	la_stream_active = false;
	la_stream_ring_active = false;
	la_stream_finite_backend = false;
	la_pre_trigger_active = false;
	la_pre_trigger_triggered = false;
	la_pre_trigger_have_prev = false;
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
	if (la_stream_ring_backend && la_stream_ring_thread_started) {
		k_sem_give(&la_stream_ring_wake_sem);
	}
	if (la_stream_ring_backend && la_stream_ring_consumer_thread_started) {
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

static void la_stream_ring_wait_idle_if_needed(bool ring_backend)
{
	uint8_t wait_mask;

	if (!ring_backend) {
		return;
	}

	wait_mask = linkr_debugger_logic_analyzer_stream_idle_wait_mask(
		la_stream_ring_thread_started && k_current_get() != &la_stream_ring_thread,
		la_stream_ring_thread_busy,
		la_stream_ring_consumer_thread_started &&
			k_current_get() != &la_stream_ring_consumer_thread,
		la_stream_ring_consumer_thread_busy);
	if ((wait_mask & LINKR_DEBUGGER_LA_STREAM_WAIT_PRODUCER) != 0U) {
		(void)k_sem_take(&la_stream_ring_idle_sem, K_FOREVER);
	}
	if ((wait_mask & LINKR_DEBUGGER_LA_STREAM_WAIT_CONSUMER) != 0U) {
		(void)k_sem_take(&la_stream_ring_consumer_idle_sem, K_FOREVER);
	}
}

static void la_stream_stop_and_cleanup(enum linkr_debugger_la_state state)
{
	bool ring_backend;
	bool finite_backend;

	k_mutex_lock(&la_mutex, K_FOREVER);
	ring_backend = la_stream_ring_backend;
	finite_backend = la_stream_finite_backend;
	la_stream_request_inactive_locked();
	k_mutex_unlock(&la_mutex);

	la_stream_ring_wait_idle_if_needed(ring_backend);
	if (finite_backend) {
		struct k_work_sync sync;

		(void)k_work_cancel_sync(&la_stream_finite_work, &sync);
	}
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
	pio = pio_rpi_pico_get_pio(la_pio_dev);
	pio_interrupt_clear(pio, 0U);
	if (la_sm_claimed) {
		pio_sm_set_enabled(pio, (uint)la_pio_sm, false);
		pio_sm_clear_fifos(pio, (uint)la_pio_sm);
	}
	if (la_dma_channel >= 0 && device_is_ready(la_dma_dev)) {
		if (la_stream_ring_backend) {
			uint32_t abort_wait_us = 0U;

			dma_channel_set_irq0_enabled((uint)la_dma_channel, false);
			#if defined(DMA_CH0_CTRL_TRIG_EN_BITS)
			hw_clear_bits(&dma_hw->ch[la_dma_channel].ctrl_trig, DMA_CH0_CTRL_TRIG_EN_BITS);
			#endif
			dma_channel_abort((uint)la_dma_channel);
			while (dma_channel_is_busy((uint)la_dma_channel) &&
			       abort_wait_us < LINKR_DEBUGGER_LA_DMA_ABORT_TIMEOUT_US) {
				k_busy_wait(1U);
				abort_wait_us++;
			}
			if (dma_channel_is_busy((uint)la_dma_channel)) {
				LOG_ERR("la ring DMA abort timed out after %u us on channel %d",
					LINKR_DEBUGGER_LA_DMA_ABORT_TIMEOUT_US, la_dma_channel);
			}
			dma_hw->ints0 = BIT(la_dma_channel);
		} else {
			(void)dma_stop(la_dma_dev, (uint32_t)la_dma_channel);
		}
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
	la_capture_active = false;
	la_stream_ring_active = false;
	la_stream_ring_backend = false;
	la_stream_legacy_backend = false;
	la_stream_finite_backend = false;
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

static void la_stream_finite_dma_callback(const struct device *dev, void *user_data,
	uint32_t channel, int status)
{
	uint32_t done_generation = (uint32_t)(uintptr_t)user_data;

	ARG_UNUSED(dev);
	ARG_UNUSED(channel);

	if (done_generation != la_generation) {
		return;
	}

	la_stream_finite_done_generation = done_generation;
	la_stream_finite_dma_status = status;
	(void)k_work_submit(&la_stream_finite_work);
}

static uint8_t la_trigger_program_length(enum linkr_debugger_la_trigger_type trigger)
{
	if (trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE) {
		return 0U;
	}

	return 3U;
}

static uint8_t la_program_reservation_length(
	enum linkr_debugger_la_trigger_type trigger,
	bool wait_for_trigger_irq)
{
	if (wait_for_trigger_irq) {
		return 2U + la_trigger_program_length(trigger);
	}
	if (trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE) {
		return 1U;
	}

	return 1U + la_trigger_program_length(trigger);
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

static int la_build_program(bool wait_for_trigger_irq, uint8_t offset,
	struct linkr_debugger_la_pio_program_layout *layout)
{
	int ret = linkr_debugger_logic_analyzer_build_capture_program(wait_for_trigger_irq,
		offset, la_program_instructions, ARRAY_SIZE(la_program_instructions), layout);

	if (ret == 0) {
		la_program.length = layout->length;
	}
	return ret;
}

static int la_configure_pio_locked(const struct linkr_debugger_la_config *config,
	bool wait_for_trigger_irq)
{
	const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	uint8_t active_pin_count = la_active_pin_count(config);
	uint32_t actual_rate = la_actual_rate_from_hw(config->sample_rate_hz);
	uint64_t div256;
	pio_sm_config sm_config;
	struct linkr_debugger_la_pio_program_layout program_layout;
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

	la_pio_offset = la_find_program_offset(pio,
		la_program_reservation_length(config->trigger, wait_for_trigger_irq));
	if (la_pio_offset < 0) {
		return la_pio_offset;
	}
	ret = la_build_program(wait_for_trigger_irq, (uint8_t)la_pio_offset,
		&program_layout);
	if (ret < 0) {
		return ret;
	}
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
	sm_config_set_in_pins(&sm_config, config->pin_base);
	sm_config_set_in_pin_count(&sm_config, 32U);
	sm_config_set_in_shift(&sm_config, true, true, 32U);
	sm_config_set_fifo_join(&sm_config, PIO_FIFO_JOIN_RX);
	sm_config_set_wrap(&sm_config, (uint)la_pio_offset + program_layout.wrap_target,
		(uint)la_pio_offset + program_layout.wrap);

	ret = pio_sm_init(pio, (uint)la_pio_sm, (uint)la_pio_offset, &sm_config);
	if (ret < 0) {
		return ret;
	}
	pio_sm_clear_fifos(pio, (uint)la_pio_sm);
	pio_sm_restart(pio, (uint)la_pio_sm);
	pio_sm_clkdiv_restart(pio, (uint)la_pio_sm);
	la_capture.actual_sample_rate_hz = actual_rate;
	la_capture.sample_period_ps = linkr_debugger_logic_analyzer_sample_period_ps(actual_rate);

	if (config->trigger != LINKR_DEBUGGER_LA_TRIGGER_NONE) {
		uint8_t trig_pin = la_pin_at(config, config->trigger_pin);
		enum linkr_debugger_la_trigger_type effective_trigger = config->trigger;

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
		}
		la_trigger_instructions[2] = (uint16_t)pio_encode_irq_set(false, 0U);

		ret = pio_rpi_pico_allocate_sm(la_pio_dev, &la_trigger_sm);
		if (ret < 0) {
			return ret;
		}
		la_trigger_sm_claimed = true;

		pio_sm_config sm2_cfg = pio_get_default_sm_config();
		sm_config_set_clkdiv_int_frac8(&sm2_cfg, (uint32_t)(div256 / 256ULL),
			(uint8_t)(div256 % 256ULL));

		la_trigger_program.length = la_trigger_program_length(config->trigger);
		la_trigger_offset = -1;
		for (uint8_t o = (uint8_t)(la_pio_offset + la_program.length);
		     o <= 32U - la_trigger_program.length; o++) {
			if (pio_can_add_program_at_offset(pio, &la_trigger_program, o)) {
				la_trigger_offset = (int)o;
				break;
			}
		}
		if (la_trigger_offset < 0) {
			return -EBUSY;
		}
		ret = pio_add_program_at_offset(pio, &la_trigger_program, (uint)la_trigger_offset);
		if (ret < 0) {
			return ret;
		}
		la_trigger_offset = ret;
		la_trigger_program_loaded = true;
		sm_config_set_wrap(&sm2_cfg, (uint)la_trigger_offset + la_trigger_program.length - 1U,
			(uint)la_trigger_offset + la_trigger_program.length - 1U);

		ret = pio_sm_init(pio, (uint)la_trigger_sm, (uint)la_trigger_offset, &sm2_cfg);
		if (ret < 0) {
			return ret;
		}
		pio_sm_clear_fifos(pio, (uint)la_trigger_sm);
		pio_sm_restart(pio, (uint)la_trigger_sm);
	}

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

bool linkr_debugger_logic_analyzer_stream_sink_should_explicit_yield(
	bool handoff_requested, uint64_t unread_samples)
{
	ARG_UNUSED(handoff_requested);
	ARG_UNUSED(unread_samples);

	return false;
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
#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
	k_work_init(&la_finalize_work, la_finalize_work_handler);
	k_work_init(&la_stream_finite_work, la_stream_finite_work_handler);
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
	memset(la_raw_samples, 0,
		LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES * sizeof(la_raw_samples[0]));
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
	la_dma_status = 0;

	ret = la_configure_pio_locked(&normalized, false);
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
#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)
	la_stream_stop_and_cleanup(LINKR_DEBUGGER_LA_STATE_IDLE);
#else
	k_mutex_lock(&la_mutex, K_FOREVER);
	la_capture.state = LINKR_DEBUGGER_LA_STATE_IDLE;
	la_capture.samples = NULL;
	k_mutex_unlock(&la_mutex);
#endif
	return 0;
}

#if defined(CONFIG_SOC_SERIES_RP2350) && !defined(LINKR_DEBUGGER_LA_HOST_TEST)

static void la_pre_trigger_finalize_handler(struct k_work *work)
{
	bool ring_backend;

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

	ring_backend = la_stream_ring_backend;
	la_stream_request_inactive_locked();
	k_mutex_unlock(&la_mutex);

	la_stream_ring_wait_idle_if_needed(ring_backend);

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
	chunk.status = LINKR_DEBUGGER_LA_RING_POLL_OK;
	la_stream_chunk_count++;

	if (la_stream_callback != NULL) {
		la_stream_callback(&chunk, la_stream_user_data);
	}
}

static void la_stream_process_block(uint32_t *raw_buf, uint16_t *out_buf)
{
	uint32_t block_index = la_stream_block_index++;

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

	if (!la_stream_triggered) {
		PIO pio = pio_rpi_pico_get_pio(la_pio_dev);
		if (pio_interrupt_get(pio, 0U)) {
			la_stream_triggered = true;
			pio_interrupt_clear(pio, 0U);
		}
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

static bool la_stream_finite_current(uint32_t generation)
{
	bool current;

	k_mutex_lock(&la_mutex, K_FOREVER);
	current = la_stream_finite_backend &&
		linkr_debugger_logic_analyzer_stream_generation_current(
			la_stream_active, generation, la_generation);
	k_mutex_unlock(&la_mutex);
	return current;
}

static int la_start_stream_finite_dma_locked(uint32_t sample_count)
{
	PIO pio = pio_rpi_pico_get_pio(la_pio_dev);
	uint32_t block_size = linkr_debugger_logic_analyzer_dma_block_size(sample_count);
	int ret;

	if (block_size == 0U) {
		return -EINVAL;
	}

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

	la_dma_config.dma_slot = RPI_PICO_DMA_DREQ_TO_SLOT(
		pio_get_dreq(pio, (uint)la_pio_sm, false));
	la_dma_config.channel_direction = PERIPHERAL_TO_MEMORY;
	la_dma_config.source_data_size = LINKR_DEBUGGER_LA_RAW_SAMPLE_BYTES;
	la_dma_config.dest_data_size = LINKR_DEBUGGER_LA_RAW_SAMPLE_BYTES;
	la_dma_config.source_burst_length = 1U;
	la_dma_config.dest_burst_length = 1U;
	la_dma_config.block_count = 1U;
	la_dma_config.head_block = &la_dma_block;
	la_dma_config.user_data = (void *)(uintptr_t)la_generation;
	la_dma_config.dma_callback = la_stream_finite_dma_callback;
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

static enum linkr_debugger_la_ring_poll_result la_stream_finite_emit_sink(
	uint32_t generation,
	uint32_t sample_count,
	const struct linkr_debugger_la_config *config,
	const struct linkr_debugger_la_stream_sink *sink)
{
	struct linkr_debugger_la_stream_sink_lease lease;
	struct linkr_debugger_la_stream_sink_commit commit;
	uint16_t values_or = 0U;
	uint16_t values_and = 0xffffU;
	int ret;

	ret = linkr_debugger_logic_analyzer_stream_sink_lease_payload(sink,
		sample_count, &lease);
	if (ret < 0) {
		return LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN;
	}

	ret = linkr_debugger_logic_analyzer_stream_sink_write_raw_payload(config,
		la_raw_samples, sample_count, 0U, sample_count, sink->bytes_per_sample,
		lease.payload, lease.capacity, &values_or, &values_and);
	if (ret < 0) {
		linkr_debugger_logic_analyzer_stream_sink_abort_payload(sink, &lease);
		return LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN;
	}
	if (!la_stream_finite_current(generation)) {
		linkr_debugger_logic_analyzer_stream_sink_abort_payload(sink, &lease);
		return LINKR_DEBUGGER_LA_RING_POLL_OK;
	}

	memset(&commit, 0, sizeof(commit));
	commit.token = lease.token;
	commit.sequence = la_stream_sequence;
	commit.sample_count = sample_count;
	commit.timestamp_us = 0U;
	commit.bytes_per_sample = sink->bytes_per_sample;
	commit.payload_len = (size_t)sample_count * sink->bytes_per_sample;
	ret = linkr_debugger_logic_analyzer_stream_sink_commit_payload(sink, &commit);
	if (ret < 0) {
		linkr_debugger_logic_analyzer_stream_sink_abort_payload(sink, &lease);
		return LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN;
	}

	if (!linkr_debugger_logic_analyzer_stream_sink_allows_protocol_update(
	    la_stream_finite_current(generation), sample_count)) {
		return LINKR_DEBUGGER_LA_RING_POLL_OK;
	}

	la_stream_values_or |= values_or;
	la_stream_values_and &= values_and;
	la_stream_chunk_count++;
	la_stream_sequence++;
	la_stream_ring_emitted_samples += sample_count;
	return LINKR_DEBUGGER_LA_RING_POLL_OK;
}

static void la_stream_finite_emit_callback(uint32_t generation,
	uint32_t sample_count,
	const struct linkr_debugger_la_config *config,
	linkr_debugger_la_stream_callback_t callback,
	void *user_data)
{
	struct linkr_debugger_la_stream_chunk chunk;
	uint16_t or_acc = 0U;
	uint16_t and_acc = 0xffffU;

	for (uint32_t i = 0U; i < sample_count; i++) {
		la_stream_scratch[i] = linkr_debugger_logic_analyzer_compress_raw_sample(
			la_raw_samples[i], config);
		or_acc |= la_stream_scratch[i];
		and_acc &= la_stream_scratch[i];
	}

	la_stream_values_or |= or_acc;
	la_stream_values_and &= and_acc;
	memset(&chunk, 0, sizeof(chunk));
	chunk.sequence = la_stream_sequence++;
	chunk.sample_count = sample_count;
	chunk.timestamp_us = 0U;
	chunk.values = la_stream_scratch;
	chunk.status = LINKR_DEBUGGER_LA_RING_POLL_OK;
	la_stream_chunk_count++;

	if (callback != NULL && la_stream_finite_current(generation)) {
		callback(&chunk, user_data);
	}
	if (linkr_debugger_logic_analyzer_stream_callback_allows_protocol_update(
	    la_stream_finite_current(generation), sample_count)) {
		la_stream_ring_emitted_samples += sample_count;
	}
}

static void la_stream_finite_emit_terminal(
	enum linkr_debugger_la_ring_poll_result status,
	uint32_t generation,
	bool sink_active,
	const struct linkr_debugger_la_stream_sink *sink,
	linkr_debugger_la_stream_callback_t callback,
	void *user_data)
{
	struct linkr_debugger_la_stream_chunk chunk;
	uint32_t sequence;

	if (!la_stream_finite_current(generation)) {
		return;
	}
	sequence = la_stream_sequence++;

	if (sink_active) {
		linkr_debugger_logic_analyzer_stream_sink_notify_terminal(sink, status, sequence);
		return;
	}
	if (callback == NULL) {
		return;
	}

	memset(&chunk, 0, sizeof(chunk));
	chunk.sequence = sequence;
	chunk.status = status;
	callback(&chunk, user_data);
}

static void la_stream_finite_work_handler(struct k_work *work)
{
	uint32_t generation;
	uint32_t sample_count;
	struct linkr_debugger_la_config config;
	struct linkr_debugger_la_stream_sink sink;
	linkr_debugger_la_stream_callback_t callback;
	void *user_data;
	bool sink_active;
	int status;
	enum linkr_debugger_la_ring_poll_result terminal_status;

	ARG_UNUSED(work);

	generation = la_stream_finite_done_generation;
	status = la_stream_finite_dma_status;
	k_mutex_lock(&la_mutex, K_FOREVER);
	if (!la_stream_finite_backend || !linkr_debugger_logic_analyzer_stream_generation_current(
	    la_stream_active, generation, la_generation)) {
		k_mutex_unlock(&la_mutex);
		return;
	}
	config = la_stream_config;
	sample_count = config.post_samples;
	callback = la_stream_callback;
	user_data = la_stream_user_data;
	sink = la_stream_sink;
	sink_active = la_stream_sink_active;
	la_stream_triggered = true;
	if (status < 0) {
		la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
	}
	k_mutex_unlock(&la_mutex);

	terminal_status = status < 0 ? LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN :
		LINKR_DEBUGGER_LA_RING_POLL_OK;
	if (status >= 0 && sample_count > 0U && la_stream_finite_current(generation)) {
		if (sink_active) {
			terminal_status = la_stream_finite_emit_sink(generation, sample_count,
				&config, &sink);
		} else {
			la_stream_finite_emit_callback(generation, sample_count, &config,
				callback, user_data);
		}
	}
	la_stream_finite_emit_terminal(terminal_status, generation, sink_active,
		&sink, callback, user_data);

	k_mutex_lock(&la_mutex, K_FOREVER);
	if (la_stream_finite_backend && generation == la_generation) {
		la_stream_active = false;
		la_stream_ring_active = false;
		la_cleanup_locked();
		la_stream_clear_callback_locked();
	}
	k_mutex_unlock(&la_mutex);
}

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

static void la_stream_ring_request_terminal(enum linkr_debugger_la_ring_poll_result status,
					    uint32_t generation)
{
	k_mutex_lock(&la_mutex, K_FOREVER);
	if (linkr_debugger_logic_analyzer_stream_generation_current(
	    la_stream_ring_active, generation, la_generation) &&
	    !la_stream_ring_terminal_pending && !la_stream_ring_terminal_emitted) {
		la_stream_ring_terminal_status = status;
		la_stream_ring_terminal_pending = true;
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
		la_stream_ring_progress.generation, la_generation, la_dma_channel,
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

static uint32_t la_stream_ring_hw_index(void)
{
	uintptr_t base = (uintptr_t)la_stream_ring_raw;
	uintptr_t write_addr = (uintptr_t)dma_hw->ch[la_dma_channel].write_addr;

	return (uint32_t)(((write_addr - base) & (LINKR_DEBUGGER_LA_RING_BUFFER_BYTES - 1U)) /
		LINKR_DEBUGGER_LA_RAW_SAMPLE_BYTES);
}

static bool la_stream_ring_prepare_emit(uint64_t first_seq, uint32_t count,
						 uint32_t generation,
						 struct linkr_debugger_la_stream_chunk *chunk,
						 uint64_t *emit_start_us)
{
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

	for (uint32_t i = 0U; i < count; i++) {
		uint32_t raw_index = (uint32_t)((first_seq + i) % LINKR_DEBUGGER_LA_RING_SAMPLES);

		la_stream_ring_values[i] = linkr_debugger_logic_analyzer_compress_raw_sample(
			la_stream_ring_raw[raw_index], &la_stream_config);
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
	uint64_t reader_seq, uint32_t emit_count)
{
	struct linkr_debugger_la_stream_sink sink;
	struct linkr_debugger_la_stream_sink_lease lease;
	struct linkr_debugger_la_stream_sink_commit commit;
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
		la_stream_ring_request_terminal(LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN,
			generation);
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_CONTINUE);
	}
	lease_active = true;

	ret = linkr_debugger_logic_analyzer_stream_sink_write_raw_payload(
		&la_stream_config, la_stream_ring_raw, LINKR_DEBUGGER_LA_RING_SAMPLES,
		reader_seq, emit_count, sink.bytes_per_sample, lease.payload,
		lease.capacity, &values_or, &values_and);
	fill_end_us = k_ticks_to_us_floor64(k_uptime_ticks());
	if (ret < 0) {
		la_stream_ring_abort_lease_once(&sink, &lease, &lease_active);
		la_stream_ring_record_consume_elapsed(
			fill_end_us >= consume_start_us ? fill_end_us - consume_start_us : 0U,
			0U, fill_end_us >= consume_start_us ? fill_end_us - consume_start_us : 0U);
		la_stream_ring_request_terminal(LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN,
			generation);
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_CONTINUE);
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
		la_stream_ring_request_terminal(LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN,
			generation);
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_CONTINUE);
	}
	handoff_requested = ret > 0;
	lease_active = false;
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
	if (la_stream_config.post_samples > 0U &&
	    la_stream_ring_emitted_samples >= la_stream_config.post_samples) {
		la_stream_ring_request_terminal(LINKR_DEBUGGER_LA_RING_POLL_OK, generation);
	}
	la_stream_ring_record_sink_handoff(handoff_requested,
		linkr_debugger_logic_analyzer_stream_sink_should_explicit_yield(
			handoff_requested, unread_samples));
	la_stream_ring_record_chunk_complete(commit_end_us);
	return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_CONTINUE);
}

static uint32_t la_stream_ring_poll_once(uint32_t generation)
{
	uint32_t produced = 0U;
	uint32_t hw_index;
	uint64_t now_us;
	uint64_t poll_gap_us = 0U;
	uint64_t unread_samples;
	enum linkr_debugger_la_ring_poll_result result;
	k_spinlock_key_t key;

	if (!la_stream_ring_current(generation) || la_dma_channel < 0) {
		return 0U;
	}

	if (!la_stream_triggered) {
		PIO pio = pio_rpi_pico_get_pio(la_pio_dev);

		if (pio_interrupt_get(pio, 0U)) {
			la_stream_triggered = true;
			pio_interrupt_clear(pio, 0U);
		}
	}

	hw_index = la_stream_ring_hw_index();
	now_us = k_ticks_to_us_floor64(k_uptime_ticks());
	key = k_spin_lock(&la_stream_ring_progress_lock);
	if (la_stream_ring_progress.initialized &&
	    now_us >= la_stream_ring_progress.last_poll_time_us) {
		poll_gap_us = now_us - la_stream_ring_progress.last_poll_time_us;
	}
	result = linkr_debugger_logic_analyzer_ring_observe(&la_stream_ring_progress,
		hw_index, now_us,
		la_capture.actual_sample_rate_hz, 0U, LINKR_DEBUGGER_LA_RING_SAMPLES,
		LINKR_DEBUGGER_LA_RING_SAFETY_SAMPLES, &produced);
	if (result == LINKR_DEBUGGER_LA_RING_POLL_OK && !la_stream_triggered) {
		la_stream_ring_progress.reader_seq = la_stream_ring_progress.writer_seq;
	}
	unread_samples = la_stream_ring_progress.writer_seq - la_stream_ring_progress.reader_seq;
	linkr_debugger_logic_analyzer_ring_metrics_update(&la_stream_ring_metrics,
		poll_gap_us, unread_samples, 0U);
	k_spin_unlock(&la_stream_ring_progress_lock, key);
	if (result != LINKR_DEBUGGER_LA_RING_POLL_OK) {
		LOG_WRN("la ring observe terminal: result=%d gen=%u cur_gen=%u dma=%d "
			"hw=%u produced=%u emitted=%u errors=%u",
			result, generation, la_generation, la_dma_channel, hw_index, produced,
			la_stream_ring_emitted_samples, la_stream_ring_error_count + 1U);
		la_stream_ring_error_count++;
		la_stream_ring_request_terminal(result, generation);
		return 0U;
	}

	if (produced > 0U || la_stream_triggered) {
		k_sem_give(&la_stream_ring_consumer_wake_sem);
	}

	if (la_stream_ring_current(generation)) {
		return linkr_debugger_logic_analyzer_ring_poll_interval_ms(
			la_capture.actual_sample_rate_hz, LINKR_DEBUGGER_LA_RING_SAMPLES,
			LINKR_DEBUGGER_LA_RING_SAFETY_SAMPLES);
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
	enum linkr_debugger_la_ring_poll_result terminal_status;
	k_spinlock_key_t key;
	bool current;
	bool sink_active_for_emit;

	if (la_stream_ring_take_terminal(generation, &terminal_status)) {
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
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_STOP);
	}

	if (!la_stream_triggered || !la_stream_ring_current(generation)) {
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_STOP);
	}

	key = k_spin_lock(&la_stream_ring_progress_lock);
	writer_seq = la_stream_ring_progress.writer_seq;
	reader_seq = la_stream_ring_progress.reader_seq;
	k_spin_unlock(&la_stream_ring_progress_lock, key);
	available = writer_seq - reader_seq;
	if (la_stream_config.post_samples > 0U) {
		remaining = la_stream_ring_emitted_samples >= la_stream_config.post_samples ? 0U :
			la_stream_config.post_samples - la_stream_ring_emitted_samples;
	}
	if (la_stream_config.post_samples > 0U && remaining == 0U) {
		la_stream_ring_request_terminal(LINKR_DEBUGGER_LA_RING_POLL_OK, generation);
		return la_stream_ring_consume_result(LA_STREAM_RING_CONSUME_CONTINUE);
	}
	k_mutex_lock(&la_mutex, K_FOREVER);
	current = linkr_debugger_logic_analyzer_stream_generation_current(
		la_stream_ring_active, generation, la_generation);
	sink_active_for_emit = current && la_stream_sink_active;
	chunk_limit = sink_active_for_emit && la_stream_sink.max_chunk_samples > 0U ?
		la_stream_sink.max_chunk_samples : LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES;
	k_mutex_unlock(&la_mutex);
	emit_count = linkr_debugger_logic_analyzer_ring_next_emit_count(available,
		remaining, chunk_limit);
	if (emit_count == 0U) {
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
	if (la_stream_config.post_samples > 0U &&
	    la_stream_ring_emitted_samples >= la_stream_config.post_samples) {
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
			    !la_stream_ring_backend || la_dma_channel < 0) {
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
			    la_stream_ring_active, generation, la_generation) ||
			    !la_stream_ring_backend) {
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
	dma_channel_config config;
	int ret;

#if !defined(DMA_CH0_CTRL_TRIG_EN_BITS) || \
	!defined(LINKR_DEBUGGER_LA_HAS_RING_DMA_ENDLESS)
	LOG_ERR("la ring setup failed: missing RP2350 DMA ring support ret=%d", -ENOTSUP);
	return -ENOTSUP;
#else
	la_dma_channel = dma_request_channel(la_dma_dev, NULL);
	if (la_dma_channel < 0) {
		LOG_ERR("la ring setup failed: dma_request_channel ret=%d", la_dma_channel);
		return la_dma_channel;
	}

	memset(la_stream_ring_raw, 0, sizeof(la_stream_ring_raw));
	{
		k_spinlock_key_t key = k_spin_lock(&la_stream_ring_progress_lock);

		memset(&la_stream_ring_progress, 0, sizeof(la_stream_ring_progress));
		la_stream_ring_progress.generation = la_generation;
		k_spin_unlock(&la_stream_ring_progress_lock, key);
	}

	config = dma_channel_get_default_config((uint)la_dma_channel);
	channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
	channel_config_set_read_increment(&config, false);
	channel_config_set_write_increment(&config, true);
	channel_config_set_ring(&config, true, LINKR_DEBUGGER_LA_RING_SIZE_BITS);
	channel_config_set_dreq(&config, pio_get_dreq(pio, (uint)la_pio_sm, false));
	dma_channel_set_irq0_enabled((uint)la_dma_channel, false);
	dma_channel_configure((uint)la_dma_channel, &config,
		la_stream_ring_raw, &pio->rxf[la_pio_sm], la_ring_transfer_count(), false);

	if (((uintptr_t)la_stream_ring_raw & (LINKR_DEBUGGER_LA_RING_BUFFER_BYTES - 1U)) != 0U) {
		ret = -EINVAL;
		goto fail;
	}

	la_stream_ring_backend = true;
	la_stream_legacy_backend = false;
	la_stream_ring_active = true;
	dma_channel_start((uint)la_dma_channel);
	{
		k_spinlock_key_t key = k_spin_lock(&la_stream_ring_progress_lock);

		la_stream_ring_progress.initialized = true;
		la_stream_ring_progress.last_hw_index = 0U;
		la_stream_ring_progress.writer_seq = 0U;
		la_stream_ring_progress.reader_seq = 0U;
		la_stream_ring_progress.last_poll_time_us = k_ticks_to_us_floor64(k_uptime_ticks());
		k_spin_unlock(&la_stream_ring_progress_lock, key);
	}
	return 0;

fail:
	LOG_ERR("la ring setup failed: ret=%d", ret);
	dma_release_channel(la_dma_dev, (uint32_t)la_dma_channel);
	la_dma_channel = -1;
	return ret;
#endif
}

static int la_start_stream_common(
	const struct linkr_debugger_la_config *config,
	linkr_debugger_la_stream_callback_t callback,
	void *user_data,
	const struct linkr_debugger_la_stream_sink *sink)
{
	int ret;
	bool finite_eligible;
	bool wait_for_trigger_irq;

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
	while (true) {
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
		if (!linkr_debugger_logic_analyzer_stream_start_must_wait_idle(
		    la_stream_ring_thread_started, la_stream_ring_thread_busy) &&
		    !linkr_debugger_logic_analyzer_stream_start_must_wait_idle(
		    la_stream_ring_consumer_thread_started, la_stream_ring_consumer_thread_busy)) {
			break;
		}
		if (k_current_get() == &la_stream_ring_thread ||
		    k_current_get() == &la_stream_ring_consumer_thread || la_stream_ring_start_waiter) {
			k_mutex_unlock(&la_mutex);
			return -EBUSY;
		}

		la_stream_ring_start_waiter = true;
		k_mutex_unlock(&la_mutex);
		la_stream_ring_wait_idle_if_needed(true);
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
	finite_eligible = linkr_debugger_logic_analyzer_finite_stream_eligible(&la_stream_config);
	wait_for_trigger_irq = finite_eligible &&
		la_stream_config.trigger != LINKR_DEBUGGER_LA_TRIGGER_NONE;

	if (sink != NULL) {
		la_stream_sink = *sink;
		la_stream_sink_active = true;
	} else {
		la_stream_callback = callback;
		la_stream_user_data = user_data;
	}
	la_stream_sequence = 0U;
	la_stream_block_index = 0U;
	la_stream_emit_div = 1U;
	la_stream_triggered = (config->trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE);
	la_stream_use_buf_a = true;
	la_stream_buf_a_ready = false;
	la_stream_buf_b_ready = false;
	la_stream_values_or = 0U;
	la_stream_values_and = 0xffffU;
	la_stream_ring_emitted_samples = 0U;
	la_stream_ring_error_count = 0U;
	memset(&la_stream_ring_metrics, 0, sizeof(la_stream_ring_metrics));
	la_stream_ring_terminal_pending = false;
	la_stream_ring_terminal_emitted = false;
	la_stream_ring_terminal_status = LINKR_DEBUGGER_LA_RING_POLL_OK;
	memset(la_stream_raw_a, 0,
		LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES * sizeof(la_stream_raw_a[0]));
	memset(la_stream_raw_b, 0,
		LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES * sizeof(la_stream_raw_b[0]));

	ret = la_configure_pio_locked(&la_stream_config, wait_for_trigger_irq);
	if (ret < 0) {
		la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
		la_stream_teardown_locked();
		la_cleanup_locked();
		la_stream_clear_callback_locked();
		k_mutex_unlock(&la_mutex);
		return ret;
	}

	la_capture.state = LINKR_DEBUGGER_LA_STATE_STREAMING;
	la_capture.requested_sample_rate_hz = config->sample_rate_hz;
	la_capture.actual_sample_rate_hz = la_actual_rate_from_hw(config->sample_rate_hz);
	la_capture.sample_period_ps = linkr_debugger_logic_analyzer_sample_period_ps(
		la_capture.actual_sample_rate_hz);
	la_capture.backend = linkr_debugger_logic_analyzer_backend();
	la_stream_config.sample_rate_hz = la_capture.actual_sample_rate_hz;

	if (finite_eligible) {
		PIO pio = pio_rpi_pico_get_pio(la_pio_dev);

		memset(la_raw_samples, 0, sizeof(la_raw_samples));
		la_stream_finite_dma_status = 0;
		la_stream_finite_done_generation = la_generation;
		la_stream_finite_backend = true;
		la_stream_ring_backend = false;
		la_stream_legacy_backend = false;
		la_stream_ring_active = false;
		if (wait_for_trigger_irq) {
			pio_interrupt_clear(pio, 0U);
		}
		ret = la_start_stream_finite_dma_locked(la_stream_config.post_samples);
		if (ret < 0) {
			LOG_ERR("la finite gated backend setup failed ret=%d", ret);
			la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
			la_stream_teardown_locked();
			la_cleanup_locked();
			la_stream_clear_callback_locked();
			k_mutex_unlock(&la_mutex);
			return ret;
		}

		la_stream_active = true;
		pio_sm_set_enabled(pio, (uint)la_pio_sm, true);
		if (la_trigger_sm_claimed) {
			pio_sm_set_enabled(pio, (uint)la_trigger_sm, true);
		}
		k_mutex_unlock(&la_mutex);
		return 0;
	}

	ret = la_start_stream_ring_dma_locked();
	if (ret < 0) {
		if (sink != NULL) {
			LOG_ERR("la stream sink backend requires ring dma ret=%d", ret);
			la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
			la_stream_teardown_locked();
			la_cleanup_locked();
			la_stream_clear_callback_locked();
			k_mutex_unlock(&la_mutex);
			return ret;
		}
		la_cleanup_locked();
		ret = la_configure_pio_locked(&la_stream_config, false);
		if (ret < 0) {
			LOG_ERR("la stream backend: legacy pio configure failed ret=%d", ret);
			la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
			la_stream_teardown_locked();
			la_cleanup_locked();
			la_stream_clear_callback_locked();
			k_mutex_unlock(&la_mutex);
			return ret;
		}
		k_work_init(&la_stream_work, la_stream_work_handler);
		ret = la_start_stream_dma_locked();
		if (ret < 0) {
			LOG_ERR("la stream backend: legacy dma setup failed ret=%d", ret);
			la_capture.state = LINKR_DEBUGGER_LA_STATE_ERROR;
			la_stream_teardown_locked();
			la_cleanup_locked();
			la_stream_clear_callback_locked();
			k_mutex_unlock(&la_mutex);
			return ret;
		}
		la_stream_legacy_backend = true;
	} else {
		la_stream_legacy_backend = false;
	}

	la_stream_active = true;

	pio_sm_set_enabled(pio_rpi_pico_get_pio(la_pio_dev), (uint)la_pio_sm, true);
	if (la_trigger_sm_claimed) {
		pio_sm_set_enabled(pio_rpi_pico_get_pio(la_pio_dev), (uint)la_trigger_sm, true);
	}
	if (la_stream_ring_backend) {
		k_sem_give(&la_stream_ring_wake_sem);
	}
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

#if 0 /* Ring buffer mode disabled - DMA hardware ring wrap conflicts with Zephyr DMA driver.
       * See doc/ring-buffer-gap-analysis.md for details.
       * TODO: Re-enable when DMA ring wrap issue is resolved.
       */

static struct linkr_debugger_la_ring_state la_ring;
static volatile bool la_ring_active;
static linkr_debugger_la_stream_callback_t la_ring_cb;
static void *la_ring_user;
static struct linkr_debugger_la_config la_ring_cfg;
static uint32_t la_ring_seq;
static struct k_work la_ring_work;
static struct dma_config la_ring_dma_config;
static struct dma_block_config la_ring_dma_block;
static uint32_t la_ring_raw_a[LINKR_DEBUGGER_LA_RING_HALF_SAMPLES] __aligned(4);
static uint32_t la_ring_raw_b[LINKR_DEBUGGER_LA_RING_HALF_SAMPLES] __aligned(4);
static uint16_t la_ring_buf_a[LINKR_DEBUGGER_LA_RING_HALF_SAMPLES] __aligned(4);
static uint16_t la_ring_buf_b[LINKR_DEBUGGER_LA_RING_HALF_SAMPLES] __aligned(4);
static volatile bool la_ring_use_buf_a;
static volatile bool la_ring_buf_a_ready;
static volatile bool la_ring_buf_b_ready;

static void la_ring_dma_callback(const struct device *dev, void *user_data,
				  uint32_t channel, int status)

static void la_ring_dma_callback(const struct device *dev, void *user_data,
				  uint32_t channel, int status)
{
	uint32_t *next_dest;

	ARG_UNUSED(user_data);

	if (!la_ring_active || status < 0) {
		return;
	}

	if (la_ring_use_buf_a) {
		la_ring_buf_a_ready = true;
	} else {
		la_ring_buf_b_ready = true;
	}
	la_ring_use_buf_a = !la_ring_use_buf_a;

	next_dest = la_ring_use_buf_a ? la_ring_raw_a : la_ring_raw_b;
	la_ring_dma_block.dest_address = (uint32_t)next_dest;
	if (dma_config(dev, channel, &la_ring_dma_config) < 0 ||
	    dma_start(dev, channel) < 0) {
		la_ring_active = false;
		return;
	}

	(void)k_work_submit(&la_ring_work);
}

static void la_ring_process_block(uint32_t *raw_buf, uint16_t *out_buf)
{
	struct linkr_debugger_la_stream_chunk chunk;

	for (uint32_t i = 0U; i < LINKR_DEBUGGER_LA_RING_HALF_SAMPLES; i++) {
		out_buf[i] = linkr_debugger_logic_analyzer_compress_raw_sample(
			raw_buf[i], &la_ring_cfg);
	}

	if (la_ring.pre_trigger_active && !la_ring.triggered) {
		for (uint32_t i = 0U; i < LINKR_DEBUGGER_LA_RING_HALF_SAMPLES; i++) {
			la_ring.pre_trigger_buf[la_ring.pre_trigger_write] = out_buf[i];
			la_ring.pre_trigger_write = (la_ring.pre_trigger_write + 1U) %
				LINKR_DEBUGGER_LA_PRE_TRIGGER_MAX;
			if (la_ring.pre_trigger_count < LINKR_DEBUGGER_LA_PRE_TRIGGER_MAX) {
				la_ring.pre_trigger_count++;
			}
		}
	}

	chunk.sequence = la_ring_seq++;
	chunk.sample_count = LINKR_DEBUGGER_LA_RING_HALF_SAMPLES;
	chunk.timestamp_us = 0U;
	chunk.values = out_buf;

	la_ring_cb(&chunk, la_ring_user);
}

static void la_ring_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!la_ring_active || la_ring_cb == NULL) {
		return;
	}

	if (!la_stream_triggered) {
		PIO pio = pio_rpi_pico_get_pio(la_pio_dev);
		if (pio_interrupt_get(pio, 0U)) {
			la_stream_triggered = true;
			pio_interrupt_clear(pio, 0U);
			la_ring.triggered = true;
			la_ring.trigger_pos = la_ring_seq * LINKR_DEBUGGER_LA_RING_HALF_SAMPLES;
		}
	}

	if (la_ring_buf_a_ready) {
		la_ring_buf_a_ready = false;
		la_ring_process_block(la_ring_raw_a, la_ring_buf_a);
	}

	if (la_ring_buf_b_ready) {
		la_ring_buf_b_ready = false;
		la_ring_process_block(la_ring_raw_b, la_ring_buf_b);
	}
}

int linkr_debugger_logic_analyzer_start_ring(
	const struct linkr_debugger_la_config *config,
	linkr_debugger_la_stream_callback_t callback,
	void *user_data)
{
	if (config == NULL || callback == NULL) {
		return -EINVAL;
	}

	int ret = linkr_debugger_logic_analyzer_validate_config(config,
		LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&la_mutex, K_FOREVER);
	if (la_ring_active) {
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

	la_ring_cfg = *config;
	la_ring_cfg.pin_count = la_active_pin_count(config);
	if (la_ring_cfg.selected_pin_count == 0U) {
		for (uint8_t i = 0U; i < la_ring_cfg.pin_count; i++) {
			la_ring_cfg.selected_pins[i] = (uint8_t)(config->pin_base + i);
		}
		la_ring_cfg.selected_pin_count = la_ring_cfg.pin_count;
	}

	la_ring_cb = callback;
	la_ring_user = user_data;
	la_ring_seq = 0U;
	la_ring.trigger_pos = 0U;
	la_ring.triggered = false;
	la_ring.overrun = false;
	la_stream_triggered = (config->trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE);
	la_ring_use_buf_a = true;
	la_ring_buf_a_ready = false;
	la_ring_buf_b_ready = false;
	la_ring.pre_trigger_write = 0U;
	la_ring.pre_trigger_count = 0U;
	la_ring.pre_trigger_active = (config->pre_samples > 0U);
	memset(la_ring_raw_a, 0, sizeof(la_ring_raw_a));
	memset(la_ring_raw_b, 0, sizeof(la_ring_raw_b));

	k_work_init(&la_ring_work, la_ring_handler);

	ret = la_configure_pio_locked(&la_ring_cfg, false);
	if (ret < 0) {
		k_mutex_unlock(&la_mutex);
		return ret;
	}

	PIO pio = pio_rpi_pico_get_pio(la_pio_dev);
	uint32_t block_size = LINKR_DEBUGGER_LA_RING_HALF_SAMPLES * sizeof(uint32_t);

	la_dma_channel = dma_request_channel(la_dma_dev, NULL);
	if (la_dma_channel < 0) {
		la_cleanup_locked();
		k_mutex_unlock(&la_mutex);
		return -EBUSY;
	}

	memset(&la_ring_dma_config, 0, sizeof(la_ring_dma_config));
	memset(&la_ring_dma_block, 0, sizeof(la_ring_dma_block));

	la_ring_dma_block.source_address = (uint32_t)&pio->rxf[la_pio_sm];
	la_ring_dma_block.dest_address = (uint32_t)la_ring_raw_a;
	la_ring_dma_block.block_size = block_size;
	la_ring_dma_block.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	la_ring_dma_block.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

	la_ring_dma_config.dma_slot = RPI_PICO_DMA_DREQ_TO_SLOT(
		pio_get_dreq(pio, (uint)la_pio_sm, false));
	la_ring_dma_config.channel_direction = PERIPHERAL_TO_MEMORY;
	la_ring_dma_config.source_data_size = LINKR_DEBUGGER_LA_RAW_SAMPLE_BYTES;
	la_ring_dma_config.dest_data_size = LINKR_DEBUGGER_LA_RAW_SAMPLE_BYTES;
	la_ring_dma_config.source_burst_length = 1U;
	la_ring_dma_config.dest_burst_length = 1U;
	la_ring_dma_config.block_count = 1U;
	la_ring_dma_config.head_block = &la_ring_dma_block;
	la_ring_dma_config.user_data = NULL;
	la_ring_dma_config.dma_callback = la_ring_dma_callback;
	la_ring_dma_config.complete_callback_en = 1U;

	ret = dma_config(la_dma_dev, (uint32_t)la_dma_channel, &la_ring_dma_config);
	if (ret < 0) {
		dma_release_channel(la_dma_dev, (uint32_t)la_dma_channel);
		la_dma_channel = -1;
		la_cleanup_locked();
		k_mutex_unlock(&la_mutex);
		return ret;
	}

	la_ring_active = true;
	la_capture.state = LINKR_DEBUGGER_LA_STATE_STREAMING;
	la_capture.requested_sample_rate_hz = config->sample_rate_hz;
	la_capture.actual_sample_rate_hz = la_actual_rate_from_hw(config->sample_rate_hz);
	la_capture.sample_period_ps = linkr_debugger_logic_analyzer_sample_period_ps(
		la_capture.actual_sample_rate_hz);
	la_capture.backend = linkr_debugger_logic_analyzer_backend();

	ret = dma_start(la_dma_dev, (uint32_t)la_dma_channel);
	if (ret < 0) {
		la_ring_active = false;
		dma_release_channel(la_dma_dev, (uint32_t)la_dma_channel);
		la_dma_channel = -1;
		la_cleanup_locked();
		k_mutex_unlock(&la_mutex);
		return ret;
	}

	pio_sm_set_enabled(pio, (uint)la_pio_sm, true);
	if (la_trigger_sm_claimed) {
		pio_sm_set_enabled(pio, (uint)la_trigger_sm, true);
	}
	k_mutex_unlock(&la_mutex);
	return 0;
}

#endif /* Ring buffer mode disabled */

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
