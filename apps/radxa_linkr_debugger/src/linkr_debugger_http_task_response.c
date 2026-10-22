/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_http_task_response.h"

#include "linkr_debugger_json_cursor.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *linkr_debugger_json_schema(void);

struct task_response {
	char *buf;
	size_t cap;
	size_t len;
};

static int task_response_append(struct task_response *response, const char *format, ...)
{
	va_list args;
	int written;

	if (response->len >= response->cap) {
		return -1;
	}
	va_start(args, format);
	written = vsnprintf(response->buf + response->len,
			    response->cap - response->len, format, args);
	va_end(args);
	if (written < 0 || (size_t)written >= response->cap - response->len) {
		return -1;
	}
	response->len += (size_t)written;
	return 0;
}

static int task_response_write(void *context, const char *text, size_t len)
{
	return task_response_append(context, "%.*s", (int)len, text);
}

static int task_response_json_string(struct task_response *response,
				     const char *value)
{
	return linkr_debugger_json_append_string(response, task_response_write,
						 value, strlen(value));
}

static void task_hex(const uint8_t *bytes, size_t len, char *hex)
{
	static const char digits[] = "0123456789abcdef";

	for (size_t i = 0U; i < len; i++) {
		hex[2U * i] = digits[bytes[i] >> 4];
		hex[2U * i + 1U] = digits[bytes[i] & 0x0fU];
	}
	hex[2U * len] = '\0';
}

static void task_uuid(const uint8_t id[LINKR_DEBUGGER_TASK_STORE_ID_SIZE],
		      char uuid[37])
{
	char hex[33];

	task_hex(id, LINKR_DEBUGGER_TASK_STORE_ID_SIZE, hex);
	(void)snprintf(uuid, 37U, "%.8s-%.4s-%.4s-%.4s-%.12s",
		hex, hex + 8, hex + 12, hex + 16, hex + 20);
}

static int task_response_identity(struct task_response *response,
				  const struct linkr_debugger_task_status *status)
{
	char uuid[37];
	char catalog[65];

	task_uuid(status->task_store_id, uuid);
	task_hex(status->catalog_fingerprint, sizeof(status->catalog_fingerprint),
		 catalog);
	return task_response_append(response,
		"\"task_store_id\":\"%s\",\"revision\":\"%" PRIu64
		"\",\"catalog_fingerprint\":\"%s\"",
		uuid, status->revision, catalog);
}

static int task_response_storage(struct task_response *response,
				 const struct linkr_debugger_task_status *status)
{
	return task_response_append(response,
		",\"storage\":{\"total_bytes\":%u,\"used_bytes\":%u,"
		"\"free_bytes\":%u,\"task_bytes\":%u,"
		"\"reserve_bytes\":%u,\"measured\":%s,"
		"\"save_allowed\":%s}",
		(unsigned int)status->storage.total_bytes,
		(unsigned int)status->storage.used_bytes,
		(unsigned int)status->storage.free_bytes,
		(unsigned int)status->storage.task_bytes,
		(unsigned int)status->storage.reserve_bytes,
		status->storage.measured ? "true" : "false",
		status->storage.save_allowed ? "true" : "false");
}

static const char *task_response_action(enum linkr_debugger_task_mutation_opcode opcode)
{
	switch (opcode) {
	case LINKR_DEBUGGER_TASK_MUTATION_UPSERT: return "upsert";
	case LINKR_DEBUGGER_TASK_MUTATION_DELETE: return "delete";
	case LINKR_DEBUGGER_TASK_MUTATION_CLEAR: return "clear";
	case LINKR_DEBUGGER_TASK_MUTATION_IMPORT: return "import";
	default: return "unknown";
	}
}

static int task_response_summary(struct task_response *response,
				 const struct linkr_debugger_task_summary *task)
{
	char source_digest[65];
	char ir_digest[65];

	task_hex(task->ir_digest, sizeof(task->ir_digest), ir_digest);
	if (task_response_append(response, "{\"slot\":%u,\"id\":",
				 (unsigned int)task->slot) < 0 ||
	    task_response_json_string(response, task->id) < 0 ||
	    task_response_append(response,
		    ",\"request_count\":%u,\"source_state\":\"%s\","
		    "\"source_bytes\":%u,\"source_digest\":",
		    (unsigned int)task->request_count,
		    task->source_present ? "present" : "absent",
		    (unsigned int)task->source_len) < 0) {
		return -1;
	}
	if (task->source_present) {
		task_hex(task->source_digest, sizeof(task->source_digest), source_digest);
		if (task_response_append(response, "\"%s\"", source_digest) < 0) {
			return -1;
		}
	} else if (task_response_append(response, "null") < 0) {
		return -1;
	}
	return task_response_append(response,
		",\"ir_bytes\":%u,\"task_ir_digest\":\"%s\"}",
		(unsigned int)task->ir_len, ir_digest);
}

