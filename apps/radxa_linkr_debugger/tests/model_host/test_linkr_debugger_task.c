/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "../../src/linkr_debugger_task.h"
#include "../../src/linkr_debugger_task_parse.h"
#include "../../src/linkr_debugger_json_value.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#define TASK_BLOB_VALID \
	"# linkr-task.v1\n" \
	"# task recovery\n" \
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":0}\n" \
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/12v_out\",\"body\":\"{\\\"state\\\":\\\"off\\\"}\",\"wait_ms\":60000}\n"

#define TASK_BLOB_ALTERNATE \
	"# linkr-task.v1\n" \
	"# task alternate\n" \
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP11\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":1}\n"

struct fake_backend {
	int load_result;
	int save_result;
	int delete_result;
	bool tasks_present;
	char tasks_value[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
	size_t tasks_length;
	unsigned int load_calls;
	unsigned int unexpected_key_calls;
};

static int fake_load_one(void *context, const char *name, void *value, size_t value_size)
{
	struct fake_backend *backend = context;

	backend->load_calls++;
	if (backend->load_result < 0) {
		return backend->load_result;
	}
	if (strcmp(name, LINKR_DEBUGGER_TASK_TASKS_KEY) != 0) {
		backend->unexpected_key_calls++;
		return -ENOENT;
	}
	if (!backend->tasks_present) {
		return 0;
	}
	assert(backend->tasks_length <= value_size);
	memcpy(value, backend->tasks_value, backend->tasks_length);
	return (int)backend->tasks_length;
}

static int fake_save_one(void *context, const char *name, const void *value, size_t value_size)
{
	struct fake_backend *backend = context;

	if (strcmp(name, LINKR_DEBUGGER_TASK_TASKS_KEY) != 0) {
		backend->unexpected_key_calls++;
		return -EINVAL;
	}
	if (backend->save_result < 0) {
		return backend->save_result;
	}
	assert(value_size <= sizeof(backend->tasks_value));
	memcpy(backend->tasks_value, value, value_size);
	backend->tasks_length = value_size;
	backend->tasks_present = true;
	return 0;
}

static int fake_delete_one(void *context, const char *name)
{
	struct fake_backend *backend = context;

	if (strcmp(name, LINKR_DEBUGGER_TASK_TASKS_KEY) != 0) {
		backend->unexpected_key_calls++;
		return -EINVAL;
	}
	if (backend->delete_result < 0) {
		return backend->delete_result;
	}
	backend->tasks_present = false;
	backend->tasks_length = 0U;
	return 0;
}

static const struct linkr_debugger_task_backend_ops fake_ops = {
	.load_one = fake_load_one,
	.save_one = fake_save_one,
	.delete_one = fake_delete_one,
};

static void init_with_backend(struct fake_backend *backend)
{
	linkr_debugger_task_test_set_backend(&fake_ops, backend);
	linkr_debugger_task_init();
}

static void test_store_list_and_clear(void)
{
	struct fake_backend backend = { 0 };
	struct linkr_debugger_task_status status;
	char blob[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
	size_t blob_len;

	init_with_backend(&backend);
	assert(linkr_debugger_task_tasks_store(TASK_BLOB_VALID, strlen(TASK_BLOB_VALID)) ==
	       LINKR_DEBUGGER_TASK_OK);
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	assert(status.backend_available);
	assert(status.task_count == 1U);
	assert(strcmp(status.tasks[0].id, "recovery") == 0);
	assert(status.tasks[0].request_count == 2U);
	assert(linkr_debugger_task_blob_snapshot(blob, sizeof(blob), &blob_len) ==
	       LINKR_DEBUGGER_TASK_OK);
	assert(blob_len == strlen(TASK_BLOB_VALID));
	assert(memcmp(blob, TASK_BLOB_VALID, blob_len) == 0);
	assert(linkr_debugger_task_tasks_clear() == LINKR_DEBUGGER_TASK_OK);
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	assert(status.task_count == 0U);
	assert(linkr_debugger_task_blob_snapshot(blob, sizeof(blob), &blob_len) ==
	       LINKR_DEBUGGER_TASK_OK);
	assert(blob_len == 0U);
	assert(backend.unexpected_key_calls == 0U);
}

static void test_init_uses_only_task_storage(void)
{
	struct fake_backend backend = { 0 };
	struct linkr_debugger_task_status status;

	memcpy(backend.tasks_value, TASK_BLOB_VALID, strlen(TASK_BLOB_VALID));
	backend.tasks_length = strlen(TASK_BLOB_VALID);
	backend.tasks_present = true;
	init_with_backend(&backend);

	assert(backend.load_calls == 1U);
	assert(backend.unexpected_key_calls == 0U);
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	assert(status.task_count == 1U);
}

static void test_rejects_malformed_records_and_waits(void)
{
	struct fake_backend backend = { 0 };
	const char *non_put =
		"# linkr-task.v1\n# task invalid\n"
		"{\"method\":\"POST\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\",\"wait_ms\":0}\n";
	const char *disallowed_path =
		"# linkr-task.v1\n# task invalid\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/serial\",\"body\":\"{}\",\"wait_ms\":0}\n";
	const char *oversized_wait =
		"# linkr-task.v1\n# task invalid\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\",\"wait_ms\":60001}\n";

	init_with_backend(&backend);
	assert(linkr_debugger_task_tasks_store("", 0U) == LINKR_DEBUGGER_TASK_INVALID_BLOB);
	assert(linkr_debugger_task_tasks_store(non_put, strlen(non_put)) ==
	       LINKR_DEBUGGER_TASK_INVALID_BLOB);
	assert(linkr_debugger_task_tasks_store(disallowed_path, strlen(disallowed_path)) ==
	       LINKR_DEBUGGER_TASK_INVALID_BLOB);
	assert(linkr_debugger_task_tasks_store(oversized_wait, strlen(oversized_wait)) ==
	       LINKR_DEBUGGER_TASK_INVALID_BLOB);
}

struct invalid_utf8_task_blob {
	const char *name;
	const char *blob;
	size_t len;
};

static void test_rejects_invalid_utf8_before_persistence(void)
{
	static const char valid_multibyte[] =
		"# linkr-task.v1\n"
		"# \xe4\xb8\xad\xe6\x96\x87 comment\n"
		"# task caf\xc3\xa9\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\","
		"\"body\":\"{\\\"label\\\":\\\"\xe4\xb8\xad\xe6\x96\x87\\\"}\"}\n";
	static const char invalid_comment_overlong[] =
		"# linkr-task.v1\n"
		"# \xc0\x80 comment\n"
		"# task valid\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"}\n";
	static const char invalid_task_id_and_name[] =
		"# linkr-task.v1\n"
		"# task valid\xc2!\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"}\n";
	static const char invalid_body_surrogate[] =
		"# linkr-task.v1\n"
		"# task valid\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\","
		"\"body\":\"{\\\"label\\\":\\\"\xed\xa0\x80\\\"}\"}\n";
	static const char invalid_whitespace_too_high[] =
		"# linkr-task.v1 \xf4\x90\x80\x80\n"
		"# task valid\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"}\n";
	static const char invalid_truncated[] =
		"# linkr-task.v1\n"
		"# task valid\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"}\n"
		"\xe2\x82";
	static const char invalid_embedded_nul[] =
		"# linkr-task.v1\n"
		"# \0comment\n"
		"# task valid\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"}\n";
	static const struct invalid_utf8_task_blob cases[] = {
		{ "comment overlong", invalid_comment_overlong,
		  sizeof(invalid_comment_overlong) - 1U },
		{ "task id and derived name bad continuation", invalid_task_id_and_name,
		  sizeof(invalid_task_id_and_name) - 1U },
		{ "request body surrogate", invalid_body_surrogate,
		  sizeof(invalid_body_surrogate) - 1U },
		{ "whitespace region code point too high", invalid_whitespace_too_high,
		  sizeof(invalid_whitespace_too_high) - 1U },
		{ "truncated sequence", invalid_truncated, sizeof(invalid_truncated) - 1U },
		{ "embedded nul", invalid_embedded_nul, sizeof(invalid_embedded_nul) - 1U },
	};
	struct fake_backend backend = { 0 };
	struct linkr_debugger_task_status status;
	char snapshot[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
	size_t snapshot_len;

	init_with_backend(&backend);
	assert(linkr_debugger_task_tasks_store(TASK_BLOB_VALID, strlen(TASK_BLOB_VALID)) ==
	       LINKR_DEBUGGER_TASK_OK);
	for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
		assert(linkr_debugger_task_tasks_store(cases[i].blob, cases[i].len) ==
		       LINKR_DEBUGGER_TASK_INVALID_BLOB);
		assert(backend.tasks_present);
		assert(backend.tasks_length == strlen(TASK_BLOB_VALID));
		assert(memcmp(backend.tasks_value, TASK_BLOB_VALID, backend.tasks_length) == 0);
		assert(linkr_debugger_task_blob_snapshot(snapshot, sizeof(snapshot), &snapshot_len) ==
		       LINKR_DEBUGGER_TASK_OK);
		assert(snapshot_len == strlen(TASK_BLOB_VALID));
		assert(memcmp(snapshot, TASK_BLOB_VALID, snapshot_len) == 0);
	}
	assert(linkr_debugger_task_tasks_store(valid_multibyte,
					       sizeof(valid_multibyte) - 1U) == LINKR_DEBUGGER_TASK_OK);
	assert(linkr_debugger_task_status_get(&status) == LINKR_DEBUGGER_TASK_OK);
	assert(status.task_count == 1U);
	assert(strcmp(status.tasks[0].id, "caf\xc3\xa9") == 0);
	assert(strcmp(status.tasks[0].name, "caf\xc3\xa9") == 0);
}

