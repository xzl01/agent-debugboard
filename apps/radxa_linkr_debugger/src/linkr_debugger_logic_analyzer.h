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

#define LINKR_DEBUGGER_LA_MAX_CHANNELS 16
#define LINKR_DEBUGGER_LA_DEFAULT_BUFFER_SIZE (32 * 1024)
#define LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES 512
#define LINKR_DEBUGGER_LA_FINITE_GATED_MAX_SAMPLES LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES
#define LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES 1024
#define LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES \
	(2U * LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES)
#define LINKR_DEBUGGER_LA_STREAM_HANDOFF_UNREAD_SAMPLES \
	LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES
#define LINKR_DEBUGGER_LA_MIN_SAMPLE_RATE_HZ 100000U
#define LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ 125000000U
#define LINKR_DEBUGGER_LA_MAX_PRE_TRIGGER_SAMPLE_RATE_HZ 25000000U

enum linkr_debugger_la_trigger_type {
	LINKR_DEBUGGER_LA_TRIGGER_NONE = 0,
	LINKR_DEBUGGER_LA_TRIGGER_RISING,
	LINKR_DEBUGGER_LA_TRIGGER_FALLING,
	LINKR_DEBUGGER_LA_TRIGGER_EITHER,
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
uint32_t linkr_debugger_logic_analyzer_capture_actual_rate(void);
uint64_t linkr_debugger_logic_analyzer_sample_period_ps(uint32_t actual_rate_hz);
uint32_t linkr_debugger_logic_analyzer_dma_block_size(uint32_t sample_count);
uint16_t linkr_debugger_logic_analyzer_compress_raw_sample(
	uint32_t raw, const struct linkr_debugger_la_config *config);
int linkr_debugger_logic_analyzer_validate_config(
	const struct linkr_debugger_la_config *config, uint32_t capacity_samples);
int linkr_debugger_logic_analyzer_validate_stream_config(
	const struct linkr_debugger_la_config *config);
const char *linkr_debugger_logic_analyzer_backend(void);
bool linkr_debugger_logic_analyzer_finite_stream_eligible(
	const struct linkr_debugger_la_config *config);
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
};

struct linkr_debugger_la_stream_sink_lease {
	uint8_t *payload;
	size_t capacity;
	void *token;
};

struct linkr_debugger_la_stream_sink_commit {
	void *token;
	uint32_t sequence;
	uint32_t sample_count;
	uint64_t timestamp_us;
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

struct linkr_debugger_la_debug {
	uint32_t stream_irqs;
	uint32_t stream_chunks;
	uint64_t stream_ring_max_emit_us;
	uint64_t stream_ring_max_compact_us;
	uint64_t stream_ring_total_compact_us;
	uint64_t stream_ring_max_callback_us;
	uint64_t stream_ring_total_callback_us;
	uint64_t stream_ring_total_consume_us;
	uint32_t stream_ring_consume_chunks;
	uint32_t pre_write_index;
	uint32_t pre_post_remaining;
	uint64_t stream_ring_max_poll_gap_us;
	uint64_t stream_ring_max_unread_samples;
	uint16_t stream_values_or;
	uint16_t stream_values_and;
	bool pre_active;
	bool pre_triggered;
	bool stream_active;
};

void linkr_debugger_logic_analyzer_get_debug(struct linkr_debugger_la_debug *out);

bool linkr_debugger_logic_analyzer_is_stream_triggered(void);

#define LINKR_DEBUGGER_LA_RING_BUFFER_BYTES (32768U)
#define LINKR_DEBUGGER_LA_RING_SIZE_BITS 15U
#define LINKR_DEBUGGER_LA_RING_SAMPLES (LINKR_DEBUGGER_LA_RING_BUFFER_BYTES / sizeof(uint32_t))
#define LINKR_DEBUGGER_LA_RING_SAFETY_SAMPLES 1024U
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
uint32_t linkr_debugger_logic_analyzer_ring_next_emit_count(
	uint64_t available_samples, uint32_t remaining_samples, uint32_t chunk_samples);
uint32_t linkr_debugger_logic_analyzer_ring_drainable_samples(
	uint64_t available_samples, uint32_t remaining_samples, uint32_t chunk_samples);
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
