/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "debugboard_http.h"

#include "debugboard_control.h"
#include "debugboard_monitoring.h"
#include "debugboard_model.h"
#include "debugboard_ws.h"

#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/parser.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/status.h>
#include <zephyr/sys/printk.h>

#define DEBUGBOARD_HTTP_PORT 8080U
#define DEBUGBOARD_HTTP_HOST "172.29.203.1"
#define DEBUGBOARD_HTTP_BODY_BUFSZ 160U
#define DEBUGBOARD_HTTP_JSON_BUFSZ 4096U
#define DEBUGBOARD_HTTP_GPIO_NAME_BUFSZ DEBUGBOARD_GPIO_NAME_BUFSZ
#define DEBUGBOARD_HTTP_GPIO_IDENT_BUFSZ 64U

struct debugboard_http_env {
	char *buf;
	size_t cap;
	size_t len;
	bool truncated;
};

struct debugboard_http_power_set_request {
	char state[8];
};

struct debugboard_http_sd_route_request {
	char route[16];
};

struct debugboard_http_gpio_write_request {
	char direction[8];
	int value;
	bool has_value;
};

static uint16_t debugboard_http_port = DEBUGBOARD_HTTP_PORT;
static struct k_mutex debugboard_http_lock;
static struct k_work_delayable debugboard_bootloader_work;

static int debugboard_http_route_request(struct http_client_ctx *client,
				 enum http_transaction_status status,
				 const struct http_request_ctx *request_ctx,
				 struct http_response_ctx *response_ctx,
				 void *user_data);

static bool debugboard_http_hex_digit(char ch, uint8_t *value)
{
	if (ch >= '0' && ch <= '9') {
		*value = (uint8_t)(ch - '0');
		return true;
	}
	if (ch >= 'a' && ch <= 'f') {
		*value = (uint8_t)(10 + ch - 'a');
		return true;
	}
	if (ch >= 'A' && ch <= 'F') {
		*value = (uint8_t)(10 + ch - 'A');
		return true;
	}
	return false;
}

static bool debugboard_http_percent_decode(const char *src, char *dst, size_t dst_len)
{
	size_t out = 0U;

	if (src == NULL || dst == NULL || dst_len == 0U) {
		return false;
	}

	while (*src != '\0') {
		char ch = *src++;
		if (ch == '%' && src[0] != '\0' && src[1] != '\0') {
			uint8_t hi;
			uint8_t lo;
			if (!debugboard_http_hex_digit(src[0], &hi) ||
			    !debugboard_http_hex_digit(src[1], &lo)) {
				return false;
			}
			ch = (char)((hi << 4) | lo);
			src += 2;
		}
		if (out + 1U >= dst_len) {
			return false;
		}
		dst[out++] = ch;
	}

	dst[out] = '\0';
	return true;
}

static const struct json_obj_descr power_set_request_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct debugboard_http_power_set_request, state, JSON_TOK_STRING_BUF),
};

static const struct json_obj_descr sd_route_request_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct debugboard_http_sd_route_request, route, JSON_TOK_STRING_BUF),
};

static const struct json_obj_descr gpio_write_request_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct debugboard_http_gpio_write_request, direction, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct debugboard_http_gpio_write_request, value, JSON_TOK_NUMBER),
};

static int debugboard_http_append(struct debugboard_http_env *env, const char *fmt, ...)
{
	va_list args;
	int written;

	if (env->len >= env->cap) {
		env->truncated = true;
		return -ENOMEM;
	}

	va_start(args, fmt);
	written = vsnprintk(env->buf + env->len, env->cap - env->len, fmt, args);
	va_end(args);

	if (written < 0) {
		return written;
	}

	if ((size_t)written >= env->cap - env->len) {
		env->len = env->cap - 1U;
		env->truncated = true;
		return -ENOMEM;
	}

	env->len += (size_t)written;
	return 0;
}

static int debugboard_http_json_string(struct debugboard_http_env *env, const char *value)
{
	int ret;

	ret = debugboard_http_append(env, "\"");
	if (ret < 0) {
		return ret;
	}

	for (const char *p = value; *p != '\0'; p++) {
		unsigned char ch = (unsigned char)*p;

		switch (ch) {
		case '"':
			ret = debugboard_http_append(env, "\\\"");
			break;
		case '\\':
			ret = debugboard_http_append(env, "\\\\");
			break;
		case '\b':
			ret = debugboard_http_append(env, "\\b");
			break;
		case '\f':
			ret = debugboard_http_append(env, "\\f");
			break;
		case '\n':
			ret = debugboard_http_append(env, "\\n");
			break;
		case '\r':
			ret = debugboard_http_append(env, "\\r");
			break;
		case '\t':
			ret = debugboard_http_append(env, "\\t");
			break;
		default:
			if (ch < 0x20U) {
				ret = debugboard_http_append(env, "\\u%04x", ch);
			} else {
				ret = debugboard_http_append(env, "%c", ch);
			}
			break;
		}

		if (ret < 0) {
			return ret;
		}
	}

	return debugboard_http_append(env, "\"");
}

static int debugboard_http_json_begin(struct debugboard_http_env *env, const char *command,
					 bool ok)
{
	int ret;

	ret = debugboard_http_append(env, "{\"schema\":");
	if (ret < 0) {
		return ret;
	}

	ret = debugboard_http_json_string(env, debugboard_json_schema());
	if (ret < 0) {
		return ret;
	}

	ret = debugboard_http_append(env, ",\"ok\":%s,\"command\":", ok ? "true" : "false");
	if (ret < 0) {
		return ret;
	}

	return debugboard_http_json_string(env, command);
}

static int debugboard_http_json_error_payload(char *buf, size_t len, const char *command,
					      const char *code, const char *message)
{
	struct debugboard_http_env env = {
		.buf = buf,
		.cap = len,
		.len = 0U,
	};

	if (debugboard_http_json_begin(&env, command, false) < 0) {
		goto done;
	}

	if (debugboard_http_append(&env, ",\"error\":{\"code\":") < 0) {
		goto done;
	}

	if (debugboard_http_json_string(&env, code) < 0) {
		goto done;
	}

	if (debugboard_http_append(&env, ",\"message\":") < 0) {
		goto done;
	}

	if (debugboard_http_json_string(&env, message) < 0) {
		goto done;
	}

	(void)debugboard_http_append(&env, "}}\n");

done:
	buf[len - 1U] = '\0';
	return 0;
}

