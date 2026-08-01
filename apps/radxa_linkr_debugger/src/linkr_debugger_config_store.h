/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_CONFIG_STORE_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_STORE_H_

#include "linkr_debugger_config_codec.h"

#include <stdbool.h>
#include <stddef.h>

#define LINKR_DEBUGGER_CONFIG_STORE_KEY "linkr/config/snapshot"

enum linkr_debugger_config_store_presence {
	LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_UNKNOWN = 0,
	LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_ABSENT,
	LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT,
};

enum linkr_debugger_config_store_reason {
	LINKR_DEBUGGER_CONFIG_STORE_REASON_BACKEND_UNAVAILABLE = 0,
	LINKR_DEBUGGER_CONFIG_STORE_REASON_ABSENT,
	LINKR_DEBUGGER_CONFIG_STORE_REASON_READY,
	LINKR_DEBUGGER_CONFIG_STORE_REASON_INVALID_SNAPSHOT,
	LINKR_DEBUGGER_CONFIG_STORE_REASON_UNSUPPORTED_VERSION,
	LINKR_DEBUGGER_CONFIG_STORE_REASON_STORAGE_ERROR,
};

enum linkr_debugger_config_store_result {
	LINKR_DEBUGGER_CONFIG_STORE_OK = 0,
	LINKR_DEBUGGER_CONFIG_STORE_INVALID_ARGUMENT,
	LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE,
	LINKR_DEBUGGER_CONFIG_STORE_NO_SNAPSHOT,
	LINKR_DEBUGGER_CONFIG_STORE_INVALID_SNAPSHOT,
	LINKR_DEBUGGER_CONFIG_STORE_UNSUPPORTED_VERSION,
	LINKR_DEBUGGER_CONFIG_STORE_STORAGE_ERROR,
};

struct linkr_debugger_config_store_status {
	bool backend_available;
	enum linkr_debugger_config_store_presence presence;
	bool snapshot_valid;
	enum linkr_debugger_config_store_reason reason;
	int last_errno;
	struct linkr_debugger_config_snapshot snapshot;
};

enum linkr_debugger_config_store_result linkr_debugger_config_store_init(void);

enum linkr_debugger_config_store_result linkr_debugger_config_store_status_get(
	struct linkr_debugger_config_store_status *status);

enum linkr_debugger_config_store_result linkr_debugger_config_store_snapshot_get(
	struct linkr_debugger_config_snapshot *snapshot);

enum linkr_debugger_config_store_result linkr_debugger_config_store_save(
	const struct linkr_debugger_config_snapshot *snapshot);

enum linkr_debugger_config_store_result linkr_debugger_config_store_clear(void);

#ifdef LINKR_DEBUGGER_CONFIG_STORE_HOST_TEST

struct linkr_debugger_config_store_backend_ops {
	int (*subsys_init)(void *context);
	int (*load_one)(void *context, const char *name, void *value,
			size_t value_size);
	int (*save_one)(void *context, const char *name, const void *value,
			size_t value_size);
	int (*delete_one)(void *context, const char *name);
};

void linkr_debugger_config_store_test_set_backend(
	const struct linkr_debugger_config_store_backend_ops *ops,
	void *context);

#endif

#endif
