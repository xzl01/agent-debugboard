/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_http.h"

#include "linkr_debugger_captive_portal.h"
#include "linkr_debugger_control.h"
#include "linkr_debugger_config_http.h"
#include "linkr_debugger_config_summary.h"
#include "linkr_debugger_http_body.h"
#include "linkr_debugger_monitoring.h"
#include "linkr_debugger_model.h"
#include "linkr_debugger_network.h"
#include "linkr_debugger_ota.h"
#include "linkr_debugger_shell.h"
#include "linkr_debugger_ws.h"

#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/parser.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/status.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(linkr_debugger_http, CONFIG_LINKR_DEBUGGER_LOG_LEVEL);

#define LINKR_DEBUGGER_HTTP_PORT 80U
#define LINKR_DEBUGGER_HTTP_HOST "0.0.0.0"
#define LINKR_DEBUGGER_HTTP_ADDR "172.29.203.1"
#define LINKR_DEBUGGER_HTTP_BODY_BUFSZ 160U
#define LINKR_DEBUGGER_HTTP_JSON_BUFSZ 4096U
#define LINKR_DEBUGGER_HTTP_STATUS_JSON_BUFSZ 6144U
#define LINKR_DEBUGGER_HTTP_GPIO_NAME_BUFSZ LINKR_DEBUGGER_GPIO_NAME_BUFSZ
#define LINKR_DEBUGGER_HTTP_GPIO_IDENT_BUFSZ 64U

enum linkr_debugger_http_route_id {
	LINKR_DEBUGGER_HTTP_ROUTE_LOGIC_ANALYZER = 1,
	LINKR_DEBUGGER_HTTP_ROUTE_LOGIC_ANALYZER_STREAM,
};

struct linkr_debugger_http_env {
	char *buf;
	size_t cap;
	size_t len;
	bool truncated;
};

struct linkr_debugger_http_power_set_request {
	char state[8];
};

struct linkr_debugger_http_switch_route_request {
	char route[16];
};

struct linkr_debugger_http_gpio_write_request {
	char direction[8];
	int value;
	bool has_value;
};

struct linkr_debugger_http_target_recovery_request {
	char mode[24];
	char rail[16];
};

static uint16_t linkr_debugger_http_port = LINKR_DEBUGGER_HTTP_PORT;
static struct k_mutex linkr_debugger_http_lock;
static struct k_work_delayable linkr_debugger_bootloader_work;

static int linkr_debugger_http_route_request(struct http_client_ctx *client,
				 enum http_transaction_status status,
				 const struct http_request_ctx *request_ctx,
				 struct http_response_ctx *response_ctx,
				 void *user_data);

static enum linkr_debugger_http_body_event linkr_debugger_http_body_event_from_status(
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

static bool linkr_debugger_http_hex_digit(char ch, uint8_t *value)
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

static bool linkr_debugger_http_percent_decode(const char *src, char *dst, size_t dst_len)
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
			if (!linkr_debugger_http_hex_digit(src[0], &hi) ||
			    !linkr_debugger_http_hex_digit(src[1], &lo)) {
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
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_http_power_set_request, state, JSON_TOK_STRING_BUF),
};

static const struct json_obj_descr switch_route_request_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_http_switch_route_request, route, JSON_TOK_STRING_BUF),
};

static const struct json_obj_descr gpio_write_request_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_http_gpio_write_request, direction, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_http_gpio_write_request, value, JSON_TOK_NUMBER),
};

static const struct json_obj_descr target_recovery_request_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_http_target_recovery_request, mode,
			    JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct linkr_debugger_http_target_recovery_request, rail,
			    JSON_TOK_STRING_BUF),
};

static int linkr_debugger_http_append(struct linkr_debugger_http_env *env, const char *fmt, ...)
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

static int linkr_debugger_http_json_string(struct linkr_debugger_http_env *env, const char *value)
{
	int ret;

	ret = linkr_debugger_http_append(env, "\"");
	if (ret < 0) {
		return ret;
	}

	for (const char *p = value; *p != '\0'; p++) {
		unsigned char ch = (unsigned char)*p;

		switch (ch) {
		case '"':
			ret = linkr_debugger_http_append(env, "\\\"");
			break;
		case '\\':
			ret = linkr_debugger_http_append(env, "\\\\");
			break;
		case '\b':
			ret = linkr_debugger_http_append(env, "\\b");
			break;
		case '\f':
			ret = linkr_debugger_http_append(env, "\\f");
			break;
		case '\n':
			ret = linkr_debugger_http_append(env, "\\n");
			break;
		case '\r':
			ret = linkr_debugger_http_append(env, "\\r");
			break;
		case '\t':
			ret = linkr_debugger_http_append(env, "\\t");
			break;
		default:
			if (ch < 0x20U) {
				ret = linkr_debugger_http_append(env, "\\u%04x", ch);
			} else {
				ret = linkr_debugger_http_append(env, "%c", ch);
			}
			break;
		}

		if (ret < 0) {
			return ret;
		}
	}

	return linkr_debugger_http_append(env, "\"");
}

static int linkr_debugger_http_json_begin(struct linkr_debugger_http_env *env, const char *command,
					 bool ok)
{
	int ret;

	ret = linkr_debugger_http_append(env, "{\"schema\":");
	if (ret < 0) {
		return ret;
	}

	ret = linkr_debugger_http_json_string(env, linkr_debugger_json_schema());
	if (ret < 0) {
		return ret;
	}

	ret = linkr_debugger_http_append(env, ",\"ok\":%s,\"command\":", ok ? "true" : "false");
	if (ret < 0) {
		return ret;
	}

	return linkr_debugger_http_json_string(env, command);
}

static int linkr_debugger_http_json_error_payload(char *buf, size_t len, const char *command,
					      const char *code, const char *message)
{
	struct linkr_debugger_http_env env = {
		.buf = buf,
		.cap = len,
		.len = 0U,
	};

	if (linkr_debugger_http_json_begin(&env, command, false) < 0) {
		goto done;
	}

	if (linkr_debugger_http_append(&env, ",\"error\":{\"code\":") < 0) {
		goto done;
	}

	if (linkr_debugger_http_json_string(&env, code) < 0) {
		goto done;
	}

	if (linkr_debugger_http_append(&env, ",\"message\":") < 0) {
		goto done;
	}

	if (linkr_debugger_http_json_string(&env, message) < 0) {
		goto done;
	}

	(void)linkr_debugger_http_append(&env, "}}\n");

done:
	buf[len - 1U] = '\0';
	return 0;
}

