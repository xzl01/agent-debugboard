/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_sigrok_linkr.h"
#include "linkr_debugger_logic_analyzer.h"
#include "linkr_debugger_capture_arbiter.h"

#ifndef LINKR_DEBUGGER_SIGROK_LINKR_HOST_TEST
#include "linkr_debugger_ws.h"
#endif

#include <errno.h>
#include <string.h>

#define LINKR_DEBUGGER_SIGROK_LINKR_WIDE11_ARENA_QUIESCE_TIMEOUT_MS 100U

#ifdef LINKR_DEBUGGER_SIGROK_LINKR_HOST_TEST
int linkr_debugger_ws_sigrok_burst_pool_begin(uint32_t session_id,
	uint32_t stream_generation, uint32_t source_generation)
{
	(void)session_id;
	(void)stream_generation;
	(void)source_generation;
	return 0;
}

void linkr_debugger_ws_sigrok_telemetry_pause_acquire(void)
{
}

void linkr_debugger_ws_sigrok_telemetry_pause_release(void)
{
}

void linkr_debugger_ws_sigrok_burst_pool_abort(uint32_t session_id,
	uint32_t stream_generation)
{
	(void)session_id;
	(void)stream_generation;
}

static int send_all(int fd, const uint8_t *data, size_t len)
{
	(void)fd;
	(void)data;
	(void)len;
	return -ENOTSUP;
}
#endif

static bool linkr_debugger_sigrok_linkr_config_is_legacy_wide11_exact_burst(
	const struct linkr_debugger_sigrok_linkr_config *config);
static int linkr_debugger_sigrok_linkr_map_la_config(
	const struct linkr_debugger_sigrok_linkr_config *config,
	bool armed,
	struct linkr_debugger_la_config *la_config);

uint32_t linkr_debugger_sigrok_linkr_packed_burst_max_chunk_samples(
	uint8_t bytes_per_sample)
{
	return bytes_per_sample == 0U ? 0U :
		LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_PAYLOAD_BYTES / bytes_per_sample;
}

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
#define LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SPACE_WAIT_MS 2000U
#define LINKR_DEBUGGER_SIGROK_LINKR_PRIORITY K_PRIO_PREEMPT(8)

BUILD_ASSERT(LINKR_DEBUGGER_SIGROK_LINKR_STREAM_QDEPTH_LIMIT == 32U,
	"Sigrok TCP stream queue depth must stay aligned with HIL coverage");
BUILD_ASSERT(LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_REQUEST_BYTES ==
	LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES,
	"Sigrok control request buffer must fit CONFIG_V2_REQ payload capacity");
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
	bool raw_burst_slot;
	uint8_t raw_burst_slot_index;
	uint8_t data[];
};

struct linkr_debugger_sigrok_linkr_runtime;
static void sigrok_linkr_stream_mark_stop(
	struct linkr_debugger_sigrok_linkr_runtime *runtime);
static int sigrok_linkr_stream_enqueue_event(
	struct linkr_debugger_sigrok_linkr_runtime *runtime,
	enum linkr_debugger_sigrok_linkr_event_type event_type,
	uint16_t session_id,
	uint32_t sample_index);
static bool sigrok_linkr_stream_sink_shape_matches(
	const struct linkr_debugger_sigrok_linkr_session *session,
	uint8_t bytes_per_sample);
static size_t linkr_debugger_sigrok_linkr_encode_packed_payload(
	uint16_t sample_count,
	uint16_t channel_mask,
	const uint8_t *packed,
	size_t packed_len,
	uint8_t *compression,
	uint8_t *out,
	size_t out_len);

struct sigrok_linkr_raw_burst_queue_item {
	void *fifo_reserved;
	size_t len;
	bool raw_burst_slot;
	uint8_t raw_burst_slot_index;
	uint8_t data[LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_MAX_FRAME_BYTES];
};

BUILD_ASSERT(offsetof(struct sigrok_linkr_raw_burst_queue_item, data) ==
	offsetof(struct sigrok_linkr_stream_queue_item, data),
	"Raw burst queue items must share the stream queue prefix");
BUILD_ASSERT(LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_PAYLOAD_BYTES ==
	LINKR_DEBUGGER_SIGROK_LINKR_MAX_DATA_BYTES / 2U,
	"Raw burst WIDE11 payload must stay at 1024 uint16 samples");
BUILD_ASSERT((LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT *
	sizeof(struct sigrok_linkr_raw_burst_queue_item)) <
	LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_QUEUE_MEMORY_LIMIT_BYTES,
	"Raw TCP burst queue must stay within the bounded memory budget");

struct sigrok_linkr_raw_burst_pool {
	struct k_mutex lock;
	struct k_sem space_sem;
	bool active;
	bool aborted;
	bool triggered;
	bool triggered_committed;
	bool terminal_committed;
	uint16_t session_id;
	uint32_t queued_items;
	uint32_t emitted_samples;
	uint32_t sample_index;
	struct linkr_debugger_sigrok_linkr_runtime *runtime;
	uint8_t encode_scratch[LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_PAYLOAD_BYTES];
	uint8_t slot_state[LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT];
	struct sigrok_linkr_raw_burst_queue_item slots[
		LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT];
};

enum sigrok_linkr_raw_burst_slot_state {
	SIGROK_LINKR_RAW_BURST_SLOT_FREE = 0,
	SIGROK_LINKR_RAW_BURST_SLOT_LEASED,
	SIGROK_LINKR_RAW_BURST_SLOT_QUEUED,
};

struct linkr_debugger_sigrok_linkr_runtime {
	int listen_fd;
	int client_fd;
	struct linkr_debugger_sigrok_linkr_session session;
	struct k_fifo stream_fifo;
	struct sigrok_linkr_raw_burst_pool raw_burst;
	atomic_t stream_qdepth;
	atomic_t stream_dropped;
	atomic_t stream_stop_pending;
	uint32_t next_sequence_id;
	uint8_t rx_control_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_REQUEST_BYTES];
	uint8_t tx_control_payload[LINKR_DEBUGGER_SIGROK_LINKR_CONTROL_MAX_RESPONSE_BYTES];
};

static struct linkr_debugger_sigrok_linkr_runtime linkr_debugger_sigrok_linkr_runtime
	Z_GENERIC_SECTION(.bss.pre_capture.sigrok_runtime);
BUILD_ASSERT(sizeof(linkr_debugger_sigrok_linkr_runtime) == 27168U);

static void sigrok_linkr_raw_burst_release_slot_locked(
	struct sigrok_linkr_raw_burst_pool *pool, uint8_t slot_index)
{
	if (slot_index >= LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT ||
	    pool->slot_state[slot_index] == SIGROK_LINKR_RAW_BURST_SLOT_FREE) {
		return;
	}
	if (pool->slot_state[slot_index] == SIGROK_LINKR_RAW_BURST_SLOT_QUEUED &&
	    pool->queued_items > 0U) {
		pool->queued_items--;
	}
	pool->slot_state[slot_index] = SIGROK_LINKR_RAW_BURST_SLOT_FREE;
	k_sem_give(&pool->space_sem);
}

static void sigrok_linkr_raw_burst_wake_all(struct sigrok_linkr_raw_burst_pool *pool)
{
	for (uint32_t i = 0U; i < LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT; i++) {
		k_sem_give(&pool->space_sem);
	}
}

