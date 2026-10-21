/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_serial_http.h"

#include "linkr_debugger_http_body.h"
#include "linkr_debugger_control.h"
#include "linkr_debugger_model.h"
#include "linkr_debugger_serial.h"
#include "linkr_debugger_serial_net.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#define SERIAL_HTTP_JSON_CAPACITY 4096U
#define SERIAL_HTTP_BODY_ROUTE 0x4a34U

struct serial_http_config_request {
	int64_t baud;
	int64_t data_bits;
	char parity[8];
	int64_t stop_bits;
	char flow_control[12];
	uint64_t expected_generation;
};

struct serial_http_poison_slot {
	uintptr_t client_key;
	uint16_t method_id;
	bool active;
};

struct serial_http_buffer {
	char *data;
	size_t capacity;
	size_t length;
};

static const struct json_obj_descr serial_http_config_request_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct serial_http_config_request, baud,
			    JSON_TOK_INT64),
	JSON_OBJ_DESCR_PRIM(struct serial_http_config_request, data_bits,
			    JSON_TOK_INT64),
	JSON_OBJ_DESCR_PRIM(struct serial_http_config_request, parity,
			    JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct serial_http_config_request, stop_bits,
			    JSON_TOK_INT64),
	JSON_OBJ_DESCR_PRIM(struct serial_http_config_request, flow_control,
			    JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct serial_http_config_request,
			    expected_generation, JSON_TOK_UINT64),
};

static uint8_t serial_http_json[SERIAL_HTTP_JSON_CAPACITY];
static struct serial_http_poison_slot
	serial_http_poison[CONFIG_HTTP_SERVER_MAX_CLIENTS];
K_MUTEX_DEFINE(serial_http_lock);

static struct serial_http_poison_slot *poison_find(uintptr_t client_key,
						    uint16_t method_id)
{
	for (size_t i = 0U; i < ARRAY_SIZE(serial_http_poison); ++i) {
		if (serial_http_poison[i].active &&
		    serial_http_poison[i].client_key == client_key &&
		    serial_http_poison[i].method_id == method_id) {
			return &serial_http_poison[i];
		}
	}
	return NULL;
}

static bool poison_mark(uintptr_t client_key, uint16_t method_id)
{
	struct serial_http_poison_slot *slot = poison_find(client_key, method_id);

	if (slot != NULL) {
		return true;
	}
	for (size_t i = 0U; i < ARRAY_SIZE(serial_http_poison); ++i) {
		if (!serial_http_poison[i].active) {
			serial_http_poison[i].active = true;
			serial_http_poison[i].client_key = client_key;
			serial_http_poison[i].method_id = method_id;
			return true;
		}
	}
	return false;
}

static void poison_clear(uintptr_t client_key, uint16_t method_id)
{
	struct serial_http_poison_slot *slot = poison_find(client_key, method_id);

	if (slot != NULL) {
		memset(slot, 0, sizeof(*slot));
	}
}

static enum linkr_debugger_http_body_event body_event_from_status(
	enum http_transaction_status status)
{
	switch (status) {
	case HTTP_SERVER_REQUEST_DATA_MORE:
		return LINKR_DEBUGGER_HTTP_BODY_MORE;
	case HTTP_SERVER_REQUEST_DATA_FINAL:
		return LINKR_DEBUGGER_HTTP_BODY_FINAL;
	case HTTP_SERVER_TRANSACTION_ABORTED:
		return LINKR_DEBUGGER_HTTP_BODY_ABORTED;
	case HTTP_SERVER_TRANSACTION_COMPLETE:
		return LINKR_DEBUGGER_HTTP_BODY_COMPLETE;
	default:
		return LINKR_DEBUGGER_HTTP_BODY_ABORTED;
	}
}

static int buffer_append(struct serial_http_buffer *buffer, const char *format,
			 ...)
{
	va_list args;
	int written;

	if (buffer->length >= buffer->capacity) {
		return -ENOMEM;
	}
	va_start(args, format);
	written = vsnprintk(buffer->data + buffer->length,
			    buffer->capacity - buffer->length, format, args);
	va_end(args);
	if (written < 0 || (size_t)written >=
			     buffer->capacity - buffer->length) {
		buffer->data[buffer->capacity - 1U] = '\0';
		return -ENOMEM;
	}
	buffer->length += (size_t)written;
	return 0;
}

