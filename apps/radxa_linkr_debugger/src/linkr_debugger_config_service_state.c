/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_config_service_state.h"

#include <string.h>

bool linkr_debugger_config_service_state_ops_valid(
	const struct linkr_debugger_config_service_ops *ops)
{
	return ops != NULL && ops->control_snapshot_get != NULL &&
	       ops->control_apply_entry != NULL && ops->store_status_get != NULL &&
	       ops->store_snapshot_get != NULL && ops->store_save != NULL &&
	       ops->store_clear != NULL && ops->capture_try_acquire != NULL &&
	       ops->capture_release != NULL && ops->flash_try_acquire != NULL &&
	       ops->flash_release != NULL;
}

bool linkr_debugger_config_service_state_reason_available(
	enum linkr_debugger_config_service_reason reason)
{
	return reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY ||
	       reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT;
}

enum linkr_debugger_config_service_reason
linkr_debugger_config_service_state_reason_from_store(
	enum linkr_debugger_config_store_reason reason)
{
	switch (reason) {
	case LINKR_DEBUGGER_CONFIG_STORE_REASON_BACKEND_UNAVAILABLE:
		return LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE;
	case LINKR_DEBUGGER_CONFIG_STORE_REASON_ABSENT:
		return LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT;
	case LINKR_DEBUGGER_CONFIG_STORE_REASON_READY:
		return LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY;
	case LINKR_DEBUGGER_CONFIG_STORE_REASON_INVALID_SNAPSHOT:
		return LINKR_DEBUGGER_CONFIG_SERVICE_REASON_INVALID_SNAPSHOT;
	case LINKR_DEBUGGER_CONFIG_STORE_REASON_UNSUPPORTED_VERSION:
		return LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNSUPPORTED_VERSION;
	case LINKR_DEBUGGER_CONFIG_STORE_REASON_STORAGE_ERROR:
		return LINKR_DEBUGGER_CONFIG_SERVICE_REASON_STORAGE_ERROR;
	default:
		return LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE;
	}
}

