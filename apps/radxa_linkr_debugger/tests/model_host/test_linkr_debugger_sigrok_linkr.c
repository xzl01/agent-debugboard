/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_sigrok_linkr.h"
#include "linkr_debugger_capture_arbiter.h"
#include "linkr_debugger_logic_analyzer.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static void test_header_roundtrip(void)
{
	struct linkr_debugger_sigrok_linkr_header original = {
		.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
		.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
		.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_REQ,
		.id = 42U,
		.payload_len = 0U,
	};
	uint8_t encoded[LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES];
	struct linkr_debugger_sigrok_linkr_header decoded;
	size_t len;

	len = linkr_debugger_sigrok_linkr_encode_header(&original, encoded, sizeof(encoded));
	assert(len == LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);

	memset(&decoded, 0xff, sizeof(decoded));
	assert(linkr_debugger_sigrok_linkr_decode_header(encoded, sizeof(encoded), &decoded) == 0);
	assert(decoded.magic == original.magic);
	assert(decoded.version == original.version);
	assert(decoded.type == original.type);
	assert(decoded.id == original.id);
	assert(decoded.payload_len == original.payload_len);
}

static size_t append_frame(uint8_t *out, size_t out_len, uint8_t frame_type,
	uint32_t frame_id, const uint8_t *payload, uint16_t payload_len)
{
	struct linkr_debugger_sigrok_linkr_header header = {
		.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
		.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
		.type = frame_type,
		.id = frame_id,
		.payload_len = payload_len,
	};
	size_t len;

	assert(out_len >= LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES + payload_len);
	len = linkr_debugger_sigrok_linkr_encode_header(&header, out, out_len);
	assert(len == LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);
	if (payload_len > 0U) {
		assert(payload != NULL);
		memcpy(out + len, payload, payload_len);
	}
	return len + payload_len;
}

static bool hello_flags_support_config_v2(uint8_t server_flags)
{
	return (server_flags & LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAG_CONFIG_V2) != 0U;
}

static bool hello_flags_support_generic_packed_burst(uint8_t server_flags)
{
	return (server_flags &
		LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAG_GENERIC_PACKED_BURST) != 0U;
}

static void encode_config_v1(uint8_t *payload, uint8_t mode_id, uint8_t trigger_type,
	uint8_t trigger_channel, uint16_t channel_mask, uint32_t samplerate_khz,
	uint16_t pre_samples, uint16_t post_samples)
{
	memset(payload, 0, LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES);
	payload[0] = mode_id;
	payload[1] = trigger_type;
	payload[2] = trigger_channel;
	payload[3] = (uint8_t)(channel_mask & 0xffU);
	payload[4] = (uint8_t)((channel_mask >> 8) & 0xffU);
	payload[5] = (uint8_t)(samplerate_khz & 0xffU);
	payload[6] = (uint8_t)((samplerate_khz >> 8) & 0xffU);
	payload[7] = (uint8_t)((samplerate_khz >> 16) & 0xffU);
	payload[8] = (uint8_t)(pre_samples & 0xffU);
	payload[9] = (uint8_t)((pre_samples >> 8) & 0xffU);
	payload[10] = (uint8_t)(post_samples & 0xffU);
	payload[11] = (uint8_t)((post_samples >> 8) & 0xffU);
}

static void encode_config_v2(uint8_t *payload, uint8_t mode_id, uint8_t trigger_type,
	uint8_t trigger_channel, uint16_t channel_mask, uint32_t samplerate_khz,
	uint32_t pre_samples, uint32_t post_samples)
{
	memset(payload, 0, LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES);
	payload[0] = mode_id;
	payload[1] = trigger_type;
	payload[2] = trigger_channel;
	payload[3] = (uint8_t)(channel_mask & 0xffU);
	payload[4] = (uint8_t)((channel_mask >> 8) & 0xffU);
	payload[5] = (uint8_t)(samplerate_khz & 0xffU);
	payload[6] = (uint8_t)((samplerate_khz >> 8) & 0xffU);
	payload[7] = (uint8_t)((samplerate_khz >> 16) & 0xffU);
	payload[8] = (uint8_t)(pre_samples & 0xffU);
	payload[9] = (uint8_t)((pre_samples >> 8) & 0xffU);
	payload[10] = (uint8_t)((pre_samples >> 16) & 0xffU);
	payload[11] = (uint8_t)((pre_samples >> 24) & 0xffU);
	payload[12] = (uint8_t)(post_samples & 0xffU);
	payload[13] = (uint8_t)((post_samples >> 8) & 0xffU);
	payload[14] = (uint8_t)((post_samples >> 16) & 0xffU);
	payload[15] = (uint8_t)((post_samples >> 24) & 0xffU);
}

static void test_decode_next_request_frame_rejects_truncated_boundaries(void)
{
	uint8_t data[LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES + 3U];
	struct linkr_debugger_sigrok_linkr_request request;
	size_t next_offset = 99U;
	bool disconnect = false;
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;
	size_t len;

	len = append_frame(data, sizeof(data), LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_REQ,
		10U, (const uint8_t[]){1U, 2U, 3U}, 3U);
	assert(linkr_debugger_sigrok_linkr_decode_next_request_frame(data, len - 1U,
		0U, &request, &next_offset, &disconnect, &error_code) == -EMSGSIZE);
	assert(!disconnect);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_LENGTH);
	assert(next_offset == 99U);
	assert(linkr_debugger_sigrok_linkr_decode_next_request_frame(data, 3U,
		0U, &request, &next_offset, &disconnect, &error_code) == -EMSGSIZE);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_LENGTH);
}

static void test_decode_next_request_frame_rejects_overlong_control_payload(void)
{
	uint8_t data[LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES];
	struct linkr_debugger_sigrok_linkr_header header = {
		.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
		.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
		.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_REQ,
		.id = 11U,
		.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_REQUEST_BYTES + 1U,
	};
	struct linkr_debugger_sigrok_linkr_request request;
	size_t next_offset = 0U;
	bool disconnect = false;
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;

	assert(linkr_debugger_sigrok_linkr_encode_header(&header, data, sizeof(data)) == sizeof(data));
	assert(linkr_debugger_sigrok_linkr_decode_next_request_frame(data, sizeof(data),
		0U, &request, &next_offset, &disconnect, &error_code) == -EMSGSIZE);
	assert(disconnect);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_OVERSIZE_PAYLOAD);
	assert(next_offset == 0U);
}

static void test_decode_next_request_frame_accepts_config_v2_control_payload(void)
{
	uint8_t frame[LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES];
	uint8_t config_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES];
	struct linkr_debugger_sigrok_linkr_request request;
	size_t next_offset = 0U;
	bool disconnect = false;
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;
	size_t len;

	encode_config_v2(config_payload, LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE, 0U, 0x00ffU, 500U, 0U, 100000U);
	len = append_frame(frame, sizeof(frame), LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_V2_REQ,
		21U, config_payload, sizeof(config_payload));
	assert(linkr_debugger_sigrok_linkr_decode_next_request_frame(frame, len,
		0U, &request, &next_offset, &disconnect, &error_code) == 0);
	assert(!disconnect);
	assert(error_code == 0U);
	assert(next_offset == len);
	assert(request.header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_V2_REQ);
	assert(request.header.payload_len == LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES);
	assert(request.payload == frame + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);
}

static void test_decode_next_request_frame_processes_concatenated_fifo(void)
{
	uint8_t data[64];
	uint8_t payload[256];
	struct linkr_debugger_sigrok_linkr_session session;
	struct linkr_debugger_sigrok_linkr_header response_header;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;
	size_t payload_len = 0U;
	size_t pos = 0U;
	size_t next_offset;
	struct linkr_debugger_sigrok_linkr_request request;

	pos += append_frame(data + pos, sizeof(data) - pos,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_REQ, 1U, NULL, 0U);
	pos += append_frame(data + pos, sizeof(data) - pos,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CAPS_REQ, 2U, NULL, 0U);

	linkr_debugger_sigrok_linkr_session_reset(&session);
	next_offset = 0U;
	assert(linkr_debugger_sigrok_linkr_decode_next_request_frame(data, pos,
		next_offset, &request, &next_offset, &disconnect, NULL) == 0);
	assert(request.header.id == 1U);
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &request, &response_header,
		payload, sizeof(payload), &payload_len, &action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_RESP);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY);

	assert(linkr_debugger_sigrok_linkr_decode_next_request_frame(data, pos,
		next_offset, &request, &next_offset, &disconnect, NULL) == 0);
	assert(request.header.id == 2U);
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &request, &response_header,
		payload, sizeof(payload), &payload_len, &action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CAPS_RESP);
	assert(next_offset == pos);
}

static void test_decode_next_request_frame_rejects_extra_nonframe_tail(void)
{
	uint8_t data[LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES + 1U];
	struct linkr_debugger_sigrok_linkr_request request;
	size_t next_offset;
	bool disconnect = false;
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;
	size_t pos = 0U;

	pos += append_frame(data, sizeof(data), LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_REQ,
		12U, NULL, 0U);
	data[pos++] = 0xa5U;
	next_offset = 0U;
	assert(linkr_debugger_sigrok_linkr_decode_next_request_frame(data, pos,
		next_offset, &request, &next_offset, &disconnect, &error_code) == 0);
	assert(next_offset == LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);
	assert(linkr_debugger_sigrok_linkr_decode_next_request_frame(data, pos,
		next_offset, &request, &next_offset, &disconnect, &error_code) == -EMSGSIZE);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_LENGTH);
}

static void test_validate_header_bad_magic(void)
{
	struct linkr_debugger_sigrok_linkr_header header = {
		.magic = 0xefU,
		.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
		.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_REQ,
		.id = 1U,
		.payload_len = 0U,
	};
	bool disconnect = false;
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;

	assert(linkr_debugger_sigrok_linkr_validate_header(&header, &disconnect, &error_code) == -EPROTO);
	assert(disconnect);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_TYPE);
}

static void test_validate_header_bad_version(void)
{
	struct linkr_debugger_sigrok_linkr_header header = {
		.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
		.version = 99U,
		.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_REQ,
		.id = 1U,
		.payload_len = 0U,
	};
	bool disconnect = false;
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;

	assert(linkr_debugger_sigrok_linkr_validate_header(&header, &disconnect, &error_code) == -EPROTO);
	assert(disconnect);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_UNSUPPORTED_VERSION);
}

static void test_validate_header_oversize(void)
{
	struct linkr_debugger_sigrok_linkr_header header = {
		.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
		.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
		.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_REQ,
		.id = 1U,
		.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_MAX_PAYLOAD_BYTES + 1U,
	};
	bool disconnect = false;
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;

	assert(linkr_debugger_sigrok_linkr_validate_header(&header, &disconnect, &error_code) == -EMSGSIZE);
	assert(disconnect);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_OVERSIZE_PAYLOAD);
}

static void test_validate_request_hello_no_payload(void)
{
	struct linkr_debugger_sigrok_linkr_request request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_REQ,
			.id = 1U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	bool disconnect = false;
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;

	assert(linkr_debugger_sigrok_linkr_validate_request(&request, &disconnect, &error_code) == 0);
	assert(!disconnect);
}

static void test_validate_request_hello_with_payload(void)
{
	uint8_t dummy = 0U;
	struct linkr_debugger_sigrok_linkr_request request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_REQ,
			.id = 1U,
			.payload_len = 1U,
		},
		.payload = &dummy,
	};
	bool disconnect = false;
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;

	assert(linkr_debugger_sigrok_linkr_validate_request(&request, &disconnect, &error_code) == -EMSGSIZE);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_LENGTH);
}

static void test_validate_request_config_lengths_are_versioned(void)
{
	uint8_t payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES];
	struct linkr_debugger_sigrok_linkr_request request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_REQ,
			.id = 20U,
			.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES,
		},
		.payload = payload,
	};
	bool disconnect = false;
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;

	assert(linkr_debugger_sigrok_linkr_validate_request(&request, &disconnect,
		&error_code) == -EMSGSIZE);
	assert(!disconnect);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_LENGTH);

	request.header.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_V2_REQ;
	request.header.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES;
	error_code = 0;
	assert(linkr_debugger_sigrok_linkr_validate_request(&request, &disconnect,
		&error_code) == -EMSGSIZE);
	assert(!disconnect);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_LENGTH);

	request.header.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES;
	error_code = 0;
	assert(linkr_debugger_sigrok_linkr_validate_request(&request, &disconnect,
		&error_code) == 0);
	assert(!disconnect);
	assert(error_code == 0U);
}

static void test_decode_config_v1_keeps_12_byte_uint16_samples(void)
{
	uint8_t payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES];
	struct linkr_debugger_sigrok_linkr_config config;

	encode_config_v1(payload, LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11,
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING, 10U, 0x07ffU, 125000U,
		1234U, UINT16_MAX);
	memset(&config, 0, sizeof(config));
	assert(linkr_debugger_sigrok_linkr_decode_config(payload, sizeof(payload),
		&config) == 0);
	assert(config.mode_id == LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11);
	assert(config.trigger_type == LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING);
	assert(config.trigger_channel == 10U);
	assert(config.channel_mask == 0x07ffU);
	assert(config.samplerate_khz == 125000U);
	assert(config.pre_samples == 1234U);
	assert(config.post_samples == UINT16_MAX);
}

static void test_decode_config_v2_accepts_u32_samples_and_zero_post_sentinel(void)
{
	uint8_t payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES];
	struct linkr_debugger_sigrok_linkr_config config;

	encode_config_v2(payload, LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE, 0U, 0x00ffU, 500U, 0U, 100000U);
	memset(&config, 0, sizeof(config));
	assert(linkr_debugger_sigrok_linkr_decode_config(payload, sizeof(payload),
		&config) == 0);
	assert(config.mode_id == LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8);
	assert(config.trigger_type == LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE);
	assert(config.channel_mask == 0x00ffU);
	assert(config.samplerate_khz == 500U);
	assert(config.pre_samples == 0U);
	assert(config.post_samples == 100000U);
	assert(linkr_debugger_sigrok_linkr_validate_config(&config, NULL, NULL) == 0);

	encode_config_v2(payload, LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE, 0U, 0x00ffU, 500U, 0U, 0U);
	memset(&config, 0xff, sizeof(config));
	assert(linkr_debugger_sigrok_linkr_decode_config(payload, sizeof(payload),
		&config) == 0);
	assert(config.post_samples == 0U);
	assert(linkr_debugger_sigrok_linkr_validate_config(&config, NULL, NULL) == 0);
}

