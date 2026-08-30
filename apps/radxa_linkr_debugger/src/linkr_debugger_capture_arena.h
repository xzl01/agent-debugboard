/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_CAPTURE_ARENA_H_
#define RADXA_LINKR_DEBUGGER_CAPTURE_ARENA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINKR_DEBUGGER_CAPTURE_ARENA_ALIGN 32768U

#define LINKR_DEBUGGER_CAPTURE_ARENA_LA_PACKED_RING_OFFSET 0U
#define LINKR_DEBUGGER_CAPTURE_ARENA_LA_PACKED_RING_BYTES 32768U
#define LINKR_DEBUGGER_CAPTURE_ARENA_LA_FINITE_SAMPLES_OFFSET 2048U
#define LINKR_DEBUGGER_CAPTURE_ARENA_LA_FINITE_SAMPLES_BYTES 4096U
#define LINKR_DEBUGGER_CAPTURE_ARENA_LA_SCRATCH_OFFSET 32768U
#define LINKR_DEBUGGER_CAPTURE_ARENA_LA_SCRATCH_BYTES 2048U
#define LINKR_DEBUGGER_CAPTURE_ARENA_LA_PRE_TRIGGER_OFFSET 34816U
#define LINKR_DEBUGGER_CAPTURE_ARENA_LA_PRE_TRIGGER_BYTES 1024U
#define LINKR_DEBUGGER_CAPTURE_ARENA_WS_SAMPLE_RING_OFFSET \
	(LINKR_DEBUGGER_CAPTURE_ARENA_LA_PRE_TRIGGER_OFFSET + \
	 LINKR_DEBUGGER_CAPTURE_ARENA_LA_PRE_TRIGGER_BYTES)
#define LINKR_DEBUGGER_CAPTURE_ARENA_WS_SAMPLE_RING_BYTES 30720U
#define LINKR_DEBUGGER_CAPTURE_ARENA_POWER_CAPTURE_OFFSET \
	(LINKR_DEBUGGER_CAPTURE_ARENA_WS_SAMPLE_RING_OFFSET + \
	 LINKR_DEBUGGER_CAPTURE_ARENA_WS_SAMPLE_RING_BYTES)
#define LINKR_DEBUGGER_CAPTURE_ARENA_POWER_CAPTURE_BYTES 65672U
#define LINKR_DEBUGGER_CAPTURE_ARENA_SIGROK_WS_POOL_OFFSET \
	(LINKR_DEBUGGER_CAPTURE_ARENA_POWER_CAPTURE_OFFSET + \
	 LINKR_DEBUGGER_CAPTURE_ARENA_POWER_CAPTURE_BYTES)
#define LINKR_DEBUGGER_CAPTURE_ARENA_SIGROK_WS_POOL_BYTES 16624U
#define LINKR_DEBUGGER_CAPTURE_ARENA_NORMAL_BYTES \
	(LINKR_DEBUGGER_CAPTURE_ARENA_SIGROK_WS_POOL_OFFSET + \
	 LINKR_DEBUGGER_CAPTURE_ARENA_SIGROK_WS_POOL_BYTES)

#define LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_A_OFFSET 0U
#define LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_A_BYTES 100000U
#define LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_B_OFFSET 100000U
#define LINKR_DEBUGGER_CAPTURE_ARENA_BURST_LANE_B_BYTES 40000U
#define LINKR_DEBUGGER_CAPTURE_ARENA_CANARY_BYTES 16U
#define LINKR_DEBUGGER_CAPTURE_ARENA_BURST_SOURCE_CANARY_OFFSET 140000U
#define LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TX0_OFFSET 140016U
#define LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TX_SLOT_BYTES 2068U
#define LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TX1_OFFSET 142084U
#define LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TERMINAL_OFFSET 144152U
#define LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TERMINAL_BYTES 16U
#define LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TAIL_CANARY_OFFSET 144168U
#define LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TOTAL_BYTES 144184U
#define LINKR_DEBUGGER_CAPTURE_ARENA_BYTES \
	((LINKR_DEBUGGER_CAPTURE_ARENA_NORMAL_BYTES > \
	  LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TOTAL_BYTES) ? \
		 LINKR_DEBUGGER_CAPTURE_ARENA_NORMAL_BYTES : \
		 LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TOTAL_BYTES)

enum linkr_debugger_capture_arena_owner {
	LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE = 0,
	LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_WIDE11_BURST,
};

enum linkr_debugger_capture_arena_state {
	LINKR_DEBUGGER_CAPTURE_ARENA_STATE_IDLE = 0,
	LINKR_DEBUGGER_CAPTURE_ARENA_STATE_QUIESCING,
	LINKR_DEBUGGER_CAPTURE_ARENA_STATE_ACQUIRED,
	LINKR_DEBUGGER_CAPTURE_ARENA_STATE_ARMED,
	LINKR_DEBUGGER_CAPTURE_ARENA_STATE_DMA_ACTIVE,
	LINKR_DEBUGGER_CAPTURE_ARENA_STATE_POSTPROCESS,
	LINKR_DEBUGGER_CAPTURE_ARENA_STATE_NETWORK_SEND,
};

