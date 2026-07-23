/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_capture_engine.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

static void test_backend_selection_and_capacity(void)
{
	struct linkr_debugger_capture_engine_layout layout;
	uint8_t fast[] = { 10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U };
	uint8_t wide[] = { 10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U, 19U, 20U, 29U };
	uint8_t sparse[] = { 7U, 8U, 9U };

	assert(linkr_debugger_capture_engine_arena_size() == 100000U);
	assert(linkr_debugger_capture_engine_select_backend(fast, sizeof(fast), &layout) == 0);
	assert(layout.backend == LINKR_DEBUGGER_CAPTURE_ENGINE_BACKEND_FAST8);
	assert(layout.bytes_per_sample == 1U);
	assert(layout.storage_bytes_per_sample == 4U);
	assert(layout.max_rate_hz == 50000000U);
	assert(layout.capacity_samples == 16384U);

	assert(linkr_debugger_capture_engine_select_backend(wide, sizeof(wide), &layout) == 0);
	assert(layout.backend == LINKR_DEBUGGER_CAPTURE_ENGINE_BACKEND_WIDE16);
	assert(layout.bytes_per_sample == 2U);
	assert(layout.storage_bytes_per_sample == 4U);
	assert(layout.max_rate_hz == 25000000U);
	assert(layout.capacity_samples == 16384U);

	assert(linkr_debugger_capture_engine_select_backend(sparse, sizeof(sparse), &layout) == 0);
	assert(layout.backend == LINKR_DEBUGGER_CAPTURE_ENGINE_BACKEND_SPARSE16);
	assert(layout.bytes_per_sample == 2U);
	assert(layout.max_rate_hz == 25000000U);
}

static void test_window_wrap_and_trigger_index_contract(void)
{
	struct linkr_debugger_capture_engine_window window;

	assert(linkr_debugger_capture_engine_plan_window(12U, 16U, 4U, 6U, &window) == 0);
	assert(window.start_index == 8U);
	assert(window.sample_count == 10U);
	assert(window.trigger_index == 4U);

	assert(linkr_debugger_capture_engine_plan_window(2U, 16U, 5U, 3U, &window) == 0);
	assert(window.start_index == 13U);
	assert(window.sample_count == 8U);
	assert(window.trigger_index == 5U);

	assert(linkr_debugger_capture_engine_plan_window(0U, 16U, 8U, 8U, &window) == 0);
	assert(window.start_index == 8U);
	assert(window.sample_count == 16U);
	assert(window.trigger_index == 8U);
	assert(linkr_debugger_capture_engine_plan_window(0U, 16U, 8U, 9U, &window) == -EINVAL);
}

static void test_binary_contract_helpers(void)
{
	uint16_t samples[] = { 0x0001U, 0x12a5U, 0xff00U };
	uint8_t packed[sizeof(samples)];

	memset(packed, 0, sizeof(packed));
	assert(linkr_debugger_capture_engine_sample_bytes(100000U, 1U) == 100000U);
	assert(linkr_debugger_capture_engine_sample_bytes(50000U, 2U) == 100000U);
	assert(linkr_debugger_capture_engine_sample_bytes(50001U, 2U) == 0U);
	assert(linkr_debugger_capture_engine_pack_u16_le(packed, sizeof(packed), samples, 3U) == 0);
	assert(packed[0] == 0x01U);
	assert(packed[1] == 0x00U);
	assert(packed[2] == 0xa5U);
	assert(packed[3] == 0x12U);
	assert(packed[4] == 0x00U);
	assert(packed[5] == 0xffU);
	assert(linkr_debugger_capture_engine_pack_u16_le(packed, 5U, samples, 3U) == -EINVAL);
}

