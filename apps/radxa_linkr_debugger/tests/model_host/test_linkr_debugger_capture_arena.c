/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_capture_arena.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

struct hook_context {
	int quiesce_ret;
	uint32_t quiesce_calls;
	uint32_t resume_calls;
	int32_t last_timeout_ms;
	bool corrupt_canary;
	bool nested_acquire_failed;
	bool resume_nested_acquire_failed;
};

static int test_quiesce_hook(int32_t timeout_ms, void *user_data)
{
	struct hook_context *ctx = user_data;
	struct linkr_debugger_capture_arena_lease nested;

	ctx->quiesce_calls++;
	ctx->last_timeout_ms = timeout_ms;
	ctx->nested_acquire_failed = !linkr_debugger_capture_arena_try_acquire_wide11(99U, &nested);
	assert(linkr_debugger_capture_arena_owner() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_WIDE11_BURST);
	assert(linkr_debugger_capture_arena_state() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_QUIESCING);
	if (ctx->corrupt_canary) {
		uint8_t *base = linkr_debugger_capture_arena_base();

		base[LINKR_DEBUGGER_CAPTURE_ARENA_BURST_SOURCE_CANARY_OFFSET] ^= 0xffU;
	}
	return ctx->quiesce_ret;
}

static void test_resume_hook(void *user_data)
{
	struct hook_context *ctx = user_data;
	struct linkr_debugger_capture_arena_lease nested;

	ctx->resume_calls++;
	ctx->resume_nested_acquire_failed =
		!linkr_debugger_capture_arena_try_acquire_wide11(98U, &nested);
}

static void register_test_hook(struct hook_context *ctx)
{
	const struct linkr_debugger_capture_arena_quiesce_ops ops = {
		.quiesce = test_quiesce_hook,
		.resume = test_resume_hook,
		.user_data = ctx,
	};

	linkr_debugger_capture_arena_register_quiesce_ops(&ops);
}

static void test_layout_offsets_and_alignment(void)
{
	uint8_t *base;

	linkr_debugger_capture_arena_init();
	base = linkr_debugger_capture_arena_base();

	assert(base != NULL);
	assert(LINKR_DEBUGGER_CAPTURE_ARENA_WS_SAMPLE_RING_OFFSET == 35840U);
	assert(LINKR_DEBUGGER_CAPTURE_ARENA_WS_SAMPLE_RING_BYTES == 30720U);
	assert(LINKR_DEBUGGER_CAPTURE_ARENA_POWER_CAPTURE_OFFSET == 66560U);
	assert(LINKR_DEBUGGER_CAPTURE_ARENA_POWER_CAPTURE_BYTES == 65672U);
	assert(LINKR_DEBUGGER_CAPTURE_ARENA_SIGROK_WS_POOL_OFFSET == 132232U);
	assert(LINKR_DEBUGGER_CAPTURE_ARENA_NORMAL_BYTES == 148856U);
	assert(LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TOTAL_BYTES == 144184U);
	assert(LINKR_DEBUGGER_CAPTURE_ARENA_BYTES == 148856U);
	assert(linkr_debugger_capture_arena_size() == LINKR_DEBUGGER_CAPTURE_ARENA_BYTES);
	assert(((uintptr_t)base & (LINKR_DEBUGGER_CAPTURE_ARENA_ALIGN - 1U)) == 0U);
	assert((uint8_t *)linkr_debugger_capture_arena_la_packed_ring() ==
		base + LINKR_DEBUGGER_CAPTURE_ARENA_LA_PACKED_RING_OFFSET);
	assert((uint8_t *)linkr_debugger_capture_arena_la_finite_samples() ==
		base + LINKR_DEBUGGER_CAPTURE_ARENA_LA_FINITE_SAMPLES_OFFSET);
	assert((uint8_t *)linkr_debugger_capture_arena_la_scratch() ==
		base + LINKR_DEBUGGER_CAPTURE_ARENA_LA_SCRATCH_OFFSET);
	assert((uint8_t *)linkr_debugger_capture_arena_la_pre_trigger() ==
		base + LINKR_DEBUGGER_CAPTURE_ARENA_LA_PRE_TRIGGER_OFFSET);
	assert((uint8_t *)linkr_debugger_capture_arena_ws_sample_ring() ==
		base + LINKR_DEBUGGER_CAPTURE_ARENA_WS_SAMPLE_RING_OFFSET);
	assert((uint8_t *)linkr_debugger_capture_arena_power_capture() ==
		base + LINKR_DEBUGGER_CAPTURE_ARENA_POWER_CAPTURE_OFFSET);
	assert((uint8_t *)linkr_debugger_capture_arena_sigrok_ws_pool() ==
		base + LINKR_DEBUGGER_CAPTURE_ARENA_SIGROK_WS_POOL_OFFSET);
	assert(linkr_debugger_capture_arena_burst_lane_a() ==
		base + LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_A_OFFSET);
	assert(linkr_debugger_capture_arena_burst_lane_b() ==
		base + LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_B_OFFSET);
	assert(linkr_debugger_capture_arena_burst_tx_slot(0U) ==
		base + LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TX0_OFFSET);
	assert(linkr_debugger_capture_arena_burst_tx_slot(1U) ==
		base + LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TX1_OFFSET);
	assert(linkr_debugger_capture_arena_burst_tx_slot(2U) == NULL);
	assert(linkr_debugger_capture_arena_burst_terminal() ==
		base + LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TERMINAL_OFFSET);
	assert(linkr_debugger_capture_arena_region(
		LINKR_DEBUGGER_CAPTURE_ARENA_BYTES, 1U) == NULL);
	assert(linkr_debugger_capture_arena_region(
		LINKR_DEBUGGER_CAPTURE_ARENA_BYTES - 1U, 1U) ==
		base + LINKR_DEBUGGER_CAPTURE_ARENA_BYTES - 1U);
}

