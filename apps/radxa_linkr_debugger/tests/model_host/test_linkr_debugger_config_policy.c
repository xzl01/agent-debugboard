#include "../../src/linkr_debugger_config_policy.h"

#include "../../src/linkr_debugger_control.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE_LOCAL(array) (sizeof(array) / sizeof((array)[0]))

struct expected_item {
	const char *id;
	uint8_t domain;
	uint8_t item_id;
};

static const struct expected_item expected_catalog[] = {
	{ "power/12v_out", LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
	  LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID },
	{ "power/5v_out", LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
	  LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID },
	{ "power/vdd_5v", LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
	  LINKR_DEBUGGER_CONFIG_POWER_VDD_5V_ID },
	{ "power/20v_out", LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
	  LINKR_DEBUGGER_CONFIG_POWER_20V_OUT_ID },
	{ "switch/sd", LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
	  LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID },
	{ "switch/usb", LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
	  LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID },
	{ "switch/tf_wp", LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
	  LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID },
	{ "switch/vin", LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
	  LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID },
	{ "gpio/GP7", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 7U },
	{ "gpio/GP8", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 8U },
	{ "gpio/GP9", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 9U },
	{ "gpio/GP10", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 10U },
	{ "gpio/GP11", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 11U },
	{ "gpio/GP12", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 12U },
	{ "gpio/GP13", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 13U },
	{ "gpio/GP14", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 14U },
	{ "gpio/GP15", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 15U },
	{ "gpio/GP16", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 16U },
	{ "gpio/GP17", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 17U },
	{ "gpio/GP18", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 18U },
	{ "gpio/GP19", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 19U },
	{ "gpio/GP20", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 20U },
	{ "gpio/GP29", LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 29U },
};

struct value_case {
	uint8_t domain;
	uint8_t item_id;
	uint8_t value;
	bool dangerous;
};

static const struct value_case value_cases[] = {
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_POWER, LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
	  LINKR_DEBUGGER_CONFIG_POWER_OFF, false },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_POWER, LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
	  LINKR_DEBUGGER_CONFIG_POWER_ON, true },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_POWER, LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID,
	  LINKR_DEBUGGER_CONFIG_POWER_ON, true },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_POWER, LINKR_DEBUGGER_CONFIG_POWER_VDD_5V_ID,
	  LINKR_DEBUGGER_CONFIG_POWER_ON, true },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_POWER, LINKR_DEBUGGER_CONFIG_POWER_20V_OUT_ID,
	  LINKR_DEBUGGER_CONFIG_POWER_ON, true },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH, LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID,
	  LINKR_DEBUGGER_CONFIG_SD_TARGET, false },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH, LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID,
	  LINKR_DEBUGGER_CONFIG_SD_USB_READER, false },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH, LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID,
	  LINKR_DEBUGGER_CONFIG_USB_PC, true },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH, LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID,
	  LINKR_DEBUGGER_CONFIG_USB_TARGET, true },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH, LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID,
	  LINKR_DEBUGGER_CONFIG_TF_WP_WRITABLE, false },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH, LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID,
	  LINKR_DEBUGGER_CONFIG_TF_WP_PROTECTED, false },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH, LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID,
	  LINKR_DEBUGGER_CONFIG_VIN_3V3, false },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH, LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID,
	  LINKR_DEBUGGER_CONFIG_VIN_1V8, true },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 7U, 0U, false },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 13U, LINKR_DEBUGGER_CONFIG_GPIO_LEVEL, false },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 29U, LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT, true },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 10U,
	  LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT | LINKR_DEBUGGER_CONFIG_GPIO_LEVEL, true },
};

static void assert_catalog_is_exact(void)
{
	assert(linkr_debugger_config_item_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(linkr_debugger_config_item_count == ARRAY_SIZE_LOCAL(expected_catalog));
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		assert(strcmp(linkr_debugger_config_items[i].id, expected_catalog[i].id) == 0);
		assert(linkr_debugger_config_items[i].domain == expected_catalog[i].domain);
		assert(linkr_debugger_config_items[i].item_id == expected_catalog[i].item_id);
	}
}

