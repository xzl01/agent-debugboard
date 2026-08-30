/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_LOGIC_ANALYZER_H_
#define RADXA_LINKR_DEBUGGER_LOGIC_ANALYZER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "linkr_debugger_capture_arena.h"

#define LINKR_DEBUGGER_LA_MAX_CHANNELS 16
#define LINKR_DEBUGGER_LA_DEFAULT_BUFFER_SIZE (32 * 1024)
#define LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES 512
#define LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES 1024
#define LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES \
	(2U * LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES)
#define LINKR_DEBUGGER_LA_STREAM_MAX_PACKED_CHUNK_SAMPLES \
	(4U * LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES)
#define LINKR_DEBUGGER_LA_STREAM_MAX_SINGLE_BITS_CHUNK_SAMPLES \
	(8U * LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES)
#define LINKR_DEBUGGER_LA_STREAM_HANDOFF_UNREAD_SAMPLES \
	LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES
#define LINKR_DEBUGGER_LA_MIN_SAMPLE_RATE_HZ 100000U
#define LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ 125000000U
#define LINKR_DEBUGGER_LA_MIN_PRE_TRIGGER_SAMPLE_RATE_HZ 1000000U
#define LINKR_DEBUGGER_LA_MAX_PRE_TRIGGER_SAMPLE_RATE_HZ 25000000U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES 100000U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_BITS_PER_SAMPLE 8U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_BITS_PER_SAMPLE 3U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_AUTOPUSH_BITS 32U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_AUTOPUSH_BITS 30U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_A_SAMPLES_PER_WORD 4U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_LANE_B_SAMPLES_PER_WORD 10U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_SAMPLE_ALIGNMENT 20U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_GP10_20_MASK 0x07ffU
#define LINKR_DEBUGGER_LA_WIDE11_BURST_SAMPLE_MASK 0x07ffU
#define LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_LANE_A_WORDS 25000U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_LANE_B_WORDS 10000U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_LANE_A_BYTES 100000U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_LANE_B_BYTES 40000U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_TOTAL_BYTES 140000U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ 100000000U
#define LINKR_DEBUGGER_LA_WIDE11_BURST_PACKED_SAMPLE_BYTES 2U
#define LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES 100000U
#define LINKR_DEBUGGER_LA_PACKED_BURST_CONTINUOUS_SAMPLES \
	LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES
#define LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES 2U

enum linkr_debugger_la_trigger_type {
	LINKR_DEBUGGER_LA_TRIGGER_NONE = 0,
	LINKR_DEBUGGER_LA_TRIGGER_RISING,
	LINKR_DEBUGGER_LA_TRIGGER_FALLING,
	LINKR_DEBUGGER_LA_TRIGGER_EITHER,
};

enum linkr_debugger_la_stop_policy {
	LINKR_DEBUGGER_LA_STOP_POLICY_BOUNDED = 0,
	LINKR_DEBUGGER_LA_STOP_POLICY_CONTINUOUS,
};

enum linkr_debugger_la_stop_reason {
	LINKR_DEBUGGER_LA_STOP_REASON_COMPLETE = 0,
	LINKR_DEBUGGER_LA_STOP_REASON_HOST_STOP,
	LINKR_DEBUGGER_LA_STOP_REASON_FREEZE_BEFORE_OVERWRITE,
	LINKR_DEBUGGER_LA_STOP_REASON_TRANSPORT_PRESSURE,
	LINKR_DEBUGGER_LA_STOP_REASON_DMA_ERROR,
	LINKR_DEBUGGER_LA_STOP_REASON_UNSUPPORTED,
};

	enum linkr_debugger_la_hardware_plan_kind {
	LINKR_DEBUGGER_LA_HARDWARE_PLAN_UNSUPPORTED = 0,
	LINKR_DEBUGGER_LA_HARDWARE_PLAN_SINGLE_PACKED,
	LINKR_DEBUGGER_LA_HARDWARE_PLAN_FAST8_PACKED,
	LINKR_DEBUGGER_LA_HARDWARE_PLAN_WIDE11_SPLIT_PACKED,
};

