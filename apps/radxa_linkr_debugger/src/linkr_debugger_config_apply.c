#include "linkr_debugger_config_apply.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

struct indexed_snapshot {
	bool present[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
	struct linkr_debugger_config_entry entries[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
};

static size_t item_index(const struct linkr_debugger_config_item_desc *item)
{
	return (size_t)(item - linkr_debugger_config_items);
}

static enum linkr_debugger_config_service_result index_snapshot(
	const struct linkr_debugger_config_snapshot *snapshot,
	struct indexed_snapshot *indexed)
{
	if (snapshot->entry_count == 0U) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION;
	}
	if (snapshot->entry_count > LINKR_DEBUGGER_CONFIG_MAX_ENTRIES ||
	    linkr_debugger_config_item_count > LINKR_DEBUGGER_CONFIG_MAX_ENTRIES) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT;
	}

	memset(indexed, 0, sizeof(*indexed));
	for (size_t i = 0U; i < snapshot->entry_count; i++) {
		const struct linkr_debugger_config_entry *entry = &snapshot->entries[i];
		const struct linkr_debugger_config_item_desc *item;
		bool requires_confirmation;
		size_t index;

		item = linkr_debugger_config_find_item(entry->domain, entry->item_id);
		if (item == NULL) {
			return LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM;
		}
		if (linkr_debugger_config_classify_entry(entry, &requires_confirmation) !=
		    LINKR_DEBUGGER_CONFIG_CODEC_OK) {
			return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT;
		}
		index = item_index(item);
		if (indexed->present[index]) {
			return LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM;
		}
		indexed->present[index] = true;
		indexed->entries[index] = *entry;
	}
	return LINKR_DEBUGGER_CONFIG_SERVICE_OK;
}

static void append_indexed(const struct indexed_snapshot *indexed,
			   const struct linkr_debugger_config_item_desc *item,
			   struct linkr_debugger_config_snapshot *ordered)
{
	size_t index = item_index(item);

	if (indexed->present[index]) {
		ordered->entries[ordered->entry_count++] = indexed->entries[index];
	}
}

static void append_named(const struct indexed_snapshot *indexed, uint8_t domain,
			 uint8_t item_id,
			 struct linkr_debugger_config_snapshot *ordered)
{
	const struct linkr_debugger_config_item_desc *item =
		linkr_debugger_config_find_item(domain, item_id);

	if (item != NULL) {
		append_indexed(indexed, item, ordered);
	}
}

enum linkr_debugger_config_service_result linkr_debugger_config_apply_order_snapshot(
	const struct linkr_debugger_config_snapshot *snapshot,
	struct linkr_debugger_config_snapshot *ordered_snapshot)
{
	struct linkr_debugger_config_snapshot ordered = {0};
	struct indexed_snapshot indexed;
	enum linkr_debugger_config_service_result result;

	if (ordered_snapshot == NULL) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
	if (snapshot == NULL) {
		*ordered_snapshot = ordered;
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
	result = index_snapshot(snapshot, &indexed);
	if (result != LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		*ordered_snapshot = ordered;
		return result;
	}

	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		if (indexed.present[i] &&
		    linkr_debugger_config_items[i].domain == LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO &&
		    (indexed.entries[i].value & LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT) == 0U) {
			append_indexed(&indexed, &linkr_debugger_config_items[i], &ordered);
		}
	}
	append_named(&indexed, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID, &ordered);
	append_named(&indexed, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID, &ordered);
	append_named(&indexed, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID, &ordered);
	append_named(&indexed, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_VDD_5V_ID, &ordered);
	append_named(&indexed, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID, &ordered);
	append_named(&indexed, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID, &ordered);
	append_named(&indexed, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_20V_OUT_ID, &ordered);
	append_named(&indexed, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID, &ordered);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		if (indexed.present[i] &&
		    linkr_debugger_config_items[i].domain == LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO &&
		    (indexed.entries[i].value & LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT) != 0U) {
			append_indexed(&indexed, &linkr_debugger_config_items[i], &ordered);
		}
	}
	if (ordered.entry_count != snapshot->entry_count) {
		*ordered_snapshot = (struct linkr_debugger_config_snapshot){0};
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT;
	}
	*ordered_snapshot = ordered;
	return LINKR_DEBUGGER_CONFIG_SERVICE_OK;
}

static void reset_apply_report(struct linkr_debugger_config_operation_report *report)
{
	report->result = LINKR_DEBUGGER_CONFIG_SERVICE_OK;
	report->applied_count = 0U;
	memset(report->applied_items, 0, sizeof(report->applied_items));
	report->pending_count = 0U;
	memset(report->pending_items, 0, sizeof(report->pending_items));
	report->failed_item = NULL;
	report->failed_errno = 0;
}

