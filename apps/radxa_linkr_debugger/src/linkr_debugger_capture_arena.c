/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_capture_arena.h"

#include <errno.h>
#include <string.h>

#ifndef LINKR_DEBUGGER_CAPTURE_ARENA_HOST_TEST
#include <zephyr/irq.h>
#endif

#ifndef __aligned
#define __aligned(x) __attribute__((aligned(x)))
#endif

#define LINKR_DEBUGGER_CAPTURE_ARENA_CANARY_PATTERN 0xa5U

#ifdef LINKR_DEBUGGER_CAPTURE_ARENA_HOST_TEST
/*
 * Mach-O limits section alignment to 16 KiB, so a 32 KiB-aligned global is
 * silently under-aligned by the macOS linker. Keep the firmware allocation
 * exact, but over-allocate and align the host-model view explicitly.
 */
static uint8_t linkr_debugger_capture_arena_host_storage[
	LINKR_DEBUGGER_CAPTURE_ARENA_BYTES + LINKR_DEBUGGER_CAPTURE_ARENA_ALIGN - 1U];

static uint8_t *linkr_debugger_capture_arena_storage(void)
{
	uintptr_t start = (uintptr_t)linkr_debugger_capture_arena_host_storage;
	uintptr_t aligned = (start + LINKR_DEBUGGER_CAPTURE_ARENA_ALIGN - 1U) &
		~(uintptr_t)(LINKR_DEBUGGER_CAPTURE_ARENA_ALIGN - 1U);

	return (uint8_t *)aligned;
}
#else
static uint8_t linkr_debugger_capture_arena[LINKR_DEBUGGER_CAPTURE_ARENA_BYTES]
	__aligned(LINKR_DEBUGGER_CAPTURE_ARENA_ALIGN);

static uint8_t *linkr_debugger_capture_arena_storage(void)
{
	return linkr_debugger_capture_arena;
}
#endif

static enum linkr_debugger_capture_arena_owner linkr_debugger_capture_arena_current_owner =
	LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE;
static enum linkr_debugger_capture_arena_state linkr_debugger_capture_arena_current_state =
	LINKR_DEBUGGER_CAPTURE_ARENA_STATE_IDLE;
static uint32_t linkr_debugger_capture_arena_current_session_id;
static uint32_t linkr_debugger_capture_arena_generation;
static bool linkr_debugger_capture_arena_resume_armed;
static bool linkr_debugger_capture_arena_release_in_progress;
static struct linkr_debugger_capture_arena_quiesce_ops linkr_debugger_capture_arena_quiesce_ops;

static unsigned int linkr_debugger_capture_arena_lock(void)
{
#ifdef LINKR_DEBUGGER_CAPTURE_ARENA_HOST_TEST
	return 0U;
#else
	return irq_lock();
#endif
}

static void linkr_debugger_capture_arena_unlock(unsigned int key)
{
#ifdef LINKR_DEBUGGER_CAPTURE_ARENA_HOST_TEST
	(void)key;
#else
	irq_unlock(key);
#endif
}

static void linkr_debugger_capture_arena_fill_canary(size_t offset)
{
	memset(&linkr_debugger_capture_arena_storage()[offset],
		LINKR_DEBUGGER_CAPTURE_ARENA_CANARY_PATTERN,
		LINKR_DEBUGGER_CAPTURE_ARENA_CANARY_BYTES);
}

static bool linkr_debugger_capture_arena_check_canary(size_t offset)
{
	for (size_t i = 0U; i < LINKR_DEBUGGER_CAPTURE_ARENA_CANARY_BYTES; i++) {
		if (linkr_debugger_capture_arena_storage()[offset + i] !=
		    LINKR_DEBUGGER_CAPTURE_ARENA_CANARY_PATTERN) {
			return false;
		}
	}

	return true;
}