static void test_decode_config_rejects_unknown_lengths_cleanly(void)
{
	uint8_t payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES] = {0};
	struct linkr_debugger_sigrok_linkr_config config;

	assert(linkr_debugger_sigrok_linkr_decode_config(payload,
		LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES - 1U, &config) == -EINVAL);
	assert(linkr_debugger_sigrok_linkr_decode_config(payload,
		LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES + 1U, &config) == -EINVAL);
	assert(linkr_debugger_sigrok_linkr_decode_config(payload,
		LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES + 1U, &config) == -EINVAL);
}

static void test_validate_config_fast8(void)
{
	struct linkr_debugger_sigrok_linkr_config config = {
		.mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE,
		.trigger_channel = 0U,
		.channel_mask = 0x00ffU,
		.samplerate_khz = 125000U,
		.pre_samples = 0U,
		.post_samples = 0U,
	};
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;
	uint16_t detail = 0U;

	assert(linkr_debugger_sigrok_linkr_validate_config(&config, &error_code, &detail) == 0);
}

static void test_validate_config_wide11(void)
{
	struct linkr_debugger_sigrok_linkr_config config = {
		.mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11,
		.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE,
		.trigger_channel = 0U,
		.channel_mask = 0x07ffU,
		.samplerate_khz = 125000U,
		.pre_samples = 0U,
		.post_samples = 0U,
	};
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;
	uint16_t detail = 0U;

	assert(linkr_debugger_sigrok_linkr_validate_config(&config, &error_code, &detail) == 0);
}

static void test_validate_config_fast8_too_fast(void)
{
	struct linkr_debugger_sigrok_linkr_config config = {
		.mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE,
		.trigger_channel = 0U,
		.channel_mask = 0x00ffU,
		.samplerate_khz = 125001U,
		.pre_samples = 0U,
		.post_samples = 0U,
	};
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;
	uint16_t detail = 0U;

	assert(linkr_debugger_sigrok_linkr_validate_config(&config, &error_code, &detail) == -EINVAL);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG);
}

static void test_validate_config_trigger_channel_not_in_mask(void)
{
	struct linkr_debugger_sigrok_linkr_config config = {
		.mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING,
		.trigger_channel = 2U,
		.channel_mask = 0x0003U,
		.samplerate_khz = 125000U,
		.pre_samples = 0U,
		.post_samples = 0U,
	};
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;
	uint16_t detail = 0U;

	assert(linkr_debugger_sigrok_linkr_validate_config(&config, &error_code, &detail) == -EINVAL);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG);
}

static void test_validate_config_accepts_only_bounded_pre_trigger_contract(void)
{
	struct linkr_debugger_sigrok_linkr_config config = {
		.mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING,
		.trigger_channel = 0U,
		.channel_mask = 0x0001U,
		.samplerate_khz = 1000U,
		.pre_samples = 16U,
		.post_samples = 16U,
	};
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;
	uint16_t detail = 0U;

	assert(linkr_debugger_sigrok_linkr_validate_config(&config, &error_code, &detail) == 0);

	config.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE;
	assert(linkr_debugger_sigrok_linkr_validate_config(&config, &error_code, &detail) == -EINVAL);

	config.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING;
	config.post_samples = 0U;
	assert(linkr_debugger_sigrok_linkr_validate_config(&config, &error_code, &detail) == -EINVAL);

	config.post_samples = 1U;
	config.samplerate_khz = 25001U;
	assert(linkr_debugger_sigrok_linkr_validate_config(&config, &error_code, &detail) == -EINVAL);

	config.samplerate_khz = 1000U;
	config.pre_samples = 512U;
	assert(linkr_debugger_sigrok_linkr_validate_config(&config, &error_code, &detail) == -EINVAL);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG);

	config.pre_samples = 256U;
	config.post_samples = 256U;
	config.samplerate_khz = 25000U;
	config.channel_mask = 0x0001U;
	assert(linkr_debugger_sigrok_linkr_validate_config(&config, &error_code, &detail) == 0);

	config.channel_mask = 0x00ffU;
	assert(linkr_debugger_sigrok_linkr_validate_config(&config, &error_code, &detail) == -EINVAL);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG);

	config.mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11;
	config.channel_mask = 0x07ffU;
	assert(linkr_debugger_sigrok_linkr_validate_config(&config, &error_code, &detail) == -EINVAL);
}

static void test_to_la_config_preserves_bounded_pre_trigger(void)
{
	struct linkr_debugger_sigrok_linkr_config config = {
		.mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_EITHER,
		.trigger_channel = 2U,
		.channel_mask = 0x0005U,
		.samplerate_khz = 1000U,
		.pre_samples = 16U,
		.post_samples = 16U,
	};
	struct linkr_debugger_la_config la_config;
	struct linkr_debugger_la_hardware_plan plan;

	assert(linkr_debugger_sigrok_linkr_to_la_config(&config, true, &la_config) == 0);
	assert(la_config.trigger == LINKR_DEBUGGER_LA_TRIGGER_EITHER);
	assert(la_config.trigger_pin == 1U);
	assert(la_config.pre_samples == 16U);
	assert(la_config.post_samples == 16U);
	assert(linkr_debugger_logic_analyzer_select_hardware_plan(&la_config, false,
		&plan) == 0);
	assert(plan.supported);
	assert(plan.pipeline_family == LINKR_DEBUGGER_LA_PIPELINE_FAMILY_COMMON_PACKED);
	assert(plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_RING_STREAM);
}

static void test_to_la_config_rejects_infeasible_bounded_pre_trigger(void)
{
	struct linkr_debugger_sigrok_linkr_config config = {
		.mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING,
		.trigger_channel = 0U,
		.channel_mask = 0x00ffU,
		.samplerate_khz = 25000U,
		.pre_samples = 256U,
		.post_samples = 256U,
	};
	struct linkr_debugger_la_config la_config;

	assert(linkr_debugger_sigrok_linkr_to_la_config(&config, true, &la_config) == -EINVAL);

	config.mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11;
	config.channel_mask = 0x07ffU;
	assert(linkr_debugger_sigrok_linkr_to_la_config(&config, true, &la_config) == -EINVAL);
}

static void test_handle_config_accepts_bounded_pre_trigger(void)
{
	uint8_t config_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES];
	uint8_t response_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_RESPONSE_BYTES];
	struct linkr_debugger_sigrok_linkr_session session;
	struct linkr_debugger_sigrok_linkr_request request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_REQ,
			.id = 61U,
			.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES,
		},
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	struct linkr_debugger_sigrok_linkr_action_result action;
	size_t response_len = 0U;
	bool disconnect = false;

	encode_config_v1(config_payload, LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING, 0U, 0x0001U, 1000U,
		16U, 16U);
	request.payload = config_payload;
	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &request, &response_header,
		response_payload, sizeof(response_payload), &response_len, &action,
		&disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_RESP);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED);
	assert(session.config.pre_samples == 16U);
	assert(session.config.post_samples == 16U);
	assert(!disconnect);
}

static void assert_la_mapping(
	uint8_t mode_id,
	uint16_t channel_mask,
	uint8_t trigger_channel,
	uint8_t expected_trigger_pin,
	uint8_t expected_pin_count,
	const uint8_t *expected_pins,
	uint8_t expected_selected_pin_count)
{
	struct linkr_debugger_sigrok_linkr_config config = {
		.mode_id = mode_id,
		.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING,
		.trigger_channel = trigger_channel,
		.channel_mask = channel_mask,
		.samplerate_khz = 1234U,
		.pre_samples = 0U,
		.post_samples = 88U,
	};
	struct linkr_debugger_la_config la_config;

	memset(&la_config, 0xff, sizeof(la_config));
	assert(linkr_debugger_sigrok_linkr_to_la_config(&config, true, &la_config) == 0);
	assert(la_config.sample_rate_hz == 1234000U);
	assert(la_config.trigger == LINKR_DEBUGGER_LA_TRIGGER_RISING);
	assert(la_config.trigger_pin == expected_trigger_pin);
	assert(la_config.pre_samples == 0U);
	assert(la_config.post_samples == 88U);
	assert(la_config.pin_base == 10U);
	assert(la_config.pin_count == expected_pin_count);
	assert(la_config.selected_pin_count == expected_selected_pin_count);
	for (uint8_t i = 0U; i < expected_selected_pin_count; i++) {
		assert(la_config.selected_pins[i] == expected_pins[i]);
	}

	memset(&la_config, 0xff, sizeof(la_config));
	assert(linkr_debugger_sigrok_linkr_to_la_config(&config, false, &la_config) == 0);
	assert(la_config.trigger == LINKR_DEBUGGER_LA_TRIGGER_NONE);
	assert(la_config.trigger_pin == 0U);
}

static void test_to_la_config_fast8_single_mask(void)
{
	const uint8_t pins[] = { 10U };

	assert_la_mapping(LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		0x0001U, 0U, 0U, 8U, pins, 1U);
}

static void test_to_la_config_fast8_sparse_mask(void)
{
	const uint8_t pins[] = { 10U, 12U, 15U, 17U };

	assert_la_mapping(LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		0x00a5U, 2U, 1U, 8U, pins, 4U);
}

static void test_to_la_config_fast8_full_mask(void)
{
	const uint8_t pins[] = { 10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U };

	assert_la_mapping(LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		0x00ffU, 7U, 7U, 8U, pins, 8U);
}

static void test_to_la_config_wide11_sparse_mask_with_bit10(void)
{
	const uint8_t pins[] = { 11U, 14U, 20U };

	assert_la_mapping(LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11,
		0x0412U, 10U, 2U, 11U, pins, 3U);
}

static void test_to_la_config_wide11_full_mask(void)
{
	const uint8_t pins[] = { 10U, 11U, 12U, 13U, 14U, 15U,
		16U, 17U, 18U, 19U, 20U };

	assert_la_mapping(LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11,
		0x07ffU, 10U, 10U, 11U, pins, 11U);
}

static void test_to_la_config_rejects_unselected_trigger_bit(void)
{
	struct linkr_debugger_sigrok_linkr_config config = {
		.mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING,
		.trigger_channel = 1U,
		.channel_mask = 0x0001U,
		.samplerate_khz = 1000U,
		.pre_samples = 0U,
		.post_samples = 1024U,
	};
	struct linkr_debugger_la_config la_config;

	assert(linkr_debugger_sigrok_linkr_to_la_config(&config, true, &la_config) == -EINVAL);
}

static void test_handle_hello(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	struct linkr_debugger_sigrok_linkr_request request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_REQ,
			.id = 1U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	linkr_debugger_sigrok_linkr_session_reset(&session);
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE,
		&request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_RESP);
	assert(payload_len == LINKR_DEBUGGER_SIGROK_LINKR_HELLO_BYTES);
	assert(payload[1] == LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAGS_CURRENT);
	assert(payload[1] ==
		(LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAG_CONFIG_V2 |
		 LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAG_GENERIC_PACKED_BURST));
	assert(hello_flags_support_config_v2(payload[1]));
	assert(hello_flags_support_generic_packed_burst(payload[1]));
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY);
	assert(!action.has_event);
}

static void test_hello_flags_distinguish_legacy_config_v2_from_generic_packed(void)
{
	uint8_t legacy_flags = LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAG_CONFIG_V2;
	uint8_t generic_flags = LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAGS_CURRENT;

	assert(hello_flags_support_config_v2(legacy_flags));
	assert(!hello_flags_support_generic_packed_burst(legacy_flags));
	assert(hello_flags_support_config_v2(generic_flags));
	assert(hello_flags_support_generic_packed_burst(generic_flags));
	assert((LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAG_CONFIG_V2 &
		LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAG_GENERIC_PACKED_BURST) == 0U);
}

static void test_handle_caps(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	struct linkr_debugger_sigrok_linkr_request request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CAPS_REQ,
			.id = 2U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[512];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE,
		&request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CAPS_RESP);
	assert(payload_len == 1U + ((size_t)LINKR_DEBUGGER_SIGROK_LINKR_CAPS_MODE_COUNT *
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_CAPS_BYTES));
}

static void test_handle_config(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	uint8_t config_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES];
	struct linkr_debugger_sigrok_linkr_request request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_REQ,
			.id = 3U,
			.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES,
		},
		.payload = config_payload,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	config_payload[0] = 1U;
	config_payload[1] = 0U;
	config_payload[2] = 0U;
	config_payload[3] = 0xffU;
	config_payload[4] = 0x00U;
	config_payload[5] = 0xf4U;
	config_payload[6] = 0x01U;
	config_payload[7] = 0x00U;
	config_payload[8] = 0x00U;
	config_payload[9] = 0x00U;
	config_payload[10] = 0x00U;
	config_payload[11] = 0x00U;

	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE,
		&request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_RESP);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED);
	assert(session.config.samplerate_khz == 500U);
	assert(session.config.channel_mask == 0x00ffU);
	assert(session.config.mode_id == LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8);
	assert(session.config.pre_samples == 0U);
	assert(session.config.post_samples == 0U);
}

static void test_handle_config_v2_fast8_large_post_prepares_on_start(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	uint8_t config_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES];
	struct linkr_debugger_sigrok_linkr_request config_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_V2_REQ,
			.id = 34U,
			.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES,
		},
		.payload = config_payload,
	};
	struct linkr_debugger_sigrok_linkr_request start_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ,
			.id = 35U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	encode_config_v2(config_payload, LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE, 0U, 0x00ffU, 500U, 0U, 100000U);
	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &config_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_RESP);
	assert(payload_len == LINKR_DEBUGGER_SIGROK_LINKR_ACK_BYTES);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED);
	assert(session.config.pre_samples == 0U);
	assert(session.config.post_samples == 100000U);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_NONE);
	assert(!action.has_event);
	assert(!disconnect);

	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &start_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_RESP);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING);
	assert(action.capture_action ==
		LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_IMMEDIATE);
	assert(action.has_event);
	assert(action.event.type_detail ==
		(uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_RUNNING);
	assert(!disconnect);
}

