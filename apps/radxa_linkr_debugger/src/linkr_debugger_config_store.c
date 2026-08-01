/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_config_store.h"

#include <errno.h>
#include <string.h>

#ifndef LINKR_DEBUGGER_CONFIG_STORE_HOST_TEST
#include <zephyr/settings/settings.h>

struct linkr_debugger_config_store_backend_ops {
	int (*subsys_init)(void *context);
	int (*load_one)(void *context, const char *name, void *value, size_t value_size);
	int (*save_one)(void *context, const char *name, const void *value, size_t value_size);
	int (*delete_one)(void *context, const char *name);
};
#endif

static struct linkr_debugger_config_store_status store_status = {
	.backend_available = false,
	.presence = LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_UNKNOWN,
	.snapshot_valid = false,
	.reason = LINKR_DEBUGGER_CONFIG_STORE_REASON_BACKEND_UNAVAILABLE,
	.last_errno = -ENODEV,
};

#ifndef LINKR_DEBUGGER_CONFIG_STORE_HOST_TEST
static int settings_backend_init(void *context)
{
	(void)context;
	return settings_subsys_init();
}

static int settings_backend_load_one(void *context, const char *name, void *value,
				     size_t value_size)
{
	(void)context;
	return (int)settings_load_one(name, value, value_size);
}

static int settings_backend_save_one(void *context, const char *name, const void *value,
				     size_t value_size)
{
	(void)context;
	return settings_save_one(name, value, value_size);
}

static int settings_backend_delete_one(void *context, const char *name)
{
	(void)context;
	return settings_delete(name);
}

static const struct linkr_debugger_config_store_backend_ops settings_backend_ops = {
	.subsys_init = settings_backend_init,
	.load_one = settings_backend_load_one,
	.save_one = settings_backend_save_one,
	.delete_one = settings_backend_delete_one,
};
#else
static const struct linkr_debugger_config_store_backend_ops *test_backend_ops;
static void *test_backend_context;

void linkr_debugger_config_store_test_set_backend(
	const struct linkr_debugger_config_store_backend_ops *ops, void *context)
{
	test_backend_ops = ops;
	test_backend_context = context;
}
#endif

static const struct linkr_debugger_config_store_backend_ops *backend_ops_get(void)
{
#ifdef LINKR_DEBUGGER_CONFIG_STORE_HOST_TEST
	return test_backend_ops;
#else
	return &settings_backend_ops;
#endif
}

static void *backend_context_get(void)
{
#ifdef LINKR_DEBUGGER_CONFIG_STORE_HOST_TEST
	return test_backend_context;
#else
	return NULL;
#endif
}

static bool backend_ops_valid(const struct linkr_debugger_config_store_backend_ops *ops)
{
	return ops != NULL && ops->subsys_init != NULL && ops->load_one != NULL &&
	       ops->save_one != NULL && ops->delete_one != NULL;
}

static void status_reset(void)
{
	memset(&store_status, 0, sizeof(store_status));
	store_status.presence = LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_UNKNOWN;
	store_status.reason = LINKR_DEBUGGER_CONFIG_STORE_REASON_BACKEND_UNAVAILABLE;
	store_status.last_errno = -ENODEV;
}

static enum linkr_debugger_config_store_result load_snapshot(const uint8_t *encoded,
						      size_t encoded_size)
{
	struct linkr_debugger_config_snapshot snapshot;
	enum linkr_debugger_config_codec_result codec_result;

	if (encoded_size > LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE) {
		store_status.reason = LINKR_DEBUGGER_CONFIG_STORE_REASON_INVALID_SNAPSHOT;
		return LINKR_DEBUGGER_CONFIG_STORE_INVALID_SNAPSHOT;
	}
	codec_result = linkr_debugger_config_decode(encoded, encoded_size, &snapshot);
	if (codec_result == LINKR_DEBUGGER_CONFIG_CODEC_OK) {
		store_status.snapshot = snapshot;
		store_status.snapshot_valid = true;
		store_status.reason = LINKR_DEBUGGER_CONFIG_STORE_REASON_READY;
		return LINKR_DEBUGGER_CONFIG_STORE_OK;
	}
	if (codec_result == LINKR_DEBUGGER_CONFIG_CODEC_NOT_APPLICABLE) {
		store_status.reason = LINKR_DEBUGGER_CONFIG_STORE_REASON_UNSUPPORTED_VERSION;
		return LINKR_DEBUGGER_CONFIG_STORE_UNSUPPORTED_VERSION;
	}
	store_status.reason = LINKR_DEBUGGER_CONFIG_STORE_REASON_INVALID_SNAPSHOT;
	return LINKR_DEBUGGER_CONFIG_STORE_INVALID_SNAPSHOT;
}

enum linkr_debugger_config_store_result linkr_debugger_config_store_init(void)
{
	const struct linkr_debugger_config_store_backend_ops *ops = backend_ops_get();
	uint8_t encoded[LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE];
	int result;

