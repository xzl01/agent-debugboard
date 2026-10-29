/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "../../src/linkr_debugger_capture_arbiter.h"
#include "../../src/linkr_debugger_flash_arbiter.h"
#include "../../src/linkr_debugger_task.h"
#include "../../src/linkr_debugger_task_mutation.h"
#include "../../src/linkr_debugger_task_sha256.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define FAKE_KEY_COUNT 24U
#define FAKE_NAME_MAX 32U
#define FAKE_WRITE_LOG_MAX 192U
#define ARRAY_SIZE(array_) (sizeof(array_) / sizeof((array_)[0]))
#define TASK_KEY_MANIFEST_NAME "linkr/task/manifest"

struct fake_entry {
	bool present;
	size_t len;
	uint8_t data[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE];
};

struct fake_settings {
	/* Behavioral model of the Settings NVS backend properties the task
	 * atomicity contract relies on: per-key atomic saves, absent keys read
	 * back as length 0, and a power cut that loses only the in-flight
	 * write. It does not model wear, GC or lookup caches.
	 */
	struct fake_entry entries[FAKE_KEY_COUNT];
	char names[FAKE_KEY_COUNT][FAKE_NAME_MAX];
	bool initialized;
	unsigned int save_calls;
	unsigned int load_calls;
	unsigned int delete_calls;
	unsigned int step_count;
	unsigned int cut_at_step;
	bool power_dead;
	bool force_enospc;
	bool corrupt_next_read;
	unsigned int write_log_count;
	char write_log[FAKE_WRITE_LOG_MAX][FAKE_NAME_MAX];
	unsigned int delete_log_count;
	char delete_log[FAKE_WRITE_LOG_MAX][FAKE_NAME_MAX];
	uint32_t random_counter;
};

static struct fake_settings fake;
static struct fake_settings baseline;
static uint8_t mutation_body[LINKR_DEBUGGER_TASK_MUTATION_MAX_SIZE];

struct fake_store_snapshot {
	struct linkr_debugger_task_status status;
	uint8_t ir[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE];
	size_t ir_len;
	uint8_t source[LINKR_DEBUGGER_TASK_MAX_TASKS][LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE];
	size_t source_len[LINKR_DEBUGGER_TASK_MAX_TASKS];
};

static struct fake_store_snapshot old_snapshot;
static struct fake_store_snapshot new_snapshot;

