#ifndef RADXA_LINKR_DEBUGGER_CONFIG_SUMMARY_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_SUMMARY_H_

#include "linkr_debugger_config_service.h"

#include <stdbool.h>
#include <stddef.h>

#define LINKR_DEBUGGER_CONFIG_SUMMARY_FRAGMENT_MAX 96U

enum linkr_debugger_config_summary_append_result {
	LINKR_DEBUGGER_CONFIG_SUMMARY_OMITTED = 0,
	LINKR_DEBUGGER_CONFIG_SUMMARY_APPENDED,
};

struct linkr_debugger_config_summary {
	bool available;
	const char *reason;
	size_t saved_count;
	size_t pending_count;
};

struct linkr_debugger_config_summary_buffer {
	char *data;
	size_t capacity;
	size_t length;
	size_t tail_reserve;
};

bool linkr_debugger_config_summary_from_status(
	const struct linkr_debugger_config_service_status *status,
	struct linkr_debugger_config_summary *summary);

enum linkr_debugger_config_summary_append_result
linkr_debugger_config_summary_append(
	struct linkr_debugger_config_summary_buffer *buffer,
	const struct linkr_debugger_config_service_status *status);

#endif