enum linkr_debugger_la_legacy_adapter {
	LINKR_DEBUGGER_LA_LEGACY_ADAPTER_NONE = 0,
	LINKR_DEBUGGER_LA_LEGACY_ADAPTER_RING_STREAM,
	LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST,
};

enum linkr_debugger_la_pipeline_family {
	LINKR_DEBUGGER_LA_PIPELINE_FAMILY_UNSUPPORTED = 0,
	LINKR_DEBUGGER_LA_PIPELINE_FAMILY_COMMON_PACKED,
};

struct linkr_debugger_la_config {
	uint8_t pin_base;
	uint8_t pin_count;
	uint8_t selected_pins[LINKR_DEBUGGER_LA_MAX_CHANNELS];
	uint8_t selected_pin_count;
	uint32_t sample_rate_hz;
	uint32_t pre_samples;
	uint32_t post_samples;
	enum linkr_debugger_la_trigger_type trigger;
	uint8_t trigger_pin;
};

struct linkr_debugger_la_session_contract {
	enum linkr_debugger_la_trigger_type trigger_gate;
	enum linkr_debugger_la_stop_policy stop_policy;
	uint32_t pre_samples;
	uint32_t post_samples;
};

struct linkr_debugger_la_pre_trigger_window {
	uint64_t first_sequence;
	uint64_t end_sequence;
	uint32_t sample_count;
	uint32_t trigger_index;
};

struct linkr_debugger_la_hardware_plan {
	enum linkr_debugger_la_hardware_plan_kind kind;
	enum linkr_debugger_la_legacy_adapter legacy_adapter;
	enum linkr_debugger_la_pipeline_family pipeline_family;
	enum linkr_debugger_la_stop_reason unsupported_reason;
	uint8_t bytes_per_sample;
	uint8_t channel_count;
	uint32_t max_sample_rate_hz;
	bool supported;
	const char *reason;
};

enum linkr_debugger_la_state {
	LINKR_DEBUGGER_LA_STATE_IDLE = 0,
	LINKR_DEBUGGER_LA_STATE_ARMED,
	LINKR_DEBUGGER_LA_STATE_CAPTURING,
	LINKR_DEBUGGER_LA_STATE_DONE,
	LINKR_DEBUGGER_LA_STATE_ERROR,
	LINKR_DEBUGGER_LA_STATE_STREAMING,
};

struct linkr_debugger_la_sample {
	uint32_t timestamp_us;
	uint16_t values;
	uint16_t reserved;
};

struct linkr_debugger_la_capture {
	enum linkr_debugger_la_state state;
	struct linkr_debugger_la_config config;
	uint32_t sample_count;
	uint32_t trigger_index;
	uint32_t requested_sample_rate_hz;
	uint32_t actual_sample_rate_hz;
	uint64_t sample_period_ps;
	const char *backend;
	struct linkr_debugger_la_sample *samples;
};

struct linkr_debugger_la_pio_program_layout {
	uint8_t length;
	uint8_t wrap_target;
	uint8_t wrap;
};

struct linkr_debugger_la_wide11_burst_plan {
	uint32_t sample_count;
	uint32_t lane_a_word_count;
	uint32_t lane_b_word_count;
	uint32_t lane_a_byte_count;
	uint32_t lane_b_byte_count;
	uint32_t total_byte_count;
};

struct linkr_debugger_la_packed_burst_lane {
	uint8_t pin_base;
	uint8_t pin_count;
	uint8_t bits_per_sample;
	uint8_t autopush_bits;
	uint8_t samples_per_word;
	uint32_t word_count;
	uint32_t byte_count;
	uint32_t arena_offset;
};

struct linkr_debugger_la_packed_burst_plan {
	enum linkr_debugger_la_hardware_plan_kind kind;
	uint32_t requested_sample_count;
	uint32_t emitted_sample_count;
	uint32_t source_sample_count;
	uint32_t sample_alignment;
	uint32_t total_byte_count;
	uint8_t lane_count;
	uint8_t bytes_per_sample;
	uint8_t selected_pin_count;
	uint8_t selected_pins[LINKR_DEBUGGER_LA_MAX_CHANNELS];
	bool continuous_until_capacity;
	struct linkr_debugger_la_packed_burst_lane lanes[
		LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES];
};