static int fake_find(const struct fake_settings *state, const char *name)
{
	for (size_t i = 0U; i < FAKE_KEY_COUNT; i++) {
		if (state->entries[i].present && strcmp(state->names[i], name) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static void fake_log(char log[FAKE_WRITE_LOG_MAX][FAKE_NAME_MAX],
		     unsigned int *count, const char *name)
{
	assert(*count < FAKE_WRITE_LOG_MAX);
	(void)snprintf(log[*count], FAKE_NAME_MAX, "%s", name);
	(*count)++;
}

static int fake_subsys_init(void *context)
{
	struct fake_settings *state = context;

	if (state->power_dead) {
		return -EIO;
	}
	state->initialized = true;
	return 0;
}

static int fake_load_one(void *context, const char *name, void *value, size_t cap)
{
	struct fake_settings *state = context;
	int index;
	size_t copied;

	state->load_calls++;
	if (state->power_dead) {
		return -EIO;
	}
	index = fake_find(state, name);
	if (index < 0) {
		return 0;
	}
	copied = state->entries[index].len < cap ? state->entries[index].len : cap;
	memcpy(value, state->entries[index].data, copied);
	if (state->corrupt_next_read && copied > 0U) {
		((uint8_t *)value)[copied / 2U] ^= 0x80U;
		state->corrupt_next_read = false;
	}
	return (int)state->entries[index].len;
}

static int fake_save_one(void *context, const char *name, const void *value,
			 size_t len)
{
	struct fake_settings *state = context;
	int index;

	state->save_calls++;
	if (state->power_dead) {
		return -EIO;
	}
	state->step_count++;
	if (state->cut_at_step != 0U && state->step_count == state->cut_at_step) {
		state->power_dead = true;
		return -EIO;
	}
	if (state->force_enospc) {
		state->force_enospc = false;
		return -ENOSPC;
	}
	assert(len <= LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE);
	index = fake_find(state, name);
	if (index < 0) {
		for (size_t i = 0U; i < FAKE_KEY_COUNT; i++) {
			if (!state->entries[i].present) {
				index = (int)i;
				break;
			}
		}
		assert(index >= 0);
		(void)snprintf(state->names[index], FAKE_NAME_MAX, "%s", name);
		state->entries[index].present = true;
	}
	memcpy(state->entries[index].data, value, len);
	state->entries[index].len = len;
	fake_log(state->write_log, &state->write_log_count, name);
	return 0;
}

static int fake_delete_one(void *context, const char *name)
{
	struct fake_settings *state = context;
	int index;

	state->delete_calls++;
	if (state->power_dead) {
		return -EIO;
	}
	state->step_count++;
	if (state->cut_at_step != 0U && state->step_count == state->cut_at_step) {
		state->power_dead = true;
		return -EIO;
	}
	index = fake_find(state, name);
	if (index >= 0) {
		state->entries[index].present = false;
		state->entries[index].len = 0U;
		fake_log(state->delete_log, &state->delete_log_count, name);
	}
	return 0;
}

static void fake_random(void *context, void *value, size_t size)
{
	struct fake_settings *state = context;
	uint8_t *bytes = value;

	for (size_t i = 0U; i < size; i++) {
		bytes[i] = (uint8_t)(state->random_counter++ + 1U);
	}
}

static const struct linkr_debugger_task_backend_ops fake_backend = {
	.subsys_init = fake_subsys_init,
	.load_one = fake_load_one,
	.save_one = fake_save_one,
	.delete_one = fake_delete_one,
	.random = fake_random,
};

/* Minimal JSON escaping for task IDs only; source/IR travel as canonical
 * base64 and never need JSON escaping. */
static size_t json_escape(char *out, size_t cap, const char *text, size_t len)
{
	size_t written = 0U;

	for (size_t i = 0U; i < len; i++) {
		char ch = text[i];

		assert(written + 3U < cap);
		if (ch == '"' || ch == '\\') {
			out[written++] = '\\';
		}
		out[written++] = ch;
	}
	out[written] = '\0';
	return written;
}

/* Canonical RFC 4648 base64 with padding and zero pad bits. */
static size_t base64_encode(const uint8_t *data, size_t len, char *out,
			    size_t cap)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t written = 0U;

	assert(cap >= ((len + 2U) / 3U) * 4U + 1U);
	for (size_t i = 0U; i < len; i += 3U) {
		uint32_t chunk = (uint32_t)data[i] << 16;
		size_t remain = len - i;

		if (remain > 1U) {
			chunk |= (uint32_t)data[i + 1U] << 8;
		}
		if (remain > 2U) {
			chunk |= data[i + 2U];
		}
		out[written++] = alphabet[(chunk >> 18) & 0x3fU];
		out[written++] = alphabet[(chunk >> 12) & 0x3fU];
		out[written++] = remain > 1U ? alphabet[(chunk >> 6) & 0x3fU] : '=';
		out[written++] = remain > 2U ? alphabet[chunk & 0x3fU] : '=';
	}
	out[written] = '\0';
	return written;
}

/* Exact canonical base64 length of one maximum 4000-byte payload. */
#define TASK_TEST_B64_PAYLOAD_MAX \
	(((LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE + 2U) / 3U) * 4U)
/* Room for the largest payload any test feeds through the encoder,
 * including deliberate oversize inputs. */
#define TASK_TEST_B64_MAX \
	(((LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE + 3U + 2U) / 3U) * 4U)

static size_t encode_mutation_json(enum linkr_debugger_task_mutation_opcode opcode,
	const struct linkr_debugger_task_status *status, const char *id,
	const uint8_t *source, size_t source_len, const char *ir, size_t ir_len,
	uint64_t expected_revision, bool bad_catalog, uint8_t *out, size_t cap)
{
	char catalog[2U * LINKR_DEBUGGER_TASK_SHA256_SIZE + 1U];
	char escaped_id[4U * LINKR_DEBUGGER_TASK_MAX_TASK_ID_LEN];
	static char encoded[TASK_TEST_B64_MAX + 1U];
	char *body = (char *)out;
	size_t len;
	int written;

	assert(status != NULL);
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_SHA256_SIZE; i++) {
		uint8_t byte = status->catalog_fingerprint[i];

		if (bad_catalog && i == 0U) {
			byte ^= 1U;
		}
		(void)snprintf(catalog + 2U * i, 3U, "%02x", byte);
	}
	written = snprintf(body, cap,
		"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"%s\","
		"\"expected_revision\":\"%llu\",\"catalog_fingerprint\":\"%s\"",
		linkr_debugger_task_mutation_action(opcode),
		(unsigned long long)expected_revision, catalog);
	assert(written > 0 && (size_t)written < cap);
	len = (size_t)written;
	if (id != NULL) {
		(void)json_escape(escaped_id, sizeof(escaped_id), id, strlen(id));
		written = snprintf(body + len, cap - len, ",\"id\":\"%s\"", escaped_id);
		assert(written > 0 && (size_t)written < cap - len);
		len += (size_t)written;
	}
	if (source != NULL && source_len > 0U) {
		(void)base64_encode(source, source_len, encoded, sizeof(encoded));
		written = snprintf(body + len, cap - len, ",\"source_base64\":\"%s\"",
				   encoded);
		assert(written > 0 && (size_t)written < cap - len);
		len += (size_t)written;
	}
	if (ir != NULL && ir_len > 0U) {
		(void)base64_encode((const uint8_t *)ir, ir_len, encoded,
				    sizeof(encoded));
		written = snprintf(body + len, cap - len, ",\"ir_base64\":\"%s\"", encoded);
		assert(written > 0 && (size_t)written < cap - len);
		len += (size_t)written;
	}
	written = snprintf(body + len, cap - len, "}");
	assert(written > 0 && (size_t)written < cap - len);
	len += (size_t)written;
	return len;
}

static enum linkr_debugger_task_result mutate_expected(
	enum linkr_debugger_task_mutation_opcode opcode, const char *id,
	const uint8_t *source, size_t source_len, const char *ir, size_t ir_len,
	uint64_t expected_revision, bool bad_catalog,
	struct linkr_debugger_task_mutation_outcome *outcome)
{
	struct linkr_debugger_task_status status;
	enum linkr_debugger_task_mutation_opcode decoded = 0;
	size_t len;

	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	len = encode_mutation_json(opcode, &status, id, source, source_len, ir, ir_len,
				   expected_revision, bad_catalog, mutation_body,
				   sizeof(mutation_body));
	return linkr_debugger_task_mutation_execute(mutation_body, len, outcome, &decoded);
}

static enum linkr_debugger_task_result mutate(
	enum linkr_debugger_task_mutation_opcode opcode, const char *id,
	const uint8_t *source, size_t source_len, const char *ir, size_t ir_len,
	struct linkr_debugger_task_mutation_outcome *outcome)
{
	struct linkr_debugger_task_status status;

	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	return mutate_expected(opcode, id, source, source_len, ir, ir_len,
			       status.revision, false, outcome);
}

static void reset_arbiters(void)
{
	linkr_debugger_capture_arbiter_reset();
	linkr_debugger_flash_arbiter_reset();
}

static void mount_settings(bool erase)
{
	if (erase) {
		memset(&fake, 0, sizeof(fake));
	}
	fake.power_dead = false;
	fake.step_count = 0U;
	fake.cut_at_step = 0U;
	fake.write_log_count = 0U;
	fake.delete_log_count = 0U;
	reset_arbiters();
	linkr_debugger_task_test_set_backend(&fake_backend, &fake);
	linkr_debugger_task_init();
}

static size_t one_task_ir(char *out, size_t cap, const char *id, const char *value)
{
	int written = snprintf(out, cap,
		"# linkr-task.v1\n# task %s\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP7\","
		"\"body\":\"{\\\"value\\\":%s}\",\"wait_ms\":0}\n",
		id, value);

	assert(written > 0 && (size_t)written < cap);
	return (size_t)written;
}

static const struct linkr_debugger_task_summary *find_task(
	const struct linkr_debugger_task_status *status, const char *id)
{
	for (size_t i = 0U; i < status->task_count; i++) {
		if (strcmp(status->tasks[i].id, id) == 0) {
			return &status->tasks[i];
		}
	}
	return NULL;
}

static void take_store_snapshot(struct fake_store_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	assert(linkr_debugger_task_status_get(&snapshot->status) == LINKR_DEBUGGER_TASK_OK);
	assert(linkr_debugger_task_ir_snapshot(snapshot->ir, sizeof(snapshot->ir),
					      &snapshot->ir_len) == LINKR_DEBUGGER_TASK_OK);
	for (size_t i = 0U; i < snapshot->status.task_count; i++) {
		const struct linkr_debugger_task_summary *task = &snapshot->status.tasks[i];

		if (task->source_present) {
			assert(linkr_debugger_task_source_snapshot(
				task->slot, snapshot->source[task->slot],
				sizeof(snapshot->source[task->slot]),
				&snapshot->source_len[task->slot]) == LINKR_DEBUGGER_TASK_OK);
		}
	}
}

static void assert_store_snapshot(const struct fake_store_snapshot *expected)
{
	struct fake_store_snapshot actual;

	take_store_snapshot(&actual);
	assert(actual.status.backend_available == expected->status.backend_available);
	assert(memcmp(actual.status.task_store_id, expected->status.task_store_id,
		      sizeof(actual.status.task_store_id)) == 0);
	assert(actual.status.revision == expected->status.revision);
	assert(memcmp(actual.status.catalog_fingerprint,
		      expected->status.catalog_fingerprint,
		      sizeof(actual.status.catalog_fingerprint)) == 0);
	assert(actual.status.full_ir_len == expected->status.full_ir_len);
	assert(memcmp(actual.status.full_ir_digest, expected->status.full_ir_digest,
		      sizeof(actual.status.full_ir_digest)) == 0);
	assert(actual.status.task_count == expected->status.task_count);
	assert(actual.ir_len == expected->ir_len);
	assert(memcmp(actual.ir, expected->ir, actual.ir_len) == 0);
	for (size_t i = 0U; i < actual.status.task_count; i++) {
		const struct linkr_debugger_task_summary *got = &actual.status.tasks[i];
		const struct linkr_debugger_task_summary *want = &expected->status.tasks[i];

		assert(got->slot == want->slot);
		assert(strcmp(got->id, want->id) == 0);
		assert(got->request_count == want->request_count);
		assert(got->source_present == want->source_present);
		assert(got->source_len == want->source_len);
		assert(got->ir_len == want->ir_len);
		assert(memcmp(got->source_digest, want->source_digest,
			      sizeof(got->source_digest)) == 0);
		assert(memcmp(got->ir_digest, want->ir_digest,
			      sizeof(got->ir_digest)) == 0);
		assert(actual.source_len[got->slot] == expected->source_len[want->slot]);
		assert(memcmp(actual.source[got->slot], expected->source[want->slot],
			      actual.source_len[got->slot]) == 0);
	}
}

static void fill_source(uint8_t source[LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE], uint8_t ch)
{
	memset(source, ch, LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE);
	source[LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE - 1U] = '\n';
}

static void pad_ir_to_max(uint8_t blob[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE], size_t len)
{
	assert(len > 0U && len < LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE && blob[len - 1U] == '\n');
	while (len < LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE) {
		size_t remaining = LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE - len;
		size_t line = remaining > 200U ? 200U : remaining;

		if (line == 1U) {
			blob[len++] = '\n';
			continue;
		}
		blob[len++] = '#';
		memset(blob + len, 'p', line - 2U);
		len += line - 2U;
		blob[len++] = '\n';
	}
}

static void assert_manifest_commit_is_last_save(void)
{
	bool manifest_seen = false;

	for (size_t i = 0U; i < fake.write_log_count; i++) {
		if (strcmp(fake.write_log[i], TASK_KEY_MANIFEST_NAME) == 0) {
			manifest_seen = true;
			assert(i == fake.write_log_count - 1U);
		}
	}
	assert(manifest_seen);
}

static void test_sha256_vector(void)
{
	static const uint8_t expected[32] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
		0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
		0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
	};
	uint8_t actual[32];

	linkr_debugger_task_sha256_digest("abc", 3U, actual);
	assert(memcmp(actual, expected, sizeof(expected)) == 0);
}