static int buffer_json_string(struct serial_http_buffer *buffer,
			      const char *value)
{
	const unsigned char *cursor = (const unsigned char *)(value != NULL ?
								value : "");

	if (buffer_append(buffer, "\"") < 0) {
		return -ENOMEM;
	}
	while (*cursor != '\0') {
		switch (*cursor) {
		case '"':
			if (buffer_append(buffer, "\\\"") < 0) {
				return -ENOMEM;
			}
			break;
		case '\\':
			if (buffer_append(buffer, "\\\\") < 0) {
				return -ENOMEM;
			}
			break;
		case '\b':
			if (buffer_append(buffer, "\\b") < 0) {
				return -ENOMEM;
			}
			break;
		case '\f':
			if (buffer_append(buffer, "\\f") < 0) {
				return -ENOMEM;
			}
			break;
		case '\n':
			if (buffer_append(buffer, "\\n") < 0) {
				return -ENOMEM;
			}
			break;
		case '\r':
			if (buffer_append(buffer, "\\r") < 0) {
				return -ENOMEM;
			}
			break;
		case '\t':
			if (buffer_append(buffer, "\\t") < 0) {
				return -ENOMEM;
			}
			break;
		default:
			if (*cursor < 0x20U) {
				if (buffer_append(buffer, "\\u%04x",
						  (unsigned int)*cursor) < 0) {
					return -ENOMEM;
				}
			} else if (buffer_append(buffer, "%c", *cursor) < 0) {
				return -ENOMEM;
			}
			break;
		}
		cursor++;
	}
	return buffer_append(buffer, "\"");
}

static void set_json_response(struct http_response_ctx *response,
			      enum http_status status,
			      struct serial_http_buffer *buffer)
{
	static const struct http_header headers[] = {
		{ .name = "Cache-Control", .value = "no-store" },
	};

	response->status = status;
	response->headers = headers;
	response->header_count = ARRAY_SIZE(headers);
	response->body = (const uint8_t *)buffer->data;
	response->body_len = buffer->length;
	response->final_chunk = true;
}

static void set_error(struct http_response_ctx *response,
		      enum http_status status, const char *code,
		      const char *message)
{
	struct serial_http_buffer buffer = {
		.data = (char *)serial_http_json,
		.capacity = sizeof(serial_http_json),
	};

	(void)buffer_append(&buffer, "{\"schema\":");
	(void)buffer_json_string(&buffer, linkr_debugger_json_schema());
	(void)buffer_append(&buffer,
			    ",\"ok\":false,\"command\":\"serial\","
			    "\"port\":\"j4\",\"error\":{\"code\":");
	(void)buffer_json_string(&buffer, code);
	(void)buffer_append(&buffer, ",\"message\":");
	(void)buffer_json_string(&buffer, message);
	(void)buffer_append(&buffer, "}}\n");
	set_json_response(response, status, &buffer);
}

static const char *state_name(enum linkr_debugger_serial_state state)
{
	switch (state) {
	case LINKR_DEBUGGER_SERIAL_READY:
		return "ready";
	case LINKR_DEBUGGER_SERIAL_RECONFIG:
		return "reconfig";
	case LINKR_DEBUGGER_SERIAL_PAUSED:
		return "paused";
	case LINKR_DEBUGGER_SERIAL_FAULTED:
	default:
		return "faulted";
	}
}

static const char *parity_name(enum linkr_debugger_serial_parity parity)
{
	switch (parity) {
	case LINKR_DEBUGGER_SERIAL_PARITY_ODD:
		return "odd";
	case LINKR_DEBUGGER_SERIAL_PARITY_EVEN:
		return "even";
	case LINKR_DEBUGGER_SERIAL_PARITY_NONE:
	default:
		return "none";
	}
}

static int encode_endpoint_counters(
	struct serial_http_buffer *buffer,
	const struct linkr_debugger_serial_net_endpoint_counters *endpoint)
{
	return buffer_append(
		buffer,
		"{\"connected\":%llu,\"disconnected\":%llu,"
		"\"rx_bytes\":%llu,\"rx_dropped\":%llu,\"tx_bytes\":%llu,"
		"\"rx_messages\":%llu,\"queue_waits\":%llu,"
		"\"send_would_block\":%llu,\"send_failures\":%llu,"
		"\"drops\":%llu,\"stale_completions\":%llu}",
		(unsigned long long)endpoint->connected,
		(unsigned long long)endpoint->disconnected,
		(unsigned long long)endpoint->rx_bytes,
		(unsigned long long)endpoint->rx_dropped,
		(unsigned long long)endpoint->tx_bytes,
		(unsigned long long)endpoint->rx_messages,
		(unsigned long long)endpoint->tx_queue_waits,
		(unsigned long long)endpoint->send_would_block,
		(unsigned long long)endpoint->send_failures,
		(unsigned long long)endpoint->inferred_rx_drops,
		(unsigned long long)endpoint->stale_completions);
}

