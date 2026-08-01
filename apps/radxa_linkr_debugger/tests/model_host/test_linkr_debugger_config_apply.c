#include "../../src/linkr_debugger_config_apply.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE_LOCAL(array) (sizeof(array) / sizeof((array)[0]))
#define TEST_ERR_IO (-5)
#define TEST_ERR_ALREADY (-114)
#define TEST_ERR_REMOTE_IO (-121)

static const uint8_t gpio_ids[] = {
	7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U,
	15U, 16U, 17U, 18U, 19U, 20U, 29U,
};

struct fake_control {
	struct linkr_debugger_config_entry trace[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES * 2U];
	bool applied[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
	uint8_t values[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
	size_t call_count;
	size_t fail_at;
	int failure_errno;
	uint8_t vdd_value;
};

static void append_entry(struct linkr_debugger_config_snapshot *snapshot,
			 uint8_t domain, uint8_t item_id, uint8_t value)
{
	struct linkr_debugger_config_entry *entry;

	assert(snapshot->entry_count < LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	entry = &snapshot->entries[snapshot->entry_count++];
	entry->domain = domain;
	entry->item_id = item_id;
	entry->value = value;
}

static void build_full_ordered(struct linkr_debugger_config_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(gpio_ids); i += 2U) {
		append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, gpio_ids[i], 0U);
	}
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID,
		     LINKR_DEBUGGER_CONFIG_SD_USB_READER);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID,
		     LINKR_DEBUGGER_CONFIG_TF_WP_PROTECTED);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID,
		     LINKR_DEBUGGER_CONFIG_USB_TARGET);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_VDD_5V_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_ON);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_ON);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_OFF);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_20V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_OFF);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID,
		     LINKR_DEBUGGER_CONFIG_VIN_1V8);
	for (size_t i = 1U; i < ARRAY_SIZE_LOCAL(gpio_ids); i += 2U) {
		append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, gpio_ids[i],
			     LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT |
			     (uint8_t)((gpio_ids[i] & 2U) != 0U ?
				LINKR_DEBUGGER_CONFIG_GPIO_LEVEL : 0U));
	}
	assert(snapshot->entry_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
}

static void reverse_snapshot(const struct linkr_debugger_config_snapshot *source,
			     struct linkr_debugger_config_snapshot *reversed)
{
	memset(reversed, 0, sizeof(*reversed));
	reversed->entry_count = source->entry_count;
	for (size_t i = 0U; i < source->entry_count; i++) {
		reversed->entries[i] = source->entries[source->entry_count - i - 1U];
	}
}

static bool entries_equal(const struct linkr_debugger_config_entry *left,
			  const struct linkr_debugger_config_entry *right)
{
	return left->domain == right->domain && left->item_id == right->item_id &&
		left->value == right->value;
}

static void assert_snapshots_equal(const struct linkr_debugger_config_snapshot *actual,
				   const struct linkr_debugger_config_snapshot *expected)
{
	assert(actual->entry_count == expected->entry_count);
	for (size_t i = 0U; i < expected->entry_count; i++) {
		assert(entries_equal(&actual->entries[i], &expected->entries[i]));
	}
}

static size_t catalog_index(const struct linkr_debugger_config_entry *entry)
{
	const struct linkr_debugger_config_item_desc *item =
		linkr_debugger_config_find_item(entry->domain, entry->item_id);

	assert(item != NULL);
	return (size_t)(item - linkr_debugger_config_items);
}

static void reset_fake(struct fake_control *control)
{
	memset(control, 0, sizeof(*control));
	control->fail_at = (size_t)-1;
	control->failure_errno = TEST_ERR_IO;
	control->vdd_value = 0xa5U;
}