struct linkr_debugger_la_packed_ring_lane {
	uint8_t pin_base;
	uint8_t pin_count;
	uint8_t bits_per_sample;
	uint8_t autopush_bits;
	uint8_t samples_per_word;
	uint8_t ring_size_bits;
	uint32_t word_count;
	uint32_t byte_count;
	uint32_t sample_capacity;
	uint32_t arena_offset;
};

struct linkr_debugger_la_packed_ring_plan {
	enum linkr_debugger_la_hardware_plan_kind kind;
	uint32_t sample_capacity;
	uint32_t usable_sample_capacity;
	uint32_t safety_samples;
	uint32_t chunk_samples;
	uint8_t lane_count;
	uint8_t bytes_per_sample;
	uint8_t selected_pin_count;
	uint8_t selected_pins[LINKR_DEBUGGER_LA_MAX_CHANNELS];
	struct linkr_debugger_la_packed_ring_lane lanes[
		LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES];
};

/* Continuous streaming mode */
enum linkr_debugger_la_ring_poll_result {
	LINKR_DEBUGGER_LA_RING_POLL_OK = 0,
	LINKR_DEBUGGER_LA_RING_POLL_POSSIBLE_OVERRUN,
	LINKR_DEBUGGER_LA_RING_POLL_DEFINITE_OVERRUN,
};

struct linkr_debugger_la_stream_chunk {
	uint32_t sequence;
	uint32_t sample_count;
	uint64_t timestamp_us;
	uint16_t *values;
	enum linkr_debugger_la_ring_poll_result status;
};

typedef void (*linkr_debugger_la_stream_callback_t)(
	const struct linkr_debugger_la_stream_chunk *chunk, void *user_data);

enum linkr_debugger_la_stream_payload_format {
	LINKR_DEBUGGER_LA_STREAM_PAYLOAD_PACKED_LE_BYTES = 1,
	LINKR_DEBUGGER_LA_STREAM_PAYLOAD_SINGLE_BITS = 2,
};

struct linkr_debugger_la_stream_sink_lease {
	uint8_t *payload;
	size_t capacity;
	void *token;
};

struct linkr_debugger_la_stream_sink_commit {
	void *token;
	uint32_t sample_count;
	uint8_t bytes_per_sample;
	size_t payload_len;
};

typedef int (*linkr_debugger_la_stream_sink_lease_t)(
	uint32_t sample_count,
	uint8_t bytes_per_sample,
	void *user_data,
	struct linkr_debugger_la_stream_sink_lease *lease);
/* Commit return contract: <0 fails the stream, 0 commits and continues raw
 * draining, >0 commits and requests one bounded transport handoff.
 */
typedef int (*linkr_debugger_la_stream_sink_commit_t)(
	const struct linkr_debugger_la_stream_sink_commit *commit,
	void *user_data);
typedef void (*linkr_debugger_la_stream_sink_abort_t)(void *token, void *user_data);
typedef void (*linkr_debugger_la_stream_sink_terminal_t)(
	enum linkr_debugger_la_ring_poll_result status,
	uint32_t sequence,
	void *user_data);

struct linkr_debugger_la_stream_sink {
	enum linkr_debugger_la_stream_payload_format format;
	uint8_t bytes_per_sample;
	uint32_t max_chunk_samples;
	linkr_debugger_la_stream_sink_lease_t lease;
	linkr_debugger_la_stream_sink_commit_t commit;
	linkr_debugger_la_stream_sink_abort_t abort;
	linkr_debugger_la_stream_sink_terminal_t terminal;
	void *user_data;
};

enum linkr_debugger_la_start_prepare_state {
	LINKR_DEBUGGER_LA_START_PREPARE_EMPTY = 0,
	LINKR_DEBUGGER_LA_START_PREPARE_READY,
	LINKR_DEBUGGER_LA_START_PREPARE_RESPONSE_SENT,
	LINKR_DEBUGGER_LA_START_PREPARE_ARMED_EVENT_SENT,
	LINKR_DEBUGGER_LA_START_PREPARE_GO_FAILED,
	LINKR_DEBUGGER_LA_START_PREPARE_CANCELLED,
};

