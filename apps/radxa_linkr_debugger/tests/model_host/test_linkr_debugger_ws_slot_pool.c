/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

/* Offline only: compile the production WS slot lifecycle and sink commit.
 * Only RTOS/control dependencies are stubbed; no device or socket is opened.
 */
#include "linkr_debugger_capture_arena.h"
#include "linkr_debugger_sigrok_linkr.h"
#include "linkr_debugger_logic_analyzer.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define BUILD_ASSERT(condition, ...) _Static_assert(condition, #condition)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define ARG_UNUSED(value) (void)(value)
#define __aligned(value) __attribute__((aligned(value)))
#define K_NO_WAIT 0
#define K_MSEC(value) (value)
#define LINKR_DEBUGGER_WS_SIGROK_DATA_SLOT_COUNT LINKR_DEBUGGER_SIGROK_LINKR_WS_DATA_SLOT_COUNT
#define LINKR_DEBUGGER_WS_SIGROK_DATA_SLOT_BYTES LINKR_DEBUGGER_SIGROK_LINKR_WS_MAX_FRAME_BYTES
#define LINKR_DEBUGGER_WS_SIGROK_TERMINAL_SLOT_BYTES \
	(LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES + LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES)
#define LINKR_DEBUGGER_WS_SIGROK_BURST_DATA_SLOT_COUNT 2U
#define LINKR_DEBUGGER_WS_SIGROK_BURST_SLOT_WAIT_MS 1000U
#define LINKR_DEBUGGER_WS_SIGROK_QBYTES_LIMIT (36U * 1024U)
#define SIGROK_WS_STREAM_PAYLOAD_OFFSET \
	(LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES + LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES)

enum sigrok_ws_stream_item_kind {
	SIGROK_WS_STREAM_ITEM_FREE,
	SIGROK_WS_STREAM_ITEM_PREFRAMED,
};

struct k_spinlock {
	pthread_mutex_t mutex;
	bool held;
};
typedef int k_spinlock_key_t;

static k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{
	assert(pthread_mutex_lock(&lock->mutex) == 0);
	assert(!lock->held);
	lock->held = true;
	return 1;
}

static void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
	assert(key == 1 && lock->held);
	lock->held = false;
	assert(pthread_mutex_unlock(&lock->mutex) == 0);
}

struct k_fifo {
	void *head;
	void *tail;
};

static void k_fifo_put(struct k_fifo *fifo, void *item)
{
	*(void **)item = NULL;
	if (fifo->tail != NULL) {
		*(void **)fifo->tail = item;
	} else {
		fifo->head = item;
	}
	fifo->tail = item;
}

static void *k_fifo_get(struct k_fifo *fifo, int timeout)
{
	void *item = fifo->head;

	assert(timeout == K_NO_WAIT);
	if (item != NULL) {
		fifo->head = *(void **)item;
		if (fifo->head == NULL) {
			fifo->tail = NULL;
		}
	}
	return item;
}

struct k_sem {
	unsigned int count;
	unsigned int limit;
};

static void k_sem_init(struct k_sem *sem, unsigned int count, unsigned int limit)
{
	sem->count = count;
	sem->limit = limit;
}

static void k_sem_reset(struct k_sem *sem)
{
	sem->count = 0U;
}

static void k_sem_give(struct k_sem *sem)
{
	if (sem->count < sem->limit) {
		sem->count++;
	}
}

static int k_sem_take(struct k_sem *sem, int timeout)
{
	ARG_UNUSED(timeout);
	if (sem->count == 0U) {
		return -EAGAIN;
	}
	sem->count--;
	return 0;
}

typedef atomic_int atomic_t;
typedef int atomic_val_t;
#define atomic_get(value) atomic_load(value)
#define atomic_set(value, next) atomic_store(value, next)
#define atomic_inc(value) atomic_fetch_add(value, 1)
#define atomic_dec(value) atomic_fetch_sub(value, 1)
#define atomic_add(value, delta) atomic_fetch_add(value, delta)

static bool atomic_cas(atomic_t *value, atomic_val_t old, atomic_val_t next)
{
	return atomic_compare_exchange_strong(value, &old, next);
}