static int encode_status(struct serial_http_buffer *buffer)
{
	struct linkr_debugger_serial_status serial;
	struct linkr_debugger_serial_net_status net;
	struct linkr_debugger_serial_terminal_responder responder;
	char responder_json[64] = "null";
	uint8_t cdc_sink;
	int ret;

	ret = linkr_debugger_serial_status_snapshot(&serial);
	if (ret < 0) {
		if (buffer_append(buffer, "{\"schema\":") < 0 ||
		    buffer_json_string(buffer, linkr_debugger_json_schema()) < 0 ||
		    buffer_append(buffer,
			    ",\"ok\":true,\"command\":\"serial\","
			    "\"port\":\"j4\",\"available\":false,"
			    "\"state\":\"unavailable\"}\n") < 0) {
			return -ENOMEM;
		}
		return 0;
	}
	(void)linkr_debugger_serial_net_status_snapshot(&net);
	responder = net.terminal_responder;
	if (serial.dtr) {
		responder = (struct linkr_debugger_serial_terminal_responder){
			.valid = true,
			.transport = LINKR_DEBUGGER_SERIAL_TERMINAL_RESPONDER_CDC,
			.slot = 0,
		};
	}
	cdc_sink = serial.dtr ? 1U : 0U;
	if (responder.valid) {
		const char *transport =
			responder.transport ==
					LINKR_DEBUGGER_SERIAL_TERMINAL_RESPONDER_CDC ?
				"cdc" : "websocket";
		int length = snprintk(responder_json, sizeof(responder_json),
			"{\"transport\":\"%s\",\"slot\":%d}", transport,
			(int)responder.slot);

		if (length < 0 || (size_t)length >= sizeof(responder_json)) {
			return -ENOMEM;
		}
	}

	/* line_coding plus generation is the authoritative applied state;
	 * config_request only describes the most recent Web request.
	 */
	if (buffer_append(buffer, "{\"schema\":") < 0 ||
	    buffer_json_string(buffer, linkr_debugger_json_schema()) < 0 ||
	    buffer_append(
		    buffer,
		    ",\"ok\":true,\"command\":\"serial\","
		    "\"port\":\"j4\",\"available\":true,\"state\":") < 0 ||
	    buffer_json_string(buffer, state_name(serial.state)) < 0 ||
	    buffer_append(buffer,
		    ",\"line_coding\":{\"baud\":%u,\"data_bits\":%u,"
		    "\"parity\":",
		    (unsigned int)serial.current_config.baud,
		    (unsigned int)serial.current_config.data_bits) < 0 ||
	    buffer_json_string(buffer, parity_name(serial.current_config.parity)) <
		    0 ||
	    buffer_append(
		    buffer,
		    ",\"stop_bits\":%u,\"flow_control\":\"none\"},"
		    "\"generation\":%llu,\"dtr\":%s,\"rts\":%s,"
		    "\"usb_configured\":%s,\"terminal_responder\":%s,"
		    "\"responder_generation\":%llu,"
		    "\"sink_count\":{\"total\":%u,\"cdc\":%u,"
		    "\"ws\":%u,\"tcp\":%u},"
		    "\"queue\":{\"used\":%u,\"capacity\":%u,"
		    "\"active_event_id\":%llu,\"admission_frozen\":%s},"
		    "\"config_request\":{\"id\":%llu,\"done\":%s,"
		    "\"result\":%d},"
		    "\"config_counters\":{\"cdc_requests\":%llu,"
		    "\"web_requests\":%llu,\"invalid\":%llu,"
		    "\"conflicts\":%llu,\"no_change\":%llu,"
		    "\"configure_count\":%llu,\"apply_succeeded\":%llu,"
		    "\"apply_failed\":%llu,\"rollback_succeeded\":%llu,"
		    "\"rollback_failed\":%llu},"
		    "\"counters\":{\"uart_rx_bytes\":%llu,"
		    "\"uart_tx_bytes\":%llu,\"tx_accepted\":%llu,"
		    "\"tx_completed\":%llu,\"tx_canceled\":%llu,"
		    "\"completion_overflow\":%llu,"
		    "\"invalid_line_coding\":%llu,"
		    "\"config_mailbox_busy\":%llu,"
		    "\"listener_failures\":%llu,\"accept_failures\":%llu,"
		    "\"refused_clients\":%llu},"
		    "\"drops\":{\"uart_pause\":%llu,\"cdc_rx\":%llu,"
		    "\"cdc_tx_stale\":%llu},"
		    "\"raw_tcp\":{\"bind\":\"0.0.0.0\",\"port\":%u,"
		    "\"max_clients\":%u,\"connected\":%u,"
		    "\"listening\":%s,\"clients\":[",
		    (unsigned int)serial.current_config.stop_bits,
		    (unsigned long long)serial.generation,
		    serial.dtr ? "true" : "false",
		    serial.rts ? "true" : "false",
		    serial.usb_configured ? "true" : "false",
		    responder_json,
		    (unsigned long long)serial.cdc_responder_generation,
		    (unsigned int)(cdc_sink + net.ws_attached + net.tcp_connected),
		    (unsigned int)cdc_sink, (unsigned int)net.ws_attached,
		    (unsigned int)net.tcp_connected,
		    (unsigned int)serial.tx_used_slots,
		    (unsigned int)LINKR_DEBUGGER_SERIAL_TX_SLOT_COUNT,
		    (unsigned long long)serial.active_event_id,
		    serial.admission_frozen ? "true" : "false",
		    (unsigned long long)serial.last_config_request_id,
		    serial.last_config_done ? "true" : "false",
		    serial.last_config_result,
		    (unsigned long long)serial.config_counters.cdc_requests,
		    (unsigned long long)serial.config_counters.web_requests,
		    (unsigned long long)serial.config_counters.invalid,
		    (unsigned long long)serial.config_counters.conflicts,
		    (unsigned long long)serial.config_counters.no_change,
		    (unsigned long long)serial.config_counters.configure_count,
		    (unsigned long long)serial.config_counters.apply_succeeded,
		    (unsigned long long)serial.config_counters.apply_failed,
		    (unsigned long long)serial.config_counters.rollback_succeeded,
		    (unsigned long long)serial.config_counters.rollback_failed,
		    (unsigned long long)serial.runtime_counters.uart_rx_bytes,
		    (unsigned long long)serial.tx_bytes_committed,
		    (unsigned long long)serial.tx_accepted,
		    (unsigned long long)serial.tx_completed,
		    (unsigned long long)serial.tx_canceled,
		    (unsigned long long)serial.runtime_counters.completion_overflow,
		    (unsigned long long)serial.runtime_counters.invalid_line_coding,
		    (unsigned long long)serial.runtime_counters.config_mailbox_busy,
		    (unsigned long long)net.counters.listener_failures,
		    (unsigned long long)net.counters.accept_failures,
		    (unsigned long long)net.counters.refused_clients,
		    (unsigned long long)serial.runtime_counters.uart_rx_pause_dropped,
		    (unsigned long long)serial.runtime_counters.cdc_rx_dropped,
		    (unsigned long long)serial.runtime_counters.cdc_tx_stale_dropped,
		    (unsigned int)LINKR_DEBUGGER_SERIAL_RAW_TCP_PORT,
		    (unsigned int)LINKR_DEBUGGER_SERIAL_RAW_TCP_MAX_CLIENTS,
		    (unsigned int)net.tcp_connected,
		    net.listening ? "true" : "false") < 0) {
		return -ENOMEM;
	}
	for (size_t i = 0U; i < ARRAY_SIZE(net.tcp); ++i) {
		if ((i > 0U && buffer_append(buffer, ",") < 0) ||
		    encode_endpoint_counters(buffer, &net.tcp[i]) < 0) {
			return -ENOMEM;
		}
	}
	if (buffer_append(buffer, "]},\"websocket\":{\"attached\":%u,"
				  "\"clients\":[",
			  (unsigned int)net.ws_attached) < 0) {
		return -ENOMEM;
	}
	for (size_t i = 0U; i < ARRAY_SIZE(net.ws); ++i) {
		if ((i > 0U && buffer_append(buffer, ",") < 0) ||
		    encode_endpoint_counters(buffer, &net.ws[i]) < 0) {
			return -ENOMEM;
		}
	}
	return buffer_append(buffer, "]}}\n");
}