static void test_handle_config_v2_exact_wide11_large_post_prepares_on_start(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	uint8_t config_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES];
	struct linkr_debugger_sigrok_linkr_request config_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_V2_REQ,
			.id = 70U,
			.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES,
		},
		.payload = config_payload,
	};
	struct linkr_debugger_sigrok_linkr_request start_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ,
			.id = 71U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	encode_config_v2(config_payload, LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11,
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE, 0U, 0x07ffU,
		LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ / 1000U, 0U,
		LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES);
	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &config_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_RESP);
	assert(session.config.post_samples == LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES);

	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &start_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_RESP);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING);
	assert(action.capture_action ==
		LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_IMMEDIATE);
	assert(!action.has_event);
	assert(!disconnect);
}

static void test_handle_config_v2_wide11_125mhz_large_post_rejects_on_start(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	uint8_t config_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES];
	struct linkr_debugger_sigrok_linkr_request config_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_V2_REQ,
			.id = 74U,
			.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES,
		},
		.payload = config_payload,
	};
	struct linkr_debugger_sigrok_linkr_request start_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ,
			.id = 75U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	encode_config_v2(config_payload, LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11,
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE, 0U, 0x07ffU, 125000U, 0U,
		LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES);
	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &config_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_RESP);
	assert(session.config.post_samples == LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES);

	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &start_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_ERROR);
	assert(payload_len == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BYTES);
	assert(payload[0] == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_NONE);
	assert(!action.has_event);
	assert(!disconnect);
}

static void test_handle_config_v2_sparse_wide11_125mhz_rejects_on_start(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	uint8_t config_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES];
	struct linkr_debugger_sigrok_linkr_request config_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_V2_REQ,
			.id = 94U,
			.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES,
		},
		.payload = config_payload,
	};
	struct linkr_debugger_sigrok_linkr_request start_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ,
			.id = 95U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	encode_config_v2(config_payload, LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11,
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE, 0U, 0x0089U, 125000U, 0U,
		65536U);
	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &config_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_RESP);
	assert(session.config.mode_id == LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11);
	assert(session.config.channel_mask == 0x0089U);
	assert(session.config.samplerate_khz == 125000U);
	assert(session.config.post_samples == 65536U);

	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &start_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_ERROR);
	assert(payload_len == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BYTES);
	assert(payload[0] == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_NONE);
	assert(!action.has_event);
	assert(!disconnect);
}

static void test_handle_config_v2_exact_wide11_triggered_prepares_with_armed_event(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	uint8_t config_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES];
	struct linkr_debugger_sigrok_linkr_request config_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_V2_REQ,
			.id = 72U,
			.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES,
		},
		.payload = config_payload,
	};
	struct linkr_debugger_sigrok_linkr_request start_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ,
			.id = 73U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	encode_config_v2(config_payload, LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11,
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING, 10U, 0x07ffU,
		LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ / 1000U, 0U,
		LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES);
	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &config_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_RESP);

	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &start_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_RESP);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_ARMED);
	assert(action.has_event);
	assert(action.event.type_detail == (uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_ARMED);
}

static void test_handle_config_v2_preserves_zero_post_start_behavior(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	uint8_t config_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES];
	struct linkr_debugger_sigrok_linkr_request config_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_V2_REQ,
			.id = 36U,
			.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES,
		},
		.payload = config_payload,
	};
	struct linkr_debugger_sigrok_linkr_request start_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ,
			.id = 37U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	encode_config_v2(config_payload, LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE, 0U, 0x00ffU, 500U, 0U, 0U);
	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &config_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_RESP);
	assert(session.config.post_samples == 0U);

	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &start_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_RESP);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_IMMEDIATE);
	assert(action.has_event);
}

static void test_handle_config_v2_sparse_fast8_125mhz_large_post_prepares_on_start(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	uint8_t config_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES];
	struct linkr_debugger_sigrok_linkr_request config_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_V2_REQ,
			.id = 86U,
			.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES,
		},
		.payload = config_payload,
	};
	struct linkr_debugger_sigrok_linkr_request start_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ,
			.id = 87U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	encode_config_v2(config_payload, LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE, 0U, 0x0089U,
		LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ / 1000U, 0U, 65536U);
	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &config_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_RESP);
	assert(session.config.channel_mask == 0x0089U);
	assert(session.config.samplerate_khz == 125000U);
	assert(session.config.post_samples == 65536U);

	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &start_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_RESP);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_IMMEDIATE);
	assert(action.has_event);
	assert(!disconnect);
}

static void test_handle_config_v2_rejects_post_above_packed_burst_capacity_on_start(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	uint8_t config_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES];
	struct linkr_debugger_sigrok_linkr_request config_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_V2_REQ,
			.id = 88U,
			.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES,
		},
		.payload = config_payload,
	};
	struct linkr_debugger_sigrok_linkr_request start_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ,
			.id = 89U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	encode_config_v2(config_payload, LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE, 0U, 0x00ffU,
		LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ / 1000U, 0U,
		LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES + 1U);
	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &config_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_RESP);
	assert(session.config.post_samples == LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES + 1U);

	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE, &start_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_ERROR);
	assert(payload_len == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BYTES);
	assert(payload[0] == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_NONE);
	assert(!action.has_event);
	assert(!disconnect);
}

static uint32_t ack_actual_rate_khz(const uint8_t *payload)
{
	return (uint32_t)payload[3] | ((uint32_t)payload[4] << 8) |
		((uint32_t)payload[5] << 16);
}

static void test_handle_config_ack_uses_current_request_rate(void)
{
	struct linkr_debugger_la_capture stale_capture;
	struct linkr_debugger_la_sample stale_sample;
	struct linkr_debugger_sigrok_linkr_session session;
	uint8_t config_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES];
	struct linkr_debugger_sigrok_linkr_request request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_REQ,
			.id = 33U,
			.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES,
		},
		.payload = config_payload,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	memset(&stale_capture, 0, sizeof(stale_capture));
	memset(&stale_sample, 0, sizeof(stale_sample));
	stale_capture.state = LINKR_DEBUGGER_LA_STATE_DONE;
	stale_capture.actual_sample_rate_hz = 1000000U;
	assert(linkr_debugger_logic_analyzer_host_set_capture(&stale_capture, &stale_sample, 1U) == 0);

	memset(config_payload, 0, sizeof(config_payload));
	config_payload[0] = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8;
	config_payload[3] = 0x01U;
	config_payload[5] = 0xf4U;
	config_payload[6] = 0x01U;

	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE,
		&request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_RESP);
	assert(payload_len == LINKR_DEBUGGER_SIGROK_LINKR_ACK_BYTES);
	assert(ack_actual_rate_khz(payload) == 500U);
}

static void test_handle_start_immediate(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	struct linkr_debugger_sigrok_linkr_request request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ,
			.id = 4U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
	session.config.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE,
		&request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_RESP);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_IMMEDIATE);
	assert(action.has_event);
	assert(action.event.type_detail == (uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_RUNNING);
}

static void test_handle_start_armed(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	struct linkr_debugger_sigrok_linkr_request request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ,
			.id = 5U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
	session.config.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE,
		&request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_ARMED);
	assert(action.has_event);
	assert(action.event.type_detail == (uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_ARMED);
}

static void test_handle_stop(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	struct linkr_debugger_sigrok_linkr_request request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_STOP_REQ,
			.id = 6U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE,
		&request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_STOP_RESP);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_STOP);
	assert(action.has_event);
	assert(action.event.type_detail == (uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_STOPPED);
}

static void test_handle_stop_idempotent_when_configured(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	struct linkr_debugger_sigrok_linkr_request request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_STOP_REQ,
			.id = 66U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
	memset(&action, 0xff, sizeof(action));
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE,
		&request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_STOP_RESP);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_NONE);
	assert(!action.has_event);
}

static void test_handle_stop_preserves_same_socket_config_start_reuse(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	uint8_t config_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES];
	uint8_t payload[256];
	struct linkr_debugger_sigrok_linkr_header response_header;
	struct linkr_debugger_sigrok_linkr_action_result action;
	size_t payload_len = 0U;
	bool disconnect = false;
	struct linkr_debugger_sigrok_linkr_request stop_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_STOP_REQ,
			.id = 67U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_request config_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_REQ,
			.id = 68U,
			.payload_len = LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES,
		},
		.payload = config_payload,
	};
	struct linkr_debugger_sigrok_linkr_request start_request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ,
			.id = 69U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};

	memset(config_payload, 0, sizeof(config_payload));
	config_payload[0] = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8;
	config_payload[1] = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE;
	config_payload[3] = 0xffU;
	config_payload[5] = 0xf4U;
	config_payload[6] = 0x01U;
	config_payload[10] = 0x00U;
	config_payload[11] = 0x04U;

	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE,
		&stop_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_STOP_RESP);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_STOP);
	assert(action.has_event);
	assert(!disconnect);

	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE,
		&config_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_RESP);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_NONE);
	assert(!disconnect);

	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_NONE,
		&start_request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_RESP);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_IMMEDIATE);
	assert(action.has_event);
	assert(!disconnect);
}

static void test_handle_start_busy(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	struct linkr_debugger_sigrok_linkr_request request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ,
			.id = 7U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[256];
	size_t payload_len = 0U;
	struct linkr_debugger_sigrok_linkr_action_result action;
	bool disconnect = false;

	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
	assert(linkr_debugger_sigrok_linkr_handle_request(&session,
		LINKR_DEBUGGER_CAPTURE_OWNER_LOGIC_ANALYZER,
		&request,
		&response_header, payload, sizeof(payload), &payload_len,
		&action, &disconnect) == 0);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_ERROR);
}

static void test_start_error_code_mapping(void)
{
	assert(linkr_debugger_sigrok_linkr_start_error_code(-EBUSY, false) ==
		LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BUSY);
	assert(linkr_debugger_sigrok_linkr_start_error_code(-EINVAL, false) ==
		LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG);
	assert(linkr_debugger_sigrok_linkr_start_error_code(-EIO, true) ==
		LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG);
	assert(linkr_debugger_sigrok_linkr_start_error_code(-EIO, false) ==
		LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INTERNAL);
}

static struct linkr_debugger_sigrok_linkr_session exact_wide11_session(
	enum linkr_debugger_sigrok_linkr_trigger_type trigger)
{
	struct linkr_debugger_sigrok_linkr_session session;

	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = trigger == LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE ?
		LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING :
		LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED;
	session.active_session_id = 9U;
	session.config.mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11;
	session.config.trigger_type = (uint8_t)trigger;
	session.config.trigger_channel = 10U;
	session.config.channel_mask = 0x07ffU;
	session.config.samplerate_khz =
		LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ / 1000U;
	session.config.pre_samples = 0U;
	session.config.post_samples = LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES;
	return session;
}

static struct linkr_debugger_sigrok_linkr_session fast8_session(
	uint32_t samplerate_khz, uint32_t post_samples)
{
	struct linkr_debugger_sigrok_linkr_session session;

	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING;
	session.active_session_id = 10U;
	session.config.mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8;
	session.config.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE;
	session.config.trigger_channel = 0U;
	session.config.channel_mask = 0x00ffU;
	session.config.samplerate_khz = samplerate_khz;
	session.config.pre_samples = 0U;
	session.config.post_samples = post_samples;
	return session;
}

static int test_burst_sink_lease(uint32_t sample_count, uint8_t bytes_per_sample,
	void *user_data, struct linkr_debugger_la_stream_sink_lease *lease)
{
	static uint8_t payload[LINKR_DEBUGGER_LA_WIDE11_BURST_PACKED_SAMPLE_BYTES * 1024U];

	(void)user_data;
	assert(sample_count <= 1024U);
	assert(bytes_per_sample == LINKR_DEBUGGER_LA_WIDE11_BURST_PACKED_SAMPLE_BYTES);
	lease->payload = payload;
	lease->capacity = sizeof(payload);
	lease->token = payload;
	return 0;
}

static int test_burst_sink_commit(
	const struct linkr_debugger_la_stream_sink_commit *commit, void *user_data)
{
	uint32_t *total = user_data;

	assert(commit != NULL);
	assert(commit->token != NULL);
	assert(commit->sample_count <= 1024U);
	assert(commit->bytes_per_sample == LINKR_DEBUGGER_LA_WIDE11_BURST_PACKED_SAMPLE_BYTES);
	assert(commit->payload_len ==
		(size_t)commit->sample_count * LINKR_DEBUGGER_LA_WIDE11_BURST_PACKED_SAMPLE_BYTES);
	*total += commit->sample_count;
	return 0;
}

static void test_burst_sink_abort(void *token, void *user_data)
{
	(void)token;
	(void)user_data;
}

static void test_burst_sink_terminal(enum linkr_debugger_la_ring_poll_result status,
	uint32_t sequence, void *user_data)
{
	(void)sequence;
	(void)user_data;
	assert(status == LINKR_DEBUGGER_LA_RING_POLL_OK);
}

static struct linkr_debugger_la_stream_sink test_wide11_burst_sink(uint32_t *total)
{
	struct linkr_debugger_la_stream_sink sink = {
		.format = LINKR_DEBUGGER_LA_STREAM_PAYLOAD_PACKED_LE_BYTES,
		.bytes_per_sample = LINKR_DEBUGGER_LA_WIDE11_BURST_PACKED_SAMPLE_BYTES,
		.max_chunk_samples = 1024U,
		.lease = test_burst_sink_lease,
		.commit = test_burst_sink_commit,
		.abort = test_burst_sink_abort,
		.terminal = test_burst_sink_terminal,
		.user_data = total,
	};

	return sink;
}

struct test_generic_burst_sink_context {
	uint32_t total;
	uint8_t expected_bytes_per_sample;
	uint32_t expected_max_chunk_samples;
};

static int test_generic_burst_sink_lease(uint32_t sample_count, uint8_t bytes_per_sample,
	void *user_data, struct linkr_debugger_la_stream_sink_lease *lease)
{
	static uint8_t payload[LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_PAYLOAD_BYTES];
	struct test_generic_burst_sink_context *ctx = user_data;

	assert(ctx != NULL);
	assert(sample_count <= ctx->expected_max_chunk_samples);
	assert(bytes_per_sample == ctx->expected_bytes_per_sample);
	lease->payload = payload;
	lease->capacity = sizeof(payload);
	lease->token = payload;
	return 0;
}

