/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_sigrok_linkr.h"
#include "linkr_debugger_logic_analyzer.h"
#include "linkr_debugger_capture_arbiter.h"

#include <errno.h>
#include <string.h>

#ifdef LINKR_DEBUGGER_SIGROK_LINKR_HOST_TEST
static int send_all(int fd, const uint8_t *data, size_t len)
{
	(void)fd;
	(void)data;
	(void)len;
	return -ENOTSUP;
}
#endif

#ifndef LINKR_DEBUGGER_SIGROK_LINKR_HOST_TEST
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/poll.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(linkr_debugger_sigrok_linkr, CONFIG_LINKR_DEBUGGER_LOG_LEVEL);

static int send_all(int fd, const uint8_t *data, size_t len);

#define LINKR_DEBUGGER_SIGROK_LINKR_BIND_ADDR "172.29.203.1"
#define LINKR_DEBUGGER_SIGROK_LINKR_RECV_TIMEOUT_MS 2000U
#define LINKR_DEBUGGER_SIGROK_LINKR_STREAM_RECV_SLICE_MS 1U
#define LINKR_DEBUGGER_SIGROK_LINKR_SEND_TIMEOUT_MS 2000U
#define LINKR_DEBUGGER_SIGROK_LINKR_PRIORITY K_PRIO_PREEMPT(8)

BUILD_ASSERT(LINKR_DEBUGGER_SIGROK_LINKR_STREAM_QDEPTH_LIMIT == 32U,
	"Sigrok TCP stream queue depth must stay aligned with HIL coverage");
BUILD_ASSERT(LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_REQUEST_BYTES ==
	LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES,
	"Sigrok control request buffer only needs CONFIG_REQ payload capacity");
BUILD_ASSERT(LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_RESPONSE_BYTES ==
	(1U + ((size_t)LINKR_DEBUGGER_SIGROK_LINKR_MODE_CAPS_BYTES *
	LINKR_DEBUGGER_SIGROK_LINKR_CAPS_MODE_COUNT)),
	"Sigrok control response buffer must fit CAPS_RESP");
BUILD_ASSERT(LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_RESPONSE_BYTES >=
	LINKR_DEBUGGER_SIGROK_LINKR_HELLO_BYTES,
	"Sigrok control response buffer must fit HELLO_RESP");
BUILD_ASSERT(LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_RESPONSE_BYTES >=
	LINKR_DEBUGGER_SIGROK_LINKR_ACK_BYTES,
	"Sigrok control response buffer must fit ACK responses");
BUILD_ASSERT(LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_RESPONSE_BYTES >=
	LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BYTES,
	"Sigrok control response buffer must fit ERROR responses");

struct sigrok_linkr_stream_queue_item {
	void *fifo_reserved;
	size_t len;
	uint8_t data[];
};

struct linkr_debugger_sigrok_linkr_runtime {
	int listen_fd;
	int client_fd;
	struct linkr_debugger_sigrok_linkr_session session;
	struct k_fifo stream_fifo;
	atomic_t stream_qdepth;
	atomic_t stream_dropped;
	atomic_t stream_stop_pending;
	uint32_t next_sequence_id;
	uint8_t rx_control_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_REQUEST_BYTES];
	uint8_t tx_control_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_RESPONSE_BYTES];
};

static struct linkr_debugger_sigrok_linkr_runtime linkr_debugger_sigrok_linkr_runtime = {
	.listen_fd = -1,
	.client_fd = -1,
	.next_sequence_id = 1U,
};

static void sigrok_linkr_stream_reset_queue(
	struct linkr_debugger_sigrok_linkr_runtime *runtime)
{
	struct sigrok_linkr_stream_queue_item *item;

	while ((item = k_fifo_get(&runtime->stream_fifo, K_NO_WAIT)) != NULL) {
		k_free(item);
	}
	atomic_set(&runtime->stream_qdepth, 0);
	atomic_set(&runtime->stream_dropped, 0);
	atomic_set(&runtime->stream_stop_pending, 0);
}

static void sigrok_linkr_stream_mark_stop(
	struct linkr_debugger_sigrok_linkr_runtime *runtime)
{
	atomic_set(&runtime->stream_stop_pending, 1);
}

static bool sigrok_linkr_stream_busy(
	struct linkr_debugger_sigrok_linkr_runtime *runtime)
{
	return runtime->session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED ||
		runtime->session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING ||
		!k_fifo_is_empty(&runtime->stream_fifo) ||
		atomic_get(&runtime->stream_stop_pending) != 0;
}

static int sigrok_linkr_stream_enqueue_event(
	struct linkr_debugger_sigrok_linkr_runtime *runtime,
	enum linkr_debugger_sigrok_linkr_event_type event_type,
	uint16_t session_id,
	uint32_t sample_index)
{
	struct sigrok_linkr_stream_queue_item *item;
	struct linkr_debugger_sigrok_linkr_header header;
	struct linkr_debugger_sigrok_linkr_event event = {
		.session_id = session_id,
		.type_detail = (uint8_t)event_type,
		.sample_index = sample_index,
	};
	size_t frame_len = LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES;
	size_t event_len;

	if (atomic_get(&runtime->stream_qdepth) >=
	    LINKR_DEBUGGER_SIGROK_LINKR_STREAM_QDEPTH_LIMIT) {
		struct sigrok_linkr_stream_queue_item *old = k_fifo_get(
			&runtime->stream_fifo, K_NO_WAIT);

		atomic_inc(&runtime->stream_dropped);
		if (old == NULL) {
			return -ENOSPC;
		}
		atomic_dec(&runtime->stream_qdepth);
		k_free(old);
	}

	item = k_malloc(sizeof(*item) + frame_len);
	if (item == NULL) {
		atomic_inc(&runtime->stream_dropped);
		return -ENOMEM;
	}

	event_len = linkr_debugger_sigrok_linkr_encode_event(&event,
		item->data + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES,
		LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES);
	linkr_debugger_sigrok_linkr_init_response_header(&header,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_EVENT, 0U, (uint16_t)event_len);
	(void)linkr_debugger_sigrok_linkr_encode_header(&header, item->data,
		LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);
	item->len = LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES + event_len;
	k_fifo_put(&runtime->stream_fifo, item);
	atomic_inc(&runtime->stream_qdepth);
	return 0;
}

static int sigrok_linkr_stream_enqueue_data(
	struct linkr_debugger_sigrok_linkr_runtime *runtime,
	const struct linkr_debugger_la_stream_chunk *chunk,
	uint16_t session_id,
	uint32_t sample_index,
	uint16_t sample_count,
	uint8_t compression,
	uint16_t channel_mask,
	bool needs_terminal_event)
{
	size_t data_len = linkr_debugger_sigrok_linkr_packed_data_len(channel_mask,
		sample_count);
	size_t frame_len = LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + data_len;
	struct sigrok_linkr_stream_queue_item *item;
	struct linkr_debugger_sigrok_linkr_header header;
	struct linkr_debugger_sigrok_linkr_data_meta meta = {
		.sample_index = sample_index,
		.sample_count = sample_count,
		.compression = compression,
		.channel_mask = channel_mask,
	};
	size_t encoded_len;

	if (data_len == 0U || data_len > LINKR_DEBUGGER_SIGROK_LINKR_MAX_DATA_BYTES) {
		return -EINVAL;
	}
	if (!linkr_debugger_sigrok_linkr_stream_queue_has_capacity(
	    (uint32_t)atomic_get(&runtime->stream_qdepth), needs_terminal_event)) {
		atomic_inc(&runtime->stream_dropped);
		return -ENOSPC;
	}

	item = k_malloc(sizeof(*item) + frame_len);
	if (item == NULL) {
		atomic_inc(&runtime->stream_dropped);
		return -ENOMEM;
	}

	(void)linkr_debugger_sigrok_linkr_encode_data_meta(&meta,
		item->data + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES,
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES);
	encoded_len = linkr_debugger_sigrok_linkr_compress_bit_pack(chunk->values,
		sample_count, channel_mask,
		item->data + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES,
		data_len);
	if (encoded_len != data_len) {
		k_free(item);
		return -EINVAL;
	}
	linkr_debugger_sigrok_linkr_init_response_header(&header,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_DATA, 0U,
		(uint16_t)(LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + data_len));
	(void)linkr_debugger_sigrok_linkr_encode_header(&header, item->data,
		LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);
	item->len = frame_len;
	k_fifo_put(&runtime->stream_fifo, item);
	atomic_inc(&runtime->stream_qdepth);
	return 0;
}

