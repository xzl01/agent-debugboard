/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_ws.h"
#include "linkr_debugger_ws_sampler_sync.h"

#include "linkr_debugger_capture_arena.h"
#include "linkr_debugger_control.h"
#include "linkr_debugger_config_summary.h"
#include "linkr_debugger_gpio_error.h"
#include "linkr_debugger_logic_analyzer.h"
#include "linkr_debugger_json_cursor.h"
#include "linkr_debugger_monitoring.h"
#include "linkr_debugger_model.h"
#include "linkr_debugger_sigrok_linkr.h"
#include "linkr_debugger_capture_arbiter.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/websocket.h>
#include <zephyr/posix/poll.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(linkr_debugger_ws, LOG_LEVEL_INF);

#define LINKR_DEBUGGER_WS_STACK_SIZE 4096
#define LINKR_DEBUGGER_WS_PRIORITY K_PRIO_PREEMPT(8)
#define LINKR_DEBUGGER_ADC_SAMPLER_PRIORITY K_PRIO_PREEMPT(9)
#define LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE 6144
#define LINKR_DEBUGGER_WS_IDLE_WAIT_MS 100
#define LINKR_DEBUGGER_WS_SIGROK_QDEPTH_LIMIT LINKR_DEBUGGER_SIGROK_LINKR_STREAM_QDEPTH_LIMIT
#define LINKR_DEBUGGER_WS_SIGROK_DRAIN_BATCH 16
#define LINKR_DEBUGGER_WS_SIGROK_COALESCE_MAX_FRAMES 16U
#define LINKR_DEBUGGER_WS_SIGROK_COALESCE_MAX_BYTES LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE
#define LINKR_DEBUGGER_WS_SIGROK_QBYTES_LIMIT (36U * 1024U)
#define LINKR_DEBUGGER_WS_SIGROK_SEND_TIMEOUT_MS 1000
#define LINKR_DEBUGGER_WS_SIGROK_SEND_RATE_CAP 600
#define LINKR_DEBUGGER_WS_SIGROK_SEND_SLOW_US 5000U
#define LINKR_DEBUGGER_WS_SESSION_IDLE_TIMEOUT_MS 30000U
#define LINKR_DEBUGGER_WS_SIGROK_DATA_SLOT_COUNT \
	LINKR_DEBUGGER_SIGROK_LINKR_WS_DATA_SLOT_COUNT
#define LINKR_DEBUGGER_WS_SIGROK_DATA_SLOT_BYTES \
	LINKR_DEBUGGER_SIGROK_LINKR_WS_MAX_FRAME_BYTES
#define LINKR_DEBUGGER_WS_SIGROK_TERMINAL_SLOT_BYTES \
	(LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES + LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES)
#define LINKR_DEBUGGER_WS_SIGROK_BURST_DATA_SLOT_COUNT 2U
#define LINKR_DEBUGGER_WS_SIGROK_BURST_TERMINAL_SLOT_COUNT 1U
#define LINKR_DEBUGGER_WS_SIGROK_BURST_SLOT_WAIT_MS 1000U
#define LINKR_DEBUGGER_WS_TELEMETRY_ADC_CHANNELS \
	LINKR_DEBUGGER_ADC_TELEMETRY_CHANNEL_COUNT
#define LINKR_DEBUGGER_POWER_CAPTURE_CURRENT_CHANNELS \
	LINKR_DEBUGGER_CURRENT_SENSOR_COUNT
#define LINKR_DEBUGGER_WS_SAMPLE_RING_SIZE 256U
#define LINKR_DEBUGGER_WS_MAX_BATCH_SIZE 20U
#define LINKR_DEBUGGER_WS_SIGROK_SLOW_SEND_LOG_LIMIT 4U

BUILD_ASSERT(LINKR_DEBUGGER_WS_SIGROK_QDEPTH_LIMIT == 32,
	"Sigrok WS jitter buffer depth must match the measured HIL value");
BUILD_ASSERT(LINKR_DEBUGGER_WS_SIGROK_COALESCE_MAX_FRAMES == 16U,
	"Sigrok WS coalesce frame cap must remain at the measured HIL value");
BUILD_ASSERT(LINKR_DEBUGGER_WS_SIGROK_COALESCE_MAX_BYTES == LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
	"Sigrok WS coalesce byte cap must use the already allocated client tx buffer");
BUILD_ASSERT(LINKR_DEBUGGER_WS_SIGROK_QBYTES_LIMIT == (36U * 1024U),
	"Sigrok WS allocated queue byte cap must stay bounded for the 65 KiB heap");
BUILD_ASSERT(LINKR_DEBUGGER_WS_SIGROK_COALESCE_MAX_BYTES <= LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
	"Sigrok WS coalesce cap must fit in the client tx buffer");
BUILD_ASSERT(LINKR_DEBUGGER_WS_SIGROK_DATA_SLOT_COUNT == 4U,
	"Stage A WS Sigrok pool must have exactly four DATA slots");
BUILD_ASSERT(LINKR_DEBUGGER_SIGROK_LINKR_WS_TERMINAL_SLOT_COUNT == 1U,
	"Stage A WS Sigrok pool must have exactly one terminal slot");
BUILD_ASSERT(LINKR_DEBUGGER_WS_SIGROK_DATA_SLOT_BYTES ==
	(LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
	 LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES + 4096U),
	"Sigrok WS DATA slot must fit one maximum packed protocol frame");
BUILD_ASSERT(LINKR_DEBUGGER_WS_SIGROK_DATA_SLOT_BYTES <=
	LINKR_DEBUGGER_WS_SIGROK_COALESCE_MAX_BYTES,
	"A maximum Sigrok WS DATA slot must be independently sendable by the coalescer");
BUILD_ASSERT(LINKR_DEBUGGER_WS_SIGROK_TERMINAL_SLOT_BYTES <=
	LINKR_DEBUGGER_WS_SIGROK_COALESCE_MAX_BYTES,
	"Sigrok WS terminal slot must be independently sendable by the coalescer");
BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TX_SLOT_BYTES == 2068U,
	"Sigrok WS burst DATA slot capacity must stay 2068 bytes");
BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TERMINAL_BYTES == 16U,
	"Sigrok WS burst terminal slot capacity must stay 16 bytes");

BUILD_ASSERT(LINKR_DEBUGGER_WS_SAMPLE_RING_SIZE > LINKR_DEBUGGER_WS_MAX_BATCH_SIZE);
BUILD_ASSERT(LINKR_DEBUGGER_WS_TELEMETRY_ADC_CHANNELS == 4U,
	"WS telemetry must retain all four ADC channels");
BUILD_ASSERT(LINKR_DEBUGGER_POWER_CAPTURE_CURRENT_CHANNELS == 3U,
	"power capture must retain only three current channels");

struct linkr_debugger_ws_request {
	char type[16];
	char id[32];
	char command[16];
	char topic[32];
	char output[16];
	char mode[24];
	char state[8];
	char route[16];
	char gpio[64];
	char direction[8];
	char trigger[16];
	char edge[8];
	int value;
	int rate_hz;
	int threshold_ua;
	int pre_samples;
	int post_samples;
	int batch_size;
	bool subscribed;
	bool telemetry;
};

static const struct json_obj_descr linkr_debugger_ws_request_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, type, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, id, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, command, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, topic, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, output, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, mode, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, state, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, route, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, gpio, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, direction, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, trigger, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, edge, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, value, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, rate_hz, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, threshold_ua, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, pre_samples, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, post_samples, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, batch_size, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, subscribed, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, telemetry, JSON_TOK_TRUE),
};

enum linkr_debugger_capture_state {
	LINKR_DEBUGGER_CAPTURE_IDLE,
	LINKR_DEBUGGER_CAPTURE_ARMED,
	LINKR_DEBUGGER_CAPTURE_TRIGGERED,
};

struct linkr_debugger_capture {
	enum linkr_debugger_capture_state state;
	char trigger[16];
	char source[64];
	char edge[8];
	int32_t threshold_ua;
	uint16_t rate_hz;
	int64_t next_sample_due_us;
	bool last_level;
	bool trigger_pending;
	bool trigger_notice_pending;
	uint32_t capture_id;
	uint64_t trigger_device_t_mono_us;
	uint64_t trigger_sample_sequence;
};

struct linkr_debugger_ws_client {
	uint8_t slot;
	uint32_t session_id;
	int ws_sock;
	bool active;
	bool connected;
	bool telemetry_enabled;
	int telemetry_rate_hz;
	uint8_t telemetry_batch_size;
	uint64_t next_sample_sequence;
	uint64_t telemetry_dropped_samples;
	int64_t next_sample_due_us;
	uint64_t sequence;
	int64_t session_created_ms;
	struct k_event events;
	struct k_mutex lock;
	uint8_t recv_buffer[LINKR_DEBUGGER_WS_RECV_BUFFER_SIZE];
	uint8_t tx_buffer[LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE];
	k_tid_t thread;
	struct k_thread thread_data;
	struct linkr_debugger_sigrok_linkr_session sigrok_session;
	bool sigrok_active;
	struct k_fifo sigrok_stream_fifo;
	atomic_t sigrok_stream_qdepth;
	atomic_t sigrok_stream_qbytes;
	atomic_t sigrok_stream_dropped;
	atomic_t sigrok_stream_stop_pending;
	atomic_t sigrok_stream_abort_pending;
	atomic_t sigrok_stream_deferred_wake_pending;
	struct k_work_delayable sigrok_stream_wake_work;
	struct k_spinlock sigrok_stream_metrics_lock;
	struct linkr_debugger_sigrok_linkr_ws_transport_metrics sigrok_stream_metrics;
	int64_t sigrok_rate_window_ms;
	int sigrok_rate_used;
	uint32_t sigrok_stream_slow_send_logs;
	uint32_t sigrok_stream_generation;
};

struct linkr_debugger_ws_adc_sample {
	struct linkr_debugger_current_sample readings[LINKR_DEBUGGER_WS_TELEMETRY_ADC_CHANNELS];
	uint64_t sequence;
	int64_t uptime_us;
	int source_rate_hz;
};

struct linkr_debugger_ws_client_thread_arg {
	struct linkr_debugger_ws_client *client;
	int ws_sock;
	uint32_t session_id;
};

struct linkr_debugger_ws_sampler_workspace {
	struct linkr_debugger_current_sample
		readings[LINKR_DEBUGGER_CURRENT_BATCH_MAX][LINKR_DEBUGGER_WS_TELEMETRY_ADC_CHANNELS];
	int64_t timestamps_us[LINKR_DEBUGGER_CURRENT_BATCH_MAX];
	struct linkr_debugger_ws_adc_sample ingest_sample;
};

static struct linkr_debugger_ws_client linkr_debugger_ws_clients[LINKR_DEBUGGER_WS_MAX_CLIENTS]
	Z_GENERIC_SECTION(.bss.pre_capture.ws_clients);
BUILD_ASSERT(sizeof(linkr_debugger_ws_clients) == 28704U);
static struct linkr_debugger_ws_client_thread_arg linkr_debugger_ws_thread_args[LINKR_DEBUGGER_WS_MAX_CLIENTS];
static K_THREAD_STACK_ARRAY_DEFINE(linkr_debugger_ws_stacks, LINKR_DEBUGGER_WS_MAX_CLIENTS,
	LINKR_DEBUGGER_WS_STACK_SIZE);
static struct k_mutex linkr_debugger_ws_clients_lock;
static struct k_mutex linkr_debugger_capture_lock;
static uint32_t linkr_debugger_ws_next_session_id = 1U;
#define LINKR_DEBUGGER_POWER_CAPTURE \
	(*((struct linkr_debugger_capture *)linkr_debugger_capture_arena_power_capture()))
static uint32_t linkr_debugger_capture_owner_session_id;
static uint32_t linkr_debugger_next_capture_id = 1U;

#define linkr_debugger_ws_sample_ring \
	((struct linkr_debugger_ws_adc_sample *)linkr_debugger_capture_arena_ws_sample_ring())
static struct linkr_debugger_ws_sampler_workspace linkr_debugger_ws_sampler_workspace;
static struct k_mutex linkr_debugger_ws_sample_ring_lock;
static struct k_event linkr_debugger_ws_sampler_events;
static struct k_sem linkr_debugger_ws_sampler_pause_ack;
static bool linkr_debugger_ws_sampler_pause_requested;
static bool linkr_debugger_ws_sigrok_telemetry_pause_requested;
static bool linkr_debugger_ws_arena_quiesced;
static struct linkr_debugger_ws_sampler_sync linkr_debugger_ws_sampler_sync;
static uint64_t linkr_debugger_ws_latest_sample_sequence;
static K_THREAD_STACK_DEFINE(linkr_debugger_adc_sampler_stack, 2048);
static struct k_thread linkr_debugger_adc_sampler_thread_data;

BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_WS_SAMPLE_RING_BYTES ==
	LINKR_DEBUGGER_WS_SAMPLE_RING_SIZE * sizeof(struct linkr_debugger_ws_adc_sample),
	"WS ADC sample ring arena slice size mismatch");
BUILD_ASSERT(sizeof(struct linkr_debugger_capture) <=
	LINKR_DEBUGGER_CAPTURE_ARENA_POWER_CAPTURE_BYTES,
	"power capture arena slice size mismatch");

enum {
	LINKR_DEBUGGER_WS_EVENT_STATE = BIT(0),
	LINKR_DEBUGGER_WS_EVENT_SAMPLE = BIT(1),
	LINKR_DEBUGGER_WS_EVENT_STREAM_DATA = BIT(2),
};

enum {
	LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG = BIT(0),
	LINKR_DEBUGGER_WS_SAMPLER_EVENT_RESUME = BIT(1),
};

static void sigrok_ws_release_capture_if_held(struct linkr_debugger_ws_client *client);
static void sigrok_ws_stream_cancel_deferred_wake(struct linkr_debugger_ws_client *client);
static void sigrok_ws_stream_cancel_deferred_wake_sync(struct linkr_debugger_ws_client *client);
static void sigrok_ws_stream_reset_queue(struct linkr_debugger_ws_client *client);
static int linkr_debugger_ws_capture_arena_quiesce(int32_t timeout_ms, void *user_data);
static void linkr_debugger_ws_capture_arena_resume(void *user_data);

static int linkr_debugger_ws_send_json_timeout(struct linkr_debugger_ws_client *client,
					       const char *payload, int32_t timeout_ms)
{
	int ret;

	ret = websocket_send_msg(client->ws_sock, (const uint8_t *)payload,
				 strlen(payload), WEBSOCKET_OPCODE_DATA_TEXT,
				 false, true, timeout_ms);
	if (ret < 0) {
		LOG_ERR("websocket_send_msg failed: %d", ret);
		client->telemetry_enabled = false;
	}

	return ret;
}

static int linkr_debugger_ws_send_json(struct linkr_debugger_ws_client *client, const char *payload)
{
	return linkr_debugger_ws_send_json_timeout(client, payload, SYS_FOREVER_MS);
}

static struct linkr_debugger_ws_client *linkr_debugger_ws_client_allocate(void)
{
	struct linkr_debugger_ws_client *client = NULL;

	k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(linkr_debugger_ws_clients); i++) {
		if (!linkr_debugger_ws_clients[i].active && linkr_debugger_ws_clients[i].thread == NULL) {
			client = &linkr_debugger_ws_clients[i];
			client->active = true;
			client->connected = false;
			client->session_id = linkr_debugger_ws_next_session_id++;
			if (client->session_id == 0U) {
				client->session_id = linkr_debugger_ws_next_session_id++;
			}
			client->session_created_ms = k_uptime_get();
			break;
		}
	}
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);

	return client;
}

static void linkr_debugger_ws_client_reset(struct linkr_debugger_ws_client *client)
{
	client->session_id = 0U;
	client->ws_sock = -1;
	client->connected = false;
	client->telemetry_enabled = false;
	client->telemetry_rate_hz = 10;
	client->telemetry_batch_size = 1U;
	client->next_sample_sequence = 0U;
	client->telemetry_dropped_samples = 0U;
	client->next_sample_due_us = 0;
	client->sequence = 1U;
	client->session_created_ms = 0;
	client->thread = NULL;
}

static void linkr_debugger_ws_client_release(struct linkr_debugger_ws_client *client, uint32_t session_id)
{
	k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
	if (client->session_id != session_id) {
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);
		return;
	}
	client->active = false;
	k_event_clear(&client->events, UINT32_MAX);
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);

	sigrok_ws_release_capture_if_held(client);
	sigrok_ws_stream_reset_queue(client);

	k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
	if (client->session_id == session_id) {
		linkr_debugger_ws_client_reset(client);
	}
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);

	k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
	if (linkr_debugger_capture_owner_session_id == session_id) {
		memset(&LINKR_DEBUGGER_POWER_CAPTURE, 0, sizeof(LINKR_DEBUGGER_POWER_CAPTURE));
		linkr_debugger_capture_owner_session_id = 0U;
	}
	k_mutex_unlock(&linkr_debugger_capture_lock);
	k_event_post(&linkr_debugger_ws_sampler_events,
		     LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
}

static struct linkr_debugger_ws_client *linkr_debugger_ws_client_find_by_session_id(uint32_t session_id)
{
	for (size_t i = 0; i < ARRAY_SIZE(linkr_debugger_ws_clients); i++) {
		if (linkr_debugger_ws_clients[i].active && linkr_debugger_ws_clients[i].session_id == session_id) {
			return &linkr_debugger_ws_clients[i];
		}
	}

	return NULL;
}

static struct linkr_debugger_ws_client *linkr_debugger_ws_client_find_by_slot(uint8_t slot)
{
	if (slot >= ARRAY_SIZE(linkr_debugger_ws_clients)) {
		return NULL;
	}

	if (!linkr_debugger_ws_clients[slot].active) {
		return NULL;
	}

	return &linkr_debugger_ws_clients[slot];
}

static void linkr_debugger_ws_session_reap_expired(void)
{
	int64_t now = k_uptime_get();

	k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(linkr_debugger_ws_clients); i++) {
		struct linkr_debugger_ws_client *client = &linkr_debugger_ws_clients[i];

		if (!client->active || client->connected) {
			continue;
		}

		if (client->thread == NULL &&
		    (now - client->session_created_ms) >= LINKR_DEBUGGER_WS_SESSION_IDLE_TIMEOUT_MS) {
			sigrok_ws_stream_reset_queue(client);
			client->active = false;
			linkr_debugger_ws_client_reset(client);
			k_event_clear(&client->events, UINT32_MAX);
		}
	}
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);
}

static void linkr_debugger_ws_publish(uint32_t event_mask)
{
	k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(linkr_debugger_ws_clients); i++) {
		if (linkr_debugger_ws_clients[i].active) {
			k_event_post(&linkr_debugger_ws_clients[i].events, event_mask);
		}
	}
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);
}

static int linkr_debugger_ws_append(char *buf, size_t size, size_t *cursor, const char *fmt, ...)
{
	va_list args;
	int ret;

	if (*cursor >= size) {
		return -ENOMEM;
	}

	va_start(args, fmt);
	ret = vsnprintk(buf + *cursor, size - *cursor, fmt, args);
	va_end(args);
	if (ret < 0) {
		return ret;
	}
	if ((size_t)ret >= size - *cursor) {
		*cursor = size - 1U;
		return -ENOMEM;
	}

	*cursor += (size_t)ret;
	return 0;
}

struct linkr_debugger_ws_string_context {
	char *buf;
	size_t size;
	size_t *cursor;
};

static int linkr_debugger_ws_write(void *context, const char *text, size_t len)
{
	struct linkr_debugger_ws_string_context *state = context;

	return linkr_debugger_ws_append(state->buf, state->size, state->cursor,
					"%.*s", (int)len, text);
}

static int linkr_debugger_ws_append_json_string(char *buf, size_t size, size_t *cursor,
						const char *value)
{
	struct linkr_debugger_ws_string_context context = {
		.buf = buf, .size = size, .cursor = cursor,
	};

	return linkr_debugger_json_append_string(
		&context, linkr_debugger_ws_write,
		value != NULL ? value : "", value != NULL ? strlen(value) : 0U);
}

static int linkr_debugger_ws_append_availability(char *buf, size_t size, size_t *cursor,
					    bool available, const char *reason)
{
	if (linkr_debugger_ws_append(buf, size, cursor,
				    "{\"available\":%s,\"reason\":",
				    available ? "true" : "false") < 0) {
		return -ENOMEM;
	}

	return linkr_debugger_ws_append_json_string(buf, size, cursor, reason);
}

