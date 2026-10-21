/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_serial_model.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(struct linkr_debugger_serial_model) <=
		       LINKR_DEBUGGER_SERIAL_STATIC_MAX,
	       "serial model must stay within the static memory budget");

static const struct linkr_debugger_serial_config config_115200_8n1 = {
	.baud = 115200U,
	.data_bits = 8U,
	.parity = LINKR_DEBUGGER_SERIAL_PARITY_NONE,
	.stop_bits = 1U,
	.flow_control = LINKR_DEBUGGER_SERIAL_FLOW_NONE,
};

static void init_model(struct linkr_debugger_serial_model *model)
{
	assert(linkr_debugger_serial_model_init(model, &config_115200_8n1) == 0);
	assert(model->generation == 1U);
	assert(model->state == LINKR_DEBUGGER_SERIAL_READY);
}

static void register_and_attach(struct linkr_debugger_serial_model *model,
				enum linkr_debugger_serial_sink sink)
{
	assert(linkr_debugger_serial_rx_sink_register(model, sink) == 0);
	assert(linkr_debugger_serial_rx_sink_attach(model, sink) == 0);
}

static void test_init_accepts_aliased_config(void)
{
	struct linkr_debugger_serial_model model;

	memset(&model, 0xa5, sizeof(model));
	model.current = config_115200_8n1;
	assert(linkr_debugger_serial_model_init(&model, &model.current) == 0);
	assert(linkr_debugger_serial_config_equal(
		       &model.current, &config_115200_8n1));
	assert(linkr_debugger_serial_config_equal(
		       &model.last_good, &config_115200_8n1));
	assert(model.generation == 1U);
}

static void test_rx_binary_fanout(void)
{
	static const uint8_t payload[] = { 0x00U, 0xffU, 0x41U, 0x00U, 0x7fU };
	struct linkr_debugger_serial_model model;
	uint8_t output[sizeof(payload)];
	size_t length;
	int sink;

	init_model(&model);
	for (sink = LINKR_DEBUGGER_SERIAL_SINK_CDC;
	     sink < LINKR_DEBUGGER_SERIAL_SINK_COUNT; ++sink) {
		register_and_attach(&model, (enum linkr_debugger_serial_sink)sink);
	}
	assert(linkr_debugger_serial_rx_write(&model, payload, sizeof(payload)) == 0);
	assert(model.rx.head == sizeof(payload));

	for (sink = LINKR_DEBUGGER_SERIAL_SINK_CDC;
	     sink < LINKR_DEBUGGER_SERIAL_SINK_COUNT; ++sink) {
		memset(output, 0x5a, sizeof(output));
		assert(linkr_debugger_serial_rx_read(
			       &model, (enum linkr_debugger_serial_sink)sink,
			       output, sizeof(output), &length) == 0);
		assert(length == sizeof(payload));
		assert(memcmp(output, payload, sizeof(payload)) == 0);
		assert(model.rx.sinks[sink].delivered == sizeof(payload));
		assert(model.rx.sinks[sink].dropped == 0U);
		assert(model.rx.sinks[sink].epoch == 2U);
	}
}

static void test_rx_slow_sink_drops_independently(void)
{
	struct linkr_debugger_serial_model model;
	uint8_t first[4090];
	uint8_t second[20];
	uint8_t output[LINKR_DEBUGGER_SERIAL_RX_CAPACITY];
	size_t length;
	size_t i;

	for (i = 0U; i < sizeof(first); ++i) {
		first[i] = (uint8_t)i;
	}
	for (i = 0U; i < sizeof(second); ++i) {
		second[i] = (uint8_t)(0xe0U + i);
	}

	init_model(&model);
	register_and_attach(&model, LINKR_DEBUGGER_SERIAL_SINK_WS0);
	register_and_attach(&model, LINKR_DEBUGGER_SERIAL_SINK_WS1);
	assert(linkr_debugger_serial_rx_write(&model, first, sizeof(first)) == 0);
	assert(linkr_debugger_serial_rx_read(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_WS0, output,
		       sizeof(output), &length) == 0);
	assert(length == sizeof(first));
	assert(memcmp(output, first, sizeof(first)) == 0);

	assert(linkr_debugger_serial_rx_write(&model, second, sizeof(second)) == 0);
	assert(model.rx.head == sizeof(first) + sizeof(second));
	assert(linkr_debugger_serial_rx_read(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_WS1, output,
		       sizeof(output), &length) == 0);
	assert(length == LINKR_DEBUGGER_SERIAL_RX_CAPACITY);
	assert(memcmp(output, first + 14U, sizeof(first) - 14U) == 0);
	assert(memcmp(output + sizeof(first) - 14U, second, sizeof(second)) == 0);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_WS1].dropped == 14U);

	assert(linkr_debugger_serial_rx_read(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_WS0, output,
		       sizeof(output), &length) == 0);
	assert(length == sizeof(second));
	assert(memcmp(output, second, sizeof(second)) == 0);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_WS0].dropped == 0U);
}

