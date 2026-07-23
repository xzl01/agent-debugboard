/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_sigrok_linkr.h"
#include "linkr_debugger_capture_arbiter.h"

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

static void test_validate_header_bad_magic(void)
{
	struct linkr_debugger_sigrok_linkr_header header = {
		.magic = 0xdeadbeefU,
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
	config_payload[5] = 0x00U;
	config_payload[6] = 0xa1U;
	config_payload[7] = 0x07U;
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
	assert(caps.mode_count == LINKR_DEBUGGER_SIGROK_LINKR_CAPS_MODE_COUNT);
	assert(caps.modes[0].mode_id == LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8);
	assert(caps.modes[0].channel_count == 8U);
	assert(caps.modes[0].sample_bytes == 1U);
	assert(caps.modes[0].max_samplerate_khz == 125000U);
	assert(caps.modes[1].mode_id == LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE12);
	assert(caps.modes[1].channel_count == 12U);
	assert(caps.modes[1].sample_bytes == 2U);
	assert(caps.modes[1].max_samplerate_khz == 125000U);
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
	test_validate_header_bad_magic();
	test_validate_header_bad_version();
	test_validate_header_oversize();
	test_validate_request_hello_no_payload();
	test_validate_request_hello_with_payload();
	test_validate_config_fast8();
	test_validate_config_wide12();
	test_validate_config_fast8_too_fast();
	test_validate_config_trigger_channel_not_in_mask();
	test_handle_hello();
	test_handle_caps();
	test_handle_config();
	test_handle_start_immediate();
	test_handle_start_armed();
	test_handle_stop();
	test_handle_start_busy();
	test_session_reset();
	test_caps_init();
	test_arbiter_sigrok_linkr_owner();
	return 0;
}