static void sigrok_linkr_stream_free_item(
	struct linkr_debugger_sigrok_linkr_runtime *runtime,
	struct sigrok_linkr_stream_queue_item *item)
{
	if (item == NULL) {
		return;
	}
	if (item->raw_burst_slot) {
		k_mutex_lock(&runtime->raw_burst.lock, K_FOREVER);
		sigrok_linkr_raw_burst_release_slot_locked(&runtime->raw_burst,
			item->raw_burst_slot_index);
		k_mutex_unlock(&runtime->raw_burst.lock);
	} else {
		k_free(item);
	}
}

static void sigrok_linkr_raw_burst_abort(
	struct linkr_debugger_sigrok_linkr_runtime *runtime)
{
	struct sigrok_linkr_raw_burst_pool *pool = &runtime->raw_burst;

	k_mutex_lock(&pool->lock, K_FOREVER);
	pool->active = false;
	pool->aborted = true;
	sigrok_linkr_raw_burst_wake_all(pool);
	k_mutex_unlock(&pool->lock);
}

static int sigrok_linkr_raw_burst_begin(
	struct linkr_debugger_sigrok_linkr_runtime *runtime)
{
	struct sigrok_linkr_raw_burst_pool *pool = &runtime->raw_burst;

	k_mutex_lock(&pool->lock, K_FOREVER);
	if (pool->active || pool->queued_items != 0U) {
		k_mutex_unlock(&pool->lock);
		return -EBUSY;
	}
	while (k_sem_take(&pool->space_sem, K_NO_WAIT) == 0) {
	}
	for (uint32_t i = 0U; i < LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT; i++) {
		pool->slot_state[i] = SIGROK_LINKR_RAW_BURST_SLOT_FREE;
		pool->slots[i].raw_burst_slot = true;
		pool->slots[i].raw_burst_slot_index = (uint8_t)i;
		k_sem_give(&pool->space_sem);
	}
	pool->active = true;
	pool->aborted = false;
	pool->triggered = runtime->session.config.trigger_type !=
		LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE;
	pool->triggered_committed = false;
	pool->terminal_committed = false;
	pool->session_id = runtime->session.active_session_id;
	pool->queued_items = 0U;
	pool->emitted_samples = 0U;
	pool->sample_index = 0U;
	pool->runtime = runtime;
	k_mutex_unlock(&pool->lock);
	return 0;
}

static int sigrok_linkr_raw_burst_lease(uint32_t sample_count, uint8_t bytes_per_sample,
	void *user_data, struct linkr_debugger_la_stream_sink_lease *lease)
{
	struct linkr_debugger_sigrok_linkr_runtime *runtime = user_data;
	struct sigrok_linkr_raw_burst_pool *pool;
	uint32_t max_samples;

	if (runtime == NULL || lease == NULL || sample_count == 0U ||
	    !sigrok_linkr_stream_sink_shape_matches(&runtime->session, bytes_per_sample)) {
		return -EINVAL;
	}
	max_samples = linkr_debugger_sigrok_linkr_packed_burst_max_chunk_samples(bytes_per_sample);
	if (max_samples == 0U || sample_count > max_samples) {
		return -EINVAL;
	}
	pool = &runtime->raw_burst;
	for (;;) {
		int ret = k_sem_take(&pool->space_sem,
			K_MSEC(LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SPACE_WAIT_MS));

		if (ret < 0) {
			return ret;
		}
		k_mutex_lock(&pool->lock, K_FOREVER);
		if (!pool->active || pool->aborted || runtime->client_fd < 0) {
			k_mutex_unlock(&pool->lock);
			k_sem_give(&pool->space_sem);
			return -ECANCELED;
		}
		for (uint32_t i = 0U; i < LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT; i++) {
			if (pool->slot_state[i] != SIGROK_LINKR_RAW_BURST_SLOT_FREE) {
				continue;
			}
			pool->slot_state[i] = SIGROK_LINKR_RAW_BURST_SLOT_LEASED;
			lease->payload = pool->slots[i].data +
				LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
				LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES;
			lease->capacity = LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_PAYLOAD_BYTES;
			lease->token = &pool->slots[i];
			k_mutex_unlock(&pool->lock);
			return 0;
		}
		k_mutex_unlock(&pool->lock);
	}
}

static int sigrok_linkr_raw_burst_commit_event(
	struct linkr_debugger_sigrok_linkr_runtime *runtime,
	enum linkr_debugger_sigrok_linkr_event_type event_type,
	uint32_t sample_index)
{
	struct linkr_debugger_la_stream_sink_lease lease;
	struct sigrok_linkr_raw_burst_pool *pool;
	struct sigrok_linkr_raw_burst_queue_item *item;
	struct linkr_debugger_sigrok_linkr_header header;
	struct linkr_debugger_sigrok_linkr_event event;
	uint8_t slot_index;
	int ret;

	ret = sigrok_linkr_raw_burst_lease(1U,
		linkr_debugger_sigrok_linkr_bytes_per_sample(
			runtime->session.config.channel_mask), runtime, &lease);
	if (ret < 0) {
		return ret;
	}
	pool = &runtime->raw_burst;
	item = lease.token;
	slot_index = item->raw_burst_slot_index;
	k_mutex_lock(&pool->lock, K_FOREVER);
	if (!pool->active || pool->aborted || runtime->client_fd < 0 ||
	    slot_index >= LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT ||
	    pool->slot_state[slot_index] != SIGROK_LINKR_RAW_BURST_SLOT_LEASED) {
		sigrok_linkr_raw_burst_release_slot_locked(pool, slot_index);
		k_mutex_unlock(&pool->lock);
		return -ECANCELED;
	}
	pool->queued_items++;
	pool->slot_state[slot_index] = SIGROK_LINKR_RAW_BURST_SLOT_QUEUED;
	event.session_id = pool->session_id;
	event.type_detail = (uint8_t)event_type;
	event.sample_index = sample_index;
	k_mutex_unlock(&pool->lock);

	(void)linkr_debugger_sigrok_linkr_encode_event(&event,
		item->data + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES,
		LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES);
	linkr_debugger_sigrok_linkr_init_response_header(&header,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_EVENT, 0U,
		LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES);
	(void)linkr_debugger_sigrok_linkr_encode_header(&header, item->data,
		LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);
	item->len = LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_TERMINAL_FRAME_BYTES;
	k_fifo_put(&runtime->stream_fifo, item);
	atomic_inc(&runtime->stream_qdepth);
	return 0;
}

