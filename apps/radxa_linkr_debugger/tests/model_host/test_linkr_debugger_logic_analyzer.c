/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_logic_analyzer.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef BIT
#define BIT(n) (1UL << (n))
#endif

static struct linkr_debugger_la_config base_config(void)
{
	struct linkr_debugger_la_config config;

	memset(&config, 0, sizeof(config));
	config.pin_base = 7U;
	config.pin_count = 4U;
	config.sample_rate_hz = 1000000U;
	config.post_samples = 16U;
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;
	return config;
}

static struct linkr_debugger_la_config fast4_config(void)
{
	struct linkr_debugger_la_config config;

	memset(&config, 0, sizeof(config));
	config.pin_base = 10U;
	config.pin_count = 4U;
	config.sample_rate_hz = 1000000U;
	config.post_samples = 16U;
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;
	return config;
}

static void wide11_model_store(uint32_t *sm_a_words, uint32_t *sm_b_words,
	uint32_t sample_index, uint16_t sample)
{
	uint32_t sm_a_word = sample_index /
		LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_SAMPLES_PER_WORD;
	uint32_t sm_b_word = sample_index /
		LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_SAMPLES_PER_WORD;
	uint8_t sm_a_shift = (uint8_t)((sample_index %
		LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_SAMPLES_PER_WORD) * 8U);
	uint8_t sm_b_shift = (uint8_t)(2U + ((sample_index %
		LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_SAMPLES_PER_WORD) * 3U));
	uint32_t sm_a_mask = 0xffU << sm_a_shift;
	uint32_t sm_b_mask = 0x07U << sm_b_shift;

	sm_a_words[sm_a_word] &= ~sm_a_mask;
	sm_a_words[sm_a_word] |= ((uint32_t)sample & 0x00ffU) << sm_a_shift;
	sm_b_words[sm_b_word] &= ~sm_b_mask;
	sm_b_words[sm_b_word] |= (((uint32_t)sample >> 8U) & 0x07U) << sm_b_shift;
}

static uint16_t wide11_target_expected_sample(uint32_t sample_index)
{
	switch (sample_index) {
	case 0U:
		return 0x0000U;
	case 1U:
		return LINKR_DEBUGGER_LA_WIDE11_BURST_SAMPLE_MASK;
	case 2U:
		return 0x0001U;
	case 31U:
		return 0x0401U;
	case 32U:
		return 0x0555U;
	case 33U:
		return 0x02aaU;
	case 99998U:
		return 0x0400U;
	case 99999U:
		return 0x0563U;
	default:
		return 0x0000U;
	}
}

static void assert_u16_span_filled(const uint16_t *values, size_t count, uint16_t value)
{
	for (size_t i = 0U; i < count; i++) {
		assert(values[i] == value);
	}
}

static uint32_t *alloc_lane_words(uint32_t word_count)
{
	uint32_t *words = calloc(word_count, sizeof(*words));

	assert(words != NULL);
	return words;
}

static void fill_single_lane_packed_ring_from_raw_samples(
	const struct linkr_debugger_la_config *config,
	const struct linkr_debugger_la_packed_ring_plan *plan,
	uint32_t *lane_words,
	const uint32_t *raw_ring,
	uint32_t ring_samples);

static struct linkr_debugger_la_config wide11_exact_config(void);
static struct linkr_debugger_la_config single_plan_config(uint32_t post_samples);
static struct linkr_debugger_la_config fast8_plan_config(uint32_t post_samples);
static void packed_ring_lane_store(const struct linkr_debugger_la_packed_ring_lane *lane,
	uint32_t *words, uint32_t sample_index, uint32_t lane_value);

static uint32_t ring_test_start_sample(const struct linkr_debugger_la_packed_ring_plan *plan,
	uint32_t ring_samples, uint32_t first_index)
{
	return (plan->sample_capacity - ring_samples + first_index) % plan->sample_capacity;
}

struct sink_test_context {
	uint8_t storage[32];
	size_t capacity;
	int lease_ret;
	int commit_ret;
	uint32_t lease_calls;
	uint32_t commit_calls;
	uint32_t abort_calls;
	uint32_t terminal_calls;
	uint32_t last_sample_count;
	uint8_t last_bytes_per_sample;
	enum linkr_debugger_la_ring_poll_result terminal_status;
	uint32_t terminal_sequence;
};

static int sink_test_lease(uint32_t sample_count, uint8_t bytes_per_sample,
	void *user_data, struct linkr_debugger_la_stream_sink_lease *lease)
{
	struct sink_test_context *ctx = user_data;

	ctx->lease_calls++;
	ctx->last_sample_count = sample_count;
	ctx->last_bytes_per_sample = bytes_per_sample;
	if (ctx->lease_ret < 0) {
		return ctx->lease_ret;
	}
	lease->payload = ctx->storage;
	lease->capacity = ctx->capacity;
	lease->token = ctx;
	return 0;
}

static int sink_test_commit(const struct linkr_debugger_la_stream_sink_commit *commit,
	void *user_data)
{
	struct sink_test_context *ctx = user_data;

	ctx->commit_calls++;
	ctx->last_sample_count = commit->sample_count;
	ctx->last_bytes_per_sample = commit->bytes_per_sample;
	return ctx->commit_ret;
}

static void sink_test_abort(void *token, void *user_data)
{
	struct sink_test_context *ctx = user_data;

	assert(token == ctx);
	ctx->abort_calls++;
}

static void sink_test_terminal(enum linkr_debugger_la_ring_poll_result status,
	uint32_t sequence, void *user_data)
{
	struct sink_test_context *ctx = user_data;

	ctx->terminal_calls++;
	ctx->terminal_status = status;
	ctx->terminal_sequence = sequence;
}

static struct linkr_debugger_la_stream_sink sink_for_context(struct sink_test_context *ctx)
{
	struct linkr_debugger_la_stream_sink sink;

	memset(&sink, 0, sizeof(sink));
	sink.format = LINKR_DEBUGGER_LA_STREAM_PAYLOAD_PACKED_LE_BYTES;
	sink.bytes_per_sample = 1U;
	sink.lease = sink_test_lease;
	sink.commit = sink_test_commit;
	sink.abort = sink_test_abort;
	sink.terminal = sink_test_terminal;
	sink.user_data = ctx;
	return sink;
}

static void test_rate_quantization(void)
{
	assert(linkr_debugger_logic_analyzer_actual_rate(1000000U) == 1000000U);
	assert(linkr_debugger_logic_analyzer_actual_rate(125000000U) == 125000000U);
	assert(linkr_debugger_logic_analyzer_actual_rate(100000000U) == 100000000U);
	assert(linkr_debugger_logic_analyzer_actual_rate(100000U) == 100000U);
	assert(linkr_debugger_logic_analyzer_actual_rate(99999U) == 0U);
	assert(linkr_debugger_logic_analyzer_actual_rate(125000001U) == 0U);
	assert(linkr_debugger_logic_analyzer_sample_period_ps(1000000U) == 1000000ULL);
	assert(linkr_debugger_logic_analyzer_sample_period_ps(125000000U) == 8000ULL);
}

static void test_sample_capacity_and_ring_constants(void)
{
	assert(linkr_debugger_logic_analyzer_max_samples(4U, 2048U) == 512U);
	assert(linkr_debugger_logic_analyzer_max_samples(0U, 2048U) == 0U);
	assert(LINKR_DEBUGGER_LA_RING_BUFFER_BYTES == 32768U);
	assert(LINKR_DEBUGGER_LA_RING_SIZE_BITS == 15U);
	assert(LINKR_DEBUGGER_LA_RING_SAMPLES == 8192U);
	assert(LINKR_DEBUGGER_LA_RING_SAFETY_SAMPLES == 2048U);
	assert(LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES == 1024U);
	assert(LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES == 2048U);
	assert(LINKR_DEBUGGER_LA_STREAM_MAX_PACKED_CHUNK_SAMPLES == 4096U);
}

static void test_wide11_burst_plan_counts_and_overflow(void)
{
	struct linkr_debugger_la_wide11_burst_plan plan;

	assert(LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES == 100000U);
	assert(LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_LANE_A_WORDS == 25000U);
	assert(LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_LANE_B_WORDS == 10000U);
	assert(LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_LANE_A_BYTES == 100000U);
	assert(LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_LANE_B_BYTES == 40000U);
	assert(LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_TOTAL_BYTES == 140000U);

	memset(&plan, 0, sizeof(plan));
	assert(linkr_debugger_logic_analyzer_wide11_burst_plan(20U, &plan) == 0);
	assert(plan.sample_count == 20U);
	assert(plan.lane_a_word_count == 5U);
	assert(plan.lane_b_word_count == 2U);
	assert(plan.lane_a_byte_count == 20U);
	assert(plan.lane_b_byte_count == 8U);
	assert(plan.total_byte_count == 28U);

	memset(&plan, 0, sizeof(plan));
	assert(linkr_debugger_logic_analyzer_wide11_burst_plan(40U, &plan) == 0);
	assert(plan.lane_a_word_count == 10U);
	assert(plan.lane_b_word_count == 4U);
	assert(plan.total_byte_count == 56U);

	memset(&plan, 0, sizeof(plan));
	assert(linkr_debugger_logic_analyzer_wide11_burst_plan(
		LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES, &plan) == 0);
	assert(plan.lane_a_word_count == LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_LANE_A_WORDS);
	assert(plan.lane_b_word_count == LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_LANE_B_WORDS);
	assert(plan.lane_a_byte_count == LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_LANE_A_BYTES);
	assert(plan.lane_b_byte_count == LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_LANE_B_BYTES);
	assert(plan.total_byte_count == LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_TOTAL_BYTES);

	assert(linkr_debugger_logic_analyzer_wide11_burst_plan(0U, &plan) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_wide11_burst_plan(1U, &plan) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_wide11_burst_plan(32U, &plan) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_wide11_burst_plan(100001U, &plan) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_wide11_burst_plan(0x80000000U,
		&plan) < 0);
	assert(linkr_debugger_logic_analyzer_wide11_burst_plan(0x79000000U,
		&plan) < 0);
	assert(linkr_debugger_logic_analyzer_wide11_burst_plan(32U, NULL) == -EINVAL);
}

static void test_ring_delta_wraps_without_equal_addr_assumption(void)
{
	assert(linkr_debugger_logic_analyzer_ring_delta_samples(10U, 42U, 8192U) == 32U);
	assert(linkr_debugger_logic_analyzer_ring_delta_samples(8190U, 3U, 8192U) == 5U);
	assert(linkr_debugger_logic_analyzer_ring_delta_samples(1234U, 1234U, 8192U) == 0U);
	assert(linkr_debugger_logic_analyzer_ring_delta_samples(8192U, 0U, 8192U) == 0U);
}

static void test_ring_elapsed_window_possible_overrun(void)
{
	assert(!linkr_debugger_logic_analyzer_ring_window_may_overrun(
		7000U, 1000000U, 8192U, 1024U));
	assert(linkr_debugger_logic_analyzer_ring_window_may_overrun(
		7168U, 1000000U, 8192U, 1024U));
	assert(linkr_debugger_logic_analyzer_ring_window_may_overrun(
		1U, 0U, 8192U, 1024U));
	assert(linkr_debugger_logic_analyzer_ring_window_may_overrun(
		1U, 1000000U, 8192U, 8192U));
}

static void test_ring_sequence_definite_overrun(void)
{
	assert(!linkr_debugger_logic_analyzer_ring_seq_overran(7168U, 0U, 8192U, 1024U));
	assert(linkr_debugger_logic_analyzer_ring_seq_overran(7169U, 0U, 8192U, 1024U));
	assert(linkr_debugger_logic_analyzer_ring_seq_overran(10U, 11U, 8192U, 1024U));
}

static void test_ring_poll_interval_bounds(void)
{
	assert(linkr_debugger_logic_analyzer_ring_poll_interval_ms(
		0U, 8192U, 1024U) == 1U);
	assert(linkr_debugger_logic_analyzer_ring_poll_interval_ms(
		1000000U, 0U, 1024U) == 1U);
	assert(linkr_debugger_logic_analyzer_ring_poll_interval_ms(
		1000000U, 8192U, 8192U) == 1U);
	assert(linkr_debugger_logic_analyzer_ring_poll_interval_ms(
		100000U, 8192U, 1024U) == 4U);
	assert(linkr_debugger_logic_analyzer_ring_poll_interval_ms(
		500000U, 8192U, 1024U) == 3U);
	assert(linkr_debugger_logic_analyzer_ring_poll_interval_ms(
		1000000U, 8192U, 1024U) == 1U);
	assert(linkr_debugger_logic_analyzer_ring_poll_interval_ms(
		2000000U, 8192U, 1024U) == 1U);
}

static void test_ring_observe_progress_and_restart_reset(void)
{
	struct linkr_debugger_la_ring_progress progress;
	uint32_t produced = 0U;

	memset(&progress, 0, sizeof(progress));
	progress.generation = 7U;
	assert(linkr_debugger_logic_analyzer_ring_observe(&progress, 100U, 1000U,
		1000000U, 0U, 8192U, 1024U, &produced) == LINKR_DEBUGGER_LA_RING_POLL_OK);
	assert(produced == 0U);
	assert(progress.initialized);
	assert(progress.last_hw_index == 100U);

	assert(linkr_debugger_logic_analyzer_ring_observe(&progress, 1100U, 1500U,
		1000000U, 500U, 8192U, 1024U, &produced) == LINKR_DEBUGGER_LA_RING_POLL_OK);
	assert(produced == 1000U);
	assert(progress.writer_seq == 1000U);
	assert(progress.reader_seq == 500U);

	assert(linkr_debugger_logic_analyzer_ring_observe(&progress, 1100U, 9000U,
		1000000U, 0U, 8192U, 1024U, &produced) ==
		LINKR_DEBUGGER_LA_RING_POLL_POSSIBLE_OVERRUN);

	memset(&progress, 0, sizeof(progress));
	progress.generation = 8U;
	assert(linkr_debugger_logic_analyzer_ring_observe(&progress, 12U, 10U,
		1000000U, 0U, 8192U, 1024U, &produced) == LINKR_DEBUGGER_LA_RING_POLL_OK);
	assert(progress.writer_seq == 0U);
	assert(progress.reader_seq == 0U);
	assert(progress.last_hw_index == 12U);
}

static void test_ring_observe_first_poll_counts_from_start_index_zero(void)
{
	struct linkr_debugger_la_ring_progress progress;
	uint32_t produced = 0U;

	memset(&progress, 0, sizeof(progress));
	progress.generation = 9U;
	progress.initialized = true;
	progress.last_hw_index = 0U;
	progress.writer_seq = 0U;
	progress.reader_seq = 0U;
	progress.last_poll_time_us = 0U;

	assert(linkr_debugger_logic_analyzer_ring_observe(&progress, 100U, 100U,
		1000000U, 0U, 8192U, 1024U, &produced) == LINKR_DEBUGGER_LA_RING_POLL_OK);
	assert(produced == 100U);
	assert(progress.writer_seq == 100U);
	assert(progress.last_hw_index == 100U);
}

static void test_ring_observe_definite_overrun(void)
{
	struct linkr_debugger_la_ring_progress progress;
	uint32_t produced = 0U;

	memset(&progress, 0, sizeof(progress));
	progress.initialized = true;
	progress.last_hw_index = 0U;
	progress.last_poll_time_us = 0U;
	progress.writer_seq = 7000U;
	progress.reader_seq = 0U;
	assert(linkr_debugger_logic_analyzer_ring_observe(&progress, 200U, 100U,
		1000000U, 0U, 8192U, 1024U, &produced) ==
		LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN);
	assert(produced == 200U);
}

static void test_ring_next_emit_count_gates_partial_tails(void)
{
	assert(linkr_debugger_logic_analyzer_ring_next_emit_count(400U, 0U, 1024U) == 0U);
	assert(linkr_debugger_logic_analyzer_ring_next_emit_count(1200U, 0U, 1024U) == 1024U);
	assert(linkr_debugger_logic_analyzer_ring_next_emit_count(2047U, 0U, 2048U) == 0U);
	assert(linkr_debugger_logic_analyzer_ring_next_emit_count(2048U, 0U, 2048U) == 2048U);
	assert(linkr_debugger_logic_analyzer_ring_next_emit_count(4095U, 0U, 2048U) == 2048U);
	assert(linkr_debugger_logic_analyzer_ring_next_emit_count(499U, 500U, 1024U) == 0U);
	assert(linkr_debugger_logic_analyzer_ring_next_emit_count(500U, 500U, 1024U) == 500U);
	assert(linkr_debugger_logic_analyzer_ring_next_emit_count(1023U, 1023U, 2048U) == 1023U);
	assert(linkr_debugger_logic_analyzer_ring_next_emit_count(1022U, 1023U, 2048U) == 0U);
	assert(linkr_debugger_logic_analyzer_ring_next_emit_count(1200U, 1500U, 1024U) == 1024U);
	assert(linkr_debugger_logic_analyzer_ring_next_emit_count(0U, 1500U, 1024U) == 0U);
	assert(linkr_debugger_logic_analyzer_ring_next_emit_count(1200U, 1500U, 0U) == 0U);
}