static void linkr_debugger_http_set_json_response(struct http_response_ctx *response_ctx,
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

static void linkr_debugger_http_error(struct http_response_ctx *response_ctx, uint8_t *buf, size_t len,
				     enum http_status status, const char *command,
				     const char *code, const char *message)
{
	(void)linkr_debugger_http_json_error_payload((char *)buf, len, command, code, message);
	linkr_debugger_http_set_json_response(response_ctx, buf, len, status);
}

static void linkr_debugger_http_set_capport_response(struct http_response_ctx *response_ctx,
						     enum linkr_debugger_captive_method method)
{
	static const struct http_header headers[] = {
		{ .name = "Content-Type", .value = "application/captive+json" },
		{ .name = "Cache-Control", .value = "no-store" },
	};
	const char *body = linkr_debugger_captive_capport_body();

	response_ctx->status = HTTP_200_OK;
	response_ctx->headers = headers;
	response_ctx->header_count = ARRAY_SIZE(headers);
	if (linkr_debugger_captive_method_has_body(method)) {
		response_ctx->body = (const uint8_t *)body;
		response_ctx->body_len = strlen(body);
	}
	response_ctx->final_chunk = true;
}

static void linkr_debugger_http_set_redirect_response(struct http_response_ctx *response_ctx)
{
	static const struct http_header headers[] = {
		{ .name = "Location", .value = LINKR_DEBUGGER_CAPTIVE_PORTAL_URL },
		{ .name = "Cache-Control", .value = "no-store" },
	};

	response_ctx->status = HTTP_302_FOUND;
	response_ctx->headers = headers;
	response_ctx->header_count = ARRAY_SIZE(headers);
	response_ctx->final_chunk = true;
}

static void linkr_debugger_http_set_method_not_allowed_response(
	struct http_response_ctx *response_ctx)
{
	static const struct http_header headers[] = {
		{ .name = "Cache-Control", .value = "no-store" },
	};

	response_ctx->status = HTTP_405_METHOD_NOT_ALLOWED;
	response_ctx->headers = headers;
	response_ctx->header_count = ARRAY_SIZE(headers);
	response_ctx->final_chunk = true;
}

static int linkr_debugger_http_json_power_output(struct linkr_debugger_http_env *env,
					 const struct linkr_debugger_rail_desc *rail)
{
	bool enabled = linkr_debugger_power_output_enabled(rail);

	if (linkr_debugger_http_append(env, "{\"name\":") < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_http_json_string(env, rail->name) < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_http_append(env, ",\"signal\":") < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_http_json_string(env, rail->signal) < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_http_append(env,
				 ",\"gp\":%u,\"controllable\":%s,\"state\":",
				 (unsigned int)rail->pin,
				 rail->controllable ? "true" : "false") < 0) {
		return -ENOMEM;
	}

	if (!rail->controllable) {
		if (linkr_debugger_http_json_string(env, "locked") < 0) {
			return -ENOMEM;
		}

		return linkr_debugger_http_append(env, ",\"value\":null}");
	}

	if (linkr_debugger_http_json_string(env, enabled ? "on" : "off") < 0) {
		return -ENOMEM;
	}

	return linkr_debugger_http_append(env, ",\"value\":%d}", enabled ? 1 : 0);
}

static int linkr_debugger_http_json_power_outputs(struct linkr_debugger_http_env *env)
{
	if (linkr_debugger_http_append(env, "[") < 0) {
		return -ENOMEM;
	}

	for (size_t i = 0; i < linkr_debugger_rail_count; i++) {
		if (i > 0U && linkr_debugger_http_append(env, ",") < 0) {
			return -ENOMEM;
		}

		if (linkr_debugger_http_json_power_output(env, &linkr_debugger_rails[i]) < 0) {
			return -ENOMEM;
		}
	}

	return linkr_debugger_http_append(env, "]");
}

static int linkr_debugger_http_json_adc_channels(struct linkr_debugger_http_env *env)
{
	if (linkr_debugger_http_append(env, "[") < 0) {
		return -ENOMEM;
	}

	for (size_t i = 0; i < linkr_debugger_adc_count; i++) {
		const struct linkr_debugger_current_desc *current = &linkr_debugger_currents[i];
		const char *sensor_channel;
		const char *unit;

		switch (current->kind) {
		case LINKR_DEBUGGER_ADC_KIND_CURRENT:
			sensor_channel = "current";
			unit = "A";
			break;
		case LINKR_DEBUGGER_ADC_KIND_VOLTAGE:
			sensor_channel = "voltage";
			unit = "V";
			break;
		default:
			return -EINVAL;
		}

		if (i > 0U && linkr_debugger_http_append(env, ",") < 0) {
			return -ENOMEM;
		}

		if (linkr_debugger_http_append(env,
					 "{\"name\":") < 0 ||
		    linkr_debugger_http_json_string(env, current->name) < 0 ||
		    linkr_debugger_http_append(env, ",\"signal\":") < 0 ||
		    linkr_debugger_http_json_string(env, current->signal) < 0 ||
		    linkr_debugger_http_append(env,
					 ",\"adc_index\":%u,\"sensor\":",
					 (unsigned int)current->adc_index) < 0 ||
		    linkr_debugger_http_json_string(env, current->sensor) < 0 ||
		    linkr_debugger_http_append(env,
					 ",\"sensor_channel\":\"%s\",\"unit\":\"%s\"}",
					 sensor_channel, unit) < 0) {
			return -ENOMEM;
		}
	}

	return linkr_debugger_http_append(env, "]");
}

static int linkr_debugger_http_json_safe_gpios(struct linkr_debugger_http_env *env)
{
	char name[LINKR_DEBUGGER_HTTP_GPIO_NAME_BUFSZ];
	int value;
	int ret;

	if (linkr_debugger_http_append(env, "[") < 0) {
		return -ENOMEM;
	}

	for (size_t i = 0; i < linkr_debugger_safe_gpio_count; i++) {
		const struct linkr_debugger_safe_gpio_desc *desc = &linkr_debugger_safe_gpios[i];

		if (i > 0U && linkr_debugger_http_append(env, ",") < 0) {
			return -ENOMEM;
		}

		if (!linkr_debugger_format_gpio_name(desc->pin, name, sizeof(name))) {
			strcpy(name, "GP?");
		}

		ret = linkr_debugger_gpio_get(desc, &value);
		if (ret < 0) {
			value = 0;
		}

		if (linkr_debugger_http_append(env, "{\"name\":") < 0 ||
		    linkr_debugger_http_json_string(env, name) < 0 ||
		    linkr_debugger_http_append(env, ",\"pin\":%u,\"note\":",
					     (unsigned int)desc->pin) < 0 ||
		    linkr_debugger_http_json_string(env, desc->note) < 0 ||
		    linkr_debugger_http_append(env, ",\"layoutGroup\":") < 0 ||
		    linkr_debugger_http_json_string(env, desc->layout_group) < 0 ||
		    linkr_debugger_http_append(env, ",\"layoutLabel\":") < 0 ||
		    linkr_debugger_http_json_string(env, desc->layout_label) < 0 ||
		    linkr_debugger_http_append(env, ",\"layoutRow\":%u,\"layoutColumn\":%u",
					     (unsigned int)desc->layout_row,
					     (unsigned int)desc->layout_column) < 0 ||
		    linkr_debugger_http_append(env, ",\"value\":%d,\"direction\":",
					     value > 0 ? 1 : 0) < 0 ||
		    linkr_debugger_http_json_string(env, linkr_debugger_safe_gpio_direction_name(i)) < 0 ||
		    linkr_debugger_http_append(env, "}") < 0) {
			return -ENOMEM;
		}
	}

	return linkr_debugger_http_append(env, "]");
}

static int linkr_debugger_http_json_gpio_layout(struct linkr_debugger_http_env *env,
					       const struct linkr_debugger_safe_gpio_desc *desc)
{
	if (linkr_debugger_http_append(env, ",\"layoutGroup\":") < 0 ||
	    linkr_debugger_http_json_string(env, desc->layout_group) < 0 ||
	    linkr_debugger_http_append(env, ",\"layoutLabel\":") < 0 ||
	    linkr_debugger_http_json_string(env, desc->layout_label) < 0 ||
	    linkr_debugger_http_append(env, ",\"layoutRow\":%u,\"layoutColumn\":%u",
				     (unsigned int)desc->layout_row,
				     (unsigned int)desc->layout_column) < 0) {
		return -ENOMEM;
	}

	return 0;
}

static int linkr_debugger_http_json_current_reading(struct linkr_debugger_http_env *env,
					       const struct linkr_debugger_current_desc *current,
					       const struct linkr_debugger_current_sample *sample)
{
	if (linkr_debugger_http_append(env, "{\"name\":") < 0 ||
	    linkr_debugger_http_json_string(env, current->name) < 0 ||
	    linkr_debugger_http_append(env, ",\"signal\":") < 0 ||
	    linkr_debugger_http_json_string(env, current->signal) < 0 ||
	    linkr_debugger_http_append(env, ",\"power_enabled\":%s,",
				     sample->rail_enabled ? "true" : "false") < 0) {
		return -ENOMEM;
	}

	if (sample->raw_available) {
		if (linkr_debugger_http_append(env, "\"raw\":%d,", sample->raw) < 0) {
			return -ENOMEM;
		}
	} else if (linkr_debugger_http_append(env, "\"raw\":null,") < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_http_append(env,
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

static int linkr_debugger_http_json_voltage_reading(struct linkr_debugger_http_env *env,
					       const struct linkr_debugger_current_desc *adc,
					       const struct linkr_debugger_current_sample *sample)
{
	if (linkr_debugger_http_append(env, "{\"name\":") < 0 ||
	    linkr_debugger_http_json_string(env, adc->name) < 0 ||
	    linkr_debugger_http_append(env, ",\"signal\":") < 0 ||
	    linkr_debugger_http_json_string(env, adc->signal) < 0) {
		return -ENOMEM;
	}

	if (sample->raw_available) {
		if (linkr_debugger_http_append(env, ",\"raw\":%d", sample->raw) < 0) {
			return -ENOMEM;
		}
	} else if (linkr_debugger_http_append(env, ",\"raw\":null") < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_http_append(env,
					 ",\"mv\":%d,\"sensor_channel\":\"voltage\",\"unit\":\"V\""
					 ",\"sensor_value\":{\"val1\":%d,\"val2\":%d}}",
					 sample->mv,
					 sample->value.val1,
					 sample->value.val2) < 0) {
		return -ENOMEM;
	}

	return 0;
}

static int linkr_debugger_http_json_adc_reading(struct linkr_debugger_http_env *env,
					 const struct linkr_debugger_current_desc *adc,
					 const struct linkr_debugger_current_sample *sample)
{
	switch (adc->kind) {
	case LINKR_DEBUGGER_ADC_KIND_CURRENT:
		return linkr_debugger_http_json_current_reading(env, adc, sample);
	case LINKR_DEBUGGER_ADC_KIND_VOLTAGE:
		return linkr_debugger_http_json_voltage_reading(env, adc, sample);
	default:
		return -EINVAL;
	}
}

static int linkr_debugger_http_json_watchdog_status(struct linkr_debugger_http_env *env)
{
	struct linkr_debugger_watchdog_status status;

	linkr_debugger_watchdog_status_get(&status);

	return linkr_debugger_http_append(env,
				     "{\"supported\":%s,\"automatic\":%s,\"healthy\":%s,"
				     "\"armed\":%s,\"timeout_ms\":%u,"
				     "\"bootloader_on_timeout\":%s,\"failing_service\":",
				     status.supported ? "true" : "false",
				     status.automatic ? "true" : "false",
				     status.healthy ? "true" : "false",
				     status.armed ? "true" : "false",
				     (unsigned int)status.timeout_ms,
				     status.bootloader_on_timeout ? "true" : "false") < 0 ||
	       linkr_debugger_http_json_string(env,
				       status.failing_service != NULL ? status.failing_service : "") < 0 ||
	       linkr_debugger_http_append(env, "}") < 0 ? -ENOMEM : 0;
}

static int linkr_debugger_http_json_switches(struct linkr_debugger_http_env *env)
{
	if (linkr_debugger_http_append(env, "{\"sd\":{\"route\":") < 0 ||
	    linkr_debugger_http_json_string(env, linkr_debugger_sd_route_name()) < 0 ||
	    linkr_debugger_http_append(env,
					",\"routes\":[\"target\",\"usb-reader\"],"
					"\"requires_confirm\":false},\"usb\":{\"route\":") < 0 ||
	    linkr_debugger_http_json_string(env, linkr_debugger_usb_route_name()) < 0 ||
	    linkr_debugger_http_append(env,
					",\"routes\":[\"pc\",\"target\"],"
					"\"requires_confirm\":true},\"tf_wp\":{\"route\":") < 0 ||
	    linkr_debugger_http_json_string(env, linkr_debugger_tf_wp_route_name()) < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_http_append(env,
				",\"routes\":[\"writable\",\"protected\"],"
				"\"requires_confirm\":false}") < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_vin_switch_available()) {
		if (linkr_debugger_http_append(env, ",\"vin\":{\"route\":") < 0 ||
		    linkr_debugger_http_json_string(env, linkr_debugger_vin_route_name()) < 0 ||
		    linkr_debugger_http_append(env,
					    ",\"routes\":[\"1.8v\",\"3.3v\"],"
					    "\"requires_confirm\":true}") < 0) {
			return -ENOMEM;
		}
	}

	return linkr_debugger_http_append(env, "}") < 0 ? -ENOMEM : 0;
}

static int linkr_debugger_http_json_availability(struct linkr_debugger_http_env *env,
						    bool available, const char *reason)
{
	if (linkr_debugger_http_append(env, "{\"available\":%s,\"reason\":",
				       available ? "true" : "false") < 0 ||
	    linkr_debugger_http_json_string(env, reason != NULL ? reason : "") < 0) {
		return -ENOMEM;
	}

	return 0;
}

static int linkr_debugger_http_json_board_monitoring(struct linkr_debugger_http_env *env)
{
	struct linkr_debugger_monitoring_snapshot snapshot;

	linkr_debugger_monitoring_snapshot_get(&snapshot);

	if (linkr_debugger_http_json_availability(env, snapshot.temperature.available,
					      snapshot.temperature.reason) < 0) {
		return -ENOMEM;
	}
	if (snapshot.temperature.available) {
		if (linkr_debugger_http_append(env, ",\"source\":") < 0 ||
		    linkr_debugger_http_json_string(env, snapshot.temperature.source) < 0 ||
		    linkr_debugger_http_append(env,
					     ",\"celsius\":{\"val1\":%d,\"val2\":%d}}",
					     snapshot.temperature.celsius_val1,
					     snapshot.temperature.celsius_val2) < 0) {
			return -ENOMEM;
		}
	} else if (snapshot.temperature.error != 0) {
		if (linkr_debugger_http_append(env, ",\"error\":%d}", snapshot.temperature.error) < 0) {
			return -ENOMEM;
		}
	} else if (linkr_debugger_http_append(env, "}") < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_http_append(env, ",\"heap\":") < 0 ||
	    linkr_debugger_http_json_availability(env, snapshot.heap.available,
					      snapshot.heap.reason) < 0) {
		return -ENOMEM;
	}
	if (snapshot.heap.available) {
		if (linkr_debugger_http_append(env,
					     ",\"source\":") < 0 ||
		    linkr_debugger_http_json_string(env, snapshot.heap.source) < 0 ||
		    linkr_debugger_http_append(env,
					     ",\"free_bytes\":%u,\"allocated_bytes\":%u,"
					     "\"max_allocated_bytes\":%u,\"total_bytes\":%u}",
					     (unsigned int)snapshot.heap.free_bytes,
					     (unsigned int)snapshot.heap.allocated_bytes,
					     (unsigned int)snapshot.heap.max_allocated_bytes,
					     (unsigned int)snapshot.heap.total_bytes) < 0) {
			return -ENOMEM;
		}
	} else if (snapshot.heap.error != 0) {
		if (linkr_debugger_http_append(env, ",\"error\":%d}", snapshot.heap.error) < 0) {
			return -ENOMEM;
		}
	} else if (linkr_debugger_http_append(env, "}") < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_http_append(env, ",\"memory\":") < 0 ||
	    linkr_debugger_http_json_availability(env, snapshot.memory.available,
					      snapshot.memory.reason) < 0 ||
	    linkr_debugger_http_append(env, ",\"source\":") < 0 ||
	    linkr_debugger_http_json_string(env, snapshot.memory.source) < 0 ||
	    linkr_debugger_http_append(env, ",\"coverage\":") < 0 ||
	    linkr_debugger_http_json_string(env, snapshot.memory.coverage) < 0 ||
	    linkr_debugger_http_append(env,
				     ",\"pressure_pct_x100\":%u,\"limiting_component\":",
				     (unsigned int)snapshot.memory.pressure_pct_x100) < 0 ||
	    linkr_debugger_http_json_string(env, snapshot.memory.limiting_component) < 0 ||
	    linkr_debugger_http_append(env, ",\"limiting_name\":") < 0 ||
	    linkr_debugger_http_json_string(env, snapshot.memory.limiting_name) < 0 ||
	    linkr_debugger_http_append(env,
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
	    linkr_debugger_http_json_string(env, snapshot.memory.stacks.max_pressure_thread) < 0 ||
	    linkr_debugger_http_append(env, "},\"current_pressure\":") < 0 ||
	    linkr_debugger_http_json_availability(env, snapshot.memory.current_pressure.available,
						  snapshot.memory.current_pressure.reason) < 0 ||
	    linkr_debugger_http_append(env, ",\"coverage\":") < 0 ||
	    linkr_debugger_http_json_string(env, snapshot.memory.current_pressure.coverage) < 0 ||
	    linkr_debugger_http_append(env,
				     ",\"pressure_pct_x100\":%u,\"limiting_component\":",
				     (unsigned int)snapshot.memory.current_pressure.pressure_pct_x100) < 0 ||
	    linkr_debugger_http_json_string(env,
					 snapshot.memory.current_pressure.limiting_component) < 0 ||
	    linkr_debugger_http_append(env, ",\"limiting_name\":") < 0 ||
	    linkr_debugger_http_json_string(env, snapshot.memory.current_pressure.limiting_name) < 0 ||
	    linkr_debugger_http_append(env, ",\"tie_count\":%u},\"peak_pressure\":",
				     (unsigned int)snapshot.memory.current_pressure.tie_count) < 0 ||
	    linkr_debugger_http_json_availability(env, snapshot.memory.peak_pressure.available,
						  snapshot.memory.peak_pressure.reason) < 0 ||
	    linkr_debugger_http_append(env, ",\"coverage\":") < 0 ||
	    linkr_debugger_http_json_string(env, snapshot.memory.peak_pressure.coverage) < 0 ||
	    linkr_debugger_http_append(env,
				     ",\"pressure_pct_x100\":%u,\"limiting_component\":",
				     (unsigned int)snapshot.memory.peak_pressure.pressure_pct_x100) < 0 ||
	    linkr_debugger_http_json_string(env,
					 snapshot.memory.peak_pressure.limiting_component) < 0 ||
	    linkr_debugger_http_append(env, ",\"limiting_name\":") < 0 ||
	    linkr_debugger_http_json_string(env, snapshot.memory.peak_pressure.limiting_name) < 0 ||
	    linkr_debugger_http_append(env, ",\"tie_count\":%u,\"since\":",
				     (unsigned int)snapshot.memory.peak_pressure.tie_count) < 0 ||
	    linkr_debugger_http_json_string(env, "boot") < 0 ||
	    linkr_debugger_http_append(env, "}}") < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_http_append(env, ",\"runtime\":") < 0 ||
	    linkr_debugger_http_json_availability(env, snapshot.runtime.available,
					      snapshot.runtime.reason) < 0) {
		return -ENOMEM;
	}
	if (snapshot.runtime.available) {
		if (linkr_debugger_http_append(env,
					     ",\"uptime_ms\":%lld,\"uptime_seconds\":%llu}",
					     (long long)snapshot.runtime.uptime_ms,
					     (unsigned long long)snapshot.runtime.uptime_seconds) < 0) {
			return -ENOMEM;
		}
	} else if (snapshot.runtime.error != 0) {
		if (linkr_debugger_http_append(env, ",\"error\":%d}", snapshot.runtime.error) < 0) {
			return -ENOMEM;
		}
	} else if (linkr_debugger_http_append(env, "}") < 0) {
		return -ENOMEM;
	}

	if (linkr_debugger_http_append(env, ",\"cpu\":") < 0 ||
	    linkr_debugger_http_json_availability(env, snapshot.cpu.available,
					      snapshot.cpu.reason) < 0) {
		return -ENOMEM;
	}
	if (snapshot.cpu.available) {
		if (linkr_debugger_http_append(env,
					     ",\"active_pct_x100\":%u,\"window_ms\":%u,"
					     "\"busy_cycles_delta\":%llu,\"total_cycles_delta\":%llu}",
					     (unsigned int)snapshot.cpu.active_pct_x100,
					     (unsigned int)snapshot.cpu.window_ms,
					     snapshot.cpu.busy_cycles_delta,
					     snapshot.cpu.total_cycles_delta) < 0) {
			return -ENOMEM;
		}
	} else if (snapshot.cpu.error != 0) {
		if (linkr_debugger_http_append(env, ",\"error\":%d}", snapshot.cpu.error) < 0) {
			return -ENOMEM;
		}
	} else if (linkr_debugger_http_append(env, "}") < 0) {
		return -ENOMEM;
	}

	return linkr_debugger_http_append(env, "}");
}

static int linkr_debugger_http_handle_status(struct http_client_ctx *client,
					 enum http_transaction_status status,
					 const struct http_request_ctx *request_ctx,
					 struct http_response_ctx *response_ctx,
					 void *user_data)
{
	static uint8_t json_buf[LINKR_DEBUGGER_HTTP_STATUS_JSON_BUFSZ];
	struct linkr_debugger_config_service_status config_status;
	const struct linkr_debugger_config_service_status *config_status_ptr = NULL;
	struct linkr_debugger_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};

	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	if (client->method != HTTP_GET) {
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_405_METHOD_NOT_ALLOWED,
				     "status", "method_not_allowed", "method not allowed");
		return 0;
	}

	if (linkr_debugger_config_service_status_get(&config_status) ==
	    LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		config_status_ptr = &config_status;
	}

	k_mutex_lock(&linkr_debugger_http_lock, K_FOREVER);
	if (linkr_debugger_http_json_begin(&env, "status", true) < 0 ||
	    linkr_debugger_http_append(&env,
				     ",\"project\":\"radxa-linkr-debugger\",\"mcu\":") < 0 ||
	    linkr_debugger_http_json_string(&env, linkr_debugger_mcu_name()) < 0 ||
	    linkr_debugger_http_append(&env, ",\"usb\":") < 0 ||
	    linkr_debugger_http_json_string(&env, linkr_debugger_usb_mode()) < 0 ||
	    linkr_debugger_http_append(&env, ",\"power_capture_protocol\":") < 0 ||
	    linkr_debugger_http_json_string(&env, linkr_debugger_power_capture_protocol()) < 0 ||
	    linkr_debugger_http_append(&env,
				 ",\"power_inputs\":[{\"name\":\"5v_fin\",\"controllable\":false,\"measured\":false}]"
				 ",\"power_outputs\":") < 0 ||
	    linkr_debugger_http_json_power_outputs(&env) < 0 ||
	    linkr_debugger_http_append(&env, ",\"switches\":") < 0 ||
	    linkr_debugger_http_json_switches(&env) < 0 ||
	    linkr_debugger_http_append(&env, ",\"adc_channels\":") < 0 ||
	    linkr_debugger_http_json_adc_channels(&env) < 0 ||
	    linkr_debugger_http_append(&env, ",\"watchdog\":") < 0 ||
	    linkr_debugger_http_json_watchdog_status(&env) < 0 ||
	    linkr_debugger_http_append(&env, ",\"board_monitoring\":{\"temperature\":") < 0 ||
	    linkr_debugger_http_json_board_monitoring(&env) < 0 ||
	    linkr_debugger_http_append(&env, ",\"gpios\":") < 0 ||
	    linkr_debugger_http_json_safe_gpios(&env) < 0) {
		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_500_INTERNAL_SERVER_ERROR, "status",
				     "response_too_large", "failed to encode status response");
		return 0;
	}

	if (config_status_ptr != NULL) {
		struct linkr_debugger_config_summary_buffer config_summary_buffer = {
			.data = env.buf,
			.capacity = env.cap,
			.length = env.len,
			.tail_reserve = 2U,
		};

		(void)linkr_debugger_config_summary_append(&config_summary_buffer,
						   config_status_ptr);
		env.len = config_summary_buffer.length;
	}

	if (linkr_debugger_http_append(&env, "}\n") < 0) {
		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_500_INTERNAL_SERVER_ERROR, "status",
				     "response_too_large", "failed to encode status response");
		return 0;
	}
	k_mutex_unlock(&linkr_debugger_http_lock);

	linkr_debugger_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
	return 0;
}

static int linkr_debugger_http_handle_power(struct http_client_ctx *client,
					enum http_transaction_status status,
					const struct http_request_ctx *request_ctx,
					struct http_response_ctx *response_ctx,
					void *user_data)
{
	static uint8_t json_buf[LINKR_DEBUGGER_HTTP_JSON_BUFSZ];
	char *path;
	const struct linkr_debugger_rail_desc *rail = NULL;
	struct linkr_debugger_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};

	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	path = strchr((char *)client->url_buffer + strlen("/api/v1/power"), '/');
	if (path != NULL) {
		rail = linkr_debugger_find_rail(path + 1);
		if (rail == NULL) {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_404_NOT_FOUND,
					     "power", "unknown_power_output",
					     "unknown power output");
			return 0;
		}
	}

	k_mutex_lock(&linkr_debugger_http_lock, K_FOREVER);
	switch (client->method) {
	case HTTP_GET:
		if (linkr_debugger_http_json_begin(&env, "power", true) < 0 ||
		    linkr_debugger_http_append(&env, ",\"action\":") < 0 ||
		    linkr_debugger_http_json_string(&env, rail == NULL ? "list" : "get") < 0) {
			break;
		}

		if (rail == NULL) {
			if (linkr_debugger_http_append(&env, ",\"power_outputs\":") < 0 ||
			    linkr_debugger_http_json_power_outputs(&env) < 0 ||
			    linkr_debugger_http_append(&env,
					     ",\"power_inputs\":[{\"name\":\"5v_fin\",\"controllable\":false,\"measured\":false}]}\n") < 0) {
				break;
			}
		} else if (linkr_debugger_http_append(&env, ",\"power_output\":") < 0 ||
			   linkr_debugger_http_json_power_output(&env, rail) < 0 ||
			   linkr_debugger_http_append(&env, "}\n") < 0) {
			break;
		}

		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;

	case HTTP_PUT: {
		struct linkr_debugger_http_power_set_request req = { 0 };
		bool enabled;
		int ret;

		if (rail == NULL) {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "power", "missing_power_output",
					     "missing power output in URL");
			return 0;
		}

		if (request_ctx->data == NULL || request_ctx->data_len == 0U) {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "power", "missing_body",
					     "missing JSON request body");
			return 0;
		}

		ret = json_obj_parse((char *)request_ctx->data, request_ctx->data_len,
				     power_set_request_descr,
				     ARRAY_SIZE(power_set_request_descr), &req);
		if (ret < 0 || !linkr_debugger_parse_bool_arg(req.state, &enabled)) {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "power", "invalid_state",
					     "state must be on/off or 1/0");
			return 0;
		}

		ret = linkr_debugger_power_output_set(rail, enabled);
		if (ret == -EPERM) {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_403_FORBIDDEN,
					     "power", "power_output_locked",
					     "power output is locked in this build");
			return 0;
		}
		if (ret < 0) {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
					     HTTP_500_INTERNAL_SERVER_ERROR,
					     "power", "set_failed",
					     "failed to set power output");
			return 0;
		}

		linkr_debugger_ws_publish_state_change();

		if (linkr_debugger_http_json_begin(&env, "power", true) < 0 ||
		    linkr_debugger_http_append(&env, ",\"action\":\"set\",\"power_output\":") < 0 ||
		    linkr_debugger_http_json_power_output(&env, rail) < 0 ||
		    linkr_debugger_http_append(&env, "}\n") < 0) {
			break;
		}

		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;
	}

	default:
		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_405_METHOD_NOT_ALLOWED,
				     "power", "method_not_allowed", "method not allowed");
		return 0;
	}

	k_mutex_unlock(&linkr_debugger_http_lock);
	linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
			     "power", "response_too_large", "failed to encode power response");
	return 0;
}