	if (!backend_ops_valid(ops)) {
		return LINKR_DEBUGGER_CONFIG_STORE_INVALID_ARGUMENT;
	}
	status_reset();
	result = ops->subsys_init(backend_context_get());
	if (result < 0) {
		store_status.last_errno = result;
		return LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE;
	}
	store_status.backend_available = true;
	store_status.last_errno = 0;
	result = ops->load_one(backend_context_get(), LINKR_DEBUGGER_CONFIG_STORE_KEY,
			       encoded, sizeof(encoded));
	if (result == 0) {
		store_status.presence = LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_ABSENT;
		store_status.reason = LINKR_DEBUGGER_CONFIG_STORE_REASON_ABSENT;
		return LINKR_DEBUGGER_CONFIG_STORE_OK;
	}
	if (result < 0) {
		store_status.last_errno = result;
		store_status.reason = LINKR_DEBUGGER_CONFIG_STORE_REASON_STORAGE_ERROR;
		return LINKR_DEBUGGER_CONFIG_STORE_STORAGE_ERROR;
	}
	store_status.presence = LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT;
	return load_snapshot(encoded, (size_t)result);
}

enum linkr_debugger_config_store_result linkr_debugger_config_store_status_get(
	struct linkr_debugger_config_store_status *status)
{
	if (status == NULL) {
		return LINKR_DEBUGGER_CONFIG_STORE_INVALID_ARGUMENT;
	}
	*status = store_status;
	return LINKR_DEBUGGER_CONFIG_STORE_OK;
}

enum linkr_debugger_config_store_result linkr_debugger_config_store_snapshot_get(
	struct linkr_debugger_config_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return LINKR_DEBUGGER_CONFIG_STORE_INVALID_ARGUMENT;
	}
	memset(snapshot, 0, sizeof(*snapshot));
	if (store_status.snapshot_valid) {
		*snapshot = store_status.snapshot;
		return LINKR_DEBUGGER_CONFIG_STORE_OK;
	}
	if (!store_status.backend_available) {
		return LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE;
	}
	switch (store_status.reason) {
	case LINKR_DEBUGGER_CONFIG_STORE_REASON_ABSENT:
		return LINKR_DEBUGGER_CONFIG_STORE_NO_SNAPSHOT;
	case LINKR_DEBUGGER_CONFIG_STORE_REASON_INVALID_SNAPSHOT:
		return LINKR_DEBUGGER_CONFIG_STORE_INVALID_SNAPSHOT;
	case LINKR_DEBUGGER_CONFIG_STORE_REASON_UNSUPPORTED_VERSION:
		return LINKR_DEBUGGER_CONFIG_STORE_UNSUPPORTED_VERSION;
	case LINKR_DEBUGGER_CONFIG_STORE_REASON_STORAGE_ERROR:
		return LINKR_DEBUGGER_CONFIG_STORE_STORAGE_ERROR;
	default:
		return LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE;
	}
}

enum linkr_debugger_config_store_result linkr_debugger_config_store_save(
	const struct linkr_debugger_config_snapshot *snapshot)
{
	const struct linkr_debugger_config_store_backend_ops *ops = backend_ops_get();
	uint8_t encoded[LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE];
	size_t encoded_size;
	int result;

	if (snapshot == NULL) {
		return LINKR_DEBUGGER_CONFIG_STORE_INVALID_ARGUMENT;
	}
	if (!store_status.backend_available) {
		return LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE;
	}
	if (linkr_debugger_config_encode(snapshot, encoded, sizeof(encoded), &encoded_size) !=
	    LINKR_DEBUGGER_CONFIG_CODEC_OK) {
		return LINKR_DEBUGGER_CONFIG_STORE_INVALID_SNAPSHOT;
	}
	result = ops->save_one(backend_context_get(), LINKR_DEBUGGER_CONFIG_STORE_KEY,
				encoded, encoded_size);
	if (result < 0) {
		store_status.reason = LINKR_DEBUGGER_CONFIG_STORE_REASON_STORAGE_ERROR;
		store_status.last_errno = result;
		return LINKR_DEBUGGER_CONFIG_STORE_STORAGE_ERROR;
	}
	store_status.presence = LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT;
	store_status.snapshot_valid = true;
	store_status.reason = LINKR_DEBUGGER_CONFIG_STORE_REASON_READY;
	store_status.last_errno = 0;
	store_status.snapshot = *snapshot;
	return LINKR_DEBUGGER_CONFIG_STORE_OK;
}

enum linkr_debugger_config_store_result linkr_debugger_config_store_clear(void)
{
	const struct linkr_debugger_config_store_backend_ops *ops = backend_ops_get();
	int result;

	if (!store_status.backend_available) {
		return LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE;
	}
	result = ops->delete_one(backend_context_get(), LINKR_DEBUGGER_CONFIG_STORE_KEY);
	if (result < 0) {
		store_status.reason = LINKR_DEBUGGER_CONFIG_STORE_REASON_STORAGE_ERROR;
		store_status.last_errno = result;
		return LINKR_DEBUGGER_CONFIG_STORE_STORAGE_ERROR;
	}
	memset(&store_status.snapshot, 0, sizeof(store_status.snapshot));
	store_status.presence = LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_ABSENT;
	store_status.snapshot_valid = false;
	store_status.reason = LINKR_DEBUGGER_CONFIG_STORE_REASON_ABSENT;
	store_status.last_errno = 0;
	return LINKR_DEBUGGER_CONFIG_STORE_OK;
}