static void test_ring_drainable_samples_batches_complete_chunks(void)
{
	assert(linkr_debugger_logic_analyzer_ring_drainable_samples(400U, 0U, 1024U) == 0U);
	assert(linkr_debugger_logic_analyzer_ring_drainable_samples(1200U, 0U, 1024U) == 1024U);
	assert(linkr_debugger_logic_analyzer_ring_drainable_samples(2500U, 0U, 1024U) == 2048U);
	assert(linkr_debugger_logic_analyzer_ring_drainable_samples(499U, 500U, 1024U) == 0U);
	assert(linkr_debugger_logic_analyzer_ring_drainable_samples(500U, 500U, 1024U) == 500U);
	assert(linkr_debugger_logic_analyzer_ring_drainable_samples(1200U, 1500U, 1024U) == 1024U);
	assert(linkr_debugger_logic_analyzer_ring_drainable_samples(1500U, 1500U, 1024U) == 1500U);
	assert(linkr_debugger_logic_analyzer_ring_drainable_samples(3000U, 2500U, 1024U) == 2500U);
	assert(linkr_debugger_logic_analyzer_ring_drainable_samples(3000U, 0U, 0U) == 0U);
}

static void test_ring_freeze_before_overwrite_retains_tail_window(void)
{
	uint32_t retained = 0U;

	assert(!linkr_debugger_logic_analyzer_ring_should_freeze_before_overwrite(
		1000U, 0U, 6000U, 8192U, 1024U, &retained));
	assert(retained == 7000U);
	assert(!linkr_debugger_logic_analyzer_ring_should_freeze_before_overwrite(
		0U, 500U, 1000U, 8192U, 1024U, &retained));
	assert(retained == 500U);
	assert(linkr_debugger_logic_analyzer_ring_should_freeze_before_overwrite(
		1000U, 0U, 6169U, 8192U, 1024U, &retained));
	assert(retained == 7169U);
	assert(linkr_debugger_logic_analyzer_ring_should_freeze_before_overwrite(
		9000U, 0U, 100U, 8192U, 1024U, &retained));
	assert(retained == 8192U);
	assert(linkr_debugger_logic_analyzer_ring_should_freeze_before_overwrite(
		100U, 200U, 1U, 8192U, 1024U, &retained));
	assert(retained == 0U);
}

static void test_ring_terminal_emit_count_drains_partial_tail_only_when_terminal(void)
{
	assert(linkr_debugger_logic_analyzer_ring_terminal_emit_count(
		400U, 0U, 1024U, false) == 0U);
	assert(linkr_debugger_logic_analyzer_ring_terminal_emit_count(
		400U, 0U, 1024U, true) == 400U);
	assert(linkr_debugger_logic_analyzer_ring_terminal_emit_count(
		2048U, 0U, 1024U, true) == 1024U);
	assert(linkr_debugger_logic_analyzer_ring_terminal_emit_count(
		1500U, 500U, 1024U, true) == 500U);
	assert(linkr_debugger_logic_analyzer_ring_terminal_emit_count(
		1500U, 0U, 0U, true) == 0U);
}

static void test_ring_freeze_policy_stops_required_stream_hardware(void)
{
	struct linkr_debugger_la_ring_freeze_policy single =
		linkr_debugger_logic_analyzer_ring_freeze_policy(1U);
	struct linkr_debugger_la_ring_freeze_policy dual =
		linkr_debugger_logic_analyzer_ring_freeze_policy(2U);

	assert(single.stop_sampler_sm_a);
	assert(!single.stop_sampler_sm_b);
	assert(single.stop_trigger_sm);
	assert(single.abort_dma_a);
	assert(!single.abort_dma_b);

	assert(dual.stop_sampler_sm_a);
	assert(dual.stop_sampler_sm_b);
	assert(dual.stop_trigger_sm);
	assert(dual.abort_dma_a);
	assert(dual.abort_dma_b);
}

static void test_ring_metrics_track_time_maxima_without_resetting_lower_values(void)
{
	struct linkr_debugger_la_ring_metrics metrics;

	memset(&metrics, 0, sizeof(metrics));
	linkr_debugger_logic_analyzer_ring_metrics_update(&metrics, 100U, 200U, 3000U);
	assert(metrics.max_poll_gap_us == 100U);
	assert(metrics.max_unread_samples == 200U);
	assert(metrics.max_emit_us == 3000U);

	linkr_debugger_logic_analyzer_ring_metrics_update(&metrics, 99U, 199U, 2999U);
	assert(metrics.max_poll_gap_us == 100U);
	assert(metrics.max_unread_samples == 200U);
	assert(metrics.max_emit_us == 3000U);

	linkr_debugger_logic_analyzer_ring_metrics_update(&metrics, 101U, 7168U, 3001U);
	assert(metrics.max_poll_gap_us == 101U);
	assert(metrics.max_unread_samples == 7168U);
	assert(metrics.max_emit_us == 3001U);
	linkr_debugger_logic_analyzer_ring_metrics_update(NULL, 999U, 999U, 999U);
}

static void test_ring_consume_metrics_track_max_totals_and_count(void)
{
	struct linkr_debugger_la_ring_metrics metrics;

	memset(&metrics, 0, sizeof(metrics));
	linkr_debugger_logic_analyzer_ring_metrics_update_consume(&metrics, 10U, 20U, 35U);
	assert(metrics.max_compact_us == 10U);
	assert(metrics.total_compact_us == 10U);
	assert(metrics.max_callback_us == 20U);
	assert(metrics.total_callback_us == 20U);
	assert(metrics.max_emit_us == 35U);
	assert(metrics.total_consume_us == 35U);
	assert(metrics.consume_chunk_count == 1U);

	linkr_debugger_logic_analyzer_ring_metrics_update_consume(&metrics, 8U, 25U, 34U);
	assert(metrics.max_compact_us == 10U);
	assert(metrics.total_compact_us == 18U);
	assert(metrics.max_callback_us == 25U);
	assert(metrics.total_callback_us == 45U);
	assert(metrics.max_emit_us == 35U);
	assert(metrics.total_consume_us == 69U);
	assert(metrics.consume_chunk_count == 2U);
	linkr_debugger_logic_analyzer_ring_metrics_update_consume(NULL, 1U, 1U, 1U);
}

static void test_ring_consumer_latency_and_handoff_metrics(void)
{
	struct linkr_debugger_la_ring_metrics metrics;

	memset(&metrics, 0, sizeof(metrics));
	linkr_debugger_logic_analyzer_ring_metrics_update_inter_chunk_gap(&metrics, 100U);
	assert(metrics.max_consumer_inter_chunk_gap_us == 0U);
	linkr_debugger_logic_analyzer_ring_metrics_mark_chunk_complete(&metrics, 100U);
	linkr_debugger_logic_analyzer_ring_metrics_update_inter_chunk_gap(&metrics, 125U);
	assert(metrics.max_consumer_inter_chunk_gap_us == 25U);
	linkr_debugger_logic_analyzer_ring_metrics_update_inter_chunk_gap(&metrics, 123U);
	assert(metrics.max_consumer_inter_chunk_gap_us == 25U);
	linkr_debugger_logic_analyzer_ring_metrics_update_inter_chunk_gap(&metrics, 140U);
	assert(metrics.max_consumer_inter_chunk_gap_us == 40U);
	linkr_debugger_logic_analyzer_ring_metrics_update_inter_chunk_gap(&metrics, 99U);
	assert(metrics.max_consumer_inter_chunk_gap_us == 40U);
	linkr_debugger_logic_analyzer_ring_metrics_clear_consumer_gap(&metrics);
	linkr_debugger_logic_analyzer_ring_metrics_update_inter_chunk_gap(&metrics, 200U);
	assert(metrics.max_consumer_inter_chunk_gap_us == 40U);

	linkr_debugger_logic_analyzer_ring_metrics_update_yield_resume(&metrics, 10U, false);
	linkr_debugger_logic_analyzer_ring_metrics_update_yield_resume(&metrics, 8U, true);
	linkr_debugger_logic_analyzer_ring_metrics_update_yield_resume(&metrics, 12U, true);
	assert(metrics.max_consumer_yield_resume_us == 12U);
	assert(metrics.legacy_yield_count == 1U);

	linkr_debugger_logic_analyzer_ring_metrics_update_sink_handoff(&metrics, false, false);
	linkr_debugger_logic_analyzer_ring_metrics_update_sink_handoff(&metrics, true, true);
	linkr_debugger_logic_analyzer_ring_metrics_update_sink_handoff(&metrics, true, false);
	assert(metrics.sink_handoff_requested_count == 2U);
	assert(metrics.sink_handoff_executed_count == 1U);
	assert(metrics.sink_handoff_skipped_backlog_count == 0U);
}

static void test_copy_complete_reader_advance_is_independent_of_callback_duration(void)
{
	struct linkr_debugger_la_ring_progress progress;
	struct linkr_debugger_la_ring_metrics metrics;
	uint32_t emitted_samples = 0U;
	bool callback_finished = false;

	memset(&progress, 0, sizeof(progress));
	memset(&metrics, 0, sizeof(metrics));
	progress.generation = 12U;
	progress.writer_seq = 3072U;
	progress.reader_seq = 1024U;
	metrics.max_unread_samples = 2048U;

	assert(linkr_debugger_logic_analyzer_stream_copy_complete_advance_reader(
		&progress, &metrics, true, 12U, 1024U, 1024U));
	assert(progress.reader_seq == 2048U);
	assert(metrics.max_unread_samples == 2048U);
	assert(!callback_finished);
	assert(emitted_samples == 0U);

	callback_finished = true;
	if (callback_finished && linkr_debugger_logic_analyzer_stream_callback_allows_protocol_update(
	    true, 1024U)) {
		emitted_samples += 1024U;
	}
	assert(emitted_samples == 1024U);
}

static void test_stale_generation_cannot_advance_reader_or_update_protocol_state(void)
{
	struct linkr_debugger_la_ring_progress progress;
	struct linkr_debugger_la_ring_metrics metrics;
	uint32_t emitted_samples = 0U;

	memset(&progress, 0, sizeof(progress));
	memset(&metrics, 0, sizeof(metrics));
	progress.generation = 21U;
	progress.writer_seq = 2048U;
	progress.reader_seq = 0U;

	assert(!linkr_debugger_logic_analyzer_stream_copy_complete_advance_reader(
		&progress, &metrics, false, 21U, 0U, 1024U));
	assert(progress.reader_seq == 0U);
	assert(!linkr_debugger_logic_analyzer_stream_copy_complete_advance_reader(
		&progress, &metrics, true, 20U, 0U, 1024U));
	assert(progress.reader_seq == 0U);

	progress.reader_seq = 512U;
	assert(!linkr_debugger_logic_analyzer_stream_copy_complete_advance_reader(
		&progress, &metrics, true, 21U, 0U, 1024U));
	assert(progress.reader_seq == 512U);

	if (linkr_debugger_logic_analyzer_stream_callback_allows_protocol_update(false, 1024U)) {
		emitted_samples += 1024U;
	}
	assert(emitted_samples == 0U);
	assert(!linkr_debugger_logic_analyzer_stream_callback_allows_protocol_update(true, 0U));
}

static void test_stream_generation_lifecycle_gate(void)
{
	assert(linkr_debugger_logic_analyzer_stream_generation_current(true, 4U, 4U));
	assert(!linkr_debugger_logic_analyzer_stream_generation_current(false, 4U, 4U));
	assert(!linkr_debugger_logic_analyzer_stream_generation_current(true, 4U, 5U));
	assert(!linkr_debugger_logic_analyzer_stream_generation_current(false, 4U, 5U));
}

static void test_stream_start_idle_wait_gate(void)
{
	assert(!linkr_debugger_logic_analyzer_stream_start_must_wait_idle(false, false));
	assert(!linkr_debugger_logic_analyzer_stream_start_must_wait_idle(true, false));
	assert(!linkr_debugger_logic_analyzer_stream_start_must_wait_idle(false, true));
	assert(linkr_debugger_logic_analyzer_stream_start_must_wait_idle(true, true));
	assert(linkr_debugger_logic_analyzer_stream_idle_wait_mask(
		false, false, false, false) == 0U);
	assert(linkr_debugger_logic_analyzer_stream_idle_wait_mask(
		true, false, true, false) == 0U);
	assert(linkr_debugger_logic_analyzer_stream_idle_wait_mask(
		true, true, true, false) == 0x01U);
	assert(linkr_debugger_logic_analyzer_stream_idle_wait_mask(
		true, false, true, true) == 0x02U);
	assert(linkr_debugger_logic_analyzer_stream_idle_wait_mask(
		true, true, true, true) == 0x03U);
	assert(linkr_debugger_logic_analyzer_stream_idle_wait_mask(
		false, true, true, true) == 0x02U);
}

static void test_stream_sink_validate_rejects_unsupported_shapes(void)
{
	struct sink_test_context ctx;
	struct linkr_debugger_la_stream_sink sink;
	struct linkr_debugger_la_config config = base_config();

	memset(&ctx, 0, sizeof(ctx));
	sink = sink_for_context(&ctx);
	assert(linkr_debugger_logic_analyzer_stream_sink_validate(&config, &sink) == 0);
	sink.max_chunk_samples = LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES;
	assert(linkr_debugger_logic_analyzer_stream_sink_validate(&config, &sink) == 0);
	sink.max_chunk_samples = LINKR_DEBUGGER_LA_STREAM_MAX_PACKED_CHUNK_SAMPLES;
	assert(linkr_debugger_logic_analyzer_stream_sink_validate(&config, &sink) == 0);
	sink.max_chunk_samples = LINKR_DEBUGGER_LA_STREAM_MAX_PACKED_CHUNK_SAMPLES + 1U;
	assert(linkr_debugger_logic_analyzer_stream_sink_validate(&config, &sink) == -EINVAL);

	sink = sink_for_context(&ctx);
	sink.bytes_per_sample = 2U;
	assert(linkr_debugger_logic_analyzer_stream_sink_validate(&config, &sink) == -EINVAL);
	sink = sink_for_context(&ctx);
	sink.format = 0;
	assert(linkr_debugger_logic_analyzer_stream_sink_validate(&config, &sink) == -ENOTSUP);
	sink = sink_for_context(&ctx);
	sink.commit = NULL;
	assert(linkr_debugger_logic_analyzer_stream_sink_validate(&config, &sink) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_stream_sink_validate(&config, NULL) == -EINVAL);
}

static void test_stream_sink_success_lifecycle_advances_after_payload_complete(void)
{
	struct sink_test_context ctx;
	struct linkr_debugger_la_stream_sink sink;
	struct linkr_debugger_la_stream_sink_lease lease;
	struct linkr_debugger_la_stream_sink_commit commit;
	struct linkr_debugger_la_ring_progress progress;
	struct linkr_debugger_la_ring_metrics metrics;
	struct linkr_debugger_la_config config = fast4_config();
	struct linkr_debugger_la_packed_ring_plan plan;
	uint32_t raw_ring[] = { 0U, (uint32_t)BIT(0), (uint32_t)BIT(1),
		(uint32_t)(BIT(0) | BIT(1)) };
	uint32_t *lane_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint16_t values_or;
	uint16_t values_and;

	memset(&ctx, 0, sizeof(ctx));
	memset(&progress, 0, sizeof(progress));
	memset(&metrics, 0, sizeof(metrics));
	ctx.capacity = sizeof(ctx.storage);
	sink = sink_for_context(&ctx);
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	lane_words = alloc_lane_words(plan.lanes[0].word_count);
	fill_single_lane_packed_ring_from_raw_samples(&config, &plan, lane_words, raw_ring, 4U);
	lane_ptrs[0] = lane_words;
	lane_counts[0] = plan.lanes[0].word_count;
	progress.generation = 3U;
	progress.writer_seq = 4U;
	progress.reader_seq = 0U;

	assert(linkr_debugger_logic_analyzer_stream_sink_lease_payload(&sink, 4U,
		&lease) == 0);
	assert(ctx.lease_calls == 1U);
	assert(lease.payload == ctx.storage);
	assert(lease.token == &ctx);
	assert(linkr_debugger_logic_analyzer_stream_sink_write_packed_payload(&plan,
		lane_ptrs, lane_counts, ring_test_start_sample(&plan, 4U, 0U),
		4U, 1U, lease.payload, lease.capacity,
		&values_or, &values_and) == 0);
	assert(progress.reader_seq == 0U);
	assert(memcmp(ctx.storage, (uint8_t[]){0x00U, 0x01U, 0x02U, 0x03U}, 4U) == 0);
	assert(values_or == 0x0003U);
	assert(values_and == 0x0000U);
	assert(linkr_debugger_logic_analyzer_stream_copy_complete_advance_reader(
		&progress, &metrics, true, 3U, 0U, 4U));
	assert(progress.reader_seq == 4U);

	memset(&commit, 0, sizeof(commit));
	commit.token = lease.token;
	commit.sample_count = 4U;
	commit.bytes_per_sample = 1U;
	commit.payload_len = 4U;
	assert(linkr_debugger_logic_analyzer_stream_sink_commit_payload(&sink, &commit) == 0);
	assert(ctx.commit_calls == 1U);
	assert(ctx.abort_calls == 0U);
	assert(linkr_debugger_logic_analyzer_stream_sink_allows_protocol_update(true, 4U));
	linkr_debugger_logic_analyzer_stream_sink_notify_terminal(&sink,
		LINKR_DEBUGGER_LA_RING_POLL_OK, 10U);
	assert(ctx.terminal_calls == 1U);
	assert(ctx.terminal_status == LINKR_DEBUGGER_LA_RING_POLL_OK);
	assert(ctx.terminal_sequence == 10U);
	free(lane_words);
}