struct linkr_debugger_ws_client {
	uint32_t session_id;
	uint32_t sigrok_stream_generation;
	bool connected;
	struct k_fifo sigrok_stream_fifo;
	atomic_t sigrok_stream_qdepth;
	atomic_t sigrok_stream_qbytes;
	atomic_t sigrok_stream_dropped;
	atomic_t sigrok_stream_stop_pending;
	atomic_t sigrok_stream_abort_pending;
	atomic_t sigrok_stream_deferred_wake_pending;
	int64_t sigrok_rate_window_ms;
	int sigrok_rate_used;
	uint32_t sigrok_stream_slow_send_logs;
	struct linkr_debugger_sigrok_linkr_session sigrok_session;
};

static int64_t k_uptime_get(void)
{
	return 1;
}

static size_t sigrok_ws_stream_qbytes(const struct linkr_debugger_ws_client *client)
{
	return (size_t)atomic_get(&client->sigrok_stream_qbytes);
}

static void sigrok_ws_stream_metrics_update_enqueue(struct linkr_debugger_ws_client *client,
	uint32_t depth, size_t bytes)
{
	assert(depth == (uint32_t)atomic_get(&client->sigrok_stream_qdepth));
	assert(bytes == sigrok_ws_stream_qbytes(client));
}

static void sigrok_ws_stream_metrics_reset(struct linkr_debugger_ws_client *client)
{
	ARG_UNUSED(client);
}

static void sigrok_ws_stream_cancel_deferred_wake_sync(struct linkr_debugger_ws_client *client)
{
	ARG_UNUSED(client);
}

static void sigrok_ws_release_capture_if_held(struct linkr_debugger_ws_client *client)
{
	ARG_UNUSED(client);
}

static void sigrok_ws_stream_apply_wake_policy(struct linkr_debugger_ws_client *client,
	bool urgent)
{
	ARG_UNUSED(client);
	ARG_UNUSED(urgent);
}

static enum linkr_debugger_la_stream_payload_format sigrok_ws_stream_sink_payload_format(
	const struct linkr_debugger_sigrok_linkr_session *session)
{
	ARG_UNUSED(session);
	return LINKR_DEBUGGER_LA_STREAM_PAYLOAD_PACKED_LE_BYTES;
}

static bool sigrok_ws_stream_sink_final_chunk(
	const struct linkr_debugger_sigrok_linkr_session *session, uint32_t count)
{
	ARG_UNUSED(session);
	ARG_UNUSED(count);
	return false;
}

#define LINKR_DEBUGGER_WS_POOL_HOST_TEST 1
/* The protocol host model supplies its own burst-control stubs. Keep the
 * production WS pool entry points distinct when linking the two models.
 */
#define linkr_debugger_ws_sigrok_burst_pool_begin ws_slot_test_burst_pool_begin
#define linkr_debugger_ws_sigrok_burst_pool_abort ws_slot_test_burst_pool_abort
#include "linkr_debugger_ws.c"

static struct linkr_debugger_ws_client make_client(uint32_t session_id)
{
	return (struct linkr_debugger_ws_client){
		.session_id = session_id,
		.sigrok_stream_generation = 1U,
		.connected = true,
	};
}

static void assert_all_free(void)
{
	k_spinlock_key_t key = k_spin_lock(&sigrok_ws_stream_pool.lock);

	assert(sigrok_ws_stream_pool_all_free_locked());
	k_spin_unlock(&sigrok_ws_stream_pool.lock, key);
}

static void test_exact_capacity_and_resident_metadata(void)
{
	struct linkr_debugger_ws_client client = make_client(1U);
	struct sigrok_ws_stream_queue_item *items[4];
	struct sigrok_ws_stream_queue_item *terminal;
	uintptr_t base = (uintptr_t)linkr_debugger_capture_arena_base();

	assert(sizeof(sigrok_ws_stream_storage) == 16480U);
	for (size_t i = 0U; i < ARRAY_SIZE(items); i++) {
		items[i] = sigrok_ws_stream_slot_acquire(&client, false);
		assert(items[i] != NULL);
		assert((uintptr_t)items[i] < base ||
			(uintptr_t)items[i] >= base + linkr_debugger_capture_arena_size());
		assert(items[i]->capacity == 4113U);
		assert(items[i]->data == (uint8_t *)base + 78984U + i * 4116U);
		assert((uintptr_t)items[i]->data % 4U == 0U);
		assert(!sigrok_ws_stream_pool_try_quiesce());
	}
	assert(sigrok_ws_stream_slot_acquire(&client, false) == NULL);
	terminal = sigrok_ws_stream_slot_acquire(&client, true);
	assert(terminal != NULL && terminal->capacity == 15U);
	assert(terminal->data == (uint8_t *)base + 95448U);
	assert(sigrok_ws_stream_slot_acquire(&client, true) == NULL);
	for (size_t i = 0U; i < ARRAY_SIZE(items); i++) {
		sigrok_ws_stream_slot_release(items[i]);
	}
	assert(!sigrok_ws_stream_pool_try_quiesce());
	sigrok_ws_stream_slot_release(terminal);
	assert_all_free();
	printf("host metadata: item=%zu pool=%zu; payload=%zu; normal=%u arena=%u\n",
		sizeof(struct sigrok_ws_stream_queue_item), sizeof(sigrok_ws_stream_pool),
		sizeof(sigrok_ws_stream_storage), LINKR_DEBUGGER_CAPTURE_ARENA_NORMAL_BYTES,
		LINKR_DEBUGGER_CAPTURE_ARENA_BYTES);
}