static int sigrok_linkr_raw_burst_commit(
	const struct linkr_debugger_la_stream_sink_commit *commit, void *user_data)
{
	struct linkr_debugger_sigrok_linkr_runtime *runtime = user_data;
	struct sigrok_linkr_raw_burst_pool *pool;
	struct sigrok_linkr_raw_burst_queue_item *item;
	struct linkr_debugger_sigrok_linkr_header header;
	struct linkr_debugger_sigrok_linkr_data_meta meta;
	uint8_t *encoded_payload;
	const uint8_t *source_payload;
	uint32_t sample_index;
	uint8_t compression;
	uint8_t slot_index;
	uint32_t max_samples;
	size_t payload_len;
	int ret;

	if (runtime == NULL || commit == NULL || commit->token == NULL ||
	    commit->sample_count == 0U ||
	    !sigrok_linkr_stream_sink_shape_matches(&runtime->session, commit->bytes_per_sample) ||
	    commit->payload_len != (size_t)commit->sample_count * commit->bytes_per_sample ||
	    commit->payload_len > LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_PAYLOAD_BYTES) {
		return -EINVAL;
	}
	max_samples = linkr_debugger_sigrok_linkr_packed_burst_max_chunk_samples(
		commit->bytes_per_sample);
	if (max_samples == 0U || commit->sample_count > max_samples) {
		return -EINVAL;
	}
	pool = &runtime->raw_burst;
	item = commit->token;
	slot_index = item->raw_burst_slot_index;
	if (linkr_debugger_sigrok_linkr_raw_burst_should_emit_triggered_event(
	    runtime->session.config.trigger_type, pool->triggered_committed)) {
		ret = sigrok_linkr_raw_burst_commit_event(runtime,
			LINKR_DEBUGGER_SIGROK_LINKR_EVENT_TRIGGERED,
			linkr_debugger_sigrok_linkr_trigger_sample_index(&runtime->session));
		if (ret < 0) {
			return ret;
		}
		k_mutex_lock(&pool->lock, K_FOREVER);
		if (!pool->active || pool->aborted) {
			k_mutex_unlock(&pool->lock);
			return -ECANCELED;
		}
		pool->triggered_committed = true;
		runtime->session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING;
		k_mutex_unlock(&pool->lock);
	}
	k_mutex_lock(&pool->lock, K_FOREVER);
	if (!pool->active || pool->aborted || runtime->client_fd < 0 ||
	    slot_index >= LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT ||
	    pool->slot_state[slot_index] != SIGROK_LINKR_RAW_BURST_SLOT_LEASED) {
		k_mutex_unlock(&pool->lock);
		return -ECANCELED;
	}
	source_payload = item->data + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES;
	encoded_payload = pool->encode_scratch;
	payload_len = linkr_debugger_sigrok_linkr_encode_packed_payload(
		(uint16_t)commit->sample_count, runtime->session.config.channel_mask,
		source_payload, commit->payload_len, &compression,
		encoded_payload, LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_PAYLOAD_BYTES);
	if (payload_len == 0U) {
		k_mutex_unlock(&pool->lock);
		return -EINVAL;
	}
	sample_index = pool->sample_index;
	pool->sample_index = linkr_debugger_sigrok_linkr_advance_sample_index(
		pool->sample_index, commit->sample_count);
	pool->emitted_samples += commit->sample_count;
	pool->queued_items++;
	pool->slot_state[slot_index] = SIGROK_LINKR_RAW_BURST_SLOT_QUEUED;
	k_mutex_unlock(&pool->lock);

	meta.sample_index = sample_index;
	meta.sample_count = (uint16_t)commit->sample_count;
	meta.compression = compression;
	meta.channel_mask = runtime->session.config.channel_mask;
	(void)linkr_debugger_sigrok_linkr_encode_data_meta(&meta,
		item->data + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES,
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES);
	memcpy(item->data + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES,
		encoded_payload, payload_len);
	linkr_debugger_sigrok_linkr_init_response_header(&header,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_DATA, 0U,
		(uint16_t)(LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + payload_len));
	(void)linkr_debugger_sigrok_linkr_encode_header(&header, item->data,
		LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);
	item->len = LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + payload_len;
	k_fifo_put(&runtime->stream_fifo, item);
	atomic_inc(&runtime->stream_qdepth);
	return 0;
}

static void sigrok_linkr_raw_burst_abort_payload(void *token, void *user_data)
{
	struct linkr_debugger_sigrok_linkr_runtime *runtime = user_data;
	struct sigrok_linkr_raw_burst_queue_item *item = token;

	if (runtime == NULL || item == NULL || !item->raw_burst_slot) {
		return;
	}
	k_mutex_lock(&runtime->raw_burst.lock, K_FOREVER);
	sigrok_linkr_raw_burst_release_slot_locked(&runtime->raw_burst,
		item->raw_burst_slot_index);
	k_mutex_unlock(&runtime->raw_burst.lock);
}

