/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_ws.h"

#include "linkr_debugger_control.h"
#include "linkr_debugger_logic_analyzer.h"
#include "linkr_debugger_monitoring.h"
#include "linkr_debugger_model.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/websocket.h>
#include <zephyr/posix/poll.h>
#include <zephyr/posix/sys/socket.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(linkr_debugger_ws, LOG_LEVEL_INF);

#define LINKR_DEBUGGER_WS_STACK_SIZE 4096
#define LINKR_DEBUGGER_WS_PRIORITY K_PRIO_PREEMPT(8)
#define LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE 6144
#define LINKR_DEBUGGER_WS_IDLE_WAIT_MS 100
#define LINKR_DEBUGGER_WS_SESSION_IDLE_TIMEOUT_MS 30000U
#define LINKR_DEBUGGER_CAPTURE_CAPACITY 2048U
#define LINKR_DEBUGGER_WS_ADC_CHANNELS 3U
#define LINKR_DEBUGGER_WS_SAMPLE_RING_SIZE 256U
#define LINKR_DEBUGGER_WS_MAX_BATCH_SIZE 20U

BUILD_ASSERT(LINKR_DEBUGGER_WS_SAMPLE_RING_SIZE > LINKR_DEBUGGER_WS_MAX_BATCH_SIZE);

struct linkr_debugger_ws_request {
	char type[16];
	char id[32];
	char command[16];
	char topic[32];
	char output[16];
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

struct linkr_debugger_capture_sample {
	uint64_t device_t_mono_us;
	uint64_t sample_sequence;
	int32_t raw[LINKR_DEBUGGER_WS_ADC_CHANNELS];
	int32_t mv[LINKR_DEBUGGER_WS_ADC_CHANNELS];
	int32_t current_ua[LINKR_DEBUGGER_WS_ADC_CHANNELS];
	uint8_t power_enabled_mask;
};

enum linkr_debugger_capture_state {
	LINKR_DEBUGGER_CAPTURE_IDLE,
	LINKR_DEBUGGER_CAPTURE_ARMED,
	LINKR_DEBUGGER_CAPTURE_TRIGGERED,
	LINKR_DEBUGGER_CAPTURE_SENDING,
};

struct linkr_debugger_capture {
	enum linkr_debugger_capture_state state;
	char trigger[16];
	char source[64];
	char edge[8];
	int32_t threshold_ua;
	uint16_t rate_hz;
	uint16_t pre_samples;
	uint16_t post_samples;
	uint16_t count;
	uint16_t write_index;
	uint16_t oldest_index;
	uint16_t post_collected;
	uint16_t send_offset;
	uint16_t trigger_offset;
	int64_t next_sample_due_us;
	bool last_level;
	bool trigger_pending;
	bool begin_sent;
	uint32_t capture_id;
	struct linkr_debugger_capture_sample samples[LINKR_DEBUGGER_CAPTURE_CAPACITY];
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
	int64_t next_sample_due_us;
	uint64_t sequence;
	int64_t session_created_ms;
	struct k_event events;
	struct k_mutex lock;
	uint8_t recv_buffer[LINKR_DEBUGGER_WS_RECV_BUFFER_SIZE];
	uint8_t tx_buffer[LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE];
	k_tid_t thread;
	struct k_thread thread_data;
};

struct linkr_debugger_ws_adc_sample {
	struct linkr_debugger_current_sample readings[LINKR_DEBUGGER_WS_ADC_CHANNELS];
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
		readings[LINKR_DEBUGGER_CURRENT_BATCH_MAX][LINKR_DEBUGGER_WS_ADC_CHANNELS];
	int64_t timestamps_us[LINKR_DEBUGGER_CURRENT_BATCH_MAX];
	struct linkr_debugger_ws_adc_sample ingest_sample;
};

static struct linkr_debugger_ws_client linkr_debugger_ws_clients[LINKR_DEBUGGER_WS_MAX_CLIENTS];
static struct linkr_debugger_ws_client_thread_arg linkr_debugger_ws_thread_args[LINKR_DEBUGGER_WS_MAX_CLIENTS];
static K_THREAD_STACK_ARRAY_DEFINE(linkr_debugger_ws_stacks, LINKR_DEBUGGER_WS_MAX_CLIENTS,
	LINKR_DEBUGGER_WS_STACK_SIZE);
static struct k_mutex linkr_debugger_ws_clients_lock;
static struct k_mutex linkr_debugger_capture_lock;
static uint32_t linkr_debugger_ws_next_session_id = 1U;
static struct linkr_debugger_capture linkr_debugger_capture;
static uint32_t linkr_debugger_capture_owner_session_id;
static uint32_t linkr_debugger_next_capture_id = 1U;

static struct linkr_debugger_ws_adc_sample
	linkr_debugger_ws_sample_ring[LINKR_DEBUGGER_WS_SAMPLE_RING_SIZE];
static struct linkr_debugger_ws_sampler_workspace linkr_debugger_ws_sampler_workspace;
static struct k_mutex linkr_debugger_ws_sample_ring_lock;
static struct k_event linkr_debugger_ws_sampler_events;
static uint64_t linkr_debugger_ws_latest_sample_sequence;
static K_THREAD_STACK_DEFINE(linkr_debugger_adc_sampler_stack, 4096);
static struct k_thread linkr_debugger_adc_sampler_thread_data;

enum {
	LINKR_DEBUGGER_WS_EVENT_STATE = BIT(0),
	LINKR_DEBUGGER_WS_EVENT_SAMPLE = BIT(1),
};

enum {
	LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG = BIT(0),
};

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
	client->next_sample_due_us = 0;
	client->sequence = 1U;
	client->session_created_ms = 0;
	client->thread = NULL;
}

