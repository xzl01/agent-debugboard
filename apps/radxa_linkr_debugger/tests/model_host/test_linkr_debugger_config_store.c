/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_config_store.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct fake_backend {
	int init_result;
	int load_result;
	int read_result;
	int save_result;
	int delete_result;
	bool stored_present;
	uint8_t stored_value[LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE + 1U];
	size_t stored_length;
	unsigned int init_calls;
	unsigned int load_calls;
	unsigned int save_calls;
	unsigned int delete_calls;
	unsigned int physical_write_count;
	bool key_matches;
};

static void fake_check_key(struct fake_backend *backend, const char *name)
{
	backend->key_matches = backend->key_matches && name != NULL &&
		strcmp(name, LINKR_DEBUGGER_CONFIG_STORE_KEY) == 0;
}

static int fake_subsys_init(void *context)
{
	struct fake_backend *backend = context;

	backend->init_calls++;
	return backend->init_result;
}

static int fake_load_one(void *context, const char *name, void *value, size_t value_size)
{
	struct fake_backend *backend = context;
	size_t copied;

	backend->load_calls++;
	fake_check_key(backend, name);
	if (backend->load_result < 0) {
		return backend->load_result;
	}
	if (backend->read_result < 0) {
		return backend->read_result;
	}
	if (!backend->stored_present) {
		return 0;
	}
	copied = backend->stored_length < value_size ? backend->stored_length : value_size;
	memcpy(value, backend->stored_value, copied);
	return (int)backend->stored_length;
}

static int fake_save_one(void *context, const char *name, const void *value, size_t value_size)
{
	struct fake_backend *backend = context;

	backend->save_calls++;
	fake_check_key(backend, name);
	if (backend->save_result < 0) {
		return backend->save_result;
	}
	assert(value_size <= sizeof(backend->stored_value));
	if (backend->stored_present && backend->stored_length == value_size &&
	    memcmp(backend->stored_value, value, value_size) == 0) {
		return 0;
	}
	memcpy(backend->stored_value, value, value_size);
	backend->stored_length = value_size;
	backend->stored_present = true;
	backend->physical_write_count++;
	return 0;
}

static int fake_delete_one(void *context, const char *name)
{
	struct fake_backend *backend = context;

	backend->delete_calls++;
	fake_check_key(backend, name);
	if (backend->delete_result < 0) {
		return backend->delete_result;
	}
	backend->stored_present = false;
	backend->stored_length = 0U;
	return 0;
}

static const struct linkr_debugger_config_store_backend_ops fake_backend_ops = {
	.subsys_init = fake_subsys_init,
	.load_one = fake_load_one,
	.save_one = fake_save_one,
	.delete_one = fake_delete_one,
};

static bool snapshot_is_zero(const struct linkr_debugger_config_snapshot *snapshot)
{
	if (snapshot->entry_count != 0U) {
		return false;
	}
	for (size_t i = 0U; i < LINKR_DEBUGGER_CONFIG_MAX_ENTRIES; i++) {
		if (snapshot->entries[i].domain != 0U || snapshot->entries[i].item_id != 0U ||
		    snapshot->entries[i].value != 0U) {
			return false;
		}
	}
	return true;
}

static bool snapshots_equal(const struct linkr_debugger_config_snapshot *left,
			    const struct linkr_debugger_config_snapshot *right)
{
	if (left->entry_count != right->entry_count) {
		return false;
	}
	for (size_t i = 0U; i < left->entry_count; i++) {
		if (left->entries[i].domain != right->entries[i].domain ||
		    left->entries[i].item_id != right->entries[i].item_id ||
		    left->entries[i].value != right->entries[i].value) {
			return false;
		}
	}
	return true;
}