struct linkr_debugger_la_start_prepare {
	uint32_t generation;
	enum linkr_debugger_la_start_prepare_state state;
	struct linkr_debugger_la_config config;
	struct linkr_debugger_la_session_contract contract;
	struct linkr_debugger_la_hardware_plan plan;
	uint32_t requested_sample_rate_hz;
	uint32_t actual_sample_rate_hz;
	uint64_t sample_period_ps;
	const char *backend;
	bool config_v2;
	bool arena_held;
	struct linkr_debugger_capture_arena_lease arena_lease;
	struct linkr_debugger_la_stream_sink sink;
	bool sink_bound;
	bool hardware_prepared;
};

int linkr_debugger_logic_analyzer_init(void);
int linkr_debugger_logic_analyzer_arm(const struct linkr_debugger_la_config *config);
int linkr_debugger_logic_analyzer_cancel(void);
enum linkr_debugger_la_state linkr_debugger_logic_analyzer_get_state(void);
int linkr_debugger_logic_analyzer_get_capture(
	struct linkr_debugger_la_capture *capture,
	struct linkr_debugger_la_sample *samples,
	size_t sample_capacity);
void linkr_debugger_logic_analyzer_release(void);
uint32_t linkr_debugger_logic_analyzer_max_samples(uint8_t pin_count, uint32_t buffer_size);
uint32_t linkr_debugger_logic_analyzer_actual_rate(uint32_t requested_rate);
int linkr_debugger_logic_analyzer_bounded_sample_target(
	uint32_t pre_samples,
	uint32_t post_samples,
	uint32_t *target_samples);
bool linkr_debugger_logic_analyzer_pre_trigger_supported(
	enum linkr_debugger_la_trigger_type trigger,
	uint32_t requested_rate_hz,
	uint32_t pre_samples,
	uint32_t post_samples);
int linkr_debugger_logic_analyzer_pre_trigger_minimum_retention_samples(
	uint32_t actual_rate_hz,
	uint32_t *required_samples);
bool linkr_debugger_logic_analyzer_pre_trigger_plan_feasible(
	const struct linkr_debugger_la_packed_ring_plan *plan,
	uint32_t actual_rate_hz);
bool linkr_debugger_logic_analyzer_pre_trigger_plan_supported(
	const struct linkr_debugger_la_config *config);
uint64_t linkr_debugger_logic_analyzer_pre_trigger_scan_start(
	uint64_t writer_sequence,
	uint32_t pre_samples);
bool linkr_debugger_logic_analyzer_pre_trigger_edge_matches(
	enum linkr_debugger_la_trigger_type trigger,
	uint8_t previous_level,
	uint8_t current_level);
bool linkr_debugger_logic_analyzer_pre_trigger_scan_source_overrun(
	uint64_t writer_sequence,
	uint64_t first_sequence,
	uint32_t ring_samples,
	uint32_t safety_margin);
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
	bool *edge_found);
int linkr_debugger_logic_analyzer_pre_trigger_window(
	const struct linkr_debugger_la_config *config,
	uint64_t writer_sequence,
	uint64_t trigger_sequence,
	uint32_t ring_samples,
	uint32_t safety_margin,
	struct linkr_debugger_la_pre_trigger_window *window);
uint32_t linkr_debugger_logic_analyzer_capture_actual_rate(void);
bool linkr_debugger_logic_analyzer_packed_rate_limit_supported(
	uint32_t requested_rate_hz,
	uint32_t actual_rate_hz,
	uint32_t requested_limit_hz);
uint64_t linkr_debugger_logic_analyzer_sample_period_ps(uint32_t actual_rate_hz);
uint16_t linkr_debugger_logic_analyzer_compress_raw_sample(
	uint32_t raw, const struct linkr_debugger_la_config *config);
int linkr_debugger_logic_analyzer_validate_config(
	const struct linkr_debugger_la_config *config, uint32_t capacity_samples);
int linkr_debugger_logic_analyzer_validate_stream_config(
	const struct linkr_debugger_la_config *config);
const char *linkr_debugger_logic_analyzer_backend(void);
bool linkr_debugger_logic_analyzer_wide11_burst_exact_eligible(
	const struct linkr_debugger_la_config *config);