static void test_reset_keeps_producer_and_sender_leases_pinned(void)
{
	struct linkr_debugger_ws_client client = make_client(2U);
	struct sigrok_ws_stream_queue_item *producer = sigrok_ws_stream_slot_acquire(&client, false);
	struct sigrok_ws_stream_queue_item *sender = sigrok_ws_stream_slot_acquire(&client, false);
	struct sigrok_ws_stream_queue_item *queued = sigrok_ws_stream_slot_acquire(&client, false);
	uint32_t generation = client.sigrok_stream_generation;

	assert(producer != NULL && sender != NULL && queued != NULL);
	sender->len = 17U;
	queued->len = 23U;
	assert(sigrok_ws_stream_put_accounted_item(&client, sender));
	assert(sigrok_ws_stream_put_accounted_item(&client, queued));
	assert(atomic_get(&client.sigrok_stream_qbytes) == 40);
	assert(sigrok_ws_stream_pop_accounted_item(&client) == sender);
	assert(atomic_get(&client.sigrok_stream_qbytes) == 23);
	sigrok_ws_stream_clear_stale_queue(&client);
	assert(client.sigrok_stream_generation == generation + 1U);
	assert(queued->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE);
	assert(sender->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED);
	assert(producer->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED);
	assert(!sigrok_ws_stream_slot_has_lease(&client, producer));
	assert(!sigrok_ws_stream_put_accounted_item(&client, producer));
	/* Rejected commit leaves the token pinned until its one abort. */
	assert(producer->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED);
	assert(!sigrok_ws_stream_pool_try_quiesce());
	assert(client.sigrok_stream_fifo.head == NULL);
	assert(atomic_get(&client.sigrok_stream_qdepth) == 0);
	assert(atomic_get(&client.sigrok_stream_qbytes) == 0);
	assert(sigrok_ws_disconnect_after_send_failure(&client, -EPIPE) == -EPIPE);
	assert(!sigrok_ws_stream_pool_try_quiesce());
	sigrok_ws_stream_slot_release(producer);
	assert(!sigrok_ws_stream_pool_try_quiesce());
	sigrok_ws_stream_free_accounted_item(&client, sender);
	assert_all_free();
	client.sigrok_stream_generation = UINT32_MAX;
	sigrok_ws_stream_reset_queue(&client);
	assert(client.sigrok_stream_generation == 1U);
}

static void test_commit_transfers_lease_until_send_completion(void)
{
	struct linkr_debugger_ws_client client = make_client(6U);
	struct sigrok_ws_stream_queue_item *item = sigrok_ws_stream_slot_acquire(&client, false);
	struct linkr_debugger_la_stream_sink_commit commit = {
		.token = item, .sample_count = 1U, .bytes_per_sample = 1U, .payload_len = 1U,
	};
	uint8_t encoded[4113];

	assert(item != NULL);
	client.sigrok_session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING;
	client.sigrok_session.config.channel_mask = 0xffU;
	*sigrok_ws_stream_payload(item) = 0x5aU;
	assert(sigrok_ws_stream_sink_commit(&commit, &client) >= 0);
	assert(item->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED);
	assert(!sigrok_ws_stream_pool_try_quiesce());
	assert(sigrok_ws_stream_pop_accounted_item(&client) == item);
	assert(!sigrok_ws_stream_pool_try_quiesce());
	assert(sigrok_ws_stream_encode_item(item, encoded, sizeof(encoded)) == item->len);
	assert(item->len == 18U && encoded[17] == 0x5aU);
	sigrok_ws_stream_free_accounted_item(&client, item);
	assert_all_free();
}