static void test_initial_manifest_identity_survives_remount(void)
{
	struct linkr_debugger_task_status before;
	struct linkr_debugger_task_status after;
	uint8_t expected_empty[32];

	mount_settings(true);
	assert(linkr_debugger_task_status_get(&before) == LINKR_DEBUGGER_TASK_OK);
	assert(before.backend_available && before.revision == 0U && before.task_count == 0U);
	assert((before.task_store_id[6] & 0xf0U) == 0x40U);
	assert((before.task_store_id[8] & 0xc0U) == 0x80U);
	linkr_debugger_task_sha256_digest(NULL, 0U, expected_empty);
	assert(memcmp(before.full_ir_digest, expected_empty, sizeof(expected_empty)) == 0);
	assert(fake_find(&fake, TASK_KEY_MANIFEST_NAME) >= 0);
	mount_settings(false);
	assert(linkr_debugger_task_status_get(&after) == LINKR_DEBUGGER_TASK_OK);
	assert(memcmp(before.task_store_id, after.task_store_id,
		      sizeof(before.task_store_id)) == 0);
}

static void test_four_full_sources_and_bank_alternation(void)
{
	static const char *const ids[] = { "A", "B", "C", "D" };
	struct linkr_debugger_task_mutation_outcome outcome;
	struct linkr_debugger_task_status status;
	uint8_t source[LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE];
	uint8_t readback[LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE];
	uint8_t maximum_ir[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE];
	bool bank_written[LINKR_DEBUGGER_TASK_MAX_TASKS][2] = { { false } };
	char ir[256];
	size_t ir_len;
	size_t source_len;
	size_t maximum_ir_len;

	mount_settings(true);
	for (size_t i = 0U; i < ARRAY_SIZE(ids); i++) {
		fill_source(source, (uint8_t)('a' + i));
		ir_len = one_task_ir(ir, sizeof(ir), ids[i], "1");
		assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, ids[i], source,
			      sizeof(source), ir, ir_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
	}
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	assert(status.revision == 4U && status.task_count == 4U);
	for (size_t i = 0U; i < ARRAY_SIZE(ids); i++) {
		const struct linkr_debugger_task_summary *task = find_task(&status, ids[i]);

		assert(task != NULL && task->source_present &&
		       task->source_len == LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE);
		assert(linkr_debugger_task_source_snapshot(task->slot, readback,
			       sizeof(readback), &source_len) == LINKR_DEBUGGER_TASK_OK);
		assert(source_len == sizeof(source));
		assert(readback[0] == (uint8_t)('a' + i));
	}
	assert(linkr_debugger_task_ir_snapshot(maximum_ir, sizeof(maximum_ir),
					      &maximum_ir_len) == LINKR_DEBUGGER_TASK_OK);
	pad_ir_to_max(maximum_ir, maximum_ir_len);
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_IMPORT, NULL, NULL, 0U,
		      (char *)maximum_ir, LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE,
		      &outcome) == LINKR_DEBUGGER_TASK_OK);
	assert(outcome.status.full_ir_len == LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE);
	for (size_t i = 0U; i < ARRAY_SIZE(ids); i++) {
		const struct linkr_debugger_task_summary *task =
			find_task(&outcome.status, ids[i]);

		assert(task != NULL && task->source_present);
	}
	for (size_t round = 0U; round < 64U; round++) {
		size_t index = round % ARRAY_SIZE(ids);
		const struct linkr_debugger_task_summary *task;

		fake.write_log_count = 0U;
		fill_source(source, (uint8_t)('k' + index));
		ir_len = one_task_ir(ir, sizeof(ir), ids[index], (round & 1U) ? "0" : "1");
		assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, ids[index], source,
			      sizeof(source), ir, ir_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
		assert_manifest_commit_is_last_save();
		task = find_task(&outcome.status, ids[index]);
		assert(task != NULL);
		/* Both A/B banks of the affected slot are used over time and
		 * every data write is confined to that slot. */
		for (size_t i = 0U; i < fake.write_log_count; i++) {
			const char *name = fake.write_log[i];
			unsigned int slot = 99U;
			unsigned int bank = 99U;

			if (strcmp(name, TASK_KEY_MANIFEST_NAME) == 0) {
				continue;
			}
			if (strncmp(name, "linkr/task/ir/", 14U) == 0) {
				assert(sscanf(name + 14U, "%u/%u", &slot, &bank) == 2);
			} else {
				assert(strncmp(name, "linkr/task/src/", 15U) == 0);
				assert(sscanf(name + 15U, "%u/%u", &slot, &bank) == 2);
			}
			assert(slot == (unsigned int)task->slot);
			assert(bank < 2U);
			bank_written[slot][bank] = true;
		}
		if ((round % 16U) == 15U) {
			assert(linkr_debugger_task_ir_snapshot(maximum_ir, sizeof(maximum_ir),
						      &maximum_ir_len) == LINKR_DEBUGGER_TASK_OK);
			assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_DELETE, "D", NULL, 0U,
				      NULL, 0U, &outcome) == LINKR_DEBUGGER_TASK_OK);
			assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_IMPORT, NULL, NULL, 0U,
				      (char *)maximum_ir, maximum_ir_len,
				      &outcome) == LINKR_DEBUGGER_TASK_OK);
			fill_source(source, 'd');
			ir_len = one_task_ir(ir, sizeof(ir), "D", "1");
			assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "D", source,
				      sizeof(source), ir, ir_len,
				      &outcome) == LINKR_DEBUGGER_TASK_OK);
		}
	}
	for (size_t slot = 0U; slot < LINKR_DEBUGGER_TASK_MAX_TASKS; slot++) {
		assert(bank_written[slot][0] && bank_written[slot][1]);
	}
	take_store_snapshot(&new_snapshot);
	mount_settings(false);
	assert_store_snapshot(&new_snapshot);
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	assert(status.task_count == 4U && status.revision == 81U);
}