int linkr_debugger_logic_analyzer_session_contract(
	const struct linkr_debugger_la_config *config,
	struct linkr_debugger_la_session_contract *contract);
int linkr_debugger_logic_analyzer_select_hardware_plan(
	const struct linkr_debugger_la_config *config,
	bool config_v2,
	struct linkr_debugger_la_hardware_plan *plan);
const char *linkr_debugger_logic_analyzer_stop_reason_name(
	enum linkr_debugger_la_stop_reason reason);
const char *linkr_debugger_logic_analyzer_hardware_plan_name(
	enum linkr_debugger_la_hardware_plan_kind kind);
bool linkr_debugger_logic_analyzer_stream_config_needs_irq_clear(
	const struct linkr_debugger_la_config *config);
enum linkr_debugger_la_trigger_type linkr_debugger_logic_analyzer_effective_trigger(
	enum linkr_debugger_la_trigger_type trigger,
	bool current_level_high);
int linkr_debugger_logic_analyzer_build_capture_program(
	bool wait_for_trigger_irq,
	uint8_t offset,
	uint16_t *instructions,
	size_t instruction_count,
	struct linkr_debugger_la_pio_program_layout *layout);
int linkr_debugger_logic_analyzer_build_wide11_sm_a_program(
	uint8_t offset,
	uint16_t *instructions,
	size_t instruction_count,
	struct linkr_debugger_la_pio_program_layout *layout);
int linkr_debugger_logic_analyzer_build_wide11_sm_b_program(
	uint8_t offset,
	uint16_t *instructions,
	size_t instruction_count,
	struct linkr_debugger_la_pio_program_layout *layout);
int linkr_debugger_logic_analyzer_wide11_burst_plan(
	uint32_t sample_count,
	struct linkr_debugger_la_wide11_burst_plan *plan);
int linkr_debugger_logic_analyzer_packed_burst_plan(
	const struct linkr_debugger_la_config *config,
	struct linkr_debugger_la_packed_burst_plan *plan);
int linkr_debugger_logic_analyzer_packed_ring_plan(
	const struct linkr_debugger_la_config *config,
	struct linkr_debugger_la_packed_ring_plan *plan);
int linkr_debugger_logic_analyzer_decode_wide11_burst(
	const uint32_t *lane_a_words,
	uint32_t lane_a_word_count,
	const uint32_t *lane_b_words,
	uint32_t lane_b_word_count,
	uint16_t *samples,
	uint32_t sample_count);
int linkr_debugger_logic_analyzer_decode_wide11_burst_span(
	const uint32_t *lane_a_words,
	uint32_t lane_a_word_count,
	const uint32_t *lane_b_words,
	uint32_t lane_b_word_count,
	uint32_t source_sample_count,
	uint32_t first_sample,
	uint8_t *packed_le,
	size_t packed_len,
	uint32_t sample_count);
int linkr_debugger_logic_analyzer_decode_packed_burst_span(
	const struct linkr_debugger_la_packed_burst_plan *plan,
	const uint32_t * const lane_words[],
	const uint32_t lane_word_counts[],
	uint32_t first_sample,
	uint8_t *packed_le,
	size_t packed_len,
	uint32_t sample_count);
int linkr_debugger_logic_analyzer_decode_packed_ring_span(
	const struct linkr_debugger_la_packed_ring_plan *plan,
	const uint32_t * const lane_words[],
	const uint32_t lane_word_counts[],
	uint64_t first_sample,
	uint8_t *packed_le,
	size_t packed_len,
	uint32_t sample_count);
uint8_t linkr_debugger_logic_analyzer_wide11_burst_completion_mask_update(
	uint32_t active_generation,
	uint32_t done_generation,
	uint8_t current_mask,
	uint8_t done_bit,
	bool *complete);
uint8_t linkr_debugger_logic_analyzer_wide11_burst_configured_pin_count_model(
	const struct linkr_debugger_la_config *config);
int linkr_debugger_logic_analyzer_prepare_wide11_burst_start(
	const struct linkr_debugger_la_config *config,
	struct linkr_debugger_la_start_prepare *prepare);