static void assert_status(bool available, enum linkr_debugger_config_store_presence presence,
			  bool valid, enum linkr_debugger_config_store_reason reason, int last_errno,
			  const struct linkr_debugger_config_snapshot *snapshot)
{
	struct linkr_debugger_config_store_status status;

	assert(linkr_debugger_config_store_status_get(&status) == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert(status.backend_available == available);
	assert(status.presence == presence);
	assert(status.snapshot_valid == valid);
	assert(status.reason == reason);
	assert(status.last_errno == last_errno);
	if (snapshot != NULL) {
		assert(snapshots_equal(&status.snapshot, snapshot));
	} else {
		assert(snapshot_is_zero(&status.snapshot));
	}
}

static void assert_status_equal(const struct linkr_debugger_config_store_status *expected)
{
	struct linkr_debugger_config_store_status actual;

	assert(linkr_debugger_config_store_status_get(&actual) == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert(actual.backend_available == expected->backend_available);
	assert(actual.presence == expected->presence);
	assert(actual.snapshot_valid == expected->snapshot_valid);
	assert(actual.reason == expected->reason);
	assert(actual.last_errno == expected->last_errno);
	assert(snapshots_equal(&actual.snapshot, &expected->snapshot));
}

static void assert_zero_snapshot_result(enum linkr_debugger_config_store_result expected)
{
	struct linkr_debugger_config_snapshot snapshot;

	memset(&snapshot, 0xa5, sizeof(snapshot));
	assert(linkr_debugger_config_store_snapshot_get(&snapshot) == expected);
	assert(snapshot_is_zero(&snapshot));
}

static void install_backend(struct fake_backend *backend)
{
	memset(backend, 0, sizeof(*backend));
	backend->key_matches = true;
	linkr_debugger_config_store_test_set_backend(&fake_backend_ops, backend);
}

static void make_snapshot(struct linkr_debugger_config_snapshot *snapshot, uint8_t value)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->entry_count = 1U;
	snapshot->entries[0].domain = LINKR_DEBUGGER_CONFIG_DOMAIN_POWER;
	snapshot->entries[0].item_id = LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID;
	snapshot->entries[0].value = value;
}

static void make_two_entry_snapshot(struct linkr_debugger_config_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->entry_count = 2U;
	snapshot->entries[0].domain = LINKR_DEBUGGER_CONFIG_DOMAIN_POWER;
	snapshot->entries[0].item_id = LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID;
	snapshot->entries[0].value = LINKR_DEBUGGER_CONFIG_POWER_ON;
	snapshot->entries[1].domain = LINKR_DEBUGGER_CONFIG_DOMAIN_POWER;
	snapshot->entries[1].item_id = LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID;
	snapshot->entries[1].value = LINKR_DEBUGGER_CONFIG_POWER_OFF;
}

static void store_snapshot(struct fake_backend *backend,
			   const struct linkr_debugger_config_snapshot *snapshot)
{
	size_t encoded_size;

	assert(linkr_debugger_config_encode(snapshot, backend->stored_value,
					  LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE,
					  &encoded_size) == LINKR_DEBUGGER_CONFIG_CODEC_OK);
	backend->stored_present = true;
	backend->stored_length = encoded_size;
}

static void store_payload(struct fake_backend *backend, const uint8_t *payload, size_t length)
{
	assert(length <= sizeof(backend->stored_value));
	memcpy(backend->stored_value, payload, length);
	backend->stored_present = true;
	backend->stored_length = length;
}

static void test_initial_state_and_null_arguments(void)
{
	struct linkr_debugger_config_store_status before;
	struct fake_backend backend;
	struct linkr_debugger_config_store_backend_ops incomplete = fake_backend_ops;

	assert_status(false, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_UNKNOWN, false,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_BACKEND_UNAVAILABLE, -ENODEV, NULL);
	assert_zero_snapshot_result(LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE);
	assert(linkr_debugger_config_store_status_get(NULL) ==
	       LINKR_DEBUGGER_CONFIG_STORE_INVALID_ARGUMENT);
	assert(linkr_debugger_config_store_snapshot_get(NULL) ==
	       LINKR_DEBUGGER_CONFIG_STORE_INVALID_ARGUMENT);
	assert(linkr_debugger_config_store_save(NULL) == LINKR_DEBUGGER_CONFIG_STORE_INVALID_ARGUMENT);
	assert(linkr_debugger_config_store_status_get(&before) == LINKR_DEBUGGER_CONFIG_STORE_OK);

	incomplete.save_one = NULL;
	install_backend(&backend);
	linkr_debugger_config_store_test_set_backend(&incomplete, &backend);
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_INVALID_ARGUMENT);
	assert(backend.init_calls == 0U);
	assert_status_equal(&before);

	linkr_debugger_config_store_test_set_backend(NULL, NULL);
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_INVALID_ARGUMENT);
	assert_status_equal(&before);
}

static void test_init_backend_failure(void)
{
	struct fake_backend backend;

	install_backend(&backend);
	backend.init_result = -EIO;
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE);
	assert(backend.init_calls == 1U);
	assert(backend.load_calls == 0U);
	assert_status(false, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_UNKNOWN, false,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_BACKEND_UNAVAILABLE, -EIO, NULL);
	assert_zero_snapshot_result(LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE);
}