static void test_canaries_gate_acquire(void)
{
	struct linkr_debugger_capture_arena_lease lease;
	uint8_t *base;

	linkr_debugger_capture_arena_init();
	base = linkr_debugger_capture_arena_base();
	assert(linkr_debugger_capture_arena_canaries_ok());

	base[LINKR_DEBUGGER_CAPTURE_ARENA_BURST_SOURCE_CANARY_OFFSET] ^= 0xffU;
	assert(!linkr_debugger_capture_arena_canaries_ok());
	assert(!linkr_debugger_capture_arena_try_acquire_wide11(1U, &lease));
	assert(linkr_debugger_capture_arena_owner() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE);

	linkr_debugger_capture_arena_init();
	assert(linkr_debugger_capture_arena_canaries_ok());
}

static void test_lease_state_machine(void)
{
	struct linkr_debugger_capture_arena_lease lease;
	struct linkr_debugger_capture_arena_lease stale;

	linkr_debugger_capture_arena_init();
	assert(!linkr_debugger_capture_arena_try_acquire_wide11(0U, &lease));
	assert(linkr_debugger_capture_arena_try_acquire_wide11(7U, &lease));
	stale = lease;
	assert(lease.owner == LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_WIDE11_BURST);
	assert(lease.session_id == 7U);
	assert(linkr_debugger_capture_arena_owner() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_WIDE11_BURST);
	assert(linkr_debugger_capture_arena_state() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_ACQUIRED);
	assert(!linkr_debugger_capture_arena_try_acquire_wide11(8U, &lease));
	assert(linkr_debugger_capture_arena_mark_dma_active(&stale) == -EINVAL);
	assert(linkr_debugger_capture_arena_mark_armed(&stale) == 0);
	assert(linkr_debugger_capture_arena_mark_armed(&stale) == -EINVAL);
	assert(linkr_debugger_capture_arena_mark_dma_active(&stale) == 0);
	assert(linkr_debugger_capture_arena_mark_postprocess(&stale) == 0);
	assert(linkr_debugger_capture_arena_mark_network_send(&stale) == 0);
	assert(linkr_debugger_capture_arena_release(&stale));
	assert(linkr_debugger_capture_arena_state() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_IDLE);
	assert(linkr_debugger_capture_arena_owner() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE);
	assert(!linkr_debugger_capture_arena_release(&stale));
}

static void test_quiesced_acquire_success_reserves_then_releases_once(void)
{
	struct hook_context ctx;
	struct linkr_debugger_capture_arena_lease lease;

	memset(&ctx, 0, sizeof(ctx));
	linkr_debugger_capture_arena_init();
	register_test_hook(&ctx);
	assert(linkr_debugger_capture_arena_try_acquire_wide11_quiesced(11U, 1234, &lease) == 0);
	assert(ctx.quiesce_calls == 1U);
	assert(ctx.resume_calls == 0U);
	assert(ctx.last_timeout_ms == 1234);
	assert(ctx.nested_acquire_failed);
	assert(lease.owner == LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_WIDE11_BURST);
	assert(linkr_debugger_capture_arena_state() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_ACQUIRED);
	assert(linkr_debugger_capture_arena_release(&lease));
	assert(ctx.resume_calls == 1U);
	assert(ctx.resume_nested_acquire_failed);
	assert(!linkr_debugger_capture_arena_release(&lease));
	assert(ctx.resume_calls == 1U);
	assert(ctx.resume_nested_acquire_failed);
	linkr_debugger_capture_arena_register_quiesce_ops(NULL);
}