static void prepare_status(const struct linkr_debugger_config_snapshot *ordered,
			   struct linkr_debugger_config_service_status *status)
{
	status->item_count = linkr_debugger_config_item_count;
	status->saved_count = ordered->entry_count;
	status->applied_count = 0U;
	status->pending_count = ordered->entry_count;
	status->failed_count = 0U;
	status->failed_item = NULL;
	status->failed_errno = 0;
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		status->items[i].item = &linkr_debugger_config_items[i];
	}
	for (size_t i = 0U; i < ordered->entry_count; i++) {
		const struct linkr_debugger_config_entry *entry = &ordered->entries[i];
		const struct linkr_debugger_config_item_desc *item =
			linkr_debugger_config_find_item(entry->domain, entry->item_id);
		struct linkr_debugger_config_item_status *item_status;
		bool requires_confirmation;

		(void)linkr_debugger_config_classify_entry(entry, &requires_confirmation);
		item_status = &status->items[item_index(item)];
		item_status->saved = true;
		item_status->saved_value = entry->value;
		item_status->saved_requires_confirmation = requires_confirmation;
		item_status->apply_state = LINKR_DEBUGGER_CONFIG_APPLY_PENDING;
	}
}

static struct linkr_debugger_config_item_status *status_for_entry(
	struct linkr_debugger_config_service_status *status,
	const struct linkr_debugger_config_entry *entry)
{
	const struct linkr_debugger_config_item_desc *item =
		linkr_debugger_config_find_item(entry->domain, entry->item_id);

	return &status->items[item_index(item)];
}

static void populate_pending_report(
	const struct linkr_debugger_config_snapshot *ordered,
	const struct linkr_debugger_config_service_status *status,
	struct linkr_debugger_config_operation_report *report)
{
	for (size_t i = 0U; i < ordered->entry_count; i++) {
		const struct linkr_debugger_config_entry *entry = &ordered->entries[i];
		const struct linkr_debugger_config_item_desc *item =
			linkr_debugger_config_find_item(entry->domain, entry->item_id);

		if (status->items[item_index(item)].apply_state !=
		    LINKR_DEBUGGER_CONFIG_APPLY_APPLIED) {
			report->pending_items[report->pending_count++] = item;
		}
	}
}

enum linkr_debugger_config_service_result linkr_debugger_config_apply_execute(
	const struct linkr_debugger_config_snapshot *snapshot,
	enum linkr_debugger_config_apply_mode mode,
	linkr_debugger_config_entry_setter_fn setter, void *context,
	struct linkr_debugger_config_service_status *status,
	struct linkr_debugger_config_operation_report *report)
{
	struct linkr_debugger_config_snapshot ordered;
	enum linkr_debugger_config_service_result result;

	if (report == NULL) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
	reset_apply_report(report);
	if (snapshot == NULL || setter == NULL || status == NULL ||
	    (mode != LINKR_DEBUGGER_CONFIG_APPLY_MODE_BOOT_SAFE &&
	     mode != LINKR_DEBUGGER_CONFIG_APPLY_MODE_CONFIRMED_FULL)) {
		report->result = LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
		return report->result;
	}
	result = linkr_debugger_config_apply_order_snapshot(snapshot, &ordered);
	if (result != LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		report->result = result;
		return result;
	}

	prepare_status(&ordered, status);
	for (size_t i = 0U; i < ordered.entry_count; i++) {
		const struct linkr_debugger_config_entry *entry = &ordered.entries[i];
		struct linkr_debugger_config_item_status *item_status =
			status_for_entry(status, entry);
		bool requires_confirmation;
		int setter_result;

		(void)linkr_debugger_config_classify_entry(entry, &requires_confirmation);
		if (mode == LINKR_DEBUGGER_CONFIG_APPLY_MODE_BOOT_SAFE &&
		    requires_confirmation) {
			continue;
		}
		setter_result = setter(context, entry);
		if (setter_result != 0) {
			item_status->apply_state = LINKR_DEBUGGER_CONFIG_APPLY_FAILED;
			status->pending_count--;
			status->failed_count = 1U;
			status->failed_item = item_status->item;
			status->failed_errno = setter_result;
			report->failed_item = item_status->item;
			report->failed_errno = setter_result;
			report->result = LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED;
			break;
		}
		item_status->apply_state = LINKR_DEBUGGER_CONFIG_APPLY_APPLIED;
		status->pending_count--;
		status->applied_count++;
		report->applied_items[report->applied_count++] = item_status->item;
	}
	populate_pending_report(&ordered, status, report);
	return report->result;
}