static void test_stream_sink_capacity_failure_aborts_exactly_once(void)
{
	struct sink_test_context ctx;
	struct linkr_debugger_la_stream_sink sink;
	struct linkr_debugger_la_stream_sink_lease lease;
	struct linkr_debugger_la_config config = fast4_config();
	struct linkr_debugger_la_packed_ring_plan plan;
	uint32_t raw_ring[] = { 0U, 1U, 2U, 3U };
	uint32_t *lane_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};

	memset(&ctx, 0, sizeof(ctx));
	ctx.capacity = 2U;
	sink = sink_for_context(&ctx);
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	lane_words = alloc_lane_words(plan.lanes[0].word_count);
	fill_single_lane_packed_ring_from_raw_samples(&config, &plan, lane_words, raw_ring, 4U);
	lane_ptrs[0] = lane_words;
	lane_counts[0] = plan.lanes[0].word_count;
	assert(linkr_debugger_logic_analyzer_stream_sink_lease_payload(&sink, 4U,
		&lease) == 0);
	assert(linkr_debugger_logic_analyzer_stream_sink_write_packed_payload(&plan,
		lane_ptrs, lane_counts, ring_test_start_sample(&plan, 4U, 0U),
		4U, 1U, lease.payload, lease.capacity,
		NULL, NULL) == -ENOSPC);
	linkr_debugger_logic_analyzer_stream_sink_abort_payload(&sink, &lease);
	linkr_debugger_logic_analyzer_stream_sink_abort_payload(&sink, &lease);
	assert(ctx.abort_calls == 1U);
	free(lane_words);
}

static void test_stream_sink_stale_generation_aborts_without_reader_advance(void)
{
	struct sink_test_context ctx;
	struct linkr_debugger_la_stream_sink sink;
	struct linkr_debugger_la_stream_sink_lease lease;
	struct linkr_debugger_la_ring_progress progress;
	struct linkr_debugger_la_ring_metrics metrics;
	struct linkr_debugger_la_config config = fast4_config();
	struct linkr_debugger_la_packed_ring_plan plan;
	uint32_t raw_ring[] = { 0U, 1U, 2U, 3U };
	uint32_t *lane_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};

	memset(&ctx, 0, sizeof(ctx));
	memset(&progress, 0, sizeof(progress));
	memset(&metrics, 0, sizeof(metrics));
	ctx.capacity = sizeof(ctx.storage);
	sink = sink_for_context(&ctx);
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	lane_words = alloc_lane_words(plan.lanes[0].word_count);
	fill_single_lane_packed_ring_from_raw_samples(&config, &plan, lane_words, raw_ring, 4U);
	lane_ptrs[0] = lane_words;
	lane_counts[0] = plan.lanes[0].word_count;
	progress.generation = 5U;
	progress.writer_seq = 4U;
	progress.reader_seq = 0U;
	assert(linkr_debugger_logic_analyzer_stream_sink_lease_payload(&sink, 4U,
		&lease) == 0);
	assert(linkr_debugger_logic_analyzer_stream_sink_write_packed_payload(&plan,
		lane_ptrs, lane_counts, ring_test_start_sample(&plan, 4U, 0U),
		4U, 1U, lease.payload, lease.capacity,
		NULL, NULL) == 0);
	assert(!linkr_debugger_logic_analyzer_stream_copy_complete_advance_reader(
		&progress, &metrics, false, 5U, 0U, 4U));
	assert(progress.reader_seq == 0U);
	linkr_debugger_logic_analyzer_stream_sink_abort_payload(&sink, &lease);
	assert(ctx.abort_calls == 1U);
	assert(ctx.commit_calls == 0U);
	free(lane_words);
}

static void test_stream_sink_commit_failure_aborts_after_reader_advance(void)
{
	struct sink_test_context ctx;
	struct linkr_debugger_la_stream_sink sink;
	struct linkr_debugger_la_stream_sink_lease lease;
	struct linkr_debugger_la_stream_sink_commit commit;
	struct linkr_debugger_la_ring_progress progress;
	struct linkr_debugger_la_ring_metrics metrics;
	struct linkr_debugger_la_config config = fast4_config();
	struct linkr_debugger_la_packed_ring_plan plan;
	uint32_t raw_ring[] = { 0U, 1U, 2U, 3U };
	uint32_t *lane_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};

	memset(&ctx, 0, sizeof(ctx));
	memset(&progress, 0, sizeof(progress));
	memset(&metrics, 0, sizeof(metrics));
	ctx.capacity = sizeof(ctx.storage);
	ctx.commit_ret = -EIO;
	sink = sink_for_context(&ctx);
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	lane_words = alloc_lane_words(plan.lanes[0].word_count);
	fill_single_lane_packed_ring_from_raw_samples(&config, &plan, lane_words, raw_ring, 4U);
	lane_ptrs[0] = lane_words;
	lane_counts[0] = plan.lanes[0].word_count;
	progress.generation = 6U;
	progress.writer_seq = 4U;
	progress.reader_seq = 0U;
	assert(linkr_debugger_logic_analyzer_stream_sink_lease_payload(&sink, 4U,
		&lease) == 0);
	assert(linkr_debugger_logic_analyzer_stream_sink_write_packed_payload(&plan,
		lane_ptrs, lane_counts, ring_test_start_sample(&plan, 4U, 0U),
		4U, 1U, lease.payload, lease.capacity,
		NULL, NULL) == 0);
	assert(linkr_debugger_logic_analyzer_stream_copy_complete_advance_reader(
		&progress, &metrics, true, 6U, 0U, 4U));
	assert(progress.reader_seq == 4U);
	memset(&commit, 0, sizeof(commit));
	commit.token = lease.token;
	commit.sample_count = 4U;
	commit.bytes_per_sample = 1U;
	commit.payload_len = 4U;
	assert(linkr_debugger_logic_analyzer_stream_sink_commit_payload(&sink, &commit) == -EIO);
	linkr_debugger_logic_analyzer_stream_sink_abort_payload(&sink, &lease);
	assert(ctx.commit_calls == 1U);
	assert(ctx.abort_calls == 1U);
	free(lane_words);
}

static void test_stream_sink_commit_positive_return_requests_handoff(void)
{
	struct sink_test_context ctx;
	struct linkr_debugger_la_stream_sink sink;
	struct linkr_debugger_la_stream_sink_commit commit;

	memset(&ctx, 0, sizeof(ctx));
	ctx.commit_ret = 1;
	sink = sink_for_context(&ctx);
	memset(&commit, 0, sizeof(commit));
	commit.token = &ctx;
	commit.sample_count = 4U;
	commit.bytes_per_sample = 1U;
	commit.payload_len = 4U;
	assert(linkr_debugger_logic_analyzer_stream_sink_commit_payload(&sink, &commit) == 1);
	assert(ctx.commit_calls == 1U);
	assert(ctx.abort_calls == 0U);
}

static void test_stream_sink_single_packed_output_wraps_packed_ring_words(void)
{
	struct linkr_debugger_la_config config;
	struct linkr_debugger_la_packed_ring_plan plan;
	uint32_t raw_ring[4];
	uint32_t *lane_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint8_t out[5];
	uint16_t values_or;
	uint16_t values_and;

	memset(&config, 0, sizeof(config));
	config.pin_base = 10U;
	config.pin_count = 1U;
	config.selected_pins[0] = 10U;
	config.selected_pin_count = 1U;
	config.sample_rate_hz = 1000000U;
	raw_ring[0] = (uint32_t)BIT(0);
	raw_ring[1] = 0U;
	raw_ring[2] = 0U;
	raw_ring[3] = (uint32_t)BIT(0);
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	lane_words = alloc_lane_words(plan.lanes[0].word_count);
	fill_single_lane_packed_ring_from_raw_samples(&config, &plan, lane_words, raw_ring, 4U);
	lane_ptrs[0] = lane_words;
	lane_counts[0] = plan.lanes[0].word_count;
	memset(out, 0xff, sizeof(out));
	assert(linkr_debugger_logic_analyzer_stream_sink_write_packed_payload(&plan,
		lane_ptrs, lane_counts, ring_test_start_sample(&plan, 4U, 2U),
		5U, 1U, out, sizeof(out), &values_or,
		&values_and) == 0);
	assert(memcmp(out, (uint8_t[]){0x00U, 0x01U, 0x01U, 0x00U, 0x00U}, 5U) == 0);
	assert(values_or == 0x0001U);
	assert(values_and == 0x0000U);
	free(lane_words);
}

static void assert_single_sink_matches_generic(
	const struct linkr_debugger_la_config *config,
	const uint32_t *raw_ring,
	uint32_t ring_samples,
	uint64_t first_seq,
	uint32_t sample_count)
{
	struct linkr_debugger_la_packed_ring_plan plan;
	uint32_t *lane_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint8_t out[8];
	uint8_t expected[8];
	uint16_t values_or;
	uint16_t values_and;
	uint16_t expected_or = 0U;
	uint16_t expected_and = 0xffffU;

	assert(sample_count <= sizeof(out));
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(config, &plan) == 0);
	lane_words = alloc_lane_words(plan.lanes[0].word_count);
	fill_single_lane_packed_ring_from_raw_samples(config, &plan, lane_words, raw_ring, ring_samples);
	lane_ptrs[0] = lane_words;
	lane_counts[0] = plan.lanes[0].word_count;
	memset(out, 0xff, sizeof(out));
	memset(expected, 0xff, sizeof(expected));
	assert(linkr_debugger_logic_analyzer_stream_sink_write_packed_payload(&plan,
		lane_ptrs, lane_counts,
		ring_test_start_sample(&plan, ring_samples, (uint32_t)first_seq),
		sample_count, 1U, out, sample_count,
		&values_or, &values_and) == 0);

	for (uint32_t i = 0U; i < sample_count; i++) {
		uint32_t raw_index = (uint32_t)((first_seq + i) % ring_samples);
		uint16_t sample = linkr_debugger_logic_analyzer_compress_raw_sample(
			raw_ring[raw_index], config);

		expected[i] = (uint8_t)sample;
		expected_or |= sample;
		expected_and &= sample;
	}

	assert(memcmp(out, expected, sample_count) == 0);
	assert(values_or == expected_or);
	assert(values_and == expected_and);
	free(lane_words);
}

static void assert_sink_matches_decoded_span(
	const struct linkr_debugger_la_packed_ring_plan *plan,
	const uint32_t * const lane_ptrs[],
	const uint32_t lane_counts[],
	uint32_t first_sample,
	uint32_t sample_count)
{
	uint16_t values_or;
	uint16_t values_and;
	uint16_t expected_or = 0U;
	uint16_t expected_and = 0xffffU;
	uint8_t bytes_per_sample = plan->bytes_per_sample;
	size_t payload_len = (size_t)sample_count * bytes_per_sample;
	uint8_t *out = malloc(payload_len);
	uint8_t *expected = malloc(payload_len);

	assert(payload_len > 0U);
	assert(out != NULL);
	assert(expected != NULL);
	memset(out, 0xa5, payload_len);
	memset(expected, 0x5a, payload_len);
	assert(linkr_debugger_logic_analyzer_decode_packed_ring_span(plan, lane_ptrs,
		lane_counts, first_sample, expected, payload_len, sample_count) == 0);
	assert(linkr_debugger_logic_analyzer_stream_sink_write_packed_payload(plan,
		lane_ptrs, lane_counts, first_sample, sample_count, bytes_per_sample, out,
		payload_len, &values_or, &values_and) == 0);
	assert(memcmp(out, expected, payload_len) == 0);

	for (uint32_t i = 0U; i < sample_count; i++) {
		uint16_t sample = expected[(size_t)i * bytes_per_sample];

		if (bytes_per_sample > 1U) {
			sample |= (uint16_t)expected[((size_t)i * bytes_per_sample) + 1U] << 8U;
		}
		expected_or |= sample;
		expected_and &= sample;
	}

	assert(values_or == expected_or);
	assert(values_and == expected_and);
	free(expected);
	free(out);
}

static void test_stream_sink_single_bits_payload_wraps_and_masks_tail(void)
{
	struct linkr_debugger_la_config config = single_plan_config(0U);
	struct linkr_debugger_la_packed_ring_plan plan;
	struct sink_test_context ctx;
	struct linkr_debugger_la_stream_sink sink;
	uint32_t *lane_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	const uint8_t samples[] = {
		1U, 0U, 1U, 1U, 0U, 0U, 1U, 0U,
		1U, 1U, 1U, 0U, 0U, 1U, 0U, 1U,
		1U, 0U, 1U,
	};
	uint8_t expected[(sizeof(samples) + 7U) / 8U] = {0};
	uint8_t out[sizeof(expected)];
	uint16_t values_or;
	uint16_t values_and;
	uint32_t first_sample;

	memset(&ctx, 0, sizeof(ctx));
	sink = sink_for_context(&ctx);
	sink.format = LINKR_DEBUGGER_LA_STREAM_PAYLOAD_SINGLE_BITS;
	sink.max_chunk_samples = LINKR_DEBUGGER_LA_STREAM_MAX_SINGLE_BITS_CHUNK_SAMPLES;
	assert(linkr_debugger_logic_analyzer_stream_sink_validate(&config, &sink) == 0);
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	lane_words = alloc_lane_words(plan.lanes[0].word_count);
	first_sample = plan.sample_capacity - 5U;
	for (size_t i = 0U; i < sizeof(samples); i++) {
		packed_ring_lane_store(&plan.lanes[0], lane_words,
			(first_sample + (uint32_t)i) % plan.sample_capacity, samples[i]);
		expected[i >> 3U] |= (uint8_t)(samples[i] << (i & 7U));
	}
	lane_ptrs[0] = lane_words;
	lane_counts[0] = plan.lanes[0].word_count;
	memset(out, 0xff, sizeof(out));
	assert(linkr_debugger_logic_analyzer_stream_sink_write_payload(&sink, &plan,
		lane_ptrs, lane_counts, first_sample, (uint32_t)sizeof(samples), out,
		sizeof(out), &values_or, &values_and) == 0);
	assert(memcmp(out, expected, sizeof(out)) == 0);
	assert((out[sizeof(out) - 1U] & 0xf8U) == 0U);
	assert(values_or == 1U);
	assert(values_and == 0U);
	free(lane_words);
}

static void test_stream_sink_single_bits_all_high_and_chunk_contract(void)
{
	struct linkr_debugger_la_config config = single_plan_config(0U);
	struct linkr_debugger_la_packed_ring_plan plan;
	struct sink_test_context ctx;
	struct linkr_debugger_la_stream_sink sink;
	uint32_t *lane_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint8_t *out;
	uint16_t values_or;
	uint16_t values_and;
	size_t payload_len;

	memset(&ctx, 0, sizeof(ctx));
	sink = sink_for_context(&ctx);
	sink.format = LINKR_DEBUGGER_LA_STREAM_PAYLOAD_SINGLE_BITS;
	sink.max_chunk_samples = LINKR_DEBUGGER_LA_STREAM_MAX_SINGLE_BITS_CHUNK_SAMPLES;
	assert(linkr_debugger_logic_analyzer_stream_sink_validate(&config, &sink) == 0);
	sink.max_chunk_samples++;
	assert(linkr_debugger_logic_analyzer_stream_sink_validate(&config, &sink) == -EINVAL);
	sink.max_chunk_samples--;
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	lane_words = alloc_lane_words(plan.lanes[0].word_count);
	for (uint32_t i = 0U; i < plan.lanes[0].word_count; i++) {
		lane_words[i] = UINT32_MAX;
	}
	lane_ptrs[0] = lane_words;
	lane_counts[0] = plan.lanes[0].word_count;
	payload_len = linkr_debugger_logic_analyzer_stream_payload_len(sink.format,
		LINKR_DEBUGGER_LA_STREAM_MAX_SINGLE_BITS_CHUNK_SAMPLES, 1U);
	assert(payload_len == LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES);
	out = malloc(payload_len);
	assert(out != NULL);
	assert(linkr_debugger_logic_analyzer_stream_sink_write_payload(&sink, &plan,
		lane_ptrs, lane_counts, 0U,
		LINKR_DEBUGGER_LA_STREAM_MAX_SINGLE_BITS_CHUNK_SAMPLES, out,
		payload_len, &values_or, &values_and) == 0);
	for (size_t i = 0U; i < payload_len; i++) {
		assert(out[i] == 0xffU);
	}
	assert(values_or == 1U);
	assert(values_and == 1U);
	free(out);
	free(lane_words);
}

static void test_stream_sink_single_fast_path_matches_generic_patterns(void)
{
	struct linkr_debugger_la_config config;
	uint32_t zeros[4] = {0};
	uint32_t ones[4];
	uint32_t mixed[5];

	memset(&config, 0, sizeof(config));
	config.pin_base = 7U;
	config.pin_count = 4U;
	config.selected_pins[0] = 10U;
	config.selected_pin_count = 1U;
	config.sample_rate_hz = 1000000U;
	for (size_t i = 0U; i < sizeof(ones) / sizeof(ones[0]); i++) {
		ones[i] = (uint32_t)BIT(3);
	}
	mixed[0] = 0U;
	mixed[1] = (uint32_t)BIT(3);
	mixed[2] = (uint32_t)BIT(2);
	mixed[3] = (uint32_t)(BIT(3) | BIT(0));
	mixed[4] = 0U;

	assert_single_sink_matches_generic(&config, zeros, 4U, 0U, 4U);
	assert_single_sink_matches_generic(&config, ones, 4U, 0U, 4U);
	assert_single_sink_matches_generic(&config, mixed, 5U, 3U, 7U);
}

static void test_stream_sink_single_fast_path_span_boundaries_match_generic(void)
{
	struct linkr_debugger_la_config config;
	uint32_t raw_ring[] = {
		0U,
		(uint32_t)BIT(2),
		0U,
		(uint32_t)BIT(2),
		(uint32_t)BIT(1),
		(uint32_t)BIT(2),
	};

	memset(&config, 0, sizeof(config));
	config.pin_base = 8U;
	config.pin_count = 4U;
	config.selected_pins[0] = 10U;
	config.selected_pin_count = 1U;
	config.sample_rate_hz = 1000000U;

	assert_single_sink_matches_generic(&config, raw_ring, 6U, 1U, 3U);
	assert_single_sink_matches_generic(&config, raw_ring, 6U, 3U, 3U);
	assert_single_sink_matches_generic(&config, raw_ring, 6U, 5U, 2U);
	assert_single_sink_matches_generic(&config, raw_ring, 6U, 4U, 5U);
}