static int linkr_debugger_http_handle_adc(struct http_client_ctx *client,
				      enum http_transaction_status status,
				      const struct http_request_ctx *request_ctx,
				      struct http_response_ctx *response_ctx,
				      void *user_data)
{
	static uint8_t json_buf[LINKR_DEBUGGER_HTTP_JSON_BUFSZ];
	struct linkr_debugger_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};
	const struct linkr_debugger_current_desc *single = NULL;
	char *query;

	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	if (client->method != HTTP_GET) {
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_405_METHOD_NOT_ALLOWED,
				     "adc", "method_not_allowed", "method not allowed");
		return 0;
	}

	query = strstr((char *)client->url_buffer, "channel=");
	if (query != NULL) {
		single = linkr_debugger_find_adc(query + strlen("channel="));
		if (single == NULL) {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_404_NOT_FOUND,
					     "adc", "unknown_channel", "unknown adc channel");
			return 0;
		}
	}

	k_mutex_lock(&linkr_debugger_http_lock, K_FOREVER);
	if (linkr_debugger_http_json_begin(&env, "adc", true) < 0 ||
	    linkr_debugger_http_append(&env, ",\"action\":\"read\",\"readings\":[") < 0) {
		goto too_large;
	}

	for (size_t i = 0; i < linkr_debugger_adc_count; i++) {
		struct linkr_debugger_current_sample sample;
		const struct linkr_debugger_current_desc *current = &linkr_debugger_currents[i];
		int ret;

		if (single != NULL && current != single) {
			continue;
		}

		ret = linkr_debugger_current_read(current, &sample);
		if (ret < 0) {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
					     HTTP_500_INTERNAL_SERVER_ERROR,
					     "adc", "read_failed", "failed to read adc channel");
			return 0;
		}

		if (env.len > 0U && env.buf[env.len - 1U] != '[' &&
		    linkr_debugger_http_append(&env, ",") < 0) {
			goto too_large;
		}

		if (linkr_debugger_http_json_adc_reading(&env, current, &sample) < 0) {
			goto too_large;
		}
	}

	if (linkr_debugger_http_append(&env, "]}\n") < 0) {
		goto too_large;
	}

	k_mutex_unlock(&linkr_debugger_http_lock);
	linkr_debugger_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
	return 0;

