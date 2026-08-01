#ifndef RADXA_LINKR_DEBUGGER_CONFIG_SERVICE_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_SERVICE_H_

#include "linkr_debugger_config_codec.h"

#include <stdbool.h>
#include <stddef.h>

enum linkr_debugger_config_service_result {
	LINKR_DEBUGGER_CONFIG_SERVICE_OK = 0,
	LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT,
	LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION,
	LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM,
	LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM,
	LINKR_DEBUGGER_CONFIG_SERVICE_ITEM_UNAVAILABLE,
	LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED,
	LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE,
	LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_FLASH,
	LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE,
	LINKR_DEBUGGER_CONFIG_SERVICE_NO_SNAPSHOT,
	LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT,
	LINKR_DEBUGGER_CONFIG_SERVICE_UNSUPPORTED_VERSION,
	LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR,
	LINKR_DEBUGGER_CONFIG_SERVICE_CONTROL_CAPTURE_FAILED,
	LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED,
};

enum linkr_debugger_config_service_reason {
	LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNINITIALIZED = 0,
	LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY,
	LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT,
	LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE,
	LINKR_DEBUGGER_CONFIG_SERVICE_REASON_STORAGE_ERROR,
	LINKR_DEBUGGER_CONFIG_SERVICE_REASON_INVALID_SNAPSHOT,
	LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNSUPPORTED_VERSION,
};

enum linkr_debugger_config_apply_state {
	LINKR_DEBUGGER_CONFIG_APPLY_NOT_SAVED = 0,
	LINKR_DEBUGGER_CONFIG_APPLY_APPLIED,
	LINKR_DEBUGGER_CONFIG_APPLY_PENDING,
	LINKR_DEBUGGER_CONFIG_APPLY_FAILED,
};

struct linkr_debugger_config_item_status {
	const struct linkr_debugger_config_item_desc *item;
	bool current_available;
	uint8_t current_value;
	bool current_requires_confirmation;
	bool saved;
	uint8_t saved_value;
	bool saved_requires_confirmation;
	enum linkr_debugger_config_apply_state apply_state;
};

struct linkr_debugger_config_service_status {
	bool available;
	enum linkr_debugger_config_service_reason reason;
	bool snapshot_present;
	size_t item_count;
	size_t saved_count;
	size_t applied_count;
	size_t pending_count;
	size_t failed_count;
	const struct linkr_debugger_config_item_desc *failed_item;
	int failed_errno;
	struct linkr_debugger_config_item_status items[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
};

struct linkr_debugger_config_save_request {
	size_t item_count;
	const char *item_ids[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
	bool confirmed;
};

struct linkr_debugger_config_operation_report {
	enum linkr_debugger_config_service_result result;
	size_t confirmation_count;
	const struct linkr_debugger_config_item_desc
		*confirmation_items[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
	size_t applied_count;
	const struct linkr_debugger_config_item_desc
		*applied_items[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
	size_t pending_count;
	const struct linkr_debugger_config_item_desc
		*pending_items[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
	const struct linkr_debugger_config_item_desc *failed_item;
	int failed_errno;
};

enum linkr_debugger_config_service_result linkr_debugger_config_service_init(void);
enum linkr_debugger_config_service_result linkr_debugger_config_service_status_get(
	struct linkr_debugger_config_service_status *status);
enum linkr_debugger_config_service_result linkr_debugger_config_service_save(
	const struct linkr_debugger_config_save_request *request,
	struct linkr_debugger_config_operation_report *report);
enum linkr_debugger_config_service_result linkr_debugger_config_service_apply(
	bool confirmed, struct linkr_debugger_config_operation_report *report);
enum linkr_debugger_config_service_result linkr_debugger_config_service_clear(void);

#endif