static void test_quiesced_acquire_failure_rolls_back_and_resumes_once(void)
{
	struct hook_context ctx;
	struct linkr_debugger_capture_arena_lease lease;

	memset(&ctx, 0, sizeof(ctx));
	ctx.quiesce_ret = -110;
	linkr_debugger_capture_arena_init();
	register_test_hook(&ctx);
	memset(&lease, 0xff, sizeof(lease));
	assert(linkr_debugger_capture_arena_try_acquire_wide11_quiesced(12U, 5, &lease) == -110);
	assert(ctx.quiesce_calls == 1U);
	assert(ctx.resume_calls == 1U);
	assert(lease.owner == LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE);
	assert(linkr_debugger_capture_arena_owner() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE);
	assert(linkr_debugger_capture_arena_state() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_IDLE);
	assert(linkr_debugger_capture_arena_try_acquire_wide11(13U, &lease));
	assert(linkr_debugger_capture_arena_release(&lease));
	assert(ctx.resume_calls == 1U);
	linkr_debugger_capture_arena_register_quiesce_ops(NULL);
}

static void test_quiesced_acquire_succeeds_after_quiesce_hook_canary_corruption(void)
{
	struct hook_context ctx;
	struct linkr_debugger_capture_arena_lease lease;

	memset(&ctx, 0, sizeof(ctx));
	ctx.corrupt_canary = true;
	linkr_debugger_capture_arena_init();
	register_test_hook(&ctx);
	assert(linkr_debugger_capture_arena_try_acquire_wide11_quiesced(14U, -1, &lease) == 0);
	assert(ctx.quiesce_calls == 1U);
	assert(ctx.resume_calls == 0U);
	assert(lease.owner == LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_WIDE11_BURST);
	assert(linkr_debugger_capture_arena_state() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_ACQUIRED);
	assert(linkr_debugger_capture_arena_canaries_ok());
	assert(linkr_debugger_capture_arena_release(&lease));
	assert(ctx.resume_calls == 1U);
	linkr_debugger_capture_arena_register_quiesce_ops(NULL);
}

static void test_quiesced_acquire_without_hook_preserves_try_acquire_semantics(void)
{
	struct linkr_debugger_capture_arena_lease lease;

	linkr_debugger_capture_arena_init();
	linkr_debugger_capture_arena_register_quiesce_ops(NULL);
	assert(linkr_debugger_capture_arena_try_acquire_wide11_quiesced(15U, 0, &lease) == 0);
	assert(linkr_debugger_capture_arena_release(&lease));
}

static void test_ws_quiesce_pure_decision_helper(void)
{
	assert(linkr_debugger_capture_arena_ws_quiesce_allows_overwrite(false, true, false, true));
	assert(!linkr_debugger_capture_arena_ws_quiesce_allows_overwrite(true, true, false, true));
	assert(!linkr_debugger_capture_arena_ws_quiesce_allows_overwrite(false, false, false, true));
	assert(!linkr_debugger_capture_arena_ws_quiesce_allows_overwrite(false, true, true, true));
	assert(!linkr_debugger_capture_arena_ws_quiesce_allows_overwrite(false, true, false, false));
}

static bool second_telemetry_client_would_emit_ring_sample(bool adc_pause_requested,
							  bool arena_quiesced,
							  uint64_t latest_sequence)
{
	return linkr_debugger_capture_arena_ws_sample_read_allowed(
		adc_pause_requested, arena_quiesced) && latest_sequence != 0U;
}

static void test_ws_sample_read_gate_blocks_wide11_lease_until_resume(void)
{
	struct hook_context ctx;
	struct linkr_debugger_capture_arena_lease lease;
	const uint64_t overlapped_ring_sequence = 42U;
	const uint64_t empty_ring_after_resume = 0U;
	const uint64_t new_sample_after_resume = 1U;

	memset(&ctx, 0, sizeof(ctx));
	linkr_debugger_capture_arena_init();
	register_test_hook(&ctx);

	assert(second_telemetry_client_would_emit_ring_sample(false, false,
		overlapped_ring_sequence));
	assert(!second_telemetry_client_would_emit_ring_sample(true, false,
		overlapped_ring_sequence));
	assert(linkr_debugger_capture_arena_try_acquire_wide11_quiesced(16U, 0,
		&lease) == 0);
	assert(!second_telemetry_client_would_emit_ring_sample(true, true,
		overlapped_ring_sequence));
	assert(!second_telemetry_client_would_emit_ring_sample(false, true,
		overlapped_ring_sequence));
	assert(linkr_debugger_capture_arena_release(&lease));
	assert(!second_telemetry_client_would_emit_ring_sample(false, false,
		empty_ring_after_resume));
	assert(second_telemetry_client_would_emit_ring_sample(false, false,
		new_sample_after_resume));
	linkr_debugger_capture_arena_register_quiesce_ops(NULL);
}