static void append_id(struct linkr_debugger_config_save_request *request, const char *id)
{
	assert(request->item_count < LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	request->item_ids[request->item_count++] = id;
}

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

static uint8_t control_value_for(const struct linkr_debugger_config_item_desc *item,
				 size_t index)
{
	if (item->domain == LINKR_DEBUGGER_CONFIG_DOMAIN_POWER) {
		return (index & 1U) != 0U ? LINKR_DEBUGGER_CONFIG_POWER_ON :
			LINKR_DEBUGGER_CONFIG_POWER_OFF;
	}
	if (item->domain == LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH) {
		switch (item->item_id) {
		case LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID:
			return LINKR_DEBUGGER_CONFIG_SD_USB_READER;
		case LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID:
			return LINKR_DEBUGGER_CONFIG_USB_TARGET;
		case LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID:
			return LINKR_DEBUGGER_CONFIG_TF_WP_PROTECTED;
		case LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID:
			return LINKR_DEBUGGER_CONFIG_VIN_3V3;
		default:
			break;
		}
		assert(false);
	}
	if (item->domain == LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO) {
		if ((index & 1U) == 0U) {
			return 0U;
		}
		return LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT |
		       (uint8_t)((item->item_id & 2U) != 0U ?
				 LINKR_DEBUGGER_CONFIG_GPIO_LEVEL : 0U);
	}
	assert(false);
	return 0U;
}

static void build_coherent_control(struct linkr_debugger_control_snapshot *control)
{
	memset(control, 0, sizeof(*control));
	control->item_count = linkr_debugger_config_item_count;
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		control->items[i].domain = linkr_debugger_config_items[i].domain;
		control->items[i].item_id = linkr_debugger_config_items[i].item_id;
		control->items[i].value = control_value_for(&linkr_debugger_config_items[i], i);
		control->items[i].available = true;
	}
}

static void build_full_request(struct linkr_debugger_config_save_request *request)
{
	memset(request, 0, sizeof(*request));
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		append_id(request, linkr_debugger_config_items[i].id);
	}
}

static void resolve_or_die(const struct linkr_debugger_config_save_request *request,
			   struct linkr_debugger_config_resolved_selection *selection)
{
	assert(linkr_debugger_config_policy_resolve_request(request, selection) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
}

static void assert_snapshot_cleared(const struct linkr_debugger_config_snapshot *snapshot)
{
	const struct linkr_debugger_config_snapshot empty = {0};

	assert(memcmp(snapshot, &empty, sizeof(empty)) == 0);
}

static void assert_selection_cleared(
	const struct linkr_debugger_config_resolved_selection *selection)
{
	assert(selection->item_count == 0U);
	for (size_t i = 0U; i < LINKR_DEBUGGER_CONFIG_MAX_ENTRIES; i++) {
		assert(selection->items[i] == NULL);
	}
}

static void seed_selection(struct linkr_debugger_config_resolved_selection *selection)
{
	memset(selection, 0xa5, sizeof(*selection));
}

static void seed_snapshot(struct linkr_debugger_config_snapshot *snapshot)
{
	memset(snapshot, 0xa5, sizeof(*snapshot));
}

static void seed_report(struct linkr_debugger_config_operation_report *report)
{
	memset(report, 0, sizeof(*report));
	report->result = LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED;
	report->applied_count = 3U;
	report->pending_count = 2U;
	for (size_t i = 0U; i < LINKR_DEBUGGER_CONFIG_MAX_ENTRIES; i++) {
		report->confirmation_items[i] = &linkr_debugger_config_items[i];
		report->applied_items[i] = &linkr_debugger_config_items[i];
		report->pending_items[i] = &linkr_debugger_config_items[i];
	}
	report->applied_items[0] = &linkr_debugger_config_items[0];
	report->applied_items[1] = &linkr_debugger_config_items[1];
	report->applied_items[2] = &linkr_debugger_config_items[2];
	report->pending_items[0] = &linkr_debugger_config_items[3];
	report->pending_items[1] = &linkr_debugger_config_items[4];
	report->failed_item = &linkr_debugger_config_items[5];
	report->failed_errno = -5;
}

static void assert_apply_report_fields_preserved(
	const struct linkr_debugger_config_operation_report *report)
{
	assert(report->applied_count == 3U);
	assert(report->applied_items[0] == &linkr_debugger_config_items[0]);
	assert(report->applied_items[1] == &linkr_debugger_config_items[1]);
	assert(report->applied_items[2] == &linkr_debugger_config_items[2]);
	assert(report->pending_count == 2U);
	assert(report->pending_items[0] == &linkr_debugger_config_items[3]);
	assert(report->pending_items[1] == &linkr_debugger_config_items[4]);
	assert(report->failed_item == &linkr_debugger_config_items[5]);
	assert(report->failed_errno == -5);
}

static void assert_confirmation_tail_clear(
	const struct linkr_debugger_config_operation_report *report)
{
	for (size_t i = report->confirmation_count;
	     i < LINKR_DEBUGGER_CONFIG_MAX_ENTRIES; i++) {
		assert(report->confirmation_items[i] == NULL);
	}
}

static void test_resolve_all_23_exact_ids_and_preserves_request_order(void)
{
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_resolved_selection selection;

	assert_catalog_is_exact();
	build_full_request(&request);
	seed_selection(&selection);
	resolve_or_die(&request, &selection);
	assert(selection.item_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		assert(selection.items[i] == &linkr_debugger_config_items[i]);
	}

	memset(&request, 0, sizeof(request));
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		append_id(&request,
			  linkr_debugger_config_items
				  [linkr_debugger_config_item_count - i - 1U].id);
	}
	resolve_or_die(&request, &selection);
	assert(selection.item_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		assert(selection.items[i] ==
		       &linkr_debugger_config_items
			       [linkr_debugger_config_item_count - i - 1U]);
	}