static void test_requires_one_leading_version_marker(void)
{
	struct fake_backend backend = { 0 };
	const char *comment_before_version =
		"# comment before marker\n"
		TASK_BLOB_VALID;
	const char *duplicate_version =
		"# linkr-task.v1\n"
		"# comment after marker\n"
		"# linkr-task.v1\n"
		"# task valid\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"}\n";
	const char *empty_lines_and_comment_after_version =
		"\n\r\n# linkr-task.v1\n\n# comment after marker\n"
		"# task valid\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"}\n";

	init_with_backend(&backend);
	assert(linkr_debugger_task_tasks_store(comment_before_version,
					       strlen(comment_before_version)) ==
	       LINKR_DEBUGGER_TASK_INVALID_BLOB);
	assert(linkr_debugger_task_tasks_store(duplicate_version, strlen(duplicate_version)) ==
	       LINKR_DEBUGGER_TASK_INVALID_BLOB);
	assert(linkr_debugger_task_tasks_store(empty_lines_and_comment_after_version,
					       strlen(empty_lines_and_comment_after_version)) ==
	       LINKR_DEBUGGER_TASK_OK);
}

struct invalid_task_blob {
	const char *name;
	const char *blob;
};

static void test_rejects_strict_json_and_path_corpus(void)
{
	struct fake_backend backend = { 0 };
	static const struct invalid_task_blob cases[] = {
		{ "marker version suffix", "# linkr-task.v10\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"}\n" },
		{ "marker junk suffix", "# linkr-task.v1junk\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"}\n" },
		{ "trailing garbage", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"}junk\n" },
		{ "truncated object", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"\n" },
		{ "truncated string", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\n" },
		{ "duplicate method", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"}\n" },
		{ "duplicate path", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"path\":\"/api/v1/gpio/GP11\",\"body\":\"{}\"}\n" },
		{ "duplicate body", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\",\"body\":\"{}\"}\n" },
		{ "duplicate wait_ms", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\",\"wait_ms\":0,\"wait_ms\":1}\n" },
		{ "method wrong type", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":true,\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"}\n" },
		{ "path wrong type", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":[],\"body\":\"{}\"}\n" },
		{ "body wrong type", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":{}}\n" },
		{ "wait_ms wrong type", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\",\"wait_ms\":\"0\"}\n" },
		{ "unknown field", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\",\"extra\":0}\n" },
		{ "invalid outer escape", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP\\q\",\"body\":\"{}\"}\n" },
		{ "truncated unicode escape", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\\u12\"}\n" },
		{ "invalid utf8", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\xc0\x80\",\"wait_ms\":0}\n" },
		{ "decoded nul", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\\u0000junk\"}\n" },
		{ "invalid nested body JSON", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{]\"}\n" },
		{ "nested body trailing JSON", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{} true\"}\n" },
		{ "dot segment", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/power/../config\",\"body\":\"{}\"}\n" },
		{ "encoded dot segment", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/power/%2e%2e/config\",\"body\":\"{}\"}\n" },
		{ "extra path segment", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out/extra\",\"body\":\"{}\"}\n" },
		{ "query", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out?x=1\",\"body\":\"{}\"}\n" },
		{ "backslash traversal", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/power\\\\..\\\\config\",\"body\":\"{}\"}\n" },
		{ "empty identifier", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/power/\",\"body\":\"{}\"}\n" },
		{ "encoded slash", "# linkr-task.v1\n# task invalid\n"
		  "{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v%2fout\",\"body\":\"{}\"}\n" },
	};
	size_t accepted = 0U;

	init_with_backend(&backend);
	for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (linkr_debugger_task_tasks_store(cases[i].blob, strlen(cases[i].blob)) ==
		    LINKR_DEBUGGER_TASK_OK) {
			fprintf(stderr, "unexpectedly accepted: %s\n", cases[i].name);
			accepted++;
		}
	}
	assert(accepted == 0U);
}