static void test_quiesced_acquire_after_normal_overwrite_succeeds(void)
{
	struct hook_context ctx;
	struct linkr_debugger_capture_arena_lease lease;
	uint8_t *base;

	memset(&ctx, 0, sizeof(ctx));
	linkr_debugger_capture_arena_init();
	register_test_hook(&ctx);

	assert(linkr_debugger_capture_arena_try_acquire_wide11_quiesced(17U, 0,
		&lease) == 0);
	assert(linkr_debugger_capture_arena_release(&lease));
	assert(ctx.resume_calls == 1U);

	base = linkr_debugger_capture_arena_base();
	memset(base + LINKR_DEBUGGER_CAPTURE_ARENA_SIGROK_WS_POOL_OFFSET, 0xBD,
		LINKR_DEBUGGER_CAPTURE_ARENA_SIGROK_WS_POOL_BYTES);

	assert(linkr_debugger_capture_arena_try_acquire_wide11_quiesced(18U, 0,
		&lease) == 0);
	assert(linkr_debugger_capture_arena_canaries_ok());
	assert(linkr_debugger_capture_arena_release(&lease));

	linkr_debugger_capture_arena_register_quiesce_ops(NULL);
}

static void test_non_quiesced_acquire_detects_corrupted_canary(void)
{
	struct linkr_debugger_capture_arena_lease lease;
	uint8_t *base;

	linkr_debugger_capture_arena_init();
	base = linkr_debugger_capture_arena_base();

	base[LINKR_DEBUGGER_CAPTURE_ARENA_BURST_SOURCE_CANARY_OFFSET] ^= 0x55U;
	assert(!linkr_debugger_capture_arena_try_acquire_wide11(19U, &lease));
	assert(linkr_debugger_capture_arena_owner() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE);

	linkr_debugger_capture_arena_init();
	assert(linkr_debugger_capture_arena_try_acquire_wide11(20U, &lease));
	assert(linkr_debugger_capture_arena_release(&lease));
}

static void test_active_lease_canary_corruption_detected_by_canaries_ok(void)
{
	struct linkr_debugger_capture_arena_lease lease;
	uint8_t *base;

	linkr_debugger_capture_arena_init();
	base = linkr_debugger_capture_arena_base();

	assert(linkr_debugger_capture_arena_try_acquire_wide11(21U, &lease));
	assert(linkr_debugger_capture_arena_canaries_ok());

	base[LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TAIL_CANARY_OFFSET] ^= 0xAAU;
	assert(!linkr_debugger_capture_arena_canaries_ok());

	assert(linkr_debugger_capture_arena_release(&lease));
	assert(linkr_debugger_capture_arena_canaries_ok());
}


static void test_burst_slot_model_two_slot_exhaustion_and_generation(void)
{
	struct linkr_debugger_capture_arena_burst_slot_model model;

	linkr_debugger_capture_arena_burst_slot_model_begin(&model, 21U, 3U);
	assert(linkr_debugger_capture_arena_burst_slot_model_acquire_data(&model, 21U, 3U));
	assert(linkr_debugger_capture_arena_burst_slot_model_acquire_data(&model, 21U, 3U));
	assert(!linkr_debugger_capture_arena_burst_slot_model_acquire_data(&model, 21U, 3U));
	assert(!linkr_debugger_capture_arena_burst_slot_model_acquire_data(&model, 21U, 4U));
	assert(!linkr_debugger_capture_arena_burst_slot_model_acquire_data(&model, 22U, 3U));
	linkr_debugger_capture_arena_burst_slot_model_release_data(&model);
	assert(linkr_debugger_capture_arena_burst_slot_model_acquire_data(&model, 21U, 3U));
}