static void test_import_source_retention_delete_and_clear(void)
{
	struct linkr_debugger_task_mutation_outcome outcome;
	struct linkr_debugger_task_status status;
	uint8_t source[64];
	uint8_t store_id[LINKR_DEBUGGER_TASK_STORE_ID_SIZE];
	uint8_t raw[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE];
	char ir_a[256];
	char ir_b[256];
	char imported[768];
	size_t ir_a_len;
	size_t ir_b_len;
	size_t imported_len;
	size_t raw_len;
	uint8_t slot_a;
	const struct linkr_debugger_task_summary *task_a;
	const struct linkr_debugger_task_summary *task_b;

	mount_settings(true);
	memset(source, 's', sizeof(source));
	ir_a_len = one_task_ir(ir_a, sizeof(ir_a), "A", "1");
	ir_b_len = one_task_ir(ir_b, sizeof(ir_b), "B", "1");
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", source, sizeof(source),
		      ir_a, ir_a_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "B", source, sizeof(source),
		      ir_b, ir_b_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	memcpy(store_id, status.task_store_id, sizeof(store_id));
	task_a = find_task(&status, "A");
	assert(task_a != NULL);
	slot_a = task_a->slot;
	assert(linkr_debugger_task_ir_snapshot(raw, sizeof(raw), &raw_len) ==
	       LINKR_DEBUGGER_TASK_OK);
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_IMPORT, NULL, NULL, 0U,
		      (char *)raw, raw_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
	task_a = find_task(&outcome.status, "A");
	task_b = find_task(&outcome.status, "B");
	assert(task_a != NULL && task_a->source_present);
	assert(task_b != NULL && task_b->source_present);

	/* Full import keeps a source only when the task IR digest is
	 * unchanged: A survives byte-identically, B changes and loses it. */
	ir_b_len = one_task_ir(ir_b, sizeof(ir_b), "B", "0");
	imported_len = ir_a_len + ir_b_len - strlen("# linkr-task.v1\n");
	memcpy(imported, ir_a, ir_a_len);
	memcpy(imported + ir_a_len, ir_b + strlen("# linkr-task.v1\n"),
	       ir_b_len - strlen("# linkr-task.v1\n"));
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_IMPORT, NULL, NULL, 0U,
		      imported, imported_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
	task_a = find_task(&outcome.status, "A");
	task_b = find_task(&outcome.status, "B");
	assert(task_a != NULL && task_a->source_present && task_a->slot == slot_a);
	assert(task_b != NULL && !task_b->source_present);
	assert(linkr_debugger_task_ir_snapshot(raw, sizeof(raw), &raw_len) ==
	       LINKR_DEBUGGER_TASK_OK);
	assert(raw_len == imported_len && memcmp(raw, imported, raw_len) == 0);

	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_DELETE, "B", NULL, 0U,
		      NULL, 0U, &outcome) == LINKR_DEBUGGER_TASK_OK);
	assert(outcome.status.task_count == 1U && find_task(&outcome.status, "A") != NULL);
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_CLEAR, NULL, NULL, 0U,
		      NULL, 0U, &outcome) == LINKR_DEBUGGER_TASK_OK);
	assert(outcome.status.task_count == 0U && outcome.status.full_ir_len == 0U);
	assert(memcmp(store_id, outcome.status.task_store_id, sizeof(store_id)) == 0);
}

static void test_raw_import_export_is_lossless(void)
{
	struct linkr_debugger_task_mutation_outcome outcome;
	uint8_t raw[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE];
	char imported[1024];
	size_t raw_len;
	int written;

	mount_settings(true);
	/* Interleaved comments inside section spans stay byte-exact. */
	written = snprintf(imported, sizeof(imported),
		"# linkr-task.v1\n# task A\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP7\","
		"\"body\":\"{\\\"value\\\":1}\",\"wait_ms\":0}\n"
		"# trailing A comment\n"
		"# task B\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP7\","
		"\"body\":\"{\\\"value\\\":1}\",\"wait_ms\":0}\n");
	assert(written > 0 && (size_t)written < sizeof(imported));
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_IMPORT, NULL, NULL, 0U,
		      imported, (size_t)written, &outcome) == LINKR_DEBUGGER_TASK_OK);
	assert(linkr_debugger_task_ir_snapshot(raw, sizeof(raw), &raw_len) ==
	       LINKR_DEBUGGER_TASK_OK);
	assert(raw_len == (size_t)written && memcmp(raw, imported, raw_len) == 0);
	mount_settings(false);
	assert(linkr_debugger_task_ir_snapshot(raw, sizeof(raw), &raw_len) ==
	       LINKR_DEBUGGER_TASK_OK);
	assert(raw_len == (size_t)written && memcmp(raw, imported, raw_len) == 0);

	/* Reordering survives import: export order is the imported IR order. */
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_IMPORT, NULL, NULL, 0U,
		      (char *)raw, raw_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
	assert(linkr_debugger_task_ir_snapshot(raw, sizeof(raw), &raw_len) ==
	       LINKR_DEBUGGER_TASK_OK);
	assert(memcmp(raw, imported, raw_len) == 0);
}