static int linkr_debugger_ws_append_board_monitoring(char *buf, size_t size, size_t *cursor)
{
	struct linkr_debugger_monitoring_snapshot snapshot;

	linkr_debugger_monitoring_snapshot_get(&snapshot);

	if (linkr_debugger_ws_append(buf, size, cursor, "\"board_monitoring\":{\"temperature\":") < 0 ||
	    linkr_debugger_ws_append_availability(buf, size, cursor, snapshot.temperature.available,
					      snapshot.temperature.reason) < 0) {
		return -ENOMEM;
	}
	if (snapshot.temperature.available) {
		if (linkr_debugger_ws_append(buf, size, cursor, ",\"source\":") < 0 ||
		    linkr_debugger_ws_append_json_string(buf, size, cursor,
						  snapshot.temperature.source) < 0 ||
		    linkr_debugger_ws_append(buf, size, cursor,
					     ",\"celsius\":{\"val1\":%d,\"val2\":%d}}",
					     snapshot.temperature.celsius_val1,
					     snapshot.temperature.celsius_val2) < 0) {
			return -ENOMEM;
		}
	} else if (snapshot.temperature.error != 0) {
		if (linkr_debugger_ws_append(buf, size, cursor, ",\"error\":%d}",
					     snapshot.temperature.error) < 0) {
			return -ENOMEM;
		}
	} else if (linkr_debugger_ws_append(buf, size, cursor, "}") < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_ws_append(buf, size, cursor, ",\"heap\":") < 0 ||
	    linkr_debugger_ws_append_availability(buf, size, cursor, snapshot.heap.available,
					      snapshot.heap.reason) < 0) {
		return -ENOMEM;
	}
	if (snapshot.heap.available) {
		if (linkr_debugger_ws_append(buf, size, cursor, ",\"source\":") < 0 ||
		    linkr_debugger_ws_append_json_string(buf, size, cursor, snapshot.heap.source) < 0 ||
		    linkr_debugger_ws_append(buf, size, cursor,
					     ",\"free_bytes\":%u,\"allocated_bytes\":%u,"
					     "\"max_allocated_bytes\":%u,\"total_bytes\":%u}",
					     (unsigned int)snapshot.heap.free_bytes,
					     (unsigned int)snapshot.heap.allocated_bytes,
					     (unsigned int)snapshot.heap.max_allocated_bytes,
					     (unsigned int)snapshot.heap.total_bytes) < 0) {
			return -ENOMEM;
		}
	} else if (snapshot.heap.error != 0) {
		if (linkr_debugger_ws_append(buf, size, cursor, ",\"error\":%d}", snapshot.heap.error) < 0) {
			return -ENOMEM;
		}
	} else if (linkr_debugger_ws_append(buf, size, cursor, "}") < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_ws_append(buf, size, cursor, ",\"memory\":") < 0 ||
	    linkr_debugger_ws_append_availability(buf, size, cursor, snapshot.memory.available,
					      snapshot.memory.reason) < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor, ",\"source\":") < 0 ||
	    linkr_debugger_ws_append_json_string(buf, size, cursor, snapshot.memory.source) < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor, ",\"coverage\":") < 0 ||
	    linkr_debugger_ws_append_json_string(buf, size, cursor, snapshot.memory.coverage) < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor,
				     ",\"pressure_pct_x100\":%u,\"limiting_component\":",
				     (unsigned int)snapshot.memory.pressure_pct_x100) < 0 ||
	    linkr_debugger_ws_append_json_string(buf, size, cursor,
					  snapshot.memory.limiting_component) < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor, ",\"limiting_name\":") < 0 ||
	    linkr_debugger_ws_append_json_string(buf, size, cursor, snapshot.memory.limiting_name) < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor,
				     ",\"system_heap_pressure_pct_x100\":%u,"
				     "\"physical\":{\"total_bytes\":%u,\"image_reserved_bytes\":%u,"
				     "\"reserved_pct_x100\":%u},"
				     "\"stacks\":{\"thread_count\":%u,\"measured_count\":%u,"
				     "\"error_count\":%u,\"total_bytes\":%u,"
				     "\"used_high_water_bytes\":%u,\"max_pressure_pct_x100\":%u,"
				     "\"max_pressure_thread\":",
				     (unsigned int)snapshot.memory.system_heap_pressure_pct_x100,
				     (unsigned int)snapshot.memory.physical.total_bytes,
				     (unsigned int)snapshot.memory.physical.image_reserved_bytes,
				     (unsigned int)snapshot.memory.physical.reserved_pct_x100,
				     (unsigned int)snapshot.memory.stacks.thread_count,
				     (unsigned int)snapshot.memory.stacks.measured_count,
				     (unsigned int)snapshot.memory.stacks.error_count,
				     (unsigned int)snapshot.memory.stacks.total_bytes,
				     (unsigned int)snapshot.memory.stacks.used_high_water_bytes,
				     (unsigned int)snapshot.memory.stacks.max_pressure_pct_x100) < 0 ||
	    linkr_debugger_ws_append_json_string(buf, size, cursor,
					  snapshot.memory.stacks.max_pressure_thread) < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor, "},\"current_pressure\":") < 0 ||
	    linkr_debugger_ws_append_availability(buf, size, cursor,
					       snapshot.memory.current_pressure.available,
					       snapshot.memory.current_pressure.reason) < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor, ",\"coverage\":") < 0 ||
	    linkr_debugger_ws_append_json_string(buf, size, cursor,
					  snapshot.memory.current_pressure.coverage) < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor,
				     ",\"pressure_pct_x100\":%u,\"limiting_component\":",
				     (unsigned int)snapshot.memory.current_pressure.pressure_pct_x100) < 0 ||
	    linkr_debugger_ws_append_json_string(buf, size, cursor,
					  snapshot.memory.current_pressure.limiting_component) < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor, ",\"limiting_name\":") < 0 ||
	    linkr_debugger_ws_append_json_string(buf, size, cursor,
					  snapshot.memory.current_pressure.limiting_name) < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor, ",\"tie_count\":%u},\"peak_pressure\":",
				     (unsigned int)snapshot.memory.current_pressure.tie_count) < 0 ||
	    linkr_debugger_ws_append_availability(buf, size, cursor,
					       snapshot.memory.peak_pressure.available,
					       snapshot.memory.peak_pressure.reason) < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor, ",\"coverage\":") < 0 ||
	    linkr_debugger_ws_append_json_string(buf, size, cursor,
					  snapshot.memory.peak_pressure.coverage) < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor,
				     ",\"pressure_pct_x100\":%u,\"limiting_component\":",
				     (unsigned int)snapshot.memory.peak_pressure.pressure_pct_x100) < 0 ||
	    linkr_debugger_ws_append_json_string(buf, size, cursor,
					  snapshot.memory.peak_pressure.limiting_component) < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor, ",\"limiting_name\":") < 0 ||
	    linkr_debugger_ws_append_json_string(buf, size, cursor,
					  snapshot.memory.peak_pressure.limiting_name) < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor, ",\"tie_count\":%u,\"since\":",
				     (unsigned int)snapshot.memory.peak_pressure.tie_count) < 0 ||
	    linkr_debugger_ws_append_json_string(buf, size, cursor, "boot") < 0 ||
	    linkr_debugger_ws_append(buf, size, cursor, "}}") < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_ws_append(buf, size, cursor, ",\"runtime\":") < 0 ||
	    linkr_debugger_ws_append_availability(buf, size, cursor, snapshot.runtime.available,
					      snapshot.runtime.reason) < 0) {
		return -ENOMEM;
	}
	if (snapshot.runtime.available) {
		if (linkr_debugger_ws_append(buf, size, cursor,
					     ",\"uptime_ms\":%lld,\"uptime_seconds\":%llu}",
					     (long long)snapshot.runtime.uptime_ms,
					     (unsigned long long)snapshot.runtime.uptime_seconds) < 0) {
			return -ENOMEM;
		}
	} else if (snapshot.runtime.error != 0) {
		if (linkr_debugger_ws_append(buf, size, cursor, ",\"error\":%d}", snapshot.runtime.error) < 0) {
			return -ENOMEM;
		}
	} else if (linkr_debugger_ws_append(buf, size, cursor, "}") < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_ws_append(buf, size, cursor, ",\"cpu\":") < 0 ||
	    linkr_debugger_ws_append_availability(buf, size, cursor, snapshot.cpu.available,
					      snapshot.cpu.reason) < 0) {
		return -ENOMEM;
	}
	if (snapshot.cpu.available) {
		if (linkr_debugger_ws_append(buf, size, cursor,
					     ",\"active_pct_x100\":%u,\"window_ms\":%u,"
					     "\"busy_cycles_delta\":%llu,\"total_cycles_delta\":%llu}",
					     (unsigned int)snapshot.cpu.active_pct_x100,
					     (unsigned int)snapshot.cpu.window_ms,
					     snapshot.cpu.busy_cycles_delta,
					     snapshot.cpu.total_cycles_delta) < 0) {
			return -ENOMEM;
		}
	} else if (snapshot.cpu.error != 0) {
		if (linkr_debugger_ws_append(buf, size, cursor, ",\"error\":%d}", snapshot.cpu.error) < 0) {
			return -ENOMEM;
		}
	} else if (linkr_debugger_ws_append(buf, size, cursor, "}") < 0) {
		return -ENOMEM;
	}

	return linkr_debugger_ws_append(buf, size, cursor, "}");
}

static int linkr_debugger_ws_append_safe_gpios(char *buf, size_t size, size_t *cursor)
{
	char name[LINKR_DEBUGGER_GPIO_NAME_BUFSZ];
	int value;
	int ret;

	for (size_t i = 0; i < linkr_debugger_safe_gpio_count; i++) {
		const struct linkr_debugger_safe_gpio_desc *desc = &linkr_debugger_safe_gpios[i];

		if (i > 0U && linkr_debugger_ws_append(buf, size, cursor, ",") < 0) {
			return -ENOMEM;
		}

		if (!linkr_debugger_format_gpio_name(desc->pin, name, sizeof(name))) {
			strcpy(name, "GP?");
		}

		ret = linkr_debugger_gpio_get(desc, &value);
		if (ret < 0) {
			value = 0;
		}

		if (linkr_debugger_ws_append(buf, size, cursor,
					"{\"name\":\"%s\",\"pin\":%u,\"note\":\"%s\","
					"\"layoutGroup\":\"%s\",\"layoutLabel\":\"%s\","
					"\"layoutRow\":%u,\"layoutColumn\":%u,"
					"\"value\":%d,\"direction\":\"%s\"}",
					name, (unsigned int)desc->pin,
					desc->note, desc->layout_group, desc->layout_label,
					(unsigned int)desc->layout_row,
					(unsigned int)desc->layout_column,
					value > 0 ? 1 : 0,
					linkr_debugger_safe_gpio_direction_name(i)) < 0) {
			return -ENOMEM;
		}
	}

	return 0;
}

static int linkr_debugger_ws_emit_status_snapshot(struct linkr_debugger_ws_client *client)
{
	char *buf = (char *)client->tx_buffer;
	struct linkr_debugger_config_service_status config_status;
	const struct linkr_debugger_config_service_status *config_status_ptr = NULL;
	struct linkr_debugger_config_summary_buffer config_summary_buffer;
	struct linkr_debugger_watchdog_status watchdog;
	size_t cursor = 0U;

	if (linkr_debugger_config_service_status_get(&config_status) ==
	    LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		config_status_ptr = &config_status;
	}

	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
				 "{\"type\":\"snapshot\",\"topic\":\"status\","
				 "\"schema\":\"%s\",\"power_capture_protocol\":\"%s\","
				 "\"sequence\":%llu,\"power_outputs\":[",
				 linkr_debugger_json_schema(),
				 linkr_debugger_power_capture_protocol(),
				 (unsigned long long)client->sequence++) < 0) {
		return -ENOMEM;
	}

	for (size_t i = 0; i < linkr_debugger_rail_count; i++) {
		const struct linkr_debugger_rail_desc *output = &linkr_debugger_rails[i];
		bool enabled = linkr_debugger_power_output_enabled(output);

		if (i > 0U && linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor, ",") < 0) {
			return -ENOMEM;
		}

		if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
					 "{\"name\":\"%s\",\"state\":\"%s\",\"value\":%d}",
					 output->name,
					 enabled ? "on" : "off",
					 enabled ? 1 : 0) < 0) {
			return -ENOMEM;
		}
	}

	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
				 "],\"sd\":{\"route\":\"%s\"},",
				 linkr_debugger_sd_route_name()) < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
				 "\"switches\":{\"sd\":{\"route\":\"%s\","
				 "\"routes\":[\"target\",\"usb-reader\"],\"requires_confirm\":false},"
				 "\"usb\":{\"route\":\"%s\","
				 "\"routes\":[\"pc\",\"target\"],\"requires_confirm\":true},"
				 "\"tf_wp\":{\"route\":\"%s\","
				 "\"routes\":[\"writable\",\"protected\"],\"requires_confirm\":false}",
				 linkr_debugger_sd_route_name(), linkr_debugger_usb_route_name(),
				 linkr_debugger_tf_wp_route_name()) < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_vin_switch_available() &&
		linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
					 ",\"vin\":{\"route\":\"%s\","
					 "\"routes\":[\"1.8v\",\"3.3v\"],\"requires_confirm\":true}",
					 linkr_debugger_vin_route_name()) < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor, "},") < 0) {
		return -ENOMEM;
	}

	linkr_debugger_watchdog_status_get(&watchdog);
	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
				 "\"watchdog\":{\"supported\":%s,\"automatic\":%s,\"healthy\":%s,"
				 "\"armed\":%s,\"timeout_ms\":%u,\"bootloader_on_timeout\":%s,"
				 "\"failing_service\":",
				 watchdog.supported ? "true" : "false",
				 watchdog.automatic ? "true" : "false",
				 watchdog.healthy ? "true" : "false",
				 watchdog.armed ? "true" : "false",
				 (unsigned int)watchdog.timeout_ms,
				 watchdog.bootloader_on_timeout ? "true" : "false") < 0) {
		return -ENOMEM;
	}
	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor, "%s",
				 watchdog.failing_service == NULL ? "\"\"}" : "") < 0) {
		return -ENOMEM;
	}
	if (watchdog.failing_service != NULL) {
		if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
					 "\"%s\"}", watchdog.failing_service) < 0) {
			return -ENOMEM;
		}
	}
	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor, ",\"gpios\":[") < 0 ||
	    linkr_debugger_ws_append_safe_gpios(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor) < 0 ||
	    linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor, "],") < 0 ||
	    linkr_debugger_ws_append_board_monitoring(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
					      &cursor) < 0) {
		return -ENOMEM;
	}
	config_summary_buffer = (struct linkr_debugger_config_summary_buffer){
		.data = buf,
		.capacity = LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
		.length = cursor,
		.tail_reserve = 1U,
	};
	(void)linkr_debugger_config_summary_append(&config_summary_buffer, config_status_ptr);
	cursor = config_summary_buffer.length;
	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor, "}") < 0) {
		return -ENOMEM;
	}

	return linkr_debugger_ws_send_json(client, buf);
}

static int linkr_debugger_ws_requested_sample_rate(void)
{
	int rate_hz = 0;

	k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(linkr_debugger_ws_clients); i++) {
		const struct linkr_debugger_ws_client *client = &linkr_debugger_ws_clients[i];

		if (client->active && client->connected && client->telemetry_enabled &&
		    client->telemetry_rate_hz > rate_hz) {
			rate_hz = client->telemetry_rate_hz;
		}
	}
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);

	k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
	if (linkr_debugger_capture_owner_session_id != 0U &&
	    LINKR_DEBUGGER_POWER_CAPTURE.state == LINKR_DEBUGGER_CAPTURE_ARMED &&
	    LINKR_DEBUGGER_POWER_CAPTURE.rate_hz > rate_hz) {
		rate_hz = LINKR_DEBUGGER_POWER_CAPTURE.rate_hz;
	}
	k_mutex_unlock(&linkr_debugger_capture_lock);

	return rate_hz;
}

static void linkr_debugger_ws_publish_unbatched_sample(void)
{
	k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(linkr_debugger_ws_clients); i++) {
		struct linkr_debugger_ws_client *client = &linkr_debugger_ws_clients[i];

		if (client->active && client->connected && client->telemetry_enabled &&
		    client->telemetry_batch_size == 1U) {
			k_event_post(&client->events, LINKR_DEBUGGER_WS_EVENT_SAMPLE);
		}
	}
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);
}

static int linkr_debugger_ws_current_index(const char *name)
{
	const struct linkr_debugger_current_desc *current = linkr_debugger_find_current(name);

	if (current == NULL) {
		return -1;
	}
	for (size_t i = 0; i < linkr_debugger_current_count; i++) {
		if (&linkr_debugger_currents[i] == current) {
			return (int)i;
		}
	}
	return -1;
}

static void linkr_debugger_ws_wake_session(uint32_t session_id, uint32_t event_mask)
{
	k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(linkr_debugger_ws_clients); i++) {
		struct linkr_debugger_ws_client *client = &linkr_debugger_ws_clients[i];

		if (client->active && client->session_id == session_id) {
			k_event_post(&client->events, event_mask);
			break;
		}
	}
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);
}

static bool linkr_debugger_capture_should_trigger(struct linkr_debugger_capture *capture,
						  const struct linkr_debugger_ws_adc_sample *sample)
{
	bool level = false;
	bool triggered = capture->trigger_pending;

	if (strcmp(capture->trigger, "current") == 0) {
		int index = linkr_debugger_ws_current_index(capture->source);
		triggered = index >= 0 && sample->readings[index].rail_enabled &&
			sample->readings[index].current_ua >= capture->threshold_ua;
	} else if (strcmp(capture->trigger, "gpio") == 0) {
		const struct linkr_debugger_safe_gpio_desc *gpio =
			linkr_debugger_find_safe_gpio_by_identifier(capture->source);
		int value = 0;

		if (gpio != NULL && linkr_debugger_gpio_get(gpio, &value) == 0) {
			level = value > 0;
			if (strcmp(capture->edge, "rising") == 0) {
				triggered = !capture->last_level && level;
			} else if (strcmp(capture->edge, "falling") == 0) {
				triggered = capture->last_level && !level;
			} else {
				triggered = capture->last_level != level;
			}
			capture->last_level = level;
		}
	} else if (strcmp(capture->trigger, "power_on") == 0) {
		int index = linkr_debugger_ws_current_index(capture->source);

		if (index >= 0) {
			level = sample->readings[index].rail_enabled;
			triggered = !capture->last_level && level;
			capture->last_level = level;
		}
	}

	capture->trigger_pending = false;
	return triggered;
}

static bool linkr_debugger_capture_should_accept_sample(struct linkr_debugger_capture *capture,
						       const struct linkr_debugger_ws_adc_sample *sample)
{
	uint32_t period_us;

	if (sample->source_rate_hz <= capture->rate_hz) {
		capture->next_sample_due_us = sample->uptime_us;
		return true;
	}

	period_us = DIV_ROUND_UP(1000000U, (uint32_t)capture->rate_hz);
	if (capture->next_sample_due_us == 0) {
		capture->next_sample_due_us = sample->uptime_us + period_us;
		return true;
	}
	if (sample->uptime_us < capture->next_sample_due_us) {
		return false;
	}

	do {
		capture->next_sample_due_us += period_us;
	} while (capture->next_sample_due_us <= sample->uptime_us);

	return true;
}

static void linkr_debugger_capture_ingest_sample(const struct linkr_debugger_ws_adc_sample *sample)
{
	uint32_t wake_session_id = 0U;

	k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
	if (linkr_debugger_capture_owner_session_id == 0U ||
	    LINKR_DEBUGGER_POWER_CAPTURE.state != LINKR_DEBUGGER_CAPTURE_ARMED ||
	    !linkr_debugger_capture_should_accept_sample(&LINKR_DEBUGGER_POWER_CAPTURE, sample)) {
		k_mutex_unlock(&linkr_debugger_capture_lock);
		return;
	}

	if (linkr_debugger_capture_should_trigger(&LINKR_DEBUGGER_POWER_CAPTURE, sample)) {
		LINKR_DEBUGGER_POWER_CAPTURE.trigger_device_t_mono_us = (uint64_t)sample->uptime_us;
		LINKR_DEBUGGER_POWER_CAPTURE.trigger_sample_sequence = sample->sequence;
		LINKR_DEBUGGER_POWER_CAPTURE.trigger_notice_pending = true;
		LINKR_DEBUGGER_POWER_CAPTURE.state = LINKR_DEBUGGER_CAPTURE_TRIGGERED;
		wake_session_id = linkr_debugger_capture_owner_session_id;
	}
	k_mutex_unlock(&linkr_debugger_capture_lock);

	if (wake_session_id != 0U) {
		linkr_debugger_ws_wake_session(wake_session_id, LINKR_DEBUGGER_WS_EVENT_SAMPLE);
	}
}

void linkr_debugger_ws_sigrok_telemetry_pause_acquire(void)
{
	k_mutex_lock(&linkr_debugger_ws_sample_ring_lock, K_FOREVER);
	linkr_debugger_ws_sigrok_telemetry_pause_requested = true;
	k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);
	k_event_post(&linkr_debugger_ws_sampler_events,
		LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
}

void linkr_debugger_ws_sigrok_telemetry_pause_release(void)
{
	k_mutex_lock(&linkr_debugger_ws_sample_ring_lock, K_FOREVER);
	linkr_debugger_ws_sigrok_telemetry_pause_requested = false;
	k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);
	k_event_post(&linkr_debugger_ws_sampler_events,
		LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
}

