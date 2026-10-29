/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_http_body.h"

#include <string.h>

/* The HTTP server serializes callbacks. Keep transaction tombstones for EVERY
 * client, independently of the smaller, shared body pool. A rejected client
 * must not acquire a newly freed body slot for its trailing FINAL fragment. */
#ifndef CONFIG_HTTP_SERVER_MAX_CLIENTS
#define CONFIG_HTTP_SERVER_MAX_CLIENTS 8
#endif

struct body_transaction {
	uintptr_t client_key;
	uint16_t method_id;
	uint16_t route_id;
	int8_t slot;
	bool drained;
};

struct body_slot {
	size_t len;
	bool used;
	uint8_t data[LINKR_DEBUGGER_HTTP_BODY_CAP];
};

static struct body_transaction transactions[CONFIG_HTTP_SERVER_MAX_CLIENTS];
static struct body_slot body_slots[LINKR_DEBUGGER_HTTP_BODY_SLOTS];

static struct body_transaction *find_transaction(uintptr_t key)
{
	for (size_t i = 0U; i < CONFIG_HTTP_SERVER_MAX_CLIENTS; i++) {
		if (transactions[i].client_key == key) {
			return &transactions[i];
		}
	}
	return NULL;
}

static void drain_transaction(struct body_transaction *tx)
{
	if (tx->slot >= 0) {
		body_slots[tx->slot].used = false;
		tx->slot = -1;
	}
	tx->drained = true;
}

bool linkr_debugger_http_body_should_handle(bool body_method,
					   enum linkr_debugger_http_body_event event)
{
	return event == LINKR_DEBUGGER_HTTP_BODY_FINAL ||
	       (body_method && (event == LINKR_DEBUGGER_HTTP_BODY_MORE ||
			       event == LINKR_DEBUGGER_HTTP_BODY_ABORTED ||
			       event == LINKR_DEBUGGER_HTTP_BODY_COMPLETE));
}

void linkr_debugger_http_body_reset_all(void)
{
	memset(transactions, 0, sizeof(transactions));
	memset(body_slots, 0, sizeof(body_slots));
}

void linkr_debugger_http_body_clear(uintptr_t client_key, uint16_t method_id,
				    uint16_t route_id)
{
	struct body_transaction *tx = client_key ? find_transaction(client_key) : NULL;

	if (tx != NULL && tx->method_id == method_id && tx->route_id == route_id) {
		drain_transaction(tx);
	}
}

enum linkr_debugger_http_body_result linkr_debugger_http_body_accumulate(
	uintptr_t client_key, uint16_t method_id, uint16_t route_id,
	enum linkr_debugger_http_body_event event, const void *data, size_t len,
	struct linkr_debugger_http_body_view *view)
{
	struct body_transaction *tx;
	struct body_slot *slot;
	enum linkr_debugger_http_body_result error;

	if (view != NULL) {
		view->data = NULL;
		view->len = 0U;
	}
	if (client_key == 0U) {
		return LINKR_DEBUGGER_HTTP_BODY_BAD_ARG;
	}
	tx = find_transaction(client_key);
	if (event == LINKR_DEBUGGER_HTTP_BODY_ABORTED ||
	    event == LINKR_DEBUGGER_HTTP_BODY_COMPLETE) {
		if (tx != NULL) {
			drain_transaction(tx);
			memset(tx, 0, sizeof(*tx));
		}
		return LINKR_DEBUGGER_HTTP_BODY_CLEARED;
	}
	if (tx == NULL) {
		tx = find_transaction(0U);
		if (tx == NULL) {
			/* Impossible for the configured server client pool. Fail closed. */
			return LINKR_DEBUGGER_HTTP_BODY_MISMATCH;
		}
		*tx = (struct body_transaction){
			.client_key = client_key, .method_id = method_id,
			.route_id = route_id, .slot = -1,
		};
	}
	if (tx->drained) {
		return LINKR_DEBUGGER_HTTP_BODY_WAITING;
	}
	error = LINKR_DEBUGGER_HTTP_BODY_MISMATCH;
	if (tx->method_id != method_id || tx->route_id != route_id) {
		goto reject;
	}
	error = LINKR_DEBUGGER_HTTP_BODY_BAD_ARG;
	if ((event != LINKR_DEBUGGER_HTTP_BODY_MORE && event != LINKR_DEBUGGER_HTTP_BODY_FINAL) ||
	    (len > 0U && data == NULL)) {
		goto reject;
	}
	if (tx->slot < 0) {
		for (size_t i = 0U; i < LINKR_DEBUGGER_HTTP_BODY_SLOTS; i++) {
			if (!body_slots[i].used) {
				tx->slot = (int8_t)i;
				body_slots[i].used = true;
				body_slots[i].len = 0U;
				break;
			}
		}
		if (tx->slot < 0) {
			error = LINKR_DEBUGGER_HTTP_BODY_MISMATCH;
			goto reject;
		}
	}
	slot = &body_slots[tx->slot];
	if (len > LINKR_DEBUGGER_HTTP_BODY_CAP - slot->len) {
		error = LINKR_DEBUGGER_HTTP_BODY_TOO_LARGE;
		goto reject;
	}
	if (len > 0U) {
		memcpy(slot->data + slot->len, data, len);
		slot->len += len;
	}
	if (event == LINKR_DEBUGGER_HTTP_BODY_MORE) {
		return LINKR_DEBUGGER_HTTP_BODY_WAITING;
	}
	/* READY at most once. Keep the view alive until caller clear/end. */
	tx->drained = true;
	if (view != NULL) {
		view->data = slot->data;
		view->len = slot->len;
	}
	return LINKR_DEBUGGER_HTTP_BODY_READY;

reject:
	drain_transaction(tx);
	return error;
}
