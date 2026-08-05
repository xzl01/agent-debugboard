#ifndef RADXA_LINKR_DEBUGGER_CONFIG_REPLAY_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_REPLAY_H_

#include "linkr_debugger_config_service.h"

typedef int (*linkr_debugger_config_entry_setter_fn)(
	void *context, const struct linkr_debugger_config_entry *entry);

enum linkr_debugger_config_service_result linkr_debugger_config_replay_order_snapshot(
	const struct linkr_debugger_config_snapshot *snapshot,
	struct linkr_debugger_config_snapshot *ordered_snapshot);
enum linkr_debugger_config_service_result linkr_debugger_config_replay_execute(
	const struct linkr_debugger_config_snapshot *snapshot,
	linkr_debugger_config_entry_setter_fn setter, void *context,
	struct linkr_debugger_config_service_status *status,
	struct linkr_debugger_config_operation_report *report);

#endif