static void test_marked_window_export_wrap_and_nonzero_pre(void)
{
	uint32_t ring[8] = {
		0x10U, 0x11U, 0x12U, 0x13U,
		0x20U, 0x21U, 0x22U, 0x23U,
	};
	uint8_t out[5];
	uint32_t sample_bytes;

	memset(out, 0, sizeof(out));
	assert(linkr_debugger_capture_engine_export_marked_window(out, sizeof(out),
		ring, 8U, 6U, 2U, 3U, 1U, 0xffU, &sample_bytes) == 0);
	assert(sample_bytes == 5U);
	assert(out[0] == 0x20U);
	assert(out[1] == 0x21U);
	assert(out[2] == 0x22U);
	assert(out[3] == 0x23U);
	assert(out[4] == 0x10U);
	assert(out[0] != 0U);
	assert(out[1] != 0U);
}

static void test_marked_window_export_off_by_one_and_u16(void)
{
	uint32_t ring[8] = {
		0x0100U, 0x0101U, 0x0102U, 0x0103U,
		0x0104U, 0x0105U, 0x0106U, 0x0107U,
	};
	uint8_t out[8];
	uint32_t sample_bytes;

	memset(out, 0, sizeof(out));
	assert(linkr_debugger_capture_engine_export_marked_window(out, sizeof(out),
		ring, 8U, 5U, 1U, 3U, 2U, 0x0fffU, &sample_bytes) == 0);
	assert(sample_bytes == 8U);
	assert(out[0] == 0x04U);
	assert(out[1] == 0x01U);
	assert(out[2] == 0x05U);
	assert(out[3] == 0x01U);
	assert(out[4] == 0x06U);
	assert(out[5] == 0x01U);
	assert(out[6] == 0x07U);
	assert(out[7] == 0x01U);
}

static void test_logic_session_no_trigger_immediate_eligibility(void)
{
	struct linkr_debugger_logic_session_config config;
	struct linkr_debugger_logic_session_chunk chunk;
	struct linkr_debugger_logic_session_status status;
	uint32_t *arena = (uint32_t *)linkr_debugger_capture_engine_arena();
	uint8_t out[4];

	linkr_debugger_logic_session_init();
	memset(&config, 0, sizeof(config));
	config.owner = LINKR_DEBUGGER_LOGIC_SESSION_OWNER_WS;
	config.trigger = LINKR_DEBUGGER_LOGIC_SESSION_TRIGGER_NONE;
	config.session_id = 10U;
	config.ring_samples = 8U;
	config.bytes_per_sample = 1U;
	config.backend = "host-fast8";
	assert(linkr_debugger_logic_session_start(&config) == 0);
	for (uint32_t i = 0U; i < 4U; i++) {
		arena[i] = 0x30U + i;
	}
	assert(linkr_debugger_logic_session_commit(4U, false, 0U) == 0);
	assert(linkr_debugger_logic_session_read(out, sizeof(out), 4U, &chunk) == 0);
	assert(chunk.first_seq == 0U);
	assert(chunk.sample_count == 4U);
	assert(!chunk.has_trigger);
	assert(memcmp(out, (uint8_t[]){0x30U, 0x31U, 0x32U, 0x33U}, 4U) == 0);
	linkr_debugger_logic_session_get_status(&status);
	assert(status.last_sent_seq == 4U);
	assert(status.last_captured_seq == 4U);
}

