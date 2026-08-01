#include "linkr_debugger_config_http_encode.h"

#include "linkr_debugger_config_http_json.h"
#include "linkr_debugger_config_http_result.h"

#include <string.h>

static const char *action_name(enum linkr_debugger_config_http_action action)
{
	static const char *const names[] = { "get", "save", "apply", "clear" };

	return (unsigned int)action <= LINKR_DEBUGGER_CONFIG_HTTP_ACTION_CLEAR ? names[action] : NULL;
}

static bool catalog_item(const struct linkr_debugger_config_item_desc *item)
{
	return item != NULL && item->id != NULL &&
	       linkr_debugger_config_find_item(item->domain, item->item_id) == item;
}

static const char *kind_name(const struct linkr_debugger_config_item_desc *item)
{
	static const char *const names[] = { NULL, "power", "switch", "gpio" };

	return item->domain <= LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO ? names[item->domain] : NULL;
}

static const char *switch_json(uint8_t item_id, uint8_t value)
{
	static const char *const values[LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID + 1U][2] = {
		[LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID] =
			{ "{\"route\":\"target\"}", "{\"route\":\"usb-reader\"}" },
		[LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID] =
			{ "{\"route\":\"pc\"}", "{\"route\":\"target\"}" },
		[LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID] =
			{ "{\"route\":\"writable\"}", "{\"route\":\"protected\"}" },
		[LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID] =
			{ "{\"route\":\"1.8v\"}", "{\"route\":\"3.3v\"}" },
	};

	return item_id > LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID || value > 1U ?
		NULL : values[item_id][value];
}

static const char *value_json(const struct linkr_debugger_config_item_desc *item,
			      uint8_t value)
{
	switch (item->domain) {
	case LINKR_DEBUGGER_CONFIG_DOMAIN_POWER:
		return value == LINKR_DEBUGGER_CONFIG_POWER_OFF ? "{\"state\":\"off\"}" :
		       value == LINKR_DEBUGGER_CONFIG_POWER_ON ? "{\"state\":\"on\"}" : NULL;
	case LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH:
		return switch_json(item->item_id, value);
	case LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO:
		if (value == 0U) return "{\"direction\":\"input\",\"value\":0}";
		if (value == LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT)
			return "{\"direction\":\"output\",\"value\":0}";
		if (value == LINKR_DEBUGGER_CONFIG_GPIO_VALUE_MASK)
			return "{\"direction\":\"output\",\"value\":1}";
		return NULL;
	default: return NULL;
	}
}

static bool status_valid(const struct linkr_debugger_config_service_status *status)
{
	if (status == NULL || status->item_count != linkr_debugger_config_item_count ||
	    status->item_count != LINKR_DEBUGGER_CONFIG_MAX_ENTRIES ||
	    linkr_debugger_config_http_reason_name(status->reason) == NULL) return false;
	for (size_t i = 0U; i < status->item_count; i++) {
		const struct linkr_debugger_config_item_status *row = &status->items[i];

		if (row->item != &linkr_debugger_config_items[i] ||
		    linkr_debugger_config_http_apply_state_name(row->apply_state) == NULL ||
		    (row->current_available && value_json(row->item, row->current_value) == NULL) ||
		    (row->saved && value_json(row->item, row->saved_value) == NULL)) return false;
	}
	return true;
}

static int begin(struct linkr_debugger_config_http_json *json, bool ok,
		 const char *action)
{
	return linkr_debugger_config_http_json_append(json,
		"{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":%s,"
		"\"command\":\"config\",\"action\":\"%s\"", ok ? "true" : "false", action);
}

static enum linkr_debugger_config_http_encode_result finish(
	struct linkr_debugger_config_http_json *json, size_t *encoded_size)
{
	if (linkr_debugger_config_http_json_append(json, "}\n") != 0) {
		return LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_NO_SPACE;
	}
	*encoded_size = json->length;
	return LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_OK;
}

static bool prepare(struct linkr_debugger_config_http_json *json, char *buffer,
		    size_t capacity, size_t *encoded_size)
{
	if (encoded_size != NULL) *encoded_size = 0U;
	linkr_debugger_config_http_json_init(json, buffer, capacity);
	return buffer != NULL && encoded_size != NULL;
}