too_large:
	k_mutex_unlock(&linkr_debugger_http_lock);
	linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
			     "adc", "response_too_large", "failed to encode adc response");
	return 0;
}

static int linkr_debugger_http_handle_switch(struct http_client_ctx *client,
				 enum http_transaction_status status,
				 const struct http_request_ctx *request_ctx,
				 struct http_response_ctx *response_ctx,
				 void *user_data)
{
	static uint8_t json_buf[LINKR_DEBUGGER_HTTP_JSON_BUFSZ];
	char *path;
	struct linkr_debugger_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};

	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	path = strchr((char *)client->url_buffer + strlen("/api/v1/switch"), '/');
	k_mutex_lock(&linkr_debugger_http_lock, K_FOREVER);
	switch (client->method) {
	case HTTP_GET:
		if (path == NULL) {
			if (linkr_debugger_http_json_begin(&env, "switch", true) < 0 ||
			    linkr_debugger_http_append(&env, ",\"action\":\"list\",\"switches\":") < 0 ||
			    linkr_debugger_http_json_switches(&env) < 0 ||
			    linkr_debugger_http_append(&env, "}\n") < 0) {
				break;
			}
		} else if (strcmp(path + 1, "sd") == 0) {
			if (linkr_debugger_http_json_begin(&env, "switch", true) < 0 ||
			    linkr_debugger_http_append(&env, ",\"action\":\"get\",\"name\":\"sd\",\"route\":") < 0 ||
			    linkr_debugger_http_json_string(&env, linkr_debugger_sd_route_name()) < 0 ||
			    linkr_debugger_http_append(&env,
					    ",\"routes\":[\"target\",\"usb-reader\"],"
					    "\"requires_confirm\":false}\n") < 0) {
				break;
			}
		} else if (strcmp(path + 1, "usb") == 0) {
			if (linkr_debugger_http_json_begin(&env, "switch", true) < 0 ||
			    linkr_debugger_http_append(&env, ",\"action\":\"get\",\"name\":\"usb\",\"route\":") < 0 ||
			    linkr_debugger_http_json_string(&env, linkr_debugger_usb_route_name()) < 0 ||
			    linkr_debugger_http_append(&env,
					    ",\"routes\":[\"pc\",\"target\"],"
					    "\"requires_confirm\":true}\n") < 0) {
				break;
			}
		} else if (strcmp(path + 1, "tf_wp") == 0) {
			if (linkr_debugger_http_json_begin(&env, "switch", true) < 0 ||
			    linkr_debugger_http_append(&env, ",\"action\":\"get\",\"name\":\"tf_wp\",\"route\":") < 0 ||
			    linkr_debugger_http_json_string(&env, linkr_debugger_tf_wp_route_name()) < 0 ||
			    linkr_debugger_http_append(&env,
					    ",\"routes\":[\"writable\",\"protected\"],"
					    "\"requires_confirm\":false}\n") < 0) {
				break;
			}
		} else if (strcmp(path + 1, "vin") == 0 && linkr_debugger_vin_switch_available()) {
			if (linkr_debugger_http_json_begin(&env, "switch", true) < 0 ||
			    linkr_debugger_http_append(&env, ",\"action\":\"get\",\"name\":\"vin\",\"route\":") < 0 ||
			    linkr_debugger_http_json_string(&env, linkr_debugger_vin_route_name()) < 0 ||
			    linkr_debugger_http_append(&env,
					    ",\"routes\":[\"1.8v\",\"3.3v\"],"
					    "\"requires_confirm\":true}\n") < 0) {
				break;
			}
		} else {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_404_NOT_FOUND,
					     "switch", "not_found", "unknown switch");
			return 0;
		}

		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;

	case HTTP_PUT: {
		struct linkr_debugger_http_switch_route_request req = { 0 };
		int ret;

		if (path == NULL) {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "switch", "missing_switch", "missing switch name in URL");
			return 0;
		}

		if (request_ctx->data == NULL || request_ctx->data_len == 0U) {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "switch", "missing_body", "missing JSON request body");
			return 0;
		}

		ret = json_obj_parse((char *)request_ctx->data, request_ctx->data_len,
				     switch_route_request_descr,
				     ARRAY_SIZE(switch_route_request_descr), &req);
		if (ret < 0) {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "switch", "invalid_route", "route must be valid for the selected switch");
			return 0;
		}

		if (strcmp(path + 1, "sd") == 0) {
			enum linkr_debugger_sd_route route;
			if (strcmp(req.route, "target") == 0) {
				route = LINKR_DEBUGGER_SD_ROUTE_TARGET;
			} else if (strcmp(req.route, "usb-reader") == 0 || strcmp(req.route, "reader") == 0) {
				route = LINKR_DEBUGGER_SD_ROUTE_USB_READER;
			} else {
				k_mutex_unlock(&linkr_debugger_http_lock);
				linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
						     "switch", "invalid_route", "sd route must be target or usb-reader");
				return 0;
			}
			ret = linkr_debugger_sd_route_set(route);
		} else if (strcmp(path + 1, "usb") == 0) {
			enum linkr_debugger_usb_route route;
			if (strcmp(req.route, "pc") == 0) {
				route = LINKR_DEBUGGER_USB_ROUTE_PC;
			} else if (strcmp(req.route, "target") == 0) {
				route = LINKR_DEBUGGER_USB_ROUTE_TARGET;
			} else {
				k_mutex_unlock(&linkr_debugger_http_lock);
				linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
						     "switch", "invalid_route", "usb route must be pc or target");
				return 0;
			}
			ret = linkr_debugger_usb_route_set(route);
		} else if (strcmp(path + 1, "tf_wp") == 0) {
			enum linkr_debugger_tf_wp_route route;

			if (!linkr_debugger_parse_tf_wp_route(req.route, &route)) {
				k_mutex_unlock(&linkr_debugger_http_lock);
				linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
						     "switch", "invalid_route", "tf_wp route must be writable or protected");
				return 0;
			}
			ret = linkr_debugger_tf_wp_route_set(route);
		} else if (strcmp(path + 1, "vin") == 0 && linkr_debugger_vin_switch_available()) {
			enum linkr_debugger_vin_route route;

			if (!linkr_debugger_parse_vin_route(req.route, &route)) {
				k_mutex_unlock(&linkr_debugger_http_lock);
				linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
						     "switch", "invalid_route", "vin route must be 1.8v or 3.3v");
				return 0;
			}
			ret = linkr_debugger_vin_route_set(route);
		} else {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_404_NOT_FOUND,
					     "switch", "not_found", "unknown switch");
			return 0;
		}

		if (ret < 0) {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
					     "switch", "set_failed", "failed to set switch route");
			return 0;
		}

		linkr_debugger_ws_publish_state_change();

		if (linkr_debugger_http_json_begin(&env, "switch", true) < 0 ||
		    linkr_debugger_http_append(&env, ",\"action\":\"route\",\"name\":") < 0 ||
		    linkr_debugger_http_json_string(&env, path + 1) < 0 ||
		    linkr_debugger_http_append(&env, ",\"route\":") < 0 ||
		    linkr_debugger_http_json_string(&env,
			    strcmp(path + 1, "sd") == 0 ? linkr_debugger_sd_route_name() :
			    strcmp(path + 1, "usb") == 0 ? linkr_debugger_usb_route_name() :
			    strcmp(path + 1, "tf_wp") == 0 ? linkr_debugger_tf_wp_route_name() :
			    linkr_debugger_vin_route_name()) < 0 ||
		    linkr_debugger_http_append(&env, "}\n") < 0) {
			break;
		}

		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;
	}

	default:
		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_405_METHOD_NOT_ALLOWED,
				     "switch", "method_not_allowed", "method not allowed");
		return 0;
	}

	k_mutex_unlock(&linkr_debugger_http_lock);
	linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
			     "switch", "response_too_large", "failed to encode switch response");
	return 0;
}