static void test_rx_rearm_is_per_sink_fresh_boundary(void)
{
	static const uint8_t old_data[] = {0x10U, 0x11U};
	static const uint8_t fresh_data[] = {0x20U};
	struct linkr_debugger_serial_model model;
	uint8_t output[4];
	uint64_t old_epoch;
	size_t length;

	init_model(&model);
	register_and_attach(&model, LINKR_DEBUGGER_SERIAL_SINK_WS0);
	register_and_attach(&model, LINKR_DEBUGGER_SERIAL_SINK_WS1);
	assert(linkr_debugger_serial_rx_write(
		       &model, old_data, sizeof(old_data)) == 0);
	old_epoch = model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_WS1].epoch;
	assert(linkr_debugger_serial_rx_sink_rearm(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_WS1) == 0);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_WS1].epoch ==
	       old_epoch + 1U);
	assert(linkr_debugger_serial_rx_write(
		       &model, fresh_data, sizeof(fresh_data)) == 0);
	assert(linkr_debugger_serial_rx_read(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_WS1, output,
		       sizeof(output), &length) == 0);
	assert(length == sizeof(fresh_data) && output[0] == fresh_data[0]);
	assert(linkr_debugger_serial_rx_read(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_WS0, output,
		       sizeof(output), &length) == 0);
	assert(length == sizeof(old_data) + sizeof(fresh_data));
	assert(memcmp(output, old_data, sizeof(old_data)) == 0);
}

static void test_rx_dtr_detach_epoch_and_no_replay(void)
{
	static const uint8_t before_attach[] = { 0x10U, 0x11U };
	static const uint8_t attached[] = { 0x00U, 0xffU };
	static const uint8_t detached[] = { 0x22U, 0x23U };
	static const uint8_t after_reattach[] = { 0x33U };
	struct linkr_debugger_serial_model model;
	uint8_t output[4];
	size_t length;

	init_model(&model);
	assert(linkr_debugger_serial_rx_sink_register(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_CDC) == 0);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_CDC].epoch == 1U);
	assert(linkr_debugger_serial_rx_write(
		       &model, before_attach, sizeof(before_attach)) == 0);

	assert(linkr_debugger_serial_set_dtr(&model, true) == 0);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_CDC].attached);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_CDC].epoch == 2U);
	assert(linkr_debugger_serial_rx_read(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_CDC, output,
		       sizeof(output), &length) == 0);
	assert(length == 0U);

	assert(linkr_debugger_serial_rx_write(&model, attached, sizeof(attached)) == 0);
	assert(linkr_debugger_serial_rx_read(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_CDC, output,
		       sizeof(output), &length) == 0);
	assert(length == sizeof(attached));
	assert(memcmp(output, attached, sizeof(attached)) == 0);

	assert(linkr_debugger_serial_set_dtr(&model, false) == 0);
	assert(!model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_CDC].attached);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_CDC].epoch == 3U);
	assert(linkr_debugger_serial_rx_write(
		       &model, detached, sizeof(detached)) == 0);
	assert(linkr_debugger_serial_rx_read(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_CDC, output,
		       sizeof(output), &length) == -ENOTCONN);

	assert(linkr_debugger_serial_set_dtr(&model, true) == 0);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_CDC].epoch == 4U);
	assert(linkr_debugger_serial_rx_write(
		       &model, after_reattach, sizeof(after_reattach)) == 0);
	assert(linkr_debugger_serial_rx_read(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_CDC, output,
		       sizeof(output), &length) == 0);
	assert(length == sizeof(after_reattach));
	assert(memcmp(output, after_reattach, sizeof(after_reattach)) == 0);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_CDC].dropped == 0U);
	assert(model.config_counters.configure_count == 0U);
	assert(model.config_counters.control_line_changes == 3U);
}