	memset(&request, 0, sizeof(request));
	append_id(&request, "gpio/GP29");
	append_id(&request, "power/5v_out");
	append_id(&request, "switch/vin");
	append_id(&request, "gpio/GP7");
	append_id(&request, "switch/sd");
	resolve_or_die(&request, &selection);
	assert(selection.item_count == 5U);
	assert(selection.items[0] ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 29U));
	assert(selection.items[1] ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
					       LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID));
	assert(selection.items[2] ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
					       LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID));
	assert(selection.items[3] ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 7U));
	assert(selection.items[4] ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
					       LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID));
}

static void assert_resolve_error(const struct linkr_debugger_config_save_request *request,
				 enum linkr_debugger_config_service_result expected)
{
	struct linkr_debugger_config_resolved_selection selection;

	seed_selection(&selection);
	assert(linkr_debugger_config_policy_resolve_request(request, &selection) ==
	       expected);
	assert_selection_cleared(&selection);
}

static void test_resolve_rejects_malformed_requests(void)
{
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_resolved_selection selection;

	build_full_request(&request);
	assert(linkr_debugger_config_policy_resolve_request(NULL, &selection) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert(linkr_debugger_config_policy_resolve_request(&request, NULL) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);

	memset(&request, 0, sizeof(request));
	assert_resolve_error(&request, LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION);

	build_full_request(&request);
	request.item_count = LINKR_DEBUGGER_CONFIG_MAX_ENTRIES + 1U;
	assert_resolve_error(&request, LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);

	memset(&request, 0, sizeof(request));
	append_id(&request, "power/12v_out");
	append_id(&request, NULL);
	assert_resolve_error(&request, LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);

	memset(&request, 0, sizeof(request));
	append_id(&request, "power/9v_out");
	assert_resolve_error(&request, LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM);

	memset(&request, 0, sizeof(request));
	append_id(&request, "12v_out");
	assert_resolve_error(&request, LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM);

	memset(&request, 0, sizeof(request));
	append_id(&request, "switch/sd");
	append_id(&request, "switch/sd");
	assert_resolve_error(&request, LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM);

	memset(&request, 0, sizeof(request));
	append_id(&request, "gpio/GP13");
	append_id(&request, "power/20v_out");
	append_id(&request, "switch/tf_wp");
	append_id(&request, "gpio/GP13");
	assert_resolve_error(&request, LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM);
}

static void test_project_copies_selected_values_in_selection_order(void)
{
	struct linkr_debugger_control_snapshot control;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_resolved_selection selection;
	struct linkr_debugger_config_snapshot projected;

	build_coherent_control(&control);
	memset(&request, 0, sizeof(request));
	append_id(&request, "gpio/GP10");
	append_id(&request, "switch/usb");
	append_id(&request, "power/12v_out");
	append_id(&request, "gpio/GP7");
	resolve_or_die(&request, &selection);
	seed_snapshot(&projected);
	assert(linkr_debugger_config_policy_project_available_snapshot(
		       &control, &selection, &projected) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(projected.entry_count == 4U);
	assert(projected.entries[0].domain == LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO);
	assert(projected.entries[0].item_id == 10U);
	assert(projected.entries[0].value ==
	       (LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT | LINKR_DEBUGGER_CONFIG_GPIO_LEVEL));
	assert(projected.entries[1].domain == LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH);
	assert(projected.entries[1].item_id == LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID);
	assert(projected.entries[1].value == LINKR_DEBUGGER_CONFIG_USB_TARGET);
	assert(projected.entries[2].domain == LINKR_DEBUGGER_CONFIG_DOMAIN_POWER);
	assert(projected.entries[2].item_id == LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID);
	assert(projected.entries[2].value == LINKR_DEBUGGER_CONFIG_POWER_OFF);
	assert(projected.entries[3].domain == LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO);
	assert(projected.entries[3].item_id == 7U);
	assert(projected.entries[3].value == 0U);
}

static void test_project_full_23_items_copies_every_value_exactly(void)
{
	struct linkr_debugger_control_snapshot control;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_resolved_selection selection;
	struct linkr_debugger_config_snapshot projected;

	build_coherent_control(&control);
	build_full_request(&request);
	resolve_or_die(&request, &selection);
	assert(linkr_debugger_config_policy_project_available_snapshot(
		       &control, &selection, &projected) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(projected.entry_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		assert(projected.entries[i].domain == control.items[i].domain);
		assert(projected.entries[i].item_id == control.items[i].item_id);
		assert(projected.entries[i].value == control.items[i].value);
		if (control.items[i].domain == LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO &&
		    (control.items[i].value & LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT) == 0U) {
			assert(projected.entries[i].value == 0U);
		}
	}
}

static void assert_project_error(const struct linkr_debugger_control_snapshot *control,
				 const struct linkr_debugger_config_resolved_selection *selection,
				 enum linkr_debugger_config_service_result expected)
{
	struct linkr_debugger_config_snapshot projected;

	seed_snapshot(&projected);
	assert(linkr_debugger_config_policy_project_available_snapshot(
		       control, selection, &projected) == expected);
	assert_snapshot_cleared(&projected);
}

static void test_project_rejects_incoherent_control_snapshots(void)
{
	struct linkr_debugger_control_snapshot control;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_resolved_selection selection;
	struct linkr_debugger_config_snapshot projected;

	build_coherent_control(&control);
	build_full_request(&request);
	resolve_or_die(&request, &selection);

	assert(linkr_debugger_config_policy_project_available_snapshot(
		       NULL, &selection, &projected) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert(linkr_debugger_config_policy_project_available_snapshot(
		       &control, NULL, &projected) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert(linkr_debugger_config_policy_project_available_snapshot(
		       &control, &selection, NULL) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);

	control.item_count = linkr_debugger_config_item_count - 1U;
	assert_project_error(&control, &selection,
			     LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT);
	control.item_count = linkr_debugger_config_item_count + 1U;
	assert_project_error(&control, &selection,
			     LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT);

	build_coherent_control(&control);
	control.items[1] = control.items[0];
	assert_project_error(&control, &selection,
			     LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT);

	build_coherent_control(&control);
	control.items[5].domain = 9U;
	control.items[5].item_id = 9U;
	assert_project_error(&control, &selection,
			     LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT);

	build_coherent_control(&control);
	memset(&selection, 0, sizeof(selection));
	assert_project_error(&control, &selection,
			     LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION);

	memset(&selection, 0, sizeof(selection));
	selection.item_count = 1U;
	assert_project_error(&control, &selection,
			     LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
}

static void test_project_rejects_unavailable_selected_items(void)
{
	struct linkr_debugger_control_snapshot control;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_resolved_selection selection;
	struct linkr_debugger_config_snapshot projected;

	build_coherent_control(&control);
	memset(&request, 0, sizeof(request));
	append_id(&request, "power/5v_out");
	append_id(&request, "gpio/GP13");
	resolve_or_die(&request, &selection);
	control.items[1].available = false;
	assert_project_error(&control, &selection,
			     LINKR_DEBUGGER_CONFIG_SERVICE_ITEM_UNAVAILABLE);

	build_coherent_control(&control);
	control.items[(size_t)(linkr_debugger_config_find_item(
				     LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 13U) -
			     linkr_debugger_config_items)]
		.available = false;
	assert_project_error(&control, &selection,
			     LINKR_DEBUGGER_CONFIG_SERVICE_ITEM_UNAVAILABLE);

	build_coherent_control(&control);
	control.items[0].available = false;
	control.items[22].available = false;
	seed_snapshot(&projected);
	assert(linkr_debugger_config_policy_project_available_snapshot(
		       &control, &selection, &projected) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(projected.entry_count == 2U);
}

static void assert_canonical_order(const struct linkr_debugger_config_snapshot *snapshot)
{
	assert(snapshot->entry_count <= LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	for (size_t i = 1U; i < snapshot->entry_count; i++) {
		const struct linkr_debugger_config_entry *previous = &snapshot->entries[i - 1U];
		const struct linkr_debugger_config_entry *entry = &snapshot->entries[i];

		assert(entry->domain > previous->domain ||
		       (entry->domain == previous->domain &&
			entry->item_id > previous->item_id));
	}
}

static void test_canonicalize_sorts_shuffled_23_entries_stably(void)
{
	struct linkr_debugger_control_snapshot control;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_resolved_selection selection;
	struct linkr_debugger_config_snapshot projected;

	build_coherent_control(&control);
	memset(&request, 0, sizeof(request));
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		append_id(&request,
			  linkr_debugger_config_items
				  [linkr_debugger_config_item_count - i - 1U].id);
	}
	resolve_or_die(&request, &selection);
	assert(linkr_debugger_config_policy_project_available_snapshot(
		       &control, &selection, &projected) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(linkr_debugger_config_policy_canonicalize_snapshot(&projected) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(projected.entry_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert_canonical_order(&projected);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		assert(projected.entries[i].domain == control.items[i].domain);
		assert(projected.entries[i].item_id == control.items[i].item_id);
		assert(projected.entries[i].value == control.items[i].value);
	}
	assert(linkr_debugger_config_policy_canonicalize_snapshot(&projected) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		assert(projected.entries[i].value == control.items[i].value);
	}
}

static void assert_canonicalize_error(struct linkr_debugger_config_snapshot *snapshot,
				      enum linkr_debugger_config_service_result expected)
{
	assert(linkr_debugger_config_policy_canonicalize_snapshot(snapshot) == expected);
	assert_snapshot_cleared(snapshot);
}

static void test_canonicalize_rejects_malformed_snapshots(void)
{
	struct linkr_debugger_config_snapshot snapshot;

	assert(linkr_debugger_config_policy_canonicalize_snapshot(NULL) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);

	memset(&snapshot, 0, sizeof(snapshot));
	assert_canonicalize_error(&snapshot, LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION);

	seed_snapshot(&snapshot);
	snapshot.entry_count = LINKR_DEBUGGER_CONFIG_MAX_ENTRIES + 1U;
	assert_canonicalize_error(&snapshot,
				  LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT);

	memset(&snapshot, 0, sizeof(snapshot));
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 21U, 0U);
	assert_canonicalize_error(&snapshot, LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM);

	memset(&snapshot, 0, sizeof(snapshot));
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID, 2U);
	assert_canonicalize_error(&snapshot,
				  LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT);

	memset(&snapshot, 0, sizeof(snapshot));
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID, 7U);
	assert_canonicalize_error(&snapshot,
				  LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT);

	memset(&snapshot, 0, sizeof(snapshot));
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 7U, 4U);
	assert_canonicalize_error(&snapshot,
				  LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT);

	memset(&snapshot, 0, sizeof(snapshot));
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_OFF);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID, LINKR_DEBUGGER_CONFIG_SD_TARGET);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_ON);
	assert_canonicalize_error(&snapshot,
				  LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM);
}

static void test_confirmation_report_classifies_every_domain_value(void)
{
	struct linkr_debugger_config_snapshot snapshot;
	struct linkr_debugger_config_operation_report report;

	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(value_cases); i++) {
		bool codec_requires_confirmation;

		memset(&snapshot, 0, sizeof(snapshot));
		append_entry(&snapshot, value_cases[i].domain, value_cases[i].item_id,
			     value_cases[i].value);
		assert(linkr_debugger_config_classify_entry(
			       &snapshot.entries[0], &codec_requires_confirmation) ==
		       LINKR_DEBUGGER_CONFIG_CODEC_OK);
		assert(codec_requires_confirmation == value_cases[i].dangerous);

		seed_report(&report);
		assert(linkr_debugger_config_policy_populate_confirmation_report(
			       &snapshot, false, &report) ==
		       (value_cases[i].dangerous ?
				LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED :
				LINKR_DEBUGGER_CONFIG_SERVICE_OK));
		assert(report.confirmation_count == (value_cases[i].dangerous ? 1U : 0U));
		if (value_cases[i].dangerous) {
			assert(report.confirmation_items[0] ==
			       linkr_debugger_config_find_item(value_cases[i].domain,
							       value_cases[i].item_id));
		}
		assert(report.result ==
		       (value_cases[i].dangerous ?
				LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED :
				LINKR_DEBUGGER_CONFIG_SERVICE_OK));
		assert_confirmation_tail_clear(&report);
		assert_apply_report_fields_preserved(&report);
	}
}

static void test_confirmation_report_lists_multi_danger_in_canonical_order(void)
{
	struct linkr_debugger_config_snapshot snapshot;
	struct linkr_debugger_config_operation_report report;

	memset(&snapshot, 0, sizeof(snapshot));
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 10U,
		     LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID, LINKR_DEBUGGER_CONFIG_VIN_1V8);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_20V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_ON);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 7U, 0U);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID, LINKR_DEBUGGER_CONFIG_USB_PC);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_ON);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID, LINKR_DEBUGGER_CONFIG_SD_TARGET);

	seed_report(&report);
	assert(linkr_debugger_config_policy_populate_confirmation_report(
		       &snapshot, false, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED);
	assert(report.confirmation_count == 5U);
	assert(report.confirmation_items[0] ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
					       LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID));
	assert(report.confirmation_items[1] ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
					       LINKR_DEBUGGER_CONFIG_POWER_20V_OUT_ID));
	assert(report.confirmation_items[2] ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
					       LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID));
	assert(report.confirmation_items[3] ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
					       LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID));
	assert(report.confirmation_items[4] ==
	       linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 10U));
	for (size_t i = 1U; i < report.confirmation_count; i++) {
		const struct linkr_debugger_config_item_desc *previous =
			report.confirmation_items[i - 1U];
		const struct linkr_debugger_config_item_desc *item =
			report.confirmation_items[i];

		assert(item->domain > previous->domain ||
		       (item->domain == previous->domain &&
			item->item_id > previous->item_id));
	}
	assert_confirmation_tail_clear(&report);

	seed_report(&report);
	assert(linkr_debugger_config_policy_populate_confirmation_report(
		       &snapshot, true, &report) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(report.confirmation_count == 5U);
	assert_apply_report_fields_preserved(&report);
}