struct linkr_debugger_capture_arena_lease {
	enum linkr_debugger_capture_arena_owner owner;
	uint32_t session_id;
	uint32_t generation;
};

struct linkr_debugger_capture_arena_quiesce_ops {
	int (*quiesce)(int32_t timeout_ms, void *user_data);
	void (*resume)(void *user_data);
	void *user_data;
};

void linkr_debugger_capture_arena_init(void);
uint8_t *linkr_debugger_capture_arena_base(void);
size_t linkr_debugger_capture_arena_size(void);
void *linkr_debugger_capture_arena_region(size_t offset, size_t size);

uint32_t *linkr_debugger_capture_arena_la_packed_ring(void);
void *linkr_debugger_capture_arena_la_finite_samples(void);
uint16_t *linkr_debugger_capture_arena_la_scratch(void);
uint16_t *linkr_debugger_capture_arena_la_pre_trigger(void);
void *linkr_debugger_capture_arena_ws_sample_ring(void);
void *linkr_debugger_capture_arena_power_capture(void);
void *linkr_debugger_capture_arena_sigrok_ws_pool(void);

uint8_t *linkr_debugger_capture_arena_burst_lane_a(void);
uint8_t *linkr_debugger_capture_arena_burst_lane_b(void);
uint8_t *linkr_debugger_capture_arena_burst_tx_slot(uint8_t slot);
uint8_t *linkr_debugger_capture_arena_burst_terminal(void);

void linkr_debugger_capture_arena_register_quiesce_ops(
	const struct linkr_debugger_capture_arena_quiesce_ops *ops);
bool linkr_debugger_capture_arena_try_acquire_wide11(
	uint32_t session_id, struct linkr_debugger_capture_arena_lease *lease);
int linkr_debugger_capture_arena_try_acquire_wide11_quiesced(
	uint32_t session_id, int32_t timeout_ms,
	struct linkr_debugger_capture_arena_lease *lease);
int linkr_debugger_capture_arena_mark_armed(
	const struct linkr_debugger_capture_arena_lease *lease);
int linkr_debugger_capture_arena_mark_dma_active(
	const struct linkr_debugger_capture_arena_lease *lease);
int linkr_debugger_capture_arena_mark_postprocess(
	const struct linkr_debugger_capture_arena_lease *lease);
int linkr_debugger_capture_arena_mark_network_send(
	const struct linkr_debugger_capture_arena_lease *lease);
bool linkr_debugger_capture_arena_release(
	const struct linkr_debugger_capture_arena_lease *lease);
enum linkr_debugger_capture_arena_state linkr_debugger_capture_arena_state(void);
enum linkr_debugger_capture_arena_owner linkr_debugger_capture_arena_owner(void);
bool linkr_debugger_capture_arena_canaries_ok(void);
bool linkr_debugger_capture_arena_ws_quiesce_allows_overwrite(
	bool power_capture_active,
	bool adc_pause_acked,
	bool sample_ring_busy,
	bool sigrok_normal_pool_all_free);
bool linkr_debugger_capture_arena_ws_sample_read_allowed(
	bool adc_pause_requested,
	bool arena_quiesced);

struct linkr_debugger_capture_arena_burst_slot_model {
	uint32_t owner_session_id;
	uint32_t owner_generation;
	uint8_t data_in_use;
	uint8_t terminal_in_use;
	bool active;
	bool source_decode_complete;
};

void linkr_debugger_capture_arena_burst_slot_model_begin(
	struct linkr_debugger_capture_arena_burst_slot_model *model,
	uint32_t session_id, uint32_t generation);
bool linkr_debugger_capture_arena_burst_slot_model_acquire_data(
	struct linkr_debugger_capture_arena_burst_slot_model *model,
	uint32_t session_id, uint32_t generation);
bool linkr_debugger_capture_arena_burst_slot_model_acquire_terminal(
	struct linkr_debugger_capture_arena_burst_slot_model *model,
	uint32_t session_id, uint32_t generation);
void linkr_debugger_capture_arena_burst_slot_model_release_data(
	struct linkr_debugger_capture_arena_burst_slot_model *model);
void linkr_debugger_capture_arena_burst_slot_model_release_terminal(
	struct linkr_debugger_capture_arena_burst_slot_model *model);
void linkr_debugger_capture_arena_burst_slot_model_mark_source_done(
	struct linkr_debugger_capture_arena_burst_slot_model *model,
	uint32_t session_id, uint32_t generation);
bool linkr_debugger_capture_arena_burst_slot_model_drained(
	const struct linkr_debugger_capture_arena_burst_slot_model *model);
void linkr_debugger_capture_arena_burst_slot_model_abort(
	struct linkr_debugger_capture_arena_burst_slot_model *model,
	uint32_t session_id, uint32_t generation);

#endif /* RADXA_LINKR_DEBUGGER_CAPTURE_ARENA_H_ */