static void test_absent_and_idempotent_clear(void)
{
	struct fake_backend backend;
	struct linkr_debugger_config_snapshot snapshot;

	install_backend(&backend);
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert(backend.init_calls == 1U);
	assert(backend.load_calls == 1U);
	assert(backend.key_matches);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_ABSENT, false,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_ABSENT, 0, NULL);
	assert_zero_snapshot_result(LINKR_DEBUGGER_CONFIG_STORE_NO_SNAPSHOT);

	make_snapshot(&snapshot, LINKR_DEBUGGER_CONFIG_POWER_ON);
	assert(linkr_debugger_config_store_save(&snapshot) == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert(backend.save_calls == 1U);
	assert(backend.physical_write_count == 1U);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT, true,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_READY, 0, &snapshot);

	assert(linkr_debugger_config_store_clear() == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert(backend.delete_calls == 1U);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_ABSENT, false,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_ABSENT, 0, NULL);
	assert(linkr_debugger_config_store_clear() == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert(backend.delete_calls == 2U);
	assert(backend.key_matches);
}

static void test_load_and_read_errors(void)
{
	struct fake_backend backend;

	install_backend(&backend);
	backend.load_result = -EIO;
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_STORAGE_ERROR);
	assert(backend.init_calls == 1U);
	assert(backend.load_calls == 1U);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_UNKNOWN, false,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_STORAGE_ERROR, -EIO, NULL);
	assert_zero_snapshot_result(LINKR_DEBUGGER_CONFIG_STORE_STORAGE_ERROR);

	install_backend(&backend);
	backend.read_result = -EBADMSG;
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_STORAGE_ERROR);
	assert(backend.init_calls == 1U);
	assert(backend.load_calls == 1U);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_UNKNOWN, false,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_STORAGE_ERROR, -EBADMSG, NULL);
	assert_zero_snapshot_result(LINKR_DEBUGGER_CONFIG_STORE_STORAGE_ERROR);
}

static void test_valid_load(void)
{
	struct fake_backend backend;
	struct linkr_debugger_config_snapshot snapshot;
	struct linkr_debugger_config_snapshot loaded;

	make_snapshot(&snapshot, LINKR_DEBUGGER_CONFIG_POWER_ON);
	install_backend(&backend);
	store_snapshot(&backend, &snapshot);
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT, true,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_READY, 0, &snapshot);
	assert(linkr_debugger_config_store_snapshot_get(&loaded) == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert(snapshots_equal(&loaded, &snapshot));
	assert(backend.key_matches);
}

static void test_short_and_oversized_loads(void)
{
	struct fake_backend backend;
	struct linkr_debugger_config_snapshot snapshot;

	for (size_t length = 1U; length < LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE; length++) {
		install_backend(&backend);
		memset(backend.stored_value, 0, sizeof(backend.stored_value));
		backend.stored_present = true;
		backend.stored_length = length;
		assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_INVALID_SNAPSHOT);
		assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT, false,
			      LINKR_DEBUGGER_CONFIG_STORE_REASON_INVALID_SNAPSHOT, 0, NULL);
		assert_zero_snapshot_result(LINKR_DEBUGGER_CONFIG_STORE_INVALID_SNAPSHOT);
		assert(backend.key_matches);
	}

	make_snapshot(&snapshot, LINKR_DEBUGGER_CONFIG_POWER_ON);
	install_backend(&backend);
	store_snapshot(&backend, &snapshot);
	backend.stored_value[LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE] = 0xffU;
	backend.stored_length = LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE + 1U;
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_INVALID_SNAPSHOT);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT, false,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_INVALID_SNAPSHOT, 0, NULL);
	assert(backend.key_matches);
}

static void assert_invalid_payload(const uint8_t *payload, size_t length)
{
	struct fake_backend backend;

	install_backend(&backend);
	store_payload(&backend, payload, length);
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_INVALID_SNAPSHOT);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT, false,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_INVALID_SNAPSHOT, 0, NULL);
	assert_zero_snapshot_result(LINKR_DEBUGGER_CONFIG_STORE_INVALID_SNAPSHOT);
	assert(backend.key_matches);
}