static void test_json_contract_is_strict_and_lossless(void)
{
	struct linkr_debugger_task_mutation_outcome outcome;
	struct linkr_debugger_task_status status;
	enum linkr_debugger_task_result result;
	static uint8_t body[LINKR_DEBUGGER_TASK_MUTATION_MAX_SIZE];
	uint8_t readback[LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE];
	static const uint8_t expected_source[] =
		"line1\nline2\t\"q\"\\\r\xe4\xb8\xad\xe6\x96\x87";
	char catalog[2U * LINKR_DEBUGGER_TASK_SHA256_SIZE + 1U];
	char expected_json[1024];
	size_t readback_len;
	unsigned int saves;
	int written;

	mount_settings(true);
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	for (size_t i = 0U; i < LINKR_DEBUGGER_TASK_SHA256_SIZE; i++) {
		(void)snprintf(catalog + 2U * i, 3U, "%02x",
			       status.catalog_fingerprint[i]);
	}

	/* Golden body: stable field order and minimal escaping. */
	{
		size_t len = encode_mutation_json(LINKR_DEBUGGER_TASK_MUTATION_DELETE,
			&status, "A", NULL, 0U, NULL, 0U, 0U, false, body,
			sizeof(body));

		written = snprintf(expected_json, sizeof(expected_json),
			"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"delete\","
			"\"expected_revision\":\"0\",\"catalog_fingerprint\":\"%s\","
			"\"id\":\"A\"}", catalog);
		assert(written > 0);
		assert(len == strlen(expected_json));
		assert(memcmp(body, expected_json, len) == 0);
	}

	/* Canonical base64 roundtrip: the fixed source_base64/ir_base64
	 * fields carry exact source/IR bytes (quotes, backslashes, newlines,
	 * tabs, CR and multi-byte UTF-8) in a bounded body. */
	{
		static const char golden_body[] =
			"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"upsert\","
			"\"expected_revision\":\"0\",\"catalog_fingerprint\":\"%s\","
			"\"id\":\"A\","
			"\"source_base64\":\"bGluZTEKbGluZTIJInEiXA3kuK3mloc=\","
			"\"ir_base64\":\"IyBsaW5rci10YXNrLnYxCiMgdGFzayBBCnsibWV0aG9kIjoi"
			"UFVUIiwicGF0aCI6Ii9hcGkvdjEvZ3Bpby9HUDciLCJib2R5Ijoie1widmFsdWVc"
			"IjoxfSIsIndhaXRfbXMiOjB9Cg==\"}";
		static const char expected_ir[] =
			"# linkr-task.v1\n# task A\n"
			"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP7\","
			"\"body\":\"{\\\"value\\\":1}\",\"wait_ms\":0}\n";
		static char golden[LINKR_DEBUGGER_TASK_MUTATION_MAX_SIZE];
		size_t len;
		size_t encoded_len;

		len = (size_t)snprintf(golden, sizeof(golden), golden_body, catalog);
		assert(len < sizeof(golden));
		encoded_len = encode_mutation_json(LINKR_DEBUGGER_TASK_MUTATION_UPSERT,
			&status, "A", expected_source, sizeof(expected_source) - 1U,
			expected_ir, sizeof(expected_ir) - 1U, 0U, false, body,
			sizeof(body));
		assert(encoded_len == len);
		assert(memcmp(body, golden, len) == 0);
		assert(linkr_debugger_task_mutation_execute(body, len, &outcome, NULL) ==
		       LINKR_DEBUGGER_TASK_OK);
		assert(linkr_debugger_task_source_snapshot(outcome.affected_slot, readback,
			       sizeof(readback), &readback_len) == LINKR_DEBUGGER_TASK_OK);
		assert(readback_len == sizeof(expected_source) - 1U);
		assert(memcmp(readback, expected_source, readback_len) == 0);
		assert(linkr_debugger_task_ir_snapshot(readback, sizeof(readback),
						      &readback_len) == LINKR_DEBUGGER_TASK_OK);
		assert(readback_len == sizeof(expected_ir) - 1U);
		assert(memcmp(readback, expected_ir, readback_len) == 0);
	}

	/* Strict field handling: unknown, duplicate, missing and malformed
	 * members never reach the store. */
	saves = fake.save_calls;
	{
		static const char *const bad_bodies[] = {
			"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"clear\","
			"\"expected_revision\":\"1\",\"catalog_fingerprint\":\"00"
			"0000000000000000000000000000000000000000000000000000000000000\","
			"\"extra\":\"x\"}",
			"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"clear\","
			"\"action\":\"clear\",\"expected_revision\":\"1\","
			"\"catalog_fingerprint\":\"00000000000000000000000000000000"
			"00000000000000000000000000000000\"}",
			"{\"action\":\"clear\"}",
			"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"clear\","
			"\"expected_revision\":\"00\",\"catalog_fingerprint\":\"00000000"
			"00000000000000000000000000000000000000000000000000000000\"}",
			"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"clear\","
			"\"expected_revision\":\"1\",\"catalog_fingerprint\":\"00000000"
			"0000000000000000000000000000000000000000000000000000000\"}",
			"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"delete\","
			"\"expected_revision\":\"1\",\"catalog_fingerprint\":\"00000000"
			"00000000000000000000000000000000000000000000000000000000\"}",
			"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"import\","
			"\"expected_revision\":\"1\",\"catalog_fingerprint\":\"00000000"
			"00000000000000000000000000000000000000000000000000000000\","
			"\"id\":\"A\",\"ir\":\"# linkr-task.v1\\n\"}",
			"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"clear\","
			"\"expected_revision\":\"1\"}",
			"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"clear\","
			"\"expected_revision\":\"1\",\"catalog_fingerprint\":\"00000000"
			"00000000000000000000000000000000000000000000000000000000\"} "
			"trailing",
			"[]",
			"",
		};

		for (size_t i = 0U; i < ARRAY_SIZE(bad_bodies); i++) {
			size_t len = strlen(bad_bodies[i]);

			memcpy(body, bad_bodies[i], len);
			result = linkr_debugger_task_mutation_execute(body, len, &outcome,
								     NULL);
			assert(result == LINKR_DEBUGGER_TASK_INVALID_MUTATION);
		}
	}
	assert(fake.save_calls == saves);

	/* Strict canonical base64: bad padding placement, excess padding,
	 * non-zero pad bits, missing padding, whitespace, JSON escapes and
	 * foreign alphabets never reach the store, in either field. */
	{
		static const char *const bad_base64[] = {
			"AA=A", "A===", "AB==", "AAA", "AA A",
			"AA\\u0041A==", "-_AA", "AA+/=",
		};
		static const char source_case[] =
			"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"upsert\","
			"\"expected_revision\":\"1\",\"catalog_fingerprint\":\"%s\","
			"\"id\":\"A\",\"source_base64\":\"%s\"}";
		static const char ir_case[] =
			"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"import\","
			"\"expected_revision\":\"1\",\"catalog_fingerprint\":\"%s\","
			"\"ir_base64\":\"%s\"}";
		size_t len;

		for (size_t i = 0U; i < ARRAY_SIZE(bad_base64); i++) {
			len = (size_t)snprintf((char *)body, sizeof(body), source_case,
					       catalog, bad_base64[i]);
			assert(len < sizeof(body));
			assert(linkr_debugger_task_mutation_execute(body, len, &outcome,
								   NULL) ==
			       LINKR_DEBUGGER_TASK_INVALID_MUTATION);
			len = (size_t)snprintf((char *)body, sizeof(body), ir_case,
					       catalog, bad_base64[i]);
			assert(len < sizeof(body));
			assert(linkr_debugger_task_mutation_execute(body, len, &outcome,
								   NULL) ==
			       LINKR_DEBUGGER_TASK_INVALID_MUTATION);
		}
		assert(fake.save_calls == saves);
	}

	/* A decoded source beyond 4000 bytes is rejected before any write,
	 * whether caught by decoded or by encoded length. */
	{
		static uint8_t big[LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE + 3U];
		char ir[256];
		size_t ir_len;

		ir_len = one_task_ir(ir, sizeof(ir), "A", "1");
		memset(big, 'x', sizeof(big));
		for (size_t extra = 1U; extra <= 3U; extra++) {
			assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", big,
				      LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE + extra,
				      ir, ir_len, &outcome) ==
			       LINKR_DEBUGGER_TASK_BODY_TOO_LARGE);
			assert(fake.save_calls == saves);
		}
	}

	/* Random binary-like source decodes from base64 but still violates
	 * the strict UTF-8/control source policy and never reaches the
	 * store; exact bytes only round-trip inside that policy. */
	{
		uint8_t binary_source[64];
		char ir[256];
		size_t ir_len;

		for (size_t i = 0U; i < sizeof(binary_source); i++) {
			binary_source[i] = (uint8_t)(i * 37U + 11U);
		}
		ir_len = one_task_ir(ir, sizeof(ir), "A", "1");
		assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", binary_source,
			      sizeof(binary_source), ir, ir_len, &outcome) ==
		       LINKR_DEBUGGER_TASK_INVALID_TASK);
		assert(fake.save_calls == saves);
	}
}