static void debugboard_http_set_json_response(struct http_response_ctx *response_ctx,
					      uint8_t *buf, size_t len,
					      enum http_status status)
{
	static const struct http_header headers[] = {
		{ .name = "Cache-Control", .value = "no-store" },
	};

	response_ctx->status = status;
	response_ctx->headers = headers;
	response_ctx->header_count = ARRAY_SIZE(headers);
	response_ctx->body = buf;
	response_ctx->body_len = strnlen((char *)buf, len);
	response_ctx->final_chunk = true;
}

static void debugboard_http_error(struct http_response_ctx *response_ctx, uint8_t *buf, size_t len,
				     enum http_status status, const char *command,
				     const char *code, const char *message)
{
	(void)debugboard_http_json_error_payload((char *)buf, len, command, code, message);
	debugboard_http_set_json_response(response_ctx, buf, len, status);
}

static int debugboard_http_json_power_output(struct debugboard_http_env *env,
					 const struct debugboard_rail_desc *rail)
{
	bool enabled = debugboard_power_output_enabled(rail);

	if (debugboard_http_append(env, "{\"name\":") < 0) {
		return -ENOMEM;
	}

	if (debugboard_http_json_string(env, rail->name) < 0) {
		return -ENOMEM;
	}

	if (debugboard_http_append(env, ",\"signal\":") < 0) {
		return -ENOMEM;
	}

	if (debugboard_http_json_string(env, rail->signal) < 0) {
		return -ENOMEM;
	}

	if (debugboard_http_append(env,
				 ",\"gp\":%u,\"controllable\":%s,\"state\":",
				 (unsigned int)rail->pin,
				 rail->controllable ? "true" : "false") < 0) {
		return -ENOMEM;
	}

	if (!rail->controllable) {
		if (debugboard_http_json_string(env, "locked") < 0) {
			return -ENOMEM;
		}

		return debugboard_http_append(env, ",\"value\":null}");
	}

	if (debugboard_http_json_string(env, enabled ? "on" : "off") < 0) {
		return -ENOMEM;
	}

	return debugboard_http_append(env, ",\"value\":%d}", enabled ? 1 : 0);
}

static int debugboard_http_json_power_outputs(struct debugboard_http_env *env)
{
	if (debugboard_http_append(env, "[") < 0) {
		return -ENOMEM;
	}

	for (size_t i = 0; i < debugboard_rail_count; i++) {
		if (i > 0U && debugboard_http_append(env, ",") < 0) {
			return -ENOMEM;
		}

		if (debugboard_http_json_power_output(env, &debugboard_rails[i]) < 0) {
			return -ENOMEM;
		}
	}

	return debugboard_http_append(env, "]");
}

static int debugboard_http_json_adc_channels(struct debugboard_http_env *env)
{
	if (debugboard_http_append(env, "[") < 0) {
		return -ENOMEM;
	}

	for (size_t i = 0; i < debugboard_current_count; i++) {
		const struct debugboard_current_desc *current = &debugboard_currents[i];

		if (i > 0U && debugboard_http_append(env, ",") < 0) {
			return -ENOMEM;
		}

		if (debugboard_http_append(env,
					 "{\"name\":") < 0 ||
		    debugboard_http_json_string(env, current->name) < 0 ||
		    debugboard_http_append(env, ",\"signal\":") < 0 ||
		    debugboard_http_json_string(env, current->signal) < 0 ||
		    debugboard_http_append(env,
					 ",\"adc_index\":%u,\"sensor\":",
					 (unsigned int)current->adc_index) < 0 ||
		    debugboard_http_json_string(env, current->sensor) < 0 ||
		    debugboard_http_append(env,
					 ",\"sensor_channel\":\"current\",\"unit\":\"A\"}") < 0) {
			return -ENOMEM;
		}
	}

	return debugboard_http_append(env, "]");
}

static int debugboard_http_json_safe_gpios(struct debugboard_http_env *env)
{
	char name[DEBUGBOARD_HTTP_GPIO_NAME_BUFSZ];
	int value;
	int ret;

	if (debugboard_http_append(env, "[") < 0) {
		return -ENOMEM;
	}

	for (size_t i = 0; i < debugboard_safe_gpio_count; i++) {
		const struct debugboard_safe_gpio_desc *desc = &debugboard_safe_gpios[i];

		if (i > 0U && debugboard_http_append(env, ",") < 0) {
			return -ENOMEM;
		}

		if (!debugboard_format_gpio_name(desc->pin, name, sizeof(name))) {
			strcpy(name, "GP?");
		}

		ret = debugboard_gpio_get(desc, &value);
		if (ret < 0) {
			value = 0;
		}

		if (debugboard_http_append(env, "{\"name\":") < 0 ||
		    debugboard_http_json_string(env, name) < 0 ||
		    debugboard_http_append(env, ",\"pin\":%u,\"note\":",
					     (unsigned int)desc->pin) < 0 ||
		    debugboard_http_json_string(env, desc->note) < 0 ||
		    debugboard_http_append(env, ",\"value\":%d,\"direction\":",
					     value > 0 ? 1 : 0) < 0 ||
		    debugboard_http_json_string(env, debugboard_safe_gpio_direction_name(i)) < 0 ||
		    debugboard_http_append(env, "}") < 0) {
			return -ENOMEM;
		}
	}

	return debugboard_http_append(env, "]");
}