static int test_generic_burst_sink_commit(
	const struct linkr_debugger_la_stream_sink_commit *commit, void *user_data)
{
	struct test_generic_burst_sink_context *ctx = user_data;

	assert(ctx != NULL);
	assert(commit != NULL);
	assert(commit->token != NULL);
	assert(commit->sample_count <= ctx->expected_max_chunk_samples);
	assert(commit->bytes_per_sample == ctx->expected_bytes_per_sample);
	assert(commit->payload_len ==
		(size_t)commit->sample_count * ctx->expected_bytes_per_sample);
	ctx->total += commit->sample_count;
	return 0;
}

static struct linkr_debugger_la_stream_sink test_generic_burst_sink(
	struct test_generic_burst_sink_context *ctx)
{
	struct linkr_debugger_la_stream_sink sink = {
		.format = LINKR_DEBUGGER_LA_STREAM_PAYLOAD_PACKED_LE_BYTES,
		.bytes_per_sample = ctx->expected_bytes_per_sample,
		.max_chunk_samples = ctx->expected_max_chunk_samples,
		.lease = test_generic_burst_sink_lease,
		.commit = test_generic_burst_sink_commit,
		.abort = test_burst_sink_abort,
		.terminal = test_burst_sink_terminal,
		.user_data = ctx,
	};

	return sink;
}

static void assert_exact_prepare_order(bool ws_transport,
	enum linkr_debugger_sigrok_linkr_trigger_type trigger)
{
	struct linkr_debugger_sigrok_linkr_session session = exact_wide11_session(trigger);
	struct linkr_debugger_sigrok_linkr_start_prepare prepare;
	struct linkr_debugger_sigrok_linkr_start_sequence_model model;
	uint32_t sink_total = 0U;
	struct linkr_debugger_la_stream_sink sink = test_wide11_burst_sink(&sink_total);
	int ret;

	linkr_debugger_capture_arbiter_reset();
	linkr_debugger_capture_arena_init();
	assert(linkr_debugger_logic_analyzer_cancel() == 0);
	linkr_debugger_sigrok_linkr_start_prepare_reset(&prepare);
	ret = linkr_debugger_sigrok_linkr_start_prepare_exact_burst(&prepare,
		&session, ws_transport, ws_transport ? 77U : 0U,
		ws_transport ? 3U : 0U, ws_transport ? 3U : 0U,
		&sink);
	assert(ret == 0);
	assert(prepare.state == LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_PREPARED);
	assert(prepare.capture_owner_held);
	assert(prepare.la_prepare_held);
	assert(prepare.arena_held);
	assert(prepare.ws_burst_pool_held == ws_transport);
	assert(session.capture_owner_held);

	linkr_debugger_sigrok_linkr_start_sequence_model_init(&model, prepare.generation);
	assert(linkr_debugger_sigrok_linkr_start_sequence_model_record(&model,
		LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_PREPARE, prepare.generation) == 0);
	assert(linkr_debugger_sigrok_linkr_start_prepare_go(&prepare, &session) == -ESTALE);
	assert(linkr_debugger_sigrok_linkr_start_prepare_mark_response_sent(&prepare) == 0);
	assert(linkr_debugger_sigrok_linkr_start_sequence_model_record(&model,
		LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_START_RESP, prepare.generation) == 0);

	if (trigger != LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE) {
		assert(linkr_debugger_sigrok_linkr_start_prepare_go(&prepare, &session) == -EPROTO);
		assert(linkr_debugger_sigrok_linkr_start_prepare_mark_armed_event_sent(&prepare) == 0);
		assert(linkr_debugger_sigrok_linkr_start_sequence_model_record(&model,
			LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_ARMED_EVENT,
			prepare.generation) == 0);
	}

	ret = linkr_debugger_sigrok_linkr_start_prepare_go(&prepare, &session);
	assert(ret == 0);
	assert(linkr_debugger_sigrok_linkr_start_sequence_model_record(&model,
		LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_GO, prepare.generation) == 0);
	assert(session.capture_owner_held);
	assert(linkr_debugger_capture_arbiter_owner() ==
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR);
	assert(linkr_debugger_logic_analyzer_stop_stream() == 0);
	assert(linkr_debugger_capture_arbiter_release(
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR));
	session.capture_owner_held = false;

	assert(model.steps[0] == LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_PREPARE);
	assert(model.steps[1] == LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_START_RESP);
	if (trigger == LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE) {
		assert(model.step_count == 3U);
		assert(model.steps[2] == LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_GO);
	} else {
		assert(model.step_count == 4U);
		assert(model.steps[2] == LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_ARMED_EVENT);
		assert(model.steps[3] == LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_GO);
	}
}

static void test_tcp_exact_prepare_order_none_and_triggered(void)
{
	assert_exact_prepare_order(false, LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE);
	assert_exact_prepare_order(false, LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING);
}

static void test_ws_exact_prepare_order_none_and_triggered(void)
{
	assert_exact_prepare_order(true, LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE);
	assert_exact_prepare_order(true, LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING);
}

static void test_exact_prepare_cancels_without_go_on_response_failure(void)
{
	struct linkr_debugger_sigrok_linkr_session session = exact_wide11_session(
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE);
	struct linkr_debugger_sigrok_linkr_start_prepare prepare;
	struct linkr_debugger_sigrok_linkr_start_sequence_model model;
	uint32_t sink_total = 0U;
	struct linkr_debugger_la_stream_sink sink = test_wide11_burst_sink(&sink_total);

	linkr_debugger_capture_arbiter_reset();
	linkr_debugger_capture_arena_init();
	assert(linkr_debugger_logic_analyzer_cancel() == 0);
	linkr_debugger_sigrok_linkr_start_prepare_reset(&prepare);
	assert(linkr_debugger_sigrok_linkr_start_prepare_exact_burst(&prepare,
		&session, false, 0U, 0U, 0U, &sink) == 0);
	linkr_debugger_sigrok_linkr_start_sequence_model_init(&model, prepare.generation);
	assert(linkr_debugger_sigrok_linkr_start_sequence_model_record(&model,
		LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_PREPARE, prepare.generation) == 0);
	linkr_debugger_sigrok_linkr_start_prepare_cancel(&prepare, &session);
	assert(model.step_count == 1U);
	assert(!model.go_called);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED);
	assert(linkr_debugger_capture_arbiter_owner() == LINKR_DEBUGGER_CAPTURE_OWNER_NONE);
}

static void test_exact_prepare_cancels_without_go_on_armed_event_failure(void)
{
	struct linkr_debugger_sigrok_linkr_session session = exact_wide11_session(
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING);
	struct linkr_debugger_sigrok_linkr_start_prepare prepare;
	struct linkr_debugger_sigrok_linkr_start_sequence_model model;
	uint32_t sink_total = 0U;
	struct linkr_debugger_la_stream_sink sink = test_wide11_burst_sink(&sink_total);

	linkr_debugger_capture_arbiter_reset();
	linkr_debugger_capture_arena_init();
	assert(linkr_debugger_logic_analyzer_cancel() == 0);
	linkr_debugger_sigrok_linkr_start_prepare_reset(&prepare);
	assert(linkr_debugger_sigrok_linkr_start_prepare_exact_burst(&prepare,
		&session, false, 0U, 0U, 0U, &sink) == 0);
	linkr_debugger_sigrok_linkr_start_sequence_model_init(&model, prepare.generation);
	assert(linkr_debugger_sigrok_linkr_start_sequence_model_record(&model,
		LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_PREPARE, prepare.generation) == 0);
	assert(linkr_debugger_sigrok_linkr_start_prepare_mark_response_sent(&prepare) == 0);
	assert(linkr_debugger_sigrok_linkr_start_sequence_model_record(&model,
		LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_START_RESP, prepare.generation) == 0);
	linkr_debugger_sigrok_linkr_start_prepare_cancel(&prepare, &session);
	assert(model.step_count == 2U);
	assert(!model.go_called);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED);
	assert(linkr_debugger_capture_arbiter_owner() == LINKR_DEBUGGER_CAPTURE_OWNER_NONE);
}

static void test_ws_exact_prepare_partial_cleanup_releases_arena_and_owner(void)
{
	struct linkr_debugger_sigrok_linkr_session session = exact_wide11_session(
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE);
	struct linkr_debugger_sigrok_linkr_start_prepare prepare;
	uint32_t sink_total = 0U;
	struct linkr_debugger_la_stream_sink sink = test_wide11_burst_sink(&sink_total);

	linkr_debugger_capture_arbiter_reset();
	linkr_debugger_capture_arena_init();
	assert(linkr_debugger_logic_analyzer_cancel() == 0);
	linkr_debugger_sigrok_linkr_start_prepare_reset(&prepare);
	assert(linkr_debugger_sigrok_linkr_start_prepare_exact_burst(&prepare,
		&session, true, 88U, 5U, 5U, &sink) == 0);
	assert(prepare.arena_held);
	assert(prepare.ws_burst_pool_held);
	assert(linkr_debugger_capture_arena_owner() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_WIDE11_BURST);
	assert(linkr_debugger_capture_arbiter_owner() ==
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR);
	linkr_debugger_sigrok_linkr_start_prepare_cancel(&prepare, &session);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED);
	assert(!session.capture_owner_held);
	assert(!prepare.arena_held);
	assert(!prepare.ws_burst_pool_held);
	assert(linkr_debugger_capture_arena_owner() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE);
	assert(linkr_debugger_capture_arbiter_owner() == LINKR_DEBUGGER_CAPTURE_OWNER_NONE);
}

static void test_generic_packed_burst_prepare_acquires_arena_for_fast8_and_post0(void)
{
	struct linkr_debugger_sigrok_linkr_start_prepare prepare;
	struct test_generic_burst_sink_context ctx = {
		.expected_bytes_per_sample = 1U,
		.expected_max_chunk_samples =
			LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_PAYLOAD_BYTES,
	};
	struct linkr_debugger_la_stream_sink sink = test_generic_burst_sink(&ctx);
	struct linkr_debugger_sigrok_linkr_session session = fast8_session(
		LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ / 1000U,
		LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES);

	linkr_debugger_capture_arbiter_reset();
	linkr_debugger_capture_arena_init();
	assert(linkr_debugger_logic_analyzer_cancel() == 0);
	linkr_debugger_sigrok_linkr_start_prepare_reset(&prepare);
	assert(linkr_debugger_sigrok_linkr_start_prepare_capture(&prepare, &session,
		true, 91U, 6U, 6U, &sink) == 0);
	assert(prepare.capture_owner_held);
	assert(prepare.arena_held);
	assert(prepare.ws_burst_pool_held);
	assert(prepare.la_prepare_held);
	assert(prepare.la_prepare.hardware_prepared);
	assert(prepare.la_prepare.plan.legacy_adapter ==
		LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);
	assert(prepare.la_prepare.sink.bytes_per_sample == 1U);
	assert(prepare.la_prepare.sink.max_chunk_samples ==
		LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_PAYLOAD_BYTES);
	linkr_debugger_sigrok_linkr_start_prepare_cancel(&prepare, &session);
	assert(linkr_debugger_capture_arena_owner() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE);
	assert(linkr_debugger_capture_arbiter_owner() == LINKR_DEBUGGER_CAPTURE_OWNER_NONE);

	session = exact_wide11_session(LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE);
	session.config.post_samples = 0U;
	ctx.expected_bytes_per_sample = LINKR_DEBUGGER_LA_WIDE11_BURST_PACKED_SAMPLE_BYTES;
	ctx.expected_max_chunk_samples =
		linkr_debugger_sigrok_linkr_packed_burst_max_chunk_samples(
			ctx.expected_bytes_per_sample);
	sink = test_generic_burst_sink(&ctx);
	linkr_debugger_sigrok_linkr_start_prepare_reset(&prepare);
	assert(linkr_debugger_sigrok_linkr_start_prepare_capture(&prepare, &session,
		true, 92U, 7U, 7U, &sink) == 0);
	assert(prepare.arena_held);
	assert(prepare.ws_burst_pool_held);
	assert(prepare.la_prepare.hardware_prepared);
	assert(prepare.la_prepare.plan.legacy_adapter ==
		LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);
	assert(prepare.la_prepare.sink.bytes_per_sample ==
		LINKR_DEBUGGER_LA_WIDE11_BURST_PACKED_SAMPLE_BYTES);
	assert(prepare.la_prepare.sink.max_chunk_samples ==
		ctx.expected_max_chunk_samples);
	linkr_debugger_sigrok_linkr_start_prepare_cancel(&prepare, &session);
	assert(linkr_debugger_capture_arena_owner() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE);
	assert(linkr_debugger_capture_arbiter_owner() == LINKR_DEBUGGER_CAPTURE_OWNER_NONE);
}

static void test_low_rate_post513_prepare_uses_ordinary_stream_without_arena(void)
{
	struct linkr_debugger_sigrok_linkr_start_prepare prepare;
	struct test_generic_burst_sink_context ctx = {
		.expected_bytes_per_sample = 1U,
		.expected_max_chunk_samples = LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES,
	};
	struct linkr_debugger_la_stream_sink sink = test_generic_burst_sink(&ctx);
	struct linkr_debugger_sigrok_linkr_session session = fast8_session(500U, 513U);

	linkr_debugger_capture_arbiter_reset();
	linkr_debugger_capture_arena_init();
	assert(linkr_debugger_logic_analyzer_cancel() == 0);
	linkr_debugger_sigrok_linkr_start_prepare_reset(&prepare);
	assert(linkr_debugger_sigrok_linkr_start_prepare_capture(&prepare, &session,
		true, 93U, 8U, 8U, &sink) == 0);
	assert(prepare.capture_owner_held);
	assert(prepare.arena_held);
	assert(prepare.ws_burst_pool_held);
	assert(prepare.la_prepare_held);
	assert(prepare.la_prepare.hardware_prepared);
	assert(prepare.la_prepare.plan.legacy_adapter ==
		LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST);
	linkr_debugger_sigrok_linkr_start_prepare_cancel(&prepare, &session);
	assert(linkr_debugger_capture_arena_owner() ==
		LINKR_DEBUGGER_CAPTURE_ARENA_OWNER_NONE);
	assert(linkr_debugger_capture_arbiter_owner() == LINKR_DEBUGGER_CAPTURE_OWNER_NONE);
}

