/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_CONFIG_SERVICE_STATE_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_SERVICE_STATE_H_

#include "linkr_debugger_config_service_internal.h"
#include "linkr_debugger_control.h"

#include <stdbool.h>

bool linkr_debugger_config_service_state_ops_valid(
	const struct linkr_debugger_config_service_ops *ops);
bool linkr_debugger_config_service_state_reason_available(
	enum linkr_debugger_config_service_reason reason);
enum linkr_debugger_config_service_reason
linkr_debugger_config_service_state_reason_from_store(
	enum linkr_debugger_config_store_reason reason);
enum linkr_debugger_config_service_result
linkr_debugger_config_service_state_result_from_store(
	enum linkr_debugger_config_store_result result);
enum linkr_debugger_config_service_result
linkr_debugger_config_service_state_result_from_reason(
	enum linkr_debugger_config_service_reason reason);
void linkr_debugger_config_service_state_set_reason(
	struct linkr_debugger_config_service_status *status,
	enum linkr_debugger_config_service_reason reason);
void linkr_debugger_config_service_state_set_store_reason(
	struct linkr_debugger_config_service_status *status,
	enum linkr_debugger_config_store_reason reason);
enum linkr_debugger_config_service_result
linkr_debugger_config_service_state_init_result(
	const struct linkr_debugger_config_service_status *status);
void linkr_debugger_config_service_state_seed_items(
	struct linkr_debugger_config_service_status *status);
void linkr_debugger_config_service_state_sync_saved(
	struct linkr_debugger_config_service_status *status,
	const struct linkr_debugger_config_snapshot *snapshot,
	enum linkr_debugger_config_apply_state state);
void linkr_debugger_config_service_state_merge_current(
	struct linkr_debugger_config_service_status *status,
	const struct linkr_debugger_control_snapshot *control_snapshot);

#endif