static int debugboard_http_json_current_reading(struct debugboard_http_env *env,
					       const struct debugboard_current_desc *current,
					       const struct debugboard_current_sample *sample)
{
	if (debugboard_http_append(env, "{\"name\":") < 0 ||
	    debugboard_http_json_string(env, current->name) < 0 ||
	    debugboard_http_append(env, ",\"signal\":") < 0 ||
	    debugboard_http_json_string(env, current->signal) < 0 ||
	    debugboard_http_append(env, ",\"power_enabled\":%s,",
				     sample->rail_enabled ? "true" : "false") < 0) {
		return -ENOMEM;
	}

	if (sample->raw_available) {
		if (debugboard_http_append(env, "\"raw\":%d,", sample->raw) < 0) {
			return -ENOMEM;
		}
	} else if (debugboard_http_append(env, "\"raw\":null,") < 0) {
		return -ENOMEM;
	}

	if (debugboard_http_append(env,
				 "\"mv\":%d,\"sensor_channel\":\"current\",\"unit\":\"A\""
				 ",\"sensor_value\":{\"val1\":%d,\"val2\":%d},\"current_ua\":%d}",
				 sample->mv,
				 sample->value.val1,
				 sample->value.val2,
				 sample->current_ua) < 0) {
		return -ENOMEM;
	}

	return 0;
}

static int debugboard_http_json_watchdog_status(struct debugboard_http_env *env)
{
	struct debugboard_watchdog_status status;

	debugboard_watchdog_status_get(&status);

	return debugboard_http_append(env,
				     "{\"supported\":%s,\"automatic\":%s,\"healthy\":%s,"
				     "\"armed\":%s,\"timeout_ms\":%u,"
				     "\"bootloader_on_timeout\":%s,\"failing_service\":",
				     status.supported ? "true" : "false",
				     status.automatic ? "true" : "false",
				     status.healthy ? "true" : "false",
				     status.armed ? "true" : "false",
				     (unsigned int)status.timeout_ms,
				     status.bootloader_on_timeout ? "true" : "false") < 0 ||
	       debugboard_http_json_string(env,
				       status.failing_service != NULL ? status.failing_service : "") < 0 ||
	       debugboard_http_append(env, "}") < 0 ? -ENOMEM : 0;
}

static int debugboard_http_json_availability(struct debugboard_http_env *env,
						    bool available, const char *reason)
{
	if (debugboard_http_append(env, "{\"available\":%s,\"reason\":",
				       available ? "true" : "false") < 0 ||
	    debugboard_http_json_string(env, reason != NULL ? reason : "") < 0) {
		return -ENOMEM;
	}

	return 0;
}

static int debugboard_http_json_board_monitoring(struct debugboard_http_env *env)
{
	struct debugboard_monitoring_snapshot snapshot;

	debugboard_monitoring_snapshot_get(&snapshot);

	if (debugboard_http_json_availability(env, snapshot.temperature.available,
					      snapshot.temperature.reason) < 0) {
		return -ENOMEM;
	}
	if (snapshot.temperature.available) {
		if (debugboard_http_append(env, ",\"source\":") < 0 ||
		    debugboard_http_json_string(env, snapshot.temperature.source) < 0 ||
		    debugboard_http_append(env,
					     ",\"celsius\":{\"val1\":%d,\"val2\":%d}}",
					     snapshot.temperature.celsius_val1,
					     snapshot.temperature.celsius_val2) < 0) {
			return -ENOMEM;
		}
	} else if (snapshot.temperature.error != 0) {
		if (debugboard_http_append(env, ",\"error\":%d}", snapshot.temperature.error) < 0) {
			return -ENOMEM;
		}
	} else if (debugboard_http_append(env, "}") < 0) {
		return -ENOMEM;
	}

	if (debugboard_http_append(env, ",\"heap\":") < 0 ||
	    debugboard_http_json_availability(env, snapshot.heap.available,
					      snapshot.heap.reason) < 0) {
		return -ENOMEM;
	}
	if (snapshot.heap.available) {
		if (debugboard_http_append(env,
					     ",\"source\":") < 0 ||
		    debugboard_http_json_string(env, snapshot.heap.source) < 0 ||
		    debugboard_http_append(env,
					     ",\"free_bytes\":%u,\"allocated_bytes\":%u,"
					     "\"max_allocated_bytes\":%u,\"total_bytes\":%u}",
					     (unsigned int)snapshot.heap.free_bytes,
					     (unsigned int)snapshot.heap.allocated_bytes,
					     (unsigned int)snapshot.heap.max_allocated_bytes,
					     (unsigned int)snapshot.heap.total_bytes) < 0) {
			return -ENOMEM;
		}
	} else if (snapshot.heap.error != 0) {
		if (debugboard_http_append(env, ",\"error\":%d}", snapshot.heap.error) < 0) {
			return -ENOMEM;
		}
	} else if (debugboard_http_append(env, "}") < 0) {
		return -ENOMEM;
	}

	if (debugboard_http_append(env, ",\"runtime\":") < 0 ||
	    debugboard_http_json_availability(env, snapshot.runtime.available,
					      snapshot.runtime.reason) < 0) {
		return -ENOMEM;
	}
	if (snapshot.runtime.available) {
		if (debugboard_http_append(env,
					     ",\"uptime_ms\":%lld,\"uptime_seconds\":%llu}",
					     (long long)snapshot.runtime.uptime_ms,
					     (unsigned long long)snapshot.runtime.uptime_seconds) < 0) {
			return -ENOMEM;
		}
	} else if (snapshot.runtime.error != 0) {
		if (debugboard_http_append(env, ",\"error\":%d}", snapshot.runtime.error) < 0) {
			return -ENOMEM;
		}
	} else if (debugboard_http_append(env, "}") < 0) {
		return -ENOMEM;
	}

	if (debugboard_http_append(env, ",\"cpu\":") < 0 ||
	    debugboard_http_json_availability(env, snapshot.cpu.available,
					      snapshot.cpu.reason) < 0) {
		return -ENOMEM;
	}
	if (snapshot.cpu.available) {
		if (debugboard_http_append(env,
					     ",\"active_pct_x100\":%u,\"window_ms\":%u,"
					     "\"busy_cycles_delta\":%llu,\"total_cycles_delta\":%llu}",
					     (unsigned int)snapshot.cpu.active_pct_x100,
					     (unsigned int)snapshot.cpu.window_ms,
					     snapshot.cpu.busy_cycles_delta,
					     snapshot.cpu.total_cycles_delta) < 0) {
			return -ENOMEM;
		}
	} else if (snapshot.cpu.error != 0) {
		if (debugboard_http_append(env, ",\"error\":%d}", snapshot.cpu.error) < 0) {
			return -ENOMEM;
		}
	} else if (debugboard_http_append(env, "}") < 0) {
		return -ENOMEM;
	}

	return debugboard_http_append(env, "}");
}

