/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_config_policy.h"
#include "linkr_debugger_config_service_state.h"

#include <string.h>

#ifdef LINKR_DEBUGGER_CONFIG_SERVICE_HOST_TEST
#include <pthread.h>
static pthread_mutex_t service_mutex = PTHREAD_MUTEX_INITIALIZER;
#define service_lock() (void)pthread_mutex_lock(&service_mutex)
#define service_unlock() (void)pthread_mutex_unlock(&service_mutex)
#else
#include <zephyr/kernel.h>
K_MUTEX_DEFINE(service_mutex);
#define service_lock() (void)k_mutex_lock(&service_mutex, K_FOREVER)
#define service_unlock() (void)k_mutex_unlock(&service_mutex)
#endif

size_t linkr_debugger_config_service_capture_release_failures;
size_t linkr_debugger_config_service_flash_release_failures;

static struct linkr_debugger_config_service_ops service_ops_table;
static const struct linkr_debugger_config_service_ops *service_ops;
static void *service_context;
static bool service_initialized;
static struct linkr_debugger_config_snapshot saved_snapshot;
static struct linkr_debugger_config_service_status service_status;

static void capture_release(void)
{
	if (!service_ops->capture_release(service_context)) {
		linkr_debugger_config_service_capture_release_failures++;
	}
}

static enum linkr_debugger_config_service_result owners_acquire(void)
{
	if (!service_ops->capture_try_acquire(service_context)) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE;
	}
	if (!service_ops->flash_try_acquire(service_context)) {
		capture_release();
		return LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_FLASH;
	}
	return LINKR_DEBUGGER_CONFIG_SERVICE_OK;
}

static void owners_release(void)
{
	if (!service_ops->flash_release(service_context)) {
		linkr_debugger_config_service_flash_release_failures++;
	}
	capture_release();
}

enum linkr_debugger_config_service_result linkr_debugger_config_service_init_with_ops(
	const struct linkr_debugger_config_service_ops *ops, void *context)
{
	struct linkr_debugger_config_store_status store_status;
	struct linkr_debugger_config_operation_report boot_report = {0};
	enum linkr_debugger_config_store_result outcome;
	enum linkr_debugger_config_service_result result;

	if (!linkr_debugger_config_service_state_ops_valid(ops)) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
	service_lock();
	service_ops_table = *ops;
	service_ops = &service_ops_table;
	service_context = context;
	service_initialized = true;
	memset(&saved_snapshot, 0, sizeof(saved_snapshot));
	memset(&service_status, 0, sizeof(service_status));
	linkr_debugger_config_service_state_seed_items(&service_status);
	outcome = ops->store_status_get(context, &store_status);
	linkr_debugger_config_service_state_set_store_reason(&service_status,
		outcome == LINKR_DEBUGGER_CONFIG_STORE_OK ?
			store_status.reason :
			LINKR_DEBUGGER_CONFIG_STORE_REASON_BACKEND_UNAVAILABLE);
	result = linkr_debugger_config_service_state_init_result(&service_status);
	if (service_status.reason != LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY) {
		service_unlock();
		return result;
	}
	outcome = ops->store_snapshot_get(context, &saved_snapshot);
	if (outcome != LINKR_DEBUGGER_CONFIG_STORE_OK) {
		memset(&saved_snapshot, 0, sizeof(saved_snapshot));
		(void)ops->store_status_get(context, &store_status);
		linkr_debugger_config_service_state_set_store_reason(&service_status,
			store_status.reason);
		service_unlock();
		return linkr_debugger_config_service_state_result_from_store(outcome);
	}
	service_status.snapshot_present = true;
	linkr_debugger_config_service_state_sync_saved(
		&service_status, &saved_snapshot, LINKR_DEBUGGER_CONFIG_APPLY_PENDING);
	result = owners_acquire();
	if (result == LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		(void)linkr_debugger_config_apply_execute(
			&saved_snapshot, LINKR_DEBUGGER_CONFIG_APPLY_MODE_BOOT_SAFE,
			service_ops->control_apply_entry, service_context,
			&service_status, &boot_report);
		result = boot_report.result;
		owners_release();
	}
	service_unlock();
	return result;
}

enum linkr_debugger_config_service_result linkr_debugger_config_service_init(void)
{
	return linkr_debugger_config_service_init_with_ops(
		&linkr_debugger_config_service_production_ops, NULL);
}

enum linkr_debugger_config_service_result linkr_debugger_config_service_status_get(
	struct linkr_debugger_config_service_status *status)
{
	struct linkr_debugger_control_snapshot control_snapshot;