static bool linkr_debugger_ws_adc_sampler_pause_if_requested(void)
{
	bool pause_requested;
	bool resumed;
	uint32_t pause_generation;

	k_mutex_lock(&linkr_debugger_ws_sample_ring_lock, K_FOREVER);
	pause_requested = linkr_debugger_ws_sampler_pause_requested;
	pause_generation = linkr_debugger_ws_sampler_sync.pause_generation;
	k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);
	if (!pause_requested) {
		return false;
	}

	k_sem_give(&linkr_debugger_ws_sampler_pause_ack);
	do {
		k_mutex_lock(&linkr_debugger_ws_sample_ring_lock, K_FOREVER);
		resumed = linkr_debugger_ws_sampler_sync_is_resumed(
			&linkr_debugger_ws_sampler_sync, pause_generation);
		k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);
		if (!resumed) {
			(void)k_event_wait_safe(&linkr_debugger_ws_sampler_events,
				LINKR_DEBUGGER_WS_SAMPLER_EVENT_RESUME, false, K_FOREVER);
		}
	} while (!resumed);
	return true;
}

static void linkr_debugger_ws_store_adc_samples(
	const struct linkr_debugger_current_sample
		readings[][LINKR_DEBUGGER_WS_TELEMETRY_ADC_CHANNELS],
	const int64_t *timestamps_us,
	size_t sample_count,
	int rate_hz)
{
	for (size_t i = 0; i < sample_count; i++) {
		struct linkr_debugger_ws_adc_sample *sample;

		k_mutex_lock(&linkr_debugger_ws_sample_ring_lock, K_FOREVER);
		linkr_debugger_ws_latest_sample_sequence++;
		sample = &linkr_debugger_ws_sample_ring[
			(linkr_debugger_ws_latest_sample_sequence - 1U) %
			LINKR_DEBUGGER_WS_SAMPLE_RING_SIZE];
		memcpy(sample->readings, readings[i], sizeof(sample->readings));
		sample->sequence = linkr_debugger_ws_latest_sample_sequence;
		sample->uptime_us = timestamps_us[i];
		sample->source_rate_hz = rate_hz;
		linkr_debugger_ws_sampler_workspace.ingest_sample = *sample;
		k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);

		linkr_debugger_capture_ingest_sample(
			&linkr_debugger_ws_sampler_workspace.ingest_sample);
	}
}

static void linkr_debugger_adc_sampler_thread(void *p1, void *p2, void *p3)
{
	const int64_t tick_us = k_ticks_to_us_ceil64(1);

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		int rate_hz;
		uint32_t period_us;
		size_t sample_count;
		int ret;
		bool telemetry_paused;

		if (linkr_debugger_ws_adc_sampler_pause_if_requested()) {
			continue;
		}

		k_mutex_lock(&linkr_debugger_ws_sample_ring_lock, K_FOREVER);
		telemetry_paused = linkr_debugger_ws_sigrok_telemetry_pause_requested;
		k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);
		if (telemetry_paused) {
			(void)k_event_wait_safe(&linkr_debugger_ws_sampler_events,
					   LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG,
					   false, K_MSEC(50));
			continue;
		}

		rate_hz = linkr_debugger_ws_requested_sample_rate();

		if (rate_hz == 0) {
			(void)k_event_wait_safe(&linkr_debugger_ws_sampler_events,
					   LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG,
					   false, K_FOREVER);
			continue;
		}

		period_us = DIV_ROUND_UP(1000000, rate_hz);
		if (rate_hz <= 100) {
			uint32_t events = k_event_wait_safe(&linkr_debugger_ws_sampler_events,
						       LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG,
						       false, K_USEC(period_us));

			if (events != 0U) {
				continue;
			}
			sample_count = 1U;
		} else {
			sample_count = MIN(LINKR_DEBUGGER_CURRENT_BATCH_MAX,
					   DIV_ROUND_UP((uint32_t)rate_hz, 50U));
		}

		ret = linkr_debugger_current_read_batch(
			&linkr_debugger_ws_sampler_workspace.readings[0][0], sample_count,
			linkr_debugger_adc_count,
			linkr_debugger_ws_sampler_workspace.timestamps_us,
			sample_count > 1U ? period_us : 0U);
		if (ret < 0) {
			LOG_ERR("ADC batch read failed: %d", ret);
			k_msleep(10);
			continue;
		}
		linkr_debugger_ws_store_adc_samples(
			linkr_debugger_ws_sampler_workspace.readings,
			linkr_debugger_ws_sampler_workspace.timestamps_us,
			sample_count, rate_hz);
		/* Batch subscribers are intentionally timer-driven. Waking them for
		 * every ADC sample (or every hardware batch) makes each wake emit a
		 * partial JSON frame, defeating batch_size and saturating USB-NCM.
		 * Single-sample subscribers still get the lowest possible latency.
		 */
		linkr_debugger_ws_publish_unbatched_sample();
		if (sample_count > 1U) {
			int64_t now_us;
			int64_t next_batch_us;
			uint32_t events = 0U;

			next_batch_us = linkr_debugger_ws_sampler_workspace
				.timestamps_us[sample_count - 1U] + period_us;
			now_us = k_ticks_to_us_floor64(k_uptime_ticks());
			if (now_us < next_batch_us - tick_us) {
				events = k_event_wait_safe(&linkr_debugger_ws_sampler_events,
						      LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG,
						      false,
						      K_USEC(next_batch_us - now_us - tick_us));
			}
			if (events == 0U) {
				now_us = k_ticks_to_us_floor64(k_uptime_ticks());
				if (now_us < next_batch_us) {
					k_busy_wait((uint32_t)(next_batch_us - now_us));
				}
			}
		}
	}
}

static uint64_t linkr_debugger_ws_latest_sequence(void)
{
	uint64_t sequence;

	k_mutex_lock(&linkr_debugger_ws_sample_ring_lock, K_FOREVER);
	sequence = linkr_debugger_ws_latest_sample_sequence;
	k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);

	return sequence;
}

static bool linkr_debugger_ws_sample_get(struct linkr_debugger_ws_client *client,
					 struct linkr_debugger_ws_adc_sample *out_sample,
					 uint32_t *dropped_samples)
{
	uint32_t period_us = DIV_ROUND_UP(1000000U,
					   (uint32_t)client->telemetry_rate_hz);
	uint64_t latest;
	uint64_t oldest;

	*dropped_samples = 0U;
	k_mutex_lock(&linkr_debugger_ws_sample_ring_lock, K_FOREVER);
	if (!linkr_debugger_capture_arena_ws_sample_read_allowed(
	    linkr_debugger_ws_sampler_pause_requested,
	    linkr_debugger_ws_arena_quiesced)) {
		k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);
		return false;
	}
	latest = linkr_debugger_ws_latest_sample_sequence;
	if (latest == 0U) {
		k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);
		return false;
	}

	oldest = latest >= LINKR_DEBUGGER_WS_SAMPLE_RING_SIZE ?
		 latest - LINKR_DEBUGGER_WS_SAMPLE_RING_SIZE + 1U : 1U;
	if (client->next_sample_sequence < oldest) {
		*dropped_samples = (uint32_t)(oldest - client->next_sample_sequence);
		client->next_sample_sequence = oldest;
	}

	while (client->next_sample_sequence <= latest) {
		const struct linkr_debugger_ws_adc_sample *sample =
			&linkr_debugger_ws_sample_ring[(client->next_sample_sequence - 1U) %
						       LINKR_DEBUGGER_WS_SAMPLE_RING_SIZE];
		bool use_all_samples = client->telemetry_rate_hz >= sample->source_rate_hz;

		if (use_all_samples || client->next_sample_due_us == 0 ||
		    sample->uptime_us >= client->next_sample_due_us) {
			*out_sample = *sample;
			if (!use_all_samples && client->next_sample_due_us == 0) {
				client->next_sample_due_us = sample->uptime_us;
			}
			if (!use_all_samples) {
				do {
					client->next_sample_due_us += period_us;
				} while (client->next_sample_due_us <= sample->uptime_us);
			}
			client->next_sample_sequence++;
			k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);
			return true;
		}
		client->next_sample_sequence++;
	}
	k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);

	return false;
}

static const char *linkr_debugger_ws_adc_kind_name(
	const struct linkr_debugger_current_desc *adc)
{
	switch (adc->kind) {
	case LINKR_DEBUGGER_ADC_KIND_CURRENT:
		return "current";
	case LINKR_DEBUGGER_ADC_KIND_VOLTAGE:
		return "voltage";
	default:
		return "";
	}
}

static int32_t linkr_debugger_ws_adc_sample_value(
	const struct linkr_debugger_current_desc *adc,
	const struct linkr_debugger_current_sample *sample)
{
	switch (adc->kind) {
	case LINKR_DEBUGGER_ADC_KIND_CURRENT:
		return sample->current_ua;
	case LINKR_DEBUGGER_ADC_KIND_VOLTAGE:
		return (int32_t)(((int64_t)sample->value.val1 * 1000000LL) + sample->value.val2);
	default:
		return 0;
	}
}

static int linkr_debugger_ws_append_adc_readings(char *buf, size_t size, size_t *cursor,
					  const struct linkr_debugger_current_sample *readings)
{
	for (size_t i = 0; i < linkr_debugger_adc_count; i++) {
		const struct linkr_debugger_current_desc *adc = &linkr_debugger_currents[i];
		const struct linkr_debugger_current_sample *sample = &readings[i];
		int32_t value = linkr_debugger_ws_adc_sample_value(adc, sample);

		if (i > 0U && linkr_debugger_ws_append(buf, size, cursor, ",") < 0) {
			return -ENOMEM;
		}
		if (linkr_debugger_ws_append(buf, size, cursor,
					 "{\"name\":\"%s\",\"signal\":\"%s\",\"kind\":\"%s\","
					 "\"unit\":\"%s\"",
					 adc->name, adc->signal, linkr_debugger_ws_adc_kind_name(adc), adc->unit) < 0) {
			return -ENOMEM;
		}
		switch (adc->kind) {
		case LINKR_DEBUGGER_ADC_KIND_CURRENT:
			if (linkr_debugger_ws_append(buf, size, cursor, ",\"power_enabled\":%s",
						     sample->rail_enabled ? "true" : "false") < 0) {
				return -ENOMEM;
			}
			break;
		case LINKR_DEBUGGER_ADC_KIND_VOLTAGE:
			break;
		default:
			return -EINVAL;
		}
		if (linkr_debugger_ws_append(buf, size, cursor, ",\"value\":%d}", value) < 0) {
			return -ENOMEM;
		}
	}

	return 0;
}

static int linkr_debugger_ws_emit_adc_sample(struct linkr_debugger_ws_client *client)
{
	char *buf = (char *)client->tx_buffer;
	struct linkr_debugger_ws_adc_sample sample;
	uint32_t dropped_samples;
	size_t cursor = 0U;

	if (!linkr_debugger_ws_sample_get(client, &sample, &dropped_samples)) {
		return 0;
	}
	client->telemetry_dropped_samples += dropped_samples;
	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
				 "{\"type\":\"telemetry\",\"topic\":\"adc\","
				 "\"schema\":\"%s\",\"sequence\":%llu,\"uptime_us\":%lld,"
				 "\"sample_sequence\":%llu,\"device_t_mono_us\":%lld",
				 linkr_debugger_json_schema(),
				 (unsigned long long)sample.sequence,
				 (long long)sample.uptime_us,
				 (unsigned long long)sample.sequence,
				 (long long)sample.uptime_us) < 0) {
		return -ENOMEM;
	}
	if (dropped_samples > 0U &&
	    linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
				     ",\"dropped_samples\":%u", dropped_samples) < 0) {
		return -ENOMEM;
	}
	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
				 ",\"readings\":[") < 0 ||
	    linkr_debugger_ws_append_adc_readings(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
					     &cursor, sample.readings) < 0 ||
	    linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor, "]}") < 0) {
		return -ENOMEM;
	}

	return linkr_debugger_ws_send_json(client, buf);
}

static int linkr_debugger_ws_emit_adc_batch(struct linkr_debugger_ws_client *client)
{
	char *buf = (char *)client->tx_buffer;
	uint32_t total_dropped_samples = 0U;
	size_t dropped_samples_offset = 0U;
	uint8_t sample_count = 0U;
	size_t cursor = 0U;

	while (sample_count < client->telemetry_batch_size) {
		struct linkr_debugger_ws_adc_sample sample;
		uint32_t dropped_samples;

		if (!linkr_debugger_ws_sample_get(client, &sample, &dropped_samples)) {
			break;
		}
		total_dropped_samples += dropped_samples;

		if (sample_count == 0U) {
			if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
						 "{\"type\":\"telemetry-batch\",\"topic\":\"adc\","
						 "\"schema\":\"%s\",\"dropped_samples\":",
						 linkr_debugger_json_schema()) < 0) {
				return -ENOMEM;
			}
			dropped_samples_offset = cursor;
			if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
						 "0000000000,\"channels\":[") < 0) {
				return -ENOMEM;
			}
			for (size_t i = 0; i < linkr_debugger_adc_count; i++) {
				const struct linkr_debugger_current_desc *adc = &linkr_debugger_currents[i];

				if (i > 0U && linkr_debugger_ws_append(buf,
								    LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
								    &cursor, ",") < 0) {
					return -ENOMEM;
				}
				if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
							 &cursor,
							 "{\"name\":\"%s\",\"signal\":\"%s\",\"kind\":\"%s\","
							 "\"unit\":\"%s\"}",
							 adc->name, adc->signal, linkr_debugger_ws_adc_kind_name(adc),
							 adc->unit) < 0) {
					return -ENOMEM;
				}
			}
			if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
						 &cursor, "],\"samples\":[") < 0) {
				return -ENOMEM;
			}
		}

		if (sample_count > 0U && linkr_debugger_ws_append(buf,
							      LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
							      &cursor, ",") < 0) {
			return -ENOMEM;
		}
		uint8_t power_enabled_mask = 0U;

		for (size_t current_index = 0; current_index < linkr_debugger_current_count;
		     current_index++) {
			if (sample.readings[current_index].rail_enabled) {
				power_enabled_mask |= BIT(current_index);
			}
		}
		if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
					 "{\"sequence\":%llu,\"uptime_us\":%lld,\"power_enabled_mask\":%u,"
					 "\"values\":[",
					 (unsigned long long)sample.sequence,
					 (long long)sample.uptime_us,
					 (unsigned int)power_enabled_mask) < 0) {
			return -ENOMEM;
		}
		for (size_t j = 0; j < linkr_debugger_adc_count; j++) {
			const struct linkr_debugger_current_desc *adc = &linkr_debugger_currents[j];
			const struct linkr_debugger_current_sample *reading = &sample.readings[j];

			if (j > 0U && linkr_debugger_ws_append(buf,
							    LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
							    &cursor, ",") < 0) {
				return -ENOMEM;
			}
			if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
						     &cursor, "%d",
						     linkr_debugger_ws_adc_sample_value(adc, reading)) < 0) {
				return -ENOMEM;
			}
		}
		if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
					     &cursor, "]}") < 0) {
			return -ENOMEM;
		}
		sample_count++;
	}
	if (sample_count == 0U) {
		return 0;
	}
	client->telemetry_dropped_samples += total_dropped_samples;
	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
				     &cursor, "]}") < 0) {
		return -ENOMEM;
	}
	{
		char dropped_buf[11];
		int dropped_len = snprintk(dropped_buf, sizeof(dropped_buf), "%u",
						 total_dropped_samples);

		if (dropped_len < 0 || dropped_len >= (int)sizeof(dropped_buf)) {
			return -ENOMEM;
		}
		memmove(buf + dropped_samples_offset + dropped_len,
			buf + dropped_samples_offset + 10U,
			cursor - dropped_samples_offset - 10U + 1U);
		memcpy(buf + dropped_samples_offset, dropped_buf, (size_t)dropped_len);
		cursor = cursor - 10U + (size_t)dropped_len;
	}

	return linkr_debugger_ws_send_json(client, buf);
}

static bool linkr_debugger_ws_client_has_capture_pending(const struct linkr_debugger_ws_client *client)
{
	bool pending;

	k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
	pending = linkr_debugger_capture_owner_session_id == client->session_id &&
		  LINKR_DEBUGGER_POWER_CAPTURE.trigger_notice_pending;
	k_mutex_unlock(&linkr_debugger_capture_lock);

	return pending;
}

static int linkr_debugger_ws_emit_capture_triggered(struct linkr_debugger_ws_client *client)
{
	char *buf = (char *)client->tx_buffer;
	uint32_t capture_id;
	uint64_t device_t_mono_us;
	uint64_t sample_sequence;
	int ret;

	k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
	if (linkr_debugger_capture_owner_session_id != client->session_id ||
	    !LINKR_DEBUGGER_POWER_CAPTURE.trigger_notice_pending) {
		k_mutex_unlock(&linkr_debugger_capture_lock);
		return 0;
	}
	capture_id = LINKR_DEBUGGER_POWER_CAPTURE.capture_id;
	device_t_mono_us = LINKR_DEBUGGER_POWER_CAPTURE.trigger_device_t_mono_us;
	sample_sequence = LINKR_DEBUGGER_POWER_CAPTURE.trigger_sample_sequence;
	k_mutex_unlock(&linkr_debugger_capture_lock);

	snprintk(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
		 "{\"type\":\"capture_triggered\",\"schema\":\"%s\",\"capture_id\":%u,"
		 "\"device_t_mono_us\":%llu,\"sample_sequence\":%llu,"
		 "\"dropped_samples\":%llu}",
		 linkr_debugger_json_schema(), (unsigned int)capture_id,
		 (unsigned long long)device_t_mono_us,
		 (unsigned long long)sample_sequence,
		 (unsigned long long)client->telemetry_dropped_samples);
	ret = linkr_debugger_ws_send_json(client, buf);
	if (ret < 0) {
		return ret;
	}

	/* Keep the notice pending until it has actually reached the owner. */
	k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
	if (linkr_debugger_capture_owner_session_id == client->session_id &&
	    LINKR_DEBUGGER_POWER_CAPTURE.capture_id == capture_id) {
		LINKR_DEBUGGER_POWER_CAPTURE.trigger_notice_pending = false;
	}
	k_mutex_unlock(&linkr_debugger_capture_lock);

	return 0;
}

static int linkr_debugger_ws_emit_capture_pending(struct linkr_debugger_ws_client *client)
{
	return linkr_debugger_ws_emit_capture_triggered(client);
}

static int linkr_debugger_ws_emit_error(struct linkr_debugger_ws_client *client,
				    const char *command,
				    const char *code,
				    const char *message)
{
	char *buf = (char *)client->tx_buffer;

	snprintk(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
		 "{\"type\":\"error\",\"schema\":\"%s\",\"command\":\"%s\","
		 "\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
		 linkr_debugger_json_schema(), command, code, message);
	return linkr_debugger_ws_send_json(client, buf);
}

static int linkr_debugger_ws_emit_result(struct linkr_debugger_ws_client *client,
				     const char *id,
				     const char *command,
				     const char *status)
{
	char *buf = (char *)client->tx_buffer;

	snprintk(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
		 "{\"type\":\"result\",\"schema\":\"%s\",\"command\":\"%s\","
		 "\"id\":\"%s\",\"ok\":true,\"status\":\"%s\"}",
		 linkr_debugger_json_schema(), command, id != NULL ? id : "", status);
	return linkr_debugger_ws_send_json(client, buf);
}

static int linkr_debugger_ws_emit_result_and_snapshot(struct linkr_debugger_ws_client *client,
					      const char *id,
					      const char *command,
					      const char *status)
{
	int ret;

	ret = linkr_debugger_ws_emit_result(client, id, command, status);
	if (ret < 0) {
		return ret;
	}

	ret = linkr_debugger_ws_emit_status_snapshot(client);
	if (ret < 0) {
		LOG_ERR("failed to emit status snapshot after %s: %d", command, ret);
	}

	return ret;
}