static int debugboard_http_handle_status(struct http_client_ctx *client,
					 enum http_transaction_status status,
					 const struct http_request_ctx *request_ctx,
					 struct http_response_ctx *response_ctx,
					 void *user_data)
{
	static uint8_t json_buf[DEBUGBOARD_HTTP_JSON_BUFSZ];
	struct debugboard_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};

	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	k_mutex_lock(&debugboard_http_lock, K_FOREVER);
	if (debugboard_http_json_begin(&env, "status", true) < 0 ||
	    debugboard_http_append(&env,
				 ",\"project\":\"agent-debugboard\",\"mcu\":\"rp2040\",\"usb\":") < 0 ||
	    debugboard_http_json_string(&env, debugboard_usb_mode()) < 0 ||
	    debugboard_http_append(&env,
				 ",\"power_inputs\":[{\"name\":\"5v_fin\",\"controllable\":false,\"measured\":false}]"
				 ",\"power_outputs\":") < 0 ||
	    debugboard_http_json_power_outputs(&env) < 0 ||
	    debugboard_http_append(&env, ",\"sd\":{\"route\":") < 0 ||
	    debugboard_http_json_string(&env, debugboard_sd_route_name()) < 0 ||
	    debugboard_http_append(&env, "},\"adc_channels\":") < 0 ||
	    debugboard_http_json_adc_channels(&env) < 0 ||
	    debugboard_http_append(&env, ",\"watchdog\":") < 0 ||
	    debugboard_http_json_watchdog_status(&env) < 0 ||
	    debugboard_http_append(&env, ",\"board_monitoring\":{\"temperature\":") < 0 ||
	    debugboard_http_json_board_monitoring(&env) < 0 ||
	    debugboard_http_append(&env, ",\"gpios\":") < 0 ||
	    debugboard_http_json_safe_gpios(&env) < 0 ||
	    debugboard_http_append(&env, "}\n") < 0) {
		k_mutex_unlock(&debugboard_http_lock);
		debugboard_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_500_INTERNAL_SERVER_ERROR, "status",
				     "response_too_large", "failed to encode status response");
		return 0;
	}
	k_mutex_unlock(&debugboard_http_lock);

	debugboard_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
	return 0;
}

static int debugboard_http_handle_power(struct http_client_ctx *client,
					enum http_transaction_status status,
					const struct http_request_ctx *request_ctx,
					struct http_response_ctx *response_ctx,
					void *user_data)
{
	static uint8_t json_buf[DEBUGBOARD_HTTP_JSON_BUFSZ];
	char *path;
	const struct debugboard_rail_desc *rail = NULL;
	struct debugboard_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};

	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	path = strchr((char *)client->url_buffer + strlen("/api/v1/power"), '/');
	if (path != NULL) {
		rail = debugboard_find_rail(path + 1);
		if (rail == NULL) {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_404_NOT_FOUND,
					     "power", "unknown_power_output",
					     "unknown power output");
			return 0;
		}
	}

	k_mutex_lock(&debugboard_http_lock, K_FOREVER);
	switch (client->method) {
	case HTTP_GET:
		if (debugboard_http_json_begin(&env, "power", true) < 0 ||
		    debugboard_http_append(&env, ",\"action\":") < 0 ||
		    debugboard_http_json_string(&env, rail == NULL ? "list" : "get") < 0) {
			break;
		}

		if (rail == NULL) {
			if (debugboard_http_append(&env, ",\"power_outputs\":") < 0 ||
			    debugboard_http_json_power_outputs(&env) < 0 ||
			    debugboard_http_append(&env,
					     ",\"power_inputs\":[{\"name\":\"5v_fin\",\"controllable\":false,\"measured\":false}]}\n") < 0) {
				break;
			}
		} else if (debugboard_http_append(&env, ",\"power_output\":") < 0 ||
			   debugboard_http_json_power_output(&env, rail) < 0 ||
			   debugboard_http_append(&env, "}\n") < 0) {
			break;
		}

		k_mutex_unlock(&debugboard_http_lock);
		debugboard_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;

	case HTTP_PUT: {
		struct debugboard_http_power_set_request req = { 0 };
		bool enabled;
		int ret;

		if (rail == NULL) {
			k_mutex_unlock(&debugboard_http_lock);
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "power", "missing_power_output",
					     "missing power output in URL");
			return 0;
		}

		if (request_ctx->data == NULL || request_ctx->data_len == 0U) {
			k_mutex_unlock(&debugboard_http_lock);
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "power", "missing_body",
					     "missing JSON request body");
			return 0;
		}

		ret = json_obj_parse((char *)request_ctx->data, request_ctx->data_len,
				     power_set_request_descr,
				     ARRAY_SIZE(power_set_request_descr), &req);
		if (ret < 0 || !debugboard_parse_bool_arg(req.state, &enabled)) {
			k_mutex_unlock(&debugboard_http_lock);
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "power", "invalid_state",
					     "state must be on/off or 1/0");
			return 0;
		}

		ret = debugboard_power_output_set(rail, enabled);
		if (ret == -EPERM) {
			k_mutex_unlock(&debugboard_http_lock);
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_403_FORBIDDEN,
					     "power", "power_output_locked",
					     "power output is locked in this build");
			return 0;
		}
		if (ret < 0) {
			k_mutex_unlock(&debugboard_http_lock);
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf),
					     HTTP_500_INTERNAL_SERVER_ERROR,
					     "power", "set_failed",
					     "failed to set power output");
			return 0;
		}

		debugboard_ws_publish_state_change();

		if (debugboard_http_json_begin(&env, "power", true) < 0 ||
		    debugboard_http_append(&env, ",\"action\":\"set\",\"power_output\":") < 0 ||
		    debugboard_http_json_power_output(&env, rail) < 0 ||
		    debugboard_http_append(&env, "}\n") < 0) {
			break;
		}

		k_mutex_unlock(&debugboard_http_lock);
		debugboard_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;
	}

	default:
		k_mutex_unlock(&debugboard_http_lock);
		debugboard_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_405_METHOD_NOT_ALLOWED,
				     "power", "method_not_allowed", "method not allowed");
		return 0;
	}

	k_mutex_unlock(&debugboard_http_lock);
	debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
			     "power", "response_too_large", "failed to encode power response");
	return 0;
}