static void test_stream_sink_wrap_batch_matches_decode_span_single_byte(void)
{
	struct linkr_debugger_la_config config = fast8_plan_config(0U);
	struct linkr_debugger_la_packed_ring_plan plan;
	uint32_t raw_ring[] = {
		0U,
		(uint32_t)BIT(2),
		(uint32_t)(BIT(0) | BIT(7)),
		(uint32_t)BIT(7),
		(uint32_t)BIT(0),
	};
	uint32_t *lane_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};

	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	lane_words = alloc_lane_words(plan.lanes[0].word_count);
	fill_single_lane_packed_ring_from_raw_samples(&config, &plan, lane_words, raw_ring,
		sizeof(raw_ring) / sizeof(raw_ring[0]));
	lane_ptrs[0] = lane_words;
	lane_counts[0] = plan.lanes[0].word_count;
	assert_sink_matches_decoded_span(&plan, lane_ptrs, lane_counts,
		ring_test_start_sample(&plan, 5U, 3U), 7U);
	free(lane_words);
}

static void test_stream_sink_wrap_batch_matches_decode_span_two_bytes(void)
{
	struct linkr_debugger_la_config config = wide11_exact_config();
	struct linkr_debugger_la_packed_ring_plan plan;
	static const uint16_t ring_samples[] = {
		0x0000U,
		0x0401U,
		0x0555U,
		0x02aaU,
	};
	uint32_t *lane_a_words;
	uint32_t *lane_b_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t base;

	config.post_samples = 0U;
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	assert(plan.lane_count == 2U);
	assert(plan.bytes_per_sample == 2U);
	lane_a_words = alloc_lane_words(plan.lanes[0].word_count);
	lane_b_words = alloc_lane_words(plan.lanes[1].word_count);
	base = plan.sample_capacity - (uint32_t)(sizeof(ring_samples) / sizeof(ring_samples[0]));
	for (uint32_t i = 0U; i < (uint32_t)(sizeof(ring_samples) / sizeof(ring_samples[0])); i++) {
		uint16_t sample = ring_samples[i];

		packed_ring_lane_store(&plan.lanes[0], lane_a_words, base + i, sample & 0x00ffU);
		packed_ring_lane_store(&plan.lanes[1], lane_b_words, base + i,
			(sample >> 8U) & 0x07U);
		packed_ring_lane_store(&plan.lanes[0], lane_a_words, i, sample & 0x00ffU);
		packed_ring_lane_store(&plan.lanes[1], lane_b_words, i,
			(sample >> 8U) & 0x07U);
	}
	lane_ptrs[0] = lane_a_words;
	lane_ptrs[1] = lane_b_words;
	lane_counts[0] = plan.lanes[0].word_count;
	lane_counts[1] = plan.lanes[1].word_count;
	assert_sink_matches_decoded_span(&plan, lane_ptrs, lane_counts, base + 2U, 5U);
	free(lane_a_words);
	free(lane_b_words);
}

static void test_stream_sink_fast8_large_random_wrap_matches_generic(void)
{
	struct linkr_debugger_la_config config = fast8_plan_config(0U);
	struct linkr_debugger_la_packed_ring_plan plan;
	uint32_t *lane_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};

	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED);
	assert(plan.selected_pin_count == 8U);
	lane_words = alloc_lane_words(plan.lanes[0].word_count);
	for (uint32_t i = 0U; i < plan.lanes[0].word_count; i++) {
		lane_words[i] = (i * 2654435761U) ^ (i >> 3U) ^ 0xa5c39e71U;
	}
	lane_ptrs[0] = lane_words;
	lane_counts[0] = plan.lanes[0].word_count;
	assert_sink_matches_decoded_span(&plan, lane_ptrs, lane_counts,
		plan.sample_capacity - 1023U, 2048U);
	free(lane_words);
}

static void test_stream_sink_wide11_large_random_lane_wraps_match_generic(void)
{
	struct linkr_debugger_la_config config = wide11_exact_config();
	struct linkr_debugger_la_packed_ring_plan plan;
	uint32_t *lane_a_words;
	uint32_t *lane_b_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};

	config.post_samples = 0U;
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_WIDE11_SPLIT_PACKED);
	assert(plan.selected_pin_count == 11U);
	assert(plan.lanes[1].sample_capacity > plan.sample_capacity);
	lane_a_words = alloc_lane_words(plan.lanes[0].word_count);
	lane_b_words = alloc_lane_words(plan.lanes[1].word_count);
	for (uint32_t i = 0U; i < plan.lanes[0].word_count; i++) {
		lane_a_words[i] = (i * 2246822519U) ^ (i >> 5U) ^ 0x6d2b79f5U;
	}
	for (uint32_t i = 0U; i < plan.lanes[1].word_count; i++) {
		lane_b_words[i] = (i * 3266489917U) ^ (i >> 7U) ^ 0x9e3779b9U;
	}
	lane_ptrs[0] = lane_a_words;
	lane_ptrs[1] = lane_b_words;
	lane_counts[0] = plan.lanes[0].word_count;
	lane_counts[1] = plan.lanes[1].word_count;
	assert_sink_matches_decoded_span(&plan, lane_ptrs, lane_counts,
		plan.sample_capacity - 513U, 1024U);
	assert_sink_matches_decoded_span(&plan, lane_ptrs, lane_counts,
		plan.lanes[1].sample_capacity - 129U, 257U);
	free(lane_a_words);
	free(lane_b_words);
}

static void test_stream_sink_sparse_fast8_selection_keeps_generic_order(void)
{
	static const uint8_t pins[] = { 17U, 10U, 14U, 12U };
	struct linkr_debugger_la_config config = fast8_plan_config(0U);
	struct linkr_debugger_la_packed_ring_plan plan;
	uint32_t *lane_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};

	memcpy(config.selected_pins, pins, sizeof(pins));
	config.selected_pin_count = (uint8_t)(sizeof(pins) / sizeof(pins[0]));
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED);
	assert(plan.selected_pin_count == 4U);
	lane_words = alloc_lane_words(plan.lanes[0].word_count);
	for (uint32_t i = 0U; i < plan.lanes[0].word_count; i++) {
		lane_words[i] = (i * 2654435761U) ^ 0x13579bdfU;
	}
	lane_ptrs[0] = lane_words;
	lane_counts[0] = plan.lanes[0].word_count;
	assert_sink_matches_decoded_span(&plan, lane_ptrs, lane_counts,
		plan.sample_capacity - 31U, 64U);
	free(lane_words);
}

static void test_stream_sink_single_fast_path_default_selection_matches_generic(void)
{
	struct linkr_debugger_la_config config;
	uint32_t raw_ring[] = { 0U, (uint32_t)BIT(0), (uint32_t)BIT(1), 0U };

	memset(&config, 0, sizeof(config));
	config.pin_base = 10U;
	config.pin_count = 1U;
	config.selected_pin_count = 0U;
	config.sample_rate_hz = 1000000U;
	assert_single_sink_matches_generic(&config, raw_ring, 4U, 1U, 4U);
}

static void test_stream_sink_single_fast_path_rejects_invalid_offsets(void)
{
	struct linkr_debugger_la_config config;
	struct linkr_debugger_la_packed_ring_plan plan;
	uint32_t *lane_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint8_t out[1];

	memset(&config, 0, sizeof(config));
	config.pin_base = 9U;
	config.pin_count = 1U;
	config.selected_pins[0] = 9U;
	config.selected_pin_count = 1U;
	config.sample_rate_hz = 1000000U;
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	lane_words = alloc_lane_words(plan.lanes[0].word_count);
	lane_ptrs[0] = lane_words;
	lane_counts[0] = plan.lanes[0].word_count;
	assert(linkr_debugger_logic_analyzer_stream_sink_write_packed_payload(&plan,
		lane_ptrs, lane_counts, 0U, 1U, 1U, out, sizeof(out), NULL, NULL) == 0);

	free(lane_words);
}

static void test_stream_sink_retries_only_precommit_backpressure_errors(void)
{
	assert(linkr_debugger_logic_analyzer_stream_sink_backpressure_retryable(-ENOSPC));
	assert(linkr_debugger_logic_analyzer_stream_sink_backpressure_retryable(-ENOMEM));
	assert(linkr_debugger_logic_analyzer_stream_sink_backpressure_retryable(-EAGAIN));
	assert(!linkr_debugger_logic_analyzer_stream_sink_backpressure_retryable(-EINVAL));
	assert(!linkr_debugger_logic_analyzer_stream_sink_backpressure_retryable(-ENOTCONN));
	assert(!linkr_debugger_logic_analyzer_stream_sink_backpressure_retryable(-EIO));
}

static void test_stream_sink_consumer_can_yield_to_transport_thread(void)
{
	assert(linkr_debugger_logic_analyzer_stream_consumer_priority(true) == 8);
}

static void test_stream_sink_helpers_do_not_change_callback_protocol_gate(void)
{
	assert(LINKR_DEBUGGER_LA_STREAM_HANDOFF_UNREAD_SAMPLES ==
		LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES);
	assert(linkr_debugger_logic_analyzer_stream_callback_allows_protocol_update(true, 1U));
	assert(!linkr_debugger_logic_analyzer_stream_callback_allows_protocol_update(true, 0U));
	assert(!linkr_debugger_logic_analyzer_stream_callback_allows_protocol_update(false, 1U));
	assert(linkr_debugger_logic_analyzer_stream_callback_should_yield_after_chunk(true, 1U));
	assert(!linkr_debugger_logic_analyzer_stream_callback_should_yield_after_chunk(true, 0U));
	assert(!linkr_debugger_logic_analyzer_stream_callback_should_yield_after_chunk(false, 1U));
	assert(linkr_debugger_logic_analyzer_stream_consumer_priority(true) == 8);
	assert(linkr_debugger_logic_analyzer_stream_consumer_priority(false) == 8);
	assert(!linkr_debugger_logic_analyzer_stream_sink_should_yield_for_handoff(false, 0U));
	assert(linkr_debugger_logic_analyzer_stream_sink_should_yield_for_handoff(true, 0U));
	assert(linkr_debugger_logic_analyzer_stream_sink_should_yield_for_handoff(true, 2047U));
	assert(linkr_debugger_logic_analyzer_stream_sink_should_yield_for_handoff(true, 2048U));
	assert(!linkr_debugger_logic_analyzer_stream_sink_should_yield_for_handoff(true, 2049U));
	assert(!linkr_debugger_logic_analyzer_stream_sink_should_explicit_yield(false, 0U));
	assert(linkr_debugger_logic_analyzer_stream_sink_should_explicit_yield(true, 0U));
	assert(linkr_debugger_logic_analyzer_stream_sink_should_explicit_yield(true, 2048U));
	assert(!linkr_debugger_logic_analyzer_stream_sink_should_explicit_yield(true, 2049U));
}

static void test_validation(void)
{
	struct linkr_debugger_la_config config = base_config();

	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == 0);

	config = base_config();
	config.selected_pins[0] = 7U;
	config.selected_pins[1] = 10U;
	config.selected_pins[2] = 20U;
	config.selected_pin_count = 3U;
	config.pin_count = 3U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == 0);

	config = base_config();
	config.pin_base = 6U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.pin_base = 21U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.selected_pins[0] = 7U;
	config.selected_pins[1] = 7U;
	config.selected_pin_count = 2U;
	config.pin_count = 2U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.pre_samples = 1U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	config.trigger_pin = 4U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	config.trigger_pin = 1U;
	config.pre_samples = 8U;
	config.post_samples = 8U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_EITHER;
	config.trigger_pin = 1U;
	config.pre_samples = 8U;
	config.post_samples = 8U;
	config.sample_rate_hz = 25000000U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	config.trigger_pin = 1U;
	config.pre_samples = 8U;
	config.post_samples = 8U;
	config.sample_rate_hz = 25000001U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.sample_rate_hz = 125000001U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.post_samples = 513U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);
}

static void test_stream_validation_allows_unlimited_and_uint16_bounded(void)
{
	struct linkr_debugger_la_config config = base_config();

	config.post_samples = 0U;
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == 0);

	config = base_config();
	config.post_samples = UINT16_MAX;
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == 0);

	config = base_config();
	config.post_samples = (uint32_t)UINT16_MAX + 1U;
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == -EINVAL);

	config = base_config();
	config.pin_base = 6U;
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == -EINVAL);

	config = base_config();
	config.selected_pins[0] = 7U;
	config.selected_pins[1] = 7U;
	config.selected_pin_count = 2U;
	config.pin_count = 2U;
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == -EINVAL);

	config = base_config();
	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ + 1U;
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == -EINVAL);

	config = base_config();
	config.pre_samples = 1U;
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == -EINVAL);
}

static void test_bounded_pre_trigger_uses_prepared_packed_ring(void)
{
	struct linkr_debugger_la_config config = fast8_plan_config(16U);
	struct linkr_debugger_la_hardware_plan plan;
	struct linkr_debugger_la_packed_ring_plan ring_plan;
	struct linkr_debugger_la_session_contract contract;

	config.sample_rate_hz = 1000000U;
	config.pre_samples = 16U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config,
		LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES) == 0);
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == 0);
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false,
		&plan) == 0);
	assert(plan.supported);
	assert(plan.pipeline_family == LINKR_DEBUGGER_LA_PIPELINE_FAMILY_COMMON_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_RING_STREAM);
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &ring_plan) == 0);
	assert(linkr_debugger_logic_analyzer_session_contract(&config, &contract) == 0);
	assert(contract.pre_samples == 16U);
	assert(contract.post_samples == 16U);
	assert(contract.stop_policy == LINKR_DEBUGGER_LA_STOP_POLICY_BOUNDED);
}

static void test_bounded_pre_trigger_rejects_unsupported_contract_shapes(void)
{
	struct linkr_debugger_la_config config = fast8_plan_config(1U);

	config.sample_rate_hz = 1000000U;
	config.pre_samples = 1U;
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == 0);

	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == -EINVAL);

	config = fast8_plan_config(0U);
	config.sample_rate_hz = 1000000U;
	config.pre_samples = 1U;
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == -EINVAL);

	config = fast8_plan_config(1U);
	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_PRE_TRIGGER_SAMPLE_RATE_HZ + 1U;
	config.pre_samples = 1U;
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == -EINVAL);

	config = fast8_plan_config(1U);
	config.sample_rate_hz = 1000000U;
	config.pre_samples = LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES;
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == -EINVAL);
}

static void test_bounded_pre_trigger_window_preserves_index_and_wraps(void)
{
	struct linkr_debugger_la_config config = fast8_plan_config(4U);
	struct linkr_debugger_la_pre_trigger_window window;
	uint32_t target_samples = 0U;

	config.sample_rate_hz = 1000000U;
	config.pre_samples = 4U;
	assert(linkr_debugger_logic_analyzer_bounded_sample_target(4U, 4U,
		&target_samples) == 0);
	assert(target_samples == 8U);
	assert(linkr_debugger_logic_analyzer_pre_trigger_supported(config.trigger,
		config.sample_rate_hz, config.pre_samples, config.post_samples));
	assert(linkr_debugger_logic_analyzer_pre_trigger_window(&config, 34U, 30U,
		32U, 4U, &window) == 0);
	assert(window.first_sequence == 26U);
	assert(window.end_sequence == 34U);
	assert(window.sample_count == 8U);
	assert(window.trigger_index == 4U);
	assert(window.first_sequence % 32U == 26U);
	assert(window.end_sequence % 32U == 2U);
	assert(linkr_debugger_logic_analyzer_pre_trigger_window(&config, 4U, 3U,
		32U, 4U, &window) == -ENODATA);
	assert(linkr_debugger_logic_analyzer_pre_trigger_window(&config, 55U, 30U,
		32U, 4U, &window) == -EOVERFLOW);
	assert(linkr_debugger_logic_analyzer_bounded_sample_target(UINT32_MAX, 1U,
		&target_samples) == -EOVERFLOW);
}

