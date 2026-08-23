/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_http_task_response.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *linkr_debugger_json_schema(void);

bool linkr_debugger_http_task_path_is_supported(const char *path)
{
	return path != NULL && strcmp(path, LINKR_DEBUGGER_TASK_HTTP_PATH) == 0;
}

enum linkr_debugger_task_http_action linkr_debugger_http_task_action_for_method(unsigned int method)
{
	switch (method) {
	case 0U: return LINKR_DEBUGGER_TASK_HTTP_ACTION_CLEAR;
	case 1U: return LINKR_DEBUGGER_TASK_HTTP_ACTION_LIST;
	case 4U: return LINKR_DEBUGGER_TASK_HTTP_ACTION_STORE;
	default: return LINKR_DEBUGGER_TASK_HTTP_ACTION_METHOD_NOT_ALLOWED;
	}
}

struct linkr_debugger_task_http_response {
	char *buf;
	size_t cap;
	size_t len;
};

static int task_response_append(struct linkr_debugger_task_http_response *response,
				const char *fmt, ...)
{
	va_list args;
	int written;

	if (response->len >= response->cap) {
		return -1;
	}
	va_start(args, fmt);
	written = vsnprintf(response->buf + response->len, response->cap - response->len,
				    fmt, args);
	va_end(args);
	if (written < 0 || (size_t)written >= response->cap - response->len) {
		return -1;
	}
	response->len += (size_t)written;
	return 0;
}

static int task_response_json_string(struct linkr_debugger_task_http_response *response,
				     const char *value, size_t value_len)
{
	if (task_response_append(response, "\"") < 0) {
		return -1;
	}
	for (size_t i = 0U; i < value_len; i++) {
		unsigned char ch = (unsigned char)value[i];
		int ret;

		switch (ch) {
		case '"': ret = task_response_append(response, "\\\""); break;
		case '\\': ret = task_response_append(response, "\\\\"); break;
		case '\n': ret = task_response_append(response, "\\n"); break;
		case '\r': ret = task_response_append(response, "\\r"); break;
		case '\t': ret = task_response_append(response, "\\t"); break;
		default:
			if (ch < 0x20U) {
				return -1;
			}
			ret = task_response_append(response, "%c", ch);
			break;
		}
		if (ret < 0) {
			return -1;
		}
	}
	return task_response_append(response, "\"");
}

static int task_response_begin(struct linkr_debugger_task_http_response *response,
			       const char *action)
{
	return task_response_append(response,
		"{\"schema\":\"%s\",\"ok\":true,\"command\":\"task\",\"action\":\"%s\"",
		linkr_debugger_json_schema(), action);
}

int linkr_debugger_http_task_list_response(char *buf, size_t cap,
	const struct linkr_debugger_task_status *status, const char *blob, size_t blob_len)
{
	struct linkr_debugger_task_http_response response = { .buf = buf, .cap = cap };

	if (buf == NULL || status == NULL || blob == NULL || blob_len > LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE) {
		return -1;
	}
	buf[0] = '\0';
	if (task_response_begin(&response, "list") < 0 ||
	    task_response_append(&response, ",\"backend\":{\"available\":%s},\"task_count\":%u,\"tasks\":[",
				 status->backend_available ? "true" : "false",
				 (unsigned int)status->task_count) < 0) {
		goto no_space;
	}
	for (size_t i = 0U; i < status->task_count; i++) {
		if ((i > 0U && task_response_append(&response, ",") < 0) ||
		    task_response_append(&response, "{\"id\":") < 0 ||
		    task_response_json_string(&response, status->tasks[i].id,
					      strlen(status->tasks[i].id)) < 0 ||
		    task_response_append(&response, ",\"name\":") < 0 ||
		    task_response_json_string(&response, status->tasks[i].name,
					      strlen(status->tasks[i].name)) < 0 ||
		    task_response_append(&response, ",\"request_count\":%u}",
					 (unsigned int)status->tasks[i].request_count) < 0) {
			goto no_space;
		}
	}
	if (task_response_append(&response, "],\"blob\":") < 0 ||
	    task_response_json_string(&response, blob, blob_len) < 0 ||
	    task_response_append(&response, "}\n") < 0) {
		goto no_space;
	}
	return (int)response.len;

no_space:
	buf[0] = '\0';
	return -1;
}

int linkr_debugger_http_task_store_response(char *buf, size_t cap)
{
	int written = snprintf(buf, cap,
		"{\"schema\":\"%s\",\"ok\":true,\"command\":\"task\",\"action\":\"store\",\"stored\":true}\n",
		linkr_debugger_json_schema());

	if (written < 0 || (size_t)written >= cap) {
		return -1;
	}
	return written;
}

int linkr_debugger_http_task_clear_response(char *buf, size_t cap)
{
	int written = snprintf(buf, cap,
		"{\"schema\":\"%s\",\"ok\":true,\"command\":\"task\",\"action\":\"clear\",\"cleared\":true}\n",
		linkr_debugger_json_schema());

	if (written < 0 || (size_t)written >= cap) {
		return -1;
	}
	return written;
}