static int decode_config(const uint8_t *data, size_t length,
			 struct serial_http_config_request *request,
			 struct linkr_debugger_serial_config *config)
{
	int64_t fields;

	memset(request, 0, sizeof(*request));
	fields = json_obj_parse((char *)data, length,
				serial_http_config_request_descr,
				ARRAY_SIZE(serial_http_config_request_descr),
				request);
	if (fields != (int64_t)(BIT64(0) | BIT64(1) | BIT64(2) | BIT64(3) |
				 BIT64(4) | BIT64(5))) {
		return -EINVAL;
	}
	if (strcmp(request->parity, "none") == 0) {
		config->parity = LINKR_DEBUGGER_SERIAL_PARITY_NONE;
	} else if (strcmp(request->parity, "odd") == 0) {
		config->parity = LINKR_DEBUGGER_SERIAL_PARITY_ODD;
	} else if (strcmp(request->parity, "even") == 0) {
		config->parity = LINKR_DEBUGGER_SERIAL_PARITY_EVEN;
	} else {
		return -EINVAL;
	}
	if (strcmp(request->flow_control, "none") != 0) {
		return -EINVAL;
	}
	if (request->baud < LINKR_DEBUGGER_SERIAL_BAUD_MIN ||
	    request->baud > LINKR_DEBUGGER_SERIAL_BAUD_MAX ||
	    request->data_bits < 5 || request->data_bits > 8 ||
	    request->stop_bits < 1 || request->stop_bits > 2) {
		return -EINVAL;
	}
	config->baud = (uint32_t)request->baud;
	config->data_bits = (uint8_t)request->data_bits;
	config->stop_bits = (uint8_t)request->stop_bits;
	config->flow_control = LINKR_DEBUGGER_SERIAL_FLOW_NONE;
	return linkr_debugger_serial_config_valid(config) ? 0 : -EINVAL;
}