static int linkr_debugger_ws_capture_arm(struct linkr_debugger_ws_client *client,
					 const struct linkr_debugger_ws_request *request)
{
	struct linkr_debugger_capture *capture = &LINKR_DEBUGGER_POWER_CAPTURE;
	int rate_hz = request->rate_hz > 0 ? request->rate_hz : 100;
	const char *trigger = request->trigger[0] != '\0' ? request->trigger : "manual";

	if (strcmp(request->mode, linkr_debugger_power_capture_protocol()) != 0) {
		return linkr_debugger_ws_emit_error(
			client, "capture", "unsupported_capture_protocol",
			"capture_arm requires mode=host-stream-v1; update the Web UI or firmware");
	}

	if (rate_hz < 1 || rate_hz > 1000) {
		return linkr_debugger_ws_emit_error(client, "capture", "invalid_rate",
						"rate_hz must be between 1 and 1000");
	}
	if (request->pre_samples < 0 || request->post_samples < 0) {
		return linkr_debugger_ws_emit_error(client, "capture", "invalid_sample_count",
						"pre_samples and post_samples must be zero or greater");
	}
	if (strcmp(trigger, "manual") != 0 && strcmp(trigger, "current") != 0 &&
	    strcmp(trigger, "gpio") != 0 && strcmp(trigger, "power_on") != 0) {
		return linkr_debugger_ws_emit_error(client, "capture", "invalid_trigger",
						"trigger must be manual, current, gpio, or power_on");
	}
	if ((strcmp(trigger, "current") == 0 || strcmp(trigger, "power_on") == 0) &&
	    linkr_debugger_find_current(request->output) == NULL) {
		return linkr_debugger_ws_emit_error(client, "capture", "invalid_source",
						"output must name a current-monitored power rail");
	}
	if (strcmp(trigger, "current") == 0 && request->threshold_ua < 0) {
		return linkr_debugger_ws_emit_error(client, "capture", "invalid_threshold",
						"threshold_ua must be zero or greater");
	}
	if (strcmp(trigger, "gpio") == 0 &&
	    linkr_debugger_find_safe_gpio_by_identifier(request->gpio) == NULL) {
		return linkr_debugger_ws_emit_error(client, "capture", "invalid_source",
						"gpio must name an allowlisted GPIO");
	}
	if (strcmp(trigger, "gpio") == 0 && request->edge[0] != '\0' &&
	    strcmp(request->edge, "rising") != 0 && strcmp(request->edge, "falling") != 0 &&
	    strcmp(request->edge, "either") != 0) {
		return linkr_debugger_ws_emit_error(client, "capture", "invalid_edge",
						"edge must be rising, falling, or either");
	}

	k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
	if (linkr_debugger_capture_owner_session_id != 0U ||
	    capture->state != LINKR_DEBUGGER_CAPTURE_IDLE) {
		k_mutex_unlock(&linkr_debugger_capture_lock);
		return linkr_debugger_ws_emit_error(client, "capture", "capture_busy",
						"cancel or finish the active capture first");
	}

	memset(capture, 0, sizeof(*capture));
	linkr_debugger_capture_owner_session_id = client->session_id;
	capture->state = LINKR_DEBUGGER_CAPTURE_ARMED;
	capture->capture_id = linkr_debugger_next_capture_id++;
	capture->rate_hz = (uint16_t)rate_hz;
	capture->threshold_ua = request->threshold_ua;
	strncpy(capture->trigger, trigger, sizeof(capture->trigger) - 1U);
	strncpy(capture->edge, request->edge[0] != '\0' ? request->edge : "either",
		sizeof(capture->edge) - 1U);
	strncpy(capture->source,
		strcmp(trigger, "gpio") == 0 ? request->gpio : request->output,
		sizeof(capture->source) - 1U);

	if (strcmp(trigger, "gpio") == 0) {
		const struct linkr_debugger_safe_gpio_desc *gpio =
			linkr_debugger_find_safe_gpio_by_identifier(capture->source);
		int value = 0;
		(void)linkr_debugger_gpio_get(gpio, &value);
		capture->last_level = value > 0;
	} else if (strcmp(trigger, "power_on") == 0) {
		int index = linkr_debugger_ws_current_index(capture->source);
		const struct linkr_debugger_current_desc *current = &linkr_debugger_currents[index];
		const struct linkr_debugger_rail_desc *rail = linkr_debugger_find_rail(current->name);
		capture->last_level = rail != NULL && linkr_debugger_power_output_enabled(rail);
	}

	k_mutex_unlock(&linkr_debugger_capture_lock);
	k_event_post(&linkr_debugger_ws_sampler_events,
		     LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
	return linkr_debugger_ws_emit_result(client, request->id, "capture_arm", "armed");
}

static int linkr_debugger_ws_handle_control_message(struct linkr_debugger_ws_client *client,
					       const struct linkr_debugger_ws_request *request)
{
	if (strcmp(request->command, "capture_arm") == 0) {
		return linkr_debugger_ws_capture_arm(client, request);
	}
	if (strcmp(request->command, "capture_trigger") == 0) {
		k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
		if (linkr_debugger_capture_owner_session_id != client->session_id ||
		    LINKR_DEBUGGER_POWER_CAPTURE.state != LINKR_DEBUGGER_CAPTURE_ARMED) {
			k_mutex_unlock(&linkr_debugger_capture_lock);
			return linkr_debugger_ws_emit_error(client, "capture", "not_armed",
							"capture is not armed");
		}
		LINKR_DEBUGGER_POWER_CAPTURE.trigger_pending = true;
		k_mutex_unlock(&linkr_debugger_capture_lock);
		k_event_post(&linkr_debugger_ws_sampler_events,
			     LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
		return linkr_debugger_ws_emit_result(client, request->id, "capture_trigger", "triggered");
	}
	if (strcmp(request->command, "capture_stop") == 0) {
		k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
		if (linkr_debugger_capture_owner_session_id != client->session_id) {
			k_mutex_unlock(&linkr_debugger_capture_lock);
			return linkr_debugger_ws_emit_error(client, "capture_stop", "not_owner",
							"this session does not own the active capture");
		}
		if (LINKR_DEBUGGER_POWER_CAPTURE.state != LINKR_DEBUGGER_CAPTURE_TRIGGERED) {
			k_mutex_unlock(&linkr_debugger_capture_lock);
			return linkr_debugger_ws_emit_error(client, "capture_stop", "not_recording",
							"capture has not triggered");
		}
		memset(&LINKR_DEBUGGER_POWER_CAPTURE, 0, sizeof(LINKR_DEBUGGER_POWER_CAPTURE));
		linkr_debugger_capture_owner_session_id = 0U;
		k_mutex_unlock(&linkr_debugger_capture_lock);

		k_event_post(&linkr_debugger_ws_sampler_events,
			     LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
		return linkr_debugger_ws_emit_result(client, request->id, "capture_stop", "stopped");
	}
	if (strcmp(request->command, "capture_cancel") == 0) {
		k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
		if (linkr_debugger_capture_owner_session_id != client->session_id) {
			k_mutex_unlock(&linkr_debugger_capture_lock);
			return linkr_debugger_ws_emit_error(client, "capture", "not_owner",
							"this session does not own the active capture");
		}
		memset(&LINKR_DEBUGGER_POWER_CAPTURE, 0, sizeof(LINKR_DEBUGGER_POWER_CAPTURE));
		linkr_debugger_capture_owner_session_id = 0U;
		k_mutex_unlock(&linkr_debugger_capture_lock);
		k_event_post(&linkr_debugger_ws_sampler_events,
			     LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
		return linkr_debugger_ws_emit_result(client, request->id, "capture_cancel", "cancelled");
	}

	if (strcmp(request->command, "power_set") == 0) {
		const struct linkr_debugger_rail_desc *output = linkr_debugger_find_rail(request->output);
		bool enabled;
		int ret;

		if (output == NULL) {
			return linkr_debugger_ws_emit_error(client, "power", "unknown_power_output",
						"unknown power output");
		}
		if (!linkr_debugger_parse_bool_arg(request->state, &enabled)) {
			return linkr_debugger_ws_emit_error(client, "power", "invalid_state",
						"state must be on/off or 1/0");
		}
		ret = linkr_debugger_power_output_set(output, enabled);
		if (ret < 0) {
			return linkr_debugger_ws_emit_error(client, "power", "set_failed",
						"failed to set power output");
		}
		linkr_debugger_ws_publish_state_change();
		return linkr_debugger_ws_emit_result_and_snapshot(client, request->id, "power_set", "ok");
	}

	if (strcmp(request->command, "switch_route") == 0) {
		int ret;

		if (strcmp(request->output, "sd") == 0) {
			enum linkr_debugger_sd_route route;
			if (strcmp(request->route, "target") == 0) {
				route = LINKR_DEBUGGER_SD_ROUTE_TARGET;
			} else if (strcmp(request->route, "usb-reader") == 0 || strcmp(request->route, "reader") == 0) {
				route = LINKR_DEBUGGER_SD_ROUTE_USB_READER;
			} else {
				return linkr_debugger_ws_emit_error(client, "switch", "invalid_route",
						"sd route must be target or usb-reader");
			}
			ret = linkr_debugger_sd_route_set(route);
		} else if (strcmp(request->output, "usb") == 0) {
			enum linkr_debugger_usb_route route;
			if (strcmp(request->route, "pc") == 0) {
				route = LINKR_DEBUGGER_USB_ROUTE_PC;
			} else if (strcmp(request->route, "target") == 0) {
				route = LINKR_DEBUGGER_USB_ROUTE_TARGET;
			} else {
				return linkr_debugger_ws_emit_error(client, "switch", "invalid_route",
						"usb route must be pc or target");
			}
			ret = linkr_debugger_usb_route_set(route);
		} else if (strcmp(request->output, "tf_wp") == 0) {
			enum linkr_debugger_tf_wp_route route;
			if (!linkr_debugger_parse_tf_wp_route(request->route, &route)) {
				return linkr_debugger_ws_emit_error(client, "switch", "invalid_route",
						"tf_wp route must be writable or protected");
			}
			ret = linkr_debugger_tf_wp_route_set(route);
		} else if (strcmp(request->output, "vin") == 0 && linkr_debugger_vin_switch_available()) {
			enum linkr_debugger_vin_route route;
			if (!linkr_debugger_parse_vin_route(request->route, &route)) {
				return linkr_debugger_ws_emit_error(client, "switch", "invalid_route",
						"vin route must be 1.8v or 3.3v");
			}
			ret = linkr_debugger_vin_route_set(route);
		} else {
			return linkr_debugger_ws_emit_error(client, "switch", "unknown_switch",
					"switch target must be sd, usb, tf_wp, or an available vin switch");
		}

		if (ret < 0) {
			return linkr_debugger_ws_emit_error(client, "switch", "set_failed",
					"failed to set switch route");
		}
		linkr_debugger_ws_publish_state_change();
		return linkr_debugger_ws_emit_result_and_snapshot(client, request->id, "switch_route", "ok");
	}

	if (strcmp(request->command, "gpio_set") == 0) {
		const struct linkr_debugger_safe_gpio_desc *gpio;
		bool output_requested;
		int ret;

		gpio = linkr_debugger_find_safe_gpio_by_identifier(request->gpio);
		if (gpio == NULL) {
			return linkr_debugger_ws_emit_error(client, "gpio", "invalid_gpio",
					"GPIO target must be GP13, 13, or an allowlist note such as CON_MAS");
		}

		output_requested = strcmp(request->direction, "output") == 0;
		if (strcmp(request->direction, "input") == 0) {
			ret = linkr_debugger_gpio_set_input(gpio);
		} else if (output_requested) {
			ret = linkr_debugger_gpio_set_output(gpio, request->value != 0);
		} else {
			return linkr_debugger_ws_emit_error(client, "gpio", "invalid_request",
						"direction must be input or output");
		}

		if (ret < 0) {
			const struct linkr_debugger_gpio_error *gpio_error =
				linkr_debugger_gpio_configure_error(output_requested, ret);

			return linkr_debugger_ws_emit_error(client, "gpio", gpio_error->code,
							gpio_error->message);
		}

		linkr_debugger_ws_publish_state_change();
		return linkr_debugger_ws_emit_result_and_snapshot(client, request->id, "gpio_set", "ok");
	}

	if (strcmp(request->command, "bootloader") == 0) {
		(void)linkr_debugger_ws_emit_result(client, request->id, "bootloader", "ok");
		linkr_debugger_ws_publish_state_change();
		(void)linkr_debugger_bootloader_now();
		return 0;
	}

	if (strcmp(request->command, "watchdog_feed") == 0) {
		return linkr_debugger_ws_emit_error(client, "watchdog", "manual_feed_removed",
					"watchdog is supervised by firmware and cannot be fed manually");
	}

	return linkr_debugger_ws_emit_error(client, "ws", "unknown_command",
					"unknown websocket command");
}

static int linkr_debugger_ws_handle_message(struct linkr_debugger_ws_client *client,
				 const uint8_t *payload,
				 size_t payload_len)
{
	struct linkr_debugger_ws_request request = { 0 };
	int ret;

	ret = json_obj_parse((char *)payload, payload_len,
			     linkr_debugger_ws_request_descr,
			     ARRAY_SIZE(linkr_debugger_ws_request_descr), &request);
	if (ret < 0) {
		return linkr_debugger_ws_emit_error(client, "ws", "invalid_json",
					"invalid websocket JSON payload");
	}

	if (strcmp(request.type, "subscribe") == 0) {
		bool capture_active;
		int emit_ret;
		int telemetry_rate_hz;
		uint8_t telemetry_batch_size;
		uint64_t next_sample_sequence;

		if (request.topic[0] != '\0' && strcmp(request.topic, "live") != 0) {
			return linkr_debugger_ws_emit_error(client, "ws", "invalid_topic",
						"topic must be live");
		}
		k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
		capture_active = linkr_debugger_capture_owner_session_id == client->session_id &&
				 LINKR_DEBUGGER_POWER_CAPTURE.state != LINKR_DEBUGGER_CAPTURE_IDLE;
		k_mutex_unlock(&linkr_debugger_capture_lock);
		if (capture_active) {
			return linkr_debugger_ws_emit_error(client, "capture", "capture_active",
						"sampling rate cannot change during an active capture");
		}
		telemetry_rate_hz = request.rate_hz > 0 ? request.rate_hz : 10;
		if (telemetry_rate_hz > 1000) {
			telemetry_rate_hz = 1000;
		}
		telemetry_batch_size = (uint8_t)CLAMP(request.batch_size,
							      1,
							      LINKR_DEBUGGER_WS_MAX_BATCH_SIZE);
		k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
		client->telemetry_enabled = false;
		client->telemetry_rate_hz = telemetry_rate_hz;
		client->telemetry_batch_size = telemetry_batch_size;
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);
		emit_ret = linkr_debugger_ws_emit_result_and_snapshot(client, request.id,
							      "subscribe", "ok");
		if (emit_ret < 0) {
			return emit_ret;
		}
		next_sample_sequence = linkr_debugger_ws_latest_sequence() + 1U;
		k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
		client->next_sample_sequence = next_sample_sequence;
		client->telemetry_dropped_samples = 0U;
		client->next_sample_due_us = 0;
		client->telemetry_enabled = true;
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);
		k_event_post(&linkr_debugger_ws_sampler_events,
			     LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
		return 0;
	}

	if (strcmp(request.type, "unsubscribe") == 0) {
		k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
		client->telemetry_enabled = false;
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);
		k_event_post(&linkr_debugger_ws_sampler_events,
			     LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
		return linkr_debugger_ws_emit_result(client, request.id, "unsubscribe", "ok");
	}

	if (strcmp(request.type, "command") == 0) {
		return linkr_debugger_ws_handle_control_message(client, &request);
	}

	return linkr_debugger_ws_emit_error(client, "ws", "unknown_type",
					"unknown websocket message type");
}

static int sigrok_ws_stream_sink_lease(uint32_t sample_count,
	uint8_t bytes_per_sample, void *user_data,
	struct linkr_debugger_la_stream_sink_lease *lease);
static int sigrok_ws_stream_sink_commit(
	const struct linkr_debugger_la_stream_sink_commit *commit, void *user_data);
static void sigrok_ws_stream_sink_abort(void *token, void *user_data);
static void sigrok_ws_stream_sink_terminal(
	enum linkr_debugger_la_ring_poll_result status, uint32_t sequence,
	void *user_data);
static bool sigrok_ws_queue_event_at(struct linkr_debugger_ws_client *client,
	enum linkr_debugger_sigrok_linkr_event_type event_type,
	uint32_t sample_index);
static bool sigrok_ws_queue_event(struct linkr_debugger_ws_client *client,
	enum linkr_debugger_sigrok_linkr_event_type event_type);
static size_t sigrok_ws_stream_qbytes(const struct linkr_debugger_ws_client *client);

enum sigrok_ws_stream_item_kind {
	SIGROK_WS_STREAM_ITEM_FREE = 0,
	SIGROK_WS_STREAM_ITEM_PREFRAMED,
};

#define SIGROK_WS_STREAM_PAYLOAD_OFFSET \
	(LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES + \
	 LINKR_DEBUGGER_SIGROK_LINKR_DATA_META_BYTES)

BUILD_ASSERT(SIGROK_WS_STREAM_PAYLOAD_OFFSET +
	LINKR_DEBUGGER_LA_STREAM_MAX_PACKED_CHUNK_SAMPLES <=
	LINKR_DEBUGGER_WS_SIGROK_DATA_SLOT_BYTES,
	"Sigrok WS packed or dense SINGLE payload must fit in an existing DATA slot");

static void sigrok_ws_release_capture_if_held(struct linkr_debugger_ws_client *client)
{
	if (client == NULL || !client->sigrok_session.capture_owner_held) {
		return;
	}

	(void)linkr_debugger_logic_analyzer_stop_stream();
	linkr_debugger_ws_sigrok_burst_pool_abort(client->session_id,
		client->sigrok_stream_generation);
	(void)linkr_debugger_capture_arbiter_release(
		LINKR_DEBUGGER_CAPTURE_OWNER_SIGROK_LINKR);
	client->sigrok_session.capture_owner_held = false;
	if (client->sigrok_session.telemetry_pause_held) {
		linkr_debugger_ws_sigrok_telemetry_pause_release();
		client->sigrok_session.telemetry_pause_held = false;
	}
}

struct sigrok_ws_stream_queue_item {
	void *fifo_reserved;
	enum linkr_debugger_sigrok_linkr_ws_slot_state state;
	uint8_t kind;
	bool burst_slot;
	bool burst_inflight;
	uint32_t owner_session_id;
	uint32_t owner_generation;
	uint8_t *data;
	size_t capacity;
	size_t len;
};

struct sigrok_ws_stream_data_slot {
	struct sigrok_ws_stream_queue_item item;
	uint8_t storage[LINKR_DEBUGGER_WS_SIGROK_DATA_SLOT_BYTES] __aligned(4);
};

struct sigrok_ws_stream_terminal_slot {
	struct sigrok_ws_stream_queue_item item;
	uint8_t storage[LINKR_DEBUGGER_WS_SIGROK_TERMINAL_SLOT_BYTES] __aligned(4);
};

struct sigrok_ws_stream_slot_pool {
	struct k_spinlock lock;
	struct sigrok_ws_stream_data_slot data[LINKR_DEBUGGER_WS_SIGROK_DATA_SLOT_COUNT];
	struct sigrok_ws_stream_terminal_slot terminal;
	bool initialized;
};

struct sigrok_ws_burst_slot_pool {
	struct k_spinlock lock;
	struct k_sem free_sem;
	struct k_sem terminal_sem;
	struct sigrok_ws_stream_queue_item data[LINKR_DEBUGGER_WS_SIGROK_BURST_DATA_SLOT_COUNT];
	struct sigrok_ws_stream_queue_item terminal;
	uint32_t owner_session_id;
	uint32_t owner_generation;
	uint32_t source_generation;
	uint8_t open_leases;
	uint8_t inflight_frames;
	bool active;
	bool terminal_committed;
	bool source_decode_complete;
};

static struct sigrok_ws_burst_slot_pool sigrok_ws_burst_pool;

#define sigrok_ws_stream_pool \
	(*((struct sigrok_ws_stream_slot_pool *)linkr_debugger_capture_arena_sigrok_ws_pool()))

BUILD_ASSERT(LINKR_DEBUGGER_CAPTURE_ARENA_SIGROK_WS_POOL_BYTES ==
	sizeof(struct sigrok_ws_stream_slot_pool),
	"Sigrok WS stream pool arena slice size mismatch");

BUILD_ASSERT((sizeof(struct sigrok_ws_stream_queue_item) +
	LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
	LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES) < LINKR_DEBUGGER_WS_SIGROK_QBYTES_LIMIT,
	"Sigrok WS terminal events must always be small enough for the byte cap");

static size_t sigrok_ws_stream_item_alloc_bytes(
	const struct sigrok_ws_stream_queue_item *item)
{
	return item == NULL ? 0U : item->len;
}

static size_t sigrok_ws_stream_event_item_bytes(void)
{
	return LINKR_DEBUGGER_WS_SIGROK_TERMINAL_SLOT_BYTES;
}

static uint8_t *sigrok_ws_stream_payload(struct sigrok_ws_stream_queue_item *item)
{
	if (item == NULL || item->capacity <= SIGROK_WS_STREAM_PAYLOAD_OFFSET) {
		return NULL;
	}
	return item->data + SIGROK_WS_STREAM_PAYLOAD_OFFSET;
}

static size_t sigrok_ws_stream_payload_capacity(
	const struct sigrok_ws_stream_queue_item *item)
{
	return item == NULL || item->capacity <= SIGROK_WS_STREAM_PAYLOAD_OFFSET ?
		0U : item->capacity - SIGROK_WS_STREAM_PAYLOAD_OFFSET;
}

static size_t sigrok_ws_stream_encode_item(
	const struct sigrok_ws_stream_queue_item *item,
	uint8_t *out,
	size_t out_len)
{
	if (item == NULL || out == NULL || item->kind != SIGROK_WS_STREAM_ITEM_PREFRAMED ||
	    item->len == 0U || item->len > out_len) {
		return 0U;
	}
	memcpy(out, item->data, item->len);
	return item->len;
}

static void sigrok_ws_stream_pool_init_locked(void)
{
	if (sigrok_ws_stream_pool.initialized) {
		return;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(sigrok_ws_stream_pool.data); i++) {
		struct sigrok_ws_stream_queue_item *item = &sigrok_ws_stream_pool.data[i].item;

		memset(item, 0, sizeof(*item));
		item->state = LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE;
		item->kind = SIGROK_WS_STREAM_ITEM_FREE;
		item->burst_slot = false;
		item->data = sigrok_ws_stream_pool.data[i].storage;
		item->capacity = sizeof(sigrok_ws_stream_pool.data[i].storage);
	}

	memset(&sigrok_ws_stream_pool.terminal.item, 0,
		sizeof(sigrok_ws_stream_pool.terminal.item));
	sigrok_ws_stream_pool.terminal.item.state = LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE;
	sigrok_ws_stream_pool.terminal.item.kind = SIGROK_WS_STREAM_ITEM_FREE;
	sigrok_ws_stream_pool.terminal.item.burst_slot = false;
	sigrok_ws_stream_pool.terminal.item.data = sigrok_ws_stream_pool.terminal.storage;
	sigrok_ws_stream_pool.terminal.item.capacity = sizeof(sigrok_ws_stream_pool.terminal.storage);
	sigrok_ws_stream_pool.initialized = true;
}

static bool sigrok_ws_stream_pool_all_free_locked(void)
{
	sigrok_ws_stream_pool_init_locked();
	for (size_t i = 0U; i < ARRAY_SIZE(sigrok_ws_stream_pool.data); i++) {
		if (sigrok_ws_stream_pool.data[i].item.state !=
		    LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE) {
			return false;
		}
	}

	return sigrok_ws_stream_pool.terminal.item.state ==
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE;
}

static bool sigrok_ws_stream_pool_all_free(void)
{
	bool all_free;
	k_spinlock_key_t key = k_spin_lock(&sigrok_ws_stream_pool.lock);

	all_free = sigrok_ws_stream_pool_all_free_locked();
	k_spin_unlock(&sigrok_ws_stream_pool.lock, key);
	return all_free;
}

static void sigrok_ws_stream_pool_reinitialize_after_arena_resume(void)
{
	memset(&sigrok_ws_stream_pool, 0, sizeof(sigrok_ws_stream_pool));
	sigrok_ws_stream_pool_init_locked();
}

static uint32_t sigrok_ws_stream_pool_data_slots_used_locked(uint32_t session_id,
	uint32_t generation)
{
	uint32_t used = 0U;

	for (size_t i = 0U; i < ARRAY_SIZE(sigrok_ws_stream_pool.data); i++) {
		const struct sigrok_ws_stream_queue_item *item = &sigrok_ws_stream_pool.data[i].item;

		if (item->state != LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE &&
		    item->owner_session_id == session_id && item->owner_generation == generation) {
			used++;
		}
	}

	return used;
}

static void sigrok_ws_stream_slot_release_locked(struct sigrok_ws_stream_queue_item *item)
{
	if (item == NULL || item->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE) {
		return;
	}

	item->fifo_reserved = NULL;
	item->state = LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE;
	item->kind = SIGROK_WS_STREAM_ITEM_FREE;
	item->burst_slot = false;
	item->owner_session_id = 0U;
	item->owner_generation = 0U;
	item->len = 0U;
}

static void sigrok_ws_stream_pool_release_client_slots(struct linkr_debugger_ws_client *client)
{
	k_spinlock_key_t key;

	if (client == NULL) {
		return;
	}

	key = k_spin_lock(&sigrok_ws_stream_pool.lock);
	sigrok_ws_stream_pool_init_locked();
	for (size_t i = 0U; i < ARRAY_SIZE(sigrok_ws_stream_pool.data); i++) {
		struct sigrok_ws_stream_queue_item *item = &sigrok_ws_stream_pool.data[i].item;

		if (item->owner_session_id == client->session_id &&
		    item->state != LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED) {
			sigrok_ws_stream_slot_release_locked(item);
		}
	}
	if (sigrok_ws_stream_pool.terminal.item.owner_session_id == client->session_id &&
	    sigrok_ws_stream_pool.terminal.item.state != LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED) {
		sigrok_ws_stream_slot_release_locked(&sigrok_ws_stream_pool.terminal.item);
	}
	k_spin_unlock(&sigrok_ws_stream_pool.lock, key);
}

static bool sigrok_ws_burst_item_matches_locked(
	const struct sigrok_ws_stream_queue_item *item,
	uint32_t session_id,
	uint32_t generation)
{
	return item != NULL && item->burst_slot &&
		item->owner_session_id == session_id &&
		item->owner_generation == generation;
}

static void sigrok_ws_burst_pool_reset_items_locked(void)
{
	for (size_t i = 0U; i < ARRAY_SIZE(sigrok_ws_burst_pool.data); i++) {
		struct sigrok_ws_stream_queue_item *item = &sigrok_ws_burst_pool.data[i];

		memset(item, 0, sizeof(*item));
		item->state = LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE;
		item->kind = SIGROK_WS_STREAM_ITEM_FREE;
		item->burst_slot = true;
		item->data = linkr_debugger_capture_arena_burst_tx_slot((uint8_t)i);
		item->capacity = LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TX_SLOT_BYTES;
	}
	memset(&sigrok_ws_burst_pool.terminal, 0, sizeof(sigrok_ws_burst_pool.terminal));
	sigrok_ws_burst_pool.terminal.state = LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE;
	sigrok_ws_burst_pool.terminal.kind = SIGROK_WS_STREAM_ITEM_FREE;
	sigrok_ws_burst_pool.terminal.burst_slot = true;
	sigrok_ws_burst_pool.terminal.data = linkr_debugger_capture_arena_burst_terminal();
	sigrok_ws_burst_pool.terminal.capacity = LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TERMINAL_BYTES;
}

static void sigrok_ws_burst_pool_init(void)
{
	k_spinlock_key_t key;

	k_sem_init(&sigrok_ws_burst_pool.free_sem,
		LINKR_DEBUGGER_WS_SIGROK_BURST_DATA_SLOT_COUNT,
		LINKR_DEBUGGER_WS_SIGROK_BURST_DATA_SLOT_COUNT);
	k_sem_init(&sigrok_ws_burst_pool.terminal_sem, 0, 1);
	key = k_spin_lock(&sigrok_ws_burst_pool.lock);
	sigrok_ws_burst_pool_reset_items_locked();
	sigrok_ws_burst_pool.active = false;
	sigrok_ws_burst_pool.owner_session_id = 0U;
	sigrok_ws_burst_pool.owner_generation = 0U;
	sigrok_ws_burst_pool.source_generation = 0U;
	sigrok_ws_burst_pool.open_leases = 0U;
	sigrok_ws_burst_pool.inflight_frames = 0U;
	sigrok_ws_burst_pool.terminal_committed = false;
	sigrok_ws_burst_pool.source_decode_complete = false;
	k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
}

int linkr_debugger_ws_sigrok_burst_pool_begin(
	uint32_t session_id, uint32_t generation, uint32_t source_generation)
{
	k_spinlock_key_t key;

	if (session_id == 0U || generation == 0U || source_generation == 0U) {
		return -EINVAL;
	}
	key = k_spin_lock(&sigrok_ws_burst_pool.lock);
	if (sigrok_ws_burst_pool.active) {
		k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
		return -EBUSY;
	}
	sigrok_ws_burst_pool_reset_items_locked();
	sigrok_ws_burst_pool.active = true;
	sigrok_ws_burst_pool.owner_session_id = session_id;
	sigrok_ws_burst_pool.owner_generation = generation;
	sigrok_ws_burst_pool.source_generation = source_generation;
	sigrok_ws_burst_pool.open_leases = 0U;
	sigrok_ws_burst_pool.inflight_frames = 0U;
	sigrok_ws_burst_pool.terminal_committed = false;
	sigrok_ws_burst_pool.source_decode_complete = false;
	k_sem_reset(&sigrok_ws_burst_pool.free_sem);
	k_sem_reset(&sigrok_ws_burst_pool.terminal_sem);
	for (uint8_t i = 0U; i < LINKR_DEBUGGER_WS_SIGROK_BURST_DATA_SLOT_COUNT; i++) {
		k_sem_give(&sigrok_ws_burst_pool.free_sem);
	}
	k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
	return 0;
}

static bool sigrok_ws_burst_pool_active_for_client(
	const struct linkr_debugger_ws_client *client)
{
	bool active;
	k_spinlock_key_t key;

	if (client == NULL) {
		return false;
	}
	key = k_spin_lock(&sigrok_ws_burst_pool.lock);
	active = sigrok_ws_burst_pool.active &&
		sigrok_ws_burst_pool.owner_session_id == client->session_id &&
		sigrok_ws_burst_pool.owner_generation == client->sigrok_stream_generation;
	k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
	return active;
}

static struct sigrok_ws_stream_queue_item *sigrok_ws_burst_slot_acquire(
	struct linkr_debugger_ws_client *client, bool terminal)
{
	struct sigrok_ws_stream_queue_item *acquired = NULL;
	k_spinlock_key_t key;
	int64_t deadline;

	if (client == NULL || !sigrok_ws_burst_pool_active_for_client(client)) {
		return NULL;
	}
	deadline = k_uptime_get() + LINKR_DEBUGGER_WS_SIGROK_BURST_SLOT_WAIT_MS;

	if (!terminal) {
		if (k_sem_take(&sigrok_ws_burst_pool.free_sem,
		    K_MSEC(LINKR_DEBUGGER_WS_SIGROK_BURST_SLOT_WAIT_MS)) < 0) {
			return NULL;
		}
	} else {
		while (true) {
			bool ready;

			key = k_spin_lock(&sigrok_ws_burst_pool.lock);
			ready = sigrok_ws_burst_pool.active && client->connected &&
				sigrok_ws_burst_pool.owner_session_id == client->session_id &&
				sigrok_ws_burst_pool.owner_generation == client->sigrok_stream_generation &&
				!sigrok_ws_burst_pool.terminal_committed &&
				sigrok_ws_burst_pool.open_leases == 0U &&
				sigrok_ws_burst_pool.terminal.state ==
				LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE;
			k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
			if (ready) {
				break;
			}
			if (!client->connected || !sigrok_ws_burst_pool_active_for_client(client) ||
			    k_uptime_get() >= deadline) {
				return NULL;
			}
			(void)k_sem_take(&sigrok_ws_burst_pool.terminal_sem, K_MSEC(1));
		}
	}

	key = k_spin_lock(&sigrok_ws_burst_pool.lock);
	if (!sigrok_ws_burst_pool.active ||
	    !client->connected || sigrok_ws_burst_pool.owner_session_id != client->session_id ||
	    sigrok_ws_burst_pool.owner_generation != client->sigrok_stream_generation) {
		k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
		if (!terminal) {
			k_sem_give(&sigrok_ws_burst_pool.free_sem);
		}
		return NULL;
	}
	if (terminal) {
		if (!sigrok_ws_burst_pool.terminal_committed &&
		    sigrok_ws_burst_pool.open_leases == 0U &&
		    sigrok_ws_burst_pool.terminal.state ==
		    LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE) {
			acquired = &sigrok_ws_burst_pool.terminal;
		}
	} else {
		for (size_t i = 0U; i < ARRAY_SIZE(sigrok_ws_burst_pool.data); i++) {
			struct sigrok_ws_stream_queue_item *item = &sigrok_ws_burst_pool.data[i];

			if (item->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE) {
				acquired = item;
				break;
			}
		}
	}
	if (acquired != NULL) {
		acquired->fifo_reserved = NULL;
		acquired->state = LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED;
		acquired->owner_session_id = client->session_id;
		acquired->owner_generation = client->sigrok_stream_generation;
		acquired->kind = SIGROK_WS_STREAM_ITEM_FREE;
		acquired->burst_slot = true;
		acquired->burst_inflight = false;
		acquired->len = 0U;
		sigrok_ws_burst_pool.open_leases++;
	} else if (!terminal) {
		k_sem_give(&sigrok_ws_burst_pool.free_sem);
	}
	k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
	return acquired;
}

static void sigrok_ws_burst_slot_release_locked(struct sigrok_ws_stream_queue_item *item)
{
	bool was_data;

	if (item == NULL || item->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE) {
		return;
	}
	was_data = item != &sigrok_ws_burst_pool.terminal;
	item->fifo_reserved = NULL;
	item->state = LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE;
	item->kind = SIGROK_WS_STREAM_ITEM_FREE;
	item->owner_session_id = 0U;
	item->owner_generation = 0U;
	item->len = 0U;
	if (item->burst_inflight) {
		if (sigrok_ws_burst_pool.inflight_frames > 0U) {
			sigrok_ws_burst_pool.inflight_frames--;
		}
		item->burst_inflight = false;
	} else if (sigrok_ws_burst_pool.open_leases > 0U) {
		sigrok_ws_burst_pool.open_leases--;
	}
	if (was_data) {
		k_sem_give(&sigrok_ws_burst_pool.free_sem);
	}
	k_sem_give(&sigrok_ws_burst_pool.terminal_sem);
}

static void sigrok_ws_burst_slot_release(struct sigrok_ws_stream_queue_item *item)
{
	k_spinlock_key_t key;

	if (item == NULL) {
		return;
	}
	key = k_spin_lock(&sigrok_ws_burst_pool.lock);
	sigrok_ws_burst_slot_release_locked(item);
	k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
}

static bool sigrok_ws_burst_commit_allowed_locked(
	const struct linkr_debugger_ws_client *client,
	const struct sigrok_ws_stream_queue_item *item)
{
	return client != NULL && item != NULL &&
		sigrok_ws_burst_pool.active &&
		sigrok_ws_burst_pool.owner_session_id == client->session_id &&
		sigrok_ws_burst_pool.owner_generation == client->sigrok_stream_generation &&
		sigrok_ws_burst_item_matches_locked(item, client->session_id,
			client->sigrok_stream_generation) &&
		item->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED;
}

bool linkr_debugger_ws_sigrok_burst_pool_source_done_and_drained(void)
{
	bool drained;
	k_spinlock_key_t key = k_spin_lock(&sigrok_ws_burst_pool.lock);

	drained = sigrok_ws_burst_pool.active &&
		sigrok_ws_burst_pool.source_decode_complete &&
		sigrok_ws_burst_pool.terminal_committed &&
		sigrok_ws_burst_pool.terminal.state ==
		LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE &&
		sigrok_ws_burst_pool.open_leases == 0U &&
		sigrok_ws_burst_pool.inflight_frames == 0U;
	k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
	return drained;
}

void linkr_debugger_ws_sigrok_burst_pool_mark_source_decode_complete(
	uint32_t source_generation)
{
	k_spinlock_key_t key = k_spin_lock(&sigrok_ws_burst_pool.lock);

	if (sigrok_ws_burst_pool.active &&
	    sigrok_ws_burst_pool.source_generation == source_generation) {
		sigrok_ws_burst_pool.source_decode_complete = true;
	}
	k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
}

void linkr_debugger_ws_sigrok_burst_pool_abort(uint32_t session_id, uint32_t generation)
{
	k_spinlock_key_t key = k_spin_lock(&sigrok_ws_burst_pool.lock);

	if (!sigrok_ws_burst_pool.active ||
	    sigrok_ws_burst_pool.owner_session_id != session_id ||
	    sigrok_ws_burst_pool.owner_generation != generation) {
		k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
		return;
	}
	sigrok_ws_burst_pool_reset_items_locked();
	sigrok_ws_burst_pool.active = false;
	sigrok_ws_burst_pool.owner_session_id = 0U;
	sigrok_ws_burst_pool.owner_generation = 0U;
	sigrok_ws_burst_pool.source_generation = 0U;
	sigrok_ws_burst_pool.open_leases = 0U;
	sigrok_ws_burst_pool.inflight_frames = 0U;
	sigrok_ws_burst_pool.terminal_committed = false;
	sigrok_ws_burst_pool.source_decode_complete = false;
	k_sem_reset(&sigrok_ws_burst_pool.free_sem);
	k_sem_reset(&sigrok_ws_burst_pool.terminal_sem);
	for (uint8_t i = 0U; i < LINKR_DEBUGGER_WS_SIGROK_BURST_DATA_SLOT_COUNT; i++) {
		k_sem_give(&sigrok_ws_burst_pool.free_sem);
	}
	k_sem_give(&sigrok_ws_burst_pool.terminal_sem);
	k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
}

static struct sigrok_ws_stream_queue_item *sigrok_ws_stream_slot_acquire(
	struct linkr_debugger_ws_client *client, bool terminal)
{
	struct sigrok_ws_stream_queue_item *acquired = NULL;
	k_spinlock_key_t key;

	if (client == NULL || client->session_id == 0U) {
		return NULL;
	}
	if (sigrok_ws_burst_pool_active_for_client(client)) {
		return sigrok_ws_burst_slot_acquire(client, terminal);
	}

	key = k_spin_lock(&sigrok_ws_stream_pool.lock);
	sigrok_ws_stream_pool_init_locked();
	if (terminal) {
		struct sigrok_ws_stream_queue_item *item = &sigrok_ws_stream_pool.terminal.item;

		if (linkr_debugger_sigrok_linkr_ws_pool_terminal_has_capacity(
		    item->state != LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE)) {
			acquired = item;
		}
	} else if (linkr_debugger_sigrok_linkr_ws_pool_data_has_capacity(
	    sigrok_ws_stream_pool_data_slots_used_locked(client->session_id,
		client->sigrok_stream_generation), false)) {
		for (size_t i = 0U; i < ARRAY_SIZE(sigrok_ws_stream_pool.data); i++) {
			struct sigrok_ws_stream_queue_item *item = &sigrok_ws_stream_pool.data[i].item;

			if (item->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_FREE) {
				acquired = item;
				break;
			}
		}
	}

	if (acquired != NULL) {
		acquired->fifo_reserved = NULL;
		acquired->state = LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED;
		acquired->owner_session_id = client->session_id;
		acquired->owner_generation = client->sigrok_stream_generation;
		acquired->kind = SIGROK_WS_STREAM_ITEM_FREE;
		acquired->len = 0U;
	}
	k_spin_unlock(&sigrok_ws_stream_pool.lock, key);
	return acquired;
}

static void sigrok_ws_stream_slot_release(struct sigrok_ws_stream_queue_item *item)
{
	k_spinlock_key_t key;

	if (item == NULL) {
		return;
	}
	if (item->burst_slot) {
		sigrok_ws_burst_slot_release(item);
		return;
	}

	key = k_spin_lock(&sigrok_ws_stream_pool.lock);
	sigrok_ws_stream_slot_release_locked(item);
	k_spin_unlock(&sigrok_ws_stream_pool.lock, key);
}

static void sigrok_ws_stream_wake_work_handler(struct k_work *work)
{
	struct k_work_delayable *delayable = k_work_delayable_from_work(work);
	struct linkr_debugger_ws_client *client = CONTAINER_OF(delayable,
		struct linkr_debugger_ws_client, sigrok_stream_wake_work);

	atomic_set(&client->sigrok_stream_deferred_wake_pending, 0);
	if (client->active && client->connected &&
	    atomic_get(&client->sigrok_stream_qdepth) > 0) {
		k_event_post(&client->events, LINKR_DEBUGGER_WS_EVENT_STREAM_DATA);
	}
}

static void sigrok_ws_stream_cancel_deferred_wake(struct linkr_debugger_ws_client *client)
{
	if (client == NULL) {
		return;
	}
	atomic_set(&client->sigrok_stream_deferred_wake_pending, 0);
	(void)k_work_cancel_delayable(&client->sigrok_stream_wake_work);
}

static void sigrok_ws_stream_cancel_deferred_wake_sync(struct linkr_debugger_ws_client *client)
{
	struct k_work_sync sync;

	if (client == NULL) {
		return;
	}
	atomic_set(&client->sigrok_stream_deferred_wake_pending, 0);
	(void)k_work_cancel_delayable_sync(&client->sigrok_stream_wake_work, &sync);
}

static void sigrok_ws_stream_apply_wake_policy(struct linkr_debugger_ws_client *client,
	bool urgent)
{
	uint32_t qdepth;
	enum linkr_debugger_sigrok_linkr_stream_wake_action action;

	if (client == NULL) {
		return;
	}
	qdepth = (uint32_t)atomic_get(&client->sigrok_stream_qdepth);
	action = linkr_debugger_sigrok_linkr_stream_wake_policy(qdepth, urgent,
		atomic_get(&client->sigrok_stream_deferred_wake_pending) != 0,
		LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_QDEPTH);
	switch (action) {
	case LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_NOW:
		sigrok_ws_stream_cancel_deferred_wake(client);
		k_event_post(&client->events, LINKR_DEBUGGER_WS_EVENT_STREAM_DATA);
		break;
	case LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_DELAY:
		if (atomic_cas(&client->sigrok_stream_deferred_wake_pending, 0, 1)) {
			(void)k_work_reschedule(&client->sigrok_stream_wake_work,
				K_MSEC(LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_TIMEOUT_MS));
		}
		break;
	case LINKR_DEBUGGER_SIGROK_LINKR_STREAM_WAKE_DEFER:
	default:
		break;
	}
}

static size_t sigrok_ws_stream_qbytes(const struct linkr_debugger_ws_client *client)
{
	atomic_val_t value = atomic_get(&client->sigrok_stream_qbytes);

	return value < 0 ? 0U : (size_t)value;
}

static void sigrok_ws_stream_metrics_reset(struct linkr_debugger_ws_client *client)
{
	k_spinlock_key_t key;

	if (client == NULL) {
		return;
	}
	key = k_spin_lock(&client->sigrok_stream_metrics_lock);
	linkr_debugger_sigrok_linkr_ws_transport_metrics_reset(&client->sigrok_stream_metrics);
	k_spin_unlock(&client->sigrok_stream_metrics_lock, key);
}

static void sigrok_ws_stream_metrics_snapshot(struct linkr_debugger_ws_client *client,
	struct linkr_debugger_sigrok_linkr_ws_transport_metrics *out)
{
	k_spinlock_key_t key;

	if (client == NULL || out == NULL) {
		return;
	}
	key = k_spin_lock(&client->sigrok_stream_metrics_lock);
	*out = client->sigrok_stream_metrics;
	k_spin_unlock(&client->sigrok_stream_metrics_lock, key);
}

static void sigrok_ws_stream_metrics_update_enqueue(struct linkr_debugger_ws_client *client,
	uint32_t qdepth, size_t qbytes)
{
	k_spinlock_key_t key;

	if (client == NULL) {
		return;
	}
	key = k_spin_lock(&client->sigrok_stream_metrics_lock);
	linkr_debugger_sigrok_linkr_ws_transport_metrics_update_enqueue(
		&client->sigrok_stream_metrics, qdepth, qbytes);
	k_spin_unlock(&client->sigrok_stream_metrics_lock, key);
}

static void sigrok_ws_stream_metrics_update_drain(struct linkr_debugger_ws_client *client,
	uint64_t duration_us, uint32_t items, size_t bytes)
{
	k_spinlock_key_t key;

	if (client == NULL) {
		return;
	}
	key = k_spin_lock(&client->sigrok_stream_metrics_lock);
	linkr_debugger_sigrok_linkr_ws_transport_metrics_update_drain(
		&client->sigrok_stream_metrics, duration_us, items, bytes);
	k_spin_unlock(&client->sigrok_stream_metrics_lock, key);
}

static void sigrok_ws_stream_metrics_update_send(struct linkr_debugger_ws_client *client,
	uint64_t duration_us, uint8_t frames, size_t bytes)
{
	k_spinlock_key_t key;

	if (client == NULL) {
		return;
	}
	key = k_spin_lock(&client->sigrok_stream_metrics_lock);
	linkr_debugger_sigrok_linkr_ws_transport_metrics_update_send(
		&client->sigrok_stream_metrics, duration_us, frames, bytes);
	k_spin_unlock(&client->sigrok_stream_metrics_lock, key);
}

static void sigrok_ws_stream_log_transport_metrics(struct linkr_debugger_ws_client *client,
	const char *reason, int status, uint32_t sequence)
{
	struct linkr_debugger_sigrok_linkr_ws_transport_metrics metrics;

	if (client == NULL) {
		return;
	}
	memset(&metrics, 0, sizeof(metrics));
	sigrok_ws_stream_metrics_snapshot(client, &metrics);
	LOG_WRN("sigrok ws transport metrics: reason=%s status=%d seq=%u qdepth=%ld "
		"qbytes=%u max_qdepth=%u max_qbytes=%u max_drain_us=%llu "
		"max_drain_items=%u max_drain_bytes=%u max_send_us=%llu "
		"max_send_frames=%u max_send_bytes=%u state=%d sample_index=%u emitted=%u",
		reason == NULL ? "unknown" : reason, status, sequence,
		(long)atomic_get(&client->sigrok_stream_qdepth),
		(unsigned int)sigrok_ws_stream_qbytes(client), metrics.max_qdepth,
		(unsigned int)metrics.max_qbytes,
		(unsigned long long)metrics.max_drain_us, metrics.max_drain_items,
		(unsigned int)metrics.max_drain_bytes,
		(unsigned long long)metrics.max_send_us, metrics.max_send_frames,
		(unsigned int)metrics.max_send_bytes, client->sigrok_session.state,
		client->sigrok_session.sample_index, client->sigrok_session.emitted_samples);
}

static void sigrok_ws_stream_qbytes_add(struct linkr_debugger_ws_client *client,
	size_t bytes)
{
	if (bytes > 0U) {
		(void)atomic_add(&client->sigrok_stream_qbytes, (atomic_val_t)bytes);
	}
}

static void sigrok_ws_stream_qbytes_sub(struct linkr_debugger_ws_client *client,
	size_t bytes)
{
	if (bytes == 0U) {
		return;
	}

	while (true) {
		atomic_val_t current = atomic_get(&client->sigrok_stream_qbytes);
		atomic_val_t next = current > (atomic_val_t)bytes ?
			current - (atomic_val_t)bytes : 0;

		if (atomic_cas(&client->sigrok_stream_qbytes, current, next)) {
			return;
		}
	}
}

static void sigrok_ws_stream_free_accounted_item(struct linkr_debugger_ws_client *client,
	struct sigrok_ws_stream_queue_item *item)
{
	ARG_UNUSED(client);

	if (item == NULL) {
		return;
	}
	sigrok_ws_stream_slot_release(item);
}

static bool sigrok_ws_stream_put_accounted_item(struct linkr_debugger_ws_client *client,
	struct sigrok_ws_stream_queue_item *item)
{
	k_spinlock_key_t key;
	bool allowed;

	if (client == NULL || item == NULL) {
		return false;
	}

	if (item->burst_slot) {
		key = k_spin_lock(&sigrok_ws_burst_pool.lock);
		allowed = sigrok_ws_burst_commit_allowed_locked(client, item);
		if (!allowed) {
			if (item->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED) {
				sigrok_ws_burst_slot_release_locked(item);
			}
			k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
			return false;
		}
		if (sigrok_ws_burst_pool.open_leases > 0U) {
			sigrok_ws_burst_pool.open_leases--;
		}
		item->burst_inflight = true;
		sigrok_ws_burst_pool.inflight_frames++;
		if (item == &sigrok_ws_burst_pool.terminal) {
			sigrok_ws_burst_pool.terminal_committed = true;
		}
		if (sigrok_ws_burst_pool.open_leases == 0U) {
			k_sem_give(&sigrok_ws_burst_pool.terminal_sem);
		}
		item->state = LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED;
		sigrok_ws_stream_qbytes_add(client, sigrok_ws_stream_item_alloc_bytes(item));
		atomic_inc(&client->sigrok_stream_qdepth);
		k_fifo_put(&client->sigrok_stream_fifo, item);
		sigrok_ws_stream_metrics_update_enqueue(client,
			(uint32_t)atomic_get(&client->sigrok_stream_qdepth),
			sigrok_ws_stream_qbytes(client));
		k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
		return true;
	}

	key = k_spin_lock(&sigrok_ws_stream_pool.lock);
	allowed = linkr_debugger_sigrok_linkr_ws_slot_commit_allowed(item->state,
		item->owner_session_id, item->owner_generation, client->session_id,
		client->sigrok_stream_generation);
	if (!allowed) {
		if (item->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED) {
			sigrok_ws_stream_slot_release_locked(item);
		}
		k_spin_unlock(&sigrok_ws_stream_pool.lock, key);
		return false;
	}
	item->state = LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED;
	sigrok_ws_stream_qbytes_add(client, sigrok_ws_stream_item_alloc_bytes(item));
	atomic_inc(&client->sigrok_stream_qdepth);
	k_fifo_put(&client->sigrok_stream_fifo, item);
	sigrok_ws_stream_metrics_update_enqueue(client,
		(uint32_t)atomic_get(&client->sigrok_stream_qdepth),
		sigrok_ws_stream_qbytes(client));
	k_spin_unlock(&sigrok_ws_stream_pool.lock, key);
	return true;
}

static struct sigrok_ws_stream_queue_item *sigrok_ws_stream_pop_accounted_item(
	struct linkr_debugger_ws_client *client)
{
	struct sigrok_ws_stream_queue_item *item;
	k_spinlock_key_t key;

	if (sigrok_ws_burst_pool_active_for_client(client)) {
		key = k_spin_lock(&sigrok_ws_burst_pool.lock);
		item = k_fifo_get(&client->sigrok_stream_fifo, K_NO_WAIT);
		if (item != NULL) {
			atomic_val_t qdepth = atomic_get(&client->sigrok_stream_qdepth);

			if (item->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED) {
				item->state = LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED;
			}
			sigrok_ws_stream_qbytes_sub(client, sigrok_ws_stream_item_alloc_bytes(item));
			if (qdepth > 0) {
				atomic_dec(&client->sigrok_stream_qdepth);
			} else {
				atomic_set(&client->sigrok_stream_qdepth, 0);
			}
		}
		k_spin_unlock(&sigrok_ws_burst_pool.lock, key);
		return item;
	}

	key = k_spin_lock(&sigrok_ws_stream_pool.lock);
	item = k_fifo_get(&client->sigrok_stream_fifo, K_NO_WAIT);
	if (item != NULL) {
		atomic_val_t qdepth = atomic_get(&client->sigrok_stream_qdepth);

		if (item->state == LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_QUEUED) {
			item->state = LINKR_DEBUGGER_SIGROK_LINKR_WS_SLOT_POPPED;
		}
		sigrok_ws_stream_qbytes_sub(client, sigrok_ws_stream_item_alloc_bytes(item));
		if (qdepth > 0) {
			atomic_dec(&client->sigrok_stream_qdepth);
		} else {
			atomic_set(&client->sigrok_stream_qdepth, 0);
		}
	}
	k_spin_unlock(&sigrok_ws_stream_pool.lock, key);
	return item;
}

static void sigrok_ws_stream_reset_queue(struct linkr_debugger_ws_client *client)
{
	struct sigrok_ws_stream_queue_item *item;
	uint32_t old_generation;

	if (client == NULL) {
		return;
	}
	sigrok_ws_stream_cancel_deferred_wake_sync(client);
	while ((item = sigrok_ws_stream_pop_accounted_item(client)) != NULL) {
		sigrok_ws_stream_free_accounted_item(client, item);
	}
	atomic_set(&client->sigrok_stream_qdepth, 0);
	atomic_set(&client->sigrok_stream_qbytes, 0);
	atomic_set(&client->sigrok_stream_dropped, 0);
	atomic_set(&client->sigrok_stream_stop_pending, 0);
	atomic_set(&client->sigrok_stream_abort_pending, 0);
	atomic_set(&client->sigrok_stream_deferred_wake_pending, 0);
	old_generation = client->sigrok_stream_generation;
	client->sigrok_stream_generation++;
	if (client->sigrok_stream_generation == 0U) {
		client->sigrok_stream_generation++;
	}
	sigrok_ws_stream_pool_release_client_slots(client);
	linkr_debugger_ws_sigrok_burst_pool_abort(client->session_id, old_generation);
	client->sigrok_rate_window_ms = k_uptime_get();
	client->sigrok_rate_used = 0;
	client->sigrok_stream_slow_send_logs = 0U;
	sigrok_ws_stream_metrics_reset(client);
}

static int sigrok_ws_disconnect_after_send_failure(struct linkr_debugger_ws_client *client,
	int send_error)
{
	if (client != NULL) {
		client->connected = false;
		sigrok_ws_release_capture_if_held(client);
		sigrok_ws_stream_reset_queue(client);
	}

	return send_error;
}

static void sigrok_ws_stream_clear_stale_queue(struct linkr_debugger_ws_client *client)
{
	struct sigrok_ws_stream_queue_item *item;
	uint32_t old_generation;

	sigrok_ws_stream_cancel_deferred_wake_sync(client);
	while ((item = sigrok_ws_stream_pop_accounted_item(client)) != NULL) {
		sigrok_ws_stream_free_accounted_item(client, item);
	}
	atomic_set(&client->sigrok_stream_qdepth, 0);
	atomic_set(&client->sigrok_stream_qbytes, 0);
	atomic_set(&client->sigrok_stream_deferred_wake_pending, 0);
	atomic_set(&client->sigrok_stream_abort_pending, 0);
	old_generation = client->sigrok_stream_generation;
	client->sigrok_stream_generation++;
	if (client->sigrok_stream_generation == 0U) {
		client->sigrok_stream_generation++;
	}
	sigrok_ws_stream_pool_release_client_slots(client);
	linkr_debugger_ws_sigrok_burst_pool_abort(client->session_id, old_generation);
}

static void sigrok_ws_mark_stop_pending(struct linkr_debugger_ws_client *client)
{
	atomic_set(&client->sigrok_stream_stop_pending, 1);
}

static void sigrok_ws_mark_abort_pending(struct linkr_debugger_ws_client *client)
{
	atomic_set(&client->sigrok_stream_abort_pending, 1);
	atomic_set(&client->sigrok_stream_stop_pending, 1);
}

static void sigrok_ws_stop_capture_if_pending(struct linkr_debugger_ws_client *client)
{
	if (client == NULL) {
		return;
	}
	if (atomic_cas(&client->sigrok_stream_abort_pending, 1, 0)) {
		atomic_set(&client->sigrok_stream_stop_pending, 0);
		sigrok_ws_release_capture_if_held(client);
		return;
	}
	if (!atomic_cas(&client->sigrok_stream_stop_pending, 1, 0)) {
		return;
	}
	if (sigrok_ws_burst_pool_active_for_client(client) &&
	    !linkr_debugger_ws_sigrok_burst_pool_source_done_and_drained()) {
		atomic_set(&client->sigrok_stream_stop_pending, 1);
		if (linkr_debugger_ws_sigrok_burst_pool_source_done_and_drained() ||
		    atomic_get(&client->sigrok_stream_qdepth) > 0 ||
		    !k_fifo_is_empty(&client->sigrok_stream_fifo)) {
			k_event_post(&client->events, LINKR_DEBUGGER_WS_EVENT_STREAM_DATA);
		}
		return;
	}
	sigrok_ws_release_capture_if_held(client);
}

static int sigrok_ws_send_terminal_overrun_event(struct linkr_debugger_ws_client *client)
{
	uint8_t ev_buf[LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES];
	struct linkr_debugger_sigrok_linkr_header ev_hdr;
	struct linkr_debugger_sigrok_linkr_event ev = {
		.session_id = client->sigrok_session.active_session_id,
		.type_detail = (uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_OVERRUN,
		.sample_index = client->sigrok_session.sample_index,
	};
	size_t ev_len;

	ev_len = linkr_debugger_sigrok_linkr_encode_event(&ev,
		ev_buf + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES,
		sizeof(ev_buf) - LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);
	linkr_debugger_sigrok_linkr_init_response_header(&ev_hdr,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_EVENT, 0U, (uint16_t)ev_len);
	(void)linkr_debugger_sigrok_linkr_encode_header(&ev_hdr, ev_buf, sizeof(ev_buf));
	ev_len += LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES;

	return websocket_send_msg(client->ws_sock, ev_buf, ev_len,
		WEBSOCKET_OPCODE_DATA_BINARY, false, true,
		LINKR_DEBUGGER_WS_SIGROK_SEND_TIMEOUT_MS);
}

static int sigrok_ws_send_terminal_error_event(struct linkr_debugger_ws_client *client)
{
	uint8_t ev_buf[LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
		LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES];
	struct linkr_debugger_sigrok_linkr_header ev_hdr;
	struct linkr_debugger_sigrok_linkr_event ev = {
		.session_id = client->sigrok_session.active_session_id,
		.type_detail = (uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_ERROR,
		.sample_index = client->sigrok_session.sample_index,
	};
	size_t ev_len;

	ev_len = linkr_debugger_sigrok_linkr_encode_event(&ev,
		ev_buf + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES,
		sizeof(ev_buf) - LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);
	linkr_debugger_sigrok_linkr_init_response_header(&ev_hdr,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_EVENT, 0U, (uint16_t)ev_len);
	(void)linkr_debugger_sigrok_linkr_encode_header(&ev_hdr, ev_buf, sizeof(ev_buf));
	ev_len += LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES;

	return websocket_send_msg(client->ws_sock, ev_buf, ev_len,
		WEBSOCKET_OPCODE_DATA_BINARY, false, true,
		LINKR_DEBUGGER_WS_SIGROK_SEND_TIMEOUT_MS);
}

static void sigrok_ws_terminalize_failed_stream(struct linkr_debugger_ws_client *client,
	int send_error)
{
	sigrok_ws_stream_log_transport_metrics(client, "failed_stream", send_error, 0U);
	if (linkr_debugger_sigrok_linkr_should_emit_local_terminal_event(
	    client->connected, send_error)) {
		int64_t started_ticks = k_uptime_ticks();
		int ret = sigrok_ws_send_terminal_overrun_event(client);
		uint64_t duration_us = k_ticks_to_us_floor64(k_uptime_ticks() - started_ticks);
		sigrok_ws_stream_metrics_update_send(client, duration_us, 1U,
			LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
			LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES);

		if (ret < 0) {
			client->connected = false;
		}
		LOG_WRN("sigrok ws terminal overrun send: batch_frames=1 len=%u qdepth=%ld "
			"qbytes=%u "
			"duration_us=%llu ret=%d rate_used=%d",
			(unsigned int)(LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
				LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES),
			(long)atomic_get(&client->sigrok_stream_qdepth),
			(unsigned int)sigrok_ws_stream_qbytes(client),
			(unsigned long long)duration_us, ret,
			client->sigrok_rate_used);
	}
	sigrok_ws_mark_abort_pending(client);
	if (client->sigrok_session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING ||
	    client->sigrok_session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED) {
		client->sigrok_session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
	}
	sigrok_ws_stream_clear_stale_queue(client);
	sigrok_ws_stop_capture_if_pending(client);
}

static int linkr_debugger_ws_handle_sigrok_binary(struct linkr_debugger_ws_client *client,
						  const uint8_t *data, size_t len)
{
	uint8_t *sigrok_resp_buf = client->tx_buffer;
	uint8_t *tx_payload = sigrok_resp_buf + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES;
	size_t tx_payload_capacity = sizeof(client->tx_buffer) -
		LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES;
	size_t offset = 0U;

	if (len < LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES) {
		return -EINVAL;
	}

	while (offset < len) {
		struct linkr_debugger_sigrok_linkr_request request;
		struct linkr_debugger_sigrok_linkr_header response_header;
		struct linkr_debugger_sigrok_linkr_action_result action;
		struct linkr_debugger_sigrok_linkr_start_prepare prepare;
		bool disconnect_required = false;
		size_t tx_payload_len = 0U;
		size_t next_offset = offset;
		int ret;

		ret = linkr_debugger_sigrok_linkr_decode_next_request_frame(data, len,
			offset, &request, &next_offset, &disconnect_required, NULL);
		if (ret < 0) {
			return ret;
		}

		if (linkr_debugger_sigrok_linkr_tcp_active()) {
			struct linkr_debugger_sigrok_linkr_error error_resp;
			uint8_t error_buf[LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
				LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BYTES];
			size_t error_len;
			struct linkr_debugger_sigrok_linkr_header error_header;

			error_resp.error_code = LINKR_DEBUGGER_SIGROK_LINKR_ERROR_BUSY;
			error_resp.detail = 0;
			error_len = linkr_debugger_sigrok_linkr_encode_error(&error_resp,
				error_buf + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES,
				sizeof(error_buf) - LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);

			linkr_debugger_sigrok_linkr_init_response_header(&error_header,
				LINKR_DEBUGGER_SIGROK_LINKR_FRAME_ERROR, request.header.id,
				(uint16_t)error_len);
			(void)linkr_debugger_sigrok_linkr_encode_header(&error_header,
				error_buf, sizeof(error_buf));
			error_len += LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES;

			ret = websocket_send_msg(client->ws_sock, error_buf, error_len,
				WEBSOCKET_OPCODE_DATA_BINARY, false, true,
				LINKR_DEBUGGER_WS_SIGROK_SEND_TIMEOUT_MS);
			if (ret < 0) {
				return sigrok_ws_disconnect_after_send_failure(client, ret);
			}
			offset = next_offset;
			continue;
		}

		if (request.header.type == LINKR_DEBUGGER_SIGROK_LINKR_FRAME_HELLO_REQ) {
			sigrok_ws_release_capture_if_held(client);
			sigrok_ws_stream_reset_queue(client);
		}

		linkr_debugger_sigrok_linkr_start_prepare_reset(&prepare);
		ret = linkr_debugger_sigrok_linkr_handle_request(
			&client->sigrok_session,
			linkr_debugger_capture_arbiter_owner(),
			&request,
			&response_header,
			tx_payload, tx_payload_capacity,
			&tx_payload_len,
			&action,
			&disconnect_required);
		if (ret < 0) {
			return ret;
		}

		if (action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_STOP) {
			sigrok_ws_release_capture_if_held(client);
			sigrok_ws_stream_reset_queue(client);
		}
		if (action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_IMMEDIATE ||
		    action.capture_action == LINKR_DEBUGGER_SIGROK_LINKR_CAPTURE_ACTION_PREPARE_ARMED) {
			struct linkr_debugger_la_packed_burst_plan burst_plan;
			bool packed_burst = linkr_debugger_sigrok_linkr_packed_burst_plan(
				&client->sigrok_session.config, &burst_plan) == 0;
			uint8_t bytes_per_sample = packed_burst ? burst_plan.bytes_per_sample :
				linkr_debugger_sigrok_linkr_bytes_per_sample(
					client->sigrok_session.config.channel_mask);
			enum linkr_debugger_la_stream_payload_format format = packed_burst ?
				LINKR_DEBUGGER_LA_STREAM_PAYLOAD_PACKED_LE_BYTES :
				linkr_debugger_sigrok_linkr_stream_payload_format(
					client->sigrok_session.config.channel_mask);
			struct linkr_debugger_la_stream_sink sink = {
				.format = format,
				.bytes_per_sample = bytes_per_sample,
				.max_chunk_samples = packed_burst ?
					linkr_debugger_sigrok_linkr_packed_burst_max_chunk_samples(
						bytes_per_sample) :
					(format == LINKR_DEBUGGER_LA_STREAM_PAYLOAD_SINGLE_BITS ?
					 LINKR_DEBUGGER_LA_STREAM_MAX_SINGLE_BITS_CHUNK_SAMPLES :
					 LINKR_DEBUGGER_SIGROK_LINKR_MAX_DATA_BYTES /
					 bytes_per_sample),
				.lease = sigrok_ws_stream_sink_lease,
				.commit = sigrok_ws_stream_sink_commit,
				.abort = sigrok_ws_stream_sink_abort,
				.terminal = sigrok_ws_stream_sink_terminal,
				.user_data = client,
			};

			sigrok_ws_stream_reset_queue(client);
			client->sigrok_session.sample_index = 0U;
			client->sigrok_session.emitted_samples = 0U;
			ret = linkr_debugger_sigrok_linkr_start_prepare_capture(&prepare,
				&client->sigrok_session, true, client->session_id,
				client->sigrok_stream_generation,
				client->sigrok_stream_generation, &sink);
			if (ret < 0) {
				enum linkr_debugger_sigrok_linkr_error_code error_code =
					linkr_debugger_sigrok_linkr_start_error_code(ret, false);

				linkr_debugger_sigrok_linkr_rollback_start_failure(
					&client->sigrok_session, &action);
				linkr_debugger_sigrok_linkr_build_error_response(&request,
					&response_header, tx_payload, tx_payload_capacity,
					error_code, (uint16_t)(-ret), &tx_payload_len);
			}
		}

		size_t resp_len;

		resp_len = linkr_debugger_sigrok_linkr_encode_header(&response_header,
			sigrok_resp_buf, sizeof(client->tx_buffer));
		if (tx_payload_len > 0) {
			resp_len += tx_payload_len;
		}

		ret = websocket_send_msg(client->ws_sock, sigrok_resp_buf, resp_len,
				 WEBSOCKET_OPCODE_DATA_BINARY, false, true,
				 LINKR_DEBUGGER_WS_SIGROK_SEND_TIMEOUT_MS);
		if (ret < 0) {
			linkr_debugger_sigrok_linkr_start_prepare_cancel(&prepare,
				&client->sigrok_session);
			return sigrok_ws_disconnect_after_send_failure(client, ret);
		}
		if (prepare.state == LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_PREPARED) {
			ret = linkr_debugger_sigrok_linkr_start_prepare_mark_response_sent(
				&prepare);
			if (ret < 0) {
				linkr_debugger_sigrok_linkr_start_prepare_cancel(&prepare,
					&client->sigrok_session);
				ret = sigrok_ws_send_terminal_error_event(client);
				if (ret < 0) {
					return sigrok_ws_disconnect_after_send_failure(client, ret);
				}
				offset = next_offset;
				continue;
			}
		}

		if (action.has_event) {
			uint8_t event_buf[LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES +
				LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES];
			size_t event_len;
			struct linkr_debugger_sigrok_linkr_header event_header;

			event_len = linkr_debugger_sigrok_linkr_encode_event(&action.event,
				event_buf + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES,
				sizeof(event_buf) - LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);
			linkr_debugger_sigrok_linkr_init_response_header(&event_header,
				LINKR_DEBUGGER_SIGROK_LINKR_FRAME_EVENT, 0U, (uint16_t)event_len);
			(void)linkr_debugger_sigrok_linkr_encode_header(&event_header,
				event_buf, sizeof(event_buf));
			event_len += LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES;

			ret = websocket_send_msg(client->ws_sock, event_buf, event_len,
					 WEBSOCKET_OPCODE_DATA_BINARY, false, true,
					 LINKR_DEBUGGER_WS_SIGROK_SEND_TIMEOUT_MS);
			if (ret < 0) {
				linkr_debugger_sigrok_linkr_start_prepare_cancel(&prepare,
					&client->sigrok_session);
				return sigrok_ws_disconnect_after_send_failure(client, ret);
			}
			if (prepare.state == LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_PREPARED &&
			    action.event.type_detail ==
			    (uint8_t)LINKR_DEBUGGER_SIGROK_LINKR_EVENT_ARMED) {
				ret = linkr_debugger_sigrok_linkr_start_prepare_mark_armed_event_sent(
					&prepare);
				if (ret < 0) {
					linkr_debugger_sigrok_linkr_start_prepare_cancel(&prepare,
						&client->sigrok_session);
					ret = sigrok_ws_send_terminal_error_event(client);
					if (ret < 0) {
						return sigrok_ws_disconnect_after_send_failure(client, ret);
					}
					offset = next_offset;
					continue;
				}
			}
		}

		if (prepare.state == LINKR_DEBUGGER_SIGROK_LINKR_START_PREPARE_PREPARED) {
			ret = linkr_debugger_sigrok_linkr_start_prepare_go(&prepare,
				&client->sigrok_session);
			if (ret < 0) {
				ret = sigrok_ws_send_terminal_error_event(client);
				if (ret < 0) {
					return sigrok_ws_disconnect_after_send_failure(client, ret);
				}
			}
		}

		if (disconnect_required) {
			return -ECONNRESET;
		}

		offset = next_offset;
	}

	return 0;
}

static bool sigrok_ws_queue_event_at(struct linkr_debugger_ws_client *client,
					    enum linkr_debugger_sigrok_linkr_event_type event_type,
					    uint32_t sample_index)
{
	uint8_t ev_buf[LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES + LINKR_DEBUGGER_SIGROK_LINKR_EVENT_BYTES];
	size_t ev_len;
	struct linkr_debugger_sigrok_linkr_header ev_hdr;
	struct linkr_debugger_sigrok_linkr_event ev = {
		.session_id = client->sigrok_session.active_session_id,
		.type_detail = (uint8_t)event_type,
		.sample_index = sample_index,
	};
	struct sigrok_ws_stream_queue_item *ev_item;
	bool terminal = event_type == LINKR_DEBUGGER_SIGROK_LINKR_EVENT_STOPPED ||
		event_type == LINKR_DEBUGGER_SIGROK_LINKR_EVENT_OVERRUN ||
		event_type == LINKR_DEBUGGER_SIGROK_LINKR_EVENT_ERROR;

	ev_len = linkr_debugger_sigrok_linkr_encode_event(&ev,
		ev_buf + LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES,
		sizeof(ev_buf) - LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES);
	linkr_debugger_sigrok_linkr_init_response_header(&ev_hdr,
		LINKR_DEBUGGER_SIGROK_LINKR_FRAME_EVENT, 0U, (uint16_t)ev_len);
	(void)linkr_debugger_sigrok_linkr_encode_header(&ev_hdr, ev_buf, sizeof(ev_buf));
	ev_len += LINKR_DEBUGGER_SIGROK_LINKR_HEADER_BYTES;

	ev_item = sigrok_ws_stream_slot_acquire(client, terminal);
	if (ev_item == NULL) {
		atomic_inc(&client->sigrok_stream_dropped);
		return false;
	}
	if (ev_len > ev_item->capacity ||
	    !linkr_debugger_sigrok_linkr_stream_queue_bytes_has_capacity(
	    sigrok_ws_stream_qbytes(client), ev_len, LINKR_DEBUGGER_WS_SIGROK_QBYTES_LIMIT,
	    false, 0U)) {
		atomic_inc(&client->sigrok_stream_dropped);
		sigrok_ws_stream_slot_release(ev_item);
		return false;
	}
	ev_item->len = ev_len;
	ev_item->kind = SIGROK_WS_STREAM_ITEM_PREFRAMED;
	memcpy(ev_item->data, ev_buf, ev_len);
	return sigrok_ws_stream_put_accounted_item(client, ev_item);
}

static bool sigrok_ws_queue_event(struct linkr_debugger_ws_client *client,
					 enum linkr_debugger_sigrok_linkr_event_type event_type)
{
	return client != NULL && sigrok_ws_queue_event_at(client, event_type,
		client->sigrok_session.sample_index);
}

static enum linkr_debugger_la_stream_payload_format
sigrok_ws_stream_sink_payload_format(
	const struct linkr_debugger_sigrok_linkr_session *session)
{
	struct linkr_debugger_la_packed_burst_plan burst_plan;

	if (session != NULL &&
	    linkr_debugger_sigrok_linkr_packed_burst_plan(&session->config, &burst_plan) == 0) {
		return LINKR_DEBUGGER_LA_STREAM_PAYLOAD_PACKED_LE_BYTES;
	}
	return session == NULL ? LINKR_DEBUGGER_LA_STREAM_PAYLOAD_PACKED_LE_BYTES :
		linkr_debugger_sigrok_linkr_stream_payload_format(session->config.channel_mask);
}

static bool sigrok_ws_stream_sink_final_chunk(
	const struct linkr_debugger_sigrok_linkr_session *session,
	uint32_t sample_count)
{
	uint32_t target_samples = linkr_debugger_sigrok_linkr_bounded_sample_target(session);

	return target_samples > 0U && session->emitted_samples < target_samples &&
		sample_count >= target_samples - session->emitted_samples;
}

static int sigrok_ws_stream_sink_lease(uint32_t sample_count,
	uint8_t bytes_per_sample, void *user_data,
	struct linkr_debugger_la_stream_sink_lease *lease)
{
	struct linkr_debugger_ws_client *client = (struct linkr_debugger_ws_client *)user_data;
	struct linkr_debugger_sigrok_linkr_session *session;
	struct sigrok_ws_stream_queue_item *item;
	uint8_t *payload;
	uint16_t send_count;

	if (client == NULL || lease == NULL || sample_count == 0U || bytes_per_sample == 0U) {
		return -EINVAL;
	}
	memset(lease, 0, sizeof(*lease));
	if (!client->connected) {
		return -ENOTCONN;
	}

	session = &client->sigrok_session;
	if (linkr_debugger_sigrok_linkr_bytes_per_sample(session->config.channel_mask) !=
	    bytes_per_sample) {
		return -EINVAL;
	}
	if (session->state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED) {
		session->state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING;
		if (!sigrok_ws_queue_event_at(client, LINKR_DEBUGGER_SIGROK_LINKR_EVENT_TRIGGERED,
		    linkr_debugger_sigrok_linkr_trigger_sample_index(session))) {
			return -ENOSPC;
		}
		sigrok_ws_stream_apply_wake_policy(client, true);
	}
	if (session->state != LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING) {
		return -ECANCELED;
	}

	send_count = linkr_debugger_sigrok_linkr_bounded_chunk_count(session, sample_count);
	if (send_count == 0U || send_count != sample_count) {
		return -EINVAL;
	}

	item = sigrok_ws_stream_slot_acquire(client, false);
	if (item == NULL) {
		atomic_inc(&client->sigrok_stream_dropped);
		return -ENOSPC;
	}
	payload = sigrok_ws_stream_payload(item);
	if (payload == NULL || sigrok_ws_stream_payload_capacity(item) == 0U) {
		sigrok_ws_stream_slot_release(item);
		return -ENOSPC;
	}

	lease->payload = payload;
	lease->capacity = sigrok_ws_stream_payload_capacity(item);
	lease->token = item;
	return 0;
}

static int sigrok_ws_stream_sink_commit(
	const struct linkr_debugger_la_stream_sink_commit *commit, void *user_data)
{
	struct linkr_debugger_ws_client *client = (struct linkr_debugger_ws_client *)user_data;
	struct linkr_debugger_sigrok_linkr_session *session;
	struct sigrok_ws_stream_queue_item *item;
	enum linkr_debugger_la_stream_payload_format format;
	uint16_t channel_mask;
	bool final_chunk;
	uint32_t qdepth;
	size_t total_len;

	if (client == NULL || commit == NULL || commit->token == NULL ||
	    commit->sample_count == 0U || commit->sample_count > UINT16_MAX ||
	    commit->bytes_per_sample == 0U || commit->payload_len == 0U) {
		return -EINVAL;
	}
	if (!client->connected) {
		return -ENOTCONN;
	}

	session = &client->sigrok_session;
	channel_mask = session->config.channel_mask;
	format = sigrok_ws_stream_sink_payload_format(session);
	item = (struct sigrok_ws_stream_queue_item *)commit->token;
	if (session->state != LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING ||
	    linkr_debugger_sigrok_linkr_bytes_per_sample(channel_mask) !=
	    commit->bytes_per_sample ||
	    commit->payload_len != linkr_debugger_logic_analyzer_stream_payload_len(
		format, commit->sample_count, commit->bytes_per_sample) ||
	    commit->payload_len > sigrok_ws_stream_payload_capacity(item)) {
		return -EINVAL;
	}

	total_len = linkr_debugger_sigrok_linkr_encode_stream_data_frame(format,
		session->sample_index, (uint16_t)commit->sample_count, channel_mask,
		sigrok_ws_stream_payload(item), commit->payload_len, true,
		item->data, item->capacity);
	if (total_len == 0U) {
		return -EMSGSIZE;
	}

	final_chunk = sigrok_ws_stream_sink_final_chunk(session, commit->sample_count);
	item->kind = SIGROK_WS_STREAM_ITEM_PREFRAMED;
	item->len = total_len;
	if (!linkr_debugger_sigrok_linkr_stream_queue_bytes_has_capacity(
	    sigrok_ws_stream_qbytes(client), item->len,
	    LINKR_DEBUGGER_WS_SIGROK_QBYTES_LIMIT, final_chunk,
	    sigrok_ws_stream_event_item_bytes())) {
		atomic_inc(&client->sigrok_stream_dropped);
		return -ENOSPC;
	}
	if (!sigrok_ws_stream_put_accounted_item(client, item)) {
		atomic_inc(&client->sigrok_stream_dropped);
		return -ESTALE;
	}

	session->emitted_samples += commit->sample_count;
	session->sample_index = linkr_debugger_sigrok_linkr_advance_sample_index(
		session->sample_index, commit->sample_count);
	qdepth = (uint32_t)atomic_get(&client->sigrok_stream_qdepth);
	if (linkr_debugger_sigrok_linkr_stream_sink_handoff_requested(qdepth)) {
		sigrok_ws_stream_apply_wake_policy(client, true);
		return 1;
	}
	sigrok_ws_stream_apply_wake_policy(client, false);
	return 0;
}

static void sigrok_ws_stream_sink_abort(void *token, void *user_data)
{
	ARG_UNUSED(user_data);

	sigrok_ws_stream_slot_release((struct sigrok_ws_stream_queue_item *)token);
}

static void sigrok_ws_stream_sink_terminal(
	enum linkr_debugger_la_ring_poll_result status, uint32_t sequence,
	void *user_data)
{
	struct linkr_debugger_ws_client *client = (struct linkr_debugger_ws_client *)user_data;
	enum linkr_debugger_sigrok_linkr_event_type event_type;

	if (client == NULL) {
		return;
	}

	event_type = status == LINKR_DEBUGGER_LA_RING_POLL_OK ?
		LINKR_DEBUGGER_SIGROK_LINKR_EVENT_STOPPED :
		LINKR_DEBUGGER_SIGROK_LINKR_EVENT_OVERRUN;
	linkr_debugger_ws_sigrok_burst_pool_mark_source_decode_complete(
		client->sigrok_stream_generation);
	if (!sigrok_ws_queue_event(client, event_type)) {
		LOG_WRN("sigrok ws sink terminal event dropped: status=%d seq=%u qdepth=%ld "
			"qbytes=%u state=%d sample_index=%u emitted=%u",
			status, sequence, (long)atomic_get(&client->sigrok_stream_qdepth),
			(unsigned int)sigrok_ws_stream_qbytes(client),
			client->sigrok_session.state, client->sigrok_session.sample_index,
			client->sigrok_session.emitted_samples);
	}
	sigrok_ws_stream_log_transport_metrics(client, "sink_terminal", status, sequence);
	client->sigrok_session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
	sigrok_ws_mark_stop_pending(client);
	sigrok_ws_stream_apply_wake_policy(client, true);
}

static void sigrok_ws_stream_drain(struct linkr_debugger_ws_client *client)
{
	struct sigrok_ws_stream_queue_item *pending_item = NULL;
	struct sigrok_ws_stream_queue_item *sent_items[LINKR_DEBUGGER_WS_SIGROK_COALESCE_MAX_FRAMES];
	bool terminal_error = false;
	int64_t drain_started_ticks = k_uptime_ticks();
	uint32_t drain_items = 0U;
	size_t drain_bytes = 0U;

	int drained = 0;
	while (drained < LINKR_DEBUGGER_WS_SIGROK_DRAIN_BATCH) {
		uint8_t coalesced_count = 0U;
		size_t coalesced_len = 0U;

		while (drained < LINKR_DEBUGGER_WS_SIGROK_DRAIN_BATCH &&
		       coalesced_count < LINKR_DEBUGGER_WS_SIGROK_COALESCE_MAX_FRAMES) {
			struct sigrok_ws_stream_queue_item *item = pending_item;

			if (item == NULL) {
				item = sigrok_ws_stream_pop_accounted_item(client);
				if (item == NULL) {
					break;
				}
			} else {
				pending_item = NULL;
			}

			if (!linkr_debugger_sigrok_linkr_coalesce_can_append(coalesced_len,
			    item->len, LINKR_DEBUGGER_WS_SIGROK_COALESCE_MAX_BYTES, coalesced_count,
			    LINKR_DEBUGGER_WS_SIGROK_COALESCE_MAX_FRAMES)) {
				if (coalesced_count == 0U) {
					size_t failed_len = item->len;

					atomic_inc(&client->sigrok_stream_dropped);
					sigrok_ws_stream_free_accounted_item(client, item);
					terminal_error = true;
					LOG_WRN("sigrok ws drain terminal failure: batch_frames=0 len=%u "
						"qdepth=%ld qbytes=%u duration_us=0 ret=0 rate_used=%d",
						(unsigned int)failed_len,
						(long)atomic_get(&client->sigrok_stream_qdepth),
						(unsigned int)sigrok_ws_stream_qbytes(client),
						client->sigrok_rate_used);
				}
				pending_item = coalesced_count == 0U ? NULL : item;
				break;
			}

			size_t encoded_len = sigrok_ws_stream_encode_item(item,
				client->tx_buffer + coalesced_len,
				LINKR_DEBUGGER_WS_SIGROK_COALESCE_MAX_BYTES - coalesced_len);
			if (encoded_len == 0U || encoded_len > item->len ||
			    encoded_len > LINKR_DEBUGGER_WS_SIGROK_COALESCE_MAX_BYTES - coalesced_len) {
				size_t max_len = item->len;
				uint8_t kind = item->kind;

				atomic_inc(&client->sigrok_stream_dropped);
				sigrok_ws_stream_free_accounted_item(client, item);
				terminal_error = true;
				LOG_WRN("sigrok ws drain terminal failure: encode len=%u max_len=%u "
					"kind=%u batch_frames=%u qdepth=%ld qbytes=%u rate_used=%d",
					(unsigned int)encoded_len, (unsigned int)max_len,
					kind, coalesced_count,
					(long)atomic_get(&client->sigrok_stream_qdepth),
					(unsigned int)sigrok_ws_stream_qbytes(client),
					client->sigrok_rate_used);
				break;
			}
			coalesced_len += encoded_len;
			sent_items[coalesced_count] = item;
			coalesced_count++;
			drained++;
		}

		if (terminal_error) {
			for (uint8_t i = 0U; i < coalesced_count; i++) {
				sigrok_ws_stream_free_accounted_item(client, sent_items[i]);
			}
			break;
		}

		if (coalesced_count == 0U) {
			break;
		}
		drain_items += coalesced_count;
		drain_bytes += coalesced_len;

		if (client->connected) {
			/* Cap the sustained WS send rate to avoid the repository-local
			 * CDC-NCM backpressure wedge observed under sustained ~1k frames/sec.
			 * Short bursts stay under the cap; long streams stop with OVERRUN.
			 */
			int64_t now = k_uptime_get();
			if (now - client->sigrok_rate_window_ms >= 1000) {
				client->sigrok_rate_window_ms = now;
				client->sigrok_rate_used = 0;
			}
			if (client->sigrok_rate_used >= LINKR_DEBUGGER_WS_SIGROK_SEND_RATE_CAP) {
				atomic_inc(&client->sigrok_stream_dropped);
				for (uint8_t i = 0U; i < coalesced_count; i++) {
					sigrok_ws_stream_free_accounted_item(client, sent_items[i]);
				}
				terminal_error = true;
				LOG_WRN("sigrok ws drain terminal failure: batch_frames=%u len=%u "
					"qdepth=%ld qbytes=%u duration_us=0 ret=0 rate_used=%d",
					coalesced_count, (unsigned int)coalesced_len,
					(long)atomic_get(&client->sigrok_stream_qdepth),
					(unsigned int)sigrok_ws_stream_qbytes(client),
					client->sigrok_rate_used);
				break;
			}
			int64_t started_ticks = k_uptime_ticks();
			int sret = websocket_send_msg(client->ws_sock, client->tx_buffer, coalesced_len,
						 WEBSOCKET_OPCODE_DATA_BINARY, false, true,
						 LINKR_DEBUGGER_WS_SIGROK_SEND_TIMEOUT_MS);
			uint64_t duration_us = k_ticks_to_us_floor64(k_uptime_ticks() - started_ticks);
			sigrok_ws_stream_metrics_update_send(client, duration_us, coalesced_count,
				coalesced_len);

			if (sret < 0) {
				LOG_WRN("sigrok ws send failed: batch_frames=%u len=%u qdepth=%ld "
					"qbytes=%u duration_us=%llu ret=%d rate_used=%d",
					coalesced_count, (unsigned int)coalesced_len,
					(long)atomic_get(&client->sigrok_stream_qdepth),
					(unsigned int)sigrok_ws_stream_qbytes(client),
					(unsigned long long)duration_us, sret,
					client->sigrok_rate_used);
			} else if (duration_us > LINKR_DEBUGGER_WS_SIGROK_SEND_SLOW_US &&
			    client->sigrok_stream_slow_send_logs <
			    LINKR_DEBUGGER_WS_SIGROK_SLOW_SEND_LOG_LIMIT) {
				client->sigrok_stream_slow_send_logs++;
				LOG_WRN("sigrok ws send: batch_frames=%u len=%u qdepth=%ld "
					"qbytes=%u duration_us=%llu ret=%d rate_used=%d slow_log=%u",
					coalesced_count, (unsigned int)coalesced_len,
					(long)atomic_get(&client->sigrok_stream_qdepth),
					(unsigned int)sigrok_ws_stream_qbytes(client),
					(unsigned long long)duration_us, sret,
					client->sigrok_rate_used,
					client->sigrok_stream_slow_send_logs);
			}
			if (sret < 0) {
				client->connected = false;
				for (uint8_t i = 0U; i < coalesced_count; i++) {
					sigrok_ws_stream_free_accounted_item(client, sent_items[i]);
				}
				terminal_error = true;
				break;
			}
			client->sigrok_rate_used++;
		}
		for (uint8_t i = 0U; i < coalesced_count; i++) {
			sigrok_ws_stream_free_accounted_item(client, sent_items[i]);
		}
		if (pending_item != NULL) {
			continue;
		}
		if (k_fifo_is_empty(&client->sigrok_stream_fifo)) {
			break;
		}
	}
	sigrok_ws_stream_metrics_update_drain(client,
		k_ticks_to_us_floor64(k_uptime_ticks() - drain_started_ticks),
		drain_items, drain_bytes);
	if (terminal_error) {
		if (pending_item != NULL) {
			sigrok_ws_stream_free_accounted_item(client, pending_item);
		}
		sigrok_ws_terminalize_failed_stream(client, client->connected ? 0 : -EIO);
	}
	/* If backlog remains after this batch, re-arm the event so the loop
	 * drains again immediately instead of waiting for the next chunk post.
	 */
	if (!terminal_error && client->connected && !k_fifo_is_empty(&client->sigrok_stream_fifo)) {
		k_event_post(&client->events, LINKR_DEBUGGER_WS_EVENT_STREAM_DATA);
	}
	if (!terminal_error && k_fifo_is_empty(&client->sigrok_stream_fifo)) {
		sigrok_ws_stream_cancel_deferred_wake_sync(client);
		sigrok_ws_stop_capture_if_pending(client);
	}
}

static void linkr_debugger_ws_thread_main(void *arg1, void *arg2, void *arg3)
{
	struct linkr_debugger_ws_client_thread_arg *thread_arg = arg1;
	struct linkr_debugger_ws_client *client = thread_arg->client;
	int ws_sock = thread_arg->ws_sock;
	uint32_t session_id = thread_arg->session_id;
	uint32_t message_type;
	uint64_t remaining;
	int ret;

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		bool capture_pending;
		uint32_t events;
		uint32_t wait_ms;

		k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
		if (!client->active || client->session_id != session_id || !client->connected) {
			k_mutex_unlock(&linkr_debugger_ws_clients_lock);
			break;
		}
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);

		capture_pending = linkr_debugger_ws_client_has_capture_pending(client);
		bool sigrok_busy = !k_fifo_is_empty(&client->sigrok_stream_fifo) ||
			client->sigrok_session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING ||
			client->sigrok_session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED;
		/* When sigrok is streaming, recv must not block (that stalls the
		 * drain); the k_event wait above provides the blocking so the thread
		 * sleeps instead of spinning.
		 */
		wait_ms = capture_pending ? 1U : client->telemetry_enabled ?
			MAX(1U, DIV_ROUND_UP(1000U * client->telemetry_batch_size,
					       (uint32_t)client->telemetry_rate_hz)) :
			LINKR_DEBUGGER_WS_IDLE_WAIT_MS;

		/* wait_safe consumes only the reported bits: a plain k_event_wait
		 * with reset=true wipes pending bits unreported at entry and on
		 * timeout, which could permanently lose one-shot event posts.
		 */
		events = k_event_wait_safe(&client->events,
			LINKR_DEBUGGER_WS_EVENT_STATE | LINKR_DEBUGGER_WS_EVENT_SAMPLE | LINKR_DEBUGGER_WS_EVENT_STREAM_DATA,
			false, K_MSEC(wait_ms));

		if (events & LINKR_DEBUGGER_WS_EVENT_STATE) {
			ret = linkr_debugger_ws_emit_status_snapshot(client);
			if (ret < 0) {
				LOG_ERR("failed to emit status snapshot: %d", ret);
				break;
			}
		}

		if (client->telemetry_enabled) {
			if (client->telemetry_batch_size > 1U) {
				ret = linkr_debugger_ws_emit_adc_batch(client);
			} else {
				ret = linkr_debugger_ws_emit_adc_sample(client);
			}
			if (ret < 0) {
				LOG_ERR("failed to emit adc sample: %d", ret);
				break;
			}
		}

		ret = linkr_debugger_ws_emit_capture_pending(client);
		if (ret < 0) {
			LOG_ERR("failed to emit capture data: %d", ret);
			break;
		}

		sigrok_ws_stream_drain(client);

		ret = websocket_recv_msg(ws_sock,
				 client->recv_buffer,
				 sizeof(client->recv_buffer) - 1U,
				 &message_type,
				 &remaining,
			 (client->telemetry_enabled || capture_pending || sigrok_busy) ?
			 0 : LINKR_DEBUGGER_WS_IDLE_WAIT_MS);
		if (ret == -EAGAIN) {
			continue;
		}
		if (ret < 0) {
			break;
		}
		if ((message_type & WEBSOCKET_FLAG_CLOSE) != 0U) {
			break;
		}
		if ((message_type & WEBSOCKET_FLAG_BINARY) != 0U) {
			if (remaining != 0U) {
				(void)linkr_debugger_ws_emit_error(client, "ws", "message_too_large",
					"fragmented or oversized websocket frames are not supported");
				break;
			}
			ret = linkr_debugger_ws_handle_sigrok_binary(client,
				client->recv_buffer, (size_t)ret);
			if (ret < 0) {
				LOG_ERR("sigrok binary handler error: %d", ret);
				break;
			}
			continue;
		}
		if ((message_type & WEBSOCKET_FLAG_TEXT) == 0U) {
			(void)linkr_debugger_ws_emit_error(client, "ws", "unsupported_frame",
						"only text and binary websocket frames are supported");
			continue;
		}
		if (remaining != 0U) {
			(void)linkr_debugger_ws_emit_error(client, "ws", "message_too_large",
						"fragmented or oversized websocket frames are not supported");
			continue;
		}

		client->recv_buffer[ret] = '\0';
		ret = linkr_debugger_ws_handle_message(client, client->recv_buffer, (size_t)ret);
		if (ret < 0) {
			break;
		}
	}

	/* The close handshake is answered by websocket_unregister: closing the
	 * websocket fd emits exactly one CLOSE frame through its vtable. Sending
	 * an explicit close here too would put a second frame on the wire and
	 * trip Chromium's "Close received after close" warning.
	 */
	(void)websocket_unregister(ws_sock);

	/* If this client had an armed/running sigrok stream, tear it down so a
	 * disconnect without STOP cannot leak the capture and wedge the arbiter
	 * for subsequent sessions.
	 */
	if (client->sigrok_session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_ARMED ||
	    client->sigrok_session.state == LINKR_DEBUGGER_SIGROK_LINKR_SESSION_RUNNING) {
		LOG_WRN("stopping leaked sigrok stream on disconnect (state=%d)",
			(int)client->sigrok_session.state);
		sigrok_ws_release_capture_if_held(client);
		client->sigrok_session.state = LINKR_DEBUGGER_SIGROK_LINKR_SESSION_CONFIGURED;
	}

	sigrok_ws_stream_reset_queue(client);

	linkr_debugger_ws_client_release(client, session_id);
}

int linkr_debugger_ws_setup(int ws_socket, struct http_request_ctx *request_ctx, void *user_data)
{
	uint8_t slot;
	struct linkr_debugger_ws_client *client = NULL;

	ARG_UNUSED(request_ctx);

	if (user_data == NULL) {
		slot = 0U;
		k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
		client = &linkr_debugger_ws_clients[slot];
		if (client->active || client->thread != NULL) {
			k_mutex_unlock(&linkr_debugger_ws_clients_lock);
			return -EBUSY;
		}
		client->active = true;
		client->connected = true;
		client->session_id = 1U;
		client->session_created_ms = k_uptime_get();
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);
	} else {
		slot = *(uint8_t *)user_data;

		k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
		client = linkr_debugger_ws_client_find_by_slot(slot);
		if (client == NULL || client->connected || client->thread != NULL) {
			k_mutex_unlock(&linkr_debugger_ws_clients_lock);
			return -ENOENT;
		}
		client->connected = true;
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);
	}

	if (client == NULL) {
		return -EBUSY;
	}

	client->ws_sock = ws_socket;
	client->telemetry_enabled = false;
	client->sigrok_active = false;
	linkr_debugger_sigrok_linkr_session_reset(&client->sigrok_session);
	k_fifo_init(&client->sigrok_stream_fifo);
	atomic_set(&client->sigrok_stream_qdepth, 0);
	atomic_set(&client->sigrok_stream_qbytes, 0);
	atomic_set(&client->sigrok_stream_dropped, 0);
	atomic_set(&client->sigrok_stream_stop_pending, 0);
	atomic_set(&client->sigrok_stream_abort_pending, 0);
	atomic_set(&client->sigrok_stream_deferred_wake_pending, 0);
	client->sigrok_rate_window_ms = k_uptime_get();
	client->sigrok_rate_used = 0;
	client->telemetry_rate_hz = 10;
	client->telemetry_batch_size = 1U;
	client->next_sample_sequence = 0U;
	client->next_sample_due_us = 0;
	client->sequence = 1U;
	k_event_clear(&client->events, UINT32_MAX);
	linkr_debugger_ws_thread_args[client->slot].client = client;
	linkr_debugger_ws_thread_args[client->slot].ws_sock = ws_socket;
	linkr_debugger_ws_thread_args[client->slot].session_id = client->session_id;

	client->thread = k_thread_create(&client->thread_data,
				      linkr_debugger_ws_stacks[client->slot],
				      K_THREAD_STACK_SIZEOF(linkr_debugger_ws_stacks[client->slot]),
				      linkr_debugger_ws_thread_main,
				      &linkr_debugger_ws_thread_args[client->slot], NULL, NULL,
				      LINKR_DEBUGGER_WS_PRIORITY,
				      0, K_NO_WAIT);
	if (IS_ENABLED(CONFIG_THREAD_NAME)) {
		char thread_name[21];
		snprintk(thread_name, sizeof(thread_name), "linkr_debugger_ws%u", client->slot);
		k_thread_name_set(&client->thread_data, thread_name);
	}
	return 0;
}

static int linkr_debugger_ws_capture_arena_quiesce(int32_t timeout_ms, void *user_data)
{
	bool power_capture_active;
	bool sigrok_pool_free;
	int ret;

	ARG_UNUSED(user_data);

	k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
	power_capture_active = linkr_debugger_capture_owner_session_id != 0U;
	k_mutex_unlock(&linkr_debugger_capture_lock);
	if (power_capture_active) {
		return -EBUSY;
	}

	k_mutex_lock(&linkr_debugger_ws_sample_ring_lock, K_FOREVER);
	linkr_debugger_ws_sampler_pause_requested = true;
	(void)linkr_debugger_ws_sampler_sync_request_pause(
		&linkr_debugger_ws_sampler_sync);
	linkr_debugger_ws_arena_quiesced = false;
	k_sem_reset(&linkr_debugger_ws_sampler_pause_ack);
	k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);
	k_event_post(&linkr_debugger_ws_sampler_events,
		LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);

	ret = k_sem_take(&linkr_debugger_ws_sampler_pause_ack,
		timeout_ms < 0 ? K_FOREVER : K_MSEC(timeout_ms));
	if (ret < 0) {
		return ret == -EAGAIN ? -ETIMEDOUT : ret;
	}

	/* Synchronize against sample-ring readers and writers after sampler ack. */
	k_mutex_lock(&linkr_debugger_ws_sample_ring_lock, K_FOREVER);
	k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);

	k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
	power_capture_active = linkr_debugger_capture_owner_session_id != 0U;
	k_mutex_unlock(&linkr_debugger_capture_lock);
	if (power_capture_active) {
		return -EBUSY;
	}

	k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
	for (size_t i = 0U; i < ARRAY_SIZE(linkr_debugger_ws_clients); i++) {
		k_event_clear(&linkr_debugger_ws_clients[i].events,
			LINKR_DEBUGGER_WS_EVENT_SAMPLE);
	}
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);

	sigrok_pool_free = sigrok_ws_stream_pool_all_free();
	if (!linkr_debugger_capture_arena_ws_quiesce_allows_overwrite(
	    power_capture_active, true, false, sigrok_pool_free)) {
		return -EBUSY;
	}

	k_mutex_lock(&linkr_debugger_ws_sample_ring_lock, K_FOREVER);
	linkr_debugger_ws_arena_quiesced = true;
	k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);
	return 0;
}