static bool linkr_debugger_capture_arena_refill_canaries_if_clean(void)
{
	linkr_debugger_capture_arena_fill_canary(
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_SOURCE_CANARY_OFFSET);
	linkr_debugger_capture_arena_fill_canary(
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TAIL_CANARY_OFFSET);
	return linkr_debugger_capture_arena_check_canary(
		       LINKR_DEBUGGER_CAPTURE_ARENA_BURST_SOURCE_CANARY_OFFSET) &&
	       linkr_debugger_capture_arena_check_canary(
		       LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TAIL_CANARY_OFFSET);
}

static bool linkr_debugger_capture_arena_lease_matches_locked(
	const struct linkr_debugger_capture_arena_lease *lease)
{
	return lease != NULL &&
		lease->owner == linkr_debugger_capture_arena_current_owner &&
		lease->owner != LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE &&
		lease->session_id == linkr_debugger_capture_arena_current_session_id &&
		lease->generation == linkr_debugger_capture_arena_generation;
}

static int linkr_debugger_capture_arena_mark(
	const struct linkr_debugger_capture_arena_lease *lease,
	enum linkr_debugger_capture_arena_state from,
	enum linkr_debugger_capture_arena_state to)
{
	int ret = -EPERM;
	unsigned int key = linkr_debugger_capture_arena_lock();

	if (linkr_debugger_capture_arena_lease_matches_locked(lease)) {
		if (linkr_debugger_capture_arena_current_state == from) {
			linkr_debugger_capture_arena_current_state = to;
			ret = 0;
		} else {
			ret = -EINVAL;
		}
	}

	linkr_debugger_capture_arena_unlock(key);
	return ret;
}

void linkr_debugger_capture_arena_init(void)
{
	unsigned int key = linkr_debugger_capture_arena_lock();

	linkr_debugger_capture_arena_current_owner =
		LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE;
	linkr_debugger_capture_arena_current_state =
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_IDLE;
	linkr_debugger_capture_arena_current_session_id = 0U;
	linkr_debugger_capture_arena_resume_armed = false;
	linkr_debugger_capture_arena_release_in_progress = false;
	linkr_debugger_capture_arena_generation++;
	linkr_debugger_capture_arena_fill_canary(
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_SOURCE_CANARY_OFFSET);
	linkr_debugger_capture_arena_fill_canary(
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TAIL_CANARY_OFFSET);

	linkr_debugger_capture_arena_unlock(key);
}

void linkr_debugger_capture_arena_register_quiesce_ops(
	const struct linkr_debugger_capture_arena_quiesce_ops *ops)
{
	unsigned int key = linkr_debugger_capture_arena_lock();

	if (ops == NULL) {
		memset(&linkr_debugger_capture_arena_quiesce_ops, 0,
			sizeof(linkr_debugger_capture_arena_quiesce_ops));
	} else {
		linkr_debugger_capture_arena_quiesce_ops = *ops;
	}

	linkr_debugger_capture_arena_unlock(key);
}

static void linkr_debugger_capture_arena_resume_if_needed(bool should_resume)
{
	struct linkr_debugger_capture_arena_quiesce_ops ops;

	if (!should_resume) {
		return;
	}

	ops = linkr_debugger_capture_arena_quiesce_ops;
	if (ops.resume != NULL) {
		ops.resume(ops.user_data);
	}
}

static void linkr_debugger_capture_arena_clear_owner_locked(void)
{
	linkr_debugger_capture_arena_current_owner =
		LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE;
	linkr_debugger_capture_arena_current_state =
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_IDLE;
	linkr_debugger_capture_arena_current_session_id = 0U;
	linkr_debugger_capture_arena_resume_armed = false;
	linkr_debugger_capture_arena_release_in_progress = false;
	linkr_debugger_capture_arena_generation++;
	linkr_debugger_capture_arena_fill_canary(
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_SOURCE_CANARY_OFFSET);
	linkr_debugger_capture_arena_fill_canary(
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TAIL_CANARY_OFFSET);
}