static void test_accepts_decoded_escapes_and_enforces_json_depth(void)
{
	struct fake_backend backend = { 0 };
	const char *escaped =
		"# linkr-task.v1\n# retained padding comment\n# task escaped\n"
		"{\"body\":\"{\\\"value\\\":\\\"line\\\\n\\u263a\\\"}\","
		"\"path\":\"\\/api\\/v1\\/gpio\\/GP10\",\"method\":\"P\\u0055T\"}\n";
	char blob[LINKR_DEBUGGER_TASK_MAX_REQUEST_LINE + 64U];
	char body[2U * LINKR_DEBUGGER_TASK_MAX_JSON_DEPTH + 4U];
	size_t body_length = 0U;
	int written;

	init_with_backend(&backend);
	assert(linkr_debugger_task_tasks_store(escaped, strlen(escaped)) ==
	       LINKR_DEBUGGER_TASK_OK);
	for (size_t index = 0U; index < LINKR_DEBUGGER_TASK_MAX_JSON_DEPTH; index++) {
		body[body_length++] = '[';
	}
	body[body_length++] = '0';
	for (size_t index = 0U; index < LINKR_DEBUGGER_TASK_MAX_JSON_DEPTH; index++) {
		body[body_length++] = ']';
	}
	body[body_length] = '\0';
	written = snprintf(blob, sizeof(blob),
		"# linkr-task.v1\n# task depth\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"%s\"}\n",
		body);
	assert(written > 0 && (size_t)written < sizeof(blob));
	assert(linkr_debugger_task_tasks_store(blob, (size_t)written) ==
	       LINKR_DEBUGGER_TASK_OK);
	body[body_length++] = ']';
	memmove(body + 1U, body, body_length);
	body[0] = '[';
	body[++body_length] = '\0';
	written = snprintf(blob, sizeof(blob),
		"# linkr-task.v1\n# task depth\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"%s\"}\n",
		body);
	assert(written > 0 && (size_t)written < sizeof(blob));
	assert(linkr_debugger_task_tasks_store(blob, (size_t)written) ==
	       LINKR_DEBUGGER_TASK_INVALID_BLOB);
}