static void test_logic_session_triggered_prehistory_wrap_and_trigger_once(void)
{
	struct linkr_debugger_logic_session_config config;
	struct linkr_debugger_logic_session_chunk chunk;
	struct linkr_debugger_logic_session_status status;
	uint32_t *arena = (uint32_t *)linkr_debugger_capture_engine_arena();
	uint8_t out[6];

	linkr_debugger_logic_session_init();
	memset(&config, 0, sizeof(config));
	config.owner = LINKR_DEBUGGER_LOGIC_SESSION_OWNER_WS;
	config.trigger = LINKR_DEBUGGER_LOGIC_SESSION_TRIGGER_EDGE;
	config.session_id = 11U;
	config.pre_samples = 3U;
	config.ring_samples = 8U;
	config.bytes_per_sample = 1U;
	config.backend = "host-fast8";
	assert(linkr_debugger_logic_session_start(&config) == 0);

	for (uint32_t seq = 0U; seq < 8U; seq++) {
		arena[seq % 8U] = 0xa0U + seq;
	}
	assert(linkr_debugger_logic_session_commit(8U, false, 0U) == 0);
	assert(linkr_debugger_logic_session_read(out, sizeof(out), 6U, &chunk) == 0);
	assert(chunk.sample_count == 0U);

	for (uint32_t seq = 8U; seq < 12U; seq++) {
		arena[seq % 8U] = 0xa0U + seq;
	}
	assert(linkr_debugger_logic_session_commit(4U, true, 2U) == 0);
	assert(linkr_debugger_logic_session_read(out, sizeof(out), 6U, &chunk) == 0);
	assert(chunk.first_seq == 7U);
	assert(chunk.sample_count == 5U);
	assert(chunk.has_trigger);
	assert(chunk.trigger_offset == 3U);
	assert(out[0] == 0xa7U);
	assert(out[1] == 0xa8U);
	assert(out[2] == 0xa9U);
	assert(out[3] == 0xaaU);
	assert(out[4] == 0xabU);
	assert(out[0] != 0U && out[1] != 0U && out[2] != 0U);

	for (uint32_t seq = 12U; seq < 14U; seq++) {
		arena[seq % 8U] = 0xa0U + seq;
	}
	assert(linkr_debugger_logic_session_commit(2U, false, 0U) == 0);
	assert(linkr_debugger_logic_session_read(out, sizeof(out), 6U, &chunk) == 0);
	assert(chunk.first_seq == 12U);
	assert(chunk.sample_count == 2U);
	assert(!chunk.has_trigger);
	assert(out[0] == 0xacU);
	assert(out[1] == 0xadU);
	linkr_debugger_logic_session_get_status(&status);
	assert(status.state == LINKR_DEBUGGER_LOGIC_SESSION_LIVE_TRIGGERED);
	assert(status.trigger_seq == 10U);
}

static void test_logic_session_stop_disconnect_and_overrun(void)
{
	struct linkr_debugger_logic_session_config config;
	struct linkr_debugger_logic_session_status status;

	linkr_debugger_logic_session_init();
	memset(&config, 0, sizeof(config));
	config.owner = LINKR_DEBUGGER_LOGIC_SESSION_OWNER_BEAGLELOGIC;
	config.trigger = LINKR_DEBUGGER_LOGIC_SESSION_TRIGGER_NONE;
	config.session_id = 12U;
	config.ring_samples = 4U;
	config.bytes_per_sample = 2U;
	config.backend = "host-wide12";
	assert(linkr_debugger_logic_session_start(&config) == 0);
	assert(linkr_debugger_logic_session_disconnect(
		LINKR_DEBUGGER_LOGIC_SESSION_OWNER_BEAGLELOGIC, 12U) == 0);
	linkr_debugger_logic_session_get_status(&status);
	assert(status.state == LINKR_DEBUGGER_LOGIC_SESSION_STOPPING);
	assert(status.terminal);
	assert(strcmp(status.reason, "disconnect") == 0);

	linkr_debugger_logic_session_init();
	assert(linkr_debugger_logic_session_start(&config) == 0);
	assert(linkr_debugger_logic_session_commit(4U, false, 0U) == 0);
	assert(linkr_debugger_logic_session_commit(1U, false, 0U) == -EOVERFLOW);
	linkr_debugger_logic_session_get_status(&status);
	assert(status.state == LINKR_DEBUGGER_LOGIC_SESSION_ERROR);
	assert(status.overrun);
	assert(status.last_captured_seq == 5U);
}

int main(void)
{
	test_backend_selection_and_capacity();
	test_window_wrap_and_trigger_index_contract();
	test_binary_contract_helpers();
	test_marked_window_export_wrap_and_nonzero_pre();
	test_marked_window_export_off_by_one_and_u16();
	test_logic_session_no_trigger_immediate_eligibility();
	test_logic_session_triggered_prehistory_wrap_and_trigger_once();
	test_logic_session_stop_disconnect_and_overrun();
	return 0;
}