static int append_desc_list(struct linkr_debugger_config_http_json *json,
			    const char *field,
			    const struct linkr_debugger_config_item_desc *const *items,
			    size_t count)
{
	if (count > LINKR_DEBUGGER_CONFIG_MAX_ENTRIES ||
	    linkr_debugger_config_http_json_append(json, ",\"%s\":[", field) != 0) return -1;
	for (size_t i = 0U; i < count; i++) {
		if (!catalog_item(items[i]) ||
		    linkr_debugger_config_http_json_append(json, "%s", i == 0U ? "" : ",") != 0 ||
		    linkr_debugger_config_http_json_string(json, items[i]->id) != 0) return -1;
	}
	return linkr_debugger_config_http_json_append(json, "]");
}

static int append_name_list(struct linkr_debugger_config_http_json *json,
			    const char *field, const char *const *items, size_t count)
{
	if (count > LINKR_DEBUGGER_CONFIG_MAX_ENTRIES ||
	    linkr_debugger_config_http_json_append(json, ",\"%s\":[", field) != 0) return -1;
	for (size_t i = 0U; i < count; i++) {
		if (items[i] == NULL || linkr_debugger_config_find_item_by_name(items[i]) == NULL ||
		    linkr_debugger_config_http_json_append(json, "%s", i == 0U ? "" : ",") != 0 ||
		    linkr_debugger_config_http_json_string(json, items[i]) != 0) return -1;
	}
	return linkr_debugger_config_http_json_append(json, "]");
}

static int append_item(struct linkr_debugger_config_http_json *json, const char *field,
		       const struct linkr_debugger_config_item_desc *item)
{
	if (linkr_debugger_config_http_json_append(json, ",\"%s\":", field) != 0) return -1;
	if (item == NULL) return linkr_debugger_config_http_json_append(json, "null");
	return catalog_item(item) ? linkr_debugger_config_http_json_string(json, item->id) : -1;
}

static int append_status_item(struct linkr_debugger_config_http_json *json,
			      const struct linkr_debugger_config_item_status *row,
			      size_t index)
{
	const char *current = row->current_available ? value_json(row->item, row->current_value) : "null";
	const char *saved = row->saved ? value_json(row->item, row->saved_value) : "null";
	const char *risk = row->saved ? (row->saved_requires_confirmation ? "true" : "false") :
		row->current_available ? (row->current_requires_confirmation ? "true" : "false") : "null";

	if (linkr_debugger_config_http_json_append(json, "%s{\"id\":", index == 0U ? "" : ",") != 0 ||
	    linkr_debugger_config_http_json_string(json, row->item->id) != 0) return -1;
	return linkr_debugger_config_http_json_append(json,
		",\"kind\":\"%s\",\"current\":%s,\"saved\":%s,\"selected\":%s,"
		"\"requires_confirm\":%s,\"apply_state\":\"%s\"}",
		kind_name(row->item), current, saved, row->saved ? "true" : "false", risk,
		linkr_debugger_config_http_apply_state_name(row->apply_state));
}

static enum linkr_debugger_config_http_encode_result invalid(
	struct linkr_debugger_config_http_json *json, size_t *encoded_size)
{
	linkr_debugger_config_http_json_discard(json);
	if (encoded_size != NULL) *encoded_size = 0U;
	return LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_INVALID_STATE;
}

enum linkr_debugger_config_http_encode_result linkr_debugger_config_http_encode_get(
	const struct linkr_debugger_config_service_status *status,
	char *buffer, size_t capacity, size_t *encoded_size)
{
	struct linkr_debugger_config_http_json json;
	const char *reason;

	if (!prepare(&json, buffer, capacity, encoded_size) || !status_valid(status))
		return invalid(&json, encoded_size);
	reason = linkr_debugger_config_http_reason_name(status->reason);
	if (begin(&json, true, "get") != 0 ||
	    linkr_debugger_config_http_json_append(&json,
		",\"backend\":{\"available\":%s,\"reason\":\"%s\"},"
		"\"snapshot\":{\"present\":%s,\"version\":%s},\"pending\":%zu,\"items\":[",
		status->available ? "true" : "false", reason,
		status->snapshot_present ? "true" : "false",
		status->snapshot_present ? "1" : "null", status->pending_count) != 0) goto no_space;
	for (size_t i = 0U; i < status->item_count; i++)
		if (append_status_item(&json, &status->items[i], i) != 0) goto no_space;
	if (linkr_debugger_config_http_json_append(&json, "]") != 0) goto no_space;
	return finish(&json, encoded_size);
no_space:
	return LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_NO_SPACE;
}