static void linkr_debugger_ws_stop_stream_if_unwatched(void)
{
	bool any_watcher = false;

	k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(linkr_debugger_ws_clients); i++) {
		struct linkr_debugger_ws_client *c = &linkr_debugger_ws_clients[i];

		if (c->active && c->connected && c->telemetry_enabled) {
			any_watcher = true;
			break;
		}
	}
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);

	/* A stream whose last chunk consumer disconnected would otherwise run
	 * forever, blocking every later start with already_armed.
	 */
	if (!any_watcher && linkr_debugger_logic_analyzer_is_streaming()) {
		(void)linkr_debugger_logic_analyzer_stop_stream();
		LOG_INF("logic stream auto-stopped: no subscribed websocket client");
	}
}

static void linkr_debugger_ws_client_release(struct linkr_debugger_ws_client *client, uint32_t session_id)
{
	k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
	if (client->session_id != session_id) {
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);
		return;
	}
	client->active = false;
	linkr_debugger_ws_client_reset(client);
	k_event_clear(&client->events, UINT32_MAX);
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);

	k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
	if (linkr_debugger_capture_owner_session_id == session_id) {
		memset(&linkr_debugger_capture, 0, sizeof(linkr_debugger_capture));
		linkr_debugger_capture_owner_session_id = 0U;
	}
	k_mutex_unlock(&linkr_debugger_capture_lock);
	k_event_post(&linkr_debugger_ws_sampler_events,
		     LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);

	linkr_debugger_ws_stop_stream_if_unwatched();
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