int linkr_debugger_logic_analyzer_prepare_wide11_burst_start_sink(
	const struct linkr_debugger_la_config *config,
	const struct linkr_debugger_capture_arena_lease *arena_lease,
	const struct linkr_debugger_la_stream_sink *sink,
	struct linkr_debugger_la_start_prepare *prepare);
int linkr_debugger_logic_analyzer_prepare_stream_start_sink(
	const struct linkr_debugger_la_config *config,
	bool config_v2,
	const struct linkr_debugger_la_stream_sink *sink,
	struct linkr_debugger_la_start_prepare *prepare);
int linkr_debugger_logic_analyzer_start_prepare_cancel(
	struct linkr_debugger_la_start_prepare *prepare);
int linkr_debugger_logic_analyzer_start_prepare_mark_response_sent(
	struct linkr_debugger_la_start_prepare *prepare);
int linkr_debugger_logic_analyzer_start_prepare_mark_armed_event_sent(
	struct linkr_debugger_la_start_prepare *prepare);
int linkr_debugger_logic_analyzer_start_prepare_go(
	struct linkr_debugger_la_start_prepare *prepare);

int linkr_debugger_logic_analyzer_start_stream(
	const struct linkr_debugger_la_config *config,
	linkr_debugger_la_stream_callback_t callback,
	void *user_data);
int linkr_debugger_logic_analyzer_start_stream_sink(
	const struct linkr_debugger_la_config *config,
	const struct linkr_debugger_la_stream_sink *sink);
int linkr_debugger_logic_analyzer_stop_stream(void);
bool linkr_debugger_logic_analyzer_is_streaming(void);
bool linkr_debugger_logic_analyzer_is_stream_triggered(void);

int linkr_debugger_logic_analyzer_stream_sink_validate(
	const struct linkr_debugger_la_config *config,
	const struct linkr_debugger_la_stream_sink *sink);
int linkr_debugger_logic_analyzer_stream_sink_lease_payload(
	const struct linkr_debugger_la_stream_sink *sink,
	uint32_t sample_count,
	struct linkr_debugger_la_stream_sink_lease *lease);
size_t linkr_debugger_logic_analyzer_stream_payload_len(
	enum linkr_debugger_la_stream_payload_format format,
	uint32_t sample_count,
	uint8_t bytes_per_sample);
int linkr_debugger_logic_analyzer_stream_sink_write_payload(
	const struct linkr_debugger_la_stream_sink *sink,
	const struct linkr_debugger_la_packed_ring_plan *plan,
	const uint32_t * const lane_words[],
	const uint32_t lane_word_counts[],
	uint64_t first_sample,
	uint32_t sample_count,
	uint8_t *payload,
	size_t payload_capacity,
	uint16_t *values_or,
	uint16_t *values_and);
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
	uint16_t *values_and);
int linkr_debugger_logic_analyzer_stream_sink_commit_payload(
	const struct linkr_debugger_la_stream_sink *sink,
	const struct linkr_debugger_la_stream_sink_commit *commit);
void linkr_debugger_logic_analyzer_stream_sink_abort_payload(
	const struct linkr_debugger_la_stream_sink *sink,
	struct linkr_debugger_la_stream_sink_lease *lease);
void linkr_debugger_logic_analyzer_stream_sink_notify_terminal(
	const struct linkr_debugger_la_stream_sink *sink,
	enum linkr_debugger_la_ring_poll_result status,
	uint32_t sequence);
bool linkr_debugger_logic_analyzer_stream_sink_allows_protocol_update(
	bool generation_current, uint32_t committed_samples);

bool linkr_debugger_logic_analyzer_is_stream_triggered(void);

#define LINKR_DEBUGGER_LA_RING_BUFFER_BYTES (32768U)
#define LINKR_DEBUGGER_LA_RING_SIZE_BITS 15U
#define LINKR_DEBUGGER_LA_RING_SAMPLES (LINKR_DEBUGGER_LA_RING_BUFFER_BYTES / sizeof(uint32_t))
#define LINKR_DEBUGGER_LA_RING_SAFETY_SAMPLES 2048U
#define LINKR_DEBUGGER_LA_RING_POLL_INTERVAL_MIN_MS 1U
#define LINKR_DEBUGGER_LA_RING_POLL_INTERVAL_MAX_MS 4U
#define LINKR_DEBUGGER_LA_RING_HALF_SAMPLES 2048U
#define LINKR_DEBUGGER_LA_PRE_TRIGGER_MAX 4096U