static void test_bounded_pre_trigger_plan_feasibility(void)
{
	struct linkr_debugger_la_config config;
	struct linkr_debugger_la_hardware_plan hardware_plan;
	struct linkr_debugger_la_packed_ring_plan ring_plan;
	uint32_t required_samples = 0U;

	assert(linkr_debugger_logic_analyzer_pre_trigger_minimum_retention_samples(
		1000000U, &required_samples) == 0);
	assert(required_samples == 2000U);
	assert(linkr_debugger_logic_analyzer_pre_trigger_minimum_retention_samples(
		25000000U, &required_samples) == 0);
	assert(required_samples == 50000U);
	assert(linkr_debugger_logic_analyzer_pre_trigger_minimum_retention_samples(
		UINT32_MAX, &required_samples) == 0);
	assert(required_samples == 8589936U);

	config = single_plan_config(256U);
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	config.pre_samples = 256U;
	config.sample_rate_hz = 25000000U;
	assert(linkr_debugger_logic_analyzer_pre_trigger_supported(config.trigger,
		config.sample_rate_hz, config.pre_samples, config.post_samples));
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &ring_plan) == 0);
	assert(linkr_debugger_logic_analyzer_pre_trigger_plan_feasible(&ring_plan,
		linkr_debugger_logic_analyzer_actual_rate(config.sample_rate_hz)));
	assert(linkr_debugger_logic_analyzer_pre_trigger_plan_supported(&config));
	assert(linkr_debugger_logic_analyzer_validate_config(&config,
		LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES) == 0);
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == 0);
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false,
		&hardware_plan) == 0);
	assert(hardware_plan.supported);

	for (enum linkr_debugger_la_trigger_type trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	     trigger <= LINKR_DEBUGGER_LA_TRIGGER_EITHER; trigger++) {
		config.trigger = trigger;
		config.sample_rate_hz = 1000000U;
		assert(linkr_debugger_logic_analyzer_pre_trigger_plan_supported(&config));
	}

	config = fast8_plan_config(256U);
	config.pre_samples = 256U;
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &ring_plan) == 0);
	assert(!linkr_debugger_logic_analyzer_pre_trigger_plan_feasible(&ring_plan,
		linkr_debugger_logic_analyzer_actual_rate(config.sample_rate_hz)));
	assert(!linkr_debugger_logic_analyzer_pre_trigger_plan_supported(&config));
	assert(linkr_debugger_logic_analyzer_validate_config(&config,
		LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false,
		&hardware_plan) == 0);
	assert(!hardware_plan.supported);

	config = wide11_exact_config();
	config.sample_rate_hz = 25000000U;
	config.pre_samples = 256U;
	config.post_samples = 256U;
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_FALLING;
	config.trigger_pin = 0U;
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &ring_plan) == 0);
	assert(!linkr_debugger_logic_analyzer_pre_trigger_plan_feasible(&ring_plan,
		linkr_debugger_logic_analyzer_actual_rate(config.sample_rate_hz)));
	assert(!linkr_debugger_logic_analyzer_pre_trigger_plan_supported(&config));
	assert(linkr_debugger_logic_analyzer_validate_stream_config(&config) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false,
		&hardware_plan) == 0);
	assert(!hardware_plan.supported);

	config = single_plan_config(256U);
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;
	config.pre_samples = 256U;
	assert(!linkr_debugger_logic_analyzer_pre_trigger_plan_supported(&config));
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	config.post_samples = 0U;
	assert(!linkr_debugger_logic_analyzer_pre_trigger_plan_supported(&config));
	config.post_samples = 257U;
	assert(!linkr_debugger_logic_analyzer_pre_trigger_plan_supported(&config));
}

static void test_bounded_pre_trigger_scan_guards(void)
{
	assert(linkr_debugger_logic_analyzer_pre_trigger_scan_start(0U, 16U) == 16U);
	assert(linkr_debugger_logic_analyzer_pre_trigger_scan_start(16U, 16U) == 16U);
	assert(linkr_debugger_logic_analyzer_pre_trigger_scan_start(31U, 16U) == 16U);
	assert(linkr_debugger_logic_analyzer_pre_trigger_scan_start(32U, 16U) == 16U);
	assert(linkr_debugger_logic_analyzer_pre_trigger_scan_start(33U, 16U) == 17U);

	assert(linkr_debugger_logic_analyzer_pre_trigger_edge_matches(
		LINKR_DEBUGGER_LA_TRIGGER_RISING, 0U, 1U));
	assert(!linkr_debugger_logic_analyzer_pre_trigger_edge_matches(
		LINKR_DEBUGGER_LA_TRIGGER_RISING, 1U, 0U));
	assert(linkr_debugger_logic_analyzer_pre_trigger_edge_matches(
		LINKR_DEBUGGER_LA_TRIGGER_FALLING, 1U, 0U));
	assert(!linkr_debugger_logic_analyzer_pre_trigger_edge_matches(
		LINKR_DEBUGGER_LA_TRIGGER_FALLING, 0U, 1U));
	assert(linkr_debugger_logic_analyzer_pre_trigger_edge_matches(
		LINKR_DEBUGGER_LA_TRIGGER_EITHER, 0U, 1U));
	assert(linkr_debugger_logic_analyzer_pre_trigger_edge_matches(
		LINKR_DEBUGGER_LA_TRIGGER_EITHER, 1U, 0U));
	assert(!linkr_debugger_logic_analyzer_pre_trigger_edge_matches(
		LINKR_DEBUGGER_LA_TRIGGER_EITHER, 1U, 1U));

	assert(!linkr_debugger_logic_analyzer_pre_trigger_scan_source_overrun(
		14336U, 0U, 16384U, 2048U));
	assert(linkr_debugger_logic_analyzer_pre_trigger_scan_source_overrun(
		14337U, 0U, 16384U, 2048U));
}

static void test_compression(void)
{
	struct linkr_debugger_la_config config = base_config();

	/* Raw PIO sample words are relative to pin_base, not absolute GPIO numbers. */
	assert(linkr_debugger_logic_analyzer_compress_raw_sample(
		(uint32_t)(BIT(0) | BIT(2)), &config) == 5U);
	assert(linkr_debugger_logic_analyzer_compress_raw_sample(
		(uint32_t)BIT(3), &config) == 8U);

	config.selected_pins[0] = 7U;
	config.selected_pins[1] = 10U;
	config.selected_pins[2] = 20U;
	config.selected_pin_count = 3U;
	config.pin_count = 3U;

	assert(linkr_debugger_logic_analyzer_compress_raw_sample(
		(uint32_t)(BIT(0) | BIT(13)), &config) == 5U);
	assert(linkr_debugger_logic_analyzer_compress_raw_sample(
		(uint32_t)BIT(3), &config) == 2U);
}

static void test_stream_irq_clear_helper(void)
{
	struct linkr_debugger_la_config config = base_config();

	assert(!linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(NULL));

	assert(!linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(&config));

	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;
	assert(!linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(&config));

	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	assert(linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(&config));

	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_FALLING;
	assert(linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(&config));

	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_EITHER;
	assert(linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(&config));

	config.pre_samples = 1U;
	assert(!linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(&config));
}

static struct linkr_debugger_la_config wide11_exact_config(void)
{
	static const uint8_t pins[] = {
		10U, 11U, 12U, 13U, 14U, 15U,
		16U, 17U, 18U, 19U, 20U,
	};
	struct linkr_debugger_la_config config;

	memset(&config, 0, sizeof(config));
	config.pin_base = 10U;
	config.pin_count = 11U;
	memcpy(config.selected_pins, pins, sizeof(pins));
	config.selected_pin_count = 11U;
	config.sample_rate_hz = LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ;
	config.pre_samples = 0U;
	config.post_samples = LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES;
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;
	return config;
}

static void test_wide11_burst_exact_eligibility_is_strict(void)
{
	struct linkr_debugger_la_config config = wide11_exact_config();

	assert(linkr_debugger_logic_analyzer_wide11_burst_exact_eligible(&config));

	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	config.trigger_pin = 10U;
	assert(linkr_debugger_logic_analyzer_wide11_burst_exact_eligible(&config));

	config = wide11_exact_config();
	config.sample_rate_hz = 99999000U;
	assert(!linkr_debugger_logic_analyzer_wide11_burst_exact_eligible(&config));

	config = wide11_exact_config();
	config.post_samples = LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES - 1U;
	assert(!linkr_debugger_logic_analyzer_wide11_burst_exact_eligible(&config));

	config = wide11_exact_config();
	config.pre_samples = 1U;
	assert(!linkr_debugger_logic_analyzer_wide11_burst_exact_eligible(&config));

	config = wide11_exact_config();
	config.selected_pins[10] = 21U;
	assert(!linkr_debugger_logic_analyzer_wide11_burst_exact_eligible(&config));

	config = wide11_exact_config();
	config.selected_pin_count = 0U;
	assert(!linkr_debugger_logic_analyzer_wide11_burst_exact_eligible(&config));
}

static struct linkr_debugger_la_config single_plan_config(uint32_t post_samples)
{
	struct linkr_debugger_la_config config;

	memset(&config, 0, sizeof(config));
	config.pin_base = 10U;
	config.pin_count = 1U;
	config.selected_pins[0] = 10U;
	config.selected_pin_count = 1U;
	config.sample_rate_hz = 1000000U;
	config.post_samples = post_samples;
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;
	return config;
}

static struct linkr_debugger_la_config fast8_plan_config(uint32_t post_samples)
{
	struct linkr_debugger_la_config config;

	memset(&config, 0, sizeof(config));
	config.pin_base = 10U;
	config.pin_count = 8U;
	config.sample_rate_hz = 25000000U;
	config.post_samples = post_samples;
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	config.trigger_pin = 0U;
	return config;
}

static void packed_lane_store(const struct linkr_debugger_la_packed_burst_lane *lane,
	uint32_t *words, uint32_t sample_index, uint32_t lane_value)
{
	uint32_t word_index = sample_index / lane->samples_per_word;
	uint8_t in_word = (uint8_t)(sample_index % lane->samples_per_word);
	uint8_t shift = (uint8_t)((32U - lane->autopush_bits) +
		((uint32_t)in_word * lane->bits_per_sample));
	uint32_t mask = ((1UL << lane->bits_per_sample) - 1UL) << shift;

	words[word_index] &= ~mask;
	words[word_index] |= (lane_value << shift) & mask;
}

static void packed_ring_lane_store(const struct linkr_debugger_la_packed_ring_lane *lane,
	uint32_t *words, uint32_t sample_index, uint32_t lane_value)
{
	uint32_t word_index = (sample_index % lane->sample_capacity) / lane->samples_per_word;
	uint8_t in_word = (uint8_t)((sample_index % lane->sample_capacity) % lane->samples_per_word);
	uint8_t shift = (uint8_t)((32U - lane->autopush_bits) +
		((uint32_t)in_word * lane->bits_per_sample));
	uint32_t mask = ((1UL << lane->bits_per_sample) - 1UL) << shift;

	words[word_index] &= ~mask;
	words[word_index] |= (lane_value << shift) & mask;
}

static void assert_packed_ring_trigger_edge(
	const struct linkr_debugger_la_packed_ring_plan *plan,
	const uint32_t * const lane_words[],
	const uint32_t lane_word_counts[],
	uint8_t trigger_pin,
	enum linkr_debugger_la_trigger_type trigger,
	uint64_t first_sequence,
	uint64_t end_sequence,
	uint8_t previous_level,
	uint64_t expected_sequence,
	uint8_t expected_level)
{
	bool edge_found = false;
	uint64_t edge_sequence = 0U;
	uint8_t last_level = 0U;

	assert(linkr_debugger_logic_analyzer_pre_trigger_scan_packed_ring(plan, lane_words,
		lane_word_counts, trigger_pin, trigger, first_sequence, end_sequence,
		previous_level, &edge_sequence, &last_level, &edge_found) == 0);
	assert(edge_found);
	assert(edge_sequence == expected_sequence);
	assert(last_level == expected_level);
}

static void test_packed_ring_pre_trigger_word_scan(void)
{
	struct linkr_debugger_la_config config;
	struct linkr_debugger_la_packed_ring_plan plan;
	const uint32_t *lane_words[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_word_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t *lane_a_words;
	uint32_t *lane_b_words;
	uint64_t wrap_sequence;

	config = single_plan_config(8U);
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	lane_a_words = alloc_lane_words(plan.lanes[0].word_count);
	lane_words[0] = lane_a_words;
	lane_word_counts[0] = plan.lanes[0].word_count;
	packed_ring_lane_store(&plan.lanes[0], lane_a_words, 31U, 0U);
	packed_ring_lane_store(&plan.lanes[0], lane_a_words, 32U, 1U);
	assert_packed_ring_trigger_edge(&plan, lane_words, lane_word_counts, 0U,
		LINKR_DEBUGGER_LA_TRIGGER_RISING, 31U, 34U, 0U, 32U, 1U);
	memset(lane_a_words, 0, (size_t)plan.lanes[0].word_count * sizeof(*lane_a_words));
	packed_ring_lane_store(&plan.lanes[0], lane_a_words, 48U, 1U);
	assert_packed_ring_trigger_edge(&plan, lane_words, lane_word_counts, 0U,
		LINKR_DEBUGGER_LA_TRIGGER_RISING, 32U, 64U, 0U, 48U, 1U);
	memset(lane_a_words, 0, (size_t)plan.lanes[0].word_count * sizeof(*lane_a_words));
	wrap_sequence = plan.lanes[0].sample_capacity;
	packed_ring_lane_store(&plan.lanes[0], lane_a_words, (uint32_t)(wrap_sequence - 1U), 0U);
	packed_ring_lane_store(&plan.lanes[0], lane_a_words, 0U, 1U);
	assert_packed_ring_trigger_edge(&plan, lane_words, lane_word_counts, 0U,
		LINKR_DEBUGGER_LA_TRIGGER_RISING, wrap_sequence - 1U, wrap_sequence + 2U,
		0U, wrap_sequence, 1U);
	free(lane_a_words);

	memset(lane_words, 0, sizeof(lane_words));
	memset(lane_word_counts, 0, sizeof(lane_word_counts));
	config = fast8_plan_config(8U);
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_FALLING;
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	lane_a_words = alloc_lane_words(plan.lanes[0].word_count);
	lane_words[0] = lane_a_words;
	lane_word_counts[0] = plan.lanes[0].word_count;
	packed_ring_lane_store(&plan.lanes[0], lane_a_words, 3U, 1U);
	packed_ring_lane_store(&plan.lanes[0], lane_a_words, 4U, 0U);
	assert_packed_ring_trigger_edge(&plan, lane_words, lane_word_counts, 0U,
		LINKR_DEBUGGER_LA_TRIGGER_FALLING, 3U, 6U, 1U, 4U, 0U);
	free(lane_a_words);

	memset(lane_words, 0, sizeof(lane_words));
	memset(lane_word_counts, 0, sizeof(lane_word_counts));
	config = wide11_exact_config();
	config.sample_rate_hz = 25000000U;
	config.pre_samples = 16U;
	config.post_samples = 16U;
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_EITHER;
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	lane_a_words = alloc_lane_words(plan.lanes[0].word_count);
	lane_b_words = alloc_lane_words(plan.lanes[1].word_count);
	lane_words[0] = lane_a_words;
	lane_words[1] = lane_b_words;
	lane_word_counts[0] = plan.lanes[0].word_count;
	lane_word_counts[1] = plan.lanes[1].word_count;
	packed_ring_lane_store(&plan.lanes[0], lane_a_words, 3U, 0U);
	packed_ring_lane_store(&plan.lanes[0], lane_a_words, 4U, 1U);
	assert_packed_ring_trigger_edge(&plan, lane_words, lane_word_counts, 0U,
		LINKR_DEBUGGER_LA_TRIGGER_EITHER, 3U, 6U, 0U, 4U, 1U);
	packed_ring_lane_store(&plan.lanes[1], lane_b_words, 9U, 0U);
	packed_ring_lane_store(&plan.lanes[1], lane_b_words, 10U, 1U);
	assert_packed_ring_trigger_edge(&plan, lane_words, lane_word_counts, 8U,
		LINKR_DEBUGGER_LA_TRIGGER_EITHER, 9U, 12U, 0U, 10U, 1U);
	memset(lane_b_words, 0, (size_t)plan.lanes[1].word_count * sizeof(*lane_b_words));
	wrap_sequence = plan.lanes[1].sample_capacity;
	packed_ring_lane_store(&plan.lanes[1], lane_b_words, (uint32_t)(wrap_sequence - 1U), 0U);
	packed_ring_lane_store(&plan.lanes[1], lane_b_words, 0U, 1U);
	assert_packed_ring_trigger_edge(&plan, lane_words, lane_word_counts, 8U,
		LINKR_DEBUGGER_LA_TRIGGER_EITHER, wrap_sequence - 1U, wrap_sequence + 2U,
		0U, wrap_sequence, 1U);
	free(lane_a_words);
	free(lane_b_words);
}

static void fill_single_lane_packed_ring_from_raw_samples(
	const struct linkr_debugger_la_config *config,
	const struct linkr_debugger_la_packed_ring_plan *plan,
	uint32_t *lane_words,
	const uint32_t *raw_ring,
	uint32_t ring_samples)
{
	uint32_t base = plan->sample_capacity - ring_samples;

	for (uint32_t i = 0U; i < ring_samples; i++) {
		uint32_t value = linkr_debugger_logic_analyzer_compress_raw_sample(raw_ring[i], config);

		packed_ring_lane_store(&plan->lanes[0], lane_words, base + i, value);
		packed_ring_lane_store(&plan->lanes[0], lane_words, i, value);
	}
}

static void test_packed_burst_plan_sizing_and_continuous_capacity(void)
{
	static const uint32_t depths[] = { 513U, 65535U, 65536U, 99999U, 100000U };
	struct linkr_debugger_la_config config;
	struct linkr_debugger_la_packed_burst_plan plan;

	for (size_t i = 0U; i < sizeof(depths) / sizeof(depths[0]); i++) {
		uint32_t aligned_single = ((depths[i] + 31U) / 32U) * 32U;
		uint32_t aligned_fast8 = ((depths[i] + 3U) / 4U) * 4U;

		config = single_plan_config(depths[i]);
		config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ;
		assert(linkr_debugger_logic_analyzer_packed_burst_plan(&config, &plan) == 0);
		assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED);
		assert(plan.lane_count == 1U);
		assert(plan.bytes_per_sample == 1U);
		assert(plan.emitted_sample_count == depths[i]);
		assert(plan.source_sample_count == aligned_single);
		assert(plan.lanes[0].word_count == aligned_single / 32U);
		assert(plan.lanes[0].byte_count == (aligned_single / 32U) * 4U);

		config = fast8_plan_config(depths[i]);
		config.sample_rate_hz = LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ;
		assert(linkr_debugger_logic_analyzer_packed_burst_plan(&config, &plan) == 0);
		assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED);
		assert(plan.lane_count == 1U);
		assert(plan.bytes_per_sample == 1U);
		assert(plan.emitted_sample_count == depths[i]);
		assert(plan.source_sample_count == aligned_fast8);
		assert(plan.lanes[0].word_count == aligned_fast8 / 4U);
		assert(plan.lanes[0].byte_count == (aligned_fast8 / 4U) * 4U);
	}

	config = wide11_exact_config();
	config.post_samples = 99999U;
	assert(linkr_debugger_logic_analyzer_packed_burst_plan(&config, &plan) == 0);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_WIDE11_SPLIT_PACKED);
	assert(plan.lane_count == 2U);
	assert(plan.bytes_per_sample == 2U);
	assert(plan.emitted_sample_count == 99999U);
	assert(plan.source_sample_count == 100000U);
	assert(plan.total_byte_count == 140000U);

	config = single_plan_config(0U);
	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ;
	assert(linkr_debugger_logic_analyzer_packed_burst_plan(&config, &plan) == 0);
	assert(plan.continuous_until_capacity);
	assert(plan.requested_sample_count == 0U);
	assert(plan.emitted_sample_count == LINKR_DEBUGGER_LA_PACKED_BURST_CONTINUOUS_SAMPLES);
	assert(plan.source_sample_count == LINKR_DEBUGGER_LA_PACKED_BURST_CONTINUOUS_SAMPLES);

	config.post_samples = LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES + 1U;
	assert(linkr_debugger_logic_analyzer_packed_burst_plan(&config, &plan) == -EINVAL);
}