	if (status == NULL) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
	if (!service_initialized) {
		memset(status, 0, sizeof(*status));
		linkr_debugger_config_service_state_seed_items(status);
		status->reason = LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNINITIALIZED;
		return LINKR_DEBUGGER_CONFIG_SERVICE_OK;
	}
	service_lock();
	*status = service_status;
	if (service_ops->control_snapshot_get(service_context, &control_snapshot) == 0) {
		linkr_debugger_config_service_state_merge_current(status,
								&control_snapshot);
	}
	service_unlock();
	return LINKR_DEBUGGER_CONFIG_SERVICE_OK;
}

enum linkr_debugger_config_service_result linkr_debugger_config_service_save(
	const struct linkr_debugger_config_save_request *request,
	struct linkr_debugger_config_operation_report *report)
{
	struct linkr_debugger_config_resolved_selection selection;
	struct linkr_debugger_control_snapshot control_snapshot;
	struct linkr_debugger_config_snapshot snapshot;

	if (request == NULL || report == NULL) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
	memset(report, 0, sizeof(*report));
	report->result = LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	if (!service_initialized) {
		return report->result;
	}
	report->result = linkr_debugger_config_policy_resolve_request(request, &selection);
	if (report->result != LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		return report->result;
	}
	service_lock();
	report->result = owners_acquire();
	if (report->result == LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		if (service_ops->control_snapshot_get(service_context,
						      &control_snapshot) != 0) {
			report->result =
				LINKR_DEBUGGER_CONFIG_SERVICE_CONTROL_CAPTURE_FAILED;
		}
		if (report->result == LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
			report->result =
				linkr_debugger_config_policy_project_available_snapshot(
					&control_snapshot, &selection, &snapshot);
		}
		if (report->result == LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
			report->result =
				linkr_debugger_config_policy_canonicalize_snapshot(&snapshot);
		}
		if (report->result == LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
			report->result =
				linkr_debugger_config_policy_populate_confirmation_report(
					&snapshot, request->confirmed, report);
		}
		if (report->result == LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
			report->result =
				linkr_debugger_config_service_state_result_from_store(
					service_ops->store_save(service_context, &snapshot));
		}
		if (report->result == LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
			saved_snapshot = snapshot;
			linkr_debugger_config_service_state_sync_saved(
				&service_status, &snapshot,
				LINKR_DEBUGGER_CONFIG_APPLY_APPLIED);
			linkr_debugger_config_service_state_set_reason(
				&service_status, LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY);
			service_status.snapshot_present = true;
		}
		owners_release();
	}
	service_unlock();
	return report->result;
}

enum linkr_debugger_config_service_result linkr_debugger_config_service_apply(
	bool confirmed, struct linkr_debugger_config_operation_report *report)
{
	if (report == NULL) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
	memset(report, 0, sizeof(*report));
	report->result = LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	if (!service_initialized) {
		return report->result;
	}
	service_lock();
	report->result =
		linkr_debugger_config_service_state_result_from_reason(
			service_status.reason);
	if (report->result == LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		report->result = linkr_debugger_config_policy_populate_confirmation_report(
			&saved_snapshot, confirmed, report);
	}
	if (report->result == LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		report->result = owners_acquire();
	}
	if (report->result == LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		report->result = linkr_debugger_config_apply_execute(
			&saved_snapshot, LINKR_DEBUGGER_CONFIG_APPLY_MODE_CONFIRMED_FULL,
			service_ops->control_apply_entry, service_context,
			&service_status, report);
		owners_release();
	}
	service_unlock();
	return report->result;
}

enum linkr_debugger_config_service_result linkr_debugger_config_service_clear(void)
{
	enum linkr_debugger_config_service_result result;

	if (!service_initialized) {
		return LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT;
	}
	service_lock();
	result = owners_acquire();
	if (result == LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		result = linkr_debugger_config_service_state_result_from_store(
			service_ops->store_clear(service_context));
		if (result == LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
			memset(&saved_snapshot, 0, sizeof(saved_snapshot));
			linkr_debugger_config_service_state_sync_saved(
				&service_status, &saved_snapshot,
				LINKR_DEBUGGER_CONFIG_APPLY_NOT_SAVED);
			linkr_debugger_config_service_state_set_reason(
				&service_status, LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT);
			service_status.snapshot_present = false;
		}
		owners_release();
	}
	service_unlock();
	return result;
}
