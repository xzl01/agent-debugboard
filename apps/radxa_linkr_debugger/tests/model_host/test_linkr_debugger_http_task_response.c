/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "../../src/linkr_debugger_http_task_response.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

const char *linkr_debugger_json_schema(void)
{
	return "radxa-linkr-debugger.v1";
}

static struct linkr_debugger_task_status sample_status(void)
{
	struct linkr_debugger_task_status status = {
		.backend_available = true,
		.revision = UINT64_C(18446744073709551615),
		.full_ir_len = 123U,
		.task_count = 1U,
	};

	for (size_t i = 0U; i < sizeof(status.task_store_id); i++) {
		status.task_store_id[i] = (uint8_t)i;
	}
	memset(status.catalog_fingerprint, 0xab, sizeof(status.catalog_fingerprint));
	memset(status.full_ir_digest, 0xcd, sizeof(status.full_ir_digest));
	status.tasks[0].slot = 3U;
	memcpy(status.tasks[0].id, "quote\"\\\xe4\xb8\xad", sizeof("quote\"\\\xe4\xb8\xad"));
	status.tasks[0].request_count = 2U;
	status.tasks[0].source_present = true;
	status.tasks[0].source_len = 4000U;
	memset(status.tasks[0].source_digest, 0x12,
	       sizeof(status.tasks[0].source_digest));
	status.tasks[0].ir_len = 117U;
	memset(status.tasks[0].ir_digest, 0x34, sizeof(status.tasks[0].ir_digest));
	return status;
}

static void test_metadata_is_bounded_and_contains_no_raw_ir(void)
{
	struct linkr_debugger_task_status status = sample_status();
	char response[LINKR_DEBUGGER_TASK_HTTP_RESPONSE_CAP];
	int written = linkr_debugger_http_task_list_response(response, sizeof(response), &status);

	assert(written > 0 && (size_t)written == strlen(response));
	assert(strstr(response, "\"command\":\"tasks\",\"action\":\"list\"") != NULL);
	assert(strstr(response, "\"task_store_id\":\"00010203-0405-0607-0809-0a0b0c0d0e0f\"") != NULL);
	assert(strstr(response, "\"revision\":\"18446744073709551615\"") != NULL);
	assert(strstr(response, "\"catalog_fingerprint\":\"abab") != NULL);
	assert(strstr(response, "\"full_ir_bytes\":123") != NULL);
	assert(strstr(response, "\"source_state\":\"present\"") != NULL);
	assert(strstr(response, "\"source_bytes\":4000") != NULL);
	assert(strstr(response, "quote\\\"\\\\\xe4\xb8\xad") != NULL);
	assert(strstr(response, "\"blob\"") == NULL);
	assert(linkr_debugger_http_task_list_response(response, 32U, &status) < 0);
}

static void test_mutation_and_conflict_responses(void)
{
	struct linkr_debugger_task_mutation_outcome outcome = {
		.status = sample_status(),
		.affected_task_present = true,
		.affected_slot = 3U,
	};
	char response[LINKR_DEBUGGER_TASK_HTTP_RESPONSE_CAP];
	int written;

	written = linkr_debugger_http_task_mutation_response(response, sizeof(response),
			&outcome, LINKR_DEBUGGER_TASK_MUTATION_UPSERT);
	assert(written > 0);
	assert(strstr(response, "\"action\":\"upsert\"") != NULL);
	assert(strstr(response, "\"task\":{\"slot\":3") != NULL);

	written = linkr_debugger_http_task_error_response(response, sizeof(response),
			"revision_conflict", "task revision changed", &outcome.status);
	assert(written > 0);
	assert(strstr(response, "\"ok\":false") != NULL);
	assert(strstr(response, "\"code\":\"revision_conflict\"") != NULL);
	assert(strstr(response, "\"revision\":\"18446744073709551615\"") != NULL);
}

int main(void)
{
	test_metadata_is_bounded_and_contains_no_raw_ir();
	test_mutation_and_conflict_responses();
	puts("linkr_debugger_http_task_response: all tests passed");
	return 0;
}