static int linkr_debugger_ws_append_json_string(char *buf, size_t size, size_t *cursor,
						const char *value)
{
	int ret;

	ret = linkr_debugger_ws_append(buf, size, cursor, "\"");
	if (ret < 0) {
		return ret;
	}

	for (const unsigned char *p = (const unsigned char *)(value != NULL ? value : "");
	     *p != '\0'; p++) {
		switch (*p) {
		case '"':
			ret = linkr_debugger_ws_append(buf, size, cursor, "\\\"");
			break;
		case '\\':
			ret = linkr_debugger_ws_append(buf, size, cursor, "\\\\");
			break;
		case '\b':
			ret = linkr_debugger_ws_append(buf, size, cursor, "\\b");
			break;
		case '\f':
			ret = linkr_debugger_ws_append(buf, size, cursor, "\\f");
			break;
		case '\n':
			ret = linkr_debugger_ws_append(buf, size, cursor, "\\n");
			break;
		case '\r':
			ret = linkr_debugger_ws_append(buf, size, cursor, "\\r");
			break;
		case '\t':
			ret = linkr_debugger_ws_append(buf, size, cursor, "\\t");
			break;
		default:
			if (*p < 0x20U) {
				ret = linkr_debugger_ws_append(buf, size, cursor, "\\u%04x", *p);
			} else {
				ret = linkr_debugger_ws_append(buf, size, cursor, "%c", *p);
			}
			break;
		}
		if (ret < 0) {
			return ret;
		}
	}

	return linkr_debugger_ws_append(buf, size, cursor, "\"");
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
	struct linkr_debugger_watchdog_status watchdog;
	size_t cursor = 0U;

	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
				 "{\"type\":\"snapshot\",\"topic\":\"status\","
				 "\"schema\":\"%s\",\"sequence\":%llu,\"power_outputs\":[",
				 linkr_debugger_json_schema(),
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
				 "\"switches\":{\"sd\":{\"route\":\"%s\"},\"usb\":{\"route\":\"%s\"}",
				 linkr_debugger_sd_route_name(), linkr_debugger_usb_route_name()) < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_vin_switch_available() &&
	    linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
				     ",\"vin\":{\"route\":\"%s\"}", linkr_debugger_vin_route_name()) < 0) {
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
	    linkr_debugger_capture.state != LINKR_DEBUGGER_CAPTURE_IDLE &&
	    linkr_debugger_capture.rate_hz > rate_hz) {
		rate_hz = linkr_debugger_capture.rate_hz;
	}
	k_mutex_unlock(&linkr_debugger_capture_lock);

	return rate_hz;
}