static void sigrok_linkr_stream_callback(
	const struct linkr_debugger_la_stream_chunk *chunk, void *user_data)
{
	struct linkr_debugger_sigrok_linkr_runtime *runtime =
		(struct linkr_debugger_sigrok_linkr_runtime *)user_data;
	struct linkr_debugger_sigrok_linkr_session *session;

	if (runtime == NULL || chunk == NULL || runtime->client_fd < 0) {
		return;
	}

	session = &runtime->session;
	if (chunk->status != LINKR_DEBUGGER_LA_RING_POLL_OK) {
		(void)sigrok_linkr_stream_enqueue_event(runtime,
			LINKR_DEBUGGER_SIGROK_LINKR_EVENT_OVERRUN,
			session->active_session_id, session->sample_index);
		session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
		sigrok_linkr_stream_mark_stop(runtime);
		return;
	}
	if (session->state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED) {
		if (linkr_debugger_logic_analyzer_is_stream_triggered()) {
			session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING;
			(void)sigrok_linkr_stream_enqueue_event(runtime,
				LINKR_DEBUGGER_SIGROK_LINKR_EVENT_TRIGGERED,
				session->active_session_id, session->sample_index);
		} else {
			session->sample_index = linkr_debugger_sigrok_linkr_advance_sample_index(
				session->sample_index, chunk->sample_count);
			return;
		}
	}
	if (session->state != LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING) {
		return;
	}

	uint16_t channel_mask = session->config.channel_mask;
	uint8_t compression = LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK;
	uint32_t sample_index = session->sample_index;
	uint16_t session_id = session->active_session_id;
	uint16_t send_count = linkr_debugger_sigrok_linkr_bounded_chunk_count(session,
		chunk->sample_count);
	bool final_chunk;

	if (send_count == 0U) {
		(void)sigrok_linkr_stream_enqueue_event(runtime,
			LINKR_DEBUGGER_SIGROK_LINKR_EVENT_STOPPED,
			session_id, sample_index);
		session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
		sigrok_linkr_stream_mark_stop(runtime);
		return;
	}

	session->emitted_samples += send_count;
	final_chunk = linkr_debugger_sigrok_linkr_bounded_capture_done(session);
	session->emitted_samples -= send_count;

	if (sigrok_linkr_stream_enqueue_data(runtime, chunk, session_id,
	    sample_index, send_count, compression, channel_mask, final_chunk) < 0) {
		(void)sigrok_linkr_stream_enqueue_event(runtime,
			LINKR_DEBUGGER_SIGROK_LINKR_EVENT_OVERRUN,
			session_id, sample_index);
		session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
		sigrok_linkr_stream_mark_stop(runtime);
		return;
	}

	session->emitted_samples += send_count;
	session->sample_index = linkr_debugger_sigrok_linkr_advance_sample_index(
		session->sample_index, send_count);
	if (final_chunk) {
		(void)sigrok_linkr_stream_enqueue_event(runtime,
			LINKR_DEBUGGER_SIGROK_LINKR_EVENT_STOPPED,
			session_id, session->sample_index);
		session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
		sigrok_linkr_stream_mark_stop(runtime);
	}
}

static void sigrok_linkr_cleanup_capture(
	struct linkr_debugger_sigrok_linkr_runtime *runtime)
{
	struct linkr_debugger_sigrok_linkr_session *session;

	if (runtime == NULL) {
		return;
	}

	session = &runtime->session;
	if (session->capture_owner_held) {
		if (linkr_debugger_logic_analyzer_is_ring_active()) {
			(void)linkr_debugger_logic_analyzer_stop_ring();
		} else {
			(void)linkr_debugger_logic_analyzer_stop_stream();
		}
		(void)linkr_debugger_capture_arbiter_release(
			LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR);
		session->capture_owner_held = false;
	}
	if (session->state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED ||
	    session->state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING) {
		session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
	}
	sigrok_linkr_stream_reset_queue(runtime);
}

static int sigrok_linkr_stream_drain(
	struct linkr_debugger_sigrok_linkr_runtime *runtime)
{
	struct sigrok_linkr_stream_queue_item *item;
	int ret = 0;

	while ((item = k_fifo_get(&runtime->stream_fifo, K_NO_WAIT)) != NULL) {
		atomic_dec(&runtime->stream_qdepth);
		ret = send_all(runtime->client_fd, item->data, item->len);
		k_free(item);
		if (ret < 0) {
			atomic_inc(&runtime->stream_dropped);
			break;
		}
	}

	return ret;
}

static void sigrok_linkr_stop_capture_if_pending(
	struct linkr_debugger_sigrok_linkr_runtime *runtime)
{
	struct linkr_debugger_sigrok_linkr_session *session = &runtime->session;

	if (!atomic_cas(&runtime->stream_stop_pending, 1, 0)) {
		return;
	}
	if (!session->capture_owner_held) {
		return;
	}

	(void)linkr_debugger_logic_analyzer_stop_stream();
	(void)linkr_debugger_capture_arbiter_release(
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR);
	session->capture_owner_held = false;
}
#endif

static uint16_t load_le16(const uint8_t *src)
{
	return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t load_le24(const uint8_t *src)
{
	return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16);
}

static uint32_t load_le32(const uint8_t *src)
{
	return (uint32_t)src[0] |
		((uint32_t)src[1] << 8) |
		((uint32_t)src[2] << 16) |
		((uint32_t)src[3] << 24);
}

static void store_le16(uint8_t *dst, uint16_t value)
{
	dst[0] = (uint8_t)(value & 0xffU);
	dst[1] = (uint8_t)((value >> 8) & 0xffU);
}

static void store_le24(uint8_t *dst, uint32_t value)
{
	dst[0] = (uint8_t)(value & 0xffU);
	dst[1] = (uint8_t)((value >> 8) & 0xffU);
	dst[2] = (uint8_t)((value >> 16) & 0xffU);
}

static void store_le32(uint8_t *dst, uint32_t value)
{
	dst[0] = (uint8_t)(value & 0xffU);
	dst[1] = (uint8_t)((value >> 8) & 0xffU);
	dst[2] = (uint8_t)((value >> 16) & 0xffU);
	dst[3] = (uint8_t)((value >> 24) & 0xffU);
}

static int send_all(int fd, const uint8_t *data, size_t len);

uint8_t linkr_debugger_sigrok_linkr_bytes_per_sample(uint16_t channel_mask)
{
	uint8_t count = (uint8_t)__builtin_popcount((unsigned)channel_mask);

	if (count == 0U) {
		return 0U;
	}
	return (count + 7U) / 8U;
}

size_t linkr_debugger_sigrok_linkr_packed_data_len(uint16_t channel_mask,
	uint16_t sample_count)
{
	return (size_t)linkr_debugger_sigrok_linkr_bytes_per_sample(channel_mask) * sample_count;
}

bool linkr_debugger_sigrok_linkr_stream_queue_has_capacity(uint32_t qdepth,
	bool needs_terminal_event)
{
	uint32_t needed = needs_terminal_event ? 2U : 1U;

	return qdepth <= LINKR_DEBUGGER_SIGROK_LINKR_STREAM_QDEPTH_LIMIT - needed;
}

bool linkr_debugger_sigrok_linkr_ws_pool_data_has_capacity(uint32_t data_slots_used,
	bool needs_terminal_event)
{
	uint32_t reserved = needs_terminal_event ? 1U : 0U;

	if (reserved >= LINKR_DEBUGGER_SIGROK_LINKR_WS_DATA_SLOT_COUNT) {
		return false;
	}

	return data_slots_used < LINKR_DEBUGGER_SIGROK_LINKR_WS_DATA_SLOT_COUNT - reserved;
}

bool linkr_debugger_sigrok_linkr_ws_pool_terminal_has_capacity(bool terminal_slot_used)
{
	return !terminal_slot_used;
}

bool linkr_debugger_sigrok_linkr_ws_slot_transition_valid(
	enum linkr_debugger_sigrok_linkr_ws_slot_state from,
	enum linkr_debugger_sigrok_linkr_ws_slot_state to)
{
	switch (from) {
	case LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE:
		return to == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED;
	case LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED:
		return to == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED ||
			to == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE;
	case LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED:
		return to == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED ||
			to == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE;
	default:
		return false;
	}
}

bool linkr_debugger_sigrok_linkr_ws_slot_commit_allowed(
	enum linkr_debugger_sigrok_linkr_ws_slot_state state,
	uint32_t slot_owner_session_id,
	uint32_t slot_owner_generation,
	uint32_t active_session_id,
	uint32_t active_generation)
{
	return state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED &&
		slot_owner_session_id != 0U && slot_owner_session_id == active_session_id &&
		slot_owner_generation == active_generation;
}

bool linkr_debugger_sigrok_linkr_stream_queue_bytes_has_capacity(size_t qbytes,
	size_t next_item_bytes, size_t byte_limit, bool needs_terminal_event,
	size_t terminal_item_bytes)
{
	if (next_item_bytes == 0U || qbytes > byte_limit || next_item_bytes > byte_limit - qbytes) {
		return false;
	}

	size_t used = qbytes + next_item_bytes;

	if (needs_terminal_event) {
		if (terminal_item_bytes == 0U || terminal_item_bytes > byte_limit - used) {
			return false;
		}
	}

	return true;
}

bool linkr_debugger_sigrok_linkr_coalesce_can_append(size_t current_len,
	size_t next_frame_len, size_t buffer_capacity, uint8_t current_count,
	uint8_t max_count)
{
	if (next_frame_len == 0U || current_count >= max_count) {
		return false;
	}
	if (current_len > buffer_capacity || next_frame_len > buffer_capacity - current_len) {
		return false;
	}

	return true;
}

bool linkr_debugger_sigrok_linkr_should_emit_local_terminal_event(bool connected,
	int send_error)
{
	return connected && send_error >= 0;
}

