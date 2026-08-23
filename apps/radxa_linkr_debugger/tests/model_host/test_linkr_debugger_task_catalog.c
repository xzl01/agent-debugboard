/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "../../src/linkr_debugger_http_task_response.h"
#include "../../src/linkr_debugger_json_value.h"
#include "../../src/linkr_debugger_task.h"
#include "../../src/linkr_debugger_task_catalog.h"
#include "../../src/linkr_debugger_task_parse.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

_Static_assert(1000U <= LINKR_DEBUGGER_TASK_MAX_WAIT_MS,
	       "catalog power-off wait exceeds task limit");
_Static_assert(sizeof("{\"direction\":\"output\",\"value\":0}") - 1U <=
		       LINKR_DEBUGGER_TASK_MAX_BODY_LEN,
	       "catalog GPIO body exceeds task limit");
_Static_assert(sizeof("/api/v1/power/20v_out") - 1U <= LINKR_DEBUGGER_TASK_MAX_PATH_LEN,
	       "catalog power path exceeds task limit");

static const char expected_catalog[] =
	"{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,\"command\":\"task\","
	"\"action\":\"catalog\",\"version\":1,\"tasks\":["
	"{\"id\":\"builtin/maskrom/5v_out\",\"name\":\"Rockchip MASKROM via 5v_out\",\"requests\":["
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":{\"state\":\"off\"},\"wait_ms\":1000},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"output\",\"value\":0},\"wait_ms\":20},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":{\"state\":\"on\"},\"wait_ms\":500},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0}],"
	"\"cleanup\":{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0}},"
	"{\"id\":\"builtin/maskrom/12v_out\",\"name\":\"Rockchip MASKROM via 12v_out\",\"requests\":["
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/12v_out\",\"body\":{\"state\":\"off\"},\"wait_ms\":1000},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"output\",\"value\":0},\"wait_ms\":20},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/12v_out\",\"body\":{\"state\":\"on\"},\"wait_ms\":500},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0}],"
	"\"cleanup\":{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0}},"
	"{\"id\":\"builtin/maskrom/20v_out\",\"name\":\"Rockchip MASKROM via 20v_out\",\"requests\":["
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/20v_out\",\"body\":{\"state\":\"off\"},\"wait_ms\":1000},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"output\",\"value\":0},\"wait_ms\":20},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/20v_out\",\"body\":{\"state\":\"on\"},\"wait_ms\":500},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0}],"
	"\"cleanup\":{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0}},"
	"{\"id\":\"builtin/edl/5v_out\",\"name\":\"Qualcomm EDL via 5v_out\",\"requests\":["
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":{\"state\":\"off\"},\"wait_ms\":1000},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"output\",\"value\":1},\"wait_ms\":20},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":{\"state\":\"on\"},\"wait_ms\":500},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0}],"
	"\"cleanup\":{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0}},"
	"{\"id\":\"builtin/edl/12v_out\",\"name\":\"Qualcomm EDL via 12v_out\",\"requests\":["
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/12v_out\",\"body\":{\"state\":\"off\"},\"wait_ms\":1000},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"output\",\"value\":1},\"wait_ms\":20},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/12v_out\",\"body\":{\"state\":\"on\"},\"wait_ms\":500},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0}],"
	"\"cleanup\":{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0}},"
	"{\"id\":\"builtin/edl/20v_out\",\"name\":\"Qualcomm EDL via 20v_out\",\"requests\":["
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/20v_out\",\"body\":{\"state\":\"off\"},\"wait_ms\":1000},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"output\",\"value\":1},\"wait_ms\":20},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/20v_out\",\"body\":{\"state\":\"on\"},\"wait_ms\":500},"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0}],"
	"\"cleanup\":{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":{\"direction\":\"input\"},\"wait_ms\":0}}]}\n";

static size_t count_substring(const char *text, const char *needle)
{
	size_t count = 0U;
	size_t needle_len = strlen(needle);
	const char *match = text;

	while ((match = strstr(match, needle)) != NULL) {
		count++;
		match += needle_len;
	}

	return count;
}

static void test_catalog_contract(void)
{
	const uint8_t *body;
	const uint8_t *second_body;
	size_t body_len;
	size_t second_body_len;
	static const char *const ids[] = {
		"builtin/maskrom/5v_out",
		"builtin/maskrom/12v_out",
		"builtin/maskrom/20v_out",
		"builtin/edl/5v_out",
		"builtin/edl/12v_out",
		"builtin/edl/20v_out",
	};

	body = linkr_debugger_task_catalog_json(&body_len);
	second_body = linkr_debugger_task_catalog_json(&second_body_len);
	assert(body == second_body);
	assert(body_len == second_body_len);
	assert(body_len == sizeof(expected_catalog) - 1U);
	assert(body_len <= LINKR_DEBUGGER_TASK_HTTP_RESPONSE_CAP);
	assert(memcmp(body, expected_catalog, body_len) == 0);
	assert(((const char *)body)[body_len - 1U] == '\n');
	assert(linkr_debugger_json_value_valid((const char *)body,
		LINKR_DEBUGGER_TASK_MAX_JSON_DEPTH, LINKR_DEBUGGER_TASK_MAX_BODY_LEN + 1U));
	assert(count_substring((const char *)body, "\"id\":\"") ==
		LINKR_DEBUGGER_TASK_CATALOG_TASK_COUNT);
	for (size_t i = 0U; i < sizeof(ids) / sizeof(ids[0]); i++) {
		assert(count_substring((const char *)body, ids[i]) == 1U);
	}
	assert(count_substring((const char *)body, "\"requests\":[") ==
		LINKR_DEBUGGER_TASK_CATALOG_TASK_COUNT);
	assert(count_substring((const char *)body, "\"method\":\"PUT\"") == 36U);
	assert(count_substring((const char *)body, "/api/v1/gpio/CON_MAS") == 24U);
	assert(count_substring((const char *)body, "/api/v1/power/5v_out") == 4U);
	assert(count_substring((const char *)body, "/api/v1/power/12v_out") == 4U);
	assert(count_substring((const char *)body, "/api/v1/power/20v_out") == 4U);
	assert(count_substring((const char *)body, "\"wait_ms\":0") == 18U);
	assert(count_substring((const char *)body, "\"wait_ms\":20") == 6U);
	assert(count_substring((const char *)body, "\"wait_ms\":500") == 6U);
	assert(count_substring((const char *)body, "\"wait_ms\":1000") == 6U);
	assert(count_substring((const char *)body,
		"\"cleanup\":{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\","
		"\"body\":{\"direction\":\"input\"},\"wait_ms\":0}") == 6U);
}

int main(void)
{
	test_catalog_contract();
	puts("linkr_debugger_task_catalog: all tests passed");
	return 0;
}
