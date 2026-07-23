/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_CAPTURE_ENGINE_H_
#define RADXA_LINKR_DEBUGGER_CAPTURE_ENGINE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINKR_DEBUGGER_CAPTURE_ENGINE_ARENA_BYTES 100000U
#define LINKR_DEBUGGER_CAPTURE_ENGINE_LA_RING_BYTES 80000U
#define LINKR_DEBUGGER_CAPTURE_ENGINE_LA_RING_SAMPLES \
	(LINKR_DEBUGGER_CAPTURE_ENGINE_LA_RING_BYTES / sizeof(uint32_t))
#define LINKR_DEBUGGER_CAPTURE_ENGINE_LA_EXPORT_OFFSET \
	LINKR_DEBUGGER_CAPTURE_ENGINE_LA_RING_BYTES
#define LINKR_DEBUGGER_CAPTURE_ENGINE_FAST_BASE_PIN 10U
#define LINKR_DEBUGGER_CAPTURE_ENGINE_FAST_PIN_COUNT 8U
#define LINKR_DEBUGGER_CAPTURE_ENGINE_WIDE_BASE_PIN 10U
#define LINKR_DEBUGGER_CAPTURE_ENGINE_WIDE_LINEAR_PIN_COUNT 11U
#define LINKR_DEBUGGER_CAPTURE_ENGINE_WIDE_EXTRA_PIN 29U
#define LINKR_DEBUGGER_CAPTURE_ENGINE_FAST_MAX_RATE_HZ 125000000U
#define LINKR_DEBUGGER_CAPTURE_ENGINE_WIDE_MAX_RATE_HZ 25000000U
#define LINKR_DEBUGGER_CAPTURE_ENGINE_SPARSE_MAX_RATE_HZ 25000000U

enum linkr_debugger_capture_engine_backend {
	LINKR_DEBUGGER_CAPTURE_ENGINE_BACKEND_INVALID = 0,
	LINKR_DEBUGGER_CAPTURE_ENGINE_BACKEND_FAST8,
	LINKR_DEBUGGER_CAPTURE_ENGINE_BACKEND_WIDE16,
	LINKR_DEBUGGER_CAPTURE_ENGINE_BACKEND_SPARSE16,
};

struct linkr_debugger_capture_engine_layout {
	enum linkr_debugger_capture_engine_backend backend;
	const char *name;
	uint8_t bytes_per_sample;
	uint8_t storage_bytes_per_sample;
	uint8_t channel_count;
	uint32_t max_rate_hz;
	uint32_t capacity_samples;
};

struct linkr_debugger_capture_engine_window {
	uint32_t start_index;
	uint32_t sample_count;
	uint32_t trigger_index;
};

enum linkr_debugger_logic_session_state {
	LINKR_DEBUGGER_LOGIC_SESSION_IDLE = 0,
	LINKR_DEBUGGER_LOGIC_SESSION_ARMED_PRETRIGGER,
	LINKR_DEBUGGER_LOGIC_SESSION_LIVE_UNTRIGGERED,
	LINKR_DEBUGGER_LOGIC_SESSION_TRIGGER_DRAIN,
	LINKR_DEBUGGER_LOGIC_SESSION_LIVE_TRIGGERED,
	LINKR_DEBUGGER_LOGIC_SESSION_STOPPING,
	LINKR_DEBUGGER_LOGIC_SESSION_ERROR,
};

enum linkr_debugger_logic_session_owner {
	LINKR_DEBUGGER_LOGIC_SESSION_OWNER_NONE = 0,
	LINKR_DEBUGGER_LOGIC_SESSION_OWNER_WS,
	LINKR_DEBUGGER_LOGIC_SESSION_OWNER_BEAGLELOGIC,
};

enum linkr_debugger_logic_session_trigger {
	LINKR_DEBUGGER_LOGIC_SESSION_TRIGGER_NONE = 0,
	LINKR_DEBUGGER_LOGIC_SESSION_TRIGGER_EDGE,
};

struct linkr_debugger_logic_session_config {
	enum linkr_debugger_logic_session_owner owner;
	enum linkr_debugger_logic_session_trigger trigger;
	uint32_t session_id;
	uint32_t pre_samples;
	uint32_t ring_samples;
	uint8_t bytes_per_sample;
	const char *backend;
};

struct linkr_debugger_logic_session_chunk {
	uint64_t first_seq;
	uint32_t sample_count;
	uint32_t trigger_offset;
	bool has_trigger;
	const char *encoding;
};

struct linkr_debugger_logic_session_status {
	enum linkr_debugger_logic_session_state state;
	enum linkr_debugger_logic_session_owner owner;
	uint32_t session_id;
	uint64_t reader_seq;
	uint64_t eligible_seq;
	uint64_t writer_seq;
	uint64_t trigger_seq;
	uint64_t last_sent_seq;
	uint64_t last_captured_seq;
	uint32_t pre_samples;
	uint32_t ring_samples;
	uint8_t bytes_per_sample;
	bool triggered;
	bool terminal;
	bool overrun;
	const char *backend;
	const char *reason;
};

uint8_t *linkr_debugger_capture_engine_arena(void);
uint32_t linkr_debugger_capture_engine_arena_size(void);

int linkr_debugger_capture_engine_select_backend(const uint8_t *pins, uint8_t pin_count,
	struct linkr_debugger_capture_engine_layout *layout);
const char *linkr_debugger_capture_engine_backend_name(
	enum linkr_debugger_capture_engine_backend backend);
uint32_t linkr_debugger_capture_engine_capacity_samples(uint8_t bytes_per_sample);
uint32_t linkr_debugger_capture_engine_sample_bytes(uint32_t sample_count,
	uint8_t bytes_per_sample);
int linkr_debugger_capture_engine_plan_window(uint32_t write_index, uint32_t capacity_samples,
	uint32_t pre_samples, uint32_t post_samples,
	struct linkr_debugger_capture_engine_window *window);
int linkr_debugger_capture_engine_pack_u16_le(uint8_t *dst, size_t dst_len,
	const uint16_t *samples, uint32_t sample_count);
int linkr_debugger_capture_engine_export_marked_window(uint8_t *dst, size_t dst_len,
	const uint32_t *ring, uint32_t ring_samples, uint32_t final_write_index,
	uint32_t pre_samples, uint32_t post_samples, uint8_t bytes_per_sample,
	uint32_t sample_mask, uint32_t *sample_bytes);

void linkr_debugger_logic_session_init(void);
int linkr_debugger_logic_session_start(
	const struct linkr_debugger_logic_session_config *config);
int linkr_debugger_logic_session_commit(uint32_t sample_count, bool trigger_seen,
	uint32_t trigger_offset);
int linkr_debugger_logic_session_stop(enum linkr_debugger_logic_session_owner owner,
	uint32_t session_id, const char *reason);
int linkr_debugger_logic_session_disconnect(enum linkr_debugger_logic_session_owner owner,
	uint32_t session_id);
int linkr_debugger_logic_session_read(uint8_t *dst, size_t dst_len, uint32_t max_samples,
	struct linkr_debugger_logic_session_chunk *chunk);
int linkr_debugger_logic_session_consume(uint32_t max_samples,
	struct linkr_debugger_logic_session_chunk *chunk);
void linkr_debugger_logic_session_get_status(
	struct linkr_debugger_logic_session_status *status);
const char *linkr_debugger_logic_session_state_name(
	enum linkr_debugger_logic_session_state state);

#endif /* RADXA_LINKR_DEBUGGER_CAPTURE_ENGINE_H_ */