static void test_packed_burst_decode_selected_pin_compaction(void)
{
	struct linkr_debugger_la_config config = fast8_plan_config(32U);
	struct linkr_debugger_la_packed_burst_plan plan;
	uint32_t lane_words[8];
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = { lane_words, NULL };
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = { 0U, 0U };
	uint8_t packed[4];

	memset(lane_words, 0, sizeof(lane_words));
	config.sample_rate_hz = LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ;
	config.pin_count = 8U;
	config.selected_pin_count = 3U;
	config.selected_pins[0] = 10U;
	config.selected_pins[1] = 12U;
	config.selected_pins[2] = 17U;
	assert(linkr_debugger_logic_analyzer_packed_burst_plan(&config, &plan) == 0);
	lane_counts[0] = plan.lanes[0].word_count;
	packed_lane_store(&plan.lanes[0], lane_words, 0U, BIT(0) | BIT(7));
	packed_lane_store(&plan.lanes[0], lane_words, 1U, BIT(2));
	packed_lane_store(&plan.lanes[0], lane_words, 2U, BIT(0) | BIT(2) | BIT(7));
	packed_lane_store(&plan.lanes[0], lane_words, 3U, 0U);

	memset(packed, 0xa5, sizeof(packed));
	assert(linkr_debugger_logic_analyzer_decode_packed_burst_span(&plan, lane_ptrs,
		lane_counts, 0U, packed, sizeof(packed), 4U) == 0);
	assert(packed[0] == 0x05U);
	assert(packed[1] == 0x02U);
	assert(packed[2] == 0x07U);
	assert(packed[3] == 0x00U);
}

static void test_packed_ring_plan_sizing_and_chunk_limits(void)
{
	struct linkr_debugger_la_config config;
	struct linkr_debugger_la_packed_ring_plan plan;

	config = single_plan_config(0U);
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED);
	assert(plan.lane_count == 1U);
	assert(plan.chunk_samples == 2048U);
	assert(plan.sample_capacity == 262144U);
	assert(plan.usable_sample_capacity == 260096U);
	assert(plan.lanes[0].word_count == 8192U);

	config = fast8_plan_config(0U);
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED);
	assert(plan.chunk_samples == 2048U);
	assert(plan.sample_capacity == 32768U);
	assert(plan.usable_sample_capacity == 30720U);

	config = wide11_exact_config();
	config.post_samples = 0U;
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_WIDE11_SPLIT_PACKED);
	assert(plan.lane_count == 2U);
	assert(plan.chunk_samples == 1024U);
	assert(plan.sample_capacity == 16384U);
	assert(plan.usable_sample_capacity == 14336U);
	assert(plan.lanes[0].word_count == 4096U);
	assert(plan.lanes[1].word_count == 2048U);
	assert(plan.lanes[1].sample_capacity == 20480U);
}

static void test_packed_ring_decode_wrap_and_sparse_selection(void)
{
	struct linkr_debugger_la_config config = fast8_plan_config(0U);
	struct linkr_debugger_la_packed_ring_plan plan;
	uint32_t *lane_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint8_t packed[4];

	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;
	config.pin_count = 8U;
	config.selected_pin_count = 3U;
	config.selected_pins[0] = 10U;
	config.selected_pins[1] = 12U;
	config.selected_pins[2] = 17U;
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	lane_words = alloc_lane_words(plan.lanes[0].word_count);
	packed_ring_lane_store(&plan.lanes[0], lane_words, plan.sample_capacity - 2U,
		BIT(0) | BIT(7));
	packed_ring_lane_store(&plan.lanes[0], lane_words, plan.sample_capacity - 1U,
		BIT(2));
	packed_ring_lane_store(&plan.lanes[0], lane_words, 0U,
		BIT(0) | BIT(2) | BIT(7));
	packed_ring_lane_store(&plan.lanes[0], lane_words, 1U, 0U);
	lane_ptrs[0] = lane_words;
	lane_counts[0] = plan.lanes[0].word_count;
	memset(packed, 0xa5, sizeof(packed));
	assert(linkr_debugger_logic_analyzer_decode_packed_ring_span(&plan, lane_ptrs,
		lane_counts, plan.sample_capacity - 2U, packed, sizeof(packed), 4U) == 0);
	assert(packed[0] == 0x05U);
	assert(packed[1] == 0x02U);
	assert(packed[2] == 0x07U);
	assert(packed[3] == 0x00U);
	free(lane_words);
}

static void test_packed_ring_decode_keeps_wide11_sequence_above_u32(void)
{
	struct linkr_debugger_la_config config = wide11_exact_config();
	struct linkr_debugger_la_packed_ring_plan plan;
	uint32_t *lane_a_words;
	uint32_t *lane_b_words;
	const uint32_t *lane_ptrs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t lane_counts[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	const uint8_t lane_a_values[] = {0x12U, 0x34U, 0x56U, 0x78U};
	const uint8_t lane_b_values[] = {0x01U, 0x02U, 0x03U, 0x04U};
	const uint64_t first_sequence = (UINT64_C(1) << 32U) + 16382U;
	uint8_t packed[8];

	config.post_samples = 0U;
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	lane_a_words = alloc_lane_words(plan.lanes[0].word_count);
	lane_b_words = alloc_lane_words(plan.lanes[1].word_count);
	for (uint32_t index = 0U; index < 4U; index++) {
		uint64_t sequence = first_sequence + index;

		packed_ring_lane_store(&plan.lanes[0], lane_a_words,
			(uint32_t)(sequence % plan.lanes[0].sample_capacity), lane_a_values[index]);
		packed_ring_lane_store(&plan.lanes[1], lane_b_words,
			(uint32_t)(sequence % plan.lanes[1].sample_capacity), lane_b_values[index]);
	}
	lane_ptrs[0] = lane_a_words;
	lane_ptrs[1] = lane_b_words;
	lane_counts[0] = plan.lanes[0].word_count;
	lane_counts[1] = plan.lanes[1].word_count;
	assert(linkr_debugger_logic_analyzer_decode_packed_ring_span(&plan, lane_ptrs,
		lane_counts, first_sequence, packed, sizeof(packed), 4U) == 0);
	for (uint32_t index = 0U; index < 4U; index++) {
		assert(packed[index * 2U] == lane_a_values[index]);
		assert(packed[index * 2U + 1U] == lane_b_values[index]);
	}
	free(lane_b_words);
	free(lane_a_words);
}

static void test_packed_ring_observe_dual_lane_min_seq_and_skew(void)
{
	struct linkr_debugger_la_config config = wide11_exact_config();
	struct linkr_debugger_la_packed_ring_plan plan;
	struct linkr_debugger_la_ring_progress progress;
	uint32_t lane_last_hw_indices[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint64_t lane_writer_seqs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t hw_indices[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES] = {0};
	uint32_t produced = 0U;
	uint32_t skew = 0U;

	config.post_samples = 0U;
	assert(linkr_debugger_logic_analyzer_packed_ring_plan(&config, &plan) == 0);
	memset(&progress, 0, sizeof(progress));
	progress.generation = 42U;
	assert(linkr_debugger_logic_analyzer_packed_ring_observe(&progress,
		lane_last_hw_indices, lane_writer_seqs, hw_indices, 0U, 1000000U, 0U,
		&plan, false, &produced, &skew) == LINKR_DEBUGGER_LA_RING_POLL_OK);
	assert(produced == 0U);

	hw_indices[0] = 4U;
	hw_indices[1] = 1U;
	assert(linkr_debugger_logic_analyzer_packed_ring_observe(&progress,
		lane_last_hw_indices, lane_writer_seqs, hw_indices, 100U, 1000000U, 0U,
		&plan, false, &produced, &skew) == LINKR_DEBUGGER_LA_RING_POLL_OK);
	assert(produced == 10U);
	assert(progress.writer_seq == 10U);
	assert(skew == 6U);

	hw_indices[0] = 9U;
	hw_indices[1] = 3U;
	assert(linkr_debugger_logic_analyzer_packed_ring_observe(&progress,
		lane_last_hw_indices, lane_writer_seqs, hw_indices, 200U, 1000000U, 10U,
		&plan, false, &produced, &skew) == LINKR_DEBUGGER_LA_RING_POLL_OK);
	assert(progress.reader_seq == 10U);
	assert(produced == 20U);
	assert(progress.writer_seq == 30U);
	assert(skew <= 20U);

	hw_indices[0] = 27U;
	hw_indices[1] = 4U;
	assert(linkr_debugger_logic_analyzer_packed_ring_observe(&progress,
		lane_last_hw_indices, lane_writer_seqs, hw_indices, 300U, 1000000U, 0U,
		&plan, false, &produced, &skew) == LINKR_DEBUGGER_LA_RING_POLL_OK);
	assert(produced == 10U);
	assert(progress.writer_seq == 40U);
	assert(skew == 68U);

	hw_indices[0] = 31U;
	assert(linkr_debugger_logic_analyzer_packed_ring_observe(&progress,
		lane_last_hw_indices, lane_writer_seqs, hw_indices, 400U, 1000000U, 0U,
		&plan, false, &produced, &skew) == LINKR_DEBUGGER_LA_RING_POLL_OK);
	assert(produced == 0U);
	assert(progress.writer_seq == 40U);
	assert(skew == 84U);

	hw_indices[0] = 36U;
	assert(linkr_debugger_logic_analyzer_packed_ring_observe(&progress,
		lane_last_hw_indices, lane_writer_seqs, hw_indices, 500U, 1000000U, 0U,
		&plan, false, &produced, &skew) == LINKR_DEBUGGER_LA_RING_POLL_OK);
	assert(produced == 0U);
	assert(progress.writer_seq == 40U);
	assert(skew == 104U);

	assert(linkr_debugger_logic_analyzer_packed_ring_observe(&progress,
		lane_last_hw_indices, lane_writer_seqs, hw_indices, 600U, 1000000U, 0U,
		&plan, true, &produced, &skew) == LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN);
}

static void test_session_contract_trigger_gate_and_stop_policy(void)
{
	struct linkr_debugger_la_config config = single_plan_config(16U);
	struct linkr_debugger_la_session_contract contract;

	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;
	assert(linkr_debugger_logic_analyzer_session_contract(&config, &contract) == 0);
	assert(contract.trigger_gate == LINKR_DEBUGGER_LA_TRIGGER_NONE);
	assert(contract.stop_policy == LINKR_DEBUGGER_LA_STOP_POLICY_BOUNDED);
	assert(contract.post_samples == 16U);

	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	assert(linkr_debugger_logic_analyzer_session_contract(&config, &contract) == 0);
	assert(contract.trigger_gate == LINKR_DEBUGGER_LA_TRIGGER_RISING);
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_FALLING;
	assert(linkr_debugger_logic_analyzer_session_contract(&config, &contract) == 0);
	assert(contract.trigger_gate == LINKR_DEBUGGER_LA_TRIGGER_FALLING);
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_EITHER;
	assert(linkr_debugger_logic_analyzer_session_contract(&config, &contract) == 0);
	assert(contract.trigger_gate == LINKR_DEBUGGER_LA_TRIGGER_EITHER);

	config.post_samples = 0U;
	assert(linkr_debugger_logic_analyzer_session_contract(&config, &contract) == 0);
	assert(contract.stop_policy == LINKR_DEBUGGER_LA_STOP_POLICY_CONTINUOUS);
	assert(strcmp(linkr_debugger_logic_analyzer_stop_reason_name(
		LINKR_DEBUGGER_LA_STOP_REASON_HOST_STOP), "host_stop") == 0);
	assert(strcmp(linkr_debugger_logic_analyzer_stop_reason_name(
		LINKR_DEBUGGER_LA_STOP_REASON_DMA_ERROR), "dma_error") == 0);
	assert(strcmp(linkr_debugger_logic_analyzer_stop_reason_name(
		LINKR_DEBUGGER_LA_STOP_REASON_UNSUPPORTED), "unsupported") == 0);
}

static void test_hardware_plan_selector_normalizes_post512_and_post513(void)
{
	struct linkr_debugger_la_config config = single_plan_config(512U);
	struct linkr_debugger_la_hardware_plan plan512;
	struct linkr_debugger_la_hardware_plan plan513;

	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false,
		&plan512) == 0);
	assert(plan512.supported);
	assert(plan512.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED);
	assert(plan512.pipeline_family == LINKR_DEBUGGER_LA_PIPELINE_FAMILY_COMMON_PACKED);
	assert(plan512.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);
	assert(plan512.bytes_per_sample == 1U);

	config.post_samples = 513U;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false,
		&plan513) == 0);
	assert(plan513.supported);
	assert(plan513.kind == plan512.kind);
	assert(plan513.pipeline_family == plan512.pipeline_family);
	assert(plan513.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);

	config.post_samples = 0U;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false,
		&plan513) == 0);
	assert(plan513.supported);
	assert(plan513.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED);
	assert(plan513.pipeline_family == LINKR_DEBUGGER_LA_PIPELINE_FAMILY_COMMON_PACKED);
	assert(plan513.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_RING_STREAM);

	config = single_plan_config(512U);
	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false,
		&plan512) == 0);
	assert(plan512.supported);
	assert(plan512.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED);
	assert(plan512.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);
	config.post_samples = 513U;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false,
		&plan513) == 0);
	assert(plan513.supported);
	assert(plan513.kind == plan512.kind);
	assert(plan513.pipeline_family == plan512.pipeline_family);
	assert(plan513.legacy_adapter == plan512.legacy_adapter);
}

static void test_hardware_plan_selector_capability_matrix(void)
{
	struct linkr_debugger_la_config config = fast8_plan_config(0U);
	struct linkr_debugger_la_hardware_plan plan;

	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false, &plan) == 0);
	assert(plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED);
	assert(plan.pipeline_family == LINKR_DEBUGGER_LA_PIPELINE_FAMILY_COMMON_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_RING_STREAM);
	assert(strcmp(linkr_debugger_logic_analyzer_hardware_plan_name(plan.kind),
		"fast8_packed") == 0);

	config = wide11_exact_config();
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &plan) == 0);
	assert(plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_WIDE11_SPLIT_PACKED);
	assert(plan.pipeline_family == LINKR_DEBUGGER_LA_PIPELINE_FAMILY_COMMON_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);
	assert(plan.bytes_per_sample == LINKR_DEBUGGER_LA_WIDE11_BURST_PACKED_SAMPLE_BYTES);
	assert(strcmp(plan.reason, "supported") == 0);

	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false, &plan) == 0);
	assert(!plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_WIDE11_SPLIT_PACKED);
	assert(plan.pipeline_family == LINKR_DEBUGGER_LA_PIPELINE_FAMILY_COMMON_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_NONE);
	assert(strcmp(plan.reason, "config_v2_required") == 0);

	config = wide11_exact_config();
	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &plan) == 0);
	assert(!plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_UNSUPPORTED);
	assert(plan.unsupported_reason == LINKR_DEBUGGER_LA_STOP_REASON_UNSUPPORTED);
	assert(strcmp(plan.reason, "unsupported_pin_plan") == 0);

	config = single_plan_config((uint32_t)UINT16_MAX + 1U);
	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false, &plan) == 0);
	assert(!plan.supported);
	assert(strcmp(plan.reason, "config_v2_required") == 0);
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &plan) == 0);
	assert(plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);
	assert(strcmp(plan.reason, "supported") == 0);

	config = single_plan_config((uint32_t)UINT16_MAX + 1U);
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false, &plan) == 0);
	assert(!plan.supported);
	assert(strcmp(plan.reason, "config_v2_required") == 0);
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &plan) == 0);
	assert(plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);
	assert(strcmp(plan.reason, "supported") == 0);
}

static void test_packed_burst_nominal_rate_routing_regressions(void)
{
	struct linkr_debugger_la_config config;
	struct linkr_debugger_la_hardware_plan plan;

	config = single_plan_config(LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES);
	config.sample_rate_hz = LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &plan) == 0);
	assert(plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);

	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &plan) == 0);
	assert(plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);

	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ - 1U;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &plan) == 0);
	assert(plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);
	assert(strcmp(plan.reason, "supported") == 0);

	config = fast8_plan_config(LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES);
	config.sample_rate_hz = LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ;
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &plan) == 0);
	assert(plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);

	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &plan) == 0);
	assert(plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);

	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ - 1U;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &plan) == 0);
	assert(plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);
	assert(strcmp(plan.reason, "supported") == 0);

	config = wide11_exact_config();
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &plan) == 0);
	assert(plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_WIDE11_SPLIT_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);
	assert(strcmp(plan.reason, "supported") == 0);

	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &plan) == 0);
	assert(!plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_UNSUPPORTED);
	assert(plan.unsupported_reason == LINKR_DEBUGGER_LA_STOP_REASON_UNSUPPORTED);
	assert(strcmp(plan.reason, "unsupported_pin_plan") == 0);

	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ - 1U;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &plan) == 0);
	assert(!plan.supported);
	assert(plan.legacy_adapter != LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);
	assert(strcmp(plan.reason, "unsupported_pin_plan") == 0);
}