enum linkr_debugger_config_service_result
linkr_debugger_config_service_state_result_from_store(
	enum linkr_debugger_config_store_result result)
{
	switch (result) {
	case LINKR_DEBUGGER_CONFIG_STORE_OK:
		return LINKR_DEBUGGER_CONFIG_SERVICE_OK;
	case LINKR_DEBUGGER_CONFIG_STORE_INVALID_ARGUMENT:
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	case LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE:
		return LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE;
	case LINKR_DEBUGGER_CONFIG_STORE_NO_SNAPSHOT:
		return LINKR_DEBUGGER_CONFIG_SERVICE_NO_SNAPSHOT;
	case LINKR_DEBUGGER_CONFIG_STORE_INVALID_SNAPSHOT:
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT;
	case LINKR_DEBUGGER_CONFIG_STORE_UNSUPPORTED_VERSION:
		return LINKR_DEBUGGER_CONFIG_SERVICE_UNSUPPORTED_VERSION;
	case LINKR_DEBUGGER_CONFIG_STORE_STORAGE_ERROR:
		return LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR;
	default:
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
}

enum linkr_debugger_config_service_result
linkr_debugger_config_service_state_result_from_reason(
	enum linkr_debugger_config_service_reason reason)
{
	switch (reason) {
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY:
		return LINKR_DEBUGGER_CONFIG_SERVICE_OK;
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT:
		return LINKR_DEBUGGER_CONFIG_SERVICE_NO_SNAPSHOT;
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE:
		return LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE;
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_STORAGE_ERROR:
		return LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR;
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_INVALID_SNAPSHOT:
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT;
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNSUPPORTED_VERSION:
		return LINKR_DEBUGGER_CONFIG_SERVICE_UNSUPPORTED_VERSION;
	default:
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
}

void linkr_debugger_config_service_state_set_reason(
	struct linkr_debugger_config_service_status *status,
	enum linkr_debugger_config_service_reason reason)
{
	status->reason = reason;
	status->available =
		linkr_debugger_config_service_state_reason_available(reason);
}

void linkr_debugger_config_service_state_set_store_reason(
	struct linkr_debugger_config_service_status *status,
	enum linkr_debugger_config_store_reason reason)
{
	linkr_debugger_config_service_state_set_reason(
		status, linkr_debugger_config_service_state_reason_from_store(reason));
}

enum linkr_debugger_config_service_result
linkr_debugger_config_service_state_init_result(
	const struct linkr_debugger_config_service_status *status)
{
	return status->available ? LINKR_DEBUGGER_CONFIG_SERVICE_OK :
	       linkr_debugger_config_service_state_result_from_reason(status->reason);
}

void linkr_debugger_config_service_state_seed_items(
	struct linkr_debugger_config_service_status *status)
{
	status->item_count = linkr_debugger_config_item_count <=
				     LINKR_DEBUGGER_CONFIG_MAX_ENTRIES ?
			     linkr_debugger_config_item_count : 0U;
	for (size_t i = 0U; i < status->item_count; i++) {
		status->items[i].item = &linkr_debugger_config_items[i];
	}
}

void linkr_debugger_config_service_state_sync_saved(
	struct linkr_debugger_config_service_status *status,
	const struct linkr_debugger_config_snapshot *snapshot,
	enum linkr_debugger_config_apply_state state)
{
	for (size_t i = 0U; i < status->item_count; i++) {
		status->items[i].saved = false;
		status->items[i].apply_state =
			LINKR_DEBUGGER_CONFIG_APPLY_NOT_SAVED;
	}
	for (size_t i = 0U; i < snapshot->entry_count; i++) {
		const struct linkr_debugger_config_entry *entry = &snapshot->entries[i];
		const struct linkr_debugger_config_item_desc *item =
			linkr_debugger_config_find_item(entry->domain, entry->item_id);
		struct linkr_debugger_config_item_status *item_status;
		bool requires_confirmation = false;

		if (item == NULL) {
			continue;
		}
		(void)linkr_debugger_config_classify_entry(entry, &requires_confirmation);
		item_status = &status->items[item - linkr_debugger_config_items];
		item_status->saved = true;
		item_status->saved_value = entry->value;
		item_status->saved_requires_confirmation = requires_confirmation;
		item_status->apply_state = state;
	}
	status->saved_count = snapshot->entry_count;
	status->applied_count =
		state == LINKR_DEBUGGER_CONFIG_APPLY_APPLIED ? snapshot->entry_count : 0U;
	status->pending_count =
		state == LINKR_DEBUGGER_CONFIG_APPLY_PENDING ? snapshot->entry_count : 0U;
	status->failed_count = 0U;
	status->failed_item = NULL;
	status->failed_errno = 0;
}

void linkr_debugger_config_service_state_merge_current(
	struct linkr_debugger_config_service_status *status,
	const struct linkr_debugger_control_snapshot *control_snapshot)
{
	bool present[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES] = {false};

	if (status == NULL || control_snapshot == NULL ||
	    control_snapshot->item_count != status->item_count ||
	    control_snapshot->item_count > LINKR_DEBUGGER_CONFIG_MAX_ENTRIES) {
		return;
	}
	for (size_t i = 0U; i < control_snapshot->item_count; i++) {
		const struct linkr_debugger_control_item_state *state =
			&control_snapshot->items[i];
		const struct linkr_debugger_config_item_desc *item =
			linkr_debugger_config_find_item(state->domain, state->item_id);
		size_t index;

		if (item == NULL) {
			return;
		}
		index = (size_t)(item - linkr_debugger_config_items);
		if (present[index]) {
			return;
		}
		present[index] = true;
	}
	for (size_t i = 0U; i < control_snapshot->item_count; i++) {
		const struct linkr_debugger_control_item_state *state =
			&control_snapshot->items[i];
		const struct linkr_debugger_config_item_desc *item =
			linkr_debugger_config_find_item(state->domain, state->item_id);
		struct linkr_debugger_config_item_status *item_status =
			&status->items[item - linkr_debugger_config_items];
		struct linkr_debugger_config_entry entry = {
			state->domain, state->item_id, state->value
		};

		item_status->current_available = state->available;
		item_status->current_value = state->value;
		(void)linkr_debugger_config_classify_entry(
			&entry, &item_status->current_requires_confirmation);
	}
}
