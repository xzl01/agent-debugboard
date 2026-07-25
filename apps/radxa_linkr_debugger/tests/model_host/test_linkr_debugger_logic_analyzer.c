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

static void test_dma_block_size(void)
{
	assert(linkr_debugger_logic_analyzer_dma_block_size(0U) == 0U);
	assert(linkr_debugger_logic_analyzer_dma_block_size(1U) == 4U);
	assert(linkr_debugger_logic_analyzer_dma_block_size(512U) == 2048U);
	assert(linkr_debugger_logic_analyzer_dma_block_size(513U) == 0U);
	assert(linkr_debugger_logic_analyzer_max_samples(4U, 2048U) == 512U);
	assert(linkr_debugger_logic_analyzer_max_samples(0U, 2048U) == 0U);
	assert(LINKR_DEBUGGER_LA_RING_BUFFER_BYTES == 32768U);
	assert(LINKR_DEBUGGER_LA_RING_SIZE_BITS == 15U);
	assert(LINKR_DEBUGGER_LA_RING_SAMPLES == 8192U);
	assert(LINKR_DEBUGGER_LA_RING_SAFETY_SAMPLES == 1024U);
	assert(LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES == 1024U);
	assert(LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES == 2048U);
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
	sink.max_chunk_samples = LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES;
	assert(linkr_debugger_logic_analyzer_stream_sink_validate(&config, &sink) == 0);
	sink.max_chunk_samples = LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES + 1U;
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
	struct linkr_debugger_la_config config = base_config();
	uint32_t raw_ring[] = { 0U, (uint32_t)BIT(0), (uint32_t)BIT(1),
		(uint32_t)(BIT(0) | BIT(1)) };
	uint16_t values_or;
	uint16_t values_and;

	memset(&ctx, 0, sizeof(ctx));
	memset(&progress, 0, sizeof(progress));
	memset(&metrics, 0, sizeof(metrics));
	ctx.capacity = sizeof(ctx.storage);
	sink = sink_for_context(&ctx);
	progress.generation = 3U;
	progress.writer_seq = 4U;
	progress.reader_seq = 0U;

	assert(linkr_debugger_logic_analyzer_stream_sink_lease_payload(&sink, 4U,
		&lease) == 0);
	assert(ctx.lease_calls == 1U);
	assert(lease.payload == ctx.storage);
	assert(lease.token == &ctx);
	assert(linkr_debugger_logic_analyzer_stream_sink_write_raw_payload(&config,
		raw_ring, 4U, 0U, 4U, 1U, lease.payload, lease.capacity,
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
	commit.sequence = 9U;
	commit.sample_count = 4U;
	commit.timestamp_us = 123U;
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
}

static void test_stream_sink_capacity_failure_aborts_exactly_once(void)
{
	struct sink_test_context ctx;
	struct linkr_debugger_la_stream_sink sink;
	struct linkr_debugger_la_stream_sink_lease lease;
	struct linkr_debugger_la_config config = base_config();
	uint32_t raw_ring[] = { 0U, 1U, 2U, 3U };

	memset(&ctx, 0, sizeof(ctx));
	ctx.capacity = 2U;
	sink = sink_for_context(&ctx);
	assert(linkr_debugger_logic_analyzer_stream_sink_lease_payload(&sink, 4U,
		&lease) == 0);
	assert(linkr_debugger_logic_analyzer_stream_sink_write_raw_payload(&config,
		raw_ring, 4U, 0U, 4U, 1U, lease.payload, lease.capacity,
		NULL, NULL) == -ENOSPC);
	linkr_debugger_logic_analyzer_stream_sink_abort_payload(&sink, &lease);
	linkr_debugger_logic_analyzer_stream_sink_abort_payload(&sink, &lease);
	assert(ctx.abort_calls == 1U);
}

static void test_stream_sink_stale_generation_aborts_without_reader_advance(void)
{
	struct sink_test_context ctx;
	struct linkr_debugger_la_stream_sink sink;
	struct linkr_debugger_la_stream_sink_lease lease;
	struct linkr_debugger_la_ring_progress progress;
	struct linkr_debugger_la_ring_metrics metrics;
	struct linkr_debugger_la_config config = base_config();
	uint32_t raw_ring[] = { 0U, 1U, 2U, 3U };

	memset(&ctx, 0, sizeof(ctx));
	memset(&progress, 0, sizeof(progress));
	memset(&metrics, 0, sizeof(metrics));
	ctx.capacity = sizeof(ctx.storage);
	sink = sink_for_context(&ctx);
	progress.generation = 5U;
	progress.writer_seq = 4U;
	progress.reader_seq = 0U;
	assert(linkr_debugger_logic_analyzer_stream_sink_lease_payload(&sink, 4U,
		&lease) == 0);
	assert(linkr_debugger_logic_analyzer_stream_sink_write_raw_payload(&config,
		raw_ring, 4U, 0U, 4U, 1U, lease.payload, lease.capacity,
		NULL, NULL) == 0);
	assert(!linkr_debugger_logic_analyzer_stream_copy_complete_advance_reader(
		&progress, &metrics, false, 5U, 0U, 4U));
	assert(progress.reader_seq == 0U);
	linkr_debugger_logic_analyzer_stream_sink_abort_payload(&sink, &lease);
	assert(ctx.abort_calls == 1U);
	assert(ctx.commit_calls == 0U);
}

static void test_stream_sink_commit_failure_aborts_after_reader_advance(void)
{
	struct sink_test_context ctx;
	struct linkr_debugger_la_stream_sink sink;
	struct linkr_debugger_la_stream_sink_lease lease;
	struct linkr_debugger_la_stream_sink_commit commit;
	struct linkr_debugger_la_ring_progress progress;
	struct linkr_debugger_la_ring_metrics metrics;
	struct linkr_debugger_la_config config = base_config();
	uint32_t raw_ring[] = { 0U, 1U, 2U, 3U };

	memset(&ctx, 0, sizeof(ctx));
	memset(&progress, 0, sizeof(progress));
	memset(&metrics, 0, sizeof(metrics));
	ctx.capacity = sizeof(ctx.storage);
	ctx.commit_ret = -EIO;
	sink = sink_for_context(&ctx);
	progress.generation = 6U;
	progress.writer_seq = 4U;
	progress.reader_seq = 0U;
	assert(linkr_debugger_logic_analyzer_stream_sink_lease_payload(&sink, 4U,
		&lease) == 0);
	assert(linkr_debugger_logic_analyzer_stream_sink_write_raw_payload(&config,
		raw_ring, 4U, 0U, 4U, 1U, lease.payload, lease.capacity,
		NULL, NULL) == 0);
	assert(linkr_debugger_logic_analyzer_stream_copy_complete_advance_reader(
		&progress, &metrics, true, 6U, 0U, 4U));
	assert(progress.reader_seq == 4U);
	memset(&commit, 0, sizeof(commit));
	commit.token = lease.token;
	commit.sequence = 1U;
	commit.sample_count = 4U;
	commit.bytes_per_sample = 1U;
	commit.payload_len = 4U;
	assert(linkr_debugger_logic_analyzer_stream_sink_commit_payload(&sink, &commit) == -EIO);
	linkr_debugger_logic_analyzer_stream_sink_abort_payload(&sink, &lease);
	assert(ctx.commit_calls == 1U);
	assert(ctx.abort_calls == 1U);
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
	commit.sequence = 2U;
	commit.sample_count = 4U;
	commit.bytes_per_sample = 1U;
	commit.payload_len = 4U;
	assert(linkr_debugger_logic_analyzer_stream_sink_commit_payload(&sink, &commit) == 1);
	assert(ctx.commit_calls == 1U);
	assert(ctx.abort_calls == 0U);
}

static void test_stream_sink_single_packed_output_wraps_raw_ring(void)
{
	struct linkr_debugger_la_config config;
	uint32_t raw_ring[4];
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
	memset(out, 0xff, sizeof(out));
	assert(linkr_debugger_logic_analyzer_stream_sink_write_raw_payload(&config,
		raw_ring, 4U, 2U, 5U, 1U, out, sizeof(out), &values_or,
		&values_and) == 0);
	assert(memcmp(out, (uint8_t[]){0x00U, 0x01U, 0x01U, 0x00U, 0x00U}, 5U) == 0);
	assert(values_or == 0x0001U);
	assert(values_and == 0x0000U);
}

static void assert_single_sink_matches_generic(
	const struct linkr_debugger_la_config *config,
	const uint32_t *raw_ring,
	uint32_t ring_samples,
	uint64_t first_seq,
	uint32_t sample_count)
{
	uint8_t out[8];
	uint8_t expected[8];
	uint16_t values_or;
	uint16_t values_and;
	uint16_t expected_or = 0U;
	uint16_t expected_and = 0xffffU;

	assert(sample_count <= sizeof(out));
	memset(out, 0xff, sizeof(out));
	memset(expected, 0xff, sizeof(expected));
	assert(linkr_debugger_logic_analyzer_stream_sink_write_raw_payload(config,
		raw_ring, ring_samples, first_seq, sample_count, 1U, out, sample_count,
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
	uint32_t raw_ring[] = { 0U };
	uint8_t out[1];

	memset(&config, 0, sizeof(config));
	config.pin_base = 10U;
	config.pin_count = 1U;
	config.selected_pins[0] = 9U;
	config.selected_pin_count = 1U;
	config.sample_rate_hz = 1000000U;
	assert(linkr_debugger_logic_analyzer_stream_sink_write_raw_payload(&config,
		raw_ring, 1U, 0U, 1U, 1U, out, sizeof(out), NULL, NULL) == -EINVAL);

	config.pin_base = 0U;
	config.selected_pins[0] = 32U;
	assert(linkr_debugger_logic_analyzer_stream_sink_write_raw_payload(&config,
		raw_ring, 1U, 0U, 1U, 1U, out, sizeof(out), NULL, NULL) == -EINVAL);
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
	assert(linkr_debugger_logic_analyzer_stream_consumer_priority(true) == 7);
	assert(linkr_debugger_logic_analyzer_stream_consumer_priority(false) == 8);
	assert(!linkr_debugger_logic_analyzer_stream_sink_should_yield_for_handoff(false, 0U));
	assert(linkr_debugger_logic_analyzer_stream_sink_should_yield_for_handoff(true, 0U));
	assert(linkr_debugger_logic_analyzer_stream_sink_should_yield_for_handoff(true, 2047U));
	assert(linkr_debugger_logic_analyzer_stream_sink_should_yield_for_handoff(true, 2048U));
	assert(!linkr_debugger_logic_analyzer_stream_sink_should_yield_for_handoff(true, 2049U));
	assert(!linkr_debugger_logic_analyzer_stream_sink_should_explicit_yield(false, 0U));
	assert(!linkr_debugger_logic_analyzer_stream_sink_should_explicit_yield(true, 0U));
	assert(!linkr_debugger_logic_analyzer_stream_sink_should_explicit_yield(true, 2048U));
	assert(!linkr_debugger_logic_analyzer_stream_sink_should_explicit_yield(true, 2049U));
}

static void test_validation(void)
{
	struct linkr_debugger_la_config config = base_config();

	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == 0);

	config = base_config();
	config.selected_pins[0] = 7U;
	config.selected_pins[1] = 10U;
	config.selected_pins[2] = 29U;
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
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == 0);

	config = base_config();
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_EITHER;
	config.trigger_pin = 1U;
	config.pre_samples = 8U;
	config.post_samples = 8U;
	config.sample_rate_hz = 25000000U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == 0);

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
	config.selected_pins[2] = 29U;
	config.selected_pin_count = 3U;
	config.pin_count = 3U;

	assert(linkr_debugger_logic_analyzer_compress_raw_sample(
		(uint32_t)(BIT(0) | BIT(22)), &config) == 5U);
	assert(linkr_debugger_logic_analyzer_compress_raw_sample(
		(uint32_t)BIT(3), &config) == 2U);
}

static void test_finite_stream_helpers(void)
{
	struct linkr_debugger_la_config config = base_config();

	assert(!linkr_debugger_logic_analyzer_finite_stream_eligible(NULL));
	assert(!linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(NULL));

	assert(linkr_debugger_logic_analyzer_finite_stream_eligible(&config));
	assert(!linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(&config));

	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;
	assert(linkr_debugger_logic_analyzer_finite_stream_eligible(&config));
	assert(!linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(&config));

	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	assert(linkr_debugger_logic_analyzer_finite_stream_eligible(&config));
	assert(linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(&config));

	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_FALLING;
	assert(linkr_debugger_logic_analyzer_finite_stream_eligible(&config));
	assert(linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(&config));

	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_EITHER;
	assert(linkr_debugger_logic_analyzer_finite_stream_eligible(&config));
	assert(linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(&config));

	config.pre_samples = 1U;
	assert(!linkr_debugger_logic_analyzer_finite_stream_eligible(&config));
	assert(linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(&config));

	config.pre_samples = 0U;
	config.post_samples = 0U;
	assert(!linkr_debugger_logic_analyzer_finite_stream_eligible(&config));

	config.post_samples = 1U;
	assert(linkr_debugger_logic_analyzer_finite_stream_eligible(&config));

	config.post_samples = LINKR_DEBUGGER_LA_FINITE_GATED_MAX_SAMPLES;
	assert(linkr_debugger_logic_analyzer_finite_stream_eligible(&config));

	config.post_samples = LINKR_DEBUGGER_LA_FINITE_GATED_MAX_SAMPLES + 1U;
	assert(!linkr_debugger_logic_analyzer_finite_stream_eligible(&config));
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
	test_rate_quantization();
	test_dma_block_size();
	test_ring_delta_wraps_without_equal_addr_assumption();
	test_ring_elapsed_window_possible_overrun();
	test_ring_sequence_definite_overrun();
	test_ring_poll_interval_bounds();
	test_ring_observe_progress_and_restart_reset();
	test_ring_observe_first_poll_counts_from_start_index_zero();
	test_ring_observe_definite_overrun();
	test_ring_next_emit_count_gates_partial_tails();
	test_ring_drainable_samples_batches_complete_chunks();
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
	test_stream_sink_single_packed_output_wraps_raw_ring();
	test_stream_sink_single_fast_path_matches_generic_patterns();
	test_stream_sink_single_fast_path_span_boundaries_match_generic();
	test_stream_sink_single_fast_path_default_selection_matches_generic();
	test_stream_sink_single_fast_path_rejects_invalid_offsets();
	test_stream_sink_helpers_do_not_change_callback_protocol_gate();
	test_validation();
	test_stream_validation_allows_unlimited_and_uint16_bounded();
	test_compression();
	test_finite_stream_helpers();
	test_effective_trigger_helper();
	test_capture_program_shapes_and_errors();
	test_capture_snapshot_copy_contract();
	return 0;
}