static void test_malformed_representatives(void)
{
	struct linkr_debugger_config_snapshot snapshot;
	uint8_t payload[LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE] = {0};
	size_t length;

	make_snapshot(&snapshot, LINKR_DEBUGGER_CONFIG_POWER_ON);
	assert(linkr_debugger_config_encode(&snapshot, payload, sizeof(payload), &length) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_OK);
	payload[0] = 'X';
	assert_invalid_payload(payload, length);

	assert(linkr_debugger_config_encode(&snapshot, payload, sizeof(payload), &length) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_OK);
	payload[8] = 0U;
	assert_invalid_payload(payload, length);

	assert(linkr_debugger_config_encode(&snapshot, payload, sizeof(payload), &length) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_OK);
	payload[7] = 1U;
	assert_invalid_payload(payload, length);

	make_two_entry_snapshot(&snapshot);
	assert(linkr_debugger_config_encode(&snapshot, payload, sizeof(payload), &length) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_OK);
	payload[13] = LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID;
	payload[17] = LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID;
	assert_invalid_payload(payload, length);

	make_two_entry_snapshot(&snapshot);
	assert(linkr_debugger_config_encode(&snapshot, payload, sizeof(payload), &length) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_OK);
	payload[17] = LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID;
	assert_invalid_payload(payload, length);

	memset(payload, 0, sizeof(payload));
	memcpy(payload, LINKR_DEBUGGER_CONFIG_MAGIC, 4U);
	payload[4] = LINKR_DEBUGGER_CONFIG_VERSION;
	payload[5] = LINKR_DEBUGGER_CONFIG_ENTRY_SIZE;
	payload[8] = LINKR_DEBUGGER_CONFIG_HEADER_SIZE;
	assert_invalid_payload(payload, LINKR_DEBUGGER_CONFIG_HEADER_SIZE);
}

static void test_unsupported_load(void)
{
	struct fake_backend backend;
	struct linkr_debugger_config_snapshot snapshot;

	make_snapshot(&snapshot, LINKR_DEBUGGER_CONFIG_POWER_ON);
	install_backend(&backend);
	store_snapshot(&backend, &snapshot);
	backend.stored_value[4] = LINKR_DEBUGGER_CONFIG_VERSION + 1U;
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_UNSUPPORTED_VERSION);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT, false,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_UNSUPPORTED_VERSION, 0, NULL);
	assert_zero_snapshot_result(LINKR_DEBUGGER_CONFIG_STORE_UNSUPPORTED_VERSION);
	assert(backend.key_matches);
}

static void test_save_replaces_nonvalid_states(void)
{
	struct fake_backend backend;
	struct linkr_debugger_config_snapshot snapshot;
	uint8_t malformed[LINKR_DEBUGGER_CONFIG_HEADER_SIZE] = {0};

	make_snapshot(&snapshot, LINKR_DEBUGGER_CONFIG_POWER_ON);
	install_backend(&backend);
	store_payload(&backend, malformed, sizeof(malformed));
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_INVALID_SNAPSHOT);
	assert(linkr_debugger_config_store_save(&snapshot) == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT, true,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_READY, 0, &snapshot);
	assert(backend.save_calls == 1U);
	assert(backend.physical_write_count == 1U);

	install_backend(&backend);
	store_snapshot(&backend, &snapshot);
	backend.stored_value[4] = LINKR_DEBUGGER_CONFIG_VERSION + 1U;
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_UNSUPPORTED_VERSION);
	assert(linkr_debugger_config_store_save(&snapshot) == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT, true,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_READY, 0, &snapshot);
	assert(backend.save_calls == 1U);
	assert(backend.physical_write_count == 1U);
	assert(backend.key_matches);
}