static void linkr_debugger_ws_capture_arena_resume(void *user_data)
{
	bool reinitialize;

	ARG_UNUSED(user_data);

	k_mutex_lock(&linkr_debugger_ws_sample_ring_lock, K_FOREVER);
	reinitialize = linkr_debugger_ws_arena_quiesced;
	linkr_debugger_ws_sampler_sync_resume_current(
		&linkr_debugger_ws_sampler_sync);
	linkr_debugger_ws_arena_quiesced = false;
	linkr_debugger_ws_sampler_pause_requested = false;
	if (reinitialize) {
		memset(linkr_debugger_ws_sample_ring, 0,
			LINKR_DEBUGGER_CAPTURE_ARENA_WS_SAMPLE_RING_BYTES);
		memset(&linkr_debugger_ws_sampler_workspace, 0,
			sizeof(linkr_debugger_ws_sampler_workspace));
		linkr_debugger_ws_latest_sample_sequence = 0U;
	}
	k_mutex_unlock(&linkr_debugger_ws_sample_ring_lock);

	if (reinitialize) {
		k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
		memset(&LINKR_DEBUGGER_POWER_CAPTURE, 0,
			sizeof(struct linkr_debugger_capture));
		linkr_debugger_capture_owner_session_id = 0U;
		k_mutex_unlock(&linkr_debugger_capture_lock);

		sigrok_ws_stream_pool_reinitialize_after_arena_resume();

		k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
		for (size_t i = 0U; i < ARRAY_SIZE(linkr_debugger_ws_clients); i++) {
			linkr_debugger_ws_clients[i].next_sample_sequence = 1U;
			linkr_debugger_ws_clients[i].next_sample_due_us = 0;
			k_event_clear(&linkr_debugger_ws_clients[i].events,
				LINKR_DEBUGGER_WS_EVENT_SAMPLE);
		}
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);
	}

	k_event_post(&linkr_debugger_ws_sampler_events,
		LINKR_DEBUGGER_WS_SAMPLER_EVENT_RESUME |
		LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
}

