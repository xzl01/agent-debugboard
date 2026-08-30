/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_SIGROK_LINKR_H_
#define RADXA_LINKR_DEBUGGER_SIGROK_LINKR_H_

#include "linkr_debugger_capture_arbiter.h"
#include "linkr_debugger_capture_arena.h"
#include "linkr_debugger_logic_analyzer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINKR_DEBUGGER_SIGROK_LINKR_PORT 5556U
#define LINKR_DEBUGGER_SIGROK_LINKR_MAGIC 0x72U
#define LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION 2U
#define LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES 9U

#define LINKR_DEBUGGER_SIGROK_LINKR_HELLO_BYTES 5U
#define LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES 12U
#define LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES 16U
#define LINKR_DEBUGGER_SIGROK_LINKR_ACK_BYTES 6U
#define LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES 6U
#define LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES 8U
#define LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BYTES 3U
#define LINKR_DEBUGGER_SIGROK_LINKR_MODE_CAPS_BYTES 8U

#define LINKR_DEBUGGER_SIGROK_LINKR_MAX_DATA_BYTES 4096U
#define LINKR_DEBUGGER_SIGROK_LINKR_MAX_PAYLOAD_BYTES \
	(LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + LINKR_DEBUGGER_SIGROK_LINKR_MAX_DATA_BYTES)
#define LINKR_DEBUGGER_SIGROK_LINKR_CAPS_MODE_COUNT 2U
#define LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_REQUEST_BYTES \
	LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES
#define LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_RESPONSE_BYTES \
	(1U + ((size_t)LINKR_DEBUGGER_SIGROK_LINKR_CAPS_MODE_COUNT * \
	LINKR_DEBUGGER_SIGROK_LINKR_MODE_CAPS_BYTES))
#define LINKR_DEBUGGER_SIGROK_LINKR_RING_BUFFER_BYTES 32768U
#define LINKR_DEBUGGER_SIGROK_LINKR_MAX_SAMPLE_INDEX 0xffffffU
#define LINKR_DEBUGGER_SIGROK_LINKR_STREAM_QDEPTH_LIMIT 32U
#define LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_QDEPTH 8U
#define LINKR_DEBUGGER_SIGROK_LINKR_STREAM_HANDOFF_QDEPTH 2U
#define LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_TIMEOUT_MS 8U
#define LINKR_DEBUGGER_SIGROK_LINKR_WS_DATA_SLOT_COUNT 4U
#define LINKR_DEBUGGER_SIGROK_LINKR_WS_TERMINAL_SLOT_COUNT 1U
#define LINKR_DEBUGGER_SIGROK_LINKR_WS_DATA_PAYLOAD_BYTES \
	LINKR_DEBUGGER_SIGROK_LINKR_MAX_DATA_BYTES
#define LINKR_DEBUGGER_SIGROK_LINKR_WS_MAX_FRAME_BYTES \
	(LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES + \
	 LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + \
	 LINKR_DEBUGGER_SIGROK_LINKR_WS_DATA_PAYLOAD_BYTES)
#define LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT 12U
#define LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_MAX_SAMPLES_PER_ITEM 1024U
#define LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_PAYLOAD_BYTES \
	(LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_MAX_SAMPLES_PER_ITEM * \
	 LINKR_DEBUGGER_LA_WIDE11_BURST_PACKED_SAMPLE_BYTES)
#define LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_MAX_FRAME_BYTES \
	(LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES + \
	 LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + \
	 LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_PAYLOAD_BYTES)
#define LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_TERMINAL_FRAME_BYTES \
	(LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES + LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES)
#define LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_QUEUE_MEMORY_LIMIT_BYTES 49152U

enum linkr_debugger_sigrok_linkr_frame_type {
	LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_REQ = 0x01,
	LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_RESP = 0x02,
	LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CAPS_REQ = 0x03,
	LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CAPS_RESP = 0x04,
	LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_REQ = 0x05,
	LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_RESP = 0x06,
	LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ = 0x07,
	LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_RESP = 0x08,
	LINKR_DEBUGGER_SIGROK_LINKR_FRAME_STOP_REQ = 0x09,
	LINKR_DEBUGGER_SIGROK_LINKR_FRAME_STOP_RESP = 0x0a,
	LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_V2_REQ = 0x0b,
	LINKR_DEBUGGER_SIGROK_LINKR_FRAME_EVENT = 0x10,
	LINKR_DEBUGGER_SIGROK_LINKR_FRAME_DATA = 0x11,
	LINKR_DEBUGGER_SIGROK_LINKR_FRAME_ERROR = 0x7f,
};