static int debugboard_http_handle_adc(struct http_client_ctx *client,
				      enum http_transaction_status status,
				      const struct http_request_ctx *request_ctx,
				      struct http_response_ctx *response_ctx,
				      void *user_data)
{
	static uint8_t json_buf[DEBUGBOARD_HTTP_JSON_BUFSZ];
	struct debugboard_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};
	const struct debugboard_current_desc *single = NULL;
	char *query;

	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	query = strstr((char *)client->url_buffer, "channel=");
	if (query != NULL) {
		single = debugboard_find_current(query + strlen("channel="));
		if (single == NULL) {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_404_NOT_FOUND,
					     "adc", "unknown_channel", "unknown adc channel");
			return 0;
		}
	}

	k_mutex_lock(&debugboard_http_lock, K_FOREVER);
	if (debugboard_http_json_begin(&env, "adc", true) < 0 ||
	    debugboard_http_append(&env, ",\"action\":\"read\",\"readings\":[") < 0) {
		goto too_large;
	}

	for (size_t i = 0; i < debugboard_current_count; i++) {
		struct debugboard_current_sample sample;
		const struct debugboard_current_desc *current = &debugboard_currents[i];
		int ret;

		if (single != NULL && current != single) {
			continue;
		}

		ret = debugboard_current_read(current, &sample);
		if (ret < 0) {
			k_mutex_unlock(&debugboard_http_lock);
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf),
					     HTTP_500_INTERNAL_SERVER_ERROR,
					     "adc", "read_failed", "failed to read adc channel");
			return 0;
		}

		if (env.len > 0U && env.buf[env.len - 1U] != '[' &&
		    debugboard_http_append(&env, ",") < 0) {
			goto too_large;
		}

		if (debugboard_http_json_current_reading(&env, current, &sample) < 0) {
			goto too_large;
		}
	}

	if (debugboard_http_append(&env, "]}\n") < 0) {
		goto too_large;
	}

	k_mutex_unlock(&debugboard_http_lock);
	debugboard_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
	return 0;

too_large:
	k_mutex_unlock(&debugboard_http_lock);
	debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
			     "adc", "response_too_large", "failed to encode adc response");
	return 0;
}

static int debugboard_http_handle_sd(struct http_client_ctx *client,
				     enum http_transaction_status status,
				     const struct http_request_ctx *request_ctx,
				     struct http_response_ctx *response_ctx,
				     void *user_data)
{
	static uint8_t json_buf[DEBUGBOARD_HTTP_JSON_BUFSZ];
	struct debugboard_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};

	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	k_mutex_lock(&debugboard_http_lock, K_FOREVER);
	switch (client->method) {
	case HTTP_GET:
		if (debugboard_http_json_begin(&env, "sd", true) < 0 ||
		    debugboard_http_append(&env, ",\"action\":\"get\",\"route\":") < 0 ||
		    debugboard_http_json_string(&env, debugboard_sd_route_name()) < 0 ||
		    debugboard_http_append(&env, "}\n") < 0) {
			break;
		}

		k_mutex_unlock(&debugboard_http_lock);
		debugboard_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;

	case HTTP_PUT: {
		struct debugboard_http_sd_route_request req = { 0 };
		enum debugboard_sd_route route;
		int ret;

		if (request_ctx->data == NULL || request_ctx->data_len == 0U) {
			k_mutex_unlock(&debugboard_http_lock);
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "sd", "missing_body", "missing JSON request body");
			return 0;
		}

		ret = json_obj_parse((char *)request_ctx->data, request_ctx->data_len,
				     sd_route_request_descr,
				     ARRAY_SIZE(sd_route_request_descr), &req);
		if (ret < 0) {
			k_mutex_unlock(&debugboard_http_lock);
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "sd", "invalid_route",
					     "route must be target or usb-reader");
			return 0;
		}

		if (strcmp(req.route, "target") == 0) {
			route = DEBUGBOARD_SD_ROUTE_TARGET;
		} else if (strcmp(req.route, "usb-reader") == 0 || strcmp(req.route, "reader") == 0) {
			route = DEBUGBOARD_SD_ROUTE_USB_READER;
		} else {
			k_mutex_unlock(&debugboard_http_lock);
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "sd", "invalid_route",
					     "route must be target or usb-reader");
			return 0;
		}

		ret = debugboard_sd_route_set(route);
		if (ret < 0) {
			k_mutex_unlock(&debugboard_http_lock);
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf),
					     HTTP_500_INTERNAL_SERVER_ERROR,
					     "sd", "set_failed", "failed to set SD route");
			return 0;
		}

		debugboard_ws_publish_state_change();

		if (debugboard_http_json_begin(&env, "sd", true) < 0 ||
		    debugboard_http_append(&env, ",\"action\":\"route\",\"route\":") < 0 ||
		    debugboard_http_json_string(&env, debugboard_sd_route_name()) < 0 ||
		    debugboard_http_append(&env, "}\n") < 0) {
			break;
		}

		k_mutex_unlock(&debugboard_http_lock);
		debugboard_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;
	}

	default:
		k_mutex_unlock(&debugboard_http_lock);
		debugboard_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_405_METHOD_NOT_ALLOWED,
				     "sd", "method_not_allowed", "method not allowed");
		return 0;
	}

	k_mutex_unlock(&debugboard_http_lock);
	debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
			     "sd", "response_too_large", "failed to encode sd response");
	return 0;
}