int linkr_debugger_serial_http_handle(
	struct http_client_ctx *client, enum http_transaction_status status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response, void *user_data)
{
	const enum linkr_debugger_serial_http_route route =
		user_data != NULL ?
			*(const enum linkr_debugger_serial_http_route *)user_data :
			LINKR_DEBUGGER_SERIAL_HTTP_ROUTE_STATUS;
	struct linkr_debugger_http_body_view body = { 0 };
	struct serial_http_config_request decoded;
	struct linkr_debugger_serial_config config;
	struct linkr_debugger_serial_status serial;
	struct serial_http_buffer buffer = {
		.data = (char *)serial_http_json,
		.capacity = sizeof(serial_http_json),
	};
	enum linkr_debugger_http_body_event body_event =
		body_event_from_status(status);
	enum linkr_debugger_http_body_result body_result;
	uint64_t request_id;
	int ret;

	if (route == LINKR_DEBUGGER_SERIAL_HTTP_ROUTE_CONFIG &&
	    client->method == HTTP_PUT) {
		bool poisoned;

		k_mutex_lock(&serial_http_lock, K_FOREVER);
		if (body_event == LINKR_DEBUGGER_HTTP_BODY_ABORTED ||
		    body_event == LINKR_DEBUGGER_HTTP_BODY_COMPLETE) {
			poison_clear((uintptr_t)client, (uint16_t)client->method);
			k_mutex_unlock(&serial_http_lock);
			(void)linkr_debugger_http_body_accumulate(
				(uintptr_t)client, (uint16_t)client->method,
				SERIAL_HTTP_BODY_ROUTE, body_event, NULL, 0U, &body);
			return 0;
		}
		poisoned = poison_find((uintptr_t)client,
				       (uint16_t)client->method) != NULL;
		k_mutex_unlock(&serial_http_lock);
		if (poisoned) {
			/* The first assembly error already produced the response. Drain
			 * transaction callbacks without allowing FINAL to become a new
			 * request on the same client key.
			 */
			return 0;
		}
		body_result = linkr_debugger_http_body_accumulate(
			(uintptr_t)client, (uint16_t)client->method,
			SERIAL_HTTP_BODY_ROUTE, body_event,
			request != NULL ? request->data : NULL,
			request != NULL ? request->data_len : 0U, &body);
		if (body_result == LINKR_DEBUGGER_HTTP_BODY_WAITING ||
		    body_result == LINKR_DEBUGGER_HTTP_BODY_CLEARED) {
			return 0;
		}
		if (body_result == LINKR_DEBUGGER_HTTP_BODY_TOO_LARGE) {
			k_mutex_lock(&serial_http_lock, K_FOREVER);
			(void)poison_mark((uintptr_t)client,
					  (uint16_t)client->method);
			set_error(response, HTTP_413_PAYLOAD_TOO_LARGE,
				  "body_too_large",
				  "serial configuration body is too large");
			k_mutex_unlock(&serial_http_lock);
			return 0;
		}
		if (body_result != LINKR_DEBUGGER_HTTP_BODY_READY) {
			k_mutex_lock(&serial_http_lock, K_FOREVER);
			(void)poison_mark((uintptr_t)client,
					  (uint16_t)client->method);
			set_error(response, HTTP_400_BAD_REQUEST, "invalid_body",
				  "failed to assemble serial configuration body");
			k_mutex_unlock(&serial_http_lock);
			return 0;
		}
	} else if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	k_mutex_lock(&serial_http_lock, K_FOREVER);
	if (route == LINKR_DEBUGGER_SERIAL_HTTP_ROUTE_STATUS) {
		if (client->method != HTTP_GET) {
			set_error(response, HTTP_405_METHOD_NOT_ALLOWED,
				  "method_not_allowed", "method not allowed");
		} else if (encode_status(&buffer) < 0) {
			set_error(response, HTTP_500_INTERNAL_SERVER_ERROR,
				  "response_too_large",
				  "failed to encode serial status");
		} else {
			set_json_response(response, HTTP_200_OK, &buffer);
		}
		k_mutex_unlock(&serial_http_lock);
		return 0;
	}