static int fake_setter(void *context, const struct linkr_debugger_config_entry *entry)
{
	struct fake_control *control = context;
	size_t call_index = control->call_count;
	size_t item_index;

	assert(call_index < ARRAY_SIZE_LOCAL(control->trace));
	control->trace[control->call_count++] = *entry;
	if (call_index == control->fail_at) {
		return control->failure_errno;
	}

	item_index = catalog_index(entry);
	control->applied[item_index] = true;
	control->values[item_index] = entry->value;
	if (entry->domain == LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH &&
	    entry->item_id == LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID) {
		control->vdd_value = entry->value == LINKR_DEBUGGER_CONFIG_USB_PC ?
			LINKR_DEBUGGER_CONFIG_POWER_ON : LINKR_DEBUGGER_CONFIG_POWER_OFF;
	}
	if (entry->domain == LINKR_DEBUGGER_CONFIG_DOMAIN_POWER &&
	    entry->item_id == LINKR_DEBUGGER_CONFIG_POWER_VDD_5V_ID) {
		control->vdd_value = entry->value;
	}
	return 0;
}

static void seed_status(struct linkr_debugger_config_service_status *status)
{
	memset(status, 0, sizeof(*status));
	status->available = true;
	status->reason = LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY;
	status->snapshot_present = true;
	status->item_count = linkr_debugger_config_item_count;
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		status->items[i].item = &linkr_debugger_config_items[i];
		status->items[i].current_available = (i & 1U) == 0U;
		status->items[i].current_value = (uint8_t)(0x40U + i);
		status->items[i].current_requires_confirmation = (i & 1U) != 0U;
		status->items[i].saved_value = (uint8_t)(0x80U + i);
		status->items[i].saved_requires_confirmation = (i & 1U) == 0U;
		status->items[i].apply_state = LINKR_DEBUGGER_CONFIG_APPLY_NOT_SAVED;
	}
}

static void seed_report(struct linkr_debugger_config_operation_report *report)
{
	memset(report, 0, sizeof(*report));
	report->result = LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED;
	report->confirmation_count = 2U;
	report->confirmation_items[0] = &linkr_debugger_config_items[0];
	report->confirmation_items[1] =
		&linkr_debugger_config_items[linkr_debugger_config_item_count - 1U];
	report->applied_count = LINKR_DEBUGGER_CONFIG_MAX_ENTRIES;
	report->pending_count = LINKR_DEBUGGER_CONFIG_MAX_ENTRIES;
	for (size_t i = 0U; i < LINKR_DEBUGGER_CONFIG_MAX_ENTRIES; i++) {
		report->applied_items[i] = &linkr_debugger_config_items[i];
		report->pending_items[i] =
			&linkr_debugger_config_items[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES - i - 1U];
	}
	report->failed_item = &linkr_debugger_config_items[0];
	report->failed_errno = TEST_ERR_ALREADY;
}

static void assert_confirmation_preserved(
	const struct linkr_debugger_config_operation_report *report)
{
	assert(report->confirmation_count == 2U);
	assert(report->confirmation_items[0] == &linkr_debugger_config_items[0]);
	assert(report->confirmation_items[1] ==
	       &linkr_debugger_config_items[linkr_debugger_config_item_count - 1U]);
}

static void assert_report_tail_is_clear(
	const struct linkr_debugger_config_operation_report *report)
{
	for (size_t i = report->applied_count; i < LINKR_DEBUGGER_CONFIG_MAX_ENTRIES; i++) {
		assert(report->applied_items[i] == NULL);
	}
	for (size_t i = report->pending_count; i < LINKR_DEBUGGER_CONFIG_MAX_ENTRIES; i++) {
		assert(report->pending_items[i] == NULL);
	}
}

static void assert_current_fields_preserved(
	const struct linkr_debugger_config_service_status *actual,
	const struct linkr_debugger_config_service_status *before)
{
	assert(actual->available == before->available);
	assert(actual->reason == before->reason);
	assert(actual->snapshot_present == before->snapshot_present);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		assert(actual->items[i].current_available == before->items[i].current_available);
		assert(actual->items[i].current_value == before->items[i].current_value);
		assert(actual->items[i].current_requires_confirmation ==
		       before->items[i].current_requires_confirmation);
	}
}