static bool linkr_debugger_capture_arena_begin_release_locked(
	const struct linkr_debugger_capture_arena_lease *lease,
	bool resume_requested, bool *should_resume)
{
	if (!linkr_debugger_capture_arena_lease_matches_locked(lease) ||
	    linkr_debugger_capture_arena_release_in_progress) {
		return false;
	}

	linkr_debugger_capture_arena_release_in_progress = true;
	linkr_debugger_capture_arena_resume_armed = false;
	*should_resume = resume_requested;
	return true;
}

static void linkr_debugger_capture_arena_finish_release(
	const struct linkr_debugger_capture_arena_lease *lease,
	bool should_resume)
{
	unsigned int key;

	linkr_debugger_capture_arena_resume_if_needed(should_resume);

	key = linkr_debugger_capture_arena_lock();
	if (linkr_debugger_capture_arena_release_in_progress &&
	    linkr_debugger_capture_arena_lease_matches_locked(lease)) {
		linkr_debugger_capture_arena_clear_owner_locked();
	}
	linkr_debugger_capture_arena_unlock(key);
}

uint8_t *linkr_debugger_capture_arena_base(void)
{
	return linkr_debugger_capture_arena_storage();
}

size_t linkr_debugger_capture_arena_size(void)
{
	return LINKR_DEBUGGER_CAPTURE_ARENA_BYTES;
}

void *linkr_debugger_capture_arena_region(size_t offset, size_t size)
{
	if (offset > LINKR_DEBUGGER_CAPTURE_ARENA_BYTES ||
	    size > LINKR_DEBUGGER_CAPTURE_ARENA_BYTES - offset) {
		return NULL;
	}

	return &linkr_debugger_capture_arena_storage()[offset];
}

uint32_t *linkr_debugger_capture_arena_la_packed_ring(void)
{
	return linkr_debugger_capture_arena_region(
		LINKR_DEBUGGER_CAPTURE_ARENA_LA_PACKED_RING_OFFSET,
		LINKR_DEBUGGER_CAPTURE_ARENA_LA_PACKED_RING_BYTES);
}

void *linkr_debugger_capture_arena_la_finite_samples(void)
{
	return linkr_debugger_capture_arena_region(
		LINKR_DEBUGGER_CAPTURE_ARENA_LA_FINITE_SAMPLES_OFFSET,
		LINKR_DEBUGGER_CAPTURE_ARENA_LA_FINITE_SAMPLES_BYTES);
}

uint16_t *linkr_debugger_capture_arena_la_scratch(void)
{
	return linkr_debugger_capture_arena_region(
		LINKR_DEBUGGER_CAPTURE_ARENA_LA_SCRATCH_OFFSET,
		LINKR_DEBUGGER_CAPTURE_ARENA_LA_SCRATCH_BYTES);
}

uint16_t *linkr_debugger_capture_arena_la_pre_trigger(void)
{
	return linkr_debugger_capture_arena_region(
		LINKR_DEBUGGER_CAPTURE_ARENA_LA_PRE_TRIGGER_OFFSET,
		LINKR_DEBUGGER_CAPTURE_ARENA_LA_PRE_TRIGGER_BYTES);
}

void *linkr_debugger_capture_arena_ws_sample_ring(void)
{
	return linkr_debugger_capture_arena_region(
		LINKR_DEBUGGER_CAPTURE_ARENA_WS_SAMPLE_RING_OFFSET,
		LINKR_DEBUGGER_CAPTURE_ARENA_WS_SAMPLE_RING_BYTES);
}

void *linkr_debugger_capture_arena_power_capture(void)
{
	return linkr_debugger_capture_arena_region(
		LINKR_DEBUGGER_CAPTURE_ARENA_POWER_CAPTURE_OFFSET,
		LINKR_DEBUGGER_CAPTURE_ARENA_POWER_CAPTURE_BYTES);
}

void *linkr_debugger_capture_arena_sigrok_ws_pool(void)
{
	return linkr_debugger_capture_arena_region(
		LINKR_DEBUGGER_CAPTURE_ARENA_SIGROK_WS_POOL_OFFSET,
		LINKR_DEBUGGER_CAPTURE_ARENA_SIGROK_WS_POOL_BYTES);
}