static void test_start_failure_rollback_and_error_response(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	struct linkr_debugger_sigrok_linkr_action_result action = {
		.capture_action = LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_IMMEDIATE,
		.has_event = true,
		.event = {
			.session_id = 9U,
			.type_detail = (uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_RUNNING,
			.sample_index = 123U,
		},
	};
	struct linkr_debugger_sigrok_linkr_request request = {
		.header = {
			.magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC,
			.version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION,
			.type = LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ,
			.id = 70U,
			.payload_len = 0U,
		},
		.payload = NULL,
	};
	struct linkr_debugger_sigrok_linkr_header response_header;
	uint8_t payload[LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BYTES];
	size_t payload_len = 0U;

	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING;
	session.capture_owner_held = true;
	linkr_debugger_sigrok_linkr_rollback_start_failure(&session, &action);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED);
	assert(!session.capture_owner_held);
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_NONE);
	assert(!action.has_event);

	linkr_debugger_sigrok_linkr_build_error_response(&request, &response_header,
		payload, sizeof(payload), LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BUSY,
		(uint16_t)EBUSY, &payload_len);
	assert(response_header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_ERROR);
	assert(response_header.id == request.header.id);
	assert(response_header.payload_len == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BYTES);
	assert(payload_len == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BYTES);
	assert(payload[0] == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BUSY);
	assert(payload[1] == (uint8_t)EBUSY);
	assert(payload[2] == 0U);
}

static void test_session_reset(void)
{
	struct linkr_debugger_sigrok_linkr_session session;

	session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING;
	session.active_session_id = 42U;
	session.capture_owner_held = true;
	linkr_debugger_sigrok_linkr_session_reset(&session);
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_WAIT_HELLO);
	assert(session.next_session_id == 1U);
	assert(!session.capture_owner_held);
}

static void test_caps_init(void)
{
	struct linkr_debugger_sigrok_linkr_caps caps;

	linkr_debugger_sigrok_linkr_caps_init(&caps);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES == 8U);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_MAX_DATA_BYTES == 4096U);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_MAX_PAYLOAD_BYTES ==
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + 4096U);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_REQUEST_BYTES ==
		LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_RESPONSE_BYTES ==
		1U + ((size_t)LINKR_DEBUGGER_SIGROK_LINKR_MODE_CAPS_BYTES *
		LINKR_DEBUGGER_SIGROK_LINKR_CAPS_MODE_COUNT));
	assert(LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_RESPONSE_BYTES == 17U);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_RESPONSE_BYTES >=
		LINKR_DEBUGGER_SIGROK_LINKR_HELLO_BYTES);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_RESPONSE_BYTES >=
		LINKR_DEBUGGER_SIGROK_LINKR_ACK_BYTES);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_RESPONSE_BYTES >=
		LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BYTES);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_RING_BUFFER_BYTES == 32768U);
	assert(caps.mode_count == LINKR_DEBUGGER_SIGROK_LINKR_CAPS_MODE_COUNT);
	assert(caps.modes[0].mode_id == LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8);
	assert(caps.modes[0].channel_count == 8U);
	assert((caps.modes[0].mode_flags & LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_PRE_TRIGGER) != 0U);
	assert(caps.modes[0].sample_bytes == 1U);
	assert(caps.modes[0].max_samplerate_khz == 125000U);
	assert(caps.modes[1].mode_id == LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11);
	assert(caps.modes[1].channel_count == 11U);
	assert((caps.modes[1].mode_flags & LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_PRE_TRIGGER) != 0U);
	assert(caps.modes[1].sample_bytes == 2U);
	assert(caps.modes[1].max_samplerate_khz == 125000U);
}

static void test_bytes_per_sample_by_mode_width(void)
{
	uint16_t samples[2] = { 0x0001U, 0x0701U };
	uint8_t out[4];

	assert(linkr_debugger_sigrok_linkr_bytes_per_sample(0x0000U) == 0U);
	assert(linkr_debugger_sigrok_linkr_bytes_per_sample(0x0001U) == 1U);
	assert(linkr_debugger_sigrok_linkr_bytes_per_sample(0x00ffU) == 1U);
	assert(linkr_debugger_sigrok_linkr_bytes_per_sample(0x07ffU) == 2U);
	assert(linkr_debugger_sigrok_linkr_compress_bit_pack(samples, 2U, 0x07ffU,
		out, sizeof(out)) == 4U);
	assert(out[0] == 0x01U);
	assert(out[1] == 0x00U);
	assert(out[2] == 0x01U);
	assert(out[3] == 0x07U);
}

static void test_bit_pack_single_fast_path_encodes_one_byte_per_sample(void)
{
	uint16_t samples[] = { 0x0000U, 0x0001U, 0x0002U, 0xffffU };
	uint8_t out[4] = {0xffU, 0xffU, 0xffU, 0xffU};

	assert(linkr_debugger_sigrok_linkr_compress_bit_pack_single(samples, 4U,
		out, sizeof(out)) == 4U);
	assert(memcmp(out, (uint8_t[]){0x00U, 0x01U, 0x00U, 0x01U}, 4U) == 0);
	assert(linkr_debugger_sigrok_linkr_compress_bit_pack_single(samples, 4U,
		out, 3U) == 0U);
	assert(linkr_debugger_sigrok_linkr_compress_bit_pack_single(NULL, 4U,
		out, sizeof(out)) == 0U);
}

static void test_packed_data_len_is_exact_by_channel_width(void)
{
	assert(linkr_debugger_sigrok_linkr_packed_data_len(0x0000U, 128U) == 0U);
	assert(linkr_debugger_sigrok_linkr_packed_data_len(0x0001U, 128U) == 128U);
	assert(linkr_debugger_sigrok_linkr_packed_data_len(0x00ffU, 4096U) == 4096U);
	assert(linkr_debugger_sigrok_linkr_packed_data_len(0x07ffU, 2048U) == 4096U);
	assert(linkr_debugger_sigrok_linkr_packed_data_len(0x07ffU, 2049U) == 4098U);
}

static void test_bit_pack_rle_encodes_idle_1_byte_units(void)
{
	uint16_t samples[8] = {0};
	uint8_t out[8];

	assert(linkr_debugger_sigrok_linkr_compress_bit_pack_rle(samples, 8U, 0x00ffU,
		out, sizeof(out)) == 3U);
	assert(out[0] == 0x00U);
	assert(out[1] == 0x08U);
	assert(out[2] == 0x00U);
}

static void test_bit_pack_rle_single_encodes_idle_1024_sample_chunks(void)
{
	uint16_t zeros[1024];
	uint16_t ones[1024];
	uint8_t out[8];

	memset(zeros, 0, sizeof(zeros));
	for (size_t i = 0U; i < 1024U; i++) {
		ones[i] = 0x0001U;
	}

	assert(linkr_debugger_sigrok_linkr_compress_bit_pack_rle_single(zeros, 1024U,
		out, sizeof(out)) == 3U);
	assert(memcmp(out, (uint8_t[]){0x00U, 0x00U, 0x04U}, 3U) == 0);
	memset(out, 0, sizeof(out));
	assert(linkr_debugger_sigrok_linkr_compress_bit_pack_rle_single(ones, 1024U,
		out, sizeof(out)) == 3U);
	assert(memcmp(out, (uint8_t[]){0x01U, 0x00U, 0x04U}, 3U) == 0);
}

static void test_bit_pack_rle_encodes_uart_like_runs(void)
{
	uint16_t samples[] = {
		0x01U, 0x01U, 0x01U, 0x01U, 0x01U, 0x01U,
		0x00U, 0x00U, 0x00U, 0x00U,
		0x01U, 0x01U, 0x01U, 0x01U, 0x01U, 0x01U,
	};
	uint8_t out[16];

	assert(linkr_debugger_sigrok_linkr_compress_bit_pack_rle(samples, 16U, 0x0001U,
		out, sizeof(out)) == 9U);
	assert(memcmp(out, (uint8_t[]){0x01U, 0x06U, 0x00U, 0x00U, 0x04U, 0x00U,
		0x01U, 0x06U, 0x00U}, 9U) == 0);
}

static void test_bit_pack_rle_single_encodes_uart_like_runs(void)
{
	uint16_t samples[] = {
		0xffffU, 0x0001U, 0x0001U, 0x0001U, 0x0001U, 0x0001U,
		0x0000U, 0x0002U, 0x0000U, 0x0002U,
		0x0001U, 0xffffU, 0x0001U, 0xffffU, 0x0001U, 0xffffU,
	};
	uint8_t out[16];

	assert(linkr_debugger_sigrok_linkr_compress_bit_pack_rle_single(samples, 16U,
		out, sizeof(out)) == 9U);
	assert(memcmp(out, (uint8_t[]){0x01U, 0x06U, 0x00U, 0x00U, 0x04U, 0x00U,
		0x01U, 0x06U, 0x00U}, 9U) == 0);
}

static void test_bit_pack_rle_alternating_falls_back_when_no_benefit(void)
{
	uint16_t samples[] = {0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x01U};
	uint8_t out[18];

	assert(linkr_debugger_sigrok_linkr_compress_bit_pack_rle(samples, 6U, 0x0001U,
		out, sizeof(out)) == 0U);
	assert(linkr_debugger_sigrok_linkr_compress_bit_pack_rle_single(samples, 6U,
		out, sizeof(out)) == 0U);
}

static void test_bit_pack_rle_single_run_length_byte_order_and_capacity(void)
{
	uint16_t samples[300];
	uint8_t out[8];

	for (size_t i = 0U; i < sizeof(samples) / sizeof(samples[0]); i++) {
		samples[i] = 0x0001U;
	}

	assert(linkr_debugger_sigrok_linkr_compress_bit_pack_rle_single(samples, 300U,
		out, sizeof(out)) == 3U);
	assert(memcmp(out, (uint8_t[]){0x01U, 0x2cU, 0x01U}, 3U) == 0);
	assert(linkr_debugger_sigrok_linkr_compress_bit_pack_rle_single(samples, 300U,
		out, 2U) == 0U);
}

static void test_bit_pack_rle_encodes_2_byte_units_with_channel_mapping(void)
{
	uint16_t samples[] = {0x0401U, 0x0401U, 0x0401U, 0x0001U, 0x0001U};
	uint8_t out[10];

	assert(linkr_debugger_sigrok_linkr_compress_bit_pack_rle(samples, 5U, 0x07ffU,
		out, sizeof(out)) == 8U);
	assert(memcmp(out, (uint8_t[]){0x01U, 0x04U, 0x03U, 0x00U, 0x01U, 0x00U,
		0x02U, 0x00U}, 8U) == 0);
}

static void test_packed_wide11_sender_prefers_bit_pack_rle_when_smaller(void)
{
	uint8_t packed[2048];
	uint8_t frame[64];
	struct linkr_debugger_sigrok_linkr_header header;
	size_t len;
	const uint8_t *meta;
	const uint8_t *payload;

	memset(packed, 0, sizeof(packed));
	len = linkr_debugger_sigrok_linkr_encode_packed_data_frame(7U, 1024U, 0x07ffU,
		packed, sizeof(packed), true, frame, sizeof(frame));
	assert(len == LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + 4U);
	assert(linkr_debugger_sigrok_linkr_decode_header(frame, len, &header) == 0);
	assert(header.payload_len == LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + 4U);
	meta = frame + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES;
	assert(meta[5] == LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK_RLE);
	payload = meta + LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES;
	assert(memcmp(payload, (uint8_t[]){0x00U, 0x00U, 0x00U, 0x04U}, 4U) == 0);
}

static size_t decode_single_rle(const uint8_t *payload, size_t payload_len,
	uint8_t *out, size_t out_len)
{
	size_t in_pos = 0U;
	size_t out_pos = 0U;

	while (in_pos + 3U <= payload_len) {
		uint8_t value = payload[in_pos];
		uint16_t count = (uint16_t)payload[in_pos + 1U] |
			((uint16_t)payload[in_pos + 2U] << 8);

		in_pos += 3U;
		if (out_pos + count > out_len) {
			return 0U;
		}
		memset(out + out_pos, value, count);
		out_pos += count;
	}

	return in_pos == payload_len ? out_pos : 0U;
}

static size_t reference_rle_if_smaller(const uint8_t *samples, uint32_t count,
	uint8_t bytes_per_sample, uint8_t *out, size_t out_len)
{
	size_t normal_len;
	size_t out_pos = 0U;
	uint32_t i = 0U;

	if (samples == NULL || out == NULL || count == 0U || bytes_per_sample == 0U) {
		return 0U;
	}
	normal_len = (size_t)count * bytes_per_sample;
	if (normal_len == 0U) {
		return 0U;
	}

	while (i < count) {
		const uint8_t *current = &samples[i * bytes_per_sample];
		uint16_t run_count = 1U;
		size_t needed = (size_t)bytes_per_sample + 2U;

		while (i + run_count < count && run_count < UINT16_MAX) {
			const uint8_t *next = &samples[(i + run_count) * bytes_per_sample];

			if (memcmp(current, next, bytes_per_sample) != 0) {
				break;
			}
			run_count++;
		}

		if (out_pos + needed >= normal_len || out_pos + needed > out_len) {
			return 0U;
		}

		memcpy(&out[out_pos], current, bytes_per_sample);
		out_pos += bytes_per_sample;
		out[out_pos] = (uint8_t)(run_count & 0xffU);
		out[out_pos + 1U] = (uint8_t)((run_count >> 8) & 0xffU);
		out_pos += 2U;
		i += run_count;
	}

	return out_pos < normal_len ? out_pos : 0U;
}

static void assert_rle_if_smaller_matches_reference(const uint8_t *samples,
	uint32_t count, uint8_t bytes_per_sample, size_t out_len)
{
	static uint8_t actual[70000];
	static uint8_t expected[70000];
	size_t actual_len;
	size_t expected_len;

	assert(out_len <= sizeof(actual));
	memset(actual, 0xa5, sizeof(actual));
	memset(expected, 0x5a, sizeof(expected));
	actual_len = linkr_debugger_sigrok_linkr_compress_rle_if_smaller(samples,
		count, bytes_per_sample, actual, out_len);
	expected_len = reference_rle_if_smaller(samples, count, bytes_per_sample,
		expected, out_len);
	assert(actual_len == expected_len);
	assert(memcmp(actual, expected, actual_len) == 0);
}