static void test_worst_case_base64_payloads_fit_bounded_body(void)
{
	struct linkr_debugger_task_mutation_outcome outcome;
	struct linkr_debugger_task_status status;
	static uint8_t worst_source[LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE];
	static uint8_t worst_ir[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE];
	uint8_t readback[LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE];
	uint8_t ir_readback[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE];
	/* Old JSON escaping worst case: quote, backslash and multi-byte
	 * UTF-8 in one 8-byte unit, repeated to exactly 4000 bytes. */
	static const uint8_t unit[8] = {
		'\\', '"', 0xe4U, 0xb8U, 0xadU, 0xe6U, 0x96U, 0x87U,
	};
	size_t ir_len;
	size_t len;
	size_t readback_len;

	mount_settings(true);
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	for (size_t i = 0U; i < sizeof(worst_source); i++) {
		worst_source[i] = unit[i % sizeof(unit)];
	}
	ir_len = one_task_ir((char *)worst_ir, sizeof(worst_ir), "A", "1");
	pad_ir_to_max(worst_ir, ir_len);
	ir_len = LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE;
	len = encode_mutation_json(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, &status, "A",
		worst_source, sizeof(worst_source), (const char *)worst_ir, ir_len,
		status.revision, false, mutation_body, sizeof(mutation_body));
	/* Two maximum canonical base64 payloads plus the fixed envelope
	 * keys fit one bounded mutation body. */
	assert(len > 2U * TASK_TEST_B64_PAYLOAD_MAX);
	assert(len <= LINKR_DEBUGGER_TASK_MUTATION_MAX_SIZE);
	assert(linkr_debugger_task_mutation_execute(mutation_body, len, &outcome, NULL) ==
	       LINKR_DEBUGGER_TASK_OK);
	assert(linkr_debugger_task_source_snapshot(outcome.affected_slot, readback,
		       sizeof(readback), &readback_len) == LINKR_DEBUGGER_TASK_OK);
	assert(readback_len == sizeof(worst_source));
	assert(memcmp(readback, worst_source, readback_len) == 0);
	assert(linkr_debugger_task_ir_snapshot(ir_readback, sizeof(ir_readback),
					      &readback_len) == LINKR_DEBUGGER_TASK_OK);
	assert(readback_len == ir_len);
	assert(memcmp(ir_readback, worst_ir, readback_len) == 0);
}

static void test_conflicts_overflow_and_validation_write_nothing(void)
{
	struct linkr_debugger_task_mutation_outcome outcome;
	struct linkr_debugger_task_status status;
	uint8_t source[] = { 'o', 'k', '\n' };
	uint8_t invalid_source[] = { 'x', 0x01U, '\n' };
	uint8_t invalid_utf8_source[] = { 0xc0U, 0x80U };
	char ir[256];
	char bad_ir[256];
	size_t ir_len;
	unsigned int saves;

	mount_settings(true);
	ir_len = one_task_ir(ir, sizeof(ir), "A", "1");
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	saves = fake.save_calls;
	assert(mutate_expected(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", source,
		       sizeof(source), ir, ir_len, status.revision + 1U, false,
		       &outcome) == LINKR_DEBUGGER_TASK_REVISION_CONFLICT);
	assert(fake.save_calls == saves);
	assert(mutate_expected(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", source,
		       sizeof(source), ir, ir_len, status.revision, true,
		       &outcome) == LINKR_DEBUGGER_TASK_CATALOG_CONFLICT);
	assert(fake.save_calls == saves);
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", invalid_source,
		      sizeof(invalid_source), ir, ir_len, &outcome) ==
	       LINKR_DEBUGGER_TASK_INVALID_TASK);
	assert(fake.save_calls == saves);
	/* Invalid UTF-8 travels as base64 but violates the strict source
	 * byte policy after decoding. */
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", invalid_utf8_source,
		      sizeof(invalid_utf8_source), ir, ir_len, &outcome) ==
	       LINKR_DEBUGGER_TASK_INVALID_TASK);
	assert(fake.save_calls == saves);
	memcpy(bad_ir, ir, ir_len);
	bad_ir[0] = '\r';
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", source,
		      sizeof(source), bad_ir, ir_len, &outcome) ==
	       LINKR_DEBUGGER_TASK_INVALID_TASK);
	assert(fake.save_calls == saves);
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "different", source,
		      sizeof(source), ir, ir_len, &outcome) ==
	       LINKR_DEBUGGER_TASK_INVALID_TASK);
	assert(fake.save_calls == saves);
	/* Import requires canonical framing: no preamble before the first task. */
	{
		char preamble[512];
		int written = snprintf(preamble, sizeof(preamble),
				       "# lead comment\n%s", ir);

		assert(written > 0);
		assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_IMPORT, NULL, NULL, 0U,
			      preamble, (size_t)written, &outcome) ==
		       LINKR_DEBUGGER_TASK_INVALID_TASK);
		assert(fake.save_calls == saves);
	}

	linkr_debugger_task_test_force_revision(UINT64_MAX);
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_CLEAR, NULL, NULL, 0U, NULL, 0U,
		      &outcome) == LINKR_DEBUGGER_TASK_INVALID_MUTATION);
	assert(fake.save_calls == saves);
}

static void test_enospc_and_readback_failure_do_not_commit(void)
{
	struct linkr_debugger_task_mutation_outcome outcome;
	struct linkr_debugger_task_status before;
	struct linkr_debugger_task_status after;
	uint8_t source[64];
	char ir[256];
	size_t ir_len;

	mount_settings(true);
	memset(source, 'x', sizeof(source));
	ir_len = one_task_ir(ir, sizeof(ir), "A", "1");
	assert(linkr_debugger_task_status_get(&before) == LINKR_DEBUGGER_TASK_OK);
	fake.force_enospc = true;
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", source, sizeof(source),
		      ir, ir_len, &outcome) == LINKR_DEBUGGER_TASK_STORAGE_FULL);
	assert(linkr_debugger_task_status_get(&after) == LINKR_DEBUGGER_TASK_OK);
	assert(after.revision == before.revision && after.task_count == 0U);
	fake.corrupt_next_read = true;
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", source, sizeof(source),
		      ir, ir_len, &outcome) == LINKR_DEBUGGER_TASK_READBACK_FAILED);
	assert(linkr_debugger_task_status_get(&after) == LINKR_DEBUGGER_TASK_OK);
	assert(after.revision == before.revision && after.task_count == 0U);
	fake.corrupt_next_read = true;
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", NULL, 0U,
		      ir, ir_len, &outcome) == LINKR_DEBUGGER_TASK_READBACK_FAILED);
	assert(linkr_debugger_task_status_get(&after) == LINKR_DEBUGGER_TASK_OK);
	assert(after.revision == before.revision && after.task_count == 0U);
	mount_settings(false);
	assert(linkr_debugger_task_status_get(&after) == LINKR_DEBUGGER_TASK_OK);
	assert(after.revision == before.revision && after.task_count == 0U);
}