enum linkr_debugger_sigrok_linkr_error_code {
	LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_TYPE = 1,
	LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_LENGTH = 2,
	LINKR_DEBUGGER_SIGROK_LINKR_ERROR_UNSUPPORTED_VERSION = 3,
	LINKR_DEBUGGER_SIGROK_LINKR_ERROR_OVERSIZE_PAYLOAD = 4,
	LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INTERNAL = 5,
	LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_STATE = 6,
	LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG = 7,
	LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BUSY = 8,
};

	enum linkr_debugger_sigrok_linkr_mode_id {
	LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8 = 1,
	LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11 = 2,
};

enum linkr_debugger_sigrok_linkr_trigger_type {
	LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE = 0,
	LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING = 1,
	LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_FALLING = 2,
	LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_EITHER = 3,
};

enum linkr_debugger_sigrok_linkr_mode_flags {
	LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_CONTINUOUS = 1U << 0,
	LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_NONE = 1U << 1,
	LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_RISING = 1U << 2,
	LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_FALLING = 1U << 3,
	LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_EITHER = 1U << 4,
	LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_PRE_TRIGGER = 1U << 5,
};

enum linkr_debugger_sigrok_linkr_server_flags {
	/* Bit 0 advertises CONFIG_V2 frame encoding only. */
	LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAG_CONFIG_V2 = 1U << 0,
	/*
	 * Bit 1 advertises the generic packed-burst matrix: SINGLE/FAST8
	 * high-rate packed support plus WIDE11 100 MHz post0/1..100000.
	 * WIDE11 125 MHz remains excluded by the capture matrix/plan
	 * validation, not by this flag alone.
	 */
	LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAG_GENERIC_PACKED_BURST = 1U << 1,
	LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAGS_CURRENT =
		LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAG_CONFIG_V2 |
		LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAG_GENERIC_PACKED_BURST,
};

enum linkr_debugger_sigrok_linkr_compression {
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_NONE = 0,
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK = 1,
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_RLE = 2,
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK_RLE = 3,
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_SINGLE_BITS = 4,
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_SINGLE_BITS_RLE = 5,
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_PACKED_PALETTE2 = 6,
};

enum linkr_debugger_sigrok_linkr_compression_flags {
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_FLAG_BIT_PACK = 1U << 0,
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_FLAG_RLE = 1U << 1,
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_FLAG_SINGLE_BITS = 1U << 2,
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_FLAG_PALETTE2 = 1U << 3,
};

enum linkr_debugger_sigrok_linkr_event_type {
	LINKR_DEBUGGER_SIGROK_LINKR_EVENT_ARMED = 1,
	LINKR_DEBUGGER_SIGROK_LINKR_EVENT_TRIGGERED = 2,
	LINKR_DEBUGGER_SIGROK_LINKR_EVENT_RUNNING = 3,
	LINKR_DEBUGGER_SIGROK_LINKR_EVENT_STOPPED = 4,
	LINKR_DEBUGGER_SIGROK_LINKR_EVENT_OVERRUN = 5,
	LINKR_DEBUGGER_SIGROK_LINKR_EVENT_ERROR = 6,
};

enum linkr_debugger_sigrok_linkr_session_state {
	LINKR_DEBUGGER_SIGROK_LINKR_SESSION_WAIT_HELLO = 0,
	LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY,
	LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED,
	LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED,
	LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING,
};

enum linkr_debugger_sigrok_linkr_capture_action {
	LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_NONE = 0,
	LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_START_IMMEDIATE,
	LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_START_ARMED,
	LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_IMMEDIATE,
	LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_ARMED,
	LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_STOP,
};

enum linkr_debugger_sigrok_linkr_start_prepare_state {
	LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_IDLE = 0,
	LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_PREPARED,
	LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_GOING,
	LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_DONE,
	LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_CANCELLED,
};

enum linkr_debugger_sigrok_linkr_start_sequence_step {
	LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_PREPARE = 1,
	LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_START_RESP,
	LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_ARMED_EVENT,
	LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_GO,
	LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_ERROR,
};

enum linkr_debugger_sigrok_linkr_stream_wake_action {
	LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_DEFER = 0,
	LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_DELAY,
	LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_NOW,
};