int linkr_debugger_http_task_list_response(char *buf, size_t cap,
	const struct linkr_debugger_task_status *status)
{
	struct task_response response = { .buf = buf, .cap = cap };
	char full_digest[65];

	if (buf == NULL || status == NULL || !status->backend_available || cap == 0U) {
		return -1;
	}
	buf[0] = '\0';
	task_hex(status->full_ir_digest, sizeof(status->full_ir_digest), full_digest);
	if (task_response_append(&response,
		    "{\"schema\":\"%s\",\"ok\":true,\"command\":\"tasks\","
		    "\"action\":\"list\",", linkr_debugger_json_schema()) < 0 ||
	    task_response_identity(&response, status) < 0 ||
	    task_response_storage(&response, status) < 0 ||
	    task_response_append(&response,
		    ",\"full_ir_bytes\":%u,\"full_ir_digest\":\"%s\",\"tasks\":[",
		    (unsigned int)status->full_ir_len, full_digest) < 0) {
		goto no_space;
	}
	for (size_t i = 0U; i < status->task_count; i++) {
		if ((i > 0U && task_response_append(&response, ",") < 0) ||
		    task_response_summary(&response, &status->tasks[i]) < 0) {
			goto no_space;
		}
	}
	if (task_response_append(&response, "]}\n") < 0) {
		goto no_space;
	}
	return (int)response.len;

no_space:
	buf[0] = '\0';
	return -1;
}

int linkr_debugger_http_task_mutation_response(char *buf, size_t cap,
	const struct linkr_debugger_task_mutation_outcome *outcome,
	enum linkr_debugger_task_mutation_opcode opcode)
{
	struct task_response response = { .buf = buf, .cap = cap };
	const struct linkr_debugger_task_summary *affected = NULL;

	if (buf == NULL || outcome == NULL || cap == 0U) {
		return -1;
	}
	for (size_t i = 0U; i < outcome->status.task_count; i++) {
		if (outcome->affected_task_present &&
		    outcome->status.tasks[i].slot == outcome->affected_slot) {
			affected = &outcome->status.tasks[i];
			break;
		}
	}
	buf[0] = '\0';
	if (task_response_append(&response,
		    "{\"schema\":\"%s\",\"ok\":true,\"command\":\"tasks\","
		    "\"action\":\"%s\",", linkr_debugger_json_schema(),
		    task_response_action(opcode)) < 0 ||
	    task_response_identity(&response, &outcome->status) < 0 ||
	    task_response_storage(&response, &outcome->status) < 0 ||
	    task_response_append(&response, ",\"task\":") < 0 ||
	    (affected != NULL ? task_response_summary(&response, affected) :
				task_response_append(&response, "null")) < 0 ||
	    task_response_append(&response, "}\n") < 0) {
		buf[0] = '\0';
		return -1;
	}
	return (int)response.len;
}

int linkr_debugger_http_task_error_response(char *buf, size_t cap,
	const char *code, const char *message,
	const struct linkr_debugger_task_status *current)
{
	struct task_response response = { .buf = buf, .cap = cap };

	if (buf == NULL || code == NULL || message == NULL || cap == 0U) {
		return -1;
	}
	buf[0] = '\0';
	if (task_response_append(&response,
		    "{\"schema\":\"%s\",\"ok\":false,\"command\":\"tasks\","
		    "\"error\":{\"code\":\"%s\",\"message\":\"%s\"}",
		    linkr_debugger_json_schema(), code, message) < 0 ||
	    (current != NULL &&
	     (task_response_append(&response, ",") < 0 ||
	      task_response_identity(&response, current) < 0)) ||
	    task_response_append(&response, "}\n") < 0) {
		buf[0] = '\0';
		return -1;
	}
	return (int)response.len;
}