enum linkr_debugger_sigrok_linkr_stream_wake_action
linkr_debugger_sigrok_linkr_stream_wake_policy(uint32_t qdepth,
	bool urgent, bool delayed_wake_pending, uint32_t wake_qdepth)
{
	if (urgent || (wake_qdepth > 0U && qdepth >= wake_qdepth)) {
		return LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_NOW;
	}
	if (qdepth > 0U && !delayed_wake_pending) {
		return LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_DELAY;
	}

	return LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_DEFER;
}

bool linkr_debugger_sigrok_linkr_stream_sink_handoff_requested(uint32_t qdepth)
{
	return qdepth >= LINKR_DEBUGGER_SIGROK_LINKR_STREAM_HANDOFF_QDEPTH;
}

void linkr_debugger_sigrok_linkr_ws_transport_metrics_reset(
	struct linkr_debugger_sigrok_linkr_ws_transport_metrics *metrics)
{
	if (metrics != NULL) {
		memset(metrics, 0, sizeof(*metrics));
	}
}

void linkr_debugger_sigrok_linkr_ws_transport_metrics_update_enqueue(
	struct linkr_debugger_sigrok_linkr_ws_transport_metrics *metrics,
	uint32_t qdepth, size_t qbytes)
{
	if (metrics == NULL) {
		return;
	}
	if (qdepth > metrics->max_qdepth) {
		metrics->max_qdepth = qdepth;
	}
	if (qbytes > metrics->max_qbytes) {
		metrics->max_qbytes = qbytes;
	}
}

void linkr_debugger_sigrok_linkr_ws_transport_metrics_update_drain(
	struct linkr_debugger_sigrok_linkr_ws_transport_metrics *metrics,
	uint64_t duration_us, uint32_t items, size_t bytes)
{
	if (metrics == NULL || duration_us <= metrics->max_drain_us) {
		return;
	}
	metrics->max_drain_us = duration_us;
	metrics->max_drain_items = items;
	metrics->max_drain_bytes = bytes;
}

void linkr_debugger_sigrok_linkr_ws_transport_metrics_update_send(
	struct linkr_debugger_sigrok_linkr_ws_transport_metrics *metrics,
	uint64_t duration_us, uint8_t frames, size_t bytes)
{
	if (metrics == NULL || duration_us <= metrics->max_send_us) {
		return;
	}
	metrics->max_send_us = duration_us;
	metrics->max_send_frames = frames;
	metrics->max_send_bytes = bytes;
}

bool linkr_debugger_sigrok_linkr_sample_range_fits(uint32_t sample_index,
	uint32_t sample_count)
{
	(void)sample_count;

	return sample_index <= LINKR_DEBUGGER_SIGROK_LINKR_MAX_SAMPLE_INDEX;
}

uint32_t linkr_debugger_sigrok_linkr_advance_sample_index(uint32_t sample_index,
	uint32_t sample_count)
{
	return (sample_index + sample_count) & LINKR_DEBUGGER_SIGROK_LINKR_MAX_SAMPLE_INDEX;
}

uint16_t linkr_debugger_sigrok_linkr_bounded_chunk_count(
	const struct linkr_debugger_sigrok_linkr_session *session,
	uint32_t offered_count)
{
	uint32_t remaining;

	if (session == NULL || offered_count == 0U) {
		return 0U;
	}
	if (session->config.post_samples == 0U) {
		return (uint16_t)(offered_count > UINT16_MAX ? UINT16_MAX : offered_count);
	}
	if (session->emitted_samples >= session->config.post_samples) {
		return 0U;
	}

	remaining = session->config.post_samples - session->emitted_samples;
	return (uint16_t)(offered_count > remaining ? remaining : offered_count);
}

bool linkr_debugger_sigrok_linkr_bounded_capture_done(
	const struct linkr_debugger_sigrok_linkr_session *session)
{
	return session != NULL && session->config.post_samples > 0U &&
		session->emitted_samples >= session->config.post_samples;
}

size_t linkr_debugger_sigrok_linkr_compress_bit_pack(
	const uint16_t *samples,
	uint32_t count,
	uint16_t channel_mask,
	uint8_t *out,
	size_t out_len)
{
	if (samples == NULL || out == NULL || count == 0U || count > out_len) {
		return 0U;
	}

	uint8_t bytes_per_sample = linkr_debugger_sigrok_linkr_bytes_per_sample(channel_mask);
	size_t total_bytes = (size_t)count * bytes_per_sample;

	if (total_bytes > out_len) {
		return 0U;
	}

	uint8_t channel_map[16];
	uint8_t channel_count = 0U;

	for (uint8_t i = 0U; i < 16U; i++) {
		if ((channel_mask & (1U << i)) != 0U) {
			channel_map[channel_count++] = i;
		}
	}

	for (uint32_t i = 0U; i < count; i++) {
		uint16_t sample = samples[i];
		uint8_t packed = 0U;

		for (uint8_t ch = 0U; ch < channel_count && ch < 8U; ch++) {
			uint8_t in_bit = channel_map[ch];

			if ((sample & (1U << in_bit)) != 0U) {
				packed |= (1U << ch);
			}
		}

		out[i * bytes_per_sample] = packed;
		if (bytes_per_sample > 1U) {
			uint8_t packed2 = 0U;

			for (uint8_t ch = 8U; ch < channel_count; ch++) {
				uint8_t in_bit = channel_map[ch];

				if ((sample & (1U << in_bit)) != 0U) {
					packed2 |= (1U << (ch - 8U));
				}
			}
			out[i * bytes_per_sample + 1U] = packed2;
		}
	}

	return total_bytes;
}

size_t linkr_debugger_sigrok_linkr_compress_bit_pack_single(
	const uint16_t *samples,
	uint32_t count,
	uint8_t *out,
	size_t out_len)
{
	if (samples == NULL || out == NULL || count == 0U || count > out_len) {
		return 0U;
	}

	for (uint32_t i = 0U; i < count; i++) {
		out[i] = (samples[i] & 0x0001U) != 0U ? 1U : 0U;
	}

	return count;
}

size_t linkr_debugger_sigrok_linkr_compress_bit_pack_rle_single(
	const uint16_t *samples,
	uint32_t count,
	uint8_t *out,
	size_t out_len)
{
	size_t out_pos = 0U;
	uint32_t i = 0U;

	if (samples == NULL || out == NULL || count == 0U) {
		return 0U;
	}

	while (i < count) {
		uint8_t value = (samples[i] & 0x0001U) != 0U ? 1U : 0U;
		uint32_t run_count = 1U;

		while (i + run_count < count && run_count < UINT16_MAX &&
		       (((samples[i + run_count] & 0x0001U) != 0U ? 1U : 0U) == value)) {
			run_count++;
		}

		if (out_pos + 3U >= count || out_pos + 3U > out_len) {
			return 0U;
		}

		out[out_pos] = value;
		out[out_pos + 1U] = (uint8_t)(run_count & 0xffU);
		out[out_pos + 2U] = (uint8_t)((run_count >> 8) & 0xffU);
		out_pos += 3U;
		i += run_count;
	}

	return out_pos < count ? out_pos : 0U;
}

size_t linkr_debugger_sigrok_linkr_compress_bit_pack_rle(
	const uint16_t *samples,
	uint32_t count,
	uint16_t channel_mask,
	uint8_t *out,
	size_t out_len)
{
	if (samples == NULL || out == NULL || count == 0U) {
		return 0U;
	}

	uint8_t bytes_per_sample = linkr_debugger_sigrok_linkr_bytes_per_sample(channel_mask);
	size_t normal_len = (size_t)count * bytes_per_sample;

	if (bytes_per_sample == 0U || normal_len == 0U || normal_len > out_len) {
		return 0U;
	}

	uint8_t channel_map[16];
	uint8_t channel_count = 0U;

	for (uint8_t i = 0U; i < 16U; i++) {
		if ((channel_mask & (1U << i)) != 0U) {
			channel_map[channel_count++] = i;
		}
	}

	size_t out_pos = 0U;
	uint32_t i = 0U;

	while (i < count) {
		uint16_t packed_value = 0U;
		uint16_t run_count = 1U;

		for (uint8_t ch = 0U; ch < channel_count; ch++) {
			uint8_t in_bit = channel_map[ch];

			if ((samples[i] & (1U << in_bit)) != 0U) {
				packed_value |= (uint16_t)(1U << ch);
			}
		}

		while (i + run_count < count && run_count < UINT16_MAX) {
			uint16_t next_packed = 0U;

			for (uint8_t ch = 0U; ch < channel_count; ch++) {
				uint8_t in_bit = channel_map[ch];

				if ((samples[i + run_count] & (1U << in_bit)) != 0U) {
					next_packed |= (uint16_t)(1U << ch);
				}
			}
			if (next_packed != packed_value) {
				break;
			}
			run_count++;
		}

		size_t needed = (size_t)bytes_per_sample + 2U;

		if (out_pos + needed >= normal_len || out_pos + needed > out_len) {
			return 0U;
		}

		out[out_pos] = (uint8_t)(packed_value & 0xffU);
		if (bytes_per_sample > 1U) {
			out[out_pos + 1U] = (uint8_t)((packed_value >> 8) & 0xffU);
		}
		out_pos += bytes_per_sample;
		out[out_pos] = (uint8_t)(run_count & 0xffU);
		out[out_pos + 1U] = (uint8_t)((run_count >> 8) & 0xffU);
		out_pos += 2U;

		i += run_count;
	}

	return out_pos < normal_len ? out_pos : 0U;
}

