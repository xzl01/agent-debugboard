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

static void test_validate_config_wide12(void)
{
	struct linkr_debugger_sigrok_linkr_config config = {
		.mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE12,
		.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE,
		.trigger_channel = 0U,
		.channel_mask = 0x0fffU,
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

static void test_validate_config_rejects_pre_samples(void)
{
	struct linkr_debugger_sigrok_linkr_config config = {
		.mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8,
		.trigger_type = LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_RISING,
		.trigger_channel = 0U,
		.channel_mask = 0x0001U,
		.samplerate_khz = 1000U,
		.pre_samples = 1U,
		.post_samples = 1024U,
	};
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;
	uint16_t detail = 0U;

	assert(linkr_debugger_sigrok_linkr_validate_config(&config, &error_code, &detail) == -EINVAL);
	assert(error_code == LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG);
	assert(detail == 1U);
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

static void test_to_la_config_wide12_sparse_mask_with_bit11(void)
{
	const uint8_t pins[] = { 11U, 14U, 20U, 29U };

	assert_la_mapping(LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE12,
		0x0c12U, 11U, 3U, 12U, pins, 4U);
}

static void test_to_la_config_wide12_full_mask(void)
{
	const uint8_t pins[] = { 10U, 11U, 12U, 13U, 14U, 15U,
		16U, 17U, 18U, 19U, 20U, 29U };

	assert_la_mapping(LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE12,
		0x0fffU, 11U, 11U, 12U, pins, 12U);
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
	assert(session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY);
	assert(!action.has_event);
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
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_START_IMMEDIATE);
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
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_START_ARMED);
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
	assert(action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_START_IMMEDIATE);
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

static void test_start_failure_rollback_and_error_response(void)
{
	struct linkr_debugger_sigrok_linkr_session session;
	struct linkr_debugger_sigrok_linkr_action_result action = {
		.capture_action = LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_START_IMMEDIATE,
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
		LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES);
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
	assert((caps.modes[0].mode_flags & LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_PRE_TRIGGER) == 0U);
	assert(caps.modes[0].sample_bytes == 1U);
	assert(caps.modes[0].max_samplerate_khz == 125000U);
	assert(caps.modes[1].mode_id == LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE12);
	assert(caps.modes[1].channel_count == 12U);
	assert((caps.modes[1].mode_flags & LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_PRE_TRIGGER) == 0U);
	assert(caps.modes[1].sample_bytes == 2U);
	assert(caps.modes[1].max_samplerate_khz == 125000U);
}

static void test_bytes_per_sample_by_mode_width(void)
{
	uint16_t samples[2] = { 0x0001U, 0x0f01U };
	uint8_t out[4];

	assert(linkr_debugger_sigrok_linkr_bytes_per_sample(0x0000U) == 0U);
	assert(linkr_debugger_sigrok_linkr_bytes_per_sample(0x0001U) == 1U);
	assert(linkr_debugger_sigrok_linkr_bytes_per_sample(0x00ffU) == 1U);
	assert(linkr_debugger_sigrok_linkr_bytes_per_sample(0x0fffU) == 2U);
	assert(linkr_debugger_sigrok_linkr_compress_bit_pack(samples, 2U, 0x0fffU,
		out, sizeof(out)) == 4U);
	assert(out[0] == 0x01U);
	assert(out[1] == 0x00U);
	assert(out[2] == 0x01U);
	assert(out[3] == 0x0fU);
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
	assert(linkr_debugger_sigrok_linkr_packed_data_len(0x0fffU, 2048U) == 4096U);
	assert(linkr_debugger_sigrok_linkr_packed_data_len(0x0fffU, 2049U) == 4098U);
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
	uint16_t samples[] = {0x0801U, 0x0801U, 0x0801U, 0x0001U, 0x0001U};
	uint8_t out[10];

	assert(linkr_debugger_sigrok_linkr_compress_bit_pack_rle(samples, 5U, 0x0fffU,
		out, sizeof(out)) == 8U);
	assert(memcmp(out, (uint8_t[]){0x01U, 0x08U, 0x03U, 0x00U, 0x01U, 0x00U,
		0x02U, 0x00U}, 8U) == 0);
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
	assert(LINKR_DEBUGGER_SIGROK_LINKR_WS_WIDE12_PAYLOAD_BYTES == 2048U);
	assert(LINKR_DEBUGGER_SIGROK_LINKR_WS_MAX_FRAME_BYTES == 2065U);
	assert(linkr_debugger_sigrok_linkr_ws_pool_data_has_capacity(0U, false));
	assert(linkr_debugger_sigrok_linkr_ws_pool_data_has_capacity(7U, false));
	assert(!linkr_debugger_sigrok_linkr_ws_pool_data_has_capacity(8U, false));
	assert(linkr_debugger_sigrok_linkr_ws_pool_data_has_capacity(6U, true));
	assert(!linkr_debugger_sigrok_linkr_ws_pool_data_has_capacity(7U, true));
	assert(linkr_debugger_sigrok_linkr_ws_pool_terminal_has_capacity(false));
	assert(!linkr_debugger_sigrok_linkr_ws_pool_terminal_has_capacity(true));
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
	test_decode_next_request_frame_processes_concatenated_fifo();
	test_decode_next_request_frame_rejects_extra_nonframe_tail();
	test_validate_header_bad_magic();
	test_validate_header_bad_version();
	test_validate_header_oversize();
	test_validate_request_hello_no_payload();
	test_validate_request_hello_with_payload();
	test_validate_config_fast8();
	test_validate_config_wide12();
	test_validate_config_fast8_too_fast();
	test_validate_config_trigger_channel_not_in_mask();
	test_validate_config_rejects_pre_samples();
	test_to_la_config_fast8_single_mask();
	test_to_la_config_fast8_sparse_mask();
	test_to_la_config_fast8_full_mask();
	test_to_la_config_wide12_sparse_mask_with_bit11();
	test_to_la_config_wide12_full_mask();
	test_to_la_config_rejects_unselected_trigger_bit();
	test_handle_hello();
	test_handle_caps();
	test_handle_config();
	test_handle_config_ack_uses_current_request_rate();
	test_handle_start_immediate();
	test_handle_start_armed();
	test_handle_stop();
	test_handle_stop_idempotent_when_configured();
	test_handle_stop_preserves_same_socket_config_start_reuse();
	test_handle_start_busy();
	test_start_error_code_mapping();
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
	test_packed_single_sender_rle_frame_decodes_to_original_bytes();
	test_packed_single_sender_fallback_frame_matches_bit_pack_bytes();
	test_packed_sender_rle_helper_falls_back_before_growth();
	test_packed_sender_rle_single_fast_path_matches_reference_patterns();
	test_packed_sender_rle_single_fast_path_splits_uint16_runs();
	test_packed_sender_rle_single_fast_path_matches_reference_randomized();
	test_packed_sender_multiple_deferred_frames_fit_tx_buffer();
	test_packed_sender_2048_single_frame_capacity_boundary();
	test_stream_queue_capacity_reserves_terminal_slot();
	test_ws_fixed_pool_capacity_has_8_data_plus_terminal_reserve();
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