struct linkr_debugger_sigrok_linkr_ws_transport_metrics {
	uint32_t max_qdepth;
	size_t max_qbytes;
	uint64_t max_drain_us;
	uint32_t max_drain_items;
	size_t max_drain_bytes;
	uint64_t max_send_us;
	uint8_t max_send_frames;
	size_t max_send_bytes;
};

enum linkr_debugger_sigrok_linkr_ws_slot_state {
	LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE = 0,
	LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED,
	LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED,
};

struct linkr_debugger_sigrok_linkr_header {
	uint8_t magic;
	uint8_t version;
	uint8_t type;
	uint32_t id;
	uint16_t payload_len;
};

struct linkr_debugger_sigrok_linkr_request {
	struct linkr_debugger_sigrok_linkr_header header;
	const uint8_t *payload;
};

struct linkr_debugger_sigrok_linkr_hello {
	uint8_t protocol_version;
	uint8_t server_flags;
	uint8_t mode_count;
	uint16_t max_payload_len;
};

struct linkr_debugger_sigrok_linkr_mode_caps {
	uint8_t mode_id;
	uint8_t mode_flags;
	uint8_t channel_count;
	uint8_t sample_bytes;
	uint32_t max_samplerate_khz;
	uint8_t compression;
};

struct linkr_debugger_sigrok_linkr_caps {
	uint8_t mode_count;
	struct linkr_debugger_sigrok_linkr_mode_caps modes[LINKR_DEBUGGER_SIGROK_LINKR_CAPS_MODE_COUNT];
};

struct linkr_debugger_sigrok_linkr_config {
	uint8_t mode_id;
	uint8_t trigger_type;
	uint8_t trigger_channel;
	uint16_t channel_mask;
	uint32_t samplerate_khz;
	uint32_t pre_samples;
	uint32_t post_samples;
};

struct linkr_debugger_la_config;

struct linkr_debugger_sigrok_linkr_ack {
	uint16_t session_id;
	uint8_t state;
	uint32_t actual_rate_khz;
};

struct linkr_debugger_sigrok_linkr_event {
	uint16_t session_id;
	uint8_t type_detail;
	uint32_t sample_index;
};

struct linkr_debugger_sigrok_linkr_data_meta {
	uint32_t sample_index;
	uint16_t sample_count;
	uint8_t compression;
	uint16_t channel_mask;
};

struct linkr_debugger_sigrok_linkr_error {
	uint8_t error_code;
	uint16_t detail;
};

struct linkr_debugger_sigrok_linkr_session {
	enum linkr_debugger_sigrok_linkr_session_state state;
	struct linkr_debugger_sigrok_linkr_config config;
	uint16_t next_session_id;
	uint16_t active_session_id;
	bool capture_owner_held;
	bool telemetry_pause_held;
	uint32_t sample_index;
	uint32_t emitted_samples;
	int client_fd;
};

struct linkr_debugger_sigrok_linkr_action_result {
	enum linkr_debugger_sigrok_linkr_capture_action capture_action;
	bool has_event;
	struct linkr_debugger_sigrok_linkr_event event;
};

struct linkr_debugger_sigrok_linkr_start_prepare {
	enum linkr_debugger_sigrok_linkr_start_prepare_state state;
	uint32_t generation;
	uint16_t sigrok_session_id;
	uint32_t ws_session_id;
	uint32_t ws_stream_generation;
	uint32_t source_generation;
	bool capture_owner_held;
	bool arena_held;
	bool ws_burst_pool_held;
	bool la_prepare_held;
	bool response_sent;
	bool armed_event_sent;
	struct linkr_debugger_capture_arena_lease arena_lease;
	struct linkr_debugger_la_start_prepare la_prepare;
};

struct linkr_debugger_sigrok_linkr_start_sequence_model {
	uint32_t generation;
	uint8_t step_count;
	bool prepared;
	bool response_sent;
	bool go_called;
	enum linkr_debugger_sigrok_linkr_start_sequence_step steps[8];
};

void linkr_debugger_sigrok_linkr_caps_init(struct linkr_debugger_sigrok_linkr_caps *caps);
void linkr_debugger_sigrok_linkr_session_reset(struct linkr_debugger_sigrok_linkr_session *session);

int linkr_debugger_sigrok_linkr_validate_header(
	const struct linkr_debugger_sigrok_linkr_header *header,
	bool *disconnect_required,
	enum linkr_debugger_sigrok_linkr_error_code *error_code);