static void linkr_debugger_ws_publish_batch_ready(void)
{
	k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(linkr_debugger_ws_clients); i++) {
		struct linkr_debugger_ws_client *client = &linkr_debugger_ws_clients[i];

		if (client->active && client->connected && client->telemetry_enabled &&
		    client->telemetry_batch_size > 1U) {
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

static void linkr_debugger_ws_adc_sample_to_capture_frame(
	const struct linkr_debugger_ws_adc_sample *sample,
	struct linkr_debugger_capture_sample *frame)
{
	memset(frame, 0, sizeof(*frame));
	frame->device_t_mono_us = (uint64_t)sample->uptime_us;
	frame->sample_sequence = sample->sequence;
	for (size_t i = 0; i < linkr_debugger_current_count; i++) {
		const struct linkr_debugger_current_sample *reading = &sample->readings[i];

		frame->raw[i] = reading->raw;
		frame->mv[i] = reading->mv;
		frame->current_ua[i] = reading->rail_enabled ? reading->current_ua : 0;
		if (reading->rail_enabled) {
			frame->power_enabled_mask |= BIT(i);
		}
	}
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

static void linkr_debugger_capture_store(struct linkr_debugger_capture *capture,
					const struct linkr_debugger_capture_sample *frame)
{
	capture->samples[capture->write_index] = *frame;
	capture->write_index = (uint16_t)((capture->write_index + 1U) % LINKR_DEBUGGER_CAPTURE_CAPACITY);
	if (capture->count < LINKR_DEBUGGER_CAPTURE_CAPACITY) {
		capture->count++;
	} else {
		capture->oldest_index = capture->write_index;
	}
}

static bool linkr_debugger_capture_should_trigger(struct linkr_debugger_capture *capture,
						  const struct linkr_debugger_capture_sample *frame)
{
	bool level = false;
	bool triggered = capture->trigger_pending;

	if (strcmp(capture->trigger, "current") == 0) {
		int index = linkr_debugger_ws_current_index(capture->source);
		triggered = index >= 0 && frame->current_ua[index] >= capture->threshold_ua;
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
			level = (frame->power_enabled_mask & BIT(index)) != 0U;
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
	struct linkr_debugger_capture_sample frame;
	uint32_t wake_session_id = 0U;

	k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
	if (linkr_debugger_capture_owner_session_id == 0U ||
	    linkr_debugger_capture.state == LINKR_DEBUGGER_CAPTURE_IDLE ||
	    linkr_debugger_capture.state == LINKR_DEBUGGER_CAPTURE_SENDING ||
	    !linkr_debugger_capture_should_accept_sample(&linkr_debugger_capture, sample)) {
		k_mutex_unlock(&linkr_debugger_capture_lock);
		return;
	}

	linkr_debugger_ws_adc_sample_to_capture_frame(sample, &frame);
	if (linkr_debugger_capture.state == LINKR_DEBUGGER_CAPTURE_ARMED) {
		if (linkr_debugger_capture_should_trigger(&linkr_debugger_capture, &frame)) {
			linkr_debugger_capture.trigger_offset = linkr_debugger_capture.count;
			linkr_debugger_capture_store(&linkr_debugger_capture, &frame);
			linkr_debugger_capture.state = linkr_debugger_capture.post_samples == 0U ?
				LINKR_DEBUGGER_CAPTURE_SENDING : LINKR_DEBUGGER_CAPTURE_TRIGGERED;
			if (linkr_debugger_capture.state == LINKR_DEBUGGER_CAPTURE_SENDING) {
				wake_session_id = linkr_debugger_capture_owner_session_id;
			}
			k_mutex_unlock(&linkr_debugger_capture_lock);
			if (wake_session_id != 0U) {
				linkr_debugger_ws_wake_session(wake_session_id, LINKR_DEBUGGER_WS_EVENT_SAMPLE);
			}
			return;
		}

		if (linkr_debugger_capture.pre_samples == 0U) {
			k_mutex_unlock(&linkr_debugger_capture_lock);
			return;
		}
		if (linkr_debugger_capture.count >= linkr_debugger_capture.pre_samples) {
			linkr_debugger_capture.oldest_index =
				(uint16_t)((linkr_debugger_capture.oldest_index + 1U) %
					   LINKR_DEBUGGER_CAPTURE_CAPACITY);
			linkr_debugger_capture.count--;
		}
		linkr_debugger_capture_store(&linkr_debugger_capture, &frame);
		k_mutex_unlock(&linkr_debugger_capture_lock);
		return;
	}

	if (linkr_debugger_capture.state == LINKR_DEBUGGER_CAPTURE_TRIGGERED) {
		linkr_debugger_capture_store(&linkr_debugger_capture, &frame);
		linkr_debugger_capture.post_collected++;
		if (linkr_debugger_capture.post_collected >= linkr_debugger_capture.post_samples) {
			linkr_debugger_capture.state = LINKR_DEBUGGER_CAPTURE_SENDING;
			linkr_debugger_capture.send_offset = 0U;
			wake_session_id = linkr_debugger_capture_owner_session_id;
		}
	}
	k_mutex_unlock(&linkr_debugger_capture_lock);

	if (wake_session_id != 0U) {
		linkr_debugger_ws_wake_session(wake_session_id, LINKR_DEBUGGER_WS_EVENT_SAMPLE);
	}
}

static void linkr_debugger_ws_store_adc_samples(
	const struct linkr_debugger_current_sample readings[][LINKR_DEBUGGER_WS_ADC_CHANNELS],
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
		int rate_hz = linkr_debugger_ws_requested_sample_rate();
		uint32_t period_us;
		size_t sample_count;
		int ret;

		if (rate_hz == 0) {
			(void)k_event_wait(&linkr_debugger_ws_sampler_events,
					   LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG,
					   true, K_FOREVER);
			continue;
		}

		period_us = DIV_ROUND_UP(1000000, rate_hz);
		if (rate_hz <= 100) {
			uint32_t events = k_event_wait(&linkr_debugger_ws_sampler_events,
						       LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG,
						       true, K_USEC(period_us));

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
			LINKR_DEBUGGER_WS_ADC_CHANNELS,
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
		if (sample_count > 1U) {
			int64_t now_us;
			int64_t next_batch_us;
			uint32_t events = 0U;

			linkr_debugger_ws_publish_batch_ready();
			next_batch_us = linkr_debugger_ws_sampler_workspace
				.timestamps_us[sample_count - 1U] + period_us;
			now_us = k_ticks_to_us_floor64(k_uptime_ticks());
			if (now_us < next_batch_us - tick_us) {
				events = k_event_wait(&linkr_debugger_ws_sampler_events,
						      LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG,
						      true,
						      K_USEC(next_batch_us - now_us - tick_us));
			}
			if (events == 0U) {
				now_us = k_ticks_to_us_floor64(k_uptime_ticks());
				if (now_us < next_batch_us) {
					k_busy_wait((uint32_t)(next_batch_us - now_us));
				}
			}
		} else {
			linkr_debugger_ws_publish(LINKR_DEBUGGER_WS_EVENT_SAMPLE);
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

static int linkr_debugger_ws_append_adc_readings(char *buf, size_t size, size_t *cursor,
					  const struct linkr_debugger_current_sample *readings)
{
	for (size_t i = 0; i < linkr_debugger_current_count; i++) {
		const struct linkr_debugger_current_desc *current = &linkr_debugger_currents[i];
		const struct linkr_debugger_current_sample *sample = &readings[i];

		if (i > 0U && linkr_debugger_ws_append(buf, size, cursor, ",") < 0) {
			return -ENOMEM;
		}
		if (linkr_debugger_ws_append(buf, size, cursor,
					 "{\"name\":\"%s\",\"signal\":\"%s\",\"power_enabled\":%s,"
					 "\"raw\":%d,\"mv\":%d,\"current_ua\":%d}",
					 current->name, current->signal,
					 sample->rail_enabled ? "true" : "false",
					 sample->raw, sample->mv, sample->current_ua) < 0) {
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
			for (size_t i = 0; i < linkr_debugger_current_count; i++) {
				if (i > 0U && linkr_debugger_ws_append(buf,
								    LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
								    &cursor, ",") < 0) {
					return -ENOMEM;
				}
				if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
							 &cursor,
							 "{\"name\":\"%s\",\"signal\":\"%s\"}",
							 linkr_debugger_currents[i].name,
							 linkr_debugger_currents[i].signal) < 0) {
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
		if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
					 "{\"sequence\":%llu,\"uptime_us\":%lld,\"values\":[",
					 (unsigned long long)sample.sequence,
					 (long long)sample.uptime_us) < 0) {
			return -ENOMEM;
		}
		for (size_t j = 0; j < linkr_debugger_current_count; j++) {
			const struct linkr_debugger_current_sample *reading = &sample.readings[j];

			if (j > 0U && linkr_debugger_ws_append(buf,
							    LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
							    &cursor, ",") < 0) {
				return -ENOMEM;
			}
			if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
						     &cursor, "[%d,%d,%d,%d]",
						     reading->rail_enabled ? 1 : 0,
						     reading->raw, reading->mv,
						     reading->current_ua) < 0) {
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
		  linkr_debugger_capture.state == LINKR_DEBUGGER_CAPTURE_SENDING;
	k_mutex_unlock(&linkr_debugger_capture_lock);

	return pending;
}

static int linkr_debugger_ws_emit_capture_begin(struct linkr_debugger_ws_client *client)
{
	struct linkr_debugger_capture *capture = &linkr_debugger_capture;
	char *buf = (char *)client->tx_buffer;

	snprintk(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
		 "{\"type\":\"capture_begin\",\"schema\":\"%s\",\"capture_id\":%u,"
		 "\"trigger\":\"%.*s\",\"source\":\"%.*s\",\"edge\":\"%.*s\","
		 "\"threshold_ua\":%d,\"pre_samples\":%u,\"post_samples\":%u,"
		 "\"sample_count\":%u,\"trigger_offset\":%u,\"rate_hz\":%u}",
		 linkr_debugger_json_schema(), (unsigned int)capture->capture_id,
		 (int)sizeof(capture->trigger) - 1, capture->trigger,
		 (int)sizeof(capture->source) - 1, capture->source,
		 (int)sizeof(capture->edge) - 1, capture->edge,
		 (int)capture->threshold_ua, (unsigned int)capture->pre_samples,
		 (unsigned int)capture->post_samples,
		 (unsigned int)capture->count,
		 (unsigned int)capture->trigger_offset, (unsigned int)capture->rate_hz);
	return linkr_debugger_ws_send_json(client, buf);
}

static int linkr_debugger_ws_emit_capture_sample(struct linkr_debugger_ws_client *client,
						 uint16_t offset)
{
	struct linkr_debugger_capture *capture = &linkr_debugger_capture;
	uint16_t index = (uint16_t)((capture->oldest_index + offset) %
					    LINKR_DEBUGGER_CAPTURE_CAPACITY);
	const struct linkr_debugger_capture_sample *frame = &capture->samples[index];
	char *buf = (char *)client->tx_buffer;
	size_t cursor = 0U;

	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
				 "{\"type\":\"capture_sample\",\"schema\":\"%s\","
				 "\"capture_id\":%u,\"offset\":%u,\"triggered\":%s,"
				 "\"sample_sequence\":%llu,\"device_t_mono_us\":%llu,\"readings\":[",
				 linkr_debugger_json_schema(), (unsigned int)capture->capture_id,
				 (unsigned int)offset, offset == capture->trigger_offset ? "true" : "false",
				 (unsigned long long)frame->sample_sequence,
				 (unsigned long long)frame->device_t_mono_us) < 0) {
		return -ENOMEM;
	}

	for (size_t i = 0; i < linkr_debugger_current_count; i++) {
		if (i > 0U && linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
							 &cursor, ",") < 0) {
			return -ENOMEM;
		}
		if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
					 "{\"name\":\"%s\",\"power_enabled\":%s,\"current_ua\":%d}",
					 linkr_debugger_currents[i].name,
					 (frame->power_enabled_mask & BIT(i)) != 0U ? "true" : "false",
					 frame->current_ua[i]) < 0) {
			return -ENOMEM;
		}
	}

	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor, "]}") < 0) {
		return -ENOMEM;
	}
	return linkr_debugger_ws_send_json(client, buf);
}