static void test_rx_pause_boundary_and_paused_drop(void)
{
	static const uint8_t before_pause[] = { 0x10U, 0x11U, 0x12U };
	static const uint8_t while_paused[] = { 0x20U, 0x21U };
	static const uint8_t after_resume[] = { 0xffU };
	struct linkr_debugger_serial_tx_cancel_batch canceled;
	struct linkr_debugger_serial_model model;
	uint8_t output[4];
	uint64_t event_id;
	size_t length;

	init_model(&model);
	register_and_attach(&model, LINKR_DEBUGGER_SERIAL_SINK_WS0);
	assert(linkr_debugger_serial_rx_write(
		       &model, before_pause, sizeof(before_pause)) == 0);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_WS0].cursor == 0U);
	assert(linkr_debugger_serial_pause(&model, &canceled) == 0);
	assert(canceled.count == 0U);
	assert(model.rx.head == sizeof(before_pause));
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_WS0].cursor ==
	       model.rx.head);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_WS0].dropped ==
	       sizeof(before_pause));
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_WS0].epoch == 3U);

	assert(linkr_debugger_serial_rx_write(
		       &model, while_paused, sizeof(while_paused)) == 0);
	assert(model.rx.head == sizeof(before_pause));
	assert(model.rx.paused_dropped == sizeof(while_paused));
	assert(linkr_debugger_serial_rx_read(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_WS0, output,
		       sizeof(output), &length) == -EAGAIN);
	assert(length == 0U);

	assert(linkr_debugger_serial_resume(&model) == 0);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_WS0].epoch == 4U);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_WS0].cursor ==
	       model.rx.head);
	assert(linkr_debugger_serial_rx_write(
		       &model, after_resume, sizeof(after_resume)) == 0);
	assert(linkr_debugger_serial_rx_read(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_WS0, output,
		       sizeof(output), &length) == 0);
	assert(length == sizeof(after_resume));
	assert(memcmp(output, after_resume, sizeof(after_resume)) == 0);

	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_WS0, after_resume,
		       sizeof(after_resume), &event_id) == 0);
	model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_WS0].epoch = UINT64_MAX;
	assert(linkr_debugger_serial_pause(&model, &canceled) == -EOVERFLOW);
	assert(canceled.count == 0U);
	assert(model.state == LINKR_DEBUGGER_SERIAL_READY);
	assert(model.tx.used_slots == 1U);
	model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_WS0].epoch = UINT64_MAX - 1U;
	assert(linkr_debugger_serial_pause(&model, &canceled) == 0);
	assert(canceled.count == 1U && canceled.events[0].event_id == event_id);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_WS0].epoch == UINT64_MAX);
	assert(linkr_debugger_serial_resume(&model) == -EOVERFLOW);
	assert(model.state == LINKR_DEBUGGER_SERIAL_PAUSED);
}

static void test_rx_overflow_is_recorded_before_detach(void)
{
	struct linkr_debugger_serial_model model;
	uint8_t input[LINKR_DEBUGGER_SERIAL_RX_CAPACITY + 10U];
	size_t i;

	for (i = 0U; i < sizeof(input); ++i) {
		input[i] = (uint8_t)i;
	}
	init_model(&model);
	register_and_attach(&model, LINKR_DEBUGGER_SERIAL_SINK_TCP0);
	assert(linkr_debugger_serial_rx_write(&model, input, sizeof(input)) == 0);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_TCP0].dropped == 10U);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_TCP0].cursor == 10U);
	assert(linkr_debugger_serial_rx_sink_detach(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_TCP0) == 0);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_TCP0].dropped == 10U);
	assert(model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_TCP0].cursor ==
	       model.rx.head);
}

