/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_http_body.h"

#include <stdbool.h>
#include <string.h>

struct linkr_debugger_http_body_slot {
	bool active;
	uintptr_t client_key;
	uint16_t method_id;
	uint16_t route_id;
	size_t len;
	uint8_t data[LINKR_DEBUGGER_HTTP_BODY_CAP];
};

static struct linkr_debugger_http_body_slot body_slots[LINKR_DEBUGGER_HTTP_BODY_SLOTS];

static void linkr_debugger_http_body_view_reset(struct linkr_debugger_http_body_view *view)
{
	if (view != NULL) {
		view->data = NULL;
		view->len = 0U;
	}
}

static bool linkr_debugger_http_body_slot_matches(
	const struct linkr_debugger_http_body_slot *slot, uintptr_t client_key,
	uint16_t method_id, uint16_t route_id)
{
	return slot->active && slot->client_key == client_key &&
	       slot->method_id == method_id && slot->route_id == route_id;
}

static void linkr_debugger_http_body_slot_clear(struct linkr_debugger_http_body_slot *slot)
{
	memset(slot, 0, sizeof(*slot));
}

static struct linkr_debugger_http_body_slot *linkr_debugger_http_body_find_slot(
	uintptr_t client_key, uint16_t method_id, uint16_t route_id)
{
	for (size_t i = 0U; i < LINKR_DEBUGGER_HTTP_BODY_SLOTS; i++) {
		if (linkr_debugger_http_body_slot_matches(&body_slots[i], client_key,
							 method_id, route_id)) {
			return &body_slots[i];
		}
	}

	return NULL;
}

static bool linkr_debugger_http_body_key_in_use(uintptr_t client_key, uint16_t method_id,
					       uint16_t route_id)
{
	for (size_t i = 0U; i < LINKR_DEBUGGER_HTTP_BODY_SLOTS; i++) {
		if (!body_slots[i].active || body_slots[i].client_key != client_key) {
			continue;
		}
		if (body_slots[i].method_id != method_id || body_slots[i].route_id != route_id) {
			linkr_debugger_http_body_slot_clear(&body_slots[i]);
			return true;
		}
	}

	return false;
}

static struct linkr_debugger_http_body_slot *linkr_debugger_http_body_alloc_slot(
	uintptr_t client_key, uint16_t method_id, uint16_t route_id)
{
	if (linkr_debugger_http_body_key_in_use(client_key, method_id, route_id)) {
		return NULL;
	}

	for (size_t i = 0U; i < LINKR_DEBUGGER_HTTP_BODY_SLOTS; i++) {
		if (!body_slots[i].active) {
			body_slots[i].active = true;
			body_slots[i].client_key = client_key;
			body_slots[i].method_id = method_id;
			body_slots[i].route_id = route_id;
			body_slots[i].len = 0U;
			return &body_slots[i];
		}
	}

	return NULL;
}

static enum linkr_debugger_http_body_result linkr_debugger_http_body_append(
	struct linkr_debugger_http_body_slot *slot, const void *data, size_t len)
{
	if (len == 0U) {
		return LINKR_DEBUGGER_HTTP_BODY_WAITING;
	}
	if (data == NULL) {
		return LINKR_DEBUGGER_HTTP_BODY_BAD_ARG;
	}
	if (len > LINKR_DEBUGGER_HTTP_BODY_CAP ||
	    slot->len > LINKR_DEBUGGER_HTTP_BODY_CAP - len) {
		linkr_debugger_http_body_slot_clear(slot);
		return LINKR_DEBUGGER_HTTP_BODY_TOO_LARGE;
	}

	memcpy(&slot->data[slot->len], data, len);
	slot->len += len;
	return LINKR_DEBUGGER_HTTP_BODY_WAITING;
}

bool linkr_debugger_http_body_should_handle(bool body_method,
					   enum linkr_debugger_http_body_event event)
{
	if (event == LINKR_DEBUGGER_HTTP_BODY_FINAL) {
		return true;
	}

	return body_method &&
	       (event == LINKR_DEBUGGER_HTTP_BODY_MORE ||
		event == LINKR_DEBUGGER_HTTP_BODY_ABORTED ||
		event == LINKR_DEBUGGER_HTTP_BODY_COMPLETE);
}

void linkr_debugger_http_body_reset_all(void)
{
	memset(body_slots, 0, sizeof(body_slots));
}

void linkr_debugger_http_body_clear(uintptr_t client_key, uint16_t method_id,
				    uint16_t route_id)
{
	struct linkr_debugger_http_body_slot *slot =
		linkr_debugger_http_body_find_slot(client_key, method_id, route_id);

	if (slot != NULL) {
		linkr_debugger_http_body_slot_clear(slot);
	}
}

enum linkr_debugger_http_body_result linkr_debugger_http_body_accumulate(
	uintptr_t client_key, uint16_t method_id, uint16_t route_id,
	enum linkr_debugger_http_body_event event, const void *data, size_t len,
	struct linkr_debugger_http_body_view *view)
{
	struct linkr_debugger_http_body_slot *slot;
	enum linkr_debugger_http_body_result ret;

	linkr_debugger_http_body_view_reset(view);

	if (client_key == 0U) {
		return LINKR_DEBUGGER_HTTP_BODY_BAD_ARG;
	}

	slot = linkr_debugger_http_body_find_slot(client_key, method_id, route_id);

	if (event == LINKR_DEBUGGER_HTTP_BODY_ABORTED ||
	    event == LINKR_DEBUGGER_HTTP_BODY_COMPLETE) {
		if (slot != NULL) {
			linkr_debugger_http_body_slot_clear(slot);
		}
		return LINKR_DEBUGGER_HTTP_BODY_CLEARED;
	}

	if (event != LINKR_DEBUGGER_HTTP_BODY_MORE && event != LINKR_DEBUGGER_HTTP_BODY_FINAL) {
		return LINKR_DEBUGGER_HTTP_BODY_BAD_ARG;
	}

	if (slot == NULL) {
		slot = linkr_debugger_http_body_alloc_slot(client_key, method_id, route_id);
		if (slot == NULL) {
			return LINKR_DEBUGGER_HTTP_BODY_MISMATCH;
		}
	}

	ret = linkr_debugger_http_body_append(slot, data, len);
	if (ret < 0) {
		return ret;
	}

	if (event == LINKR_DEBUGGER_HTTP_BODY_MORE) {
		return LINKR_DEBUGGER_HTTP_BODY_WAITING;
	}

	if (view != NULL) {
		view->data = slot->data;
		view->len = slot->len;
	}
	return LINKR_DEBUGGER_HTTP_BODY_READY;
}
