#ifndef RADXA_LINKR_DEBUGGER_CONFIG_SERVICE_INTERNAL_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_SERVICE_INTERNAL_H_

#include "linkr_debugger_config_replay.h"
#include "linkr_debugger_config_store.h"

#include <stdbool.h>

struct linkr_debugger_control_snapshot;

struct linkr_debugger_config_service_ops {
	int (*control_snapshot_get)(void *context,
				    struct linkr_debugger_control_snapshot *snapshot);
	linkr_debugger_config_entry_setter_fn control_replay_entry;
	enum linkr_debugger_config_store_result (*store_status_get)(
		void *context, struct linkr_debugger_config_store_status *status);
	enum linkr_debugger_config_store_result (*store_snapshot_get)(
		void *context, struct linkr_debugger_config_snapshot *snapshot);
	enum linkr_debugger_config_store_result (*store_save)(
		void *context, const struct linkr_debugger_config_snapshot *snapshot);
	enum linkr_debugger_config_store_result (*store_clear)(void *context);
	bool (*capture_try_acquire)(void *context);
	bool (*capture_release)(void *context);
	bool (*flash_try_acquire)(void *context);
	bool (*flash_release)(void *context);
};

extern const struct linkr_debugger_config_service_ops
	linkr_debugger_config_service_production_ops;

enum linkr_debugger_config_service_result linkr_debugger_config_service_init_with_ops(
	const struct linkr_debugger_config_service_ops *ops, void *context);

#endif