static int linkr_debugger_ws_emit_capture_pending(struct linkr_debugger_ws_client *client)
{
	struct linkr_debugger_capture *capture = &linkr_debugger_capture;
	char *buf = (char *)client->tx_buffer;
	int ret;

	if (linkr_debugger_capture_owner_session_id != client->session_id) {
		return 0;
	}
	if (capture->state != LINKR_DEBUGGER_CAPTURE_SENDING) {
		return 0;
	}
	if (!capture->begin_sent) {
		ret = linkr_debugger_ws_emit_capture_begin(client);
		if (ret < 0) {
			return ret;
		}
		capture->begin_sent = true;
	}
	if (capture->send_offset < capture->count) {
		ret = linkr_debugger_ws_emit_capture_sample(client, capture->send_offset);
		if (ret < 0) {
			return ret;
		}
		capture->send_offset++;
		return 0;
	}

	snprintk(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE,
		 "{\"type\":\"capture_complete\",\"schema\":\"%s\","
		 "\"capture_id\":%u,\"sample_count\":%u,\"trigger_offset\":%u}",
		 linkr_debugger_json_schema(), (unsigned int)capture->capture_id,
		 (unsigned int)capture->count, (unsigned int)capture->trigger_offset);
	ret = linkr_debugger_ws_send_json(client, buf);
	if (ret >= 0) {
		k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
		capture->state = LINKR_DEBUGGER_CAPTURE_IDLE;
		linkr_debugger_capture_owner_session_id = 0U;
		k_mutex_unlock(&linkr_debugger_capture_lock);
		k_event_post(&linkr_debugger_ws_sampler_events,
			     LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
	}
	return ret;
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
	struct linkr_debugger_capture *capture = &linkr_debugger_capture;
	int pre_samples = request->pre_samples;
	int post_samples = request->post_samples != 0 ? request->post_samples : 1;
	int rate_hz = request->rate_hz > 0 ? request->rate_hz : 100;
	const char *trigger = request->trigger[0] != '\0' ? request->trigger : "manual";

	if (rate_hz < 1 || rate_hz > 1000) {
		return linkr_debugger_ws_emit_error(client, "capture", "invalid_rate",
						"rate_hz must be between 1 and 1000");
	}
	if (pre_samples < 0 || post_samples < 0 ||
	    (size_t)pre_samples + (size_t)post_samples + 1U > LINKR_DEBUGGER_CAPTURE_CAPACITY) {
		return linkr_debugger_ws_emit_error(client, "capture", "capture_too_large",
						"pre_samples + post_samples + 1 exceeds capture capacity");
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
	capture->pre_samples = (uint16_t)pre_samples;
	capture->post_samples = (uint16_t)post_samples;
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
		    linkr_debugger_capture.state != LINKR_DEBUGGER_CAPTURE_ARMED) {
			k_mutex_unlock(&linkr_debugger_capture_lock);
			return linkr_debugger_ws_emit_error(client, "capture", "not_armed",
							"capture is not armed");
		}
		linkr_debugger_capture.trigger_pending = true;
		k_mutex_unlock(&linkr_debugger_capture_lock);
		k_event_post(&linkr_debugger_ws_sampler_events,
			     LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
		return linkr_debugger_ws_emit_result(client, request->id, "capture_trigger", "triggered");
	}
	if (strcmp(request->command, "capture_cancel") == 0) {
		k_mutex_lock(&linkr_debugger_capture_lock, K_FOREVER);
		if (linkr_debugger_capture_owner_session_id != client->session_id) {
			k_mutex_unlock(&linkr_debugger_capture_lock);
			return linkr_debugger_ws_emit_error(client, "capture", "not_owner",
							"this session does not own the active capture");
		}
		memset(&linkr_debugger_capture, 0, sizeof(linkr_debugger_capture));
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
		} else if (strcmp(request->output, "vin") == 0 && linkr_debugger_vin_switch_available()) {
			enum linkr_debugger_vin_route route;
			if (!linkr_debugger_parse_vin_route(request->route, &route)) {
				return linkr_debugger_ws_emit_error(client, "switch", "invalid_route",
						"vin route must be 1.8v or 3.3v");
			}
			ret = linkr_debugger_vin_route_set(route);
		} else {
			return linkr_debugger_ws_emit_error(client, "switch", "unknown_switch",
					"switch target must be sd, usb, or an available vin switch");
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
		int ret;

		gpio = linkr_debugger_find_safe_gpio_by_identifier(request->gpio);
		if (gpio == NULL) {
			return linkr_debugger_ws_emit_error(client, "gpio", "invalid_gpio",
					"GPIO target must be GP13, 13, or an allowlist note such as CON_MAS");
		}

		if (strcmp(request->direction, "input") == 0) {
			ret = linkr_debugger_gpio_set_input(gpio);
		} else if (strcmp(request->direction, "output") == 0) {
			ret = linkr_debugger_gpio_set_output(gpio, request->value != 0);
		} else {
			return linkr_debugger_ws_emit_error(client, "gpio", "invalid_request",
						"direction must be input or output");
		}

		if (ret < 0) {
			return linkr_debugger_ws_emit_error(client, "gpio", "configure_failed",
						"failed to configure GPIO");
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
				 linkr_debugger_capture.state != LINKR_DEBUGGER_CAPTURE_IDLE;
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
		client->next_sample_due_us = 0;
		client->telemetry_enabled = true;
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);
		k_event_post(&linkr_debugger_ws_sampler_events,
			     LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
		linkr_debugger_ws_publish_state_change();
		linkr_debugger_ws_publish_sample();
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
		wait_ms = capture_pending ? 1U : client->telemetry_enabled ?
			MAX(1U, DIV_ROUND_UP(1000U * client->telemetry_batch_size,
					       (uint32_t)client->telemetry_rate_hz)) :
			LINKR_DEBUGGER_WS_IDLE_WAIT_MS;

		/* wait_safe consumes only the reported bits: a plain k_event_wait
		 * with reset=true wipes pending bits unreported at entry and on
		 * timeout, which could permanently lose one-shot event posts.
		 */
		events = k_event_wait_safe(&client->events,
			LINKR_DEBUGGER_WS_EVENT_STATE | LINKR_DEBUGGER_WS_EVENT_SAMPLE,
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

		ret = websocket_recv_msg(ws_sock,
				 client->recv_buffer,
				 sizeof(client->recv_buffer) - 1U,
				 &message_type,
				 &remaining,
				 (client->telemetry_enabled || capture_pending) ?
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
		if ((message_type & WEBSOCKET_FLAG_TEXT) == 0U) {
			(void)linkr_debugger_ws_emit_error(client, "ws", "unsupported_frame",
						"only text websocket frames are supported");
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

int linkr_debugger_ws_init(void)
{
k_mutex_init(&linkr_debugger_ws_clients_lock);
k_mutex_init(&linkr_debugger_capture_lock);
	for (size_t i = 0; i < ARRAY_SIZE(linkr_debugger_ws_clients); i++) {
		linkr_debugger_ws_clients[i].slot = (uint8_t)i;
		k_mutex_init(&linkr_debugger_ws_clients[i].lock);
		k_event_init(&linkr_debugger_ws_clients[i].events);
		linkr_debugger_ws_clients[i].active = false;
		linkr_debugger_ws_client_reset(&linkr_debugger_ws_clients[i]);
	}

	k_mutex_init(&linkr_debugger_ws_sample_ring_lock);
	k_event_init(&linkr_debugger_ws_sampler_events);
	linkr_debugger_ws_latest_sample_sequence = 0U;
	k_thread_create(&linkr_debugger_adc_sampler_thread_data,
			linkr_debugger_adc_sampler_stack,
			K_THREAD_STACK_SIZEOF(linkr_debugger_adc_sampler_stack),
			linkr_debugger_adc_sampler_thread,
			NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
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
		k_event_post(&client->events,
			     LINKR_DEBUGGER_WS_EVENT_STATE | LINKR_DEBUGGER_WS_EVENT_SAMPLE);
		(void)zsock_shutdown(client->ws_sock, ZSOCK_SHUT_RDWR);
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);
		k_event_post(&linkr_debugger_ws_sampler_events,
			     LINKR_DEBUGGER_WS_SAMPLER_EVENT_CONFIG);
		return 0;
	}
	client->active = false;
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
	linkr_debugger_ws_publish(LINKR_DEBUGGER_WS_EVENT_SAMPLE);
}