size_t linkr_debugger_sigrok_linkr_compress_rle(
	const uint8_t *samples,
	uint32_t count,
	uint8_t bytes_per_sample,
	uint8_t *out,
	size_t out_len)
{
	if (samples == NULL || out == NULL || count == 0U || bytes_per_sample == 0U) {
		return 0U;
	}

	size_t out_pos = 0U;
	uint32_t i = 0U;

	while (i < count) {
		const uint8_t *current = &samples[i * bytes_per_sample];
		uint16_t run_count = 1U;

		while (i + run_count < count && run_count < 65535U) {
			const uint8_t *next = &samples[(i + run_count) * bytes_per_sample];

			if (memcmp(current, next, bytes_per_sample) != 0) {
				break;
			}
			run_count++;
		}

		size_t needed = (size_t)bytes_per_sample + 2U;

		if (out_pos + needed > out_len) {
			return 0U;
		}

		memcpy(&out[out_pos], current, bytes_per_sample);
		out_pos += bytes_per_sample;
		out[out_pos] = (uint8_t)(run_count & 0xffU);
		out[out_pos + 1U] = (uint8_t)((run_count >> 8) & 0xffU);
		out_pos += 2U;

		i += run_count;
	}

	return out_pos;
}

static size_t linkr_debugger_sigrok_linkr_compress_rle_if_smaller_single(
	const uint8_t *samples,
	uint32_t count,
	uint8_t *out,
	size_t out_len,
	size_t normal_len)
{
	size_t out_pos = 0U;
	uint32_t i = 0U;

	while (i < count) {
		uint8_t current = samples[i];
		uint16_t run_count = 1U;

		while (i + run_count < count && run_count < UINT16_MAX &&
		       samples[i + run_count] == current) {
			run_count++;
		}

		if (out_pos + 3U >= normal_len || out_pos + 3U > out_len) {
			return 0U;
		}

		out[out_pos] = current;
		out[out_pos + 1U] = (uint8_t)(run_count & 0xffU);
		out[out_pos + 2U] = (uint8_t)((run_count >> 8) & 0xffU);
		out_pos += 3U;
		i += run_count;
	}

	return out_pos < normal_len ? out_pos : 0U;
}