static void test_settings_power_loss_ordering_and_atomicity(void)
{
	struct linkr_debugger_task_mutation_outcome outcome;
	uint8_t source[LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE];
	char ir[256];
	char ir_new[256];
	char imported[768];
	char ir_a[256];
	char ir_b[256];
	size_t ir_len;
	size_t ir_new_len;
	size_t ir_a_len;
	size_t ir_b_len;
	size_t imported_len;
	unsigned int total_steps;

	mount_settings(true);
	fill_source(source, 'a');
	ir_len = one_task_ir(ir, sizeof(ir), "A", "1");
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", source, sizeof(source),
		      ir, ir_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
	ir_b_len = one_task_ir(ir_b, sizeof(ir_b), "B", "1");
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "B", NULL, 0U,
		      ir_b, ir_b_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
	take_store_snapshot(&old_snapshot);
	baseline = fake;

	/* Ordering: source key first, then the IR key, then the manifest. */
	fake.write_log_count = 0U;
	fake.step_count = 0U;
	fill_source(source, 'z');
	ir_new_len = one_task_ir(ir_new, sizeof(ir_new), "A", "0");
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", source, sizeof(source),
		      ir_new, ir_new_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
	assert_manifest_commit_is_last_save();
	assert(fake.write_log_count == 3U);
	assert(strncmp(fake.write_log[0], "linkr/task/src/", 15U) == 0);
	assert(strncmp(fake.write_log[1], "linkr/task/ir/", 14U) == 0);
	assert(strcmp(fake.write_log[2], TASK_KEY_MANIFEST_NAME) == 0);
	total_steps = fake.step_count;
	assert(total_steps == 3U);
	take_store_snapshot(&new_snapshot);

	/* Power loss at every write step leaves exactly the old or exactly the
	 * new state: source/IR keys never clobber the live manifest's banks. */
	for (unsigned int cut = 1U; cut <= total_steps; cut++) {
		fake = baseline;
		mount_settings(false);
		fake.step_count = 0U;
		fake.cut_at_step = cut;
		fill_source(source, 'z');
		(void)mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", source,
			     sizeof(source), ir_new, ir_new_len, &outcome);
		fake.cut_at_step = 0U;
		mount_settings(false);
		assert(linkr_debugger_task_status_get(&outcome.status) ==
		       LINKR_DEBUGGER_TASK_OK);
		assert(outcome.status.backend_available);
		if (outcome.status.revision == old_snapshot.status.revision) {
			assert_store_snapshot(&old_snapshot);
		} else {
			assert_store_snapshot(&new_snapshot);
		}
	}

	/* Multi-key import writes every IR key before the manifest as well. */
	fake = baseline;
	mount_settings(false);
	fake.write_log_count = 0U;
	fake.delete_log_count = 0U;
	fake.step_count = 0U;
	ir_a_len = one_task_ir(ir_a, sizeof(ir_a), "A", "0");
	imported_len = ir_a_len + ir_b_len - strlen("# linkr-task.v1\n");
	memcpy(imported, ir_a, ir_a_len);
	memcpy(imported + ir_a_len, ir_b + strlen("# linkr-task.v1\n"),
	       ir_b_len - strlen("# linkr-task.v1\n"));
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_IMPORT, NULL, NULL, 0U,
		      imported, imported_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
	assert_manifest_commit_is_last_save();
	assert(fake.write_log_count == 3U);
	assert(strncmp(fake.write_log[0], "linkr/task/ir/", 14U) == 0);
	assert(strncmp(fake.write_log[1], "linkr/task/ir/", 14U) == 0);
	/* A's source is dropped (its IR digest changed) and cleaned up only
	 * after the manifest commit. */
	assert(fake.delete_log_count == 1U);
	assert(strcmp(fake.delete_log[0], "linkr/task/src/0/1") == 0);
	total_steps = fake.step_count;
	take_store_snapshot(&new_snapshot);
	for (unsigned int cut = 1U; cut <= total_steps; cut++) {
		fake = baseline;
		mount_settings(false);
		fake.step_count = 0U;
		fake.cut_at_step = cut;
		(void)mutate(LINKR_DEBUGGER_TASK_MUTATION_IMPORT, NULL, NULL, 0U,
			     imported, imported_len, &outcome);
		fake.cut_at_step = 0U;
		mount_settings(false);
		assert(linkr_debugger_task_status_get(&outcome.status) ==
		       LINKR_DEBUGGER_TASK_OK);
		assert(outcome.status.backend_available);
		if (outcome.status.revision == old_snapshot.status.revision) {
			assert_store_snapshot(&old_snapshot);
		} else {
			assert_store_snapshot(&new_snapshot);
		}
	}
}

static void test_import_new_ids_preserves_committed_banks(void)
{
	struct linkr_debugger_task_mutation_outcome outcome;
	char old_ir[256], new_ir[256];
	size_t old_len = one_task_ir(old_ir, sizeof(old_ir), "A", "1");
	size_t new_len = one_task_ir(new_ir, sizeof(new_ir), "C", "0");
	const uint8_t source[] = "old source";

	for (unsigned int saves = 1U; saves <= 2U; saves++) {
		mount_settings(true);
		for (unsigned int i = 0U; i < saves; i++) {
			assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A",
				source, sizeof(source) - 1U, old_ir, old_len, &outcome) ==
			       LINKR_DEBUGGER_TASK_OK);
		}
		take_store_snapshot(&old_snapshot);
		baseline = fake;
		/* Fail the IR write, its readback, or the manifest commit. */
		for (unsigned int attempt = 0U; attempt < 6U; attempt++) {
			unsigned int failure = (attempt + 2U) % 3U;

			fake = baseline;
			mount_settings(false);
			fake.step_count = 0U;
			fake.cut_at_step = failure == 1U ? 0U : (failure == 0U ? 1U : 2U);
			fake.corrupt_next_read = failure == 1U;
			assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_IMPORT, NULL, NULL, 0U,
				new_ir, new_len, &outcome) == (failure == 1U ?
				LINKR_DEBUGGER_TASK_READBACK_FAILED : LINKR_DEBUGGER_TASK_STORAGE_ERROR));
			fake.cut_at_step = 0U;
			fake.power_dead = false;
			if (attempt < 3U) {
				mount_settings(false);
			}
			assert_store_snapshot(&old_snapshot);
			/* Retry still chooses the inactive bank, then deletes old sources. */
			assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_IMPORT, NULL, NULL, 0U,
				new_ir, new_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
			take_store_snapshot(&new_snapshot);
			assert(new_snapshot.status.task_count == 1U);
			assert(strcmp(new_snapshot.status.tasks[0].id, "C") == 0);
			assert(fake_find(&fake, "linkr/task/src/0/0") < 0);
			assert(fake_find(&fake, "linkr/task/src/0/1") < 0);
			mount_settings(false);
			assert_store_snapshot(&new_snapshot);
		}
	}
}