static void test_packed_burst_requested_rate_cap_allows_quantized_125mhz(void)
{
	struct linkr_debugger_la_config config;
	struct linkr_debugger_la_hardware_plan hardware_plan;
	struct linkr_debugger_la_packed_burst_plan burst_plan;

	config = single_plan_config(513U);
	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ;
	assert(linkr_debugger_logic_analyzer_packed_burst_plan(&config, &burst_plan) == 0);
	assert(burst_plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED);
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &hardware_plan) == 0);
	assert(hardware_plan.supported);
	assert(hardware_plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED);

	config = fast8_plan_config(513U);
	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ;
	assert(linkr_debugger_logic_analyzer_packed_burst_plan(&config, &burst_plan) == 0);
	assert(burst_plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED);
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, true, &hardware_plan) == 0);
	assert(hardware_plan.supported);
	assert(hardware_plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED);
}

static void test_packed_physical_plan_rate_policy_prefers_requested_limit(void)
{
	assert(linkr_debugger_logic_analyzer_packed_rate_limit_supported(
		125000000U, 125081000U, 125000000U));
	assert(!linkr_debugger_logic_analyzer_packed_rate_limit_supported(
		125000001U, 125081000U, 125000000U));
	assert(!linkr_debugger_logic_analyzer_packed_rate_limit_supported(
		125000000U, 0U, 125000000U));
	assert(!linkr_debugger_logic_analyzer_packed_rate_limit_supported(
		125000000U, 100000000U, 100000000U));
}

static void test_hardware_plan_selector_rejects_removed_raw32_shapes(void)
{
	struct linkr_debugger_la_config config;
	struct linkr_debugger_la_hardware_plan plan;

	config = wide11_exact_config();
	config.post_samples = 0U;
	config.sample_rate_hz = 25000000U;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false, &plan) == 0);
	assert(plan.supported);
	assert(plan.kind == LINKR_DEBUGGER_LA_HARDWARE_PLAN_WIDE11_SPLIT_PACKED);
	assert(plan.pipeline_family == LINKR_DEBUGGER_LA_PIPELINE_FAMILY_COMMON_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_RING_STREAM);
	assert(strcmp(plan.reason, "supported") == 0);

	config = single_plan_config(65535U);
	config.pin_base = 7U;
	config.pin_count = 3U;
	config.selected_pins[0] = 7U;
	config.selected_pins[1] = 10U;
	config.selected_pins[2] = 20U;
	config.selected_pin_count = 3U;
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_EITHER;
	config.trigger_pin = 2U;
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&config, false, &plan) == 0);
	assert(!plan.supported);
	assert(strcmp(plan.reason, "unsupported_pin_plan") == 0);
}

static void test_packed_burst_prepare_rejects_ordinary_stream_routing(void)
{
	struct sink_test_context ctx;
	struct linkr_debugger_la_stream_sink sink;
	struct linkr_debugger_la_config config = single_plan_config(0U);
	struct linkr_debugger_la_start_prepare prepare;

	memset(&ctx, 0, sizeof(ctx));
	memset(&prepare, 0, sizeof(prepare));
	ctx.capacity = sizeof(ctx.storage);
	sink = sink_for_context(&ctx);
	config.sample_rate_hz = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ;
	assert(linkr_debugger_logic_analyzer_prepare_stream_start_sink(&config, false,
		&sink, &prepare) == -EINVAL);

	memset(&prepare, 0, sizeof(prepare));
	assert(linkr_debugger_logic_analyzer_prepare_wide11_burst_start_sink(&config,
		NULL, NULL, &prepare) == 0);
	assert(prepare.hardware_prepared);
	assert(prepare.plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);
	assert(linkr_debugger_logic_analyzer_start_prepare_cancel(&prepare) == 0);

	config = single_plan_config(513U);
	config.sample_rate_hz = 1000000U;
	memset(&prepare, 0, sizeof(prepare));
	assert(linkr_debugger_logic_analyzer_prepare_stream_start_sink(&config, false,
		&sink, &prepare) == -EINVAL);
}

static void test_wide11_burst_start_prepare_ordering_and_cleanup(void)
{
	struct linkr_debugger_la_config config = wide11_exact_config();
	struct linkr_debugger_la_start_prepare prepare;
	struct linkr_debugger_la_start_prepare second;
	uint32_t first_generation;

	memset(&prepare, 0, sizeof(prepare));
	memset(&second, 0, sizeof(second));
	assert(linkr_debugger_logic_analyzer_prepare_wide11_burst_start(
		&config, &prepare) == 0);
	assert(prepare.generation != 0U);
	assert(prepare.state == LINKR_DEBUGGER_LA_START_PREPARE_READY);
	assert(prepare.requested_sample_rate_hz ==
		LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ);
	assert(prepare.actual_sample_rate_hz ==
		LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ);
	assert(prepare.sample_period_ps == 10000ULL);
	assert(strcmp(prepare.backend, linkr_debugger_logic_analyzer_backend()) == 0);
	first_generation = prepare.generation;

	assert(linkr_debugger_logic_analyzer_prepare_wide11_burst_start(
		&config, &second) == -EBUSY);
	assert(linkr_debugger_logic_analyzer_start_prepare_go(&prepare) == -EPROTO);
	assert(linkr_debugger_logic_analyzer_start_prepare_mark_armed_event_sent(
		&prepare) == -EPROTO);
	assert(prepare.state == LINKR_DEBUGGER_LA_START_PREPARE_READY);

	assert(linkr_debugger_logic_analyzer_start_prepare_mark_response_sent(
		&prepare) == 0);
	assert(prepare.state == LINKR_DEBUGGER_LA_START_PREPARE_RESPONSE_SENT);
	assert(linkr_debugger_logic_analyzer_start_prepare_mark_response_sent(
		&prepare) == -EPROTO);
	assert(linkr_debugger_logic_analyzer_start_prepare_go(&prepare) == -ENOTSUP);
	assert(prepare.state == LINKR_DEBUGGER_LA_START_PREPARE_GO_FAILED);
	assert(linkr_debugger_logic_analyzer_start_prepare_go(&prepare) == -EALREADY);

	memset(&second, 0, sizeof(second));
	assert(linkr_debugger_logic_analyzer_prepare_wide11_burst_start(
		&config, &second) == 0);
	assert(second.generation != first_generation);
	assert(linkr_debugger_logic_analyzer_start_prepare_cancel(&second) == 0);
	assert(second.state == LINKR_DEBUGGER_LA_START_PREPARE_CANCELLED);
	assert(linkr_debugger_logic_analyzer_start_prepare_cancel(&second) == 0);
}

static void test_wide11_burst_triggered_start_prepare_requires_armed_event_mark(void)
{
	struct linkr_debugger_la_config config = wide11_exact_config();
	struct linkr_debugger_la_start_prepare prepare;

	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	config.trigger_pin = 11U;
	memset(&prepare, 0, sizeof(prepare));
	assert(linkr_debugger_logic_analyzer_prepare_wide11_burst_start(
		&config, &prepare) == 0);
	assert(linkr_debugger_logic_analyzer_start_prepare_mark_response_sent(
		&prepare) == 0);
	assert(linkr_debugger_logic_analyzer_start_prepare_go(&prepare) == -EPROTO);
	assert(linkr_debugger_logic_analyzer_start_prepare_mark_armed_event_sent(
		&prepare) == 0);
	assert(prepare.state == LINKR_DEBUGGER_LA_START_PREPARE_ARMED_EVENT_SENT);
	assert(linkr_debugger_logic_analyzer_start_prepare_go(&prepare) == -ENOTSUP);
	assert(prepare.state == LINKR_DEBUGGER_LA_START_PREPARE_GO_FAILED);
}

static void test_wide11_burst_start_prepare_rejects_invalid_and_stale_tokens(void)
{
	struct linkr_debugger_la_config config = wide11_exact_config();
	struct linkr_debugger_la_start_prepare prepare;
	struct linkr_debugger_la_start_prepare stale;

	memset(&prepare, 0, sizeof(prepare));
	assert(linkr_debugger_logic_analyzer_prepare_wide11_burst_start(
		NULL, &prepare) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_prepare_wide11_burst_start(
		&config, NULL) == -EINVAL);
	config.post_samples = LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES + 1U;
	assert(linkr_debugger_logic_analyzer_prepare_wide11_burst_start(
		&config, &prepare) == -EINVAL);

	config = wide11_exact_config();
	assert(linkr_debugger_logic_analyzer_prepare_wide11_burst_start(
		&config, &prepare) == 0);
	stale = prepare;
	stale.generation++;
	assert(linkr_debugger_logic_analyzer_start_prepare_mark_response_sent(
		&stale) == -ESTALE);
	stale = prepare;
	stale.state = LINKR_DEBUGGER_LA_START_PREPARE_RESPONSE_SENT;
	assert(linkr_debugger_logic_analyzer_start_prepare_mark_armed_event_sent(
		&stale) == -ESTALE);
	assert(linkr_debugger_logic_analyzer_start_prepare_cancel(&prepare) == 0);

	assert(linkr_debugger_logic_analyzer_prepare_wide11_burst_start(
		&config, &prepare) == 0);
	assert(linkr_debugger_logic_analyzer_cancel() == 0);
	assert(linkr_debugger_logic_analyzer_start_prepare_mark_response_sent(
		&prepare) == -EINVAL);
	memset(&prepare, 0, sizeof(prepare));
	assert(linkr_debugger_logic_analyzer_start_prepare_cancel(&prepare) == -EINVAL);
}

static void test_stream_start_prepare_sets_hardware_barrier_before_response(void)
{
	struct sink_test_context ctx;
	struct linkr_debugger_la_stream_sink sink;
	struct linkr_debugger_la_config config = fast8_plan_config(0U);
	struct linkr_debugger_la_start_prepare prepare;
	struct linkr_debugger_la_start_prepare second;

	memset(&ctx, 0, sizeof(ctx));
	memset(&prepare, 0, sizeof(prepare));
	memset(&second, 0, sizeof(second));
	ctx.capacity = sizeof(ctx.storage);
	sink = sink_for_context(&ctx);
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;
	config.trigger_pin = 0U;
	assert(linkr_debugger_logic_analyzer_prepare_stream_start_sink(&config, false,
		&sink, &prepare) == 0);
	assert(prepare.state == LINKR_DEBUGGER_LA_START_PREPARE_READY);
	assert(prepare.sink_bound);
	assert(prepare.hardware_prepared);
	assert(prepare.plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_RING_STREAM);
	assert(linkr_debugger_logic_analyzer_prepare_stream_start_sink(&config, false,
		&sink, &second) == -EBUSY);
	assert(linkr_debugger_logic_analyzer_start_prepare_go(&prepare) == -EPROTO);
	assert(prepare.state == LINKR_DEBUGGER_LA_START_PREPARE_READY);
	assert(linkr_debugger_logic_analyzer_start_prepare_mark_response_sent(
		&prepare) == 0);
	assert(linkr_debugger_logic_analyzer_start_prepare_go(&prepare) == 0);

	memset(&second, 0, sizeof(second));
	assert(linkr_debugger_logic_analyzer_prepare_stream_start_sink(&config, false,
		&sink, &second) == 0);
	assert(second.hardware_prepared);
	assert(linkr_debugger_logic_analyzer_start_prepare_cancel(&second) == 0);
	assert(second.state == LINKR_DEBUGGER_LA_START_PREPARE_CANCELLED);
	memset(&second, 0, sizeof(second));
	assert(linkr_debugger_logic_analyzer_prepare_stream_start_sink(&config, false,
		&sink, &second) == 0);
	assert(linkr_debugger_logic_analyzer_start_prepare_cancel(&second) == 0);
}

static void test_stream_start_prepare_triggered_requires_armed_marker(void)
{
	struct sink_test_context ctx;
	struct linkr_debugger_la_stream_sink sink;
	struct linkr_debugger_la_config config = fast8_plan_config(0U);
	struct linkr_debugger_la_start_prepare prepare;

	memset(&ctx, 0, sizeof(ctx));
	memset(&prepare, 0, sizeof(prepare));
	ctx.capacity = sizeof(ctx.storage);
	sink = sink_for_context(&ctx);
	assert(linkr_debugger_logic_analyzer_prepare_stream_start_sink(&config, false,
		&sink, &prepare) == 0);
	assert(prepare.hardware_prepared);
	assert(linkr_debugger_logic_analyzer_start_prepare_mark_response_sent(
		&prepare) == 0);
	assert(linkr_debugger_logic_analyzer_start_prepare_go(&prepare) == -EPROTO);
	assert(linkr_debugger_logic_analyzer_start_prepare_mark_armed_event_sent(
		&prepare) == 0);
	assert(linkr_debugger_logic_analyzer_start_prepare_go(&prepare) == 0);
}

static void test_effective_trigger_helper(void)
{
	assert(linkr_debugger_logic_analyzer_effective_trigger(
		LINKR_DEBUGGER_LA_TRIGGER_NONE, false) == LINKR_DEBUGGER_LA_TRIGGER_NONE);
	assert(linkr_debugger_logic_analyzer_effective_trigger(
		LINKR_DEBUGGER_LA_TRIGGER_RISING, true) == LINKR_DEBUGGER_LA_TRIGGER_RISING);
	assert(linkr_debugger_logic_analyzer_effective_trigger(
		LINKR_DEBUGGER_LA_TRIGGER_FALLING, false) == LINKR_DEBUGGER_LA_TRIGGER_FALLING);
	assert(linkr_debugger_logic_analyzer_effective_trigger(
		LINKR_DEBUGGER_LA_TRIGGER_EITHER, false) == LINKR_DEBUGGER_LA_TRIGGER_RISING);
	assert(linkr_debugger_logic_analyzer_effective_trigger(
		LINKR_DEBUGGER_LA_TRIGGER_EITHER, true) == LINKR_DEBUGGER_LA_TRIGGER_FALLING);
}

static void test_capture_program_shapes_and_errors(void)
{
	struct linkr_debugger_la_pio_program_layout layout;
	uint16_t instructions[2] = {0xaaaaU, 0xbbbbU};

	memset(&layout, 0, sizeof(layout));
	assert(linkr_debugger_logic_analyzer_build_capture_program(
		false, 31U, instructions, 1U, &layout) == 0);
	assert(instructions[0] == 0x4000U);
	assert(instructions[1] == 0xbbbbU);
	assert(layout.length == 1U);
	assert(layout.wrap_target == 0U);
	assert(layout.wrap == 0U);

	memset(&layout, 0xff, sizeof(layout));
	instructions[0] = 0xaaaaU;
	instructions[1] = 0xbbbbU;
	assert(linkr_debugger_logic_analyzer_build_capture_program(
		true, 30U, instructions, 2U, &layout) == 0);
	assert(instructions[0] == 0x20c0U);
	assert(instructions[1] == 0x4000U);
	assert(layout.length == 2U);
	assert(layout.wrap_target == 1U);
	assert(layout.wrap == 1U);

	assert(linkr_debugger_logic_analyzer_build_capture_program(
		false, 32U, instructions, 1U, &layout) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_build_capture_program(
		true, 31U, instructions, 2U, &layout) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_build_capture_program(
		false, 0U, instructions, 0U, &layout) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_build_capture_program(
		true, 0U, instructions, 1U, &layout) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_build_capture_program(
		false, 0U, NULL, 1U, &layout) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_build_capture_program(
		false, 0U, instructions, 1U, NULL) == -EINVAL);
}

static void test_wide11_burst_program_builders(void)
{
	struct linkr_debugger_la_pio_program_layout layout;
	uint16_t instructions[2] = {0xaaaaU, 0xbbbbU};

	memset(&layout, 0xff, sizeof(layout));
	assert(linkr_debugger_logic_analyzer_build_wide11_sm_a_program(
		31U, instructions, 1U, &layout) == 0);
	assert(instructions[0] == 0x4008U);
	assert(instructions[1] == 0xbbbbU);
	assert(layout.length == 1U);
	assert(layout.wrap_target == 0U);
	assert(layout.wrap == 0U);

	memset(&layout, 0xff, sizeof(layout));
	instructions[0] = 0xaaaaU;
	instructions[1] = 0xbbbbU;
	assert(linkr_debugger_logic_analyzer_build_wide11_sm_b_program(
		31U, instructions, 1U, &layout) == 0);
	assert(instructions[0] == 0x4003U);
	assert(instructions[1] == 0xbbbbU);
	assert(layout.length == 1U);
	assert(layout.wrap_target == 0U);
	assert(layout.wrap == 0U);

	assert(linkr_debugger_logic_analyzer_build_wide11_sm_a_program(
		32U, instructions, 1U, &layout) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_build_wide11_sm_b_program(
		32U, instructions, 1U, &layout) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_build_wide11_sm_a_program(
		0U, instructions, 0U, &layout) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_build_wide11_sm_b_program(
		0U, instructions, 0U, &layout) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_build_wide11_sm_a_program(
		0U, NULL, 1U, &layout) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_build_wide11_sm_b_program(
		0U, instructions, 1U, NULL) == -EINVAL);
}