size_t linkr_debugger_sigrok_linkr_compress_rle_if_smaller(
	const uint8_t *samples,
	uint32_t count,
	uint8_t bytes_per_sample,
	uint8_t *out,
	size_t out_len)
{
	if (samples == NULL || out == NULL || count == 0U || bytes_per_sample == 0U) {
		return 0U;
	}

	size_t normal_len = (size_t)count * bytes_per_sample;
	size_t out_pos = 0U;
	uint32_t i = 0U;

	if (normal_len == 0U) {
		return 0U;
	}
	if (bytes_per_sample == 1U) {
		return linkr_debugger_sigrok_linkr_compress_rle_if_smaller_single(samples,
			count, out, out_len, normal_len);
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

size_t linkr_debugger_sigrok_linkr_encode_packed_data_frame(
	uint32_t sample_index,
	uint16_t sample_count,
	uint16_t channel_mask,
	const uint8_t *packed,
	size_t packed_len,
	bool try_single_rle,
	uint8_t *out,
	size_t out_len)
{
	uint8_t bytes_per_sample = linkr_debugger_sigrok_linkr_bytes_per_sample(channel_mask);
	size_t expected_len = (size_t)sample_count * bytes_per_sample;
	size_t prefix_len = LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES;
	uint8_t compression = LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK;
	size_t payload_len;
	struct linkr_debugger_sigrok_linkr_header header;
	struct linkr_debugger_sigrok_linkr_data_meta meta;

	if (packed == NULL || out == NULL || sample_count == 0U || bytes_per_sample == 0U ||
	    packed_len != expected_len || prefix_len > out_len) {
		return 0U;
	}

	if (try_single_rle && channel_mask == 0x0001U && bytes_per_sample == 1U) {
		payload_len = linkr_debugger_sigrok_linkr_compress_rle_if_smaller(packed,
			sample_count, bytes_per_sample, out + prefix_len, out_len - prefix_len);
		if (payload_len > 0U) {
			compression = LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK_RLE;
		} else {
			if (expected_len > out_len - prefix_len) {
				return 0U;
			}
			payload_len = expected_len;
			memcpy(out + prefix_len, packed, payload_len);
		}
	} else {
		if (expected_len > out_len - prefix_len) {
			return 0U;
		}
		payload_len = expected_len;
		memcpy(out + prefix_len, packed, payload_len);
	}

	meta.sample_index = sample_index;
	meta.sample_count = sample_count;
	meta.compression = compression;
	meta.channel_mask = channel_mask;
	(void)linkr_debugger_sigrok_linkr_encode_data_meta(&meta,
		out + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES,
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES);
	linkr_debugger_sigrok_linkr_init_response_header(&header,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_DATA, 0U,
		(uint16_t)(LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + payload_len));
	(void)linkr_debugger_sigrok_linkr_encode_header(&header, out,
		LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);
	return prefix_len + payload_len;
}

int linkr_debugger_sigrok_linkr_send_data_frame(
	int client_fd,
	const struct linkr_debugger_sigrok_linkr_session *session,
	uint16_t session_id,
	uint32_t sample_index,
	uint16_t sample_count,
	uint8_t compression,
	uint16_t channel_mask,
	const uint8_t *data,
	size_t data_len)
{
	uint8_t header_buf[LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES];
	uint8_t meta_buf[LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES];
	struct linkr_debugger_sigrok_linkr_header header;
	struct linkr_debugger_sigrok_linkr_data_meta meta;

	if (client_fd < 0 || session == NULL) {
		return -EINVAL;
	}
	(void)session_id;

	meta.sample_index = sample_index;
	meta.sample_count = sample_count;
	meta.compression = compression;
	meta.channel_mask = channel_mask;

	linkr_debugger_sigrok_linkr_encode_data_meta(&meta, meta_buf, sizeof(meta_buf));

	linkr_debugger_sigrok_linkr_init_response_header(&header,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_DATA, 0U,
		(uint16_t)(LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + data_len));

	linkr_debugger_sigrok_linkr_encode_header(&header, header_buf, sizeof(header_buf));

	int ret = send_all(client_fd, header_buf, sizeof(header_buf));

	if (ret < 0) {
		return ret;
	}
	ret = send_all(client_fd, meta_buf, sizeof(meta_buf));
	if (ret < 0) {
		return ret;
	}
	if (data_len > 0U && data != NULL) {
		ret = send_all(client_fd, data, data_len);
	}
	return ret;
}

int linkr_debugger_sigrok_linkr_send_event_frame(
	int client_fd,
	const struct linkr_debugger_sigrok_linkr_event *event)
{
	uint8_t header_buf[LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES];
	uint8_t event_buf[LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES];
	struct linkr_debugger_sigrok_linkr_header header;

	if (client_fd < 0 || event == NULL) {
		return -EINVAL;
	}

	linkr_debugger_sigrok_linkr_encode_event(event, event_buf, sizeof(event_buf));

	linkr_debugger_sigrok_linkr_init_response_header(&header,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_EVENT, 0U,
		LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES);

	linkr_debugger_sigrok_linkr_encode_header(&header, header_buf, sizeof(header_buf));

	int ret = send_all(client_fd, header_buf, sizeof(header_buf));

	if (ret < 0) {
		return ret;
	}
	return send_all(client_fd, event_buf, sizeof(event_buf));
}

static void build_error_response(
	const struct linkr_debugger_sigrok_linkr_request *request,
	struct linkr_debugger_sigrok_linkr_header *response_header,
	uint8_t *payload_out,
	size_t payload_out_len,
	uint8_t error_code,
	uint16_t detail,
	size_t *payload_len_out)
{
	struct linkr_debugger_sigrok_linkr_error error_payload = {
		.error_code = error_code,
		.detail = detail,
	};

	*payload_len_out = linkr_debugger_sigrok_linkr_encode_error(&error_payload,
		payload_out, payload_out_len);
	linkr_debugger_sigrok_linkr_init_response_header(response_header,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_ERROR,
		request->header.id,
		(uint16_t)*payload_len_out);
}

void linkr_debugger_sigrok_linkr_build_error_response(
	const struct linkr_debugger_sigrok_linkr_request *request,
	struct linkr_debugger_sigrok_linkr_header *response_header,
	uint8_t *payload_out,
	size_t payload_out_len,
	enum linkr_debugger_sigrok_linkr_error_code error_code,
	uint16_t detail,
	size_t *payload_len_out)
{
	if (payload_len_out != NULL) {
		*payload_len_out = 0U;
	}
	if (request == NULL || response_header == NULL || payload_out == NULL ||
	    payload_len_out == NULL) {
		return;
	}

	build_error_response(request, response_header, payload_out, payload_out_len,
		(uint8_t)error_code, detail, payload_len_out);
}

enum linkr_debugger_sigrok_linkr_error_code
linkr_debugger_sigrok_linkr_start_error_code(int ret, bool invalid_config)
{
	if (ret == -EBUSY) {
		return LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BUSY;
	}
	if (invalid_config || ret == -EINVAL) {
		return LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG;
	}

	return LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INTERNAL;
}

void linkr_debugger_sigrok_linkr_rollback_start_failure(
	struct linkr_debugger_sigrok_linkr_session *session,
	struct linkr_debugger_sigrok_linkr_action_result *action)
{
	if (session != NULL) {
		session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
		session->capture_owner_held = false;
	}
	if (action != NULL) {
		memset(action, 0, sizeof(*action));
	}
}

void linkr_debugger_sigrok_linkr_caps_init(struct linkr_debugger_sigrok_linkr_caps *caps)
{
	if (caps == NULL) {
		return;
	}

	memset(caps, 0, sizeof(*caps));
	caps->mode_count = LINKR_DEBUGGER_SIGROK_LINKR_CAPS_MODE_COUNT;

	caps->modes[0].mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8;
	caps->modes[0].mode_flags = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_CONTINUOUS |
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_NONE |
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_RISING |
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_FALLING |
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_EITHER;
	caps->modes[0].channel_count = 8U;
	caps->modes[0].sample_bytes = 1U;
	caps->modes[0].max_samplerate_khz = 125000U;
	caps->modes[0].compression = LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK |
		LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_RLE;

	caps->modes[1].mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE12;
	caps->modes[1].mode_flags = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_CONTINUOUS |
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_NONE |
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_RISING |
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_FALLING |
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_EITHER;
	caps->modes[1].channel_count = 12U;
	caps->modes[1].sample_bytes = 2U;
	caps->modes[1].max_samplerate_khz = 125000U;
	caps->modes[1].compression = LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK |
		LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_RLE;
}

void linkr_debugger_sigrok_linkr_session_reset(struct linkr_debugger_sigrok_linkr_session *session)
{
	if (session == NULL) {
		return;
	}

	memset(session, 0, sizeof(*session));
	session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_WAIT_HELLO;
	session->next_session_id = 1U;
}

int linkr_debugger_sigrok_linkr_validate_header(
	const struct linkr_debugger_sigrok_linkr_header *header,
	bool *disconnect_required,
	enum linkr_debugger_sigrok_linkr_error_code *error_code)
{
	if (disconnect_required != NULL) {
		*disconnect_required = false;
	}
	if (error_code != NULL) {
		*error_code = 0;
	}
	if (header == NULL) {
		return -EINVAL;
	}
	if (header->magic != LINKR_DEBUGGER_SIGROK_LINKR_MAGIC) {
		if (disconnect_required != NULL) {
			*disconnect_required = true;
		}
		if (error_code != NULL) {
			*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_TYPE;
		}
		return -EPROTO;
	}
	if (header->version != LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION) {
		if (disconnect_required != NULL) {
			*disconnect_required = true;
		}
		if (error_code != NULL) {
			*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_UNSUPPORTED_VERSION;
		}
		return -EPROTO;
	}
	if (header->payload_len > LINKR_DEBUGGER_SIGROK_LINKR_MAX_PAYLOAD_BYTES) {
		if (disconnect_required != NULL) {
			*disconnect_required = true;
		}
		if (error_code != NULL) {
			*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_OVERSIZE_PAYLOAD;
		}
		return -EMSGSIZE;
	}
	return 0;
}

int linkr_debugger_sigrok_linkr_validate_request(
	const struct linkr_debugger_sigrok_linkr_request *request,
	bool *disconnect_required,
	enum linkr_debugger_sigrok_linkr_error_code *error_code)
{
	int ret;

	if (request == NULL) {
		return -EINVAL;
	}
	ret = linkr_debugger_sigrok_linkr_validate_header(&request->header,
		disconnect_required, error_code);
	if (ret < 0) {
		return ret;
	}
	if (request->header.payload_len > 0U && request->payload == NULL) {
		return -EINVAL;
	}

	switch (request->header.type) {
	case LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_REQ:
	case LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CAPS_REQ:
	case LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ:
	case LINKR_DEBUGGER_SIGROK_LINKR_FRAME_STOP_REQ:
		if (request->header.payload_len != 0U) {
			if (error_code != NULL) {
				*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_LENGTH;
			}
			return -EMSGSIZE;
		}
		return 0;
	case LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_REQ:
		if (request->header.payload_len != LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES) {
			if (error_code != NULL) {
				*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_LENGTH;
			}
			return -EMSGSIZE;
		}
		return 0;
	default:
		if (error_code != NULL) {
			*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_TYPE;
		}
		return -ENOTSUP;
	}
}

int linkr_debugger_sigrok_linkr_decode_config(
	const uint8_t *payload,
	size_t payload_len,
	struct linkr_debugger_sigrok_linkr_config *config)
{
	if (payload == NULL || config == NULL || payload_len != LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES) {
		return -EINVAL;
	}

	config->mode_id = payload[0];
	config->trigger_type = payload[1];
	config->trigger_channel = payload[2];
	config->channel_mask = load_le16(&payload[3]);
	config->samplerate_khz = load_le24(&payload[5]);
	config->pre_samples = load_le16(&payload[8]);
	config->post_samples = load_le16(&payload[10]);
	return 0;
}

int linkr_debugger_sigrok_linkr_validate_config(
	const struct linkr_debugger_sigrok_linkr_config *config,
	enum linkr_debugger_sigrok_linkr_error_code *error_code,
	uint16_t *detail)
{
	uint16_t mask_limit;
	uint32_t max_rate_khz;

	if (error_code != NULL) {
		*error_code = 0;
	}
	if (detail != NULL) {
		*detail = 0U;
	}
	if (config == NULL) {
		return -EINVAL;
	}

	switch (config->mode_id) {
	case LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8:
		mask_limit = 0x00ffU;
		max_rate_khz = 125000U;
		break;
	case LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE12:
		mask_limit = 0x0fffU;
		max_rate_khz = 125000U;
		break;
	default:
		if (error_code != NULL) {
			*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG;
		}
		if (detail != NULL) {
			*detail = config->mode_id;
		}
		return -EINVAL;
	}
	if (config->channel_mask == 0U || (config->channel_mask & (uint16_t)~mask_limit) != 0U) {
		if (error_code != NULL) {
			*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG;
		}
		if (detail != NULL) {
			*detail = config->channel_mask;
		}
		return -EINVAL;
	}
	if (config->samplerate_khz == 0U || config->samplerate_khz > max_rate_khz) {
		if (error_code != NULL) {
			*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG;
		}
		if (detail != NULL) {
			*detail = (uint16_t)(config->samplerate_khz > UINT16_MAX ? UINT16_MAX : config->samplerate_khz);
		}
		return -EINVAL;
	}
	if (config->trigger_type > LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_EITHER) {
		if (error_code != NULL) {
			*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG;
		}
		if (detail != NULL) {
			*detail = config->trigger_type;
		}
		return -EINVAL;
	}
	if (config->trigger_type != LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE) {
		if (config->trigger_channel >= 16U ||
		    (config->channel_mask & (uint16_t)(1U << config->trigger_channel)) == 0U) {
			if (error_code != NULL) {
				*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG;
			}
			if (detail != NULL) {
				*detail = config->trigger_channel;
			}
			return -EINVAL;
		}
	}
	if (config->pre_samples > 0U) {
		if (error_code != NULL) {
			*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG;
		}
		if (detail != NULL) {
			*detail = config->pre_samples;
		}
		return -EINVAL;
	}

	return 0;
}

int linkr_debugger_sigrok_linkr_to_la_config(
	const struct linkr_debugger_sigrok_linkr_config *config,
	bool armed,
	struct linkr_debugger_la_config *la_config)
{
	uint8_t logical_channels;
	uint16_t mask_limit;
	uint8_t selected = 0U;

	if (config == NULL || la_config == NULL) {
		return -EINVAL;
	}

	switch (config->mode_id) {
	case LINKR_DEBUGGER_SIGROK_LINKR_MODE_FAST8:
		logical_channels = 8U;
		mask_limit = 0x00ffU;
		break;
	case LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE12:
		logical_channels = 12U;
		mask_limit = 0x0fffU;
		break;
	default:
		return -EINVAL;
	}

	if (config->channel_mask == 0U ||
	    (config->channel_mask & (uint16_t)~mask_limit) != 0U) {
		return -EINVAL;
	}
	if (config->pre_samples > 0U) {
		return -EINVAL;
	}
	if (armed && config->trigger_type != LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE &&
	    (config->trigger_channel >= logical_channels ||
	     (config->channel_mask & (uint16_t)(1U << config->trigger_channel)) == 0U)) {
		return -EINVAL;
	}

	memset(la_config, 0, sizeof(*la_config));
	la_config->sample_rate_hz = config->samplerate_khz * 1000U;
	if (armed) {
		la_config->trigger = (enum linkr_debugger_la_trigger_type)config->trigger_type;
	} else {
		la_config->trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;
		la_config->trigger_pin = 0U;
	}
	la_config->pre_samples = 0U;
	la_config->post_samples = config->post_samples;
	la_config->pin_base = 10U;
	la_config->pin_count = logical_channels;

	for (uint8_t i = 0U; i < logical_channels; i++) {
		if ((config->channel_mask & (1U << i)) == 0U) {
			continue;
		}
		if (armed && config->trigger_type != LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE &&
		    i == config->trigger_channel) {
			la_config->trigger_pin = selected;
		}
		la_config->selected_pins[selected++] =
			(i == 11U) ? 29U : (uint8_t)(10U + i);
	}
	la_config->selected_pin_count = selected;

	return 0;
}

void linkr_debugger_sigrok_linkr_init_response_header(
	struct linkr_debugger_sigrok_linkr_header *header,
	uint8_t type,
	uint32_t id,
	uint16_t payload_len)
{
	if (header == NULL) {
		return;
	}
	header->magic = LINKR_DEBUGGER_SIGROK_LINKR_MAGIC;
	header->version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION;
	header->type = type;
	header->id = id;
	header->payload_len = payload_len;
}

size_t linkr_debugger_sigrok_linkr_encode_header(
	const struct linkr_debugger_sigrok_linkr_header *header,
	uint8_t *out,
	size_t out_len)
{
	if (header == NULL || out == NULL || out_len < LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES) {
		return 0U;
	}
	out[0] = header->magic;
	out[1] = header->version;
	out[2] = header->type;
	store_le32(&out[3], header->id);
	store_le16(&out[7], header->payload_len);
	return LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES;
}

int linkr_debugger_sigrok_linkr_decode_header(
	const uint8_t *data,
	size_t data_len,
	struct linkr_debugger_sigrok_linkr_header *header)
{
	if (data == NULL || header == NULL || data_len < LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES) {
		return -EINVAL;
	}
	header->magic = data[0];
	header->version = data[1];
	header->type = data[2];
	header->id = load_le32(&data[3]);
	header->payload_len = load_le16(&data[7]);
	return 0;
}

int linkr_debugger_sigrok_linkr_decode_next_request_frame(
	const uint8_t *data,
	size_t data_len,
	size_t offset,
	struct linkr_debugger_sigrok_linkr_request *request,
	size_t *next_offset,
	bool *disconnect_required,
	enum linkr_debugger_sigrok_linkr_error_code *error_code)
{
	struct linkr_debugger_sigrok_linkr_header header;
	size_t payload_offset;
	size_t frame_len;
	int ret;

	if (disconnect_required != NULL) {
		*disconnect_required = false;
	}
	if (error_code != NULL) {
		*error_code = 0;
	}
	if (request == NULL || next_offset == NULL || data == NULL || offset > data_len) {
		return -EINVAL;
	}
	if (data_len - offset < LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES) {
		if (error_code != NULL) {
			*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_LENGTH;
		}
		return -EMSGSIZE;
	}

	ret = linkr_debugger_sigrok_linkr_decode_header(data + offset,
		data_len - offset, &header);
	if (ret < 0) {
		return ret;
	}
	ret = linkr_debugger_sigrok_linkr_validate_header(&header,
		disconnect_required, error_code);
	if (ret < 0) {
		return ret;
	}
	if (header.payload_len > LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_REQUEST_BYTES) {
		if (disconnect_required != NULL) {
			*disconnect_required = true;
		}
		if (error_code != NULL) {
			*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_OVERSIZE_PAYLOAD;
		}
		return -EMSGSIZE;
	}

	payload_offset = offset + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES;
	frame_len = LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES + (size_t)header.payload_len;
	if (frame_len > data_len - offset) {
		if (error_code != NULL) {
			*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_LENGTH;
		}
		return -EMSGSIZE;
	}

	request->header = header;
	request->payload = header.payload_len > 0U ? data + payload_offset : NULL;
	*next_offset = offset + frame_len;
	return 0;
}

size_t linkr_debugger_sigrok_linkr_encode_hello(
	const struct linkr_debugger_sigrok_linkr_hello *hello,
	uint8_t *out,
	size_t out_len)
{
	if (hello == NULL || out == NULL || out_len < LINKR_DEBUGGER_SIGROK_LINKR_HELLO_BYTES) {
		return 0U;
	}
	out[0] = hello->protocol_version;
	out[1] = hello->server_flags;
	out[2] = hello->mode_count;
	store_le16(&out[3], hello->max_payload_len);
	return LINKR_DEBUGGER_SIGROK_LINKR_HELLO_BYTES;
}

size_t linkr_debugger_sigrok_linkr_encode_caps(
	const struct linkr_debugger_sigrok_linkr_caps *caps,
	uint8_t *out,
	size_t out_len)
{
	const size_t expected_len = 1U + ((size_t)LINKR_DEBUGGER_SIGROK_LINKR_CAPS_MODE_COUNT *
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_CAPS_BYTES);

	if (caps == NULL || out == NULL || out_len < expected_len) {
		return 0U;
	}
	out[0] = caps->mode_count;
	for (size_t i = 0U; i < LINKR_DEBUGGER_SIGROK_LINKR_CAPS_MODE_COUNT; i++) {
		uint8_t *mode = &out[1U + (i * LINKR_DEBUGGER_SIGROK_LINKR_MODE_CAPS_BYTES)];

		mode[0] = caps->modes[i].mode_id;
		mode[1] = caps->modes[i].mode_flags;
		mode[2] = caps->modes[i].channel_count;
		mode[3] = caps->modes[i].sample_bytes;
		store_le24(&mode[4], caps->modes[i].max_samplerate_khz);
		mode[7] = caps->modes[i].compression;
	}
	return expected_len;
}

size_t linkr_debugger_sigrok_linkr_encode_ack(
	const struct linkr_debugger_sigrok_linkr_ack *ack,
	uint8_t *out,
	size_t out_len)
{
	if (ack == NULL || out == NULL || out_len < LINKR_DEBUGGER_SIGROK_LINKR_ACK_BYTES) {
		return 0U;
	}
	store_le16(&out[0], ack->session_id);
	out[2] = ack->state;
	store_le24(&out[3], ack->actual_rate_khz);
	return LINKR_DEBUGGER_SIGROK_LINKR_ACK_BYTES;
}

size_t linkr_debugger_sigrok_linkr_encode_event(
	const struct linkr_debugger_sigrok_linkr_event *event,
	uint8_t *out,
	size_t out_len)
{
	if (event == NULL || out == NULL || out_len < LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES) {
		return 0U;
	}
	store_le16(&out[0], event->session_id);
	out[2] = event->type_detail;
	store_le24(&out[3], event->sample_index);
	return LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES;
}

size_t linkr_debugger_sigrok_linkr_encode_data_meta(
	const struct linkr_debugger_sigrok_linkr_data_meta *meta,
	uint8_t *out,
	size_t out_len)
{
	if (meta == NULL || out == NULL || out_len < LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES) {
		return 0U;
	}
	store_le24(&out[0], meta->sample_index);
	store_le16(&out[3], meta->sample_count);
	out[5] = meta->compression;
	store_le16(&out[6], meta->channel_mask);
	return LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES;
}

size_t linkr_debugger_sigrok_linkr_encode_error(
	const struct linkr_debugger_sigrok_linkr_error *error,
	uint8_t *out,
	size_t out_len)
{
	if (error == NULL || out == NULL || out_len < LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BYTES) {
		return 0U;
	}
	out[0] = error->error_code;
	store_le16(&out[1], error->detail);
	return LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BYTES;
}

static void fill_ack(struct linkr_debugger_sigrok_linkr_ack *ack,
	const struct linkr_debugger_sigrok_linkr_session *session,
	enum linkr_debugger_sigrok_linkr_session_state state)
{
	static const uint8_t state_map[] = {
		[LINKR_DEBUGGER_SIGROK_LINKR_SESSION_WAIT_HELLO] = 0U,
		[LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY] = 0U,
		[LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED] = 1U,
		[LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED] = 2U,
		[LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING] = 3U,
	};
	ack->session_id = session->active_session_id;
	ack->state = state_map[state];
	ack->actual_rate_khz = linkr_debugger_logic_analyzer_actual_rate(
		session->config.samplerate_khz * 1000U) / 1000U;
}

int linkr_debugger_sigrok_linkr_handle_request(
	struct linkr_debugger_sigrok_linkr_session *session,
	enum linkr_debugger_capture_owner current_owner,
	const struct linkr_debugger_sigrok_linkr_request *request,
	struct linkr_debugger_sigrok_linkr_header *response_header,
	uint8_t *payload_out,
	size_t payload_out_len,
	size_t *payload_len_out,
	struct linkr_debugger_sigrok_linkr_action_result *action,
	bool *disconnect_required)
{
	enum linkr_debugger_sigrok_linkr_error_code error_code = 0;
	int ret;

	if (payload_len_out != NULL) {
		*payload_len_out = 0U;
	}
	if (disconnect_required != NULL) {
		*disconnect_required = false;
	}
	if (action != NULL) {
		memset(action, 0, sizeof(*action));
	}
	if (session == NULL || request == NULL || response_header == NULL || payload_out == NULL ||
	    payload_len_out == NULL) {
		return -EINVAL;
	}

	ret = linkr_debugger_sigrok_linkr_validate_request(request, disconnect_required, &error_code);
	if (ret == -ENOTSUP || ret == -EMSGSIZE) {
		build_error_response(request, response_header, payload_out,
			payload_out_len, (uint8_t)error_code,
			ret == -ENOTSUP ? request->header.type : (uint16_t)request->header.payload_len,
			payload_len_out);
		return 0;
	}
	if (ret < 0) {
		return ret;
	}

	switch (request->header.type) {
	case LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_REQ: {
		struct linkr_debugger_sigrok_linkr_hello hello;

		if (session->state != LINKR_DEBUGGER_SIGROK_LINKR_SESSION_WAIT_HELLO) {
			build_error_response(request, response_header, payload_out,
				payload_out_len, LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_STATE,
				(uint16_t)session->state, payload_len_out);
			return 0;
		}
		hello.protocol_version = LINKR_DEBUGGER_SIGROK_LINKR_PROTOCOL_VERSION;
		hello.server_flags = 0U;
		hello.mode_count = LINKR_DEBUGGER_SIGROK_LINKR_CAPS_MODE_COUNT;
		hello.max_payload_len = LINKR_DEBUGGER_SIGROK_LINKR_MAX_PAYLOAD_BYTES;
		*payload_len_out = linkr_debugger_sigrok_linkr_encode_hello(&hello,
			payload_out, payload_out_len);
		linkr_debugger_sigrok_linkr_init_response_header(response_header,
			LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_RESP,
			request->header.id, (uint16_t)*payload_len_out);
		session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_READY;
		return 0;
	}
	case LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CAPS_REQ: {
		struct linkr_debugger_sigrok_linkr_caps caps;

		if (session->state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_WAIT_HELLO) {
			build_error_response(request, response_header, payload_out,
				payload_out_len, LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_STATE,
				(uint16_t)session->state, payload_len_out);
			return 0;
		}
		linkr_debugger_sigrok_linkr_caps_init(&caps);
		*payload_len_out = linkr_debugger_sigrok_linkr_encode_caps(&caps,
			payload_out, payload_out_len);
		linkr_debugger_sigrok_linkr_init_response_header(response_header,
			LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CAPS_RESP,
			request->header.id, (uint16_t)*payload_len_out);
		return 0;
	}
	case LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_REQ: {
		struct linkr_debugger_sigrok_linkr_config config;
		struct linkr_debugger_sigrok_linkr_ack ack;
		uint16_t detail = 0U;

		if (session->state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_WAIT_HELLO ||
		    session->state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED ||
		    session->state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING) {
			build_error_response(request, response_header, payload_out,
				payload_out_len, LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_STATE,
				(uint16_t)session->state, payload_len_out);
			return 0;
		}
		ret = linkr_debugger_sigrok_linkr_decode_config(request->payload,
			request->header.payload_len, &config);
		if (ret < 0 || linkr_debugger_sigrok_linkr_validate_config(&config,
			&error_code, &detail) < 0) {
			build_error_response(request, response_header, payload_out,
				payload_out_len, LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG,
				detail, payload_len_out);
			return 0;
		}
		session->config = config;
		session->active_session_id = session->next_session_id++;
		session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
		fill_ack(&ack, session, session->state);
		*payload_len_out = linkr_debugger_sigrok_linkr_encode_ack(&ack,
			payload_out, payload_out_len);
		linkr_debugger_sigrok_linkr_init_response_header(response_header,
			LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_RESP,
			request->header.id, (uint16_t)*payload_len_out);
		return 0;
	}
	case LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_REQ: {
		struct linkr_debugger_sigrok_linkr_ack ack;

		if (session->state != LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED) {
			build_error_response(request, response_header, payload_out,
				payload_out_len, LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_STATE,
				(uint16_t)session->state, payload_len_out);
			return 0;
		}
		if (current_owner != LINKR_DEBUGGER_CAPTURE_OWNER_NONE &&
		    current_owner != LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR) {
			build_error_response(request, response_header, payload_out,
				payload_out_len, LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BUSY,
				(uint16_t)current_owner, payload_len_out);
			return 0;
		}
		if (session->config.trigger_type == LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE) {
			session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING;
			if (action != NULL) {
				action->capture_action = LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_START_IMMEDIATE;
				action->has_event = true;
				action->event.session_id = session->active_session_id;
				action->event.type_detail = (uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_RUNNING;
			}
		} else {
			session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED;
			if (action != NULL) {
				action->capture_action = LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_START_ARMED;
				action->has_event = true;
				action->event.session_id = session->active_session_id;
				action->event.type_detail = (uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_ARMED;
			}
		}
		fill_ack(&ack, session, session->state);
		*payload_len_out = linkr_debugger_sigrok_linkr_encode_ack(&ack,
			payload_out, payload_out_len);
		linkr_debugger_sigrok_linkr_init_response_header(response_header,
			LINKR_DEBUGGER_SIGROK_LINKR_FRAME_START_RESP,
			request->header.id, (uint16_t)*payload_len_out);
		return 0;
	}
	case LINKR_DEBUGGER_SIGROK_LINKR_FRAME_STOP_REQ: {
		struct linkr_debugger_sigrok_linkr_ack ack;

		if (session->state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED) {
			fill_ack(&ack, session, session->state);
			*payload_len_out = linkr_debugger_sigrok_linkr_encode_ack(&ack,
				payload_out, payload_out_len);
			linkr_debugger_sigrok_linkr_init_response_header(response_header,
				LINKR_DEBUGGER_SIGROK_LINKR_FRAME_STOP_RESP,
				request->header.id, (uint16_t)*payload_len_out);
			return 0;
		}
		if (session->state != LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED &&
		    session->state != LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING) {
			build_error_response(request, response_header, payload_out,
				payload_out_len, LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_STATE,
				(uint16_t)session->state, payload_len_out);
			return 0;
		}
		if (action != NULL) {
			action->capture_action = LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_STOP;
			action->has_event = true;
			action->event.session_id = session->active_session_id;
			action->event.type_detail = (uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_STOPPED;
		}
		session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
		fill_ack(&ack, session, session->state);
		*payload_len_out = linkr_debugger_sigrok_linkr_encode_ack(&ack,
			payload_out, payload_out_len);
		linkr_debugger_sigrok_linkr_init_response_header(response_header,
			LINKR_DEBUGGER_SIGROK_LINKR_FRAME_STOP_RESP,
			request->header.id, (uint16_t)*payload_len_out);
		return 0;
	}
	default:
		return -ENOTSUP;
	}
}

#ifndef LINKR_DEBUGGER_SIGROK_LINKR_HOST_TEST
static void close_fd(int *fd)
{
	if (*fd >= 0) {
		(void)zsock_close(*fd);
		*fd = -1;
	}
}

static int send_all(int fd, const uint8_t *data, size_t len)
{
	int64_t deadline = k_uptime_get() + LINKR_DEBUGGER_SIGROK_LINKR_SEND_TIMEOUT_MS;

	while (len > 0U) {
		ssize_t ret = zsock_send(fd, data, len, 0);
		if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (k_uptime_get() >= deadline) {
					return -ETIMEDOUT;
				}
				k_msleep(1);
				continue;
			}
			return -errno;
		}
		if (ret == 0) {
			return -ECONNRESET;
		}
		data += (size_t)ret;
		len -= (size_t)ret;
	}
	return 0;
}

static int recv_exact_streaming(struct linkr_debugger_sigrok_linkr_runtime *runtime,
	uint8_t *data, size_t len)
{
	int64_t idle_deadline = k_uptime_get() + LINKR_DEBUGGER_SIGROK_LINKR_RECV_TIMEOUT_MS;
	int ret;

	while (len > 0U) {
		struct pollfd pfd = {
			.fd = runtime->client_fd,
			.events = POLLIN | POLLHUP | POLLERR | POLLNVAL,
		};

		ret = sigrok_linkr_stream_drain(runtime);
		if (ret < 0) {
			return ret;
		}
		sigrok_linkr_stop_capture_if_pending(runtime);

		ret = zsock_poll(&pfd, 1, LINKR_DEBUGGER_SIGROK_LINKR_STREAM_RECV_SLICE_MS);
		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -errno;
		}
		if (ret == 0) {
			if (sigrok_linkr_stream_busy(runtime)) {
				idle_deadline = k_uptime_get() +
					LINKR_DEBUGGER_SIGROK_LINKR_RECV_TIMEOUT_MS;
				continue;
			}
			if (k_uptime_get() >= idle_deadline) {
				return -ETIMEDOUT;
			}
			continue;
		}
		if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
			return -ECONNRESET;
		}
		if ((pfd.revents & POLLHUP) != 0 && (pfd.revents & POLLIN) == 0) {
			return -ECONNRESET;
		}
		if ((pfd.revents & POLLIN) == 0) {
			continue;
		}

		ssize_t got = zsock_recv(runtime->client_fd, data, len, 0);
		if (got < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (sigrok_linkr_stream_busy(runtime)) {
					idle_deadline = k_uptime_get() +
						LINKR_DEBUGGER_SIGROK_LINKR_RECV_TIMEOUT_MS;
					continue;
				}
				if (k_uptime_get() >= idle_deadline) {
					return -ETIMEDOUT;
				}
				continue;
			}
			return -errno;
		}
		if (got == 0) {
			return -ECONNRESET;
		}
		data += (size_t)got;
		len -= (size_t)got;
		idle_deadline = k_uptime_get() + LINKR_DEBUGGER_SIGROK_LINKR_RECV_TIMEOUT_MS;
	}

	return 0;
}