int linkr_debugger_ws_init(void)
{
	memset(linkr_debugger_ws_clients, 0, sizeof(linkr_debugger_ws_clients));
	k_mutex_init(&linkr_debugger_ws_clients_lock);
	k_mutex_init(&linkr_debugger_capture_lock);
	for (size_t i = 0; i < ARRAY_SIZE(linkr_debugger_ws_clients); i++) {
		linkr_debugger_ws_clients[i].slot = (uint8_t)i;
		k_mutex_init(&linkr_debugger_ws_clients[i].lock);
		k_event_init(&linkr_debugger_ws_clients[i].events);
		k_fifo_init(&linkr_debugger_ws_clients[i].sigrok_stream_fifo);
		k_work_init_delayable(&linkr_debugger_ws_clients[i].sigrok_stream_wake_work,
			sigrok_ws_stream_wake_work_handler);
		linkr_debugger_ws_clients[i].active = false;
		linkr_debugger_ws_client_reset(&linkr_debugger_ws_clients[i]);
	}

	k_mutex_init(&linkr_debugger_ws_sample_ring_lock);
	k_event_init(&linkr_debugger_ws_sampler_events);
	sigrok_ws_burst_pool_init();
	k_sem_init(&linkr_debugger_ws_sampler_pause_ack, 0, 1);
	linkr_debugger_ws_sampler_pause_requested = false;
	linkr_debugger_ws_sigrok_telemetry_pause_requested = false;
	linkr_debugger_ws_arena_quiesced = false;
	memset(&linkr_debugger_ws_sampler_sync, 0,
		sizeof(linkr_debugger_ws_sampler_sync));
	linkr_debugger_ws_latest_sample_sequence = 0U;
	linkr_debugger_capture_arena_register_quiesce_ops(
		&(const struct linkr_debugger_capture_arena_quiesce_ops){
			.quiesce = linkr_debugger_ws_capture_arena_quiesce,
			.resume = linkr_debugger_ws_capture_arena_resume,
		});
	k_thread_create(&linkr_debugger_adc_sampler_thread_data,
			linkr_debugger_adc_sampler_stack,
			K_THREAD_STACK_SIZEOF(linkr_debugger_adc_sampler_stack),
			linkr_debugger_adc_sampler_thread,
			NULL, NULL, NULL, LINKR_DEBUGGER_ADC_SAMPLER_PRIORITY, 0, K_NO_WAIT);
	if (IS_ENABLED(CONFIG_THREAD_NAME)) {
		k_thread_name_set(&linkr_debugger_adc_sampler_thread_data,
				  "linkr_adc_sampler");
	}

	return 0;
}