uint8_t *linkr_debugger_capture_arena_burst_lane_a(void)
{
	return linkr_debugger_capture_arena_region(
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_A_OFFSET,
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_A_BYTES);
}

uint8_t *linkr_debugger_capture_arena_burst_lane_b(void)
{
	return linkr_debugger_capture_arena_region(
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_B_OFFSET,
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_B_BYTES);
}

uint8_t *linkr_debugger_capture_arena_burst_tx_slot(uint8_t slot)
{
	size_t offset;

	if (slot == 0U) {
		offset = LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TX0_OFFSET;
	} else if (slot == 1U) {
		offset = LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TX1_OFFSET;
	} else {
		return NULL;
	}

	return linkr_debugger_capture_arena_region(offset,
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TX_SLOT_BYTES);
}

uint8_t *linkr_debugger_capture_arena_burst_terminal(void)
{
	return linkr_debugger_capture_arena_region(
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TERMINAL_OFFSET,
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TERMINAL_BYTES);
}

bool linkr_debugger_capture_arena_try_acquire_wide11(
	uint32_t session_id, struct linkr_debugger_capture_arena_lease *lease)
{
	bool acquired = false;
	unsigned int key;

	if (session_id == 0U || lease == NULL) {
		return false;
	}

	key = linkr_debugger_capture_arena_lock();
	if (linkr_debugger_capture_arena_current_owner ==
	    LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE &&
	    linkr_debugger_capture_arena_current_state ==
	    LINKR_DEBUGGER_CAPTURE_ARENA_STATE_IDLE &&
	    linkr_debugger_capture_arena_canaries_ok()) {
		linkr_debugger_capture_arena_generation++;
		linkr_debugger_capture_arena_current_owner =
			LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_WIDE11_BURST;
		linkr_debugger_capture_arena_current_state =
			LINKR_DEBUGGER_CAPTURE_ARENA_STATE_ACQUIRED;
		linkr_debugger_capture_arena_current_session_id = session_id;
		linkr_debugger_capture_arena_resume_armed = false;
		lease->owner = linkr_debugger_capture_arena_current_owner;
		lease->session_id = session_id;
		lease->generation = linkr_debugger_capture_arena_generation;
		acquired = true;
	}
	linkr_debugger_capture_arena_unlock(key);

	if (!acquired) {
		memset(lease, 0, sizeof(*lease));
	}

	return acquired;
}

int linkr_debugger_capture_arena_try_acquire_wide11_quiesced(
	uint32_t session_id, int32_t timeout_ms,
	struct linkr_debugger_capture_arena_lease *lease)
{
	struct linkr_debugger_capture_arena_quiesce_ops ops;
	struct linkr_debugger_capture_arena_lease reserved;
	bool hook_called = false;
	bool release_started;
	bool should_resume = false;
	unsigned int key;
	int ret = 0;

	if (session_id == 0U || lease == NULL) {
		return -EINVAL;
	}
	memset(lease, 0, sizeof(*lease));
	memset(&reserved, 0, sizeof(reserved));

	key = linkr_debugger_capture_arena_lock();
	if (linkr_debugger_capture_arena_current_owner !=
	    LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE ||
	    linkr_debugger_capture_arena_current_state !=
	    LINKR_DEBUGGER_CAPTURE_ARENA_STATE_IDLE) {
		linkr_debugger_capture_arena_unlock(key);
		return -EBUSY;
	}

	linkr_debugger_capture_arena_generation++;
	linkr_debugger_capture_arena_current_owner =
		LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_WIDE11_BURST;
	linkr_debugger_capture_arena_current_state =
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_QUIESCING;
	linkr_debugger_capture_arena_current_session_id = session_id;
	linkr_debugger_capture_arena_resume_armed = false;
	reserved.owner = linkr_debugger_capture_arena_current_owner;
	reserved.session_id = session_id;
	reserved.generation = linkr_debugger_capture_arena_generation;
	ops = linkr_debugger_capture_arena_quiesce_ops;
	linkr_debugger_capture_arena_unlock(key);