enum linkr_debugger_config_http_encode_result linkr_debugger_config_http_encode_save(
	const struct linkr_debugger_config_save_request *request,
	const struct linkr_debugger_config_operation_report *report,
	char *buffer, size_t capacity, size_t *encoded_size)
{
	struct linkr_debugger_config_http_json json;

	if (!prepare(&json, buffer, capacity, encoded_size) || request == NULL || report == NULL ||
	    begin(&json, true, "save") != 0) goto failed;
	if (append_name_list(&json, "saved_items", request->item_ids, request->item_count) != 0 ||
	    append_desc_list(&json, "confirmation_items", report->confirmation_items,
			     report->confirmation_count) != 0 ||
	    linkr_debugger_config_http_json_append(&json,
		",\"snapshot\":{\"present\":true,\"version\":1},\"pending\":0") != 0) goto failed;
	return finish(&json, encoded_size);
failed:
	return json.failed ? LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_NO_SPACE : invalid(&json, encoded_size);
}

enum linkr_debugger_config_http_encode_result linkr_debugger_config_http_encode_apply(
	const struct linkr_debugger_config_operation_report *report, bool noop,
	char *buffer, size_t capacity, size_t *encoded_size)
{
	struct linkr_debugger_config_http_json json;

	if (!prepare(&json, buffer, capacity, encoded_size) || report == NULL ||
	    begin(&json, true, "apply") != 0 ||
	    linkr_debugger_config_http_json_append(&json, ",\"noop\":%s", noop ? "true" : "false") != 0 ||
	    append_desc_list(&json, "applied_items", report->applied_items, report->applied_count) != 0) goto failed;
	if (append_item(&json, "failed_item", report->failed_item) != 0) goto failed;
	if (append_desc_list(&json, "pending_items", report->pending_items,
			     report->pending_count) != 0) goto failed;
	return finish(&json, encoded_size);
failed:
	return json.failed ? LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_NO_SPACE : invalid(&json, encoded_size);
}

enum linkr_debugger_config_http_encode_result linkr_debugger_config_http_encode_clear(
	bool noop, char *buffer, size_t capacity, size_t *encoded_size)
{
	struct linkr_debugger_config_http_json json;

	if (!prepare(&json, buffer, capacity, encoded_size)) return invalid(&json, encoded_size);
	if (begin(&json, true, "clear") != 0 ||
	    linkr_debugger_config_http_json_append(&json,
		",\"noop\":%s,\"snapshot\":{\"present\":false,\"version\":null},\"pending\":0",
		noop ? "true" : "false") != 0) return LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_NO_SPACE;
	return finish(&json, encoded_size);
}

enum linkr_debugger_config_http_encode_result linkr_debugger_config_http_encode_error(
	enum linkr_debugger_config_http_action action,
	const struct linkr_debugger_config_http_error *error,
	const struct linkr_debugger_config_operation_report *report,
	char *buffer, size_t capacity, size_t *encoded_size)
{
	struct linkr_debugger_config_http_json json;
	const char *name = action_name(action);
	bool confirmation, apply_failed;

	if (!prepare(&json, buffer, capacity, encoded_size) || error == NULL || name == NULL ||
	    error->code == NULL || error->message == NULL) return invalid(&json, encoded_size);
	confirmation = strcmp(error->code, "confirmation_required") == 0;
	apply_failed = strcmp(error->code, "apply_failed") == 0;
	if ((confirmation || apply_failed) && report == NULL) return invalid(&json, encoded_size);
	if (begin(&json, false, name) != 0 ||
	    linkr_debugger_config_http_json_append(&json, ",\"error\":{\"code\":") != 0 ||
	    linkr_debugger_config_http_json_string(&json, error->code) != 0 ||
	    linkr_debugger_config_http_json_append(&json, ",\"message\":") != 0 ||
	    linkr_debugger_config_http_json_string(&json, error->message) != 0 ||
	    linkr_debugger_config_http_json_append(&json, "}") != 0) goto failed;
	if (confirmation && append_desc_list(&json, "dangerous_items", report->confirmation_items,
					     report->confirmation_count) != 0) goto failed;
	if (error->activity != NULL &&
	    (linkr_debugger_config_http_json_append(&json, ",\"activity\":") != 0 ||
	     linkr_debugger_config_http_json_string(&json, error->activity) != 0)) goto failed;
	if (apply_failed) {
		if (append_desc_list(&json, "applied_items", report->applied_items,
				     report->applied_count) != 0 ||
		    append_item(&json, "failed_item", report->failed_item) != 0) goto failed;
		if (append_desc_list(&json, "pending_items", report->pending_items,
				     report->pending_count) != 0) goto failed;
	}
	return finish(&json, encoded_size);
failed:
	return json.failed ? LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_NO_SPACE : invalid(&json, encoded_size);
}