static int debugboard_http_handle_gpio(struct http_client_ctx *client,
				       enum http_transaction_status status,
				       const struct http_request_ctx *request_ctx,
				       struct http_response_ctx *response_ctx,
				       void *user_data)
{
	static uint8_t json_buf[DEBUGBOARD_HTTP_JSON_BUFSZ];
	char *path;
	char name[DEBUGBOARD_HTTP_GPIO_NAME_BUFSZ];
	char identifier[DEBUGBOARD_HTTP_GPIO_IDENT_BUFSZ];
	const struct debugboard_safe_gpio_desc *desc = NULL;
	struct debugboard_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};

	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	path = strchr((char *)client->url_buffer + strlen("/api/v1/gpio"), '/');
	if (path != NULL) {
		if (!debugboard_http_percent_decode(path + 1, identifier, sizeof(identifier))) {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "gpio", "invalid_gpio",
					     "GPIO target must be GP13, 13, or an allowlist note such as CON_MAS");
			return 0;
		}

		desc = debugboard_find_safe_gpio_by_identifier(identifier);
		if (desc == NULL) {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_403_FORBIDDEN,
					     "gpio", "not_allowed", "GPIO is not in the allowlist");
			return 0;
		}
	}

	k_mutex_lock(&debugboard_http_lock, K_FOREVER);
	switch (client->method) {
	case HTTP_GET:
		if (desc == NULL) {
			if (debugboard_http_json_begin(&env, "gpio", true) < 0 ||
			    debugboard_http_append(&env, ",\"action\":\"list\",\"gpios\":") < 0 ||
			    debugboard_http_json_safe_gpios(&env) < 0 ||
			    debugboard_http_append(&env, ",\"reserved\":") < 0 ||
			    debugboard_http_json_string(&env, debugboard_reserved_gpios()) < 0 ||
			    debugboard_http_append(&env, "}\n") < 0) {
				break;
			}
		} else {
			int value;
			int ret = debugboard_gpio_get(desc, &value);
			if (ret < 0) {
				k_mutex_unlock(&debugboard_http_lock);
				debugboard_http_error(response_ctx, json_buf, sizeof(json_buf),
						     HTTP_500_INTERNAL_SERVER_ERROR,
						     "gpio", "read_failed", "failed to read GPIO");
				return 0;
			}

			if (!debugboard_format_gpio_name(desc->pin, name, sizeof(name))) {
				strcpy(name, "GP?");
			}

			if (debugboard_http_json_begin(&env, "gpio", true) < 0 ||
			    debugboard_http_append(&env, ",\"action\":\"get\",\"gpio\":{\"name\":") < 0 ||
			    debugboard_http_json_string(&env, name) < 0 ||
			    debugboard_http_append(&env,
					     ",\"pin\":%u,\"note\":", (unsigned int)desc->pin) < 0 ||
			    debugboard_http_json_string(&env, desc->note) < 0 ||
			    debugboard_http_append(&env, ",\"direction\":") < 0 ||
			    debugboard_http_json_string(&env, debugboard_safe_gpio_direction_name(
				    (size_t)(desc - debugboard_safe_gpios))) < 0 ||
			    debugboard_http_append(&env, ",\"value\":%d}}\n",
					     value > 0 ? 1 : 0) < 0) {
				break;
			}
		}

		k_mutex_unlock(&debugboard_http_lock);
		debugboard_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;

	case HTTP_PUT: {
		struct debugboard_http_gpio_write_request req = { 0 };
		int ret;

		if (desc == NULL) {
			k_mutex_unlock(&debugboard_http_lock);
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "gpio", "missing_gpio", "missing GPIO in URL");
			return 0;
		}

		if (request_ctx->data == NULL || request_ctx->data_len == 0U) {
			k_mutex_unlock(&debugboard_http_lock);
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "gpio", "missing_body", "missing JSON request body");
			return 0;
		}

		ret = json_obj_parse((char *)request_ctx->data, request_ctx->data_len,
				     gpio_write_request_descr,
				     ARRAY_SIZE(gpio_write_request_descr), &req);
		if (ret < 0) {
			k_mutex_unlock(&debugboard_http_lock);
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "gpio", "invalid_request",
					     "request must contain direction and optional value");
			return 0;
		}

		if (!debugboard_format_gpio_name(desc->pin, name, sizeof(name))) {
			strcpy(name, "GP?");
		}

		if (strcmp(req.direction, "input") == 0) {
			ret = debugboard_gpio_set_input(desc);
			if (ret < 0) {
				k_mutex_unlock(&debugboard_http_lock);
				debugboard_http_error(response_ctx, json_buf, sizeof(json_buf),
						     HTTP_500_INTERNAL_SERVER_ERROR,
						     "gpio", "configure_failed",
						     "failed to configure GPIO input");
				return 0;
			}

			debugboard_ws_publish_state_change();

			if (debugboard_http_json_begin(&env, "gpio", true) < 0 ||
			    debugboard_http_append(&env,
					     ",\"action\":\"input\",\"gpio\":{\"name\":") < 0 ||
			    debugboard_http_json_string(&env, name) < 0 ||
			    debugboard_http_append(&env,
					     ",\"pin\":%u,\"note\":", (unsigned int)desc->pin) < 0 ||
			    debugboard_http_json_string(&env, desc->note) < 0 ||
			    debugboard_http_append(&env,
					     ",\"direction\":\"input\",\"value\":null}}\n") < 0) {
				break;
			}
		} else if (strcmp(req.direction, "output") == 0) {
			bool value = req.value != 0;

			ret = debugboard_gpio_set_output(desc, value);
			if (ret < 0) {
				k_mutex_unlock(&debugboard_http_lock);
				debugboard_http_error(response_ctx, json_buf, sizeof(json_buf),
						     HTTP_500_INTERNAL_SERVER_ERROR,
						     "gpio", "configure_failed",
						     "failed to configure GPIO output");
				return 0;
			}

			debugboard_ws_publish_state_change();

			if (debugboard_http_json_begin(&env, "gpio", true) < 0 ||
			    debugboard_http_append(&env,
					     ",\"action\":\"set\",\"gpio\":{\"name\":") < 0 ||
			    debugboard_http_json_string(&env, name) < 0 ||
			    debugboard_http_append(&env,
					     ",\"pin\":%u,\"note\":", (unsigned int)desc->pin) < 0 ||
			    debugboard_http_json_string(&env, desc->note) < 0 ||
			    debugboard_http_append(&env,
					     ",\"direction\":\"output\",\"value\":%d}}\n",
					     value ? 1 : 0) < 0) {
				break;
			}
		} else {
			k_mutex_unlock(&debugboard_http_lock);
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "gpio", "invalid_request",
					     "direction must be input or output");
			return 0;
		}

		k_mutex_unlock(&debugboard_http_lock);
		debugboard_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;
	}

	default:
		k_mutex_unlock(&debugboard_http_lock);
		debugboard_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_405_METHOD_NOT_ALLOWED,
				     "gpio", "method_not_allowed", "method not allowed");
		return 0;
	}

	k_mutex_unlock(&debugboard_http_lock);
	debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
			     "gpio", "response_too_large", "failed to encode gpio response");
	return 0;
}

