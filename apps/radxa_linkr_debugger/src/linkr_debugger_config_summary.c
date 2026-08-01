#include "linkr_debugger_config_summary.h"

#include <stdio.h>
#include <string.h>

bool linkr_debugger_config_summary_from_status(
	const struct linkr_debugger_config_service_status *status,
	struct linkr_debugger_config_summary *summary)
{
	const char *reason;

	if (status == NULL || summary == NULL ||
	    status->saved_count > LINKR_DEBUGGER_CONFIG_MAX_ENTRIES ||
	    status->pending_count > status->saved_count) {
		return false;
	}

	switch (status->reason) {
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNINITIALIZED:
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE:
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_STORAGE_ERROR:
		reason = "storage_error";
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY:
		reason = "ready";
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT:
		reason = "absent";
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_INVALID_SNAPSHOT:
		reason = "invalid_snapshot";
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNSUPPORTED_VERSION:
		reason = "unsupported_version";
		break;
	default:
		return false;
	}

	summary->available = status->available;
	summary->reason = reason;
	summary->saved_count = status->saved_count;
	summary->pending_count = status->pending_count;
	return true;
}

enum linkr_debugger_config_summary_append_result
linkr_debugger_config_summary_append(
	struct linkr_debugger_config_summary_buffer *buffer,
	const struct linkr_debugger_config_service_status *status)
{
	struct linkr_debugger_config_summary summary;
	char scratch[LINKR_DEBUGGER_CONFIG_SUMMARY_FRAGMENT_MAX + 1U];
	size_t fragment_length;
	size_t remaining;
	int formatted;

	if (buffer == NULL || buffer->data == NULL || buffer->length > buffer->capacity ||
	    !linkr_debugger_config_summary_from_status(status, &summary)) {
		return LINKR_DEBUGGER_CONFIG_SUMMARY_OMITTED;
	}

	formatted = snprintf(scratch, sizeof(scratch),
		",\"config\":{\"available\":%s,\"reason\":\"%s\",\"saved_count\":%zu,"
		"\"pending_count\":%zu}", summary.available ? "true" : "false",
		summary.reason, summary.saved_count, summary.pending_count);
	if (formatted < 0 || (size_t)formatted >= sizeof(scratch) ||
	    (size_t)formatted > LINKR_DEBUGGER_CONFIG_SUMMARY_FRAGMENT_MAX) {
		return LINKR_DEBUGGER_CONFIG_SUMMARY_OMITTED;
	}

	fragment_length = (size_t)formatted;
	remaining = buffer->capacity - buffer->length;
	if (fragment_length > remaining) {
		return LINKR_DEBUGGER_CONFIG_SUMMARY_OMITTED;
	}
	remaining -= fragment_length;
	if (buffer->tail_reserve > remaining) {
		return LINKR_DEBUGGER_CONFIG_SUMMARY_OMITTED;
	}
	remaining -= buffer->tail_reserve;
	if (remaining == 0U) {
		return LINKR_DEBUGGER_CONFIG_SUMMARY_OMITTED;
	}

	memcpy(buffer->data + buffer->length, scratch, fragment_length + 1U);
	buffer->length += fragment_length;
	return LINKR_DEBUGGER_CONFIG_SUMMARY_APPENDED;
}