static void test_tx_fifo_partial_and_binary(void)
{
	static const uint8_t first[] = { 0x00U, 0xffU, 0x41U };
	static const uint8_t second[] = { 0x31U, 0x32U, 0x33U, 0x34U };
	struct linkr_debugger_serial_model model;
	enum linkr_debugger_serial_tx_source source;
	const uint8_t *data;
	uint64_t first_id;
	uint64_t second_id;
	uint64_t peeked_id;
	size_t length;

	init_model(&model);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_WS0, first,
		       sizeof(first), &first_id) == 0);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_TCP0, second,
		       sizeof(second), &second_id) == 0);
	assert(first_id == 1U && second_id == 2U);

	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &peeked_id) == 0);
	assert(length == sizeof(first) && source == LINKR_DEBUGGER_SERIAL_TX_WS0);
	assert(peeked_id == first_id && memcmp(data, first, sizeof(first)) == 0);
	assert(linkr_debugger_serial_tx_commit(&model, first_id, 4U) == -EINVAL);
	assert(linkr_debugger_serial_tx_commit(&model, first_id, 2U) == 0);
	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &peeked_id) == 0);
	assert(length == 1U && data[0] == 0x41U && peeked_id == first_id);
	assert(linkr_debugger_serial_tx_commit(&model, first_id, 1U) == 0);
	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &peeked_id) == -EAGAIN);
	assert(data == NULL && length == 0U);
	assert(linkr_debugger_serial_tx_finish_active(
		       &model, second_id, true) == -ESTALE);
	assert(linkr_debugger_serial_tx_finish_active(
		       &model, first_id, false) == -EBUSY);
	assert(linkr_debugger_serial_tx_finish_active(
		       &model, first_id, true) == 0);

	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &peeked_id) == 0);
	assert(length == sizeof(second));
	assert(source == LINKR_DEBUGGER_SERIAL_TX_TCP0 && peeked_id == second_id);
	assert(memcmp(data, second, sizeof(second)) == 0);
	assert(linkr_debugger_serial_tx_commit(
		       &model, second_id, sizeof(second)) == 0);
	assert(linkr_debugger_serial_tx_finish_active(
		       &model, second_id, true) == 0);
	assert(model.tx.completed == 2U);
	assert(model.tx.last_completed_event_id == second_id);
	assert(model.tx.bytes_committed == sizeof(first) + sizeof(second));
}

static void test_tx_quota_full_and_cancel(void)
{
	static const uint8_t message[] = { 0xa5U, 0x5aU };
	static const enum linkr_debugger_serial_tx_source single_quota_sources[] = {
		LINKR_DEBUGGER_SERIAL_TX_WS0,
		LINKR_DEBUGGER_SERIAL_TX_WS1,
		LINKR_DEBUGGER_SERIAL_TX_WS2,
		LINKR_DEBUGGER_SERIAL_TX_WS3,
		LINKR_DEBUGGER_SERIAL_TX_TCP0,
		LINKR_DEBUGGER_SERIAL_TX_TCP1,
	};
	struct linkr_debugger_serial_model model;
	enum linkr_debugger_serial_tx_source source;
	const uint8_t *data;
	uint64_t active_id;
	uint64_t event_id;
	uint64_t rejected_id = 0xfeedU;
	size_t length;
	size_t i;

	init_model(&model);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_CDC, message,
		       sizeof(message), &active_id) == 0);
	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &event_id) == 0);
	assert(event_id == active_id && length == sizeof(message));
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_CDC, message,
		       sizeof(message), &event_id) == 0);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_CDC, message,
		       sizeof(message), &rejected_id) == -EAGAIN);
	assert(rejected_id == 0xfeedU);

	for (i = 0U; i < sizeof(single_quota_sources) /
				       sizeof(single_quota_sources[0]); ++i) {
		assert(linkr_debugger_serial_tx_enqueue(
			       &model, single_quota_sources[i], message,
			       sizeof(message), &event_id) == 0);
	}
	assert(model.tx.used_slots == LINKR_DEBUGGER_SERIAL_TX_SLOT_COUNT);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_WS0, message,
		       sizeof(message), &rejected_id) == -EAGAIN);
	assert(model.tx.accepted == LINKR_DEBUGGER_SERIAL_TX_SLOT_COUNT);
	assert(model.tx.rejected == 2U);
	assert(model.tx.sources[LINKR_DEBUGGER_SERIAL_TX_CDC].rejected == 1U);

	assert(linkr_debugger_serial_tx_cancel_active(
		       &model, active_id, false) == -EBUSY);
	assert(linkr_debugger_serial_tx_cancel_active(
		       &model, active_id, true) == 0);
	assert(model.tx.canceled == 1U);
	assert(model.tx.last_canceled_event_id == active_id);
	assert(model.tx.sources[LINKR_DEBUGGER_SERIAL_TX_CDC].canceled == 1U);

	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_CDC, message,
		       LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX + 1U,
		       &rejected_id) == -EMSGSIZE);
}

