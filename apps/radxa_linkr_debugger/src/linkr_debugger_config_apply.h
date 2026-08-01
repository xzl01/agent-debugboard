#ifndef RADXA_LINKR_DEBUGGER_CONFIG_APPLY_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_APPLY_H_

#include "linkr_debugger_config_service.h"

enum linkr_debugger_config_apply_mode {
	LINKR_DEBUGGER_CONFIG_APPLY_MODE_BOOT_SAFE = 0,
	LINKR_DEBUGGER_CONFIG_APPLY_MODE_CONFIRMED_FULL,
};

typedef int (*linkr_debugger_config_entry_setter_fn)(
	void *context, const struct linkr_debugger_config_entry *entry);

enum linkr_debugger_config_service_result linkr_debugger_config_apply_order_snapshot(
	const struct linkr_debugger_config_snapshot *snapshot,
	struct linkr_debugger_config_snapshot *ordered_snapshot);
enum linkr_debugger_config_service_result linkr_debugger_config_apply_execute(
	const struct linkr_debugger_config_snapshot *snapshot,
	enum linkr_debugger_config_apply_mode mode,
	linkr_debugger_config_entry_setter_fn setter, void *context,
	struct linkr_debugger_config_service_status *status,
	struct linkr_debugger_config_operation_report *report);

#endif