	if (ops.quiesce != NULL) {
		hook_called = true;
		ret = ops.quiesce(timeout_ms, ops.user_data);
	}

	key = linkr_debugger_capture_arena_lock();
	if (ret == 0 && linkr_debugger_capture_arena_lease_matches_locked(&reserved) &&
	    linkr_debugger_capture_arena_current_state ==
	    LINKR_DEBUGGER_CAPTURE_ARENA_STATE_QUIESCING &&
	    linkr_debugger_capture_arena_refill_canaries_if_clean()) {
		linkr_debugger_capture_arena_current_state =
			LINKR_DEBUGGER_CAPTURE_ARENA_STATE_ACQUIRED;
		linkr_debugger_capture_arena_resume_armed = hook_called;
		*lease = reserved;
		linkr_debugger_capture_arena_unlock(key);
		return 0;
	}

	if (ret == 0) {
		ret = linkr_debugger_capture_arena_canaries_ok() ? -ESTALE : -EIO;
	}
	release_started = linkr_debugger_capture_arena_begin_release_locked(
		&reserved, hook_called, &should_resume);
	linkr_debugger_capture_arena_unlock(key);

	if (release_started) {
		linkr_debugger_capture_arena_finish_release(&reserved, should_resume);
	}
	return ret;
}

int linkr_debugger_capture_arena_mark_armed(
	const struct linkr_debugger_capture_arena_lease *lease)
{
	return linkr_debugger_capture_arena_mark(lease,
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_ACQUIRED,
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_ARMED);
}

int linkr_debugger_capture_arena_mark_dma_active(
	const struct linkr_debugger_capture_arena_lease *lease)
{
	return linkr_debugger_capture_arena_mark(lease,
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_ARMED,
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_DMA_ACTIVE);
}

int linkr_debugger_capture_arena_mark_postprocess(
	const struct linkr_debugger_capture_arena_lease *lease)
{
	return linkr_debugger_capture_arena_mark(lease,
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_DMA_ACTIVE,
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_POSTPROCESS);
}

int linkr_debugger_capture_arena_mark_network_send(
	const struct linkr_debugger_capture_arena_lease *lease)
{
	return linkr_debugger_capture_arena_mark(lease,
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_POSTPROCESS,
		LINKR_DEBUGGER_CAPTURE_ARENA_STATE_NETWORK_SEND);
}

bool linkr_debugger_capture_arena_release(
	const struct linkr_debugger_capture_arena_lease *lease)
{
	bool released;
	bool should_resume = false;
	unsigned int key = linkr_debugger_capture_arena_lock();

	released = linkr_debugger_capture_arena_begin_release_locked(
		lease, linkr_debugger_capture_arena_resume_armed, &should_resume);

	linkr_debugger_capture_arena_unlock(key);
	if (released) {
		linkr_debugger_capture_arena_finish_release(lease, should_resume);
	}
	return released;
}

enum linkr_debugger_capture_arena_state linkr_debugger_capture_arena_state(void)
{
	enum linkr_debugger_capture_arena_state state;
	unsigned int key = linkr_debugger_capture_arena_lock();

	state = linkr_debugger_capture_arena_current_state;
	linkr_debugger_capture_arena_unlock(key);
	return state;
}

enum linkr_debugger_capture_arena_owner linkr_debugger_capture_arena_owner(void)
{
	enum linkr_debugger_capture_arena_owner owner;
	unsigned int key = linkr_debugger_capture_arena_lock();

	owner = linkr_debugger_capture_arena_current_owner;
	linkr_debugger_capture_arena_unlock(key);
	return owner;
}