static void test_tx_non_cdc_quota_includes_active(void)
{
	static const uint8_t message[] = { 0x11U, 0x22U };
	struct linkr_debugger_serial_model model;
	enum linkr_debugger_serial_tx_source source;
	const uint8_t *data;
	uint64_t event_id;
	uint64_t rejected_id = 99U;
	size_t length;

	init_model(&model);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_WS2, message,
		       sizeof(message), &event_id) == 0);
	assert(model.tx.used_slots == 1U);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_WS2, message,
		       sizeof(message), &rejected_id) == -EAGAIN);
	assert(model.tx.used_slots == 1U);
	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &rejected_id) == 0);
	assert(rejected_id == event_id && length == sizeof(message));
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_WS2, message,
		       sizeof(message), &rejected_id) == -EAGAIN);
	assert(model.tx.used_slots == 1U);
	assert(linkr_debugger_serial_tx_cancel_active(
		       &model, event_id, true) == 0);
}

static void test_tx_uint64_max_event_id(void)
{
	static const uint8_t message = 0x7eU;
	struct linkr_debugger_serial_model model;
	enum linkr_debugger_serial_tx_source source;
	const uint8_t *data;
	uint64_t event_id;
	uint64_t rejected_id = 123U;
	size_t length;

	init_model(&model);
	model.tx.last_event_id = UINT64_MAX - 1U;
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_WS0, &message, 1U,
		       &event_id) == 0);
	assert(event_id == UINT64_MAX);
	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &event_id) == 0);
	assert(event_id == UINT64_MAX && length == 1U && data[0] == message);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_TCP0, &message, 1U,
		       &rejected_id) == -EOVERFLOW);
	assert(rejected_id == 123U);
	assert(model.tx.used_slots == 1U);
	assert(model.tx.accepted == 1U && model.tx.rejected == 0U);
	assert(linkr_debugger_serial_tx_cancel_active(
		       &model, UINT64_MAX, true) == 0);
}

static void test_tx_pause_preserves_only_active(void)
{
	static const uint8_t active_message[] = { 1U, 2U, 3U };
	static const uint8_t queued_message[] = { 4U, 5U };
	struct linkr_debugger_serial_tx_cancel_batch canceled;
	struct linkr_debugger_serial_model model;
	enum linkr_debugger_serial_tx_source source;
	const uint8_t *data;
	uint64_t active_id;
	uint64_t ws_id;
	uint64_t tcp_id;
	uint64_t event_id;
	size_t length;

	init_model(&model);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_CDC, active_message,
		       sizeof(active_message), &active_id) == 0);
	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &event_id) == 0);
	assert(event_id == active_id && length == sizeof(active_message));
	assert(linkr_debugger_serial_tx_commit(&model, active_id, 1U) == 0);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_WS0, queued_message,
		       sizeof(queued_message), &ws_id) == 0);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_TCP0, queued_message,
		       sizeof(queued_message), &tcp_id) == 0);

	assert(linkr_debugger_serial_pause(&model, &canceled) == 0);
	assert(model.state == LINKR_DEBUGGER_SERIAL_PAUSED);
	assert(canceled.count == 2U);
	assert(canceled.events[0].event_id == ws_id);
	assert(canceled.events[1].event_id == tcp_id);
	assert(model.tx.used_slots == 1U);
	assert(model.tx.canceled == 2U);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_WS0, queued_message,
		       sizeof(queued_message), &event_id) == -EAGAIN);

	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &event_id) == -EAGAIN);
	assert(linkr_debugger_serial_tx_commit(&model, active_id, 1U) == -EAGAIN);
	assert(linkr_debugger_serial_tx_cancel_active(
		       &model, active_id, false) == -EBUSY);
	assert(linkr_debugger_serial_tx_cancel_all(
		       &model, false, &canceled) == -EBUSY);
	assert(canceled.count == 0U && model.tx.used_slots == 1U);
	assert(linkr_debugger_serial_tx_cancel_all(
		       &model, true, &canceled) == 0);
	assert(canceled.count == 1U && canceled.events[0].event_id == active_id);
	assert(linkr_debugger_serial_pause(&model, &canceled) == 0);
	assert(canceled.count == 0U);
	assert(linkr_debugger_serial_resume(&model) == 0);
	assert(model.state == LINKR_DEBUGGER_SERIAL_READY);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_WS0, queued_message,
		       sizeof(queued_message), &event_id) == 0);
	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &event_id) == 0);
	assert(linkr_debugger_serial_tx_commit(&model, event_id, length) == 0);
	assert(linkr_debugger_serial_pause(&model, &canceled) == 0);
	assert(canceled.count == 0U);
	assert(linkr_debugger_serial_tx_finish_active(
		       &model, event_id, true) == 0);
	assert(linkr_debugger_serial_resume(&model) == 0);
}

