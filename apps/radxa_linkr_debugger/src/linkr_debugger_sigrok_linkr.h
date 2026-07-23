/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_SIGROK_LINKR_H_
#define RADXA_LINKR_DEBUGGER_SIGROK_LINKR_H_

#include "linkr_debugger_capture_arbiter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINKR_DEBUGGER_SIGROK_LINKR_PORT 5556U
#define LINKR_DEBUGGER_SIGROK_LINKR_MAGIC 0x72U
#define LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION 1U
#define LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES 9U

#define LINKR_DEBUGGER_SIGROK_LINKR_HELLO_BYTES 5U
#define LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES 12U
#define LINKR_DEBUGGER_SIGROK_LINKR_ACK_BYTES 6U
#define LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES 6U
#define LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES 8U
#define LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BYTES 3U
#define LINKR_DEBUGGER_SIGROK_LINKR_MODE_CAPS_BYTES 8U

#define LINKR_DEBUGGER_SIGROK_LINKR_MAX_PAYLOAD_BYTES 16400U
#define LINKR_DEBUGGER_SIGROK_LINKR_MAX_DATA_BYTES 16384U
#define LINKR_DEBUGGER_SIGROK_LINKR_CAPS_MODE_COUNT 2U
#define LINKR_DEBUGGER_SIGROK_LINKR_RING_BUFFER_BYTES 98304U

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
	LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE12 = 2,
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

enum linkr_debugger_sigrok_linkr_compression {
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_NONE = 0,
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK = 1,
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_RLE = 2,
	LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK_RLE = 3,
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
	LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_STOP,
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
	uint16_t pre_samples;
	uint16_t post_samples;
};

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
	uint32_t sample_index;
	int client_fd;
};

struct linkr_debugger_sigrok_linkr_action_result {
	enum linkr_debugger_sigrok_linkr_capture_action capture_action;
	bool has_event;
	struct linkr_debugger_sigrok_linkr_event event;
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
size_t linkr_debugger_sigrok_linkr_compress_bit_pack(
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

#endif