static void test_json_value_string_validation_honors_capacity(void)
{
	static const char escaped[] =
		"{\"a\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"}";
	static const char invalid_utf8[] = "{\"key\":\"\xc0\x80\"}";

	assert(linkr_debugger_json_value_valid("{\"key\":\"abc\"}", 16U, 4U));
	assert(!linkr_debugger_json_value_valid("{\"key\":\"abcd\"}", 16U, 4U));
	assert(!linkr_debugger_json_value_valid("{\"abcd\":0}", 16U, 4U));
	assert(linkr_debugger_json_value_valid(escaped, 16U, 9U));
	assert(linkr_debugger_json_value_valid("{\"key\":\"\\uD83D\\uDE00\"}", 16U, 5U));
	assert(!linkr_debugger_json_value_valid("{\"key\":\"\\uD83D\"}", 16U, 5U));
	assert(!linkr_debugger_json_value_valid("{\"key\":\"\\uDC00\"}", 16U, 5U));
	assert(!linkr_debugger_json_value_valid("{\"key\":\"\\u0000\"}", 16U, 5U));
	assert(!linkr_debugger_json_value_valid(invalid_utf8, 16U, 5U));
}

static void test_rejects_duplicate_task_ids_with_different_actions(void)
{
	struct fake_backend backend = { 0 };
	const char *duplicate =
		"# linkr-task.v1\n"
		"# task recovery\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{}\"}\n"
		"# task recovery\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\","
		"\"body\":\"{\\\"state\\\":\\\"off\\\"}\"}\n";

	init_with_backend(&backend);
	assert(linkr_debugger_task_tasks_store(duplicate, strlen(duplicate)) ==
	       LINKR_DEBUGGER_TASK_INVALID_BLOB);
}

