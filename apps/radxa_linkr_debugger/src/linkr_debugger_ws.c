/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "linkr_debugger_ws.h"

#include "linkr_debugger_control.h"
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
#define LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE 4096
#define LINKR_DEBUGGER_WS_IDLE_WAIT_MS 100
#define LINKR_DEBUGGER_WS_SESSION_IDLE_TIMEOUT_MS 30000U

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
	int value;
	int rate_hz;
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
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, value, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, rate_hz, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, subscribed, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_ws_request, telemetry, JSON_TOK_TRUE),
};

struct linkr_debugger_ws_client {
	uint8_t slot;
	uint32_t session_id;
	int ws_sock;
	bool active;
	bool connected;
	bool telemetry_enabled;
	int telemetry_rate_hz;
	uint32_t sequence;
	int64_t session_created_ms;
	struct k_event events;
	struct k_mutex lock;
	uint8_t recv_buffer[LINKR_DEBUGGER_WS_RECV_BUFFER_SIZE];
	uint8_t tx_buffer[LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE];
	k_tid_t thread;
	struct k_thread thread_data;
};

struct linkr_debugger_ws_client_thread_arg {
	struct linkr_debugger_ws_client *client;
	int ws_sock;
	uint32_t session_id;
};

static struct linkr_debugger_ws_client linkr_debugger_ws_clients[LINKR_DEBUGGER_WS_MAX_CLIENTS];
static struct linkr_debugger_ws_client_thread_arg linkr_debugger_ws_thread_args[LINKR_DEBUGGER_WS_MAX_CLIENTS];
static K_THREAD_STACK_ARRAY_DEFINE(linkr_debugger_ws_stacks, LINKR_DEBUGGER_WS_MAX_CLIENTS,
	LINKR_DEBUGGER_WS_STACK_SIZE);
static struct k_mutex linkr_debugger_ws_clients_lock;
static uint32_t linkr_debugger_ws_next_session_id = 1U;

enum {
	LINKR_DEBUGGER_WS_EVENT_STATE = BIT(0),
	LINKR_DEBUGGER_WS_EVENT_SAMPLE = BIT(1),
};

static int linkr_debugger_ws_send_json(struct linkr_debugger_ws_client *client, const char *payload)
{
	int ret;

	ret = websocket_send_msg(client->ws_sock, (const uint8_t *)payload,
				 strlen(payload), WEBSOCKET_OPCODE_DATA_TEXT,
				 false, true, SYS_FOREVER_MS);
	if (ret < 0) {
		LOG_ERR("websocket_send_msg failed: %d", ret);
		client->telemetry_enabled = false;
	}

	return ret;
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
	linkr_debugger_ws_client_reset(client);
	k_event_clear(&client->events, UINT32_MAX);
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);
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

static int linkr_debugger_ws_append_availability(char *buf, size_t size, size_t *cursor,
					    bool available, const char *reason)
{
	return linkr_debugger_ws_append(buf, size, cursor,
				 "{\"available\":%s,\"reason\":\"%s\"",
				 available ? "true" : "false", reason != NULL ? reason : "");
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
		if (linkr_debugger_ws_append(buf, size, cursor,
					     ",\"source\":\"%s\",\"celsius\":{\"val1\":%d,\"val2\":%d}}",
					     snapshot.temperature.source,
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
		if (linkr_debugger_ws_append(buf, size, cursor,
					     ",\"source\":\"%s\",\"free_bytes\":%u,\"allocated_bytes\":%u,"
					     "\"max_allocated_bytes\":%u,\"total_bytes\":%u}",
					     snapshot.heap.source,
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
					"\"value\":%d,\"direction\":\"%s\"}",
					name, (unsigned int)desc->pin,
					desc->note, value > 0 ? 1 : 0,
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
				 "\"schema\":\"%s\",\"sequence\":%u,\"power_outputs\":[",
				 linkr_debugger_json_schema(), client->sequence++) < 0) {
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
				 "\"switches\":{\"sd\":{\"route\":\"%s\"},\"usb\":{\"route\":\"%s\"}},",
				 linkr_debugger_sd_route_name(), linkr_debugger_usb_route_name()) < 0) {
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

static int linkr_debugger_ws_emit_adc_sample(struct linkr_debugger_ws_client *client)
{
	char *buf = (char *)client->tx_buffer;
	size_t cursor = 0U;

	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
				 "{\"type\":\"telemetry\",\"topic\":\"adc\","
				 "\"schema\":\"%s\",\"sequence\":%u,\"readings\":[",
				 linkr_debugger_json_schema(), client->sequence++) < 0) {
		return -ENOMEM;
	}

	for (size_t i = 0; i < linkr_debugger_current_count; i++) {
		struct linkr_debugger_current_sample sample;
		const struct linkr_debugger_current_desc *current = &linkr_debugger_currents[i];
		int ret;

		ret = linkr_debugger_current_read(current, &sample);
		if (ret < 0) {
			return ret;
		}

		if (i > 0U && linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor, ",") < 0) {
			return -ENOMEM;
		}

		if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor,
					 "{\"name\":\"%s\",\"signal\":\"%s\",\"power_enabled\":%s,"
					 "\"raw\":%d,\"mv\":%d,\"current_ua\":%d}",
					 current->name,
					 current->signal,
					 sample.rail_enabled ? "true" : "false",
					 sample.raw,
					 sample.mv,
					 sample.current_ua) < 0) {
			return -ENOMEM;
		}
	}

	if (linkr_debugger_ws_append(buf, LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE, &cursor, "]}") < 0) {
		return -ENOMEM;
	}

	return linkr_debugger_ws_send_json(client, buf);
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