static void test_save_dedup_and_change(void)
{
	struct fake_backend backend;
	struct linkr_debugger_config_snapshot on_snapshot;
	struct linkr_debugger_config_snapshot off_snapshot;

	make_snapshot(&on_snapshot, LINKR_DEBUGGER_CONFIG_POWER_ON);
	make_snapshot(&off_snapshot, LINKR_DEBUGGER_CONFIG_POWER_OFF);
	install_backend(&backend);
	store_snapshot(&backend, &on_snapshot);
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert(linkr_debugger_config_store_save(&on_snapshot) == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert(backend.save_calls == 1U);
	assert(backend.physical_write_count == 0U);
	assert(linkr_debugger_config_store_save(&off_snapshot) == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert(backend.save_calls == 2U);
	assert(backend.physical_write_count == 1U);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT, true,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_READY, 0, &off_snapshot);
	assert(backend.key_matches);
}

static void test_save_errors_preserve_content(void)
{
	struct fake_backend backend;
	struct linkr_debugger_config_snapshot on_snapshot;
	struct linkr_debugger_config_snapshot off_snapshot;
	struct linkr_debugger_config_snapshot invalid_snapshot = {0};
	struct linkr_debugger_config_snapshot loaded;
	struct linkr_debugger_config_store_status before;

	make_snapshot(&on_snapshot, LINKR_DEBUGGER_CONFIG_POWER_ON);
	make_snapshot(&off_snapshot, LINKR_DEBUGGER_CONFIG_POWER_OFF);
	install_backend(&backend);
	store_snapshot(&backend, &on_snapshot);
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_OK);
	backend.save_result = -EIO;
	assert(linkr_debugger_config_store_save(&off_snapshot) ==
	       LINKR_DEBUGGER_CONFIG_STORE_STORAGE_ERROR);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT, true,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_STORAGE_ERROR, -EIO, &on_snapshot);
	assert(linkr_debugger_config_store_snapshot_get(&loaded) == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert(snapshots_equal(&loaded, &on_snapshot));
	assert(backend.save_calls == 1U);
	assert(backend.physical_write_count == 0U);

	install_backend(&backend);
	store_snapshot(&backend, &on_snapshot);
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert(linkr_debugger_config_store_status_get(&before) == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert(linkr_debugger_config_store_save(&invalid_snapshot) ==
	       LINKR_DEBUGGER_CONFIG_STORE_INVALID_SNAPSHOT);
	assert(backend.save_calls == 0U);
	assert_status_equal(&before);
}

static void test_clear_nonvalid_and_delete_error(void)
{
	struct fake_backend backend;
	struct linkr_debugger_config_snapshot snapshot;
	struct linkr_debugger_config_snapshot loaded;
	uint8_t malformed[LINKR_DEBUGGER_CONFIG_HEADER_SIZE] = {0};

	install_backend(&backend);
	store_payload(&backend, malformed, sizeof(malformed));
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_INVALID_SNAPSHOT);
	assert(linkr_debugger_config_store_clear() == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_ABSENT, false,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_ABSENT, 0, NULL);
	assert(backend.delete_calls == 1U);

	make_snapshot(&snapshot, LINKR_DEBUGGER_CONFIG_POWER_ON);
	install_backend(&backend);
	store_snapshot(&backend, &snapshot);
	backend.stored_value[4] = LINKR_DEBUGGER_CONFIG_VERSION + 1U;
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_UNSUPPORTED_VERSION);
	assert(linkr_debugger_config_store_clear() == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_ABSENT, false,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_ABSENT, 0, NULL);
	assert(backend.delete_calls == 1U);

	install_backend(&backend);
	store_snapshot(&backend, &snapshot);
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_OK);
	backend.delete_result = -EIO;
	assert(linkr_debugger_config_store_clear() == LINKR_DEBUGGER_CONFIG_STORE_STORAGE_ERROR);
	assert_status(true, LINKR_DEBUGGER_CONFIG_STORE_PRESENCE_PRESENT, true,
		      LINKR_DEBUGGER_CONFIG_STORE_REASON_STORAGE_ERROR, -EIO, &snapshot);
	assert(linkr_debugger_config_store_snapshot_get(&loaded) == LINKR_DEBUGGER_CONFIG_STORE_OK);
	assert(snapshots_equal(&loaded, &snapshot));
	assert(backend.delete_calls == 1U);
	assert(backend.stored_present);
	assert(backend.key_matches);
}

static void test_unavailable_mutations(void)
{
	struct fake_backend backend;
	struct linkr_debugger_config_snapshot snapshot;

	make_snapshot(&snapshot, LINKR_DEBUGGER_CONFIG_POWER_ON);
	install_backend(&backend);
	backend.init_result = -EIO;
	assert(linkr_debugger_config_store_init() == LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE);
	assert(linkr_debugger_config_store_save(&snapshot) ==
	       LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE);
	assert(linkr_debugger_config_store_clear() == LINKR_DEBUGGER_CONFIG_STORE_BACKEND_UNAVAILABLE);
	assert(backend.save_calls == 0U);
	assert(backend.delete_calls == 0U);
}

int main(void)
{
	test_initial_state_and_null_arguments();
	test_init_backend_failure();
	test_absent_and_idempotent_clear();
	test_load_and_read_errors();
	test_valid_load();
	test_short_and_oversized_loads();
	test_malformed_representatives();
	test_unsupported_load();
	test_save_replaces_nonvalid_states();
	test_save_dedup_and_change();
	test_save_errors_preserve_content();
	test_clear_nonvalid_and_delete_error();
	test_unavailable_mutations();
	printf("linkr_debugger_config_store: all tests passed\n");
	return 0;
}