static void test_request_limit(void)
{
	struct fake_backend backend = { 0 };
	char blob[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
	int written = snprintf(blob, sizeof(blob), "# linkr-task.v1\n# task bounded\n");
	size_t length;

	assert(written > 0 && (size_t)written < sizeof(blob));
	length = (size_t)written;
	for (size_t index = 0U; index <= LINKR_DEBUGGER_TASK_MAX_REQUESTS; index++) {
		written = snprintf(blob + length, sizeof(blob) - length,
			"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":0}\n");
		assert(written > 0 && (size_t)written < sizeof(blob) - length);
		length += (size_t)written;
	}

	init_with_backend(&backend);
	assert(linkr_debugger_task_tasks_store(blob, length) == LINKR_DEBUGGER_TASK_INVALID_BLOB);
	blob[length - (size_t)written] = '\0';
	assert(linkr_debugger_task_tasks_store(blob, strlen(blob)) == LINKR_DEBUGGER_TASK_OK);
}

static void fill_maximum_blob(char *blob)
{
	static const char prefix[] =
		"# linkr-task.v1\n"
		"# task max\n"
		"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/GP10\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":0}\n";
	size_t len = sizeof(prefix) - 1U;

	memcpy(blob, prefix, len);
	while (len < LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE) {
		size_t chunk = LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE - len;

		if (chunk > 256U) {
			chunk = 256U;
		}
		blob[len] = '#';
		if (chunk > 1U) {
			memset(blob + len + 1U, 'x', chunk - 2U);
			blob[len + chunk - 1U] = '\n';
		}
		len += chunk;
	}
}

static void test_maximum_snapshot_and_one_over_rejection(void)
{
	struct fake_backend backend = { 0 };
	char blob[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
	char snapshot[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
	char oversized[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE + 1U];
	size_t snapshot_len;

	fill_maximum_blob(blob);
	init_with_backend(&backend);
	assert(linkr_debugger_task_tasks_store(blob, sizeof(blob)) == LINKR_DEBUGGER_TASK_OK);
	assert(linkr_debugger_task_blob_snapshot(snapshot, sizeof(snapshot), &snapshot_len) ==
	       LINKR_DEBUGGER_TASK_OK);
	assert(snapshot_len == sizeof(blob));
	assert(memcmp(snapshot, blob, sizeof(blob)) == 0);
	memset(oversized, 'x', sizeof(oversized));
	assert(linkr_debugger_task_tasks_store(oversized, sizeof(oversized)) ==
	       LINKR_DEBUGGER_TASK_INVALID_BLOB);
}

struct snapshot_stress {
	atomic_bool done;
};

static void *snapshot_writer(void *context)
{
	struct snapshot_stress *stress = context;

	for (size_t i = 0U; i < 200U; i++) {
		const char *blob = (i & 1U) == 0U ? TASK_BLOB_VALID : TASK_BLOB_ALTERNATE;

		assert(linkr_debugger_task_tasks_store(blob, strlen(blob)) == LINKR_DEBUGGER_TASK_OK);
	}
	atomic_store(&stress->done, true);
	return NULL;
}

static void *snapshot_reader(void *context)
{
	struct snapshot_stress *stress = context;
	char blob[LINKR_DEBUGGER_TASK_MAX_BLOB_SIZE];
	size_t blob_len;

	while (!atomic_load(&stress->done)) {
		assert(linkr_debugger_task_blob_snapshot(blob, sizeof(blob), &blob_len) ==
		       LINKR_DEBUGGER_TASK_OK);
		assert((blob_len == strlen(TASK_BLOB_VALID) &&
			memcmp(blob, TASK_BLOB_VALID, blob_len) == 0) ||
		       (blob_len == strlen(TASK_BLOB_ALTERNATE) &&
			memcmp(blob, TASK_BLOB_ALTERNATE, blob_len) == 0));
	}
	return NULL;
}

static void test_snapshot_is_never_partial_during_store(void)
{
	struct fake_backend backend = { 0 };
	struct snapshot_stress stress = { 0 };
	pthread_t writer;
	pthread_t reader;

	init_with_backend(&backend);
	assert(linkr_debugger_task_tasks_store(TASK_BLOB_VALID, strlen(TASK_BLOB_VALID)) ==
	       LINKR_DEBUGGER_TASK_OK);
	assert(pthread_create(&writer, NULL, snapshot_writer, &stress) == 0);
	assert(pthread_create(&reader, NULL, snapshot_reader, &stress) == 0);
	assert(pthread_join(writer, NULL) == 0);
	assert(pthread_join(reader, NULL) == 0);
}

int main(void)
{
	test_store_list_and_clear();
	test_init_uses_only_task_storage();
	test_rejects_malformed_records_and_waits();
	test_rejects_invalid_utf8_before_persistence();
	test_requires_one_leading_version_marker();
	test_rejects_strict_json_and_path_corpus();
	test_accepts_decoded_escapes_and_enforces_json_depth();
	test_rejects_duplicate_task_ids_with_different_actions();
	test_json_value_string_validation_honors_capacity();
	test_request_limit();
	test_maximum_snapshot_and_one_over_rejection();
	test_snapshot_is_never_partial_during_store();
	puts("linkr_debugger_task: all tests passed");
	return 0;
}