static void test_tx_fault_requires_driver_stop(void)
{
	static const uint8_t first[] = { 0x61U, 0x62U };
	static const uint8_t second[] = { 0x71U };
	struct linkr_debugger_serial_tx_cancel_batch canceled;
	struct linkr_debugger_serial_model model;
	enum linkr_debugger_serial_tx_source source;
	const uint8_t *data;
	uint64_t first_id;
	uint64_t second_id;
	uint64_t event_id;
	size_t length;

	init_model(&model);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_WS0, first,
		       sizeof(first), &first_id) == 0);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_TCP0, second,
		       sizeof(second), &second_id) == 0);
	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &event_id) == 0);
	assert(event_id == first_id);
	assert(linkr_debugger_serial_mark_faulted(&model) == 0);
	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &event_id) == -EIO);
	assert(linkr_debugger_serial_tx_commit(&model, first_id, 1U) == -EIO);
	assert(linkr_debugger_serial_tx_finish_active(
		       &model, first_id, true) == -EIO);
	assert(linkr_debugger_serial_tx_cancel_all(
		       &model, false, &canceled) == -EBUSY);
	assert(canceled.count == 0U && model.tx.used_slots == 2U);
	assert(linkr_debugger_serial_tx_cancel_all(
		       &model, true, &canceled) == 0);
	assert(canceled.count == 2U);
	assert(canceled.events[0].event_id == first_id);
	assert(canceled.events[1].event_id == second_id);
	assert(model.tx.canceled == 2U && model.tx.completed == 0U);
}

static void test_config_baud_range_and_presets(void)
{
	static const uint32_t accepted[] = {
		LINKR_DEBUGGER_SERIAL_BAUD_MIN,
		5200U,
		LINKR_DEBUGGER_SERIAL_BAUD_MAX,
	};
	static const uint32_t rejected[] = {
		0U,
		LINKR_DEBUGGER_SERIAL_BAUD_MIN - 1U,
		LINKR_DEBUGGER_SERIAL_BAUD_MAX + 1U,
		UINT32_MAX,
	};
	struct linkr_debugger_serial_config config = config_115200_8n1;
	struct linkr_debugger_serial_model model;
	enum linkr_debugger_serial_config_decision decision;
	size_t i;

	_Static_assert(sizeof(linkr_debugger_serial_baud_presets) /
			       sizeof(linkr_debugger_serial_baud_presets[0]) ==
		       LINKR_DEBUGGER_SERIAL_BAUD_PRESET_COUNT,
		       "serial preset count changed");
	for (i = 0U; i < LINKR_DEBUGGER_SERIAL_BAUD_PRESET_COUNT; ++i) {
		config.baud = linkr_debugger_serial_baud_presets[i];
		assert(linkr_debugger_serial_config_valid(&config));
	}
	for (i = 0U; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
		config.baud = accepted[i];
		assert(linkr_debugger_serial_config_valid(&config));
	}
	for (i = 0U; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
		config.baud = rejected[i];
		assert(!linkr_debugger_serial_config_valid(&config));
	}
	config = config_115200_8n1;
	config.data_bits = 4U;
	assert(!linkr_debugger_serial_config_valid(&config));
	config.data_bits = 5U;
	config.parity = LINKR_DEBUGGER_SERIAL_PARITY_ODD;
	config.stop_bits = 2U;
	assert(linkr_debugger_serial_config_valid(&config));
	config.parity = (enum linkr_debugger_serial_parity)3;
	assert(!linkr_debugger_serial_config_valid(&config));
	config = config_115200_8n1;
	config.stop_bits = 0U;
	assert(!linkr_debugger_serial_config_valid(&config));
	config = config_115200_8n1;
	config.flow_control = LINKR_DEBUGGER_SERIAL_FLOW_RTS_CTS;
	assert(!linkr_debugger_serial_config_valid(&config));
	init_model(&model);
	assert(linkr_debugger_serial_config_request_cdc(
		       &model, &config, &decision) == -EINVAL);
	assert(model.state == LINKR_DEBUGGER_SERIAL_READY);
	assert(model.generation == 1U);
	assert(model.config_counters.invalid == 1U);
	assert(model.config_counters.configure_count == 0U);
}