static void test_orders_shuffled_23_entries(void)
{
	struct linkr_debugger_config_snapshot expected;
	struct linkr_debugger_config_snapshot shuffled;
	struct linkr_debugger_config_snapshot ordered;
	struct linkr_debugger_config_snapshot alias;

	build_full_ordered(&expected);
	reverse_snapshot(&expected, &shuffled);
	memset(&ordered, 0xa5, sizeof(ordered));
	assert(linkr_debugger_config_apply_order_snapshot(&shuffled, &ordered) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert_snapshots_equal(&ordered, &expected);

	alias = shuffled;
	assert(linkr_debugger_config_apply_order_snapshot(&alias, &alias) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert_snapshots_equal(&alias, &expected);
}

static void assert_order_error(const struct linkr_debugger_config_snapshot *snapshot,
			       enum linkr_debugger_config_service_result expected)
{
	const struct linkr_debugger_config_snapshot empty = {0};
	struct linkr_debugger_config_snapshot ordered;

	memset(&ordered, 0xa5, sizeof(ordered));
	assert(linkr_debugger_config_apply_order_snapshot(snapshot, &ordered) == expected);
	assert(memcmp(&ordered, &empty, sizeof(empty)) == 0);
}

static void test_order_rejects_malformed_without_partial_output(void)
{
	struct linkr_debugger_config_snapshot snapshot = {0};
	struct linkr_debugger_config_snapshot ordered;

	memset(&ordered, 0xa5, sizeof(ordered));
	assert(linkr_debugger_config_apply_order_snapshot(NULL, &ordered) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert(ordered.entry_count == 0U);
	assert(linkr_debugger_config_apply_order_snapshot(&snapshot, NULL) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert_order_error(&snapshot, LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION);

	snapshot.entry_count = LINKR_DEBUGGER_CONFIG_MAX_ENTRIES + 1U;
	assert_order_error(&snapshot, LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT);

	memset(&snapshot, 0, sizeof(snapshot));
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 21U, 0U);
	assert_order_error(&snapshot, LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM);

	memset(&snapshot, 0, sizeof(snapshot));
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_OFF);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID, LINKR_DEBUGGER_CONFIG_SD_TARGET);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_ON);
	assert_order_error(&snapshot, LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM);

	memset(&snapshot, 0, sizeof(snapshot));
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID, 2U);
	assert_order_error(&snapshot, LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT);

	memset(&snapshot, 0, sizeof(snapshot));
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 7U,
		     LINKR_DEBUGGER_CONFIG_GPIO_LEVEL);
	assert(linkr_debugger_config_apply_order_snapshot(&snapshot, &ordered) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert_snapshots_equal(&ordered, &snapshot);
}