static int linkr_debugger_http_handle_gpio(struct http_client_ctx *client,
				       enum http_transaction_status status,
				       const struct http_request_ctx *request_ctx,
				       struct http_response_ctx *response_ctx,
				       void *user_data)
{
	static uint8_t json_buf[LINKR_DEBUGGER_HTTP_JSON_BUFSZ];
	char *path;
	char name[LINKR_DEBUGGER_HTTP_GPIO_NAME_BUFSZ];
	char identifier[LINKR_DEBUGGER_HTTP_GPIO_IDENT_BUFSZ];
	const struct linkr_debugger_safe_gpio_desc *desc = NULL;
	struct linkr_debugger_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};

	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	path = strchr((char *)client->url_buffer + strlen("/api/v1/gpio"), '/');
	if (path != NULL) {
		if (!linkr_debugger_http_percent_decode(path + 1, identifier, sizeof(identifier))) {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "gpio", "invalid_gpio",
					     "GPIO target must be GP13, 13, or an allowlist note such as CON_MAS");
			return 0;
		}

		desc = linkr_debugger_find_safe_gpio_by_identifier(identifier);
		if (desc == NULL) {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_403_FORBIDDEN,
					     "gpio", "not_allowed", "GPIO is not in the allowlist");
			return 0;
		}
	}

	k_mutex_lock(&linkr_debugger_http_lock, K_FOREVER);
	switch (client->method) {
	case HTTP_GET:
		if (desc == NULL) {
			if (linkr_debugger_http_json_begin(&env, "gpio", true) < 0 ||
			    linkr_debugger_http_append(&env, ",\"action\":\"list\",\"gpios\":") < 0 ||
			    linkr_debugger_http_json_safe_gpios(&env) < 0 ||
			    linkr_debugger_http_append(&env, ",\"reserved\":") < 0 ||
			    linkr_debugger_http_json_string(&env, linkr_debugger_reserved_gpios()) < 0 ||
			    linkr_debugger_http_append(&env, "}\n") < 0) {
				break;
			}
		} else {
			int value;
			int ret = linkr_debugger_gpio_get(desc, &value);
			if (ret < 0) {
				k_mutex_unlock(&linkr_debugger_http_lock);
				linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
						     HTTP_500_INTERNAL_SERVER_ERROR,
						     "gpio", "read_failed", "failed to read GPIO");
				return 0;
			}

			if (!linkr_debugger_format_gpio_name(desc->pin, name, sizeof(name))) {
				strcpy(name, "GP?");
			}

			if (linkr_debugger_http_json_begin(&env, "gpio", true) < 0 ||
			    linkr_debugger_http_append(&env, ",\"action\":\"get\",\"gpio\":{\"name\":") < 0 ||
			    linkr_debugger_http_json_string(&env, name) < 0 ||
			    linkr_debugger_http_append(&env,
					     ",\"pin\":%u,\"note\":", (unsigned int)desc->pin) < 0 ||
			    linkr_debugger_http_json_string(&env, desc->note) < 0 ||
			    linkr_debugger_http_json_gpio_layout(&env, desc) < 0 ||
			    linkr_debugger_http_append(&env, ",\"direction\":") < 0 ||
			    linkr_debugger_http_json_string(&env,
				    linkr_debugger_safe_gpio_direction(desc)) < 0 ||
			    linkr_debugger_http_append(&env, ",\"value\":%d}}\n",
					     value > 0 ? 1 : 0) < 0) {
				break;
			}
		}

		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;

	case HTTP_PUT: {
		struct linkr_debugger_http_gpio_write_request req = { 0 };
		int ret;

		if (desc == NULL) {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "gpio", "missing_gpio", "missing GPIO in URL");
			return 0;
		}

		if (request_ctx->data == NULL || request_ctx->data_len == 0U) {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "gpio", "missing_body", "missing JSON request body");
			return 0;
		}

		ret = json_obj_parse((char *)request_ctx->data, request_ctx->data_len,
				     gpio_write_request_descr,
				     ARRAY_SIZE(gpio_write_request_descr), &req);
		if (ret < 0) {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "gpio", "invalid_request",
					     "request must contain direction and optional value");
			return 0;
		}

		if (!linkr_debugger_format_gpio_name(desc->pin, name, sizeof(name))) {
			strcpy(name, "GP?");
		}

		if (strcmp(req.direction, "input") == 0) {
			ret = linkr_debugger_gpio_set_input(desc);
			if (ret < 0) {
				k_mutex_unlock(&linkr_debugger_http_lock);
				linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
						     HTTP_500_INTERNAL_SERVER_ERROR,
						     "gpio", "configure_failed",
						     "failed to configure GPIO input");
				return 0;
			}

			linkr_debugger_ws_publish_state_change();

			if (linkr_debugger_http_json_begin(&env, "gpio", true) < 0 ||
			    linkr_debugger_http_append(&env,
					     ",\"action\":\"input\",\"gpio\":{\"name\":") < 0 ||
			    linkr_debugger_http_json_string(&env, name) < 0 ||
			    linkr_debugger_http_append(&env,
					     ",\"pin\":%u,\"note\":", (unsigned int)desc->pin) < 0 ||
			    linkr_debugger_http_json_string(&env, desc->note) < 0 ||
			    linkr_debugger_http_json_gpio_layout(&env, desc) < 0 ||
			    linkr_debugger_http_append(&env,
					     ",\"direction\":\"input\",\"value\":null}}\n") < 0) {
				break;
			}
		} else if (strcmp(req.direction, "output") == 0) {
			bool value = req.value != 0;

			ret = linkr_debugger_gpio_set_output(desc, value);
			if (ret < 0) {
				k_mutex_unlock(&linkr_debugger_http_lock);
				linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
						     HTTP_500_INTERNAL_SERVER_ERROR,
						     "gpio", "configure_failed",
						     "failed to configure GPIO output");
				return 0;
			}

			linkr_debugger_ws_publish_state_change();

			if (linkr_debugger_http_json_begin(&env, "gpio", true) < 0 ||
			    linkr_debugger_http_append(&env,
					     ",\"action\":\"set\",\"gpio\":{\"name\":") < 0 ||
			    linkr_debugger_http_json_string(&env, name) < 0 ||
			    linkr_debugger_http_append(&env,
					     ",\"pin\":%u,\"note\":", (unsigned int)desc->pin) < 0 ||
			    linkr_debugger_http_json_string(&env, desc->note) < 0 ||
			    linkr_debugger_http_json_gpio_layout(&env, desc) < 0 ||
			    linkr_debugger_http_append(&env,
					     ",\"direction\":\"output\",\"value\":%d}}\n",
					     value ? 1 : 0) < 0) {
				break;
			}
		} else {
			k_mutex_unlock(&linkr_debugger_http_lock);
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "gpio", "invalid_request",
					     "direction must be input or output");
			return 0;
		}

		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;
	}

	default:
		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_405_METHOD_NOT_ALLOWED,
				     "gpio", "method_not_allowed", "method not allowed");
		return 0;
	}

	k_mutex_unlock(&linkr_debugger_http_lock);
	linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
			     "gpio", "response_too_large", "failed to encode gpio response");
	return 0;
}