static void test_config_priority_cas_and_rollback(void)
{
	struct linkr_debugger_serial_config cdc_config = config_115200_8n1;
	struct linkr_debugger_serial_config web_config = config_115200_8n1;
	struct linkr_debugger_serial_model model;
	enum linkr_debugger_serial_config_decision decision;

	cdc_config.baud = 57600U;
	web_config.baud = 38400U;
	init_model(&model);
	assert(linkr_debugger_serial_rx_sink_register(
		       &model, LINKR_DEBUGGER_SERIAL_SINK_CDC) == 0);
	assert(linkr_debugger_serial_set_dtr(&model, true) == 0);
	assert(linkr_debugger_serial_config_request_web(
		       &model, &config_115200_8n1, 0U, &decision) == 0);
	assert(decision == LINKR_DEBUGGER_SERIAL_CONFIG_NO_CHANGE);
	assert(linkr_debugger_serial_config_request_web(
		       &model, &web_config, model.generation, &decision) == -EBUSY);
	assert(model.state == LINKR_DEBUGGER_SERIAL_READY);
	assert(model.config_counters.configure_count == 0U);

	assert(linkr_debugger_serial_config_request_cdc(
		       &model, &cdc_config, &decision) == 0);
	assert(decision == LINKR_DEBUGGER_SERIAL_CONFIG_APPLY);
	assert(model.state == LINKR_DEBUGGER_SERIAL_RECONFIG);
	assert(model.pending_origin == LINKR_DEBUGGER_SERIAL_CONFIG_ORIGIN_CDC);
	assert(model.config_counters.configure_count == 1U);
	assert(linkr_debugger_serial_config_apply_result(&model, true) == 0);
	assert(model.state == LINKR_DEBUGGER_SERIAL_READY);
	assert(model.generation == 2U);
	assert(linkr_debugger_serial_config_equal(&model.current, &cdc_config));
	assert(linkr_debugger_serial_config_equal(&model.last_good, &cdc_config));

	assert(linkr_debugger_serial_config_request_cdc(
		       &model, &cdc_config, &decision) == 0);
	assert(decision == LINKR_DEBUGGER_SERIAL_CONFIG_NO_CHANGE);
	assert(model.config_counters.configure_count == 1U);
	assert(linkr_debugger_serial_set_dtr(&model, false) == 0);
	assert(model.config_counters.configure_count == 1U);

	assert(linkr_debugger_serial_config_request_web(
		       &model, &web_config, 1U, &decision) == -ESTALE);
	assert(linkr_debugger_serial_config_request_web(
		       &model, &web_config, 2U, &decision) == 0);
	assert(decision == LINKR_DEBUGGER_SERIAL_CONFIG_APPLY);
	assert(model.pending_origin == LINKR_DEBUGGER_SERIAL_CONFIG_ORIGIN_WEB);
	assert(linkr_debugger_serial_config_apply_result(&model, false) == 0);
	assert(model.state == LINKR_DEBUGGER_SERIAL_RECONFIG);
	assert(model.rollback_pending);
	assert(linkr_debugger_serial_config_rollback_result(&model, true) == 0);
	assert(model.state == LINKR_DEBUGGER_SERIAL_READY);
	assert(model.generation == 2U);
	assert(linkr_debugger_serial_config_equal(&model.current, &cdc_config));
	assert(model.config_counters.apply_failed == 1U);
	assert(model.config_counters.rollback_succeeded == 1U);
}

static void test_config_waits_for_physical_tx_completion(void)
{
	static const uint8_t first[] = { 0x31U, 0x32U };
	static const uint8_t second[] = { 0x41U };
	struct linkr_debugger_serial_config next = config_115200_8n1;
	struct linkr_debugger_serial_model model;
	enum linkr_debugger_serial_config_decision decision;
	enum linkr_debugger_serial_tx_source source;
	const uint8_t *data;
	uint64_t first_id;
	uint64_t second_id;
	uint64_t event_id;
	size_t length;

	next.baud = 57600U;
	init_model(&model);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_WS0, first,
		       sizeof(first), &first_id) == 0);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_TCP0, second,
		       sizeof(second), &second_id) == 0);
	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &event_id) == 0);
	assert(event_id == first_id && length == sizeof(first));

	assert(linkr_debugger_serial_config_request_cdc(
		       &model, &next, &decision) == -EBUSY);
	assert(model.state == LINKR_DEBUGGER_SERIAL_READY);
	assert(linkr_debugger_serial_tx_commit(
		       &model, first_id, sizeof(first)) == 0);
	assert(linkr_debugger_serial_config_request_cdc(
		       &model, &next, &decision) == -EBUSY);
	assert(linkr_debugger_serial_tx_finish_active(
		       &model, first_id, false) == -EBUSY);
	assert(linkr_debugger_serial_tx_finish_active(
		       &model, first_id, true) == 0);

	assert(linkr_debugger_serial_config_request_cdc(
		       &model, &next, &decision) == 0);
	assert(decision == LINKR_DEBUGGER_SERIAL_CONFIG_APPLY);
	assert(model.state == LINKR_DEBUGGER_SERIAL_RECONFIG);
	assert(model.tx.active_slot < 0 && model.tx.used_slots == 1U);
	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &event_id) == -EBUSY);
	assert(data == NULL && length == 0U && model.tx.active_slot < 0);
	assert(linkr_debugger_serial_config_apply_result(&model, true) == 0);
	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &event_id) == 0);
	assert(event_id == second_id && length == sizeof(second));
	assert(linkr_debugger_serial_tx_commit(
		       &model, second_id, sizeof(second)) == 0);
	assert(linkr_debugger_serial_tx_finish_active(
		       &model, second_id, true) == 0);
}