static void test_full_apply_replays_every_entry_and_usb_vdd_order(void)
{
	struct linkr_debugger_config_snapshot expected;
	struct linkr_debugger_config_snapshot shuffled;
	struct linkr_debugger_config_service_status status;
	struct linkr_debugger_config_service_status before;
	struct linkr_debugger_config_operation_report report;
	struct fake_control control;

	build_full_ordered(&expected);
	reverse_snapshot(&expected, &shuffled);
	seed_status(&status);
	before = status;
	seed_report(&report);
	reset_fake(&control);
	assert(linkr_debugger_config_apply_execute(
		       &shuffled, LINKR_DEBUGGER_CONFIG_APPLY_MODE_CONFIRMED_FULL,
		       fake_setter, &control, &status, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(control.call_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	for (size_t i = 0U; i < expected.entry_count; i++) {
		assert(entries_equal(&control.trace[i], &expected.entries[i]));
		assert(report.applied_items[i] ==
		       linkr_debugger_config_find_item(expected.entries[i].domain,
					       expected.entries[i].item_id));
	}
	assert(control.vdd_value == LINKR_DEBUGGER_CONFIG_POWER_ON);
	assert(status.item_count == linkr_debugger_config_item_count);
	assert(status.saved_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(status.applied_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(status.pending_count == 0U);
	assert(status.failed_count == 0U);
	assert(status.failed_item == NULL);
	assert(status.failed_errno == 0);
	assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(report.applied_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(report.pending_count == 0U);
	assert(report.failed_item == NULL);
	assert(report.failed_errno == 0);
	assert_confirmation_preserved(&report);
	assert_report_tail_is_clear(&report);
	assert_current_fields_preserved(&status, &before);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		assert(status.items[i].saved);
		assert(status.items[i].apply_state == LINKR_DEBUGGER_CONFIG_APPLY_APPLIED);
	}
}

static void test_usb_without_explicit_vdd_has_no_synthetic_entry(void)
{
	struct linkr_debugger_config_snapshot snapshot = {0};
	struct linkr_debugger_config_snapshot ordered;
	struct linkr_debugger_config_service_status status;
	struct linkr_debugger_config_operation_report report;
	struct fake_control control;

	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID,
		     LINKR_DEBUGGER_CONFIG_USB_TARGET);
	assert(linkr_debugger_config_apply_order_snapshot(&snapshot, &ordered) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(ordered.entry_count == 1U);
	assert(ordered.entries[0].domain == LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH);
	assert(ordered.entries[0].item_id == LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID);

	seed_status(&status);
	seed_report(&report);
	reset_fake(&control);
	assert(linkr_debugger_config_apply_execute(
		       &snapshot, LINKR_DEBUGGER_CONFIG_APPLY_MODE_CONFIRMED_FULL,
		       fake_setter, &control, &status, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(control.call_count == 1U);
	assert(control.vdd_value == LINKR_DEBUGGER_CONFIG_POWER_OFF);
	assert(status.saved_count == 1U);
	assert(status.applied_count == 1U);
}

static void build_risk_partition(
	const struct linkr_debugger_config_snapshot *ordered,
	struct linkr_debugger_config_snapshot *safe,
	struct linkr_debugger_config_snapshot *dangerous)
{
	memset(safe, 0, sizeof(*safe));
	memset(dangerous, 0, sizeof(*dangerous));
	for (size_t i = 0U; i < ordered->entry_count; i++) {
		bool requires_confirmation;

		assert(linkr_debugger_config_classify_entry(
			       &ordered->entries[i], &requires_confirmation) ==
		       LINKR_DEBUGGER_CONFIG_CODEC_OK);
		if (requires_confirmation) {
			append_entry(dangerous, ordered->entries[i].domain,
				     ordered->entries[i].item_id, ordered->entries[i].value);
		} else {
			append_entry(safe, ordered->entries[i].domain,
				     ordered->entries[i].item_id, ordered->entries[i].value);
		}
	}
}

static void test_boot_applies_only_safe_entries_and_continues_after_skips(void)
{
	struct linkr_debugger_config_snapshot expected;
	struct linkr_debugger_config_snapshot shuffled;
	struct linkr_debugger_config_snapshot safe;
	struct linkr_debugger_config_snapshot dangerous;
	struct linkr_debugger_config_service_status status;
	struct linkr_debugger_config_service_status before;
	struct linkr_debugger_config_operation_report report;
	struct fake_control control;

	build_full_ordered(&expected);
	reverse_snapshot(&expected, &shuffled);
	build_risk_partition(&expected, &safe, &dangerous);
	assert(safe.entry_count == 12U);
	assert(dangerous.entry_count == 11U);
	seed_status(&status);
	before = status;
	seed_report(&report);
	reset_fake(&control);
	assert(linkr_debugger_config_apply_execute(
		       &shuffled, LINKR_DEBUGGER_CONFIG_APPLY_MODE_BOOT_SAFE,
		       fake_setter, &control, &status, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(control.call_count == safe.entry_count);
	for (size_t i = 0U; i < safe.entry_count; i++) {
		assert(entries_equal(&control.trace[i], &safe.entries[i]));
		assert(report.applied_items[i] ==
		       linkr_debugger_config_find_item(safe.entries[i].domain,
					       safe.entries[i].item_id));
	}
	for (size_t i = 0U; i < dangerous.entry_count; i++) {
		assert(report.pending_items[i] ==
		       linkr_debugger_config_find_item(dangerous.entries[i].domain,
					       dangerous.entries[i].item_id));
	}
	assert(status.saved_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(status.applied_count == safe.entry_count);
	assert(status.pending_count == dangerous.entry_count);
	assert(status.failed_count == 0U);
	assert(report.applied_count == safe.entry_count);
	assert(report.pending_count == dangerous.entry_count);
	assert_confirmation_preserved(&report);
	assert_report_tail_is_clear(&report);
	assert_current_fields_preserved(&status, &before);
	for (size_t i = 0U; i < expected.entry_count; i++) {
		bool requires_confirmation;
		size_t item_index = catalog_index(&expected.entries[i]);

		assert(linkr_debugger_config_classify_entry(
			       &expected.entries[i], &requires_confirmation) ==
		       LINKR_DEBUGGER_CONFIG_CODEC_OK);
		assert(status.items[item_index].saved_requires_confirmation ==
		       requires_confirmation);
		assert(status.items[item_index].apply_state ==
		       (requires_confirmation ? LINKR_DEBUGGER_CONFIG_APPLY_PENDING :
					LINKR_DEBUGGER_CONFIG_APPLY_APPLIED));
	}
}

static void test_full_failure_at_every_position(void)
{
	struct linkr_debugger_config_snapshot expected;
	struct linkr_debugger_config_snapshot shuffled;

	build_full_ordered(&expected);
	reverse_snapshot(&expected, &shuffled);
	for (size_t failure = 0U; failure < expected.entry_count; failure++) {
		struct linkr_debugger_config_service_status status;
		struct linkr_debugger_config_operation_report report;
		struct fake_control control;

		seed_status(&status);
		seed_report(&report);
		reset_fake(&control);
		control.fail_at = failure;
		control.failure_errno = TEST_ERR_IO - (int)failure;
		assert(linkr_debugger_config_apply_execute(
			       &shuffled, LINKR_DEBUGGER_CONFIG_APPLY_MODE_CONFIRMED_FULL,
			       fake_setter, &control, &status, &report) ==
		       LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED);
		assert(control.call_count == failure + 1U);
		for (size_t i = 0U; i <= failure; i++) {
			assert(entries_equal(&control.trace[i], &expected.entries[i]));
		}
		assert(status.saved_count == expected.entry_count);
		assert(status.applied_count == failure);
		assert(status.failed_count == 1U);
		assert(status.pending_count == expected.entry_count - failure - 1U);
		assert(status.failed_item ==
		       linkr_debugger_config_find_item(expected.entries[failure].domain,
					       expected.entries[failure].item_id));
		assert(status.failed_errno == control.failure_errno);
		assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED);
		assert(report.applied_count == failure);
		assert(report.pending_count == expected.entry_count - failure);
		assert(report.failed_item == status.failed_item);
		assert(report.failed_errno == control.failure_errno);
		for (size_t i = 0U; i < failure; i++) {
			size_t item_index = catalog_index(&expected.entries[i]);

			assert(control.applied[item_index]);
			assert(status.items[item_index].apply_state ==
			       LINKR_DEBUGGER_CONFIG_APPLY_APPLIED);
			assert(report.applied_items[i] == status.items[item_index].item);
		}
		assert(!control.applied[catalog_index(&expected.entries[failure])]);
		assert(status.items[catalog_index(&expected.entries[failure])].apply_state ==
		       LINKR_DEBUGGER_CONFIG_APPLY_FAILED);
		assert(report.pending_items[0] == status.failed_item);
		for (size_t i = failure + 1U; i < expected.entry_count; i++) {
			size_t item_index = catalog_index(&expected.entries[i]);

			assert(!control.applied[item_index]);
			assert(status.items[item_index].apply_state ==
			       LINKR_DEBUGGER_CONFIG_APPLY_PENDING);
			assert(report.pending_items[i - failure] ==
			       status.items[item_index].item);
		}
		assert_confirmation_preserved(&report);
		assert_report_tail_is_clear(&report);
	}
}

static void test_boot_failure_stops_after_skipped_dangerous_entries(void)
{
	struct linkr_debugger_config_snapshot expected;
	struct linkr_debugger_config_snapshot shuffled;
	struct linkr_debugger_config_snapshot safe;
	struct linkr_debugger_config_snapshot dangerous;
	struct linkr_debugger_config_service_status status;
	struct linkr_debugger_config_operation_report report;
	struct fake_control control;
	size_t pending_index = 0U;

	build_full_ordered(&expected);
	reverse_snapshot(&expected, &shuffled);
	build_risk_partition(&expected, &safe, &dangerous);
	seed_status(&status);
	seed_report(&report);
	reset_fake(&control);
	control.fail_at = 10U;
	control.failure_errno = TEST_ERR_REMOTE_IO;
	assert(linkr_debugger_config_apply_execute(
		       &shuffled, LINKR_DEBUGGER_CONFIG_APPLY_MODE_BOOT_SAFE,
		       fake_setter, &control, &status, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED);
	assert(control.call_count == 11U);
	for (size_t i = 0U; i < control.call_count; i++) {
		assert(entries_equal(&control.trace[i], &safe.entries[i]));
	}
	assert(status.applied_count == 10U);
	assert(status.failed_count == 1U);
	assert(status.pending_count == 12U);
	assert(status.failed_item ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
				       LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID));
	assert(report.applied_count == 10U);
	assert(report.pending_count == 13U);
	for (size_t i = 0U; i < expected.entry_count; i++) {
		size_t item_index = catalog_index(&expected.entries[i]);

		if (status.items[item_index].apply_state != LINKR_DEBUGGER_CONFIG_APPLY_APPLIED) {
			assert(report.pending_items[pending_index++] == status.items[item_index].item);
		}
	}
	assert(pending_index == report.pending_count);
	assert_confirmation_preserved(&report);
	assert_report_tail_is_clear(&report);
}

static void test_later_full_success_clears_prior_failure(void)
{
	struct linkr_debugger_config_snapshot expected;
	struct linkr_debugger_config_snapshot shuffled;
	struct linkr_debugger_config_service_status status;
	struct linkr_debugger_config_operation_report report;
	struct fake_control control;

	build_full_ordered(&expected);
	reverse_snapshot(&expected, &shuffled);
	seed_status(&status);
	seed_report(&report);
	reset_fake(&control);
	control.fail_at = 7U;
	assert(linkr_debugger_config_apply_execute(
		       &shuffled, LINKR_DEBUGGER_CONFIG_APPLY_MODE_CONFIRMED_FULL,
		       fake_setter, &control, &status, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED);
	assert(status.failed_count == 1U);

	reset_fake(&control);
	assert(linkr_debugger_config_apply_execute(
		       &shuffled, LINKR_DEBUGGER_CONFIG_APPLY_MODE_CONFIRMED_FULL,
		       fake_setter, &control, &status, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(control.call_count == expected.entry_count);
	assert(status.applied_count == expected.entry_count);
	assert(status.pending_count == 0U);
	assert(status.failed_count == 0U);
	assert(status.failed_item == NULL);
	assert(status.failed_errno == 0);
	assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(report.failed_item == NULL);
	assert(report.failed_errno == 0);
	assert_confirmation_preserved(&report);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		assert(status.items[i].apply_state == LINKR_DEBUGGER_CONFIG_APPLY_APPLIED);
	}
}

static void test_status_merge_preserves_non_saved_entries(void)
{
	struct linkr_debugger_config_snapshot snapshot = {0};
	struct linkr_debugger_config_service_status status;
	struct linkr_debugger_config_service_status before;
	struct linkr_debugger_config_operation_report report;
	struct fake_control control;
	bool selected[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES] = {false};

	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID, LINKR_DEBUGGER_CONFIG_SD_TARGET);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_OFF);
	seed_status(&status);
	before = status;
	seed_report(&report);
	reset_fake(&control);
	assert(linkr_debugger_config_apply_execute(
		       &snapshot, LINKR_DEBUGGER_CONFIG_APPLY_MODE_CONFIRMED_FULL,
		       fake_setter, &control, &status, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	for (size_t i = 0U; i < snapshot.entry_count; i++) {
		selected[catalog_index(&snapshot.entries[i])] = true;
	}
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		if (!selected[i]) {
			assert(memcmp(&status.items[i], &before.items[i],
				      sizeof(status.items[i])) == 0);
		} else {
			assert(status.items[i].item == &linkr_debugger_config_items[i]);
			assert(status.items[i].saved);
			assert(status.items[i].apply_state == LINKR_DEBUGGER_CONFIG_APPLY_APPLIED);
		}
	}
	assert(status.item_count == linkr_debugger_config_item_count);
	assert(status.saved_count == snapshot.entry_count);
	assert(status.applied_count == snapshot.entry_count);
	assert_current_fields_preserved(&status, &before);
}

static void test_execute_rejects_null_and_malformed_inputs(void)
{
	struct linkr_debugger_config_snapshot valid = {0};
	struct linkr_debugger_config_snapshot malformed = {0};
	struct linkr_debugger_config_service_status status;
	struct linkr_debugger_config_service_status before;
	struct linkr_debugger_config_operation_report report;
	struct fake_control control;

	append_entry(&valid, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_OFF);
	append_entry(&malformed, LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 21U, 0U);
	seed_status(&status);
	before = status;
	seed_report(&report);
	reset_fake(&control);
	assert(linkr_debugger_config_apply_execute(
		       &malformed, LINKR_DEBUGGER_CONFIG_APPLY_MODE_CONFIRMED_FULL,
		       fake_setter, &control, &status, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM);
	assert(control.call_count == 0U);
	assert(memcmp(&status, &before, sizeof(status)) == 0);
	assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM);
	assert(report.applied_count == 0U);
	assert(report.pending_count == 0U);
	assert(report.failed_item == NULL);
	assert(report.failed_errno == 0);
	assert_confirmation_preserved(&report);
	assert_report_tail_is_clear(&report);

	seed_report(&report);
	assert(linkr_debugger_config_apply_execute(
		       NULL, LINKR_DEBUGGER_CONFIG_APPLY_MODE_CONFIRMED_FULL,
		       fake_setter, &control, &status, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert_confirmation_preserved(&report);
	assert(linkr_debugger_config_apply_execute(
		       &valid, (enum linkr_debugger_config_apply_mode)99,
		       fake_setter, &control, &status, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert(linkr_debugger_config_apply_execute(
		       &valid, LINKR_DEBUGGER_CONFIG_APPLY_MODE_CONFIRMED_FULL,
		       NULL, &control, &status, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert(linkr_debugger_config_apply_execute(
		       &valid, LINKR_DEBUGGER_CONFIG_APPLY_MODE_CONFIRMED_FULL,
		       fake_setter, &control, NULL, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert(linkr_debugger_config_apply_execute(
		       &valid, LINKR_DEBUGGER_CONFIG_APPLY_MODE_CONFIRMED_FULL,
		       fake_setter, &control, &status, NULL) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert(control.call_count == 0U);
}

int main(void)
{
	test_orders_shuffled_23_entries();
	test_order_rejects_malformed_without_partial_output();
	test_full_apply_replays_every_entry_and_usb_vdd_order();
	test_usb_without_explicit_vdd_has_no_synthetic_entry();
	test_boot_applies_only_safe_entries_and_continues_after_skips();
	test_full_failure_at_every_position();
	test_boot_failure_stops_after_skipped_dangerous_entries();
	test_later_full_success_clears_prior_failure();
	test_status_merge_preserves_non_saved_entries();
	test_execute_rejects_null_and_malformed_inputs();
	printf("linkr_debugger_config_apply: order=23 boot=12/11 failures=23/23 passed\n");
	return 0;
}