static void test_packed_single_sender_rle_frame_decodes_to_original_bytes(void)
{
	uint8_t packed[1024];
	uint8_t decoded[1024];
	uint8_t frame[64];
	struct linkr_debugger_sigrok_linkr_header header;
	size_t len;
	const uint8_t *meta;
	const uint8_t *payload;

	memset(packed, 0, sizeof(packed));
	len = linkr_debugger_sigrok_linkr_encode_packed_data_frame(0x123456U,
		1024U, 0x0001U, packed, sizeof(packed), true, frame, sizeof(frame));
	assert(len == LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + 3U);
	assert(linkr_debugger_sigrok_linkr_decode_header(frame, len, &header) == 0);
	assert(header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_DATA);
	assert(header.payload_len == LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + 3U);
	meta = frame + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES;
	assert(memcmp(meta, (uint8_t[]){0x56U, 0x34U, 0x12U, 0x00U, 0x04U,
		LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK_RLE, 0x01U, 0x00U},
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES) == 0);
	payload = meta + LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES;
	assert(memcmp(payload, (uint8_t[]){0x00U, 0x00U, 0x04U}, 3U) == 0);
	assert(decode_single_rle(payload, 3U, decoded, sizeof(decoded)) == sizeof(packed));
	assert(memcmp(decoded, packed, sizeof(packed)) == 0);
}

static void test_packed_single_sender_fallback_frame_matches_bit_pack_bytes(void)
{
	uint8_t packed[] = {0U, 1U, 0U, 1U, 0U, 1U};
	uint8_t frame[64];
	struct linkr_debugger_sigrok_linkr_header header;
	size_t len;
	const uint8_t *meta;
	const uint8_t *payload;

	len = linkr_debugger_sigrok_linkr_encode_packed_data_frame(5U,
		6U, 0x0001U, packed, sizeof(packed), true, frame, sizeof(frame));
	assert(len == LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + sizeof(packed));
	assert(linkr_debugger_sigrok_linkr_decode_header(frame, len, &header) == 0);
	assert(header.payload_len == LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + sizeof(packed));
	meta = frame + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES;
	assert(meta[5] == LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK);
	payload = meta + LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES;
	assert(memcmp(payload, packed, sizeof(packed)) == 0);
}

static void test_packed_sender_rle_helper_falls_back_before_growth(void)
{
	uint8_t zeros[8];
	uint8_t alternating[] = {0U, 1U, 0U, 1U, 0U, 1U};
	uint8_t out[18];

	memset(zeros, 0, sizeof(zeros));
	assert(linkr_debugger_sigrok_linkr_compress_rle_if_smaller(zeros, 8U, 1U,
		out, sizeof(out)) == 3U);
	assert(memcmp(out, (uint8_t[]){0x00U, 0x08U, 0x00U}, 3U) == 0);
	assert(linkr_debugger_sigrok_linkr_compress_rle_if_smaller(alternating, 6U,
		1U, out, sizeof(out)) == 0U);
}

static void test_packed_sender_rle_single_fast_path_matches_reference_patterns(void)
{
	uint8_t alternating[32];
	uint8_t mixed[] = {7U, 7U, 7U, 1U, 2U, 3U, 3U, 3U, 3U, 9U, 9U};
	uint8_t idle[1024];
	uint8_t exact[] = {4U, 4U, 4U, 4U, 8U, 8U, 8U, 8U};
	uint8_t out[16];

	for (size_t i = 0U; i < sizeof(alternating); i++) {
		alternating[i] = (uint8_t)(i & 1U);
	}
	memset(idle, 0U, sizeof(idle));

	assert_rle_if_smaller_matches_reference(alternating, sizeof(alternating), 1U,
		sizeof(alternating) * 3U);
	assert_rle_if_smaller_matches_reference(mixed, sizeof(mixed), 1U, sizeof(out));
	assert_rle_if_smaller_matches_reference(idle, sizeof(idle), 1U, sizeof(out));
	assert_rle_if_smaller_matches_reference(exact, sizeof(exact), 1U, 5U);
	assert_rle_if_smaller_matches_reference(exact, sizeof(exact), 1U, 6U);
	assert(linkr_debugger_sigrok_linkr_compress_rle_if_smaller(exact,
		sizeof(exact), 1U, out, 5U) == 0U);
	assert(linkr_debugger_sigrok_linkr_compress_rle_if_smaller(exact,
		sizeof(exact), 1U, out, 6U) == 6U);
	assert(memcmp(out, (uint8_t[]){4U, 4U, 0U, 8U, 4U, 0U}, 6U) == 0);
}

static void test_packed_sender_rle_single_fast_path_splits_uint16_runs(void)
{
	static uint8_t samples[65536];
	static uint8_t encoded[8];
	static uint8_t decoded[65536];
	size_t len;

	memset(samples, 0x6d, sizeof(samples));
	assert_rle_if_smaller_matches_reference(samples, 65535U, 1U, sizeof(encoded));
	assert_rle_if_smaller_matches_reference(samples, 65536U, 1U, sizeof(encoded));
	len = linkr_debugger_sigrok_linkr_compress_rle_if_smaller(samples, 65536U,
		1U, encoded, sizeof(encoded));
	assert(len == 6U);
	assert(memcmp(encoded, (uint8_t[]){0x6dU, 0xffU, 0xffU, 0x6dU, 0x01U, 0x00U},
		6U) == 0);
	assert(decode_single_rle(encoded, len, decoded, sizeof(decoded)) == sizeof(samples));
	assert(memcmp(decoded, samples, sizeof(samples)) == 0);
}

static void test_packed_sender_rle_single_fast_path_matches_reference_randomized(void)
{
	uint8_t samples[257];
	uint32_t state = 0x12345678U;

	for (uint32_t round = 0U; round < 64U; round++) {
		uint32_t len = 1U + ((round * 37U) % sizeof(samples));

		for (uint32_t i = 0U; i < len; i++) {
			state = state * 1664525U + 1013904223U;
			if ((state & 0x07U) < 5U && i > 0U) {
				samples[i] = samples[i - 1U];
			} else {
				samples[i] = (uint8_t)(state >> 24);
			}
		}

		assert_rle_if_smaller_matches_reference(samples, len, 1U, sizeof(samples) * 3U);
		assert_rle_if_smaller_matches_reference(samples, len, 1U, len > 0U ? len - 1U : 0U);
	}
}

static void test_packed_sender_multiple_deferred_frames_fit_tx_buffer(void)
{
	const size_t tx_buffer = 6144U;
	const size_t single_1024_frame = LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + 1024U;
	const size_t single_2048_frame = LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES +
		LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES;
	uint8_t tx[6144];
	uint8_t zeros[1024];
	uint8_t alternating[1024];
	uint8_t wide_alternating[LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES];
	size_t pos = 0U;

	memset(zeros, 0, sizeof(zeros));
	for (size_t i = 0U; i < sizeof(alternating); i++) {
		alternating[i] = (uint8_t)(i & 1U);
	}
	for (size_t i = 0U; i < sizeof(wide_alternating); i++) {
		wide_alternating[i] = (uint8_t)(i & 1U);
	}

	for (uint8_t i = 0U; i < 16U; i++) {
		assert(linkr_debugger_sigrok_linkr_coalesce_can_append(pos,
			single_1024_frame, tx_buffer, i, 16U));
		size_t len = linkr_debugger_sigrok_linkr_encode_packed_data_frame(i * 1024U,
			1024U, 0x0001U, zeros, sizeof(zeros), true, tx + pos,
			sizeof(tx) - pos);
		assert(len == LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
			LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + 3U);
		pos += len;
	}
	assert(pos <= tx_buffer);

	pos = 0U;
	for (uint8_t i = 0U; i < 5U; i++) {
		assert(linkr_debugger_sigrok_linkr_coalesce_can_append(pos,
			single_1024_frame, tx_buffer, i, 16U));
		size_t len = linkr_debugger_sigrok_linkr_encode_packed_data_frame(i * 1024U,
			1024U, 0x0001U, alternating, sizeof(alternating), true,
			tx + pos, sizeof(tx) - pos);
		assert(len == single_1024_frame);
		pos += len;
	}
	assert(!linkr_debugger_sigrok_linkr_coalesce_can_append(pos,
		single_1024_frame, tx_buffer, 5U, 16U));

	pos = 0U;
	for (uint8_t i = 0U; i < 2U; i++) {
		assert(linkr_debugger_sigrok_linkr_coalesce_can_append(pos,
			single_2048_frame, tx_buffer, i, 16U));
		size_t len = linkr_debugger_sigrok_linkr_encode_packed_data_frame(i *
			LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES,
			LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES, 0x0001U,
			wide_alternating, sizeof(wide_alternating), true, tx + pos,
			sizeof(tx) - pos);
		assert(len == single_2048_frame);
		pos += len;
	}
	assert(!linkr_debugger_sigrok_linkr_coalesce_can_append(pos,
		single_2048_frame, tx_buffer, 2U, 16U));
}

static void test_packed_sender_2048_single_frame_capacity_boundary(void)
{
	uint8_t packed[LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES + 1U];
	uint8_t frame[LINKR_DEBUGGER_SIGROK_LINKR_WS_MAX_FRAME_BYTES];
	size_t len;

	for (size_t i = 0U; i < sizeof(packed); i++) {
		packed[i] = (uint8_t)(i & 1U);
	}
	len = linkr_debugger_sigrok_linkr_encode_packed_data_frame(0U,
		LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES, 0x0001U,
		packed, LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES, true,
		frame, sizeof(frame));
	assert(len == LINKR_DEBUGGER_SIGROK_LINKR_WS_MAX_FRAME_BYTES);
	assert(linkr_debugger_sigrok_linkr_encode_packed_data_frame(0U,
		LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES + 1U, 0x0001U,
		packed, LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES + 1U, true,
		frame, sizeof(frame)) == 0U);
}

static void test_packed_sender_sparse_fast8_1696_tail_frame_encodes(void)
{
	uint8_t packed[1696U];
	uint8_t frame[LINKR_DEBUGGER_SIGROK_LINKR_WS_MAX_FRAME_BYTES];
	struct linkr_debugger_sigrok_linkr_header header;
	size_t len;

	memset(packed, 0xa5, sizeof(packed));
	len = linkr_debugger_sigrok_linkr_encode_packed_data_frame(98304U, 1696U,
		0x0089U, packed, sizeof(packed), true, frame, sizeof(frame));
	assert(len > LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES);
	assert(len <= LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + sizeof(packed));
	assert(linkr_debugger_sigrok_linkr_decode_header(frame,
		LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES, &header) == 0);
	assert(header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_DATA);
	assert(header.payload_len > LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES);
	assert(header.payload_len <= LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES +
		sizeof(packed));
}

static void test_stream_queue_capacity_reserves_terminal_slot(void)
{
	assert(LINKR_DEBUGGER_SIGROK_LINKR_STREAM_QDEPTH_LIMIT == 32U);
	assert(linkr_debugger_sigrok_linkr_stream_queue_has_capacity(0U, false));
	assert(linkr_debugger_sigrok_linkr_stream_queue_has_capacity(31U, false));
	assert(!linkr_debugger_sigrok_linkr_stream_queue_has_capacity(32U, false));
	assert(linkr_debugger_sigrok_linkr_stream_queue_has_capacity(30U, true));
	assert(!linkr_debugger_sigrok_linkr_stream_queue_has_capacity(31U, true));
	assert(!linkr_debugger_sigrok_linkr_stream_queue_has_capacity(32U, true));
}

static void test_ws_fixed_pool_capacity_has_8_data_plus_terminal_reserve(void)
{
	assert(LINKR_DEBUGGER_SIGROK_LINKR_WS_DATA_SLOT_COUNT == 8U);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_WS_TERMINAL_SLOT_COUNT == 1U);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_WS_WIDE11_PAYLOAD_BYTES == 2048U);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_WS_MAX_FRAME_BYTES == 2065U);
	assert(linkr_debugger_sigrok_linkr_ws_pool_data_has_capacity(0U, false));
	assert(linkr_debugger_sigrok_linkr_ws_pool_data_has_capacity(7U, false));
	assert(!linkr_debugger_sigrok_linkr_ws_pool_data_has_capacity(8U, false));
	assert(linkr_debugger_sigrok_linkr_ws_pool_data_has_capacity(6U, true));
	assert(!linkr_debugger_sigrok_linkr_ws_pool_data_has_capacity(7U, true));
	assert(linkr_debugger_sigrok_linkr_ws_pool_terminal_has_capacity(false));
	assert(!linkr_debugger_sigrok_linkr_ws_pool_terminal_has_capacity(true));
}

static void test_raw_burst_queue_capacity_and_backpressure_wake_model(void)
{
	uint32_t queued = 0U;

	assert(LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT == 12U);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_MAX_SAMPLES_PER_ITEM == 1024U);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_MAX_FRAME_BYTES == 2065U);
	assert(linkr_debugger_sigrok_linkr_raw_burst_queue_memory_bytes(
		LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT,
		LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_MAX_FRAME_BYTES) <
		LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_QUEUE_MEMORY_LIMIT_BYTES);
	while (queued < LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT) {
		assert(linkr_debugger_sigrok_linkr_raw_burst_queue_has_space(queued,
			LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT));
		queued++;
	}
	assert(!linkr_debugger_sigrok_linkr_raw_burst_queue_has_space(queued,
		LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT));
	queued--;
	assert(linkr_debugger_sigrok_linkr_raw_burst_queue_has_space(queued,
		LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT));
}

static void test_raw_burst_exact_98_frame_accounting(void)
{
	uint32_t emitted = 0U;
	uint32_t frames = 0U;
	uint32_t sample_index = 0U;

	while (emitted < LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES) {
		uint16_t count = linkr_debugger_sigrok_linkr_raw_burst_frame_sample_count(
			emitted, LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES,
			LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_MAX_SAMPLES_PER_ITEM);

		assert(count > 0U);
		assert(count <= LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_MAX_SAMPLES_PER_ITEM);
		assert(sample_index == (emitted & LINKR_DEBUGGER_SIGROK_LINKR_MAX_SAMPLE_INDEX));
		sample_index = linkr_debugger_sigrok_linkr_advance_sample_index(sample_index,
			count);
		emitted += count;
		frames++;
	}
	assert(frames == 98U);
	assert(emitted == 100000U);
	assert(linkr_debugger_sigrok_linkr_raw_burst_frame_sample_count(emitted,
		LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES,
		LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_MAX_SAMPLES_PER_ITEM) == 0U);
}