bool linkr_debugger_capture_arena_canaries_ok(void)
{
	return linkr_debugger_capture_arena_check_canary(
		LINKR_DEBUGGER_CAPTURE_ARENA_BURST_SOURCE_CANARY_OFFSET) &&
		linkr_debugger_capture_arena_check_canary(
			LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TAIL_CANARY_OFFSET);
}


bool linkr_debugger_capture_arena_ws_quiesce_allows_overwrite(
	bool power_capture_active,
	bool adc_pause_acked,
	bool sample_ring_busy,
	bool sigrok_normal_pool_all_free)
{
	return !power_capture_active && adc_pause_acked && !sample_ring_busy &&
		sigrok_normal_pool_all_free;
}

bool linkr_debugger_capture_arena_ws_sample_read_allowed(
	bool adc_pause_requested,
	bool arena_quiesced)
{
	return !adc_pause_requested && !arena_quiesced;
}


static bool linkr_debugger_capture_arena_burst_slot_model_matches(
	const struct linkr_debugger_capture_arena_burst_slot_model *model,
	uint32_t session_id, uint32_t generation)
{
	return model != NULL && model->active && session_id != 0U && generation != 0U &&
		model->owner_session_id == session_id &&
		model->owner_generation == generation;
}

void linkr_debugger_capture_arena_burst_slot_model_begin(
	struct linkr_debugger_capture_arena_burst_slot_model *model,
	uint32_t session_id, uint32_t generation)
{
	if (model == NULL) {
		return;
	}
	memset(model, 0, sizeof(*model));
	if (session_id == 0U || generation == 0U) {
		return;
	}
	model->active = true;
	model->owner_session_id = session_id;
	model->owner_generation = generation;
}

bool linkr_debugger_capture_arena_burst_slot_model_acquire_data(
	struct linkr_debugger_capture_arena_burst_slot_model *model,
	uint32_t session_id, uint32_t generation)
{
	if (!linkr_debugger_capture_arena_burst_slot_model_matches(model,
	    session_id, generation) || model->data_in_use >= 2U ||
	    model->terminal_in_use != 0U) {
		return false;
	}
	model->data_in_use++;
	return true;
}

bool linkr_debugger_capture_arena_burst_slot_model_acquire_terminal(
	struct linkr_debugger_capture_arena_burst_slot_model *model,
	uint32_t session_id, uint32_t generation)
{
	if (!linkr_debugger_capture_arena_burst_slot_model_matches(model,
	    session_id, generation) || model->terminal_in_use != 0U ||
	    model->data_in_use != 0U) {
		return false;
	}
	model->terminal_in_use = 1U;
	return true;
}

void linkr_debugger_capture_arena_burst_slot_model_release_data(
	struct linkr_debugger_capture_arena_burst_slot_model *model)
{
	if (model != NULL && model->data_in_use > 0U) {
		model->data_in_use--;
	}
}

void linkr_debugger_capture_arena_burst_slot_model_release_terminal(
	struct linkr_debugger_capture_arena_burst_slot_model *model)
{
	if (model != NULL) {
		model->terminal_in_use = 0U;
	}
}

void linkr_debugger_capture_arena_burst_slot_model_mark_source_done(
	struct linkr_debugger_capture_arena_burst_slot_model *model,
	uint32_t session_id, uint32_t generation)
{
	if (linkr_debugger_capture_arena_burst_slot_model_matches(model,
	    session_id, generation)) {
		model->source_decode_complete = true;
	}
}

bool linkr_debugger_capture_arena_burst_slot_model_drained(
	const struct linkr_debugger_capture_arena_burst_slot_model *model)
{
	return model != NULL && model->active && model->source_decode_complete &&
		model->data_in_use == 0U && model->terminal_in_use == 0U;
}

void linkr_debugger_capture_arena_burst_slot_model_abort(
	struct linkr_debugger_capture_arena_burst_slot_model *model,
	uint32_t session_id, uint32_t generation)
{
	if (linkr_debugger_capture_arena_burst_slot_model_matches(model,
	    session_id, generation)) {
		memset(model, 0, sizeof(*model));
	}
}