static void test_aborted_burst_queue_keeps_independent_metadata(void)
{
	struct linkr_debugger_ws_client client = make_client(7U);
	struct sigrok_ws_stream_queue_item *item;

	assert(ws_slot_test_burst_pool_begin(client.session_id, client.sigrok_stream_generation, 99U) == 0);
	item = sigrok_ws_stream_slot_acquire(&client, false);
	assert(item != NULL && item->burst_slot);
	item->len = 21U;
	assert(sigrok_ws_stream_put_accounted_item(&client, item));
	ws_slot_test_burst_pool_abort(client.session_id, client.sigrok_stream_generation);
	sigrok_ws_stream_reset_queue(&client);
	assert(item->burst_slot);
	assert(item->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE);
	assert(atomic_get(&client.sigrok_stream_qdepth) == 0);
	assert(atomic_get(&client.sigrok_stream_qbytes) == 0);
	assert_all_free();
}

static int pool_quiesce(int32_t timeout_ms, void *context)
{
	ARG_UNUSED(timeout_ms);
	ARG_UNUSED(context);
	return sigrok_ws_stream_pool_try_quiesce() ? 0 : -EBUSY;
}

static void pool_resume(void *context)
{
	ARG_UNUSED(context);
	sigrok_ws_stream_pool_reinitialize_after_arena_resume();
}

static void test_borrowed_dma_payload_survives_other_client_cleanup(void)
{
	struct linkr_debugger_ws_client other = make_client(3U);
	struct linkr_debugger_capture_arena_lease lease;
	const struct linkr_debugger_capture_arena_quiesce_ops ops = {
		.quiesce = pool_quiesce,
		.resume = pool_resume,
	};
	uint8_t expected[140000];
	struct sigrok_ws_stream_queue_item *stale = &sigrok_ws_stream_pool.data[0].item;
	struct sigrok_ws_stream_queue_item *held;
	struct linkr_debugger_la_stream_sink_commit invalid = {
		.token = stale, .sample_count = 1U, .bytes_per_sample = 1U, .payload_len = 1U,
	};

	linkr_debugger_capture_arena_init();
	linkr_debugger_capture_arena_register_quiesce_ops(&ops);
	held = sigrok_ws_stream_slot_acquire(&other, false);
	assert(held != NULL);
	assert(linkr_debugger_capture_arena_try_acquire_wide11_quiesced(54U, 0, &lease) == -EBUSY);
	assert(sigrok_ws_stream_slot_has_lease(&other, held));
	sigrok_ws_stream_slot_release(held);
	assert(linkr_debugger_capture_arena_try_acquire_wide11_quiesced(55U, 0, &lease) == 0);
	memset(expected, 0xd3, sizeof(expected));
	memcpy(linkr_debugger_capture_arena_burst_lane_a(), expected, sizeof(expected));
	assert(sigrok_ws_stream_pool.gate_closed);
	assert(sigrok_ws_stream_slot_acquire(&other, false) == NULL);
	assert(sigrok_ws_stream_slot_acquire(&other, true) == NULL);
	sigrok_ws_stream_reset_queue(&other);
	sigrok_ws_stream_clear_stale_queue(&other);
	assert(sigrok_ws_disconnect_after_send_failure(&other, -EPIPE) == -EPIPE);
	/* A callback without a lease must reject before touching overlaid payload. */
	other.connected = true;
	other.sigrok_session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING;
	other.sigrok_session.config.channel_mask = 1U;
	assert(sigrok_ws_stream_sink_commit(&invalid, &other) == -ESTALE);
	assert(!sigrok_ws_stream_put_accounted_item(&other, stale));
	assert(memcmp(expected, linkr_debugger_capture_arena_burst_lane_a(), sizeof(expected)) == 0);
	assert(linkr_debugger_capture_arena_canaries_ok());
	assert(linkr_debugger_capture_arena_release(&lease));
	assert(!sigrok_ws_stream_pool.gate_closed);
	assert(sigrok_ws_stream_slot_acquire(&other, true) == &sigrok_ws_stream_pool.terminal.item);
	sigrok_ws_stream_slot_release(&sigrok_ws_stream_pool.terminal.item);
	stale = sigrok_ws_stream_slot_acquire(&other, false);
	assert(stale != NULL && stale->data == sigrok_ws_stream_storage.data[0].bytes);
	assert(stale->capacity == 4113U && sigrok_ws_stream_slot_has_lease(&other, stale));
	sigrok_ws_stream_slot_release(stale);
	assert_all_free();
	linkr_debugger_capture_arena_register_quiesce_ops(NULL);
}