static int debugboard_http_handle_bootloader(struct http_client_ctx *client,
				     enum http_transaction_status status,
				     const struct http_request_ctx *request_ctx,
				     struct http_response_ctx *response_ctx,
				     void *user_data)
{
	static uint8_t json_buf[DEBUGBOARD_HTTP_JSON_BUFSZ];
	struct debugboard_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};

	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	if (debugboard_http_json_begin(&env, "bootloader", true) < 0 ||
	    debugboard_http_append(&env, ",\"message\":\"entering RP2040 BOOTSEL in 250 ms\"}\n") < 0) {
		debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
				     "bootloader", "response_too_large",
				     "failed to encode bootloader response");
		return 0;
	}

	debugboard_ws_publish_state_change();
	(void)k_work_reschedule(&debugboard_bootloader_work, K_MSEC(250));
	debugboard_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
	return 0;
}

static int debugboard_http_handle_watchdog(struct http_client_ctx *client,
				     enum http_transaction_status status,
				     const struct http_request_ctx *request_ctx,
				     struct http_response_ctx *response_ctx,
				     void *user_data)
{
	static uint8_t json_buf[DEBUGBOARD_HTTP_JSON_BUFSZ];
	struct debugboard_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};

	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	k_mutex_lock(&debugboard_http_lock, K_FOREVER);
	switch (client->method) {
	case HTTP_GET:
		if (debugboard_http_json_begin(&env, "watchdog", true) < 0 ||
		    debugboard_http_append(&env, ",\"action\":\"status\",\"watchdog\":") < 0 ||
		    debugboard_http_json_watchdog_status(&env) < 0 ||
		    debugboard_http_append(&env, "}\n") < 0) {
			break;
		}

		k_mutex_unlock(&debugboard_http_lock);
		debugboard_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;

	case HTTP_POST:
		k_mutex_unlock(&debugboard_http_lock);
		debugboard_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_405_METHOD_NOT_ALLOWED,
				     "watchdog", "manual_feed_removed",
				     "watchdog is supervised by firmware and cannot be fed manually");
		return 0;

	default:
		k_mutex_unlock(&debugboard_http_lock);
		debugboard_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_405_METHOD_NOT_ALLOWED,
				     "watchdog", "method_not_allowed", "method not allowed");
		return 0;
	}

	k_mutex_unlock(&debugboard_http_lock);
	debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
			     "watchdog", "response_too_large", "failed to encode watchdog response");
	return 0;
}

static void debugboard_bootloader_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)debugboard_bootloader_now();
}

static struct http_resource_detail_dynamic fallback_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_PUT) | BIT(HTTP_POST) |
			BIT(HTTP_DELETE),
		.content_type = "application/json",
	},
	.cb = debugboard_http_route_request,
};

static uint8_t debugboard_ws_resource_buffers[DEBUGBOARD_WS_MAX_CLIENTS][DEBUGBOARD_WS_RECV_BUFFER_SIZE];
static uint8_t debugboard_ws_resource_slots[DEBUGBOARD_WS_MAX_CLIENTS] = { 0, 1, 2, 3 };