static int linkr_debugger_ws_handle_control_message(struct linkr_debugger_ws_client *client,
					       const struct linkr_debugger_ws_request *request)
{
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
		} else {
			return linkr_debugger_ws_emit_error(client, "switch", "unknown_switch",
					"switch target must be sd or usb");
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
		if (request.topic[0] != '\0' && strcmp(request.topic, "live") != 0) {
			return linkr_debugger_ws_emit_error(client, "ws", "invalid_topic",
						"topic must be live");
		}
		client->telemetry_enabled = true;
		client->telemetry_rate_hz = request.rate_hz > 0 ? request.rate_hz : 10;
		if (client->telemetry_rate_hz > 1000) {
			client->telemetry_rate_hz = 1000;
		}
		linkr_debugger_ws_publish_state_change();
		linkr_debugger_ws_publish_sample();
		return linkr_debugger_ws_emit_result_and_snapshot(client, request.id, "subscribe", "ok");
	}

	if (strcmp(request.type, "unsubscribe") == 0) {
		client->telemetry_enabled = false;
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
		uint32_t events;
		uint32_t wait_ms;

		k_mutex_lock(&linkr_debugger_ws_clients_lock, K_FOREVER);
		if (!client->active || client->session_id != session_id || !client->connected) {
			k_mutex_unlock(&linkr_debugger_ws_clients_lock);
			break;
		}
		wait_ms = client->telemetry_enabled ?
			(client->telemetry_rate_hz > 0 ? (1000U / (uint32_t)client->telemetry_rate_hz) : 100U) :
			LINKR_DEBUGGER_WS_IDLE_WAIT_MS;
		k_mutex_unlock(&linkr_debugger_ws_clients_lock);

		events = k_event_wait(&client->events,
			LINKR_DEBUGGER_WS_EVENT_STATE | LINKR_DEBUGGER_WS_EVENT_SAMPLE,
			true, K_MSEC(wait_ms));

		if (events & LINKR_DEBUGGER_WS_EVENT_STATE) {
			ret = linkr_debugger_ws_emit_status_snapshot(client);
			if (ret < 0) {
				LOG_ERR("failed to emit status snapshot: %d", ret);
				break;
			}
		}

		if (client->telemetry_enabled) {
			ret = linkr_debugger_ws_emit_adc_sample(client);
			if (ret < 0) {
				LOG_ERR("failed to emit adc sample: %d", ret);
				break;
			}
		}

		ret = websocket_recv_msg(ws_sock,
				 client->recv_buffer,
				 sizeof(client->recv_buffer) - 1U,
				 &message_type,
				 &remaining,
				 client->telemetry_enabled ? 0 : LINKR_DEBUGGER_WS_IDLE_WAIT_MS);
		if (ret == -EAGAIN) {
			continue;
		}
		if (ret < 0) {
			break;
		}
		if ((message_type & WEBSOCKET_FLAG_CLOSE) != 0U) {
			(void)websocket_send_msg(ws_sock, NULL, 0, WEBSOCKET_OPCODE_CLOSE, false, true, 200);
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

	(void)websocket_send_msg(ws_sock, NULL, 0, WEBSOCKET_OPCODE_CLOSE, false, true, 200);
	(void)zsock_shutdown(ws_sock, ZSOCK_SHUT_RDWR);
	k_msleep(500);
	(void)zsock_close(ws_sock);
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
		char thread_name[20];
		snprintk(thread_name, sizeof(thread_name), "linkr_debugger_ws%u", client->slot);
		k_thread_name_set(&client->thread_data, thread_name);
	}
	return 0;
}

int linkr_debugger_ws_init(void)
{
	k_mutex_init(&linkr_debugger_ws_clients_lock);
	for (size_t i = 0; i < ARRAY_SIZE(linkr_debugger_ws_clients); i++) {
		linkr_debugger_ws_clients[i].slot = (uint8_t)i;
		k_mutex_init(&linkr_debugger_ws_clients[i].lock);
		k_event_init(&linkr_debugger_ws_clients[i].events);
		linkr_debugger_ws_clients[i].active = false;
		linkr_debugger_ws_client_reset(&linkr_debugger_ws_clients[i]);
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
		return 0;
	}
	client->active = false;
	linkr_debugger_ws_client_reset(client);
	k_event_clear(&client->events, UINT32_MAX);
	k_mutex_unlock(&linkr_debugger_ws_clients_lock);

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