int linkr_debugger_ws_session_create(struct linkr_debugger_ws_session_info *info)
{
	struct linkr_debugger_ws_client *client;

	linkr_debugger_ws_session_reap_expired();
	client = linkr_debugger_ws_client_allocate();
	if (client == NULL) {
		return -EBUSY;
	}

	if (info != NULL) {
		info->slot = client->slot;
		info->active = true;
		info->connected = false;
		info->session_id = client->session_id;
		snprintk(info->ws_path, sizeof(info->ws_path), "/api/v1/ws/%u", client->slot);
	}

	return 0;
}

int linkr_debugger_ws_session_delete(uint32_t session_id)
{
	struct linkr_debugger_ws_client *client;

	k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
	client = linkr_debugger_ws_client_find_by_session_id(session_id);
	if (client == NULL) {
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);
		return -ENOENT;
	}
	if (client->connected && client->ws_sock >= 0) {
		client->active = false;
		client->connected = false;
		client->telemetry_enabled = false;
		sigrok_ws_stream_cancel_deferred_wake_sync(client);
		k_event_post(&client->events,
			     LINKR_DEBUGGER_WS_EVENT_STATE | LINKR_DEBUGGER_WS_EVENT_SAMPLE);
		(void)zsock_shutdown(client->ws_sock, ZSOCK_SHUT_RDWR);
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);
		k_event_post(&linkr_debugger_ws_sampler_events,
			     LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
		return 0;
	}
	client->active = false;
	sigrok_ws_stream_cancel_deferred_wake_sync(client);
	linkr_debugger_ws_client_reset(client);
	k_event_clear(&client->events, UINT32_MAX);
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);
	k_event_post(&linkr_debugger_ws_sampler_events,
		     LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);

	return 0;
}

int linkr_debugger_ws_session_lookup(uint32_t session_id, struct linkr_debugger_ws_session_info *info)
{
	struct linkr_debugger_ws_client *client;

	linkr_debugger_ws_session_reap_expired();
	k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
	client = linkr_debugger_ws_client_find_by_session_id(session_id);
	if (client == NULL) {
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);
		return -ENOENT;
	}
	if (info != NULL) {
		info->slot = client->slot;
		info->active = client->active;
		info->connected = client->connected;
		info->session_id = client->session_id;
		snprintk(info->ws_path, sizeof(info->ws_path), "/api/v1/ws/%u", client->slot);
	}
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);

	return 0;
}

void linkr_debugger_ws_publish_state_change(void)
{
	linkr_debugger_ws_publish(LINKR_DEBUGGER_WS_EVENT_STATE);
}

void linkr_debugger_ws_publish_sample(void)
{
	linkr_debugger_ws_publish_unbatched_sample();
}