int linkr_debugger_sigrok_linkr_validate_request(
	const struct linkr_debugger_sigrok_linkr_request *request,
	bool *disconnect_required,
	enum linkr_debugger_sigrok_linkr_error_code *error_code);
int linkr_debugger_sigrok_linkr_decode_config(
	const uint8_t *payload,
	size_t payload_len,
	struct linkr_debugger_sigrok_linkr_config *config);
int linkr_debugger_sigrok_linkr_validate_config(
	const struct linkr_debugger_sigrok_linkr_config *config,
	enum linkr_debugger_sigrok_linkr_error_code *error_code,
	uint16_t *detail);
int linkr_debugger_sigrok_linkr_to_la_config(
	const struct linkr_debugger_sigrok_linkr_config *config,
	bool armed,
	struct linkr_debugger_la_config *la_config);
bool linkr_debugger_sigrok_linkr_config_is_wide11_exact_burst(
	const struct linkr_debugger_sigrok_linkr_config *config);
bool linkr_debugger_sigrok_linkr_config_is_packed_burst(
	const struct linkr_debugger_sigrok_linkr_config *config);
int linkr_debugger_sigrok_linkr_packed_burst_plan(
	const struct linkr_debugger_sigrok_linkr_config *config,
	struct linkr_debugger_la_packed_burst_plan *plan);
uint32_t linkr_debugger_sigrok_linkr_packed_burst_max_chunk_samples(
	uint8_t bytes_per_sample);

void linkr_debugger_sigrok_linkr_init_response_header(
	struct linkr_debugger_sigrok_linkr_header *header,
	uint8_t type,
	uint32_t id,
	uint16_t payload_len);

size_t linkr_debugger_sigrok_linkr_encode_header(
	const struct linkr_debugger_sigrok_linkr_header *header,
	uint8_t *out,
	size_t out_len);
int linkr_debugger_sigrok_linkr_decode_header(
	const uint8_t *data,
	size_t data_len,
	struct linkr_debugger_sigrok_linkr_header *header);
int linkr_debugger_sigrok_linkr_decode_next_request_frame(
	const uint8_t *data,
	size_t data_len,
	size_t offset,
	struct linkr_debugger_sigrok_linkr_request *request,
	size_t *next_offset,
	bool *disconnect_required,
	enum linkr_debugger_sigrok_linkr_error_code *error_code);
size_t linkr_debugger_sigrok_linkr_encode_hello(
	const struct linkr_debugger_sigrok_linkr_hello *hello,
	uint8_t *out,
	size_t out_len);
size_t linkr_debugger_sigrok_linkr_encode_caps(
	const struct linkr_debugger_sigrok_linkr_caps *caps,
	uint8_t *out,
	size_t out_len);
size_t linkr_debugger_sigrok_linkr_encode_ack(
	const struct linkr_debugger_sigrok_linkr_ack *ack,
	uint8_t *out,
	size_t out_len);
size_t linkr_debugger_sigrok_linkr_encode_event(
	const struct linkr_debugger_sigrok_linkr_event *event,
	uint8_t *out,
	size_t out_len);
size_t linkr_debugger_sigrok_linkr_encode_data_meta(
	const struct linkr_debugger_sigrok_linkr_data_meta *meta,
	uint8_t *out,
	size_t out_len);
size_t linkr_debugger_sigrok_linkr_encode_error(
	const struct linkr_debugger_sigrok_linkr_error *error,
	uint8_t *out,
	size_t out_len);

int linkr_debugger_sigrok_linkr_handle_request(
	struct linkr_debugger_sigrok_linkr_session *session,
	enum linkr_debugger_capture_owner current_owner,
	const struct linkr_debugger_sigrok_linkr_request *request,
	struct linkr_debugger_sigrok_linkr_header *response_header,
	uint8_t *payload_out,
	size_t payload_out_len,
	size_t *payload_len_out,
	struct linkr_debugger_sigrok_linkr_action_result *action,
	bool *disconnect_required);
enum linkr_debugger_sigrok_linkr_error_code
linkr_debugger_sigrok_linkr_start_error_code(int ret, bool invalid_config);
void linkr_debugger_sigrok_linkr_build_error_response(
	const struct linkr_debugger_sigrok_linkr_request *request,
	struct linkr_debugger_sigrok_linkr_header *response_header,
	uint8_t *payload_out,
	size_t payload_out_len,
	enum linkr_debugger_sigrok_linkr_error_code error_code,
	uint16_t detail,
	size_t *payload_len_out);