static int linkr_debugger_http_handle_bootloader(struct http_client_ctx *client,
				     enum http_transaction_status status,
				     const struct http_request_ctx *request_ctx,
				     struct http_response_ctx *response_ctx,
				     void *user_data)
{
	static uint8_t json_buf[LINKR_DEBUGGER_HTTP_JSON_BUFSZ];
	struct linkr_debugger_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};

	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	if (linkr_debugger_http_json_begin(&env, "bootloader", true) < 0 ||
	    linkr_debugger_http_append(&env, ",\"message\":\"entering %s BOOTSEL in 250 ms\"}\n",
				     linkr_debugger_mcu_name()) < 0) {
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
				     "bootloader", "response_too_large",
				     "failed to encode bootloader response");
		return 0;
	}

	linkr_debugger_ws_publish_state_change();
	(void)k_work_reschedule(&linkr_debugger_bootloader_work, K_MSEC(250));
	linkr_debugger_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
	return 0;
}

static int linkr_debugger_http_handle_target_recovery(
	struct http_client_ctx *client,
	enum http_transaction_status status,
	const struct http_request_ctx *request_ctx,
	struct http_response_ctx *response_ctx,
	void *user_data)
{
	static uint8_t json_buf[LINKR_DEBUGGER_HTTP_JSON_BUFSZ];
	struct linkr_debugger_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};
	struct linkr_debugger_http_target_recovery_request req = { 0 };
	enum linkr_debugger_target_recovery_mode mode;
	const struct linkr_debugger_rail_desc *rail;
	int ret;

	ARG_UNUSED(user_data);
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	k_mutex_lock(&linkr_debugger_http_lock, K_FOREVER);
	if (client->method == HTTP_GET) {
		ret = linkr_debugger_http_json_begin(&env, "target-recovery", true);
		if (ret >= 0) {
			ret = linkr_debugger_http_append(
				&env,
				",\"modes\":[{\"name\":\"qualcomm-edl\",\"active_level\":1},"
				"{\"name\":\"rockchip-maskrom\",\"active_level\":0}],"
				"\"rails\":[\"5v_out\",\"12v_out\",\"20v_out\"],"
				"\"off_ms\":%u,\"setup_ms\":%u,\"hold_ms\":%u,"
				"\"release_direction\":\"input\"}\n",
				LINKR_DEBUGGER_TARGET_RECOVERY_OFF_MS,
				LINKR_DEBUGGER_TARGET_RECOVERY_SETUP_MS,
				LINKR_DEBUGGER_TARGET_RECOVERY_HOLD_MS);
		}
		k_mutex_unlock(&linkr_debugger_http_lock);
		if (ret < 0) {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
					     HTTP_500_INTERNAL_SERVER_ERROR,
					     "target-recovery", "response_too_large",
					     "failed to encode target recovery response");
			return 0;
		}
		linkr_debugger_http_set_json_response(response_ctx, json_buf,
						       sizeof(json_buf), HTTP_200_OK);
		return 0;
	}

	if (client->method != HTTP_POST) {
		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_405_METHOD_NOT_ALLOWED,
				     "target-recovery", "method_not_allowed",
				     "method not allowed");
		return 0;
	}
	if (request_ctx->data == NULL || request_ctx->data_len == 0U) {
		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_400_BAD_REQUEST,
				     "target-recovery", "missing_body",
				     "missing JSON request body");
		return 0;
	}

	ret = json_obj_parse((char *)request_ctx->data, request_ctx->data_len,
			     target_recovery_request_descr,
			     ARRAY_SIZE(target_recovery_request_descr), &req);
	if (ret < 0 || !linkr_debugger_parse_target_recovery_mode(req.mode, &mode)) {
		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_400_BAD_REQUEST,
				     "target-recovery", "invalid_mode",
				     "mode must be qualcomm-edl or rockchip-maskrom");
		return 0;
	}

	rail = linkr_debugger_find_rail(req.rail);
	if (!linkr_debugger_target_recovery_rail_allowed(rail)) {
		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_400_BAD_REQUEST,
				     "target-recovery", "invalid_rail",
				     "rail must be 5v_out, 12v_out, or 20v_out");
		return 0;
	}

	ret = linkr_debugger_target_recovery_enter(mode, rail);
	if (ret < 0) {
		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_500_INTERNAL_SERVER_ERROR,
				     "target-recovery", "sequence_failed",
				     "target recovery sequence failed; CON_MAS was released");
		return 0;
	}

	linkr_debugger_ws_publish_state_change();
	ret = linkr_debugger_http_json_begin(&env, "target-recovery", true);
	if (ret >= 0) {
		ret = linkr_debugger_http_append(
			&env,
			",\"action\":\"enter\",\"mode\":\"%s\",\"rail\":\"%s\","
			"\"active_level\":%u,\"off_ms\":%u,\"setup_ms\":%u,"
			"\"hold_ms\":%u,\"release_direction\":\"input\"}\n",
			linkr_debugger_target_recovery_mode_to_string(mode), rail->name,
			linkr_debugger_target_recovery_active_level(mode) ? 1U : 0U,
			LINKR_DEBUGGER_TARGET_RECOVERY_OFF_MS,
			LINKR_DEBUGGER_TARGET_RECOVERY_SETUP_MS,
			LINKR_DEBUGGER_TARGET_RECOVERY_HOLD_MS);
	}
	k_mutex_unlock(&linkr_debugger_http_lock);
	if (ret < 0) {
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_500_INTERNAL_SERVER_ERROR,
				     "target-recovery", "response_too_large",
				     "failed to encode target recovery response");
		return 0;
	}

	linkr_debugger_http_set_json_response(response_ctx, json_buf, sizeof(json_buf),
					       HTTP_200_OK);
	return 0;
}

static int linkr_debugger_http_handle_watchdog(struct http_client_ctx *client,
				     enum http_transaction_status status,
				     const struct http_request_ctx *request_ctx,
				     struct http_response_ctx *response_ctx,
				     void *user_data)
{
	static uint8_t json_buf[LINKR_DEBUGGER_HTTP_JSON_BUFSZ];
	struct linkr_debugger_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};
	enum linkr_debugger_http_body_event body_event =
		linkr_debugger_http_body_event_from_status(status);

	ARG_UNUSED(user_data);

	if (!linkr_debugger_http_body_should_handle(client->method == HTTP_POST, body_event)) {
		return 0;
	}

	k_mutex_lock(&linkr_debugger_http_lock, K_FOREVER);
	switch (client->method) {
	case HTTP_GET:
		if (linkr_debugger_http_json_begin(&env, "watchdog", true) < 0 ||
		    linkr_debugger_http_append(&env, ",\"action\":\"status\",\"watchdog\":") < 0 ||
		    linkr_debugger_http_json_watchdog_status(&env) < 0 ||
		    linkr_debugger_http_append(&env, "}\n") < 0) {
			break;
		}

		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;

	case HTTP_POST:
		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_405_METHOD_NOT_ALLOWED,
				     "watchdog", "manual_feed_removed",
				     "watchdog is supervised by firmware and cannot be fed manually");
		return 0;

	default:
		k_mutex_unlock(&linkr_debugger_http_lock);
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf),
				     HTTP_405_METHOD_NOT_ALLOWED,
				     "watchdog", "method_not_allowed", "method not allowed");
		return 0;
	}

	k_mutex_unlock(&linkr_debugger_http_lock);
	linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
			     "watchdog", "response_too_large", "failed to encode watchdog response");
	return 0;
}