struct race_context {
	pthread_mutex_t start_lock;
	pthread_cond_t start_changed;
	unsigned int ready;
	struct linkr_debugger_ws_client client;
	struct sigrok_ws_stream_queue_item *item;
	bool quiesced;
};

static void race_start(struct race_context *context)
{
	assert(pthread_mutex_lock(&context->start_lock) == 0);
	context->ready++;
	if (context->ready == 2U) {
		assert(pthread_cond_broadcast(&context->start_changed) == 0);
	} else {
		while (context->ready != 2U) {
			assert(pthread_cond_wait(&context->start_changed, &context->start_lock) == 0);
		}
	}
	assert(pthread_mutex_unlock(&context->start_lock) == 0);
}

static void *race_acquire(void *data)
{
	struct race_context *context = data;

	race_start(context);
	context->item = sigrok_ws_stream_slot_acquire(&context->client, false);
	return NULL;
}

static void *race_quiesce(void *data)
{
	struct race_context *context = data;

	race_start(context);
	context->quiesced = sigrok_ws_stream_pool_try_quiesce();
	return NULL;
}

static void test_gate_and_lease_share_one_atomic_transition(void)
{
	struct linkr_debugger_ws_client client = make_client(4U);
	struct sigrok_ws_stream_queue_item *item;

	/* Exercise both orderings deterministically, then concurrent STARTs. */
	item = sigrok_ws_stream_slot_acquire(&client, false);
	assert(item != NULL && !sigrok_ws_stream_pool_try_quiesce());
	sigrok_ws_stream_pool_reinitialize_after_arena_resume();
	assert(sigrok_ws_stream_slot_has_lease(&client, item));
	sigrok_ws_stream_slot_release(item);
	assert(sigrok_ws_stream_pool_try_quiesce());
	assert(sigrok_ws_stream_slot_acquire(&client, false) == NULL);
	assert(sigrok_ws_stream_slot_acquire(&client, true) == NULL);
	sigrok_ws_stream_pool_reinitialize_after_arena_resume();
	for (unsigned int i = 0U; i < 100U; i++) {
		struct race_context context = { .client = make_client(5U) };
		pthread_t acquire;
		pthread_t quiesce;

		assert(pthread_mutex_init(&context.start_lock, NULL) == 0);
		assert(pthread_cond_init(&context.start_changed, NULL) == 0);
		assert(pthread_create(&acquire, NULL, race_acquire, &context) == 0);
		assert(pthread_create(&quiesce, NULL, race_quiesce, &context) == 0);
		assert(pthread_join(acquire, NULL) == 0);
		assert(pthread_join(quiesce, NULL) == 0);
		assert(context.quiesced != (context.item != NULL));
		if (context.item != NULL) {
			sigrok_ws_stream_slot_release(context.item);
		}
		sigrok_ws_stream_pool_reinitialize_after_arena_resume();
		assert(pthread_cond_destroy(&context.start_changed) == 0);
		assert(pthread_mutex_destroy(&context.start_lock) == 0);
		assert_all_free();
	}
}

int main(void)
{
	assert(pthread_mutex_init(&sigrok_ws_stream_pool.lock.mutex, NULL) == 0);
	assert(pthread_mutex_init(&sigrok_ws_burst_pool.lock.mutex, NULL) == 0);
	sigrok_ws_burst_pool_init();
	linkr_debugger_capture_arena_init();
	test_exact_capacity_and_resident_metadata();
	test_reset_keeps_producer_and_sender_leases_pinned();
	test_commit_transfers_lease_until_send_completion();
	test_aborted_burst_queue_keeps_independent_metadata();
	test_borrowed_dma_payload_survives_other_client_cleanup();
	test_gate_and_lease_share_one_atomic_transition();
	puts("WS slot pool: payload-only overlay, held leases, stale generation, gate race and resume passed");
	return 0;
}