void linkr_debugger_sigrok_linkr_rollback_start_failure(
	struct linkr_debugger_sigrok_linkr_session *session,
	struct linkr_debugger_sigrok_linkr_action_result *action);
void linkr_debugger_sigrok_linkr_start_prepare_reset(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare);
int linkr_debugger_sigrok_linkr_start_prepare_exact_burst(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare,
	struct linkr_debugger_sigrok_linkr_session *session,
	bool ws_transport,
	uint32_t ws_session_id,
	uint32_t ws_stream_generation,
	uint32_t source_generation,
	const struct linkr_debugger_la_stream_sink *sink);
int linkr_debugger_sigrok_linkr_start_prepare_capture(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare,
	struct linkr_debugger_sigrok_linkr_session *session,
	bool ws_transport,
	uint32_t ws_session_id,
	uint32_t ws_stream_generation,
	uint32_t source_generation,
	const struct linkr_debugger_la_stream_sink *sink);
void linkr_debugger_sigrok_linkr_start_prepare_cancel(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare,
	struct linkr_debugger_sigrok_linkr_session *session);
int linkr_debugger_sigrok_linkr_start_prepare_mark_response_sent(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare);
int linkr_debugger_sigrok_linkr_start_prepare_mark_armed_event_sent(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare);
int linkr_debugger_sigrok_linkr_start_prepare_go(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare,
	struct linkr_debugger_sigrok_linkr_session *session);
void linkr_debugger_sigrok_linkr_start_sequence_model_init(
	struct linkr_debugger_sigrok_linkr_start_sequence_model *model,
	uint32_t generation);
int linkr_debugger_sigrok_linkr_start_sequence_model_record(
	struct linkr_debugger_sigrok_linkr_start_sequence_model *model,
	enum linkr_debugger_sigrok_linkr_start_sequence_step step,
	uint32_t generation);

int linkr_debugger_sigrok_linkr_init(void);

bool linkr_debugger_sigrok_linkr_tcp_active(void);

int linkr_debugger_sigrok_linkr_send_data_frame(
	int client_fd,
	const struct linkr_debugger_sigrok_linkr_session *session,
	uint16_t session_id,
	uint32_t sample_index,
	uint16_t sample_count,
	uint8_t compression,
	uint16_t channel_mask,
	const uint8_t *data,
	size_t data_len);

int linkr_debugger_sigrok_linkr_send_event_frame(
	int client_fd,
	const struct linkr_debugger_sigrok_linkr_event *event);

uint8_t linkr_debugger_sigrok_linkr_bytes_per_sample(uint16_t channel_mask);
enum linkr_debugger_la_stream_payload_format
linkr_debugger_sigrok_linkr_stream_payload_format(uint16_t channel_mask);
size_t linkr_debugger_sigrok_linkr_packed_data_len(uint16_t channel_mask,
	uint16_t sample_count);
bool linkr_debugger_sigrok_linkr_stream_queue_has_capacity(uint32_t qdepth,
	uint32_t qdepth_limit, bool needs_terminal_event);
bool linkr_debugger_sigrok_linkr_ws_pool_data_has_capacity(uint32_t data_slots_used,
	bool needs_terminal_event);
bool linkr_debugger_sigrok_linkr_ws_pool_terminal_has_capacity(bool terminal_slot_used);
bool linkr_debugger_sigrok_linkr_ws_slot_transition_valid(
	enum linkr_debugger_sigrok_linkr_ws_slot_state from,
	enum linkr_debugger_sigrok_linkr_ws_slot_state to);
bool linkr_debugger_sigrok_linkr_ws_slot_commit_allowed(
	enum linkr_debugger_sigrok_linkr_ws_slot_state state,
	uint32_t slot_owner_session_id,
	uint32_t slot_owner_generation,
	uint32_t active_session_id,
	uint32_t active_generation);
bool linkr_debugger_sigrok_linkr_stream_queue_bytes_has_capacity(size_t qbytes,
	size_t next_item_bytes, size_t byte_limit, bool needs_terminal_event,
	size_t terminal_item_bytes);
size_t linkr_debugger_sigrok_linkr_raw_burst_queue_memory_bytes(uint32_t slot_count,
	size_t slot_frame_bytes);
bool linkr_debugger_sigrok_linkr_raw_burst_queue_has_space(uint32_t queued_items,
	uint32_t slot_count);
uint16_t linkr_debugger_sigrok_linkr_raw_burst_frame_sample_count(uint32_t emitted,
	uint32_t total_samples, uint32_t max_samples_per_item);