static void test_no_overwrite_siblings(void)
{
	struct linkr_debugger_task_mutation_outcome outcome;
	struct fake_settings before_state;
	uint8_t source[LINKR_DEBUGGER_TASK_MAX_SOURCE_SIZE];
	char ir[256];
	size_t ir_len;

	mount_settings(true);
	fill_source(source, 'a');
	ir_len = one_task_ir(ir, sizeof(ir), "A", "1");
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", source, sizeof(source),
		      ir, ir_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
	ir_len = one_task_ir(ir, sizeof(ir), "B", "1");
	fill_source(source, 'b');
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "B", source, sizeof(source),
		      ir, ir_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
	before_state = fake;

	fake.write_log_count = 0U;
	ir_len = one_task_ir(ir, sizeof(ir), "A", "0");
	fill_source(source, 'z');
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", source, sizeof(source),
		      ir, ir_len, &outcome) == LINKR_DEBUGGER_TASK_OK);
	assert_manifest_commit_is_last_save();
	assert(fake.write_log_count == 3U);
	for (size_t i = 0U; i < fake.write_log_count; i++) {
		const char *name = fake.write_log[i];

		if (strcmp(name, TASK_KEY_MANIFEST_NAME) == 0) {
			continue;
		}
		/* Data writes stay inside slot 0 and only create the A/B bank
		 * keys the live manifest does not reference: no existing data
		 * key is ever overwritten before the manifest commit. */
		assert(strstr(name, "/0/") != NULL);
		assert(fake_find(&before_state, name) < 0);
	}
	/* Sibling keys and every other live key are byte-identical. */
	for (size_t i = 0U; i < FAKE_KEY_COUNT; i++) {
		bool written = false;
		const char *name = fake.entries[i].present ?
			fake.names[i] : before_state.names[i];

		if (!before_state.entries[i].present && !fake.entries[i].present) {
			continue;
		}
		for (size_t w = 0U; w < fake.write_log_count; w++) {
			if (strcmp(name, fake.write_log[w]) == 0) {
				written = true;
			}
		}
		if (!written) {
			assert(fake.entries[i].present == before_state.entries[i].present);
			assert(fake.entries[i].len == before_state.entries[i].len);
			assert(memcmp(fake.entries[i].data, before_state.entries[i].data,
				      before_state.entries[i].len) == 0);
		}
	}
}

struct concurrent_mutation {
	atomic_uint *ready;
	atomic_bool *go;
	char id[2];
	enum linkr_debugger_task_result result;
};

static void *concurrent_writer(void *context)
{
	struct concurrent_mutation *job = context;
	struct linkr_debugger_task_mutation_outcome outcome;
	struct linkr_debugger_task_status status;
	uint8_t body[1024];
	char ir[256];
	size_t ir_len;
	size_t body_len;

	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	ir_len = one_task_ir(ir, sizeof(ir), job->id, "1");
	body_len = encode_mutation_json(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, &status,
		job->id, NULL, 0U, ir, ir_len, status.revision, false, body,
		sizeof(body));
	atomic_fetch_add(job->ready, 1U);
	while (!atomic_load(job->go)) {
	}
	job->result = linkr_debugger_task_mutation_execute(body, body_len, &outcome, NULL);
	return NULL;
}

static void test_concurrent_same_revision_has_one_linearized_winner(void)
{
	atomic_uint ready = 0U;
	atomic_bool go = false;
	struct concurrent_mutation jobs[2] = {
		{ .ready = &ready, .go = &go, .id = "A" },
		{ .ready = &ready, .go = &go, .id = "B" },
	};
	struct linkr_debugger_task_status status;
	pthread_t threads[2];
	unsigned int winners = 0U;

	mount_settings(true);
	for (size_t i = 0U; i < ARRAY_SIZE(threads); i++) {
		assert(pthread_create(&threads[i], NULL, concurrent_writer, &jobs[i]) == 0);
	}
	while (atomic_load(&ready) != ARRAY_SIZE(threads)) {
	}
	atomic_store(&go, true);
	for (size_t i = 0U; i < ARRAY_SIZE(threads); i++) {
		assert(pthread_join(threads[i], NULL) == 0);
		if (jobs[i].result == LINKR_DEBUGGER_TASK_OK) {
			winners++;
		} else {
			assert(jobs[i].result == LINKR_DEBUGGER_TASK_BUSY ||
			       jobs[i].result == LINKR_DEBUGGER_TASK_REVISION_CONFLICT);
		}
	}
	assert(winners == 1U);
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	assert(status.revision == 1U && status.task_count == 1U);
}

static void test_storage_usage_and_save_gate(void)
{
	struct linkr_debugger_task_mutation_outcome outcome;
	struct linkr_debugger_task_status status;
	uint8_t source[] = { 'o', 'k', '\n' };
	char ir[256];
	size_t ir_len;

	mount_settings(true);
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	assert(status.storage.measured);
	assert(status.storage.total_bytes == 128U * 1024U);
	assert(status.storage.used_bytes + status.storage.free_bytes ==
	       status.storage.total_bytes);
	assert(status.storage.task_bytes > 0U);
	assert(status.storage.save_allowed);

	ir_len = one_task_ir(ir, sizeof(ir), "A", "1");
	linkr_debugger_task_test_set_storage_free(0);
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", source,
		      sizeof(source), ir, ir_len, &outcome) ==
	       LINKR_DEBUGGER_TASK_STORAGE_FULL);
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	assert(status.revision == 0U && status.task_count == 0U);
	assert(!status.storage.save_allowed);

	linkr_debugger_task_test_set_storage_free(-1);
	assert(mutate(LINKR_DEBUGGER_TASK_MUTATION_UPSERT, "A", source,
		      sizeof(source), ir, ir_len, &outcome) ==
	       LINKR_DEBUGGER_TASK_OK);
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	assert(status.revision == 1U && status.task_count == 1U);
	assert(status.storage.task_bytes > 0U);
}

int main(void)
{
	test_sha256_vector();
	test_initial_manifest_identity_survives_remount();
	test_four_full_sources_and_bank_alternation();
	test_import_source_retention_delete_and_clear();
	test_raw_import_export_is_lossless();
	test_json_contract_is_strict_and_lossless();
	test_worst_case_base64_payloads_fit_bounded_body();
	test_conflicts_overflow_and_validation_write_nothing();
	test_enospc_and_readback_failure_do_not_commit();
	test_settings_power_loss_ordering_and_atomicity();
	test_import_new_ids_preserves_committed_banks();
	test_no_overwrite_siblings();
	test_concurrent_same_revision_has_one_linearized_winner();
	test_storage_usage_and_save_gate();
	puts("linkr_debugger_task: all tests passed");
	return 0;
}