static void test_confirmation_report_handles_safe_snapshot_and_malformed_calls(void)
{
	struct linkr_debugger_config_snapshot snapshot;
	struct linkr_debugger_config_operation_report report;

	memset(&snapshot, 0, sizeof(snapshot));
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_OFF);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID,
		     LINKR_DEBUGGER_CONFIG_SD_USB_READER);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID,
		     LINKR_DEBUGGER_CONFIG_TF_WP_PROTECTED);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID, LINKR_DEBUGGER_CONFIG_VIN_3V3);
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 29U, 0U);
	seed_report(&report);
	assert(linkr_debugger_config_policy_populate_confirmation_report(
		       &snapshot, false, &report) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(report.confirmation_count == 0U);
	assert_confirmation_tail_clear(&report);
	assert_apply_report_fields_preserved(&report);

	assert(linkr_debugger_config_policy_populate_confirmation_report(
		       &snapshot, false, NULL) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	seed_report(&report);
	assert(linkr_debugger_config_policy_populate_confirmation_report(
		       NULL, false, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT);
	assert(report.confirmation_count == 0U);
	assert_confirmation_tail_clear(&report);

	memset(&snapshot, 0, sizeof(snapshot));
	append_entry(&snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID, 9U);
	seed_report(&report);
	assert(linkr_debugger_config_policy_populate_confirmation_report(
		       &snapshot, true, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT);
	assert(report.result == LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT);
	assert(report.confirmation_count == 0U);
	assert_confirmation_tail_clear(&report);
	assert_apply_report_fields_preserved(&report);
}

static void test_full_pipeline_resolve_project_canonicalize_confirm(void)
{
	struct linkr_debugger_control_snapshot control;
	struct linkr_debugger_config_save_request request;
	struct linkr_debugger_config_resolved_selection selection;
	struct linkr_debugger_config_snapshot projected;
	struct linkr_debugger_config_operation_report report;
	size_t expected_dangerous = 0U;

	build_coherent_control(&control);
	build_full_request(&request);
	resolve_or_die(&request, &selection);
	assert(linkr_debugger_config_policy_project_available_snapshot(
		       &control, &selection, &projected) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(linkr_debugger_config_policy_canonicalize_snapshot(&projected) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(projected.entry_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	for (size_t i = 0U; i < projected.entry_count; i++) {
		bool requires_confirmation;

		assert(linkr_debugger_config_classify_entry(
			       &projected.entries[i], &requires_confirmation) ==
		       LINKR_DEBUGGER_CONFIG_CODEC_OK);
		if (requires_confirmation) {
			expected_dangerous++;
		}
	}
	assert(expected_dangerous > 0U);

	seed_report(&report);
	assert(linkr_debugger_config_policy_populate_confirmation_report(
		       &projected, false, &report) ==
	       LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED);
	assert(report.confirmation_count == expected_dangerous);

	seed_report(&report);
	assert(linkr_debugger_config_policy_populate_confirmation_report(
		       &projected, true, &report) == LINKR_DEBUGGER_CONFIG_SERVICE_OK);
	assert(report.confirmation_count == expected_dangerous);
}

int main(void)
{
	test_resolve_all_23_exact_ids_and_preserves_request_order();
	test_resolve_rejects_malformed_requests();
	test_project_copies_selected_values_in_selection_order();
	test_project_full_23_items_copies_every_value_exactly();
	test_project_rejects_incoherent_control_snapshots();
	test_project_rejects_unavailable_selected_items();
	test_canonicalize_sorts_shuffled_23_entries_stably();
	test_canonicalize_rejects_malformed_snapshots();
	test_confirmation_report_classifies_every_domain_value();
	test_confirmation_report_lists_multi_danger_in_canonical_order();
	test_confirmation_report_handles_safe_snapshot_and_malformed_calls();
	test_full_pipeline_resolve_project_canonicalize_confirm();
	printf("linkr_debugger_config_policy: resolve=23 project=23 canonical=23 "
	       "confirm=17/17 passed\n");
	return 0;
}