static void test_ws_burst_reused_slots_deliver_2048_tail_before_stopped_model(void)
{
	enum {
		WS_BURST_MODEL_DATA = 1,
		WS_BURST_MODEL_STOPPED = 2,
	};
	uint8_t order[50];
	uint32_t emitted = 0U;
	uint32_t frames = 0U;
	uint32_t outstanding = 0U;
	uint32_t free_slots = 2U;
	uint32_t slot_use[2] = { 0U, 0U };

	while (emitted < LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES) {
		uint16_t count = linkr_debugger_sigrok_linkr_raw_burst_frame_sample_count(
			emitted, LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES,
			LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES);
		uint32_t slot;

		assert(count > 0U);
		assert(free_slots > 0U);
		free_slots--;
		outstanding++;
		slot = frames % 2U;
		slot_use[slot]++;

		order[frames++] = WS_BURST_MODEL_DATA;
		if (frames <= 48U) {
			assert(count == LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES);
		} else {
			assert(count == 1696U);
		}
		emitted += count;

		assert(outstanding > 0U);
		outstanding--;
		free_slots++;
	}
	order[frames++] = WS_BURST_MODEL_STOPPED;

	assert(frames == 50U);
	assert(emitted == LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES);
	assert(slot_use[0] == 25U);
	assert(slot_use[1] == 24U);
	assert(outstanding == 0U);
	assert(free_slots == 2U);
	for (uint32_t i = 0U; i < frames - 1U; i++) {
		assert(order[i] == WS_BURST_MODEL_DATA);
	}
	assert(order[48] == WS_BURST_MODEL_DATA);
	assert(order[49] == WS_BURST_MODEL_STOPPED);
}

static void test_ws_burst_lease_vs_inflight_terminal_model(void)
{
	uint32_t open_leases = 0U;
	uint32_t inflight_frames = 0U;
	bool terminal_committed = false;
	bool terminal_free = true;
	bool source_decode_complete = false;

	/* A TRIGGERED event that has already entered FIFO must not block STOPPED enqueue. */
	open_leases++;
	assert(open_leases == 1U);
	assert(inflight_frames == 0U);
	open_leases--;
	inflight_frames++;
	assert(open_leases == 0U);
	assert(inflight_frames == 1U);
	assert(open_leases == 0U && terminal_free);

	open_leases++;
	terminal_free = false;
	assert(open_leases == 1U);
	open_leases--;
	inflight_frames++;
	terminal_committed = true;
	source_decode_complete = true;
	assert(terminal_committed);
	assert(inflight_frames == 2U);
	assert(!(source_decode_complete && terminal_committed && terminal_free &&
		open_leases == 0U && inflight_frames == 0U));

	/* Drain requires sender-owned TRIGGERED and terminal frames to release. */
	inflight_frames--;
	assert(inflight_frames == 1U);
	terminal_free = true;
	inflight_frames--;
	assert(source_decode_complete && terminal_committed && terminal_free &&
		open_leases == 0U && inflight_frames == 0U);
}

static void test_ws_burst_open_lease_blocks_terminal_model(void)
{
	uint32_t open_leases = 0U;
	uint32_t inflight_frames = 0U;
	bool terminal_free = true;

	open_leases++;
	assert(!(open_leases == 0U && terminal_free));

	open_leases--;
	inflight_frames++;
	assert(open_leases == 0U && terminal_free);
	assert(inflight_frames == 1U);
}

static void test_ws_burst_stop_pending_waits_for_source_done_and_drained_model(void)
{
	bool stop_pending = true;
	bool abort_pending = false;
	bool burst_active = true;
	bool source_done = false;
	bool terminal_committed = false;
	bool terminal_free = true;
	uint32_t open_leases = 0U;
	uint32_t inflight_frames = 0U;
	bool capture_released = false;

	/* Consumer observes FIFO empty before producer commits the final DATA and STOPPED. */
	assert(stop_pending);
	assert(!abort_pending);
	assert(burst_active);
	assert(!(source_done && terminal_committed && terminal_free && open_leases == 0U &&
		inflight_frames == 0U));
	assert(!capture_released);

	/* Producer then commits final DATA and terminal; normal stop must still wait. */
	inflight_frames += 2U;
	terminal_committed = true;
	terminal_free = false;
	source_done = true;
	assert(stop_pending);
	assert(!(source_done && terminal_committed && terminal_free && open_leases == 0U &&
		inflight_frames == 0U));
	assert(!capture_released);

	/* DATA release alone is insufficient; terminal release completes the drain. */
	inflight_frames--;
	assert(!capture_released);
	terminal_free = true;
	inflight_frames--;
	assert(source_done && terminal_committed && terminal_free && open_leases == 0U &&
		inflight_frames == 0U);
	stop_pending = false;
	capture_released = true;
	assert(capture_released);
}

static void test_ws_burst_drain_failure_abort_pending_releases_immediately_model(void)
{
	bool stop_pending = true;
	bool abort_pending = true;
	bool source_done = true;
	bool terminal_committed = true;
	bool terminal_free = false;
	uint32_t open_leases = 0U;
	uint32_t inflight_frames = 2U;
	bool capture_released = false;

	assert(!(source_done && terminal_committed && terminal_free && open_leases == 0U &&
		inflight_frames == 0U));
	if (abort_pending) {
		abort_pending = false;
		stop_pending = false;
		capture_released = true;
	}

	assert(!abort_pending);
	assert(!stop_pending);
	assert(capture_released);
}

static void test_raw_burst_disconnect_unblocks_waiter_model(void)
{
	bool active = true;
	bool aborted = false;
	uint32_t queued = LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT;

	assert(!linkr_debugger_sigrok_linkr_raw_burst_queue_has_space(queued,
		LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT));
	aborted = true;
	active = false;
	queued--;
	assert(aborted);
	assert(!active);
	assert(linkr_debugger_sigrok_linkr_raw_burst_queue_has_space(queued,
		LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT));
}

static void test_raw_burst_terminal_order_model(void)
{
	enum {
		RAW_BURST_MODEL_DATA = 1,
		RAW_BURST_MODEL_TERMINAL = 2,
	};
	uint8_t order[99];
	uint32_t emitted = 0U;
	uint32_t frames = 0U;

	while (emitted < LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES) {
		uint16_t count = linkr_debugger_sigrok_linkr_raw_burst_frame_sample_count(
			emitted, LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES,
			LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_MAX_SAMPLES_PER_ITEM);

		order[frames++] = RAW_BURST_MODEL_DATA;
		emitted += count;
	}
	order[frames++] = RAW_BURST_MODEL_TERMINAL;
	assert(frames == 99U);
	for (uint32_t i = 0U; i < frames - 1U; i++) {
		assert(order[i] == RAW_BURST_MODEL_DATA);
	}
	assert(order[frames - 1U] == RAW_BURST_MODEL_TERMINAL);
}

static void test_raw_burst_triggered_exact_order_and_one_shot_model(void)
{
	enum {
		RAW_BURST_MODEL_START_RESP = 1,
		RAW_BURST_MODEL_ARMED = 2,
		RAW_BURST_MODEL_TRIGGERED = 3,
		RAW_BURST_MODEL_DATA = 4,
		RAW_BURST_MODEL_STOPPED = 5,
	};
	uint8_t order[102];
	uint32_t emitted = 0U;
	uint32_t frames = 0U;
	bool triggered_committed = false;
	uint32_t triggered_count = 0U;

	assert(!linkr_debugger_sigrok_linkr_raw_burst_should_emit_triggered_event(
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE, false));
	assert(linkr_debugger_sigrok_linkr_raw_burst_should_emit_triggered_event(
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING, false));
	assert(!linkr_debugger_sigrok_linkr_raw_burst_should_emit_triggered_event(
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING, true));

	order[frames++] = RAW_BURST_MODEL_START_RESP;
	order[frames++] = RAW_BURST_MODEL_ARMED;
	while (emitted < LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES) {
		uint16_t count = linkr_debugger_sigrok_linkr_raw_burst_frame_sample_count(
			emitted, LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES,
			LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_MAX_SAMPLES_PER_ITEM);

		if (linkr_debugger_sigrok_linkr_raw_burst_should_emit_triggered_event(
		    LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING, triggered_committed)) {
			order[frames++] = RAW_BURST_MODEL_TRIGGERED;
			triggered_committed = true;
			triggered_count++;
		}
		order[frames++] = RAW_BURST_MODEL_DATA;
		emitted += count;
	}
	order[frames++] = RAW_BURST_MODEL_STOPPED;

	assert(frames == 102U);
	assert(triggered_count == 1U);
	assert(order[0] == RAW_BURST_MODEL_START_RESP);
	assert(order[1] == RAW_BURST_MODEL_ARMED);
	assert(order[2] == RAW_BURST_MODEL_TRIGGERED);
	for (uint32_t i = 3U; i < frames - 1U; i++) {
		assert(order[i] == RAW_BURST_MODEL_DATA);
	}
	assert(order[frames - 1U] == RAW_BURST_MODEL_STOPPED);
}

static void test_ws_slot_state_contract_returns_slots_exactly_once(void)
{
	assert(linkr_debugger_sigrok_linkr_ws_slot_transition_valid(
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE,
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED));
	assert(linkr_debugger_sigrok_linkr_ws_slot_transition_valid(
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED,
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED));
	assert(linkr_debugger_sigrok_linkr_ws_slot_transition_valid(
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED,
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED));
	assert(linkr_debugger_sigrok_linkr_ws_slot_transition_valid(
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED,
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE));
	assert(linkr_debugger_sigrok_linkr_ws_slot_transition_valid(
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED,
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE));
	assert(!linkr_debugger_sigrok_linkr_ws_slot_transition_valid(
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE,
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED));
	assert(!linkr_debugger_sigrok_linkr_ws_slot_transition_valid(
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED,
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED));
	assert(!linkr_debugger_sigrok_linkr_ws_slot_transition_valid(
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE,
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE));
}

static void test_ws_slot_commit_gate_requires_popped_matching_owner_generation(void)
{
	assert(linkr_debugger_sigrok_linkr_ws_slot_commit_allowed(
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED, 10U, 20U, 10U, 20U));
	assert(!linkr_debugger_sigrok_linkr_ws_slot_commit_allowed(
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE, 10U, 20U, 10U, 20U));
	assert(!linkr_debugger_sigrok_linkr_ws_slot_commit_allowed(
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED, 10U, 20U, 10U, 20U));
	assert(!linkr_debugger_sigrok_linkr_ws_slot_commit_allowed(
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED, 0U, 20U, 0U, 20U));
	assert(!linkr_debugger_sigrok_linkr_ws_slot_commit_allowed(
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED, 10U, 20U, 11U, 20U));
	assert(!linkr_debugger_sigrok_linkr_ws_slot_commit_allowed(
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED, 10U, 20U, 10U, 21U));
}

static void test_stream_queue_byte_capacity_enforces_36kib_boundary(void)
{
	const size_t cap = 36U * 1024U;

	assert(linkr_debugger_sigrok_linkr_stream_queue_bytes_has_capacity(0U, cap,
		cap, false, 0U));
	assert(!linkr_debugger_sigrok_linkr_stream_queue_bytes_has_capacity(1U, cap,
		cap, false, 0U));
	assert(linkr_debugger_sigrok_linkr_stream_queue_bytes_has_capacity(cap - 64U,
		64U, cap, false, 0U));
	assert(!linkr_debugger_sigrok_linkr_stream_queue_bytes_has_capacity(cap - 63U,
		64U, cap, false, 0U));
	assert(!linkr_debugger_sigrok_linkr_stream_queue_bytes_has_capacity(cap + 1U,
		1U, cap, false, 0U));
	assert(!linkr_debugger_sigrok_linkr_stream_queue_bytes_has_capacity(0U, 0U,
		cap, false, 0U));
}

static void test_stream_queue_byte_capacity_reserves_terminal_event(void)
{
	const size_t cap = 36U * 1024U;
	const size_t terminal = 48U;

	assert(linkr_debugger_sigrok_linkr_stream_queue_bytes_has_capacity(cap - 128U,
		80U, cap, true, terminal));
	assert(!linkr_debugger_sigrok_linkr_stream_queue_bytes_has_capacity(cap - 127U,
		80U, cap, true, terminal));
	assert(!linkr_debugger_sigrok_linkr_stream_queue_bytes_has_capacity(cap - 128U,
		80U, cap, true, 0U));
}

static void test_stream_wake_policy_defers_below_depth_and_arms_timeout(void)
{
	assert(LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_QDEPTH == 8U);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_TIMEOUT_MS == 8U);
	assert(linkr_debugger_sigrok_linkr_stream_wake_policy(0U, false, false,
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_QDEPTH) ==
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_DEFER);
	assert(linkr_debugger_sigrok_linkr_stream_wake_policy(1U, false, false,
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_QDEPTH) ==
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_DELAY);
	assert(linkr_debugger_sigrok_linkr_stream_wake_policy(7U, false, true,
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_QDEPTH) ==
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_DEFER);
}

static void test_stream_wake_policy_wakes_at_depth8_and_urgent(void)
{
	assert(linkr_debugger_sigrok_linkr_stream_wake_policy(8U, false, false,
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_QDEPTH) ==
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_NOW);
	assert(linkr_debugger_sigrok_linkr_stream_wake_policy(32U, false, true,
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_QDEPTH) ==
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_NOW);
	assert(linkr_debugger_sigrok_linkr_stream_wake_policy(0U, true, false,
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_QDEPTH) ==
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_NOW);
	assert(linkr_debugger_sigrok_linkr_stream_wake_policy(1U, true, true,
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_QDEPTH) ==
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_NOW);
}

static void test_stream_sink_handoff_requested_at_depth2(void)
{
	assert(LINKR_DEBUGGER_SIGROK_LINKR_STREAM_HANDOFF_QDEPTH == 2U);
	assert(!linkr_debugger_sigrok_linkr_stream_sink_handoff_requested(1U));
	assert(linkr_debugger_sigrok_linkr_stream_sink_handoff_requested(2U));
	assert(linkr_debugger_sigrok_linkr_stream_sink_handoff_requested(3U));
}