static void linkr_debugger_bootloader_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)linkr_debugger_bootloader_now();
}

static const uint8_t linkr_debugger_web_index_html_gz[] = {
#include "linkr_debugger_web_index.html.gz.inc"
};

static const uint8_t linkr_debugger_web_app_css_gz[] = {
#include "linkr_debugger_web_app.css.gz.inc"
};

static const uint8_t linkr_debugger_web_app_js_gz[] = {
#include "linkr_debugger_web_app.js.gz.inc"
};

static const uint8_t linkr_debugger_web_logic_decoder_js_gz[] = {
#include "linkr_debugger_web_logic_decoder.js.gz.inc"
};

static const uint8_t linkr_debugger_web_logic_decoder_wasm_gz[] = {
#include "linkr_debugger_web_logic_decoder_bg.wasm.gz.inc"
};

#define LINKR_DEBUGGER_WEB_RESOURCE_DETAIL(name_, content_type_, data_)                         \
	static struct http_resource_detail_static name_ = {                                     \
		.common = {                                                                      \
			.type = HTTP_RESOURCE_TYPE_STATIC,                                        \
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),                       \
			.content_encoding = "gzip",                                             \
			.content_type = content_type_,                                           \
		},                                                                               \
		.static_data = data_,                                                              \
		.static_data_len = sizeof(data_),                                                  \
	}

LINKR_DEBUGGER_WEB_RESOURCE_DETAIL(linkr_debugger_web_root_detail, "text/html",
				   linkr_debugger_web_index_html_gz);
LINKR_DEBUGGER_WEB_RESOURCE_DETAIL(linkr_debugger_web_index_detail, "text/html",
				   linkr_debugger_web_index_html_gz);
LINKR_DEBUGGER_WEB_RESOURCE_DETAIL(linkr_debugger_web_css_detail, "text/css",
				   linkr_debugger_web_app_css_gz);
LINKR_DEBUGGER_WEB_RESOURCE_DETAIL(linkr_debugger_web_js_detail, "text/javascript",
				   linkr_debugger_web_app_js_gz);
LINKR_DEBUGGER_WEB_RESOURCE_DETAIL(linkr_debugger_web_logic_decoder_js_detail, "text/javascript",
				   linkr_debugger_web_logic_decoder_js_gz);
LINKR_DEBUGGER_WEB_RESOURCE_DETAIL(linkr_debugger_web_logic_decoder_wasm_detail,
				   "application/wasm", linkr_debugger_web_logic_decoder_wasm_gz);

HTTP_RESOURCE_DEFINE(linkr_debugger_web_root_resource, linkr_debugger_http_service, "/",
		     &linkr_debugger_web_root_detail);
HTTP_RESOURCE_DEFINE(linkr_debugger_web_index_resource, linkr_debugger_http_service,
		     "/index.html", &linkr_debugger_web_index_detail);
HTTP_RESOURCE_DEFINE(linkr_debugger_web_css_resource, linkr_debugger_http_service,
		     "/assets/app.css", &linkr_debugger_web_css_detail);
HTTP_RESOURCE_DEFINE(linkr_debugger_web_js_resource, linkr_debugger_http_service,
		     "/assets/app.js", &linkr_debugger_web_js_detail);
HTTP_RESOURCE_DEFINE(linkr_debugger_web_logic_decoder_js_resource, linkr_debugger_http_service,
		     "/assets/decoder/logic-decoder.js", &linkr_debugger_web_logic_decoder_js_detail);
HTTP_RESOURCE_DEFINE(linkr_debugger_web_logic_decoder_wasm_resource, linkr_debugger_http_service,
		     "/assets/decoder/logic-decoder_bg.wasm",
		     &linkr_debugger_web_logic_decoder_wasm_detail);

static struct http_resource_detail_dynamic fallback_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_HEAD) | BIT(HTTP_PUT) |
			BIT(HTTP_POST) | BIT(HTTP_DELETE),
		.content_type = "application/json",
	},
	.cb = linkr_debugger_http_route_request,
};

#define LINKR_DEBUGGER_HTTP_HOLDER_STALE_MS 5000U

void linkr_debugger_http_reap_stale_holders(void)
{
	static int64_t holder_since_ms;
	struct http_client_ctx *holder = fallback_resource_detail.holder;
	int64_t now = k_uptime_get();

	if (holder == NULL) {
		holder_since_ms = 0;
		return;
	}
	if (holder_since_ms == 0) {
		holder_since_ms = now;
		return;
	}
	if (now - holder_since_ms < (int64_t)LINKR_DEBUGGER_HTTP_HOLDER_STALE_MS) {
		return;
	}

	/* A dynamic-resource holder should only live for one request
	 * transaction (milliseconds). When it persists, the owning connection
	 * died without the HTTP server noticing, and every fallback API
	 * request gets a bare 409 until reboot.
	 *
	 * FIXME: this reaps the symptom; the exact Zephyr-side sequence that
	 * strands the holder (connection dropped mid-transaction without
	 * client_release_resources running) is not root-caused yet and
	 * deserves an upstream fix once reproduced deterministically.
	 */
	LOG_WRN("clearing stuck fallback resource holder: fd=%d server_state=%d method=%d",
		holder->fd, (int)holder->server_state, (int)holder->method);
	fallback_resource_detail.holder = NULL;
	holder_since_ms = 0;
}