#define DEBUGBOARD_WS_RESOURCE_DETAIL(slot_)                                                     \
	static struct http_resource_detail_websocket debugboard_ws_resource_detail_##slot_ = {    \
		.common = {                                                                        \
			.type = HTTP_RESOURCE_TYPE_WEBSOCKET,                                        \
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),                          \
		},                                                                                \
		.cb = debugboard_ws_setup,                                                       \
		.data_buffer = debugboard_ws_resource_buffers[slot_],                            \
		.data_buffer_len = sizeof(debugboard_ws_resource_buffers[slot_]),                \
		.user_data = &debugboard_ws_resource_slots[slot_],                               \
	};                                                                                   \
	HTTP_RESOURCE_DEFINE(debugboard_ws_resource_##slot_, debugboard_http_service,       \
			     "/api/v1/ws/" #slot_, &debugboard_ws_resource_detail_##slot_)

DEBUGBOARD_WS_RESOURCE_DETAIL(0);
DEBUGBOARD_WS_RESOURCE_DETAIL(1);
DEBUGBOARD_WS_RESOURCE_DETAIL(2);
DEBUGBOARD_WS_RESOURCE_DETAIL(3);

static int debugboard_http_handle_live_sessions(struct http_client_ctx *client,
					 enum http_transaction_status status,
					 const struct http_request_ctx *request_ctx,
					 struct http_response_ctx *response_ctx,
					 void *user_data)
{
	static uint8_t json_buf[DEBUGBOARD_HTTP_JSON_BUFSZ];
	struct debugboard_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};
	struct debugboard_ws_session_info info = { 0 };
	char ws_url[96];
	char *path;
	uint32_t session_id;
	int ret;

	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	path = strchr((char *)client->url_buffer + strlen("/api/v1/live-sessions"), '/');

	switch (client->method) {
	case HTTP_POST:
		if (path != NULL) {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_404_NOT_FOUND,
					     "live-sessions", "not_found", "unknown live session path");
			return 0;
		}
		ret = debugboard_ws_session_create(&info);
		if (ret == -EBUSY) {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_503_SERVICE_UNAVAILABLE,
					     "live-sessions", "no_slots_available",
					     "no websocket session slots available");
			return 0;
		}
		if (ret < 0) {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
					     "live-sessions", "create_failed",
					     "failed to create live websocket session");
			return 0;
		}
		if (debugboard_http_json_begin(&env, "live-sessions", true) < 0 ||
		    debugboard_http_append(&env, ",\"action\":\"create\",\"session_id\":%u,\"ws_url\":",
				     (unsigned int)info.session_id) < 0 ||
		    snprintk(ws_url, sizeof(ws_url), "ws://%s:%u%s",
			     debugboard_http_listener_host(), (unsigned int)debugboard_http_listener_port(),
			     info.ws_path) < 0 ||
		    debugboard_http_json_string(&env, ws_url) < 0 ||
		    debugboard_http_append(&env, ",\"connected\":false}\n") < 0) {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
					     "live-sessions", "response_too_large",
					     "failed to encode live session response");
			return 0;
		}
		debugboard_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_201_CREATED);
		return 0;

	case HTTP_GET:
		if (path == NULL || *(path + 1) == '\0') {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "live-sessions", "missing_session_id",
					     "missing session id in URL");
			return 0;
		}
		session_id = (uint32_t)strtoul(path + 1, NULL, 10);
		ret = debugboard_ws_session_lookup(session_id, &info);
		if (ret < 0) {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_404_NOT_FOUND,
					     "live-sessions", "unknown_session_id",
					     "unknown live session id");
			return 0;
		}
		if (debugboard_http_json_begin(&env, "live-sessions", true) < 0 ||
		    debugboard_http_append(&env, ",\"action\":\"get\",\"session_id\":%u,\"slot\":%u,\"connected\":%s,\"ws_url\":",
				     (unsigned int)info.session_id, (unsigned int)info.slot,
				     info.connected ? "true" : "false") < 0 ||
		    snprintk(ws_url, sizeof(ws_url), "ws://%s:%u%s",
			     debugboard_http_listener_host(), (unsigned int)debugboard_http_listener_port(),
			     info.ws_path) < 0 ||
		    debugboard_http_json_string(&env, ws_url) < 0 ||
		    debugboard_http_append(&env, "}\n") < 0) {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
					     "live-sessions", "response_too_large",
					     "failed to encode live session response");
			return 0;
		}
		debugboard_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;

	case HTTP_DELETE:
		if (path == NULL || *(path + 1) == '\0') {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "live-sessions", "missing_session_id",
					     "missing session id in URL");
			return 0;
		}
		session_id = (uint32_t)strtoul(path + 1, NULL, 10);
		ret = debugboard_ws_session_delete(session_id);
		if (ret == -ENOENT) {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_404_NOT_FOUND,
					     "live-sessions", "unknown_session_id",
					     "unknown live session id");
			return 0;
		}
		if (ret == -EBUSY) {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_409_CONFLICT,
					     "live-sessions", "session_connected",
					     "session is already connected");
			return 0;
		}
		if (ret < 0) {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
					     "live-sessions", "delete_failed",
					     "failed to delete live session");
			return 0;
		}
		if (debugboard_http_json_begin(&env, "live-sessions", true) < 0 ||
		    debugboard_http_append(&env, ",\"action\":\"delete\",\"session_id\":%u}\n",
				     (unsigned int)session_id) < 0) {
			debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
					     "live-sessions", "response_too_large",
					     "failed to encode live session delete response");
			return 0;
		}
		debugboard_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;

	default:
		debugboard_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_405_METHOD_NOT_ALLOWED,
				     "live-sessions", "method_not_allowed", "method not allowed");
		return 0;
	}
}

static int debugboard_http_route_request(struct http_client_ctx *client,
					 enum http_transaction_status status,
					 const struct http_request_ctx *request_ctx,
					 struct http_response_ctx *response_ctx,
					 void *user_data)
{
	char *path = (char *)client->url_buffer;

	if (strncmp(path, "/api/v1/status", strlen("/api/v1/status")) == 0) {
		return debugboard_http_handle_status(client, status, request_ctx, response_ctx, user_data);
	}

	if (strncmp(path, "/api/v1/power", strlen("/api/v1/power")) == 0) {
		return debugboard_http_handle_power(client, status, request_ctx, response_ctx, user_data);
	}

	if (strncmp(path, "/api/v1/adc/read", strlen("/api/v1/adc/read")) == 0) {
		return debugboard_http_handle_adc(client, status, request_ctx, response_ctx, user_data);
	}

	if (strncmp(path, "/api/v1/sd", strlen("/api/v1/sd")) == 0) {
		return debugboard_http_handle_sd(client, status, request_ctx, response_ctx, user_data);
	}

	if (strncmp(path, "/api/v1/gpio", strlen("/api/v1/gpio")) == 0) {
		return debugboard_http_handle_gpio(client, status, request_ctx, response_ctx, user_data);
	}

	if (strncmp(path, "/api/v1/bootloader", strlen("/api/v1/bootloader")) == 0) {
		return debugboard_http_handle_bootloader(client, status, request_ctx, response_ctx, user_data);
	}

	if (strncmp(path, "/api/v1/watchdog", strlen("/api/v1/watchdog")) == 0) {
		return debugboard_http_handle_watchdog(client, status, request_ctx, response_ctx, user_data);
	}

	if (strncmp(path, "/api/v1/live-sessions", strlen("/api/v1/live-sessions")) == 0) {
		return debugboard_http_handle_live_sessions(client, status, request_ctx, response_ctx, user_data);
	}

	debugboard_http_error(response_ctx, (uint8_t *)client->buffer, sizeof(client->buffer),
			     HTTP_404_NOT_FOUND, "http", "not_found", "unknown API path");
	return 0;
}

HTTP_SERVICE_DEFINE(debugboard_http_service, DEBUGBOARD_HTTP_HOST, &debugboard_http_port,
		    CONFIG_HTTP_SERVER_MAX_CLIENTS, CONFIG_HTTP_SERVER_MAX_CLIENTS, NULL,
		    &fallback_resource_detail.common, NULL);

int debugboard_http_listener_fd(void)
{
	return debugboard_http_service.fd != NULL ? *debugboard_http_service.fd : -1;
}

uint16_t debugboard_http_listener_port(void)
{
	return debugboard_http_port;
}

const char *debugboard_http_listener_host(void)
{
	return DEBUGBOARD_HTTP_HOST;
}

void debugboard_http_init(void)
{
	k_mutex_init(&debugboard_http_lock);
	k_work_init_delayable(&debugboard_bootloader_work, debugboard_bootloader_work_handler);
}

void debugboard_http_publish_state_change(void)
{
	debugboard_ws_publish_state_change();
}