static int send_frame(int fd,
	const struct linkr_debugger_sigrok_linkr_header *header,
	const uint8_t *payload,
	size_t payload_len)
{
	uint8_t encoded_header[LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES];
	int ret;

	if (linkr_debugger_sigrok_linkr_encode_header(header, encoded_header,
	    sizeof(encoded_header)) != sizeof(encoded_header)) {
		return -EINVAL;
	}
	ret = send_all(fd, encoded_header, sizeof(encoded_header));
	if (ret < 0) {
		return ret;
	}
	if (payload_len == 0U) {
		return 0;
	}
	return send_all(fd, payload, payload_len);
}

static void session_loop(int fd)
{
	struct linkr_debugger_sigrok_linkr_runtime *runtime = &linkr_debugger_sigrok_linkr_runtime;
	struct zsock_timeval send_tv;

	linkr_debugger_sigrok_linkr_session_reset(&runtime->session);
	sigrok_linkr_stream_reset_queue(runtime);
	runtime->next_sequence_id = 1U;
	runtime->client_fd = fd;
	send_tv.tv_sec = LINKR_DEBUGGER_SIGROK_LINKR_SEND_TIMEOUT_MS / 1000U;
	send_tv.tv_usec = 0;
	(void)zsock_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_tv, sizeof(send_tv));

	for (;;) {
		uint8_t rx_header[LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES];
		struct linkr_debugger_sigrok_linkr_header request_header;
		struct linkr_debugger_sigrok_linkr_request request;
		struct linkr_debugger_sigrok_linkr_header response_header;
		struct linkr_debugger_sigrok_linkr_action_result action;
		size_t tx_payload_len = 0U;
		bool disconnect_required = false;
		enum linkr_debugger_capture_owner current_owner;
		int ret;

		ret = recv_exact_streaming(runtime, rx_header, sizeof(rx_header));
		if (ret < 0) {
			sigrok_linkr_cleanup_capture(runtime);
			return;
		}
		ret = linkr_debugger_sigrok_linkr_decode_header(rx_header, sizeof(rx_header), &request_header);
		if (ret < 0) {
			sigrok_linkr_cleanup_capture(runtime);
			return;
		}
		if (request_header.payload_len >
		    LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_REQUEST_BYTES) {
			sigrok_linkr_cleanup_capture(runtime);
			return;
		}
		if (request_header.payload_len > 0U) {
			ret = recv_exact_streaming(runtime, runtime->rx_control_payload,
				request_header.payload_len);
			if (ret < 0) {
				sigrok_linkr_cleanup_capture(runtime);
				return;
			}
		}
		request.header = request_header;
		request.payload = request_header.payload_len > 0U ? runtime->rx_control_payload : NULL;
		current_owner = linkr_debugger_capture_arbiter_owner();
		ret = linkr_debugger_sigrok_linkr_handle_request(&runtime->session,
			current_owner,
			&request,
			&response_header,
			runtime->tx_control_payload,
			sizeof(runtime->tx_control_payload),
			&tx_payload_len,
			&action,
			&disconnect_required);
		if (ret < 0) {
			sigrok_linkr_cleanup_capture(runtime);
			return;
		}
		if (action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_STOP) {
			sigrok_linkr_cleanup_capture(runtime);
		}
		if (action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_START_IMMEDIATE ||
		    action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_START_ARMED) {
			struct linkr_debugger_la_config la_config;
			bool armed = action.capture_action ==
				LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_START_ARMED;
			enum linkr_debugger_sigrok_linkr_error_code error_code;

			ret = linkr_debugger_sigrok_linkr_to_la_config(
				&runtime->session.config, armed, &la_config);
			if (ret < 0) {
				error_code = linkr_debugger_sigrok_linkr_start_error_code(ret, true);
				linkr_debugger_sigrok_linkr_rollback_start_failure(
					&runtime->session, &action);
				linkr_debugger_sigrok_linkr_build_error_response(&request,
					&response_header, runtime->tx_control_payload,
					sizeof(runtime->tx_control_payload), error_code,
					(uint16_t)(-ret), &tx_payload_len);
			} else if (!linkr_debugger_capture_arbiter_try_acquire(
			    LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR)) {
				ret = -EBUSY;
				error_code = linkr_debugger_sigrok_linkr_start_error_code(ret, false);
				linkr_debugger_sigrok_linkr_rollback_start_failure(
					&runtime->session, &action);
				linkr_debugger_sigrok_linkr_build_error_response(&request,
					&response_header, runtime->tx_control_payload,
					sizeof(runtime->tx_control_payload), error_code,
					(uint16_t)(-ret), &tx_payload_len);
			} else {
				runtime->session.capture_owner_held = true;
				sigrok_linkr_stream_reset_queue(runtime);
				runtime->session.sample_index = 0U;
				runtime->session.emitted_samples = 0U;
				ret = linkr_debugger_logic_analyzer_start_stream(&la_config,
					sigrok_linkr_stream_callback, runtime);
				if (ret < 0) {
					error_code = linkr_debugger_sigrok_linkr_start_error_code(ret, false);
					(void)linkr_debugger_capture_arbiter_release(
						LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR);
					linkr_debugger_sigrok_linkr_rollback_start_failure(
						&runtime->session, &action);
					linkr_debugger_sigrok_linkr_build_error_response(&request,
						&response_header, runtime->tx_control_payload,
						sizeof(runtime->tx_control_payload), error_code,
						(uint16_t)(-ret), &tx_payload_len);
				}
			}
		}
		ret = send_frame(fd, &response_header, runtime->tx_control_payload, tx_payload_len);
		if (ret < 0) {
			sigrok_linkr_cleanup_capture(runtime);
			return;
		}
		if (action.has_event) {
			struct linkr_debugger_sigrok_linkr_header event_header;
			uint8_t event_buf[LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES];
			size_t event_len;

			event_len = linkr_debugger_sigrok_linkr_encode_event(&action.event,
				event_buf, sizeof(event_buf));
			linkr_debugger_sigrok_linkr_init_response_header(&event_header,
				LINKR_DEBUGGER_SIGROK_LINKR_FRAME_EVENT,
				0U,
				(uint16_t)event_len);
			ret = send_frame(fd, &event_header, event_buf, event_len);
			if (ret < 0) {
				sigrok_linkr_cleanup_capture(runtime);
				return;
			}
		}
		if (disconnect_required) {
			sigrok_linkr_cleanup_capture(runtime);
			return;
		}
	}
}