struct linkr_debugger_la_ring_progress {
	uint32_t last_hw_index;
	uint64_t writer_seq;
	uint64_t reader_seq;
	uint64_t last_poll_time_us;
	uint32_t generation;
	bool initialized;
};

struct linkr_debugger_la_ring_metrics {
	uint64_t max_poll_gap_us;
	uint64_t max_unread_samples;
	uint64_t max_emit_us;
	uint64_t max_compact_us;
	uint64_t total_compact_us;
	uint64_t max_callback_us;
	uint64_t total_callback_us;
	uint64_t total_consume_us;
	uint64_t last_consume_complete_us;
	uint64_t max_consumer_inter_chunk_gap_us;
	uint64_t max_consumer_yield_resume_us;
	uint32_t consume_chunk_count;
	uint32_t legacy_yield_count;
	uint32_t sink_handoff_requested_count;
	uint32_t sink_handoff_executed_count;
	uint32_t sink_handoff_skipped_backlog_count;
	bool consumer_gap_armed;
};

uint32_t linkr_debugger_logic_analyzer_ring_delta_samples(
	uint32_t last_hw_index, uint32_t hw_index, uint32_t ring_samples);
bool linkr_debugger_logic_analyzer_ring_window_may_overrun(
	uint64_t elapsed_us, uint32_t actual_rate_hz,
	uint32_t ring_samples, uint32_t safety_margin);
bool linkr_debugger_logic_analyzer_ring_seq_overran(
	uint64_t writer_seq, uint64_t reader_seq,
	uint32_t ring_samples, uint32_t safety_margin);
bool linkr_debugger_logic_analyzer_ring_should_freeze_before_overwrite(
	uint64_t writer_seq, uint64_t reader_seq, uint32_t produced_samples,
	uint32_t ring_samples, uint32_t safety_margin,
	uint32_t *retained_samples);
uint32_t linkr_debugger_logic_analyzer_ring_poll_interval_ms(
	uint32_t actual_rate_hz, uint32_t ring_samples, uint32_t safety_margin);
enum linkr_debugger_la_ring_poll_result linkr_debugger_logic_analyzer_ring_observe(
	struct linkr_debugger_la_ring_progress *progress,
	uint32_t hw_index,
	uint64_t now_us,
	uint32_t actual_rate_hz,
	uint32_t consumed_samples,
	uint32_t ring_samples,
	uint32_t safety_margin,
	uint32_t *produced_samples);
enum linkr_debugger_la_ring_poll_result linkr_debugger_logic_analyzer_packed_ring_observe(
	struct linkr_debugger_la_ring_progress *progress,
	uint32_t lane_last_hw_indices[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES],
	uint64_t lane_writer_seqs[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES],
	const uint32_t hw_word_indices[LINKR_DEBUGGER_LA_PACKED_BURST_MAX_LANES],
	uint64_t now_us,
	uint32_t actual_rate_hz,
	uint32_t consumed_samples,
	const struct linkr_debugger_la_packed_ring_plan *plan,
	bool rx_fifo_stalled,
	uint32_t *produced_samples,
	uint32_t *skew_samples);
uint32_t linkr_debugger_logic_analyzer_ring_next_emit_count(
	uint64_t available_samples, uint32_t remaining_samples, uint32_t chunk_samples);
uint32_t linkr_debugger_logic_analyzer_ring_drainable_samples(
	uint64_t available_samples, uint32_t remaining_samples, uint32_t chunk_samples);
uint32_t linkr_debugger_logic_analyzer_ring_terminal_emit_count(
	uint64_t available_samples, uint32_t remaining_samples,
	uint32_t chunk_samples, bool terminal_pending);
void linkr_debugger_logic_analyzer_ring_metrics_update(
	struct linkr_debugger_la_ring_metrics *metrics,
	uint64_t poll_gap_us, uint64_t unread_samples, uint64_t emit_us);