static void test_burst_slot_model_terminal_ordering_and_drained(void)
{
	struct linkr_debugger_capture_arena_burst_slot_model model;

	linkr_debugger_capture_arena_burst_slot_model_begin(&model, 31U, 5U);
	assert(linkr_debugger_capture_arena_burst_slot_model_acquire_data(&model, 31U, 5U));
	assert(!linkr_debugger_capture_arena_burst_slot_model_acquire_terminal(&model, 31U, 5U));
	linkr_debugger_capture_arena_burst_slot_model_mark_source_done(&model, 31U, 4U);
	assert(!linkr_debugger_capture_arena_burst_slot_model_drained(&model));
	linkr_debugger_capture_arena_burst_slot_model_mark_source_done(&model, 31U, 5U);
	assert(!linkr_debugger_capture_arena_burst_slot_model_drained(&model));
	linkr_debugger_capture_arena_burst_slot_model_release_data(&model);
	assert(linkr_debugger_capture_arena_burst_slot_model_acquire_terminal(&model, 31U, 5U));
	assert(!linkr_debugger_capture_arena_burst_slot_model_acquire_terminal(&model, 31U, 5U));
	assert(!linkr_debugger_capture_arena_burst_slot_model_drained(&model));
	linkr_debugger_capture_arena_burst_slot_model_release_terminal(&model);
	assert(linkr_debugger_capture_arena_burst_slot_model_drained(&model));
}

static void test_burst_slot_model_two_slot_wake_terminal_after_data(void)
{
	struct linkr_debugger_capture_arena_burst_slot_model model;

	linkr_debugger_capture_arena_burst_slot_model_begin(&model, 51U, 9U);
	assert(linkr_debugger_capture_arena_burst_slot_model_acquire_data(&model, 51U, 9U));
	assert(linkr_debugger_capture_arena_burst_slot_model_acquire_data(&model, 51U, 9U));
	assert(!linkr_debugger_capture_arena_burst_slot_model_acquire_data(&model, 51U, 9U));
	linkr_debugger_capture_arena_burst_slot_model_mark_source_done(&model, 51U, 9U);
	assert(!linkr_debugger_capture_arena_burst_slot_model_acquire_terminal(&model, 51U, 9U));
	linkr_debugger_capture_arena_burst_slot_model_release_data(&model);
	assert(linkr_debugger_capture_arena_burst_slot_model_acquire_data(&model, 51U, 9U));
	assert(!linkr_debugger_capture_arena_burst_slot_model_acquire_terminal(&model, 51U, 9U));
	linkr_debugger_capture_arena_burst_slot_model_release_data(&model);
	assert(!linkr_debugger_capture_arena_burst_slot_model_acquire_terminal(&model, 51U, 9U));
	linkr_debugger_capture_arena_burst_slot_model_release_data(&model);
	assert(linkr_debugger_capture_arena_burst_slot_model_acquire_terminal(&model, 51U, 9U));
	assert(!linkr_debugger_capture_arena_burst_slot_model_drained(&model));
	linkr_debugger_capture_arena_burst_slot_model_release_terminal(&model);
	assert(linkr_debugger_capture_arena_burst_slot_model_drained(&model));
}

static void test_burst_slot_model_abort_requires_owner_generation(void)
{
	struct linkr_debugger_capture_arena_burst_slot_model model;

	linkr_debugger_capture_arena_burst_slot_model_begin(&model, 41U, 7U);
	assert(linkr_debugger_capture_arena_burst_slot_model_acquire_data(&model, 41U, 7U));
	linkr_debugger_capture_arena_burst_slot_model_abort(&model, 41U, 8U);
	assert(model.active);
	linkr_debugger_capture_arena_burst_slot_model_abort(&model, 41U, 7U);
	assert(!model.active);
	assert(model.data_in_use == 0U);
}

int main(void)
{
	test_layout_offsets_and_alignment();
	test_canaries_gate_acquire();
	test_lease_state_machine();
	test_quiesced_acquire_success_reserves_then_releases_once();
	test_quiesced_acquire_failure_rolls_back_and_resumes_once();
	test_quiesced_acquire_succeeds_after_quiesce_hook_canary_corruption();
	test_quiesced_acquire_without_hook_preserves_try_acquire_semantics();
	test_ws_quiesce_pure_decision_helper();
	test_ws_sample_read_gate_blocks_wide11_lease_until_resume();
	test_quiesced_acquire_after_normal_overwrite_succeeds();
	test_non_quiesced_acquire_detects_corrupted_canary();
	test_active_lease_canary_corruption_detected_by_canaries_ok();
	test_burst_slot_model_two_slot_exhaustion_and_generation();
	test_burst_slot_model_terminal_ordering_and_drained();
	test_burst_slot_model_two_slot_wake_terminal_after_data();
	test_burst_slot_model_abort_requires_owner_generation();
	return 0;
}