static void server_thread(void *p1, void *p2, void *p3)
{
	struct sockaddr_in addr;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (linkr_debugger_sigrok_linkr_runtime.listen_fd < 0) {
		int fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		int reuse = 1;

		if (fd < 0) {
			k_msleep(500);
			continue;
		}
		(void)zsock_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(LINKR_DEBUGGER_SIGROK_LINKR_PORT);
		(void)zsock_inet_pton(NET_AF_INET, LINKR_DEBUGGER_SIGROK_LINKR_BIND_ADDR, &addr.sin_addr);
		if (zsock_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
		    zsock_listen(fd, 1U) < 0) {
			close_fd(&fd);
			k_msleep(500);
			continue;
		}
		linkr_debugger_sigrok_linkr_runtime.listen_fd = fd;
	}

	for (;;) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client_fd = zsock_accept(linkr_debugger_sigrok_linkr_runtime.listen_fd,
			(struct sockaddr *)&client_addr, &client_len);

		if (client_fd < 0) {
			k_msleep(20);
			continue;
		}
		if (linkr_debugger_sigrok_linkr_runtime.client_fd >= 0) {
			close_fd(&client_fd);
			continue;
		}
		linkr_debugger_sigrok_linkr_runtime.client_fd = client_fd;
		session_loop(client_fd);
		close_fd(&linkr_debugger_sigrok_linkr_runtime.client_fd);
	}
}