static void test_wide11_burst_decode_model_patterns(void)
{
	uint32_t sm_a_words[16];
	uint32_t sm_b_words[4];
	uint16_t decoded[42];
	uint16_t expected[40];

	memset(sm_a_words, 0, sizeof(sm_a_words));
	memset(sm_b_words, 0, sizeof(sm_b_words));
	memset(expected, 0, sizeof(expected));
	for (size_t i = 0U; i < sizeof(decoded) / sizeof(decoded[0]); i++) {
		decoded[i] = 0xa55aU;
	}

	expected[0] = 0x0000U;
	expected[1] = LINKR_DEBUGGER_LA_WIDE11_BURST_SAMPLE_MASK;
	for (uint8_t bit = 0U; bit < 11U; bit++) {
		expected[2U + bit] = (uint16_t)BIT(bit);
	}
	expected[14] = 0x0555U;
	expected[15] = 0x02aaU;
	expected[16] = 0x0163U;
	expected[17] = 0x069cU;
	expected[39] = 0x0001U;

	for (uint32_t i = 0U; i < 40U; i++) {
		wide11_model_store(sm_a_words, sm_b_words, i, expected[i]);
	}

	assert(linkr_debugger_logic_analyzer_decode_wide11_burst(
		sm_a_words, 10U, sm_b_words, 4U, &decoded[1], 40U) == 0);
	assert(decoded[0] == 0xa55aU);
	assert(decoded[41] == 0xa55aU);
	for (uint32_t i = 0U; i < 40U; i++) {
		assert(decoded[1U + i] == expected[i]);
	}
}

static void test_wide11_burst_decode_target_boundaries(void)
{
	struct linkr_debugger_la_wide11_burst_plan plan;
	uint32_t *sm_a_words;
	uint32_t *sm_b_words;
	uint16_t *decoded;

	assert(linkr_debugger_logic_analyzer_wide11_burst_plan(
		LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES, &plan) == 0);
	sm_a_words = calloc(plan.lane_a_word_count, sizeof(sm_a_words[0]));
	sm_b_words = calloc(plan.lane_b_word_count, sizeof(sm_b_words[0]));
	decoded = malloc(((size_t)plan.sample_count + 2U) * sizeof(decoded[0]));
	assert(sm_a_words != NULL);
	assert(sm_b_words != NULL);
	assert(decoded != NULL);

	for (uint32_t i = 0U; i < plan.sample_count + 2U; i++) {
		decoded[i] = 0xa55aU;
	}

	wide11_model_store(sm_a_words, sm_b_words, 0U, wide11_target_expected_sample(0U));
	wide11_model_store(sm_a_words, sm_b_words, 1U, wide11_target_expected_sample(1U));
	wide11_model_store(sm_a_words, sm_b_words, 2U, wide11_target_expected_sample(2U));
	wide11_model_store(sm_a_words, sm_b_words, 31U, wide11_target_expected_sample(31U));
	wide11_model_store(sm_a_words, sm_b_words, 32U, wide11_target_expected_sample(32U));
	wide11_model_store(sm_a_words, sm_b_words, 33U, wide11_target_expected_sample(33U));
	wide11_model_store(sm_a_words, sm_b_words, 99998U,
		wide11_target_expected_sample(99998U));
	wide11_model_store(sm_a_words, sm_b_words, 99999U,
		wide11_target_expected_sample(99999U));

	assert(linkr_debugger_logic_analyzer_decode_wide11_burst(
		sm_a_words, plan.lane_a_word_count, sm_b_words, plan.lane_b_word_count,
		&decoded[1], plan.sample_count) == 0);
	assert(decoded[0] == 0xa55aU);
	assert(decoded[plan.sample_count + 1U] == 0xa55aU);
	for (uint32_t i = 0U; i < plan.sample_count; i++) {
		assert(decoded[1U + i] == wide11_target_expected_sample(i));
	}

	free(sm_a_words);
	free(sm_b_words);
	free(decoded);
}

static void test_wide11_burst_decode_rejects_invalid_sizes_without_writes(void)
{
	uint32_t sm_a_words[16];
	uint32_t sm_b_words[4];
	uint16_t decoded[42];

	memset(sm_a_words, 0, sizeof(sm_a_words));
	memset(sm_b_words, 0, sizeof(sm_b_words));
	for (size_t i = 0U; i < sizeof(decoded) / sizeof(decoded[0]); i++) {
		decoded[i] = 0xa55aU;
	}

	assert(linkr_debugger_logic_analyzer_decode_wide11_burst(
		sm_a_words, 10U, sm_b_words, 4U, &decoded[1], 0U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_decode_wide11_burst(
		sm_a_words, 10U, sm_b_words, 4U, &decoded[1], 31U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_decode_wide11_burst(
		sm_a_words, 9U, sm_b_words, 4U, &decoded[1], 40U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_decode_wide11_burst(
		sm_a_words, 10U, sm_b_words, 3U, &decoded[1], 40U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_decode_wide11_burst(
		NULL, 10U, sm_b_words, 4U, &decoded[1], 40U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_decode_wide11_burst(
		sm_a_words, 10U, NULL, 4U, &decoded[1], 40U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_decode_wide11_burst(
		sm_a_words, 10U, sm_b_words, 4U, NULL, 40U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_decode_wide11_burst(
		sm_a_words, 0U, sm_b_words, 0U, &decoded[1], 0x80000000U) == -EINVAL);
	assert_u16_span_filled(decoded, sizeof(decoded) / sizeof(decoded[0]), 0xa55aU);
}

static void test_wide11_burst_decode_span_outputs_packed_le_slices(void)
{
	uint32_t sm_a_words[32];
	uint32_t sm_b_words[8];
	uint8_t packed[12];
	const uint16_t expected[] = { 0x0555U, 0x02aaU, 0x0163U, 0x069cU };

	memset(sm_a_words, 0, sizeof(sm_a_words));
	memset(sm_b_words, 0, sizeof(sm_b_words));
	memset(packed, 0xa5, sizeof(packed));

	for (uint32_t i = 0U; i < 80U; i++) {
		wide11_model_store(sm_a_words, sm_b_words, i, (uint16_t)(i & 0x07ffU));
	}
	for (uint32_t i = 0U; i < sizeof(expected) / sizeof(expected[0]); i++) {
		wide11_model_store(sm_a_words, sm_b_words, 14U + i, expected[i]);
	}

	assert(linkr_debugger_logic_analyzer_decode_wide11_burst_span(
		sm_a_words, 20U, sm_b_words, 8U, 80U, 14U, &packed[2], 8U, 4U) == 0);
	assert(packed[0] == 0xa5U);
	assert(packed[1] == 0xa5U);
	assert(packed[10] == 0xa5U);
	assert(packed[11] == 0xa5U);
	for (uint32_t i = 0U; i < 4U; i++) {
		assert(packed[2U + (i * 2U)] == (uint8_t)(expected[i] & 0xffU));
		assert(packed[3U + (i * 2U)] == (uint8_t)((expected[i] >> 8) & 0xffU));
	}
}

static void test_wide11_burst_decode_span_rejects_bad_ranges_without_writes(void)
{
	uint32_t sm_a_words[16];
	uint32_t sm_b_words[4];
	uint8_t packed[8];

	memset(sm_a_words, 0, sizeof(sm_a_words));
	memset(sm_b_words, 0, sizeof(sm_b_words));
	memset(packed, 0xa5, sizeof(packed));

	assert(linkr_debugger_logic_analyzer_decode_wide11_burst_span(
		sm_a_words, 10U, sm_b_words, 4U, 40U, 0U, &packed[0], 8U, 0U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_decode_wide11_burst_span(
		sm_a_words, 10U, sm_b_words, 4U, 40U, 37U, &packed[0], 8U, 4U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_decode_wide11_burst_span(
		sm_a_words, 10U, sm_b_words, 4U, 40U, 0U, &packed[0], 7U, 4U) == -EMSGSIZE);
	assert(linkr_debugger_logic_analyzer_decode_wide11_burst_span(
		sm_a_words, 9U, sm_b_words, 4U, 40U, 0U, &packed[0], 8U, 4U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_decode_wide11_burst_span(
		NULL, 10U, sm_b_words, 4U, 40U, 0U, &packed[0], 8U, 4U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_decode_wide11_burst_span(
		sm_a_words, 10U, NULL, 4U, 40U, 0U, &packed[0], 8U, 4U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_decode_wide11_burst_span(
		sm_a_words, 10U, sm_b_words, 4U, 40U, 0U, NULL, 8U, 4U) == -EINVAL);
	assert_u16_span_filled((const uint16_t *)packed, sizeof(packed) / sizeof(uint16_t), 0xa5a5U);
}

static void test_wide11_burst_completion_mask_generation_model(void)
{
	bool complete = true;
	uint8_t mask = 0U;

	mask = linkr_debugger_logic_analyzer_wide11_burst_completion_mask_update(
		7U, 6U, mask, 1U, &complete);
	assert(mask == 0U);
	assert(!complete);
	mask = linkr_debugger_logic_analyzer_wide11_burst_completion_mask_update(
		7U, 7U, mask, 1U, &complete);
	assert(mask == 1U);
	assert(!complete);
	mask = linkr_debugger_logic_analyzer_wide11_burst_completion_mask_update(
		7U, 8U, mask, 2U, &complete);
	assert(mask == 1U);
	assert(!complete);
	mask = linkr_debugger_logic_analyzer_wide11_burst_completion_mask_update(
		7U, 7U, mask, 2U, &complete);
	assert(mask == 3U);
	assert(complete);
	mask = linkr_debugger_logic_analyzer_wide11_burst_completion_mask_update(
		7U, 7U, mask, 4U, &complete);
	assert(mask == 3U);
	assert(!complete);
}

static void test_wide11_burst_100000_chunk_accounting(void)
{
	uint32_t emitted = 0U;
	uint32_t chunks = 0U;
	uint32_t sequence = 0U;

	while (emitted < LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES) {
		uint32_t chunk = LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES - emitted;

		if (chunk > 1024U) {
			chunk = 1024U;
		}
		assert(chunk > 0U);
		assert(chunk <= 1024U);
		assert(sequence == chunks);
		emitted += chunk;
		chunks++;
		sequence++;
	}
	assert(emitted == LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES);
	assert(chunks == 98U);
}

static void test_wide11_burst_triggered_configured_pin_count_stays_11(void)
{
	struct linkr_debugger_la_config config = wide11_exact_config();

	assert(linkr_debugger_logic_analyzer_wide11_burst_configured_pin_count_model(
		&config) == 11U);
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	config.trigger_pin = 10U;
	assert(linkr_debugger_logic_analyzer_wide11_burst_configured_pin_count_model(
		&config) == 11U);
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_EITHER;
	config.trigger_pin = 0U;
	assert(linkr_debugger_logic_analyzer_wide11_burst_configured_pin_count_model(
		&config) == 11U);
	config.selected_pin_count = 10U;
	assert(linkr_debugger_logic_analyzer_wide11_burst_configured_pin_count_model(
		&config) == 0U);
}

static void test_capture_snapshot_copy_contract(void)
{
	struct linkr_debugger_la_capture capture;
	struct linkr_debugger_la_capture snapshot;
	struct linkr_debugger_la_sample source[3];
	struct linkr_debugger_la_sample copied[3];
	struct linkr_debugger_la_sample too_small[2];

	memset(&capture, 0, sizeof(capture));
	memset(source, 0, sizeof(source));
	capture.state = LINKR_DEBUGGER_LA_STATE_DONE;
	capture.sample_count = 3U;
	capture.trigger_index = 1U;
	capture.requested_sample_rate_hz = 1000000U;
	capture.actual_sample_rate_hz = 1000000U;
	capture.sample_period_ps = 1000000ULL;
	capture.backend = "host-test";
	source[0].timestamp_us = 1U;
	source[0].values = 0x0001U;
	source[1].timestamp_us = 2U;
	source[1].values = 0x0002U;
	source[2].timestamp_us = 3U;
	source[2].values = 0x0004U;

	assert(linkr_debugger_logic_analyzer_get_capture(&snapshot, copied, 3U) == -ENODATA);
	assert(linkr_debugger_logic_analyzer_host_set_capture(&capture, source, 3U) == 0);
	assert(linkr_debugger_logic_analyzer_get_capture(&snapshot, too_small, 2U) == -ENOSPC);
	assert(linkr_debugger_logic_analyzer_get_capture(NULL, copied, 3U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_get_capture(&snapshot, NULL, 3U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_get_capture(&snapshot, copied, 0U) == -EINVAL);

	assert(linkr_debugger_logic_analyzer_get_capture(&snapshot, copied, 3U) == 0);
	assert(snapshot.samples == copied);
	assert(snapshot.sample_count == 3U);
	assert(copied[0].timestamp_us == 1U);
	assert(copied[1].values == 0x0002U);
	source[1].values = 0xffffU;
	assert(copied[1].values == 0x0002U);
}

int main(void)
{
	assert(linkr_debugger_logic_analyzer_init() == 0);
	test_rate_quantization();
	test_sample_capacity_and_ring_constants();
	test_wide11_burst_plan_counts_and_overflow();
	test_ring_delta_wraps_without_equal_addr_assumption();
	test_ring_elapsed_window_possible_overrun();
	test_ring_sequence_definite_overrun();
	test_ring_poll_interval_bounds();
	test_ring_observe_progress_and_restart_reset();
	test_ring_observe_first_poll_counts_from_start_index_zero();
	test_ring_observe_definite_overrun();
	test_ring_next_emit_count_gates_partial_tails();
	test_ring_drainable_samples_batches_complete_chunks();
	test_ring_freeze_before_overwrite_retains_tail_window();
	test_ring_terminal_emit_count_drains_partial_tail_only_when_terminal();
	test_ring_freeze_policy_stops_required_stream_hardware();
	test_ring_metrics_track_time_maxima_without_resetting_lower_values();
	test_ring_consume_metrics_track_max_totals_and_count();
	test_ring_consumer_latency_and_handoff_metrics();
	test_copy_complete_reader_advance_is_independent_of_callback_duration();
	test_stale_generation_cannot_advance_reader_or_update_protocol_state();
	test_stream_generation_lifecycle_gate();
	test_stream_start_idle_wait_gate();
	test_stream_sink_validate_rejects_unsupported_shapes();
	test_stream_sink_success_lifecycle_advances_after_payload_complete();
	test_stream_sink_capacity_failure_aborts_exactly_once();
	test_stream_sink_stale_generation_aborts_without_reader_advance();
	test_stream_sink_commit_failure_aborts_after_reader_advance();
	test_stream_sink_commit_positive_return_requests_handoff();
	test_stream_sink_single_packed_output_wraps_packed_ring_words();
	test_stream_sink_single_bits_payload_wraps_and_masks_tail();
	test_stream_sink_single_bits_all_high_and_chunk_contract();
	test_stream_sink_single_fast_path_matches_generic_patterns();
	test_stream_sink_single_fast_path_span_boundaries_match_generic();
	test_stream_sink_wrap_batch_matches_decode_span_single_byte();
	test_stream_sink_wrap_batch_matches_decode_span_two_bytes();
	test_stream_sink_fast8_large_random_wrap_matches_generic();
	test_stream_sink_wide11_large_random_lane_wraps_match_generic();
	test_stream_sink_sparse_fast8_selection_keeps_generic_order();
	test_stream_sink_single_fast_path_default_selection_matches_generic();
	test_stream_sink_single_fast_path_rejects_invalid_offsets();
	test_stream_sink_retries_only_precommit_backpressure_errors();
	test_stream_sink_consumer_can_yield_to_transport_thread();
	test_stream_sink_helpers_do_not_change_callback_protocol_gate();
	test_validation();
	test_stream_validation_allows_unlimited_and_uint16_bounded();
	test_bounded_pre_trigger_uses_prepared_packed_ring();
	test_bounded_pre_trigger_rejects_unsupported_contract_shapes();
	test_bounded_pre_trigger_window_preserves_index_and_wraps();
	test_bounded_pre_trigger_plan_feasibility();
	test_bounded_pre_trigger_scan_guards();
	test_compression();
	test_stream_irq_clear_helper();
	test_wide11_burst_exact_eligibility_is_strict();
	test_session_contract_trigger_gate_and_stop_policy();
	test_packed_burst_plan_sizing_and_continuous_capacity();
	test_packed_burst_decode_selected_pin_compaction();
	test_packed_ring_plan_sizing_and_chunk_limits();
	test_packed_ring_pre_trigger_word_scan();
	test_packed_ring_decode_wrap_and_sparse_selection();
	test_packed_ring_decode_keeps_wide11_sequence_above_u32();
	test_packed_ring_observe_dual_lane_min_seq_and_skew();
	test_hardware_plan_selector_normalizes_post512_and_post513();
	test_hardware_plan_selector_capability_matrix();
	test_packed_burst_nominal_rate_routing_regressions();
	test_packed_burst_requested_rate_cap_allows_quantized_125mhz();
	test_packed_physical_plan_rate_policy_prefers_requested_limit();
	test_hardware_plan_selector_rejects_removed_raw32_shapes();
	test_packed_burst_prepare_rejects_ordinary_stream_routing();
	test_wide11_burst_start_prepare_ordering_and_cleanup();
	test_wide11_burst_triggered_start_prepare_requires_armed_event_mark();
	test_wide11_burst_start_prepare_rejects_invalid_and_stale_tokens();
	test_stream_start_prepare_sets_hardware_barrier_before_response();
	test_stream_start_prepare_triggered_requires_armed_marker();
	test_effective_trigger_helper();
	test_capture_program_shapes_and_errors();
	test_wide11_burst_program_builders();
	test_wide11_burst_decode_model_patterns();
	test_wide11_burst_decode_target_boundaries();
	test_wide11_burst_decode_rejects_invalid_sizes_without_writes();
	test_wide11_burst_decode_span_outputs_packed_le_slices();
	test_wide11_burst_decode_span_rejects_bad_ranges_without_writes();
	test_wide11_burst_completion_mask_generation_model();
	test_wide11_burst_100000_chunk_accounting();
	test_wide11_burst_triggered_configured_pin_count_stays_11();
	test_capture_snapshot_copy_contract();
	return 0;
}