void linkr_debugger_logic_analyzer_ring_metrics_update_consume(
	struct linkr_debugger_la_ring_metrics *metrics,
	uint64_t compact_us, uint64_t callback_us, uint64_t total_us);
void linkr_debugger_logic_analyzer_ring_metrics_clear_consumer_gap(
	struct linkr_debugger_la_ring_metrics *metrics);
void linkr_debugger_logic_analyzer_ring_metrics_update_inter_chunk_gap(
	struct linkr_debugger_la_ring_metrics *metrics,
	uint64_t next_consume_start_us);
void linkr_debugger_logic_analyzer_ring_metrics_mark_chunk_complete(
	struct linkr_debugger_la_ring_metrics *metrics,
	uint64_t complete_us);
void linkr_debugger_logic_analyzer_ring_metrics_update_yield_resume(
	struct linkr_debugger_la_ring_metrics *metrics,
	uint64_t duration_us, bool sink_handoff);
void linkr_debugger_logic_analyzer_ring_metrics_update_sink_handoff(
	struct linkr_debugger_la_ring_metrics *metrics,
	bool requested, bool executed);
bool linkr_debugger_logic_analyzer_stream_copy_complete_advance_reader(
	struct linkr_debugger_la_ring_progress *progress,
	struct linkr_debugger_la_ring_metrics *metrics,
	bool generation_current,
	uint32_t operation_generation,
	uint64_t start_reader_seq,
	uint32_t copied_samples);
bool linkr_debugger_logic_analyzer_stream_callback_allows_protocol_update(
	bool generation_current, uint32_t emitted_samples);
bool linkr_debugger_logic_analyzer_stream_callback_should_yield_after_chunk(
	bool generation_current, uint32_t emitted_samples);
bool linkr_debugger_logic_analyzer_stream_sink_backpressure_retryable(int ret);
bool linkr_debugger_logic_analyzer_stream_sink_should_yield_for_handoff(
	bool handoff_requested, uint64_t unread_samples);
bool linkr_debugger_logic_analyzer_stream_sink_should_explicit_yield(
	bool handoff_requested, uint64_t unread_samples);
int linkr_debugger_logic_analyzer_stream_consumer_priority(bool sink_session);
bool linkr_debugger_logic_analyzer_stream_generation_current(
	bool active, uint32_t stream_generation, uint32_t current_generation);
bool linkr_debugger_logic_analyzer_stream_start_must_wait_idle(
	bool thread_started, bool thread_busy);
uint8_t linkr_debugger_logic_analyzer_stream_idle_wait_mask(
	bool producer_started, bool producer_busy,
	bool consumer_started, bool consumer_busy);

struct linkr_debugger_la_ring_freeze_policy {
	bool stop_sampler_sm_a;
	bool stop_sampler_sm_b;
	bool stop_trigger_sm;
	bool abort_dma_a;
	bool abort_dma_b;
};

struct linkr_debugger_la_ring_freeze_policy
	linkr_debugger_logic_analyzer_ring_freeze_policy(uint8_t lane_count);

struct linkr_debugger_la_ring_state {
	volatile uint32_t trigger_pos;
	volatile bool triggered;
	volatile bool overrun;
	uint16_t pre_trigger_buf[LINKR_DEBUGGER_LA_PRE_TRIGGER_MAX];
	uint32_t pre_trigger_write;
	uint32_t pre_trigger_count;
	bool pre_trigger_active;
};

int linkr_debugger_logic_analyzer_start_ring(
	const struct linkr_debugger_la_config *config,
	linkr_debugger_la_stream_callback_t callback,
	void *user_data);
int linkr_debugger_logic_analyzer_stop_ring(void);
bool linkr_debugger_logic_analyzer_is_ring_active(void);
bool linkr_debugger_logic_analyzer_is_ring_triggered(void);
uint32_t linkr_debugger_logic_analyzer_ring_trigger_pos(void);

#if defined(LINKR_DEBUGGER_LA_HOST_TEST)
int linkr_debugger_logic_analyzer_host_set_capture(
	const struct linkr_debugger_la_capture *capture,
	const struct linkr_debugger_la_sample *samples,
	size_t sample_count);
#endif

#endif /* RADXA_LINKR_DEBUGGER_LOGIC_ANALYZER_H_ */