static void test_ws_transport_metrics_track_maxima_and_reset(void)
{
	struct linkr_debugger_sigrok_linkr_ws_transport_metrics metrics;

	memset(&metrics, 0, sizeof(metrics));
	linkr_debugger_sigrok_linkr_ws_transport_metrics_update_enqueue(&metrics, 3U, 300U);
	linkr_debugger_sigrok_linkr_ws_transport_metrics_update_enqueue(&metrics, 2U, 400U);
	assert(metrics.max_qdepth == 3U);
	assert(metrics.max_qbytes == 400U);

	linkr_debugger_sigrok_linkr_ws_transport_metrics_update_send(&metrics, 10U, 4U, 1024U);
	linkr_debugger_sigrok_linkr_ws_transport_metrics_update_send(&metrics, 9U, 5U, 2048U);
	linkr_debugger_sigrok_linkr_ws_transport_metrics_update_send(&metrics, 12U, 2U, 512U);
	assert(metrics.max_send_us == 12U);
	assert(metrics.max_send_frames == 2U);
	assert(metrics.max_send_bytes == 512U);

	linkr_debugger_sigrok_linkr_ws_transport_metrics_update_drain(&metrics, 20U, 6U, 3000U);
	linkr_debugger_sigrok_linkr_ws_transport_metrics_update_drain(&metrics, 19U, 7U, 4000U);
	linkr_debugger_sigrok_linkr_ws_transport_metrics_update_drain(&metrics, 21U, 1U, 100U);
	assert(metrics.max_drain_us == 21U);
	assert(metrics.max_drain_items == 1U);
	assert(metrics.max_drain_bytes == 100U);

	linkr_debugger_sigrok_linkr_ws_transport_metrics_reset(&metrics);
	assert(metrics.max_qdepth == 0U);
	assert(metrics.max_qbytes == 0U);
	assert(metrics.max_send_us == 0U);
	assert(metrics.max_drain_us == 0U);
	linkr_debugger_sigrok_linkr_ws_transport_metrics_update_enqueue(NULL, 9U, 9U);
	linkr_debugger_sigrok_linkr_ws_transport_metrics_update_send(NULL, 9U, 9U, 9U);
	linkr_debugger_sigrok_linkr_ws_transport_metrics_update_drain(NULL, 9U, 9U, 9U);
	linkr_debugger_sigrok_linkr_ws_transport_metrics_reset(NULL);
}

static void test_sample_range_fits_24_bit_meta(void)
{
	assert(linkr_debugger_sigrok_linkr_sample_range_fits(0U, 0U));
	assert(linkr_debugger_sigrok_linkr_sample_range_fits(
		LINKR_DEBUGGER_SIGROK_LINKR_MAX_SAMPLE_INDEX, 0U));
	assert(linkr_debugger_sigrok_linkr_sample_range_fits(
		LINKR_DEBUGGER_SIGROK_LINKR_MAX_SAMPLE_INDEX, 1U));
	assert(linkr_debugger_sigrok_linkr_sample_range_fits(
		LINKR_DEBUGGER_SIGROK_LINKR_MAX_SAMPLE_INDEX - 9U, 10U));
	assert(!linkr_debugger_sigrok_linkr_sample_range_fits(
		LINKR_DEBUGGER_SIGROK_LINKR_MAX_SAMPLE_INDEX + 1U, 0U));
	assert(linkr_debugger_sigrok_linkr_sample_range_fits(
		LINKR_DEBUGGER_SIGROK_LINKR_MAX_SAMPLE_INDEX, 2U));
	assert(linkr_debugger_sigrok_linkr_sample_range_fits(
		LINKR_DEBUGGER_SIGROK_LINKR_MAX_SAMPLE_INDEX - 9U, 11U));
	assert(linkr_debugger_sigrok_linkr_advance_sample_index(0U, 5U) == 5U);
	assert(linkr_debugger_sigrok_linkr_advance_sample_index(
		LINKR_DEBUGGER_SIGROK_LINKR_MAX_SAMPLE_INDEX - 1U, 4U) == 2U);
}

static void test_bounded_capture_helpers(void)
{
	struct linkr_debugger_sigrok_linkr_session session;

	linkr_debugger_sigrok_linkr_session_reset(&session);
	session.config.post_samples = 0U;
	assert(linkr_debugger_sigrok_linkr_bounded_chunk_count(&session, 1024U) == 1024U);
	assert(!linkr_debugger_sigrok_linkr_bounded_capture_done(&session));

	session.config.post_samples = 1500U;
	session.emitted_samples = 1000U;
	assert(linkr_debugger_sigrok_linkr_bounded_chunk_count(&session, 1024U) == 500U);
	assert(!linkr_debugger_sigrok_linkr_bounded_capture_done(&session));
	session.emitted_samples = 1500U;
	assert(linkr_debugger_sigrok_linkr_bounded_chunk_count(&session, 1024U) == 0U);
	assert(linkr_debugger_sigrok_linkr_bounded_capture_done(&session));

	session.config.post_samples = UINT16_MAX;
	session.emitted_samples = UINT16_MAX - 10U;
	assert(linkr_debugger_sigrok_linkr_bounded_chunk_count(&session, 1024U) == 10U);

	session.config.pre_samples = 16U;
	session.config.post_samples = 16U;
	session.emitted_samples = 0U;
	assert(linkr_debugger_sigrok_linkr_bounded_sample_target(&session) == 32U);
	assert(linkr_debugger_sigrok_linkr_trigger_sample_index(&session) == 16U);
	assert(linkr_debugger_sigrok_linkr_bounded_chunk_count(&session, 32U) == 32U);
	assert(!linkr_debugger_sigrok_linkr_bounded_capture_done(&session));
	session.emitted_samples = 31U;
	assert(!linkr_debugger_sigrok_linkr_bounded_capture_done(&session));
	session.emitted_samples = 32U;
	assert(linkr_debugger_sigrok_linkr_bounded_capture_done(&session));
}

static void test_coalesce_helper_limits_count_and_buffer_capacity(void)
{
	const size_t tx_buffer = 6144U;
	const size_t single_frame = LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + 1024U;
	const size_t max_frame = LINKR_DEBUGGER_SIGROK_LINKR_WS_MAX_FRAME_BYTES;

	assert(single_frame == 1041U);
	assert(max_frame == 2065U);
	assert(linkr_debugger_sigrok_linkr_coalesce_can_append(0U, single_frame,
		tx_buffer, 0U, 16U));
	assert(linkr_debugger_sigrok_linkr_coalesce_can_append(single_frame * 4U,
		single_frame, tx_buffer, 4U, 16U));
	assert(!linkr_debugger_sigrok_linkr_coalesce_can_append(single_frame * 5U,
		single_frame, tx_buffer, 5U, 16U));
	assert(linkr_debugger_sigrok_linkr_coalesce_can_append(0U, max_frame,
		tx_buffer, 0U, 16U));
	assert(linkr_debugger_sigrok_linkr_coalesce_can_append(max_frame, max_frame,
		tx_buffer, 1U, 16U));
	assert(!linkr_debugger_sigrok_linkr_coalesce_can_append(max_frame * 2U,
		max_frame, tx_buffer, 2U, 16U));
	assert(!linkr_debugger_sigrok_linkr_coalesce_can_append(0U, 1U,
		tx_buffer, 16U, 16U));
	assert(!linkr_debugger_sigrok_linkr_coalesce_can_append(0U, 0U,
		tx_buffer, 0U, 16U));
}

static void test_local_terminal_event_helper_skips_failed_websocket_send(void)
{
	assert(linkr_debugger_sigrok_linkr_should_emit_local_terminal_event(true, 0));
	assert(linkr_debugger_sigrok_linkr_should_emit_local_terminal_event(true, 1));
	assert(!linkr_debugger_sigrok_linkr_should_emit_local_terminal_event(true, -EIO));
	assert(!linkr_debugger_sigrok_linkr_should_emit_local_terminal_event(false, 0));
}

static void test_arbiter_sigrok_linkr_owner(void)
{
	linkr_debugger_capture_arbiter_reset();
	assert(linkr_debugger_capture_arbiter_try_acquire(
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR));
	assert(linkr_debugger_capture_arbiter_owner() ==
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR);
	assert(!linkr_debugger_capture_arbiter_try_acquire(
		LINKR_DEBUGGER_CAPTURE_OWNER_LOGIC_ANALYZER));
	assert(linkr_debugger_capture_arbiter_release(
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR));
}

int main(void)
{
	test_header_roundtrip();
	test_decode_next_request_frame_rejects_truncated_boundaries();
	test_decode_next_request_frame_rejects_overlong_control_payload();
	test_decode_next_request_frame_accepts_config_v2_control_payload();
	test_decode_next_request_frame_processes_concatenated_fifo();
	test_decode_next_request_frame_rejects_extra_nonframe_tail();
	test_validate_header_bad_magic();
	test_validate_header_bad_version();
	test_validate_header_oversize();
	test_validate_request_hello_no_payload();
	test_validate_request_hello_with_payload();
	test_validate_request_config_lengths_are_versioned();
	test_decode_config_v1_keeps_12_byte_uint16_samples();
	test_decode_config_v2_accepts_u32_samples_and_zero_post_sentinel();
	test_decode_config_rejects_unknown_lengths_cleanly();
	test_validate_config_fast8();
	test_validate_config_wide11();
	test_validate_config_fast8_too_fast();
	test_validate_config_trigger_channel_not_in_mask();
	test_validate_config_accepts_only_bounded_pre_trigger_contract();
	test_to_la_config_fast8_single_mask();
	test_to_la_config_fast8_sparse_mask();
	test_to_la_config_fast8_full_mask();
	test_to_la_config_wide11_sparse_mask_with_bit10();
	test_to_la_config_wide11_full_mask();
	test_to_la_config_rejects_unselected_trigger_bit();
	test_to_la_config_preserves_bounded_pre_trigger();
	test_to_la_config_rejects_infeasible_bounded_pre_trigger();
	test_handle_config_accepts_bounded_pre_trigger();
	test_handle_hello();
	test_hello_flags_distinguish_legacy_config_v2_from_generic_packed();
	test_handle_caps();
	test_handle_config();
	test_handle_config_v2_fast8_large_post_prepares_on_start();
	test_handle_config_v2_exact_wide11_large_post_prepares_on_start();
	test_handle_config_v2_wide11_125mhz_large_post_rejects_on_start();
	test_handle_config_v2_sparse_wide11_125mhz_rejects_on_start();
	test_handle_config_v2_exact_wide11_triggered_prepares_with_armed_event();
	test_handle_config_v2_preserves_zero_post_start_behavior();
	test_handle_config_v2_sparse_fast8_125mhz_large_post_prepares_on_start();
	test_handle_config_v2_rejects_post_above_packed_burst_capacity_on_start();
	test_handle_config_ack_uses_current_request_rate();
	test_handle_start_immediate();
	test_handle_start_armed();
	test_handle_stop();
	test_handle_stop_idempotent_when_configured();
	test_handle_stop_preserves_same_socket_config_start_reuse();
	test_handle_start_busy();
	test_start_error_code_mapping();
	test_tcp_exact_prepare_order_none_and_triggered();
	test_ws_exact_prepare_order_none_and_triggered();
	test_exact_prepare_cancels_without_go_on_response_failure();
	test_exact_prepare_cancels_without_go_on_armed_event_failure();
	test_ws_exact_prepare_partial_cleanup_releases_arena_and_owner();
	test_generic_packed_burst_prepare_acquires_arena_for_fast8_and_post0();
	test_low_rate_post513_prepare_uses_ordinary_stream_without_arena();
	test_start_failure_rollback_and_error_response();
	test_session_reset();
	test_caps_init();
	test_bytes_per_sample_by_mode_width();
	test_bit_pack_single_fast_path_encodes_one_byte_per_sample();
	test_packed_data_len_is_exact_by_channel_width();
	test_bit_pack_rle_encodes_idle_1_byte_units();
	test_bit_pack_rle_single_encodes_idle_1024_sample_chunks();
	test_bit_pack_rle_encodes_uart_like_runs();
	test_bit_pack_rle_single_encodes_uart_like_runs();
	test_bit_pack_rle_alternating_falls_back_when_no_benefit();
	test_bit_pack_rle_single_run_length_byte_order_and_capacity();
	test_bit_pack_rle_encodes_2_byte_units_with_channel_mapping();
	test_packed_wide11_sender_prefers_bit_pack_rle_when_smaller();
	test_packed_single_sender_rle_frame_decodes_to_original_bytes();
	test_packed_single_sender_fallback_frame_matches_bit_pack_bytes();
	test_packed_sender_rle_helper_falls_back_before_growth();
	test_packed_sender_rle_single_fast_path_matches_reference_patterns();
	test_packed_sender_rle_single_fast_path_splits_uint16_runs();
	test_packed_sender_rle_single_fast_path_matches_reference_randomized();
	test_packed_sender_multiple_deferred_frames_fit_tx_buffer();
	test_packed_sender_2048_single_frame_capacity_boundary();
	test_packed_sender_sparse_fast8_1696_tail_frame_encodes();
	test_stream_queue_capacity_reserves_terminal_slot();
	test_ws_fixed_pool_capacity_has_8_data_plus_terminal_reserve();
	test_raw_burst_queue_capacity_and_backpressure_wake_model();
	test_raw_burst_exact_98_frame_accounting();
	test_ws_burst_reused_slots_deliver_2048_tail_before_stopped_model();
	test_ws_burst_lease_vs_inflight_terminal_model();
	test_ws_burst_open_lease_blocks_terminal_model();
	test_ws_burst_stop_pending_waits_for_source_done_and_drained_model();
	test_ws_burst_drain_failure_abort_pending_releases_immediately_model();
	test_raw_burst_disconnect_unblocks_waiter_model();
	test_raw_burst_terminal_order_model();
	test_raw_burst_triggered_exact_order_and_one_shot_model();
	test_ws_slot_state_contract_returns_slots_exactly_once();
	test_ws_slot_commit_gate_requires_popped_matching_owner_generation();
	test_stream_queue_byte_capacity_enforces_36kib_boundary();
	test_stream_queue_byte_capacity_reserves_terminal_event();
	test_stream_wake_policy_defers_below_depth_and_arms_timeout();
	test_stream_wake_policy_wakes_at_depth8_and_urgent();
	test_stream_sink_handoff_requested_at_depth2();
	test_ws_transport_metrics_track_maxima_and_reset();
	test_sample_range_fits_24_bit_meta();
	test_bounded_capture_helpers();
	test_coalesce_helper_limits_count_and_buffer_capacity();
	test_local_terminal_event_helper_skips_failed_websocket_send();
	test_arbiter_sigrok_linkr_owner();
	return 0;
}