#if defined(CONFIG_LINKR_DEBUGGER_OTA)
#define LINKR_DEBUGGER_OTA_RESOURCE(name_, path_, methods_, route_)                              \
	static const enum linkr_debugger_ota_route name_##_route = route_;                         \
	static struct http_resource_detail_dynamic name_##_detail = {                              \
		.common = {                                                                          \
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,                                            \
			.bitmask_of_supported_http_methods = methods_,                                 \
			.content_type = "application/json",                                          \
		},                                                                                   \
		.cb = linkr_debugger_ota_http_handle,                                                \
		.user_data = (void *)&name_##_route,                                                  \
	};                                                                                       \
	HTTP_RESOURCE_DEFINE(name_, linkr_debugger_http_service, path_, &name_##_detail)

LINKR_DEBUGGER_OTA_RESOURCE(linkr_debugger_ota_status_resource, "/api/v1/ota", BIT(HTTP_GET),
			   LINKR_DEBUGGER_OTA_ROUTE_STATUS);
LINKR_DEBUGGER_OTA_RESOURCE(linkr_debugger_ota_upload_resource, "/api/v1/ota/upload",
			   BIT(HTTP_POST), LINKR_DEBUGGER_OTA_ROUTE_UPLOAD);
LINKR_DEBUGGER_OTA_RESOURCE(linkr_debugger_ota_test_resource, "/api/v1/ota/test", BIT(HTTP_POST),
			   LINKR_DEBUGGER_OTA_ROUTE_TEST);
LINKR_DEBUGGER_OTA_RESOURCE(linkr_debugger_ota_confirm_resource, "/api/v1/ota/confirm",
			   BIT(HTTP_POST), LINKR_DEBUGGER_OTA_ROUTE_CONFIRM);
#endif

#define LINKR_DEBUGGER_CONFIG_HTTP_RESOURCE(name_, path_, methods_, route_)                       \
	static const enum linkr_debugger_config_http_route name_##_route = route_;                    \
	static struct http_resource_detail_dynamic name_##_detail = {                                 \
		.common = {                                                                             \
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,                                               \
			.bitmask_of_supported_http_methods = methods_,                                    \
			.content_type = "application/json",                                             \
		},                                                                                      \
		.cb = linkr_debugger_config_http_handle,                                               \
		.user_data = (void *)&name_##_route,                                                  \
	};                                                                                          \
	HTTP_RESOURCE_DEFINE(name_, linkr_debugger_http_service, path_, &name_##_detail)

LINKR_DEBUGGER_CONFIG_HTTP_RESOURCE(linkr_debugger_config_resource, "/api/v1/config",
					    BIT(HTTP_GET) | BIT(HTTP_PUT) | BIT(HTTP_DELETE),
					    LINKR_DEBUGGER_CONFIG_HTTP_ROUTE_CONFIG);

static uint8_t linkr_debugger_ws_resource_buffers[LINKR_DEBUGGER_WS_MAX_CLIENTS][LINKR_DEBUGGER_WS_RECV_BUFFER_SIZE];
static uint8_t linkr_debugger_ws_resource_slots[LINKR_DEBUGGER_WS_MAX_CLIENTS] = { 0, 1, 2, 3 };

#define LINKR_DEBUGGER_WS_RESOURCE_DETAIL(slot_)                                                     \
	static struct http_resource_detail_websocket linkr_debugger_ws_resource_detail_##slot_ = {    \
		.common = {                                                                        \
			.type = HTTP_RESOURCE_TYPE_WEBSOCKET,                                        \
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),                          \
		},                                                                                \
		.cb = linkr_debugger_ws_setup,                                                       \
		.data_buffer = linkr_debugger_ws_resource_buffers[slot_],                            \
		.data_buffer_len = sizeof(linkr_debugger_ws_resource_buffers[slot_]),                \
		.user_data = &linkr_debugger_ws_resource_slots[slot_],                               \
	};                                                                                   \
	HTTP_RESOURCE_DEFINE(linkr_debugger_ws_resource_##slot_, linkr_debugger_http_service,       \
			     "/api/v1/ws/" #slot_, &linkr_debugger_ws_resource_detail_##slot_)

LINKR_DEBUGGER_WS_RESOURCE_DETAIL(0);
LINKR_DEBUGGER_WS_RESOURCE_DETAIL(1);
LINKR_DEBUGGER_WS_RESOURCE_DETAIL(2);
LINKR_DEBUGGER_WS_RESOURCE_DETAIL(3);

static int linkr_debugger_http_handle_live_sessions(struct http_client_ctx *client,
					 enum http_transaction_status status,
					 const struct http_request_ctx *request_ctx,
					 struct http_response_ctx *response_ctx,
					 void *user_data)
{
	static uint8_t json_buf[LINKR_DEBUGGER_HTTP_JSON_BUFSZ];
	struct linkr_debugger_http_env env = {
		.buf = (char *)json_buf,
		.cap = sizeof(json_buf),
	};
	struct linkr_debugger_ws_session_info info = { 0 };
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
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_404_NOT_FOUND,
					     "live-sessions", "not_found", "unknown live session path");
			return 0;
		}
		ret = linkr_debugger_ws_session_create(&info);
		if (ret == -EBUSY) {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_503_SERVICE_UNAVAILABLE,
					     "live-sessions", "no_slots_available",
					     "no websocket session slots available");
			return 0;
		}
		if (ret < 0) {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
					     "live-sessions", "create_failed",
					     "failed to create live websocket session");
			return 0;
		}
		if (linkr_debugger_http_json_begin(&env, "live-sessions", true) < 0 ||
		    linkr_debugger_http_append(&env, ",\"action\":\"create\",\"session_id\":%u,\"ws_url\":",
				     (unsigned int)info.session_id) < 0 ||
		    snprintk(ws_url, sizeof(ws_url), "ws://%s:%u%s",
			     linkr_debugger_http_listener_host(), (unsigned int)linkr_debugger_http_listener_port(),
			     info.ws_path) < 0 ||
		    linkr_debugger_http_json_string(&env, ws_url) < 0 ||
		    linkr_debugger_http_append(&env, ",\"connected\":false}\n") < 0) {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
					     "live-sessions", "response_too_large",
					     "failed to encode live session response");
			return 0;
		}
		linkr_debugger_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_201_CREATED);
		return 0;

	case HTTP_GET:
		if (path == NULL || *(path + 1) == '\0') {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "live-sessions", "missing_session_id",
					     "missing session id in URL");
			return 0;
		}
		session_id = (uint32_t)strtoul(path + 1, NULL, 10);
		ret = linkr_debugger_ws_session_lookup(session_id, &info);
		if (ret < 0) {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_404_NOT_FOUND,
					     "live-sessions", "unknown_session_id",
					     "unknown live session id");
			return 0;
		}
		if (linkr_debugger_http_json_begin(&env, "live-sessions", true) < 0 ||
		    linkr_debugger_http_append(&env, ",\"action\":\"get\",\"session_id\":%u,\"slot\":%u,\"connected\":%s,\"ws_url\":",
				     (unsigned int)info.session_id, (unsigned int)info.slot,
				     info.connected ? "true" : "false") < 0 ||
		    snprintk(ws_url, sizeof(ws_url), "ws://%s:%u%s",
			     linkr_debugger_http_listener_host(), (unsigned int)linkr_debugger_http_listener_port(),
			     info.ws_path) < 0 ||
		    linkr_debugger_http_json_string(&env, ws_url) < 0 ||
		    linkr_debugger_http_append(&env, "}\n") < 0) {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
					     "live-sessions", "response_too_large",
					     "failed to encode live session response");
			return 0;
		}
		linkr_debugger_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;

	case HTTP_DELETE:
		if (path == NULL || *(path + 1) == '\0') {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_400_BAD_REQUEST,
					     "live-sessions", "missing_session_id",
					     "missing session id in URL");
			return 0;
		}
		session_id = (uint32_t)strtoul(path + 1, NULL, 10);
		ret = linkr_debugger_ws_session_delete(session_id);
		if (ret == -ENOENT) {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_404_NOT_FOUND,
					     "live-sessions", "unknown_session_id",
					     "unknown live session id");
			return 0;
		}
		if (ret == -EBUSY) {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_409_CONFLICT,
					     "live-sessions", "session_connected",
					     "session is already connected");
			return 0;
		}
		if (ret < 0) {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
					     "live-sessions", "delete_failed",
					     "failed to delete live session");
			return 0;
		}
		if (linkr_debugger_http_json_begin(&env, "live-sessions", true) < 0 ||
		    linkr_debugger_http_append(&env, ",\"action\":\"delete\",\"session_id\":%u}\n",
				     (unsigned int)session_id) < 0) {
			linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_500_INTERNAL_SERVER_ERROR,
					     "live-sessions", "response_too_large",
					     "failed to encode live session delete response");
			return 0;
		}
		linkr_debugger_http_set_json_response(response_ctx, json_buf, sizeof(json_buf), HTTP_200_OK);
		return 0;

	default:
		linkr_debugger_http_error(response_ctx, json_buf, sizeof(json_buf), HTTP_405_METHOD_NOT_ALLOWED,
				     "live-sessions", "method_not_allowed", "method not allowed");
		return 0;
	}
}

static enum linkr_debugger_captive_method linkr_debugger_http_captive_method(
	enum http_method method)
{
	switch (method) {
	case HTTP_GET:
		return LINKR_DEBUGGER_CAPTIVE_METHOD_GET;
	case HTTP_HEAD:
		return LINKR_DEBUGGER_CAPTIVE_METHOD_HEAD;
	default:
		return LINKR_DEBUGGER_CAPTIVE_METHOD_OTHER;
	}
}

static bool linkr_debugger_http_path_matches(const char *path, const char *prefix,
					     bool allow_children)
{
	size_t prefix_len = strlen(prefix);

	return strncmp(path, prefix, prefix_len) == 0 &&
	       (path[prefix_len] == '\0' || path[prefix_len] == '?' ||
		(allow_children && path[prefix_len] == '/'));
}

static int linkr_debugger_http_route_request(struct http_client_ctx *client,
					 enum http_transaction_status status,
					 const struct http_request_ctx *request_ctx,
					 struct http_response_ctx *response_ctx,
					 void *user_data)
{
	char *path = (char *)client->url_buffer;
	enum linkr_debugger_captive_method captive_method;
	enum linkr_debugger_captive_action captive_action;

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	captive_method = linkr_debugger_http_captive_method(client->method);
	captive_action = linkr_debugger_captive_select_action(captive_method, path);
	if (captive_action == LINKR_DEBUGGER_CAPTIVE_ACTION_CAPPORT_JSON) {
		linkr_debugger_http_set_capport_response(response_ctx, captive_method);
		return 0;
	}

	if (linkr_debugger_http_path_matches(path, "/api/v1/status", false)) {
		return linkr_debugger_http_handle_status(client, status, request_ctx, response_ctx, user_data);
	}

	if (linkr_debugger_http_path_matches(path, "/api/v1/power", true)) {
		return linkr_debugger_http_handle_power(client, status, request_ctx, response_ctx, user_data);
	}

	if (linkr_debugger_http_path_matches(path, "/api/v1/adc/read", false)) {
		return linkr_debugger_http_handle_adc(client, status, request_ctx, response_ctx, user_data);
	}

	if (linkr_debugger_http_path_matches(path, "/api/v1/switch", true)) {
		return linkr_debugger_http_handle_switch(client, status, request_ctx, response_ctx, user_data);
	}

	if (linkr_debugger_http_path_matches(path, "/api/v1/gpio", true)) {
		return linkr_debugger_http_handle_gpio(client, status, request_ctx, response_ctx, user_data);
	}

	if (linkr_debugger_http_path_matches(path, "/api/v1/bootloader", false)) {
		return linkr_debugger_http_handle_bootloader(client, status, request_ctx, response_ctx, user_data);
	}

	if (linkr_debugger_http_path_matches(path, "/api/v1/target-recovery", false)) {
		return linkr_debugger_http_handle_target_recovery(client, status, request_ctx,
							 response_ctx, user_data);
	}

	if (linkr_debugger_http_path_matches(path, "/api/v1/watchdog", false)) {
		return linkr_debugger_http_handle_watchdog(client, status, request_ctx, response_ctx, user_data);
	}

	if (linkr_debugger_http_path_matches(path, "/api/v1/live-sessions", true)) {
		return linkr_debugger_http_handle_live_sessions(client, status, request_ctx, response_ctx, user_data);
	}

	if (captive_action == LINKR_DEBUGGER_CAPTIVE_ACTION_REDIRECT) {
		linkr_debugger_http_set_redirect_response(response_ctx);
		return 0;
	}

	if (captive_action == LINKR_DEBUGGER_CAPTIVE_ACTION_METHOD_NOT_ALLOWED) {
		linkr_debugger_http_set_method_not_allowed_response(response_ctx);
		return 0;
	}

	linkr_debugger_http_error(response_ctx, (uint8_t *)client->buffer, sizeof(client->buffer),
			     HTTP_404_NOT_FOUND, "http", "not_found", "unknown API path");
	return 0;
}

HTTP_SERVICE_DEFINE(linkr_debugger_http_service, LINKR_DEBUGGER_HTTP_HOST, &linkr_debugger_http_port,
		    CONFIG_HTTP_SERVER_MAX_CLIENTS, CONFIG_HTTP_SERVER_MAX_CLIENTS, NULL,
		    &fallback_resource_detail.common, NULL);

int linkr_debugger_http_listener_fd(void)
{
	return linkr_debugger_http_service.fd != NULL ? *linkr_debugger_http_service.fd : -1;
}

uint16_t linkr_debugger_http_listener_port(void)
{
	return linkr_debugger_http_port;
}

const char *linkr_debugger_http_listener_host(void)
{
	return LINKR_DEBUGGER_HTTP_ADDR;
}

void linkr_debugger_http_init(void)
{
	k_mutex_init(&linkr_debugger_http_lock);
	k_work_init_delayable(&linkr_debugger_bootloader_work, linkr_debugger_bootloader_work_handler);
	if (IS_ENABLED(CONFIG_LINKR_DEBUGGER_OTA)) {
		linkr_debugger_ota_init();
	}
}

void linkr_debugger_http_publish_state_change(void)
{
	linkr_debugger_ws_publish_state_change();
}