	if (client->method != HTTP_PUT) {
		set_error(response, HTTP_405_METHOD_NOT_ALLOWED,
			  "method_not_allowed", "method not allowed");
		k_mutex_unlock(&serial_http_lock);
		return 0;
	}
	if (decode_config(body.data, body.len, &decoded, &config) < 0) {
		linkr_debugger_http_body_clear((uintptr_t)client,
					 HTTP_PUT, SERIAL_HTTP_BODY_ROUTE);
		(void)poison_mark((uintptr_t)client, (uint16_t)client->method);
		set_error(response, HTTP_400_BAD_REQUEST, "invalid_config",
			  "expected baud, data_bits, parity, stop_bits, "
			  "flow_control=none, and expected_generation");
		k_mutex_unlock(&serial_http_lock);
		return 0;
	}
	linkr_debugger_http_body_clear((uintptr_t)client, HTTP_PUT,
				 SERIAL_HTTP_BODY_ROUTE);
	ret = linkr_debugger_serial_status_snapshot(&serial);
	if (ret < 0) {
		set_error(response, HTTP_503_SERVICE_UNAVAILABLE, "unavailable",
			  "J4 serial is unavailable");
		k_mutex_unlock(&serial_http_lock);
		return 0;
	}
	if (decoded.expected_generation != serial.generation) {
		set_error(response, HTTP_409_CONFLICT, "generation_conflict",
			  "serial generation changed; refresh status and retry");
		k_mutex_unlock(&serial_http_lock);
		return 0;
	}
	ret = linkr_debugger_serial_config_cas(
		&config, decoded.expected_generation, &request_id);
	if (ret == -EBUSY || ret == -EAGAIN) {
		set_error(response, HTTP_409_CONFLICT, "configuration_busy",
			  "another serial transition is in progress");
	} else if (ret == -EINVAL) {
		set_error(response, HTTP_400_BAD_REQUEST, "invalid_config",
			  "unsupported serial line coding");
	} else if (ret == -ENODEV) {
		set_error(response, HTTP_503_SERVICE_UNAVAILABLE, "unavailable",
			  "J4 serial is unavailable");
	} else if (ret < 0) {
		set_error(response, HTTP_500_INTERNAL_SERVER_ERROR,
			  "configuration_failed",
			  "failed to queue serial configuration");
	} else if (buffer_append(&buffer, "{\"schema\":") < 0 ||
		   buffer_json_string(&buffer, linkr_debugger_json_schema()) < 0 ||
		   buffer_append(
			   &buffer,
			   ",\"ok\":true,\"command\":\"serial\","
			   "\"port\":\"j4\",\"action\":\"configure\","
			   "\"accepted\":true,\"request_id\":%llu}\n",
			   (unsigned long long)request_id) < 0) {
		set_error(response, HTTP_500_INTERNAL_SERVER_ERROR,
			  "response_too_large",
			  "failed to encode serial configuration response");
	} else {
		set_json_response(response, HTTP_202_ACCEPTED, &buffer);
	}
	k_mutex_unlock(&serial_http_lock);
	return 0;
}
