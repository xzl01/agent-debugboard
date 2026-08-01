#include "linkr_debugger_config_policy.h"

#include "linkr_debugger_control.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

struct control_state_index {
	bool present[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
	bool available[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
	uint8_t values[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
};

static size_t item_index(const struct linkr_debugger_config_item_desc *item)
{
	return (size_t)(item - linkr_debugger_config_items);
}

static bool entry_precedes(const struct linkr_debugger_config_entry *entry,
			   const struct linkr_debugger_config_entry *other)
{
	return entry->domain < other->domain ||
	       (entry->domain == other->domain && entry->item_id < other->item_id);
}

static bool item_precedes(const struct linkr_debugger_config_item_desc *item,
			  const struct linkr_debugger_config_item_desc *other)
{
	return item->domain < other->domain ||
	       (item->domain == other->domain && item->item_id < other->item_id);
}

enum linkr_debugger_config_service_result linkr_debugger_config_policy_resolve_request(
	const struct linkr_debugger_config_save_request *request,
	struct linkr_debugger_config_resolved_selection *selection)
{
	struct linkr_debugger_config_resolved_selection resolved = {0};
	enum linkr_debugger_config_service_result result =
		LINKR_DEBUGGER_CONFIG_SERVICE_OK;

	if (selection == NULL) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
	if (request == NULL || request->item_count > LINKR_DEBUGGER_CONFIG_MAX_ENTRIES) {
		result = LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	} else if (request->item_count == 0U) {
		result = LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION;
	} else {
		for (size_t i = 0U; i < request->item_count; i++) {
			const struct linkr_debugger_config_item_desc *item;

			if (request->item_ids[i] == NULL) {
				result = LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
				break;
			}
			item = linkr_debugger_config_find_item_by_name(request->item_ids[i]);
			if (item == NULL) {
				result = LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM;
				break;
			}
			for (size_t j = 0U; j < resolved.item_count; j++) {
				if (resolved.items[j] == item) {
					result = LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM;
					break;
				}
			}
			if (result != LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
				break;
			}
			resolved.items[resolved.item_count++] = item;
		}
	}

	if (result != LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		memset(selection, 0, sizeof(*selection));
		return result;
	}
	*selection = resolved;
	return LINKR_DEBUGGER_CONFIG_SERVICE_OK;
}

static enum linkr_debugger_config_service_result index_control_snapshot(
	const struct linkr_debugger_control_snapshot *control_snapshot,
	struct control_state_index *indexed)
{
	if (control_snapshot->item_count != linkr_debugger_config_item_count ||
	    linkr_debugger_config_item_count > LINKR_DEBUGGER_CONFIG_MAX_ENTRIES) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT;
	}

	memset(indexed, 0, sizeof(*indexed));
	for (size_t i = 0U; i < control_snapshot->item_count; i++) {
		const struct linkr_debugger_control_item_state *state =
			&control_snapshot->items[i];
		const struct linkr_debugger_config_item_desc *item =
			linkr_debugger_config_find_item(state->domain, state->item_id);
		size_t index;

		if (item == NULL) {
			return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT;
		}
		index = item_index(item);
		if (indexed->present[index]) {
			return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT;
		}
		indexed->present[index] = true;
		indexed->available[index] = state->available;
		indexed->values[index] = state->value;
	}
	return LINKR_DEBUGGER_CONFIG_SERVICE_OK;
}

enum linkr_debugger_config_service_result
linkr_debugger_config_policy_project_available_snapshot(
	const struct linkr_debugger_control_snapshot *control_snapshot,
	const struct linkr_debugger_config_resolved_selection *selection,
	struct linkr_debugger_config_snapshot *snapshot)
{
	struct linkr_debugger_config_snapshot projected = {0};
	struct control_state_index indexed;
	enum linkr_debugger_config_service_result result;

	if (snapshot == NULL) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
	linkr_debugger_config_snapshot_clear(snapshot);
	if (control_snapshot == NULL || selection == NULL) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
	if (selection->item_count == 0U) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION;
	}
	if (selection->item_count > LINKR_DEBUGGER_CONFIG_MAX_ENTRIES) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
	result = index_control_snapshot(control_snapshot, &indexed);
	if (result != LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		return result;
	}

	for (size_t i = 0U; i < selection->item_count; i++) {
		const struct linkr_debugger_config_item_desc *selected = selection->items[i];
		const struct linkr_debugger_config_item_desc *item;
		struct linkr_debugger_config_entry *entry;
		size_t index;

		if (selected == NULL) {
			return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
		}
		item = linkr_debugger_config_find_item(selected->domain, selected->item_id);
		if (item == NULL) {
			return LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM;
		}
		index = item_index(item);
		if (!indexed.present[index] || !indexed.available[index]) {
			return LINKR_DEBUGGER_CONFIG_SERVICE_ITEM_UNAVAILABLE;
		}
		entry = &projected.entries[projected.entry_count++];
		entry->domain = selected->domain;
		entry->item_id = selected->item_id;
		entry->value = indexed.values[index];
	}

	*snapshot = projected;
	return LINKR_DEBUGGER_CONFIG_SERVICE_OK;
}

enum linkr_debugger_config_service_result
linkr_debugger_config_policy_canonicalize_snapshot(
	struct linkr_debugger_config_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
	if (snapshot->entry_count == 0U) {
		linkr_debugger_config_snapshot_clear(snapshot);
		return LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION;
	}
	if (snapshot->entry_count > LINKR_DEBUGGER_CONFIG_MAX_ENTRIES) {
		linkr_debugger_config_snapshot_clear(snapshot);
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT;
	}

	for (size_t i = 0U; i < snapshot->entry_count; i++) {
		const struct linkr_debugger_config_entry *entry = &snapshot->entries[i];
		bool requires_confirmation;

		if (linkr_debugger_config_find_item(entry->domain, entry->item_id) == NULL) {
			linkr_debugger_config_snapshot_clear(snapshot);
			return LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM;
		}
		if (linkr_debugger_config_classify_entry(entry, &requires_confirmation) !=
		    LINKR_DEBUGGER_CONFIG_CODEC_OK) {
			linkr_debugger_config_snapshot_clear(snapshot);
			return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT;
		}
		for (size_t j = 0U; j < i; j++) {
			if (snapshot->entries[j].domain == entry->domain &&
			    snapshot->entries[j].item_id == entry->item_id) {
				linkr_debugger_config_snapshot_clear(snapshot);
				return LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM;
			}
		}
	}

	for (size_t i = 1U; i < snapshot->entry_count; i++) {
		struct linkr_debugger_config_entry key = snapshot->entries[i];
		size_t j = i;

		while (j > 0U && entry_precedes(&key, &snapshot->entries[j - 1U])) {
			snapshot->entries[j] = snapshot->entries[j - 1U];
			j--;
		}
		snapshot->entries[j] = key;
	}
	return LINKR_DEBUGGER_CONFIG_SERVICE_OK;
}

enum linkr_debugger_config_service_result
linkr_debugger_config_policy_populate_confirmation_report(
	const struct linkr_debugger_config_snapshot *snapshot, bool confirmed,
	struct linkr_debugger_config_operation_report *report)
{
	const struct linkr_debugger_config_item_desc
		*dangerous[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
	size_t dangerous_count = 0U;

	if (report == NULL) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
	report->confirmation_count = 0U;
	memset(report->confirmation_items, 0, sizeof(report->confirmation_items));
	if (snapshot == NULL) {
		report->result = LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
		return report->result;
	}
	if (snapshot->entry_count > LINKR_DEBUGGER_CONFIG_MAX_ENTRIES) {
		report->result = LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT;
		return report->result;
	}

	for (size_t i = 0U; i < snapshot->entry_count; i++) {
		const struct linkr_debugger_config_entry *entry = &snapshot->entries[i];
		const struct linkr_debugger_config_item_desc *item;
		bool requires_confirmation;

		if (linkr_debugger_config_classify_entry(entry, &requires_confirmation) !=
		    LINKR_DEBUGGER_CONFIG_CODEC_OK) {
			report->result = LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT;
			return report->result;
		}
		if (requires_confirmation) {
			item = linkr_debugger_config_find_item(entry->domain, entry->item_id);
			dangerous[dangerous_count++] = item;
		}
	}

	for (size_t i = 1U; i < dangerous_count; i++) {
		const struct linkr_debugger_config_item_desc *key = dangerous[i];
		size_t j = i;

		while (j > 0U && item_precedes(key, dangerous[j - 1U])) {
			dangerous[j] = dangerous[j - 1U];
			j--;
		}
		dangerous[j] = key;
	}
	for (size_t i = 0U; i < dangerous_count; i++) {
		report->confirmation_items[i] = dangerous[i];
	}
	report->confirmation_count = dangerous_count;
	report->result = dangerous_count > 0U && !confirmed ?
		LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED :
		LINKR_DEBUGGER_CONFIG_SERVICE_OK;
	return report->result;
}