static void sigrok_linkr_raw_burst_terminal(enum linkr_debugger_la_ring_poll_result status,
	uint32_t sequence, void *user_data)
{
	struct linkr_debugger_sigrok_linkr_runtime *runtime = user_data;
	struct sigrok_linkr_raw_burst_pool *pool;
	struct sigrok_linkr_raw_burst_queue_item *item;
	struct linkr_debugger_sigrok_linkr_header header;
	struct linkr_debugger_sigrok_linkr_event event;
	uint8_t slot_index;

	ARG_UNUSED(sequence);
	if (runtime == NULL) {
		return;
	}
	pool = &runtime->raw_burst;
	if (sigrok_linkr_raw_burst_lease(1U,
	    linkr_debugger_sigrok_linkr_bytes_per_sample(runtime->session.config.channel_mask), runtime,
	    &(struct linkr_debugger_la_stream_sink_lease){0}) < 0) {
		sigrok_linkr_stream_mark_stop(runtime);
		return;
	}
	/* Re-open the leased terminal slot so we can encode an EVENT instead of DATA. */
	k_mutex_lock(&pool->lock, K_FOREVER);
	item = NULL;
	slot_index = 0U;
	for (uint32_t i = 0U; i < LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT; i++) {
		if (pool->slot_state[i] == SIGROK_LINKR_RAW_BURST_SLOT_LEASED) {
			item = &pool->slots[i];
			slot_index = (uint8_t)i;
			break;
		}
	}
	if (item == NULL || !pool->active || pool->aborted || pool->terminal_committed) {
		if (item != NULL) {
			sigrok_linkr_raw_burst_release_slot_locked(pool, slot_index);
		}
		k_mutex_unlock(&pool->lock);
		sigrok_linkr_stream_mark_stop(runtime);
		return;
	}
	pool->terminal_committed = true;
	pool->queued_items++;
	pool->slot_state[slot_index] = SIGROK_LINKR_RAW_BURST_SLOT_QUEUED;
	event.session_id = pool->session_id;
	event.type_detail = status == LINKR_DEBUGGER_LA_RING_POLL_OK ?
		(uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_STOPPED :
		(uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_OVERRUN;
	event.sample_index = pool->sample_index;
	pool->active = false;
	k_mutex_unlock(&pool->lock);

	(void)linkr_debugger_sigrok_linkr_encode_event(&event,
		item->data + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES,
		LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES);
	linkr_debugger_sigrok_linkr_init_response_header(&header,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_EVENT, 0U,
		LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES);
	(void)linkr_debugger_sigrok_linkr_encode_header(&header, item->data,
		LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);
	item->len = LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_TERMINAL_FRAME_BYTES;
	k_fifo_put(&runtime->stream_fifo, item);
	atomic_inc(&runtime->stream_qdepth);
	runtime->session.sample_index = event.sample_index;
	runtime->session.emitted_samples = pool->emitted_samples;
	runtime->session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
	sigrok_linkr_stream_mark_stop(runtime);
}

static struct linkr_debugger_la_stream_sink sigrok_linkr_raw_burst_sink(
	struct linkr_debugger_sigrok_linkr_runtime *runtime)
{
	struct linkr_debugger_la_stream_sink sink = {
		.format = LINKR_DEBUGGER_LA_STREAM_PAYLOAD_PACKED_LE_BYTES,
		.bytes_per_sample = linkr_debugger_sigrok_linkr_bytes_per_sample(
			runtime->session.config.channel_mask),
		.max_chunk_samples = linkr_debugger_sigrok_linkr_packed_burst_max_chunk_samples(
			linkr_debugger_sigrok_linkr_bytes_per_sample(
				runtime->session.config.channel_mask)),
		.lease = sigrok_linkr_raw_burst_lease,
		.commit = sigrok_linkr_raw_burst_commit,
		.abort = sigrok_linkr_raw_burst_abort_payload,
		.terminal = sigrok_linkr_raw_burst_terminal,
		.user_data = runtime,
	};

	return sink;
}

static bool sigrok_linkr_stream_sink_shape_matches(
	const struct linkr_debugger_sigrok_linkr_session *session,
	uint8_t bytes_per_sample)
{
	return session != NULL && bytes_per_sample > 0U &&
		linkr_debugger_sigrok_linkr_bytes_per_sample(session->config.channel_mask) ==
		bytes_per_sample;
}

static bool sigrok_linkr_stream_sink_final_chunk(
	const struct linkr_debugger_sigrok_linkr_session *session,
	uint32_t sample_count)
{
	uint32_t target_samples = linkr_debugger_sigrok_linkr_bounded_sample_target(session);

	return target_samples > 0U && session->emitted_samples < target_samples &&
		sample_count >= target_samples - session->emitted_samples;
}

static int sigrok_linkr_stream_sink_lease(uint32_t sample_count,
	uint8_t bytes_per_sample, void *user_data,
	struct linkr_debugger_la_stream_sink_lease *lease)
{
	struct linkr_debugger_sigrok_linkr_runtime *runtime = user_data;
	struct linkr_debugger_sigrok_linkr_session *session;
	struct sigrok_linkr_stream_queue_item *item;
	uint16_t send_count;
	bool final_chunk;
	size_t payload_len;
	size_t frame_len;

	if (runtime == NULL || lease == NULL || sample_count == 0U ||
	    sample_count > UINT16_MAX || bytes_per_sample == 0U) {
		return -EINVAL;
	}
	memset(lease, 0, sizeof(*lease));
	if (runtime->client_fd < 0) {
		return -ENOTCONN;
	}

	session = &runtime->session;
	if (!sigrok_linkr_stream_sink_shape_matches(session, bytes_per_sample)) {
		return -EINVAL;
	}
	if (session->state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED) {
		session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING;
		if (sigrok_linkr_stream_enqueue_event(runtime,
		    LINKR_DEBUGGER_SIGROK_LINKR_EVENT_TRIGGERED,
		    session->active_session_id,
		    linkr_debugger_sigrok_linkr_trigger_sample_index(session)) < 0) {
			return -ENOSPC;
		}
	}
	if (session->state != LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING) {
		return -ECANCELED;
	}

	send_count = linkr_debugger_sigrok_linkr_bounded_chunk_count(session,
		sample_count);
	if (send_count == 0U || send_count != sample_count) {
		return -EINVAL;
	}
	final_chunk = sigrok_linkr_stream_sink_final_chunk(session, sample_count);
	if (!linkr_debugger_sigrok_linkr_stream_queue_has_capacity(
	    (uint32_t)atomic_get(&runtime->stream_qdepth), final_chunk)) {
		atomic_inc(&runtime->stream_dropped);
		return -ENOSPC;
	}

	payload_len = (size_t)sample_count * bytes_per_sample;
	frame_len = LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + payload_len;
	if (payload_len == 0U || payload_len > LINKR_DEBUGGER_SIGROK_LINKR_MAX_DATA_BYTES) {
		return -EINVAL;
	}
	item = k_malloc(sizeof(*item) + frame_len);
	if (item == NULL) {
		atomic_inc(&runtime->stream_dropped);
		return -ENOMEM;
	}
	item->len = 0U;
	item->raw_burst_slot = false;
	item->raw_burst_slot_index = 0U;
	lease->payload = item->data + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES;
	lease->capacity = payload_len;
	lease->token = item;
	return 0;
}

static int sigrok_linkr_stream_sink_commit(
	const struct linkr_debugger_la_stream_sink_commit *commit, void *user_data)
{
	struct linkr_debugger_sigrok_linkr_runtime *runtime = user_data;
	struct linkr_debugger_sigrok_linkr_session *session;
	struct sigrok_linkr_stream_queue_item *item;
	uint16_t channel_mask;
	size_t frame_capacity;
	size_t frame_len;

	if (runtime == NULL || commit == NULL || commit->token == NULL ||
	    commit->sample_count == 0U || commit->sample_count > UINT16_MAX ||
	    commit->bytes_per_sample == 0U || commit->payload_len == 0U) {
		return -EINVAL;
	}
	if (runtime->client_fd < 0) {
		return -ENOTCONN;
	}

	session = &runtime->session;
	channel_mask = session->config.channel_mask;
	item = commit->token;
	frame_capacity = LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + commit->payload_len;
	if (session->state != LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING ||
	    !sigrok_linkr_stream_sink_shape_matches(session, commit->bytes_per_sample) ||
	    commit->payload_len != (size_t)commit->sample_count * commit->bytes_per_sample ||
	    commit->payload_len > LINKR_DEBUGGER_SIGROK_LINKR_MAX_DATA_BYTES) {
		return -EINVAL;
	}
	frame_len = linkr_debugger_sigrok_linkr_encode_packed_data_frame(
		session->sample_index, (uint16_t)commit->sample_count, channel_mask,
		item->data + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES, commit->payload_len, true,
		item->data, frame_capacity);
	if (frame_len == 0U) {
		return -EINVAL;
	}

	item->len = frame_len;
	k_fifo_put(&runtime->stream_fifo, item);
	atomic_inc(&runtime->stream_qdepth);
	session->emitted_samples += commit->sample_count;
	session->sample_index = linkr_debugger_sigrok_linkr_advance_sample_index(
		session->sample_index, commit->sample_count);
	return 0;
}

static void sigrok_linkr_stream_sink_abort(void *token, void *user_data)
{
	ARG_UNUSED(user_data);

	k_free(token);
}

static void sigrok_linkr_stream_sink_terminal(
	enum linkr_debugger_la_ring_poll_result status, uint32_t sequence, void *user_data)
{
	struct linkr_debugger_sigrok_linkr_runtime *runtime = user_data;
	struct linkr_debugger_sigrok_linkr_session *session;
	enum linkr_debugger_sigrok_linkr_event_type event_type;

	ARG_UNUSED(sequence);
	if (runtime == NULL) {
		return;
	}

	session = &runtime->session;
	event_type = status == LINKR_DEBUGGER_LA_RING_POLL_OK ?
		LINKR_DEBUGGER_SIGROK_LINKR_EVENT_STOPPED :
		LINKR_DEBUGGER_SIGROK_LINKR_EVENT_OVERRUN;
	if (sigrok_linkr_stream_enqueue_event(runtime, event_type,
	    session->active_session_id, session->sample_index) < 0) {
		atomic_inc(&runtime->stream_dropped);
	}
	session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
	sigrok_linkr_stream_mark_stop(runtime);
}

static struct linkr_debugger_la_stream_sink sigrok_linkr_stream_sink(
	struct linkr_debugger_sigrok_linkr_runtime *runtime)
{
	struct linkr_debugger_la_stream_sink sink = {
		.format = LINKR_DEBUGGER_LA_STREAM_PAYLOAD_PACKED_LE_BYTES,
		.bytes_per_sample = linkr_debugger_sigrok_linkr_bytes_per_sample(
			runtime->session.config.channel_mask),
		.max_chunk_samples = LINKR_DEBUGGER_LA_STREAM_MAX_SINK_CHUNK_SAMPLES,
		.lease = sigrok_linkr_stream_sink_lease,
		.commit = sigrok_linkr_stream_sink_commit,
		.abort = sigrok_linkr_stream_sink_abort,
		.terminal = sigrok_linkr_stream_sink_terminal,
		.user_data = runtime,
	};

	return sink;
}

static void sigrok_linkr_stream_reset_queue(
	struct linkr_debugger_sigrok_linkr_runtime *runtime)
{
	struct sigrok_linkr_stream_queue_item *item;

	sigrok_linkr_raw_burst_abort(runtime);

	while ((item = k_fifo_get(&runtime->stream_fifo, K_NO_WAIT)) != NULL) {
		sigrok_linkr_stream_free_item(runtime, item);
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
	item->raw_burst_slot = false;
	item->raw_burst_slot_index = 0U;
	k_fifo_put(&runtime->stream_fifo, item);
	atomic_inc(&runtime->stream_qdepth);
	return 0;
}

static void sigrok_linkr_cleanup_capture(
	struct linkr_debugger_sigrok_linkr_runtime *runtime)
{
	struct linkr_debugger_sigrok_linkr_session *session;

	if (runtime == NULL) {
		return;
	}

	session = &runtime->session;
	sigrok_linkr_raw_burst_abort(runtime);
	if (session->telemetry_pause_held) {
		linkr_debugger_ws_sigrok_telemetry_pause_release();
		session->telemetry_pause_held = false;
	}
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
		sigrok_linkr_stream_free_item(runtime, item);
		if (ret < 0) {
			atomic_inc(&runtime->stream_dropped);
			sigrok_linkr_raw_burst_abort(runtime);
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
		if (session->telemetry_pause_held) {
			linkr_debugger_ws_sigrok_telemetry_pause_release();
			session->telemetry_pause_held = false;
		}
		return;
	}

	(void)linkr_debugger_logic_analyzer_stop_stream();
	(void)linkr_debugger_capture_arbiter_release(
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR);
	session->capture_owner_held = false;
	if (session->telemetry_pause_held) {
		linkr_debugger_ws_sigrok_telemetry_pause_release();
		session->telemetry_pause_held = false;
	}
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

static uint32_t linkr_debugger_sigrok_linkr_next_prepare_generation;

static uint32_t linkr_debugger_sigrok_linkr_alloc_prepare_generation(void)
{
	linkr_debugger_sigrok_linkr_next_prepare_generation++;
	if (linkr_debugger_sigrok_linkr_next_prepare_generation == 0U) {
		linkr_debugger_sigrok_linkr_next_prepare_generation++;
	}

	return linkr_debugger_sigrok_linkr_next_prepare_generation;
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

size_t linkr_debugger_sigrok_linkr_raw_burst_queue_memory_bytes(uint32_t slot_count,
	size_t slot_frame_bytes)
{
	return (size_t)slot_count * slot_frame_bytes;
}

bool linkr_debugger_sigrok_linkr_raw_burst_queue_has_space(uint32_t queued_items,
	uint32_t slot_count)
{
	return slot_count > 0U && queued_items < slot_count;
}

uint16_t linkr_debugger_sigrok_linkr_raw_burst_frame_sample_count(uint32_t emitted,
	uint32_t total_samples, uint32_t max_samples_per_item)
{
	uint32_t remaining;

	if (max_samples_per_item == 0U || emitted >= total_samples) {
		return 0U;
	}
	remaining = total_samples - emitted;
	return (uint16_t)(remaining > max_samples_per_item ? max_samples_per_item : remaining);
}

bool linkr_debugger_sigrok_linkr_raw_burst_should_emit_triggered_event(
	uint8_t trigger_type, bool triggered_committed)
{
	return trigger_type != LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE && !triggered_committed;
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
	uint32_t target_samples;

	if (session == NULL || offered_count == 0U) {
		return 0U;
	}
	target_samples = linkr_debugger_sigrok_linkr_bounded_sample_target(session);
	if (target_samples == 0U) {
		return (uint16_t)(offered_count > UINT16_MAX ? UINT16_MAX : offered_count);
	}
	if (session->emitted_samples >= target_samples) {
		return 0U;
	}

	remaining = target_samples - session->emitted_samples;
	return (uint16_t)(offered_count > remaining ? remaining : offered_count);
}

uint32_t linkr_debugger_sigrok_linkr_bounded_sample_target(
	const struct linkr_debugger_sigrok_linkr_session *session)
{
	uint32_t target_samples;

	if (session == NULL || session->config.post_samples == 0U ||
	    linkr_debugger_logic_analyzer_bounded_sample_target(session->config.pre_samples,
		session->config.post_samples, &target_samples) < 0) {
		return 0U;
	}

	return target_samples;
}

uint32_t linkr_debugger_sigrok_linkr_trigger_sample_index(
	const struct linkr_debugger_sigrok_linkr_session *session)
{
	return session != NULL ? session->config.pre_samples : 0U;
}

bool linkr_debugger_sigrok_linkr_bounded_capture_done(
	const struct linkr_debugger_sigrok_linkr_session *session)
{
	uint32_t target_samples = linkr_debugger_sigrok_linkr_bounded_sample_target(session);

	return target_samples > 0U && session->emitted_samples >= target_samples;
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

static size_t linkr_debugger_sigrok_linkr_encode_packed_payload(
	uint16_t sample_count,
	uint16_t channel_mask,
	const uint8_t *packed,
	size_t packed_len,
	uint8_t *compression,
	uint8_t *out,
	size_t out_len)
{
	uint8_t bytes_per_sample = linkr_debugger_sigrok_linkr_bytes_per_sample(channel_mask);
	size_t expected_len = (size_t)sample_count * bytes_per_sample;
	size_t payload_len;

	if (packed == NULL || compression == NULL || out == NULL || sample_count == 0U ||
	    bytes_per_sample == 0U || packed_len != expected_len) {
		return 0U;
	}

	payload_len = linkr_debugger_sigrok_linkr_compress_rle_if_smaller(packed,
		sample_count, bytes_per_sample, out, out_len);
	if (payload_len > 0U) {
		*compression = LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK_RLE;
		return payload_len;
	}
	if (expected_len > out_len) {
		return 0U;
	}

	memcpy(out, packed, expected_len);
	*compression = LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK;
	return expected_len;
}

size_t linkr_debugger_sigrok_linkr_encode_packed_data_frame(
	uint32_t sample_index,
	uint16_t sample_count,
	uint16_t channel_mask,
	const uint8_t *packed,
	size_t packed_len,
	bool try_bit_pack_rle,
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

	if (try_bit_pack_rle) {
		payload_len = linkr_debugger_sigrok_linkr_encode_packed_payload(sample_count,
			channel_mask, packed, packed_len, &compression, out + prefix_len,
			out_len - prefix_len);
		if (payload_len == 0U) {
			return 0U;
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
		if (session->telemetry_pause_held) {
			linkr_debugger_ws_sigrok_telemetry_pause_release();
			session->telemetry_pause_held = false;
		}
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
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_EITHER |
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_PRE_TRIGGER;
	caps->modes[0].channel_count = 8U;
	caps->modes[0].sample_bytes = 1U;
	caps->modes[0].max_samplerate_khz = 125000U;
	caps->modes[0].compression = LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_BIT_PACK |
		LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_RLE;

	caps->modes[1].mode_id = LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11;
	caps->modes[1].mode_flags = LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_CONTINUOUS |
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_NONE |
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_RISING |
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_FALLING |
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_TRIGGER_EITHER |
		LINKR_DEBUGGER_SIGROK_LINKR_MODE_FLAG_PRE_TRIGGER;
	caps->modes[1].channel_count = 11U;
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
	case LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_V2_REQ:
		if (request->header.payload_len != LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES) {
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
	if (payload == NULL || config == NULL ||
	    (payload_len != LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES &&
	     payload_len != LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_V2_BYTES)) {
		return -EINVAL;
	}

	config->mode_id = payload[0];
	config->trigger_type = payload[1];
	config->trigger_channel = payload[2];
	config->channel_mask = load_le16(&payload[3]);
	config->samplerate_khz = load_le24(&payload[5]);
	if (payload_len == LINKR_DEBUGGER_SIGROK_LINKR_CONFIG_BYTES) {
		config->pre_samples = load_le16(&payload[8]);
		config->post_samples = load_le16(&payload[10]);
	} else {
		config->pre_samples = load_le32(&payload[8]);
		config->post_samples = load_le32(&payload[12]);
	}
	return 0;
}

static int linkr_debugger_sigrok_linkr_map_la_config(
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
	case LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11:
		logical_channels = 11U;
		mask_limit = 0x07ffU;
		break;
	default:
		return -EINVAL;
	}
	if (config->channel_mask == 0U ||
	    (config->channel_mask & (uint16_t)~mask_limit) != 0U ||
	    config->samplerate_khz > UINT32_MAX / 1000U ||
	    config->trigger_type > LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_EITHER ||
	    (armed && config->trigger_type != LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE &&
	     (config->trigger_channel >= logical_channels ||
	      (config->channel_mask & (uint16_t)(1U << config->trigger_channel)) == 0U))) {
		return -EINVAL;
	}

	memset(la_config, 0, sizeof(*la_config));
	la_config->sample_rate_hz = config->samplerate_khz * 1000U;
	la_config->trigger = armed ?
		(enum linkr_debugger_la_trigger_type)config->trigger_type :
		LINKR_DEBUGGER_LA_TRIGGER_NONE;
	la_config->pre_samples = config->pre_samples;
	la_config->post_samples = config->post_samples;
	la_config->pin_base = 10U;
	la_config->pin_count = logical_channels;

	for (uint8_t index = 0U; index < logical_channels; index++) {
		if ((config->channel_mask & (1U << index)) == 0U) {
			continue;
		}
		if (armed && config->trigger_type != LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE &&
		    index == config->trigger_channel) {
			la_config->trigger_pin = selected;
		}
		la_config->selected_pins[selected++] = (uint8_t)(10U + index);
	}
	la_config->selected_pin_count = selected;
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
	case LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11:
		mask_limit = 0x07ffU;
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
		struct linkr_debugger_la_config la_config;

		if (linkr_debugger_sigrok_linkr_map_la_config(config, true, &la_config) < 0 ||
		    !linkr_debugger_logic_analyzer_pre_trigger_plan_supported(&la_config)) {
			if (error_code != NULL) {
				*error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG;
			}
			if (detail != NULL) {
				*detail = config->pre_samples > UINT16_MAX ? UINT16_MAX :
					(uint16_t)config->pre_samples;
			}
			return -EINVAL;
		}
	}

	return 0;
}

bool linkr_debugger_sigrok_linkr_config_is_wide11_exact_burst(
	const struct linkr_debugger_sigrok_linkr_config *config)
{
	return linkr_debugger_sigrok_linkr_config_is_legacy_wide11_exact_burst(config);
}

int linkr_debugger_sigrok_linkr_packed_burst_plan(
	const struct linkr_debugger_sigrok_linkr_config *config,
	struct linkr_debugger_la_packed_burst_plan *plan)
{
	struct linkr_debugger_la_config la_config;

	if (linkr_debugger_sigrok_linkr_to_la_config(config,
	    config != NULL && config->trigger_type != LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE,
	    &la_config) < 0) {
		return -EINVAL;
	}
	return linkr_debugger_logic_analyzer_packed_burst_plan(&la_config, plan);
}

bool linkr_debugger_sigrok_linkr_config_is_packed_burst(
	const struct linkr_debugger_sigrok_linkr_config *config)
{
	struct linkr_debugger_la_packed_burst_plan plan;

	return linkr_debugger_sigrok_linkr_packed_burst_plan(config, &plan) == 0;
}

static bool linkr_debugger_sigrok_linkr_config_is_legacy_wide11_exact_burst(
	const struct linkr_debugger_sigrok_linkr_config *config)
{
	return config != NULL &&
		config->mode_id == LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11 &&
		config->trigger_type <= LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_EITHER &&
		(config->trigger_type == LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE ||
		 (config->trigger_channel < 11U &&
		  (config->channel_mask & (uint16_t)(1U << config->trigger_channel)) != 0U)) &&
		config->channel_mask == 0x07ffU &&
		config->samplerate_khz ==
			LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_RATE_HZ / 1000U &&
		config->pre_samples == 0U &&
		config->post_samples == LINKR_DEBUGGER_LA_WIDE11_BURST_TARGET_SAMPLES;
}

int linkr_debugger_sigrok_linkr_to_la_config(
	const struct linkr_debugger_sigrok_linkr_config *config,
	bool armed,
	struct linkr_debugger_la_config *la_config)
{
	if (config == NULL || config->post_samples > LINKR_DEBUGGER_LA_PACKED_BURST_MAX_SAMPLES ||
	    linkr_debugger_sigrok_linkr_map_la_config(config, armed, la_config) < 0 ||
	    (config->pre_samples > 0U &&
	     (!armed || !linkr_debugger_logic_analyzer_pre_trigger_plan_supported(la_config)))) {
		return -EINVAL;
	}

	return 0;
}

void linkr_debugger_sigrok_linkr_start_prepare_reset(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare)
{
	if (prepare != NULL) {
		memset(prepare, 0, sizeof(*prepare));
	}
}

static void linkr_debugger_sigrok_linkr_start_prepare_cleanup(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare)
{
	if (prepare == NULL) {
		return;
	}
	if (prepare->ws_burst_pool_held) {
		linkr_debugger_ws_sigrok_burst_pool_abort(prepare->ws_session_id,
			prepare->ws_stream_generation);
		prepare->ws_burst_pool_held = false;
	}
	if (prepare->la_prepare_held) {
		(void)linkr_debugger_logic_analyzer_start_prepare_cancel(&prepare->la_prepare);
		prepare->la_prepare_held = false;
		if (prepare->la_prepare.arena_held) {
			prepare->arena_held = false;
		}
	}
	if (prepare->arena_held) {
		(void)linkr_debugger_capture_arena_release(&prepare->arena_lease);
		prepare->arena_held = false;
	}
	if (prepare->capture_owner_held) {
		(void)linkr_debugger_capture_arbiter_release(
			LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR);
		prepare->capture_owner_held = false;
	}
}

int linkr_debugger_sigrok_linkr_start_prepare_capture(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare,
	struct linkr_debugger_sigrok_linkr_session *session,
	bool ws_transport,
	uint32_t ws_session_id,
	uint32_t ws_stream_generation,
	uint32_t source_generation,
	const struct linkr_debugger_la_stream_sink *sink)
{
	struct linkr_debugger_la_config la_config;
	struct linkr_debugger_la_hardware_plan plan;
	bool config_v2;
	int ret;

	if (prepare == NULL || session == NULL || sink == NULL) {
		return -EINVAL;
	}
	if (prepare->state != LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_IDLE) {
		return -EBUSY;
	}
	if (ws_transport && (ws_session_id == 0U || ws_stream_generation == 0U)) {
		return -EINVAL;
	}
	ret = linkr_debugger_sigrok_linkr_to_la_config(&session->config,
		session->config.trigger_type != LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE,
		&la_config);
	if (ret < 0) {
		return ret;
	}
	config_v2 = session->config.post_samples > UINT16_MAX;
	ret = linkr_debugger_logic_analyzer_select_hardware_plan(&la_config, config_v2,
		&plan);
	if (ret < 0) {
		return ret;
	}
	if (!plan.supported || plan.pipeline_family !=
	    LINKR_DEBUGGER_LA_PIPELINE_FAMILY_COMMON_PACKED) {
		return -EINVAL;
	}
	if (!linkr_debugger_capture_arbiter_try_acquire(
	    LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR)) {
		return -EBUSY;
	}

	prepare->capture_owner_held = true;
	prepare->generation = linkr_debugger_sigrok_linkr_alloc_prepare_generation();
	prepare->sigrok_session_id = session->active_session_id;
	prepare->ws_session_id = ws_session_id;
	prepare->ws_stream_generation = ws_stream_generation;
	prepare->source_generation = source_generation;
	if (plan.legacy_adapter == LINKR_DEBUGGER_LA_LEGACY_ADAPTER_PACKED_BURST) {
		ret = linkr_debugger_capture_arena_try_acquire_wide11_quiesced(
			ws_transport ? ws_session_id : session->active_session_id,
			LINKR_DEBUGGER_SIGROK_LINKR_WIDE11_ARENA_QUIESCE_TIMEOUT_MS,
			&prepare->arena_lease);
		if (ret < 0) {
			goto fail;
		}
		prepare->arena_held = true;
		ret = linkr_debugger_capture_arena_mark_armed(&prepare->arena_lease);
		if (ret < 0) {
			goto fail;
		}
		if (ws_transport) {
			ret = linkr_debugger_ws_sigrok_burst_pool_begin(ws_session_id,
				ws_stream_generation, source_generation);
			if (ret < 0) {
				goto fail;
			}
			prepare->ws_burst_pool_held = true;
		}

		ret = linkr_debugger_logic_analyzer_prepare_wide11_burst_start_sink(
			&la_config, &prepare->arena_lease, sink, &prepare->la_prepare);
	} else {
		ret = linkr_debugger_logic_analyzer_prepare_stream_start_sink(&la_config,
			config_v2, sink, &prepare->la_prepare);
	}
	if (ret < 0) {
		goto fail;
	}
	prepare->la_prepare_held = true;

	prepare->state = LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_PREPARED;
	session->capture_owner_held = true;
	if (!session->telemetry_pause_held) {
		linkr_debugger_ws_sigrok_telemetry_pause_acquire();
		session->telemetry_pause_held = true;
	}
	return 0;

fail:
	linkr_debugger_sigrok_linkr_start_prepare_cleanup(prepare);
	prepare->state = LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_CANCELLED;
	session->capture_owner_held = false;
	if (session->telemetry_pause_held) {
		linkr_debugger_ws_sigrok_telemetry_pause_release();
		session->telemetry_pause_held = false;
	}
	return ret;
}

int linkr_debugger_sigrok_linkr_start_prepare_exact_burst(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare,
	struct linkr_debugger_sigrok_linkr_session *session,
	bool ws_transport,
	uint32_t ws_session_id,
	uint32_t ws_stream_generation,
	uint32_t source_generation,
	const struct linkr_debugger_la_stream_sink *sink)
{
	if (session == NULL ||
	    !linkr_debugger_sigrok_linkr_config_is_packed_burst(&session->config)) {
		return -EINVAL;
	}

	return linkr_debugger_sigrok_linkr_start_prepare_capture(prepare, session,
		ws_transport, ws_session_id, ws_stream_generation, source_generation, sink);
}

void linkr_debugger_sigrok_linkr_start_prepare_cancel(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare,
	struct linkr_debugger_sigrok_linkr_session *session)
{
	if (prepare == NULL || prepare->state ==
	    LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_IDLE ||
	    prepare->state == LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_CANCELLED ||
	    prepare->state == LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_DONE) {
		return;
	}
	linkr_debugger_sigrok_linkr_start_prepare_cleanup(prepare);
	prepare->state = LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_CANCELLED;
	if (session != NULL) {
		session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
		session->capture_owner_held = false;
		if (session->telemetry_pause_held) {
			linkr_debugger_ws_sigrok_telemetry_pause_release();
			session->telemetry_pause_held = false;
		}
	}
}

int linkr_debugger_sigrok_linkr_start_prepare_mark_response_sent(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare)
{
	int ret;

	if (prepare == NULL || prepare->state !=
	    LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_PREPARED ||
	    !prepare->la_prepare_held || prepare->response_sent) {
		return -EINVAL;
	}
	ret = linkr_debugger_logic_analyzer_start_prepare_mark_response_sent(
		&prepare->la_prepare);
	if (ret == 0) {
		prepare->response_sent = true;
	}
	return ret;
}

int linkr_debugger_sigrok_linkr_start_prepare_mark_armed_event_sent(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare)
{
	int ret;

	if (prepare == NULL || prepare->state !=
	    LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_PREPARED ||
	    !prepare->la_prepare_held || !prepare->response_sent ||
	    prepare->armed_event_sent) {
		return -EINVAL;
	}
	ret = linkr_debugger_logic_analyzer_start_prepare_mark_armed_event_sent(
		&prepare->la_prepare);
	if (ret == 0) {
		prepare->armed_event_sent = true;
	}
	return ret;
}

int linkr_debugger_sigrok_linkr_start_prepare_go(
	struct linkr_debugger_sigrok_linkr_start_prepare *prepare,
	struct linkr_debugger_sigrok_linkr_session *session)
{
	if (prepare == NULL || session == NULL ||
	    prepare->state != LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_PREPARED ||
	    prepare->sigrok_session_id != session->active_session_id ||
	    !prepare->la_prepare_held || !prepare->response_sent) {
		return -ESTALE;
	}
	if (session->config.trigger_type != LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE &&
	    !prepare->armed_event_sent) {
		return -EPROTO;
	}

	prepare->state = LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_GOING;
	int ret = linkr_debugger_logic_analyzer_start_prepare_go(&prepare->la_prepare);
	prepare->la_prepare_held = false;
	if (ret == 0) {
		prepare->arena_held = false;
		prepare->ws_burst_pool_held = false;
		prepare->capture_owner_held = false;
	} else {
		linkr_debugger_sigrok_linkr_start_prepare_cleanup(prepare);
		session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
		session->capture_owner_held = false;
		if (session->telemetry_pause_held) {
			linkr_debugger_ws_sigrok_telemetry_pause_release();
			session->telemetry_pause_held = false;
		}
	}
	prepare->state = LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_DONE;
	return ret;
}

void linkr_debugger_sigrok_linkr_start_sequence_model_init(
	struct linkr_debugger_sigrok_linkr_start_sequence_model *model,
	uint32_t generation)
{
	if (model != NULL) {
		memset(model, 0, sizeof(*model));
		model->generation = generation;
	}
}

int linkr_debugger_sigrok_linkr_start_sequence_model_record(
	struct linkr_debugger_sigrok_linkr_start_sequence_model *model,
	enum linkr_debugger_sigrok_linkr_start_sequence_step step,
	uint32_t generation)
{
	if (model == NULL || generation == 0U || generation != model->generation ||
	    model->step_count >= sizeof(model->steps) / sizeof(model->steps[0])) {
		return -ESTALE;
	}
	switch (step) {
	case LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_PREPARE:
		if (model->prepared || model->response_sent || model->go_called) {
			return -EINVAL;
		}
		model->prepared = true;
		break;
	case LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_START_RESP:
		if (!model->prepared || model->response_sent || model->go_called) {
			return -EINVAL;
		}
		model->response_sent = true;
		break;
	case LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_ARMED_EVENT:
		if (!model->response_sent || model->go_called) {
			return -EINVAL;
		}
		break;
	case LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_GO:
		if (!model->response_sent || model->go_called) {
			return -EINVAL;
		}
		model->go_called = true;
		break;
	case LINKR_DEBUGGER_SIGROK_LINKR_START_SEQUENCE_ERROR:
		if (!model->go_called) {
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
	model->steps[model->step_count++] = step;
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
		hello.server_flags = LINKR_DEBUGGER_SIGROK_LINKR_SERVER_FLAGS_CURRENT;
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
	case LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_REQ:
	case LINKR_DEBUGGER_SIGROK_LINKR_FRAME_CONFIG_V2_REQ: {
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
		bool legacy_wide11_exact = linkr_debugger_sigrok_linkr_config_is_legacy_wide11_exact_burst(
			&session->config);

		if (session->config.mode_id == LINKR_DEBUGGER_SIGROK_LINKR_MODE_WIDE11 &&
		    session->config.samplerate_khz == LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ / 1000U) {
			build_error_response(request, response_header, payload_out,
				payload_out_len, LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG,
				UINT16_MAX, payload_len_out);
			return 0;
		}
		if (session->config.post_samples > UINT16_MAX &&
		    !linkr_debugger_sigrok_linkr_config_is_packed_burst(&session->config)) {
			build_error_response(request, response_header, payload_out,
				payload_out_len, LINKR_DEBUGGER_SIGROK_LINKR_ERROR_INVALID_CONFIG,
				UINT16_MAX, payload_len_out);
			return 0;
		}
		if (session->config.trigger_type == LINKR_DEBUGGER_SIGROK_LINKR_TRIGGER_NONE) {
			session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING;
			if (action != NULL) {
				action->capture_action =
					LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_IMMEDIATE;
			action->has_event = !legacy_wide11_exact;
				action->event.session_id = session->active_session_id;
				action->event.type_detail = (uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_RUNNING;
			}
		} else {
			session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED;
			if (action != NULL) {
				action->capture_action =
					LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_ARMED;
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
		int closing_fd = *fd;
		struct net_linger abortive_linger = {
			.l_onoff = 1,
			.l_linger = 0,
		};

		*fd = -1;
		(void)zsock_setsockopt(closing_fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_LINGER,
			&abortive_linger, sizeof(abortive_linger));
		(void)zsock_shutdown(closing_fd, ZSOCK_SHUT_RDWR);
		(void)zsock_close(closing_fd);
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

static int send_terminal_error_event(int fd,
	const struct linkr_debugger_sigrok_linkr_session *session)
{
	struct linkr_debugger_sigrok_linkr_header event_header;
	uint8_t event_buf[LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES];
	struct linkr_debugger_sigrok_linkr_event event = {
		.session_id = session->active_session_id,
		.type_detail = (uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_ERROR,
		.sample_index = session->sample_index,
	};
	size_t event_len;

	event_len = linkr_debugger_sigrok_linkr_encode_event(&event,
		event_buf, sizeof(event_buf));
	linkr_debugger_sigrok_linkr_init_response_header(&event_header,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_EVENT, 0U,
		(uint16_t)event_len);
	return send_frame(fd, &event_header, event_buf, event_len);
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
		struct linkr_debugger_sigrok_linkr_start_prepare prepare;

		linkr_debugger_sigrok_linkr_start_prepare_reset(&prepare);
		if (action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_IMMEDIATE ||
		    action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_ARMED) {
			bool packed_burst = linkr_debugger_sigrok_linkr_config_is_packed_burst(
				&runtime->session.config);
			struct linkr_debugger_la_stream_sink sink = packed_burst ?
				sigrok_linkr_raw_burst_sink(runtime) :
				sigrok_linkr_stream_sink(runtime);

			sigrok_linkr_stream_reset_queue(runtime);
			runtime->session.sample_index = 0U;
			runtime->session.emitted_samples = 0U;
			if (packed_burst) {
				ret = sigrok_linkr_raw_burst_begin(runtime);
			} else {
				ret = 0;
			}
			if (ret < 0) {
				enum linkr_debugger_sigrok_linkr_error_code error_code =
					linkr_debugger_sigrok_linkr_start_error_code(ret, false);

				linkr_debugger_sigrok_linkr_rollback_start_failure(
					&runtime->session, &action);
				linkr_debugger_sigrok_linkr_build_error_response(&request,
					&response_header, runtime->tx_control_payload,
					sizeof(runtime->tx_control_payload), error_code,
					(uint16_t)(-ret), &tx_payload_len);
				goto send_response;
			}
			ret = linkr_debugger_sigrok_linkr_start_prepare_capture(&prepare,
				&runtime->session, false, 0U, 0U, 0U, &sink);
			if (ret < 0) {
				enum linkr_debugger_sigrok_linkr_error_code error_code =
					linkr_debugger_sigrok_linkr_start_error_code(ret, false);

				if (packed_burst) {
					sigrok_linkr_raw_burst_abort(runtime);
				}
				linkr_debugger_sigrok_linkr_rollback_start_failure(
					&runtime->session, &action);
				linkr_debugger_sigrok_linkr_build_error_response(&request,
					&response_header, runtime->tx_control_payload,
					sizeof(runtime->tx_control_payload), error_code,
					(uint16_t)(-ret), &tx_payload_len);
			}
		}
send_response:
		ret = send_frame(fd, &response_header, runtime->tx_control_payload, tx_payload_len);
		if (ret < 0) {
			linkr_debugger_sigrok_linkr_start_prepare_cancel(&prepare,
				&runtime->session);
			sigrok_linkr_cleanup_capture(runtime);
			return;
		}
		if (prepare.state == LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_PREPARED) {
			ret = linkr_debugger_sigrok_linkr_start_prepare_mark_response_sent(
				&prepare);
			if (ret < 0) {
				linkr_debugger_sigrok_linkr_start_prepare_cancel(&prepare,
					&runtime->session);
				(void)send_terminal_error_event(fd, &runtime->session);
				continue;
			}
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
				linkr_debugger_sigrok_linkr_start_prepare_cancel(&prepare,
					&runtime->session);
				sigrok_linkr_cleanup_capture(runtime);
				return;
			}
			if (prepare.state == LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_PREPARED &&
			    action.event.type_detail ==
			    (uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_ARMED) {
				ret = linkr_debugger_sigrok_linkr_start_prepare_mark_armed_event_sent(
					&prepare);
				if (ret < 0) {
					linkr_debugger_sigrok_linkr_start_prepare_cancel(&prepare,
						&runtime->session);
					(void)send_terminal_error_event(fd, &runtime->session);
					continue;
				}
			}
		}
		if (prepare.state == LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_PREPARED) {
			ret = linkr_debugger_sigrok_linkr_start_prepare_go(&prepare,
				&runtime->session);
			if (ret < 0) {
				(void)send_terminal_error_event(fd, &runtime->session);
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

static K_THREAD_STACK_DEFINE(server_stack, 2048U);
static struct k_thread server_thread_data;

int linkr_debugger_sigrok_linkr_init(void)
{
	memset(&linkr_debugger_sigrok_linkr_runtime, 0,
		sizeof(linkr_debugger_sigrok_linkr_runtime));
	linkr_debugger_sigrok_linkr_runtime.listen_fd = -1;
	linkr_debugger_sigrok_linkr_runtime.client_fd = -1;
	linkr_debugger_sigrok_linkr_runtime.next_sequence_id = 1U;
	k_fifo_init(&linkr_debugger_sigrok_linkr_runtime.stream_fifo);
	k_mutex_init(&linkr_debugger_sigrok_linkr_runtime.raw_burst.lock);
	k_sem_init(&linkr_debugger_sigrok_linkr_runtime.raw_burst.space_sem,
		LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT,
		LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT);
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