static K_THREAD_STACK_DEFINE(server_stack, 4096U);
static struct k_thread server_thread_data;

int linkr_debugger_sigrok_linkr_init(void)
{
	k_fifo_init(&linkr_debugger_sigrok_linkr_runtime.stream_fifo);
	atomic_set(&linkr_debugger_sigrok_linkr_runtime.stream_qdepth, 0);
	atomic_set(&linkr_debugger_sigrok_linkr_runtime.stream_dropped, 0);
	atomic_set(&linkr_debugger_sigrok_linkr_runtime.stream_stop_pending, 0);
	(void)k_thread_create(&server_thread_data,
		server_stack,
		K_THREAD_STACK_SIZEOF(server_stack),
		server_thread, NULL, NULL, NULL,
		LINKR_DEBUGGER_SIGROK_LINKR_PRIORITY, 0, K_NO_WAIT);
	(void)k_thread_name_set(&server_thread_data, "sigrok-linkr");
	return 0;
}

bool linkr_debugger_sigrok_linkr_tcp_active(void)
{
	return linkr_debugger_sigrok_linkr_runtime.client_fd >= 0;
}
#else
int linkr_debugger_sigrok_linkr_init(void)
{
	return 0;
}

bool linkr_debugger_sigrok_linkr_tcp_active(void)
{
	return false;
}
#endif