static void test_config_fault_and_control_line_noop(void)
{
	struct linkr_debugger_serial_config next = config_115200_8n1;
	struct linkr_debugger_serial_model model;
	struct linkr_debugger_serial_tx_cancel_batch canceled;
	enum linkr_debugger_serial_config_decision decision;
	enum linkr_debugger_serial_tx_source source;
	const uint8_t *data;
	uint64_t first_id;
	uint64_t second_id;
	uint64_t rejected_id;
	size_t length;
	static const uint8_t byte = 0x5aU;

	next.baud = 57600U;
	init_model(&model);
	assert(linkr_debugger_serial_set_dtr(&model, false) == 0);
	assert(model.config_counters.control_line_changes == 0U);
	assert(model.config_counters.configure_count == 0U);
	assert(linkr_debugger_serial_pause(&model, &canceled) == 0);
	assert(linkr_debugger_serial_config_request_cdc(
		       &model, &next, &decision) == -EAGAIN);
	assert(linkr_debugger_serial_resume(&model) == 0);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_WS0, &byte, 1U,
		       &first_id) == 0);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_TCP0, &byte, 1U,
		       &second_id) == 0);

	assert(linkr_debugger_serial_config_request_web(
		       &model, &next, model.generation, &decision) == 0);
	assert(linkr_debugger_serial_config_apply_result(&model, false) == 0);
	assert(linkr_debugger_serial_config_rollback_result(&model, false) == 0);
	assert(model.state == LINKR_DEBUGGER_SERIAL_FAULTED);
	assert(model.config_counters.rollback_failed == 1U);
	assert(linkr_debugger_serial_config_equal(
		       &model.last_good, &config_115200_8n1));
	assert(linkr_debugger_serial_config_request_cdc(
		       &model, &next, &decision) == -EIO);
	assert(linkr_debugger_serial_tx_peek(
		       &model, &data, &length, &source, &rejected_id) == -EIO);
	assert(data == NULL && length == 0U);
	assert(linkr_debugger_serial_tx_commit(&model, first_id, 1U) == -EIO);
	assert(model.tx.used_slots == 2U && model.tx.active_slot < 0);
	assert(linkr_debugger_serial_tx_cancel_all(
		       &model, false, &canceled) == 0);
	assert(canceled.count == 2U);
	assert(canceled.events[0].event_id == first_id);
	assert(canceled.events[1].event_id == second_id);
	assert(model.tx.used_slots == 0U && model.tx.canceled == 2U);
	assert(model.tx.completed == 0U);
	assert(linkr_debugger_serial_tx_enqueue(
		       &model, LINKR_DEBUGGER_SERIAL_TX_CDC, &byte, 1U,
		       &rejected_id) == -EAGAIN);
}

int main(void)
{
	test_init_accepts_aliased_config();
	test_rx_binary_fanout();
	test_rx_slow_sink_drops_independently();
	test_rx_rearm_is_per_sink_fresh_boundary();
	test_rx_dtr_detach_epoch_and_no_replay();
	test_rx_pause_boundary_and_paused_drop();
	test_rx_overflow_is_recorded_before_detach();
	test_tx_fifo_partial_and_binary();
	test_tx_quota_full_and_cancel();
	test_tx_non_cdc_quota_includes_active();
	test_tx_uint64_max_event_id();
	test_tx_pause_preserves_only_active();
	test_tx_fault_requires_driver_stop();
	test_config_baud_range_and_presets();
	test_config_priority_cas_and_rollback();
	test_config_waits_for_physical_tx_completion();
	test_config_fault_and_control_line_noop();
	return 0;
}