bool linkr_debugger_sigrok_linkr_raw_burst_should_emit_triggered_event(
	uint8_t trigger_type, bool triggered_committed);
bool linkr_debugger_sigrok_linkr_coalesce_can_append(size_t current_len,
	size_t next_frame_len, size_t buffer_capacity, uint8_t current_count,
	uint8_t max_count);
bool linkr_debugger_sigrok_linkr_should_emit_local_terminal_event(bool connected,
	int send_error);
enum linkr_debugger_sigrok_linkr_stream_wake_action
linkr_debugger_sigrok_linkr_stream_wake_policy(uint32_t qdepth,
	bool urgent, bool delayed_wake_pending, uint32_t wake_qdepth);
bool linkr_debugger_sigrok_linkr_stream_sink_handoff_requested(uint32_t qdepth);
void linkr_debugger_sigrok_linkr_ws_transport_metrics_reset(
	struct linkr_debugger_sigrok_linkr_ws_transport_metrics *metrics);
void linkr_debugger_sigrok_linkr_ws_transport_metrics_update_enqueue(
	struct linkr_debugger_sigrok_linkr_ws_transport_metrics *metrics,
	uint32_t qdepth, size_t qbytes);
void linkr_debugger_sigrok_linkr_ws_transport_metrics_update_drain(
	struct linkr_debugger_sigrok_linkr_ws_transport_metrics *metrics,
	uint64_t duration_us, uint32_t items, size_t bytes);
void linkr_debugger_sigrok_linkr_ws_transport_metrics_update_send(
	struct linkr_debugger_sigrok_linkr_ws_transport_metrics *metrics,
	uint64_t duration_us, uint8_t frames, size_t bytes);
bool linkr_debugger_sigrok_linkr_sample_range_fits(uint32_t sample_index,
	uint32_t sample_count);
uint32_t linkr_debugger_sigrok_linkr_advance_sample_index(uint32_t sample_index,
	uint32_t sample_count);
uint16_t linkr_debugger_sigrok_linkr_bounded_chunk_count(
	const struct linkr_debugger_sigrok_linkr_session *session,
	uint32_t offered_count);
uint32_t linkr_debugger_sigrok_linkr_bounded_sample_target(
	const struct linkr_debugger_sigrok_linkr_session *session);
uint32_t linkr_debugger_sigrok_linkr_trigger_sample_index(
	const struct linkr_debugger_sigrok_linkr_session *session);
bool linkr_debugger_sigrok_linkr_bounded_capture_done(
	const struct linkr_debugger_sigrok_linkr_session *session);
size_t linkr_debugger_sigrok_linkr_compress_bit_pack(
	const uint16_t *samples,
	uint32_t count,
	uint16_t channel_mask,
	uint8_t *out,
	size_t out_len);
size_t linkr_debugger_sigrok_linkr_compress_bit_pack_single(
	const uint16_t *samples,
	uint32_t count,
	uint8_t *out,
	size_t out_len);
size_t linkr_debugger_sigrok_linkr_compress_bit_pack_rle_single(
	const uint16_t *samples,
	uint32_t count,
	uint8_t *out,
	size_t out_len);
size_t linkr_debugger_sigrok_linkr_compress_bit_pack_rle(
	const uint16_t *samples,
	uint32_t count,
	uint16_t channel_mask,
	uint8_t *out,
	size_t out_len);
size_t linkr_debugger_sigrok_linkr_compress_rle(
	const uint8_t *samples,
	uint32_t count,
	uint8_t bytes_per_sample,
	uint8_t *out,
	size_t out_len);
size_t linkr_debugger_sigrok_linkr_compress_rle_if_smaller(
	const uint8_t *samples,
	uint32_t count,
	uint8_t bytes_per_sample,
	uint8_t *out,
	size_t out_len);
size_t linkr_debugger_sigrok_linkr_encode_stream_data_frame(
	enum linkr_debugger_la_stream_payload_format format,
	uint32_t sample_index,
	uint16_t sample_count,
	uint16_t channel_mask,
	const uint8_t *payload,
	size_t payload_len,
	bool try_rle,
	uint8_t *out,
	size_t out_len);
size_t linkr_debugger_sigrok_linkr_encode_packed_data_frame(
	uint32_t sample_index,
	uint16_t sample_count,
	uint16_t channel_mask,
	const uint8_t *packed,
	size_t packed_len,
	bool try_single_rle,
	uint8_t *out,
	size_t out_len);

#endif
