/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "test_linkr_debugger_http_net_stubs.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the production fallback router plus the /api/v1/net and /api/v1/pd
 * callbacks directly. */
#include "../../src/linkr_debugger_http.c"

static unsigned int setter_calls;
static uint8_t setter_subnet;
static uint8_t current_subnet = 203U;
static unsigned int pd_fixed_calls;
static unsigned int pd_pps_calls;
static unsigned int pd_status_calls;
static uint16_t pd_fixed_mv;
static uint16_t pd_pps_mv;
static int pd_status_result;
static struct linkr_debugger_pd_status pd_status;

const char *linkr_debugger_json_schema(void)
{
	return "radxa-linkr-debugger.v1";
}

int64_t json_obj_parse(char *payload, size_t len,
		       const struct json_obj_descr *descr, size_t descr_len,
		       void *value)
{
	char text[2048];
	int consumed = 0;

	if (len >= sizeof(text)) {
		return -EINVAL;
	}
	memcpy(text, payload, len);
	text[len] = '\0';

	if (descr_len == 1U) {
		int subnet;

		if (sscanf(text, "{\"subnet\":%d}%n", &subnet, &consumed) != 1 ||
		    consumed != (int)len) {
			return -EINVAL;
		}
		*(int *)((char *)value + descr[0].offset) = subnet;
		return BIT64(0);
	}
	if (descr_len == 2U) {
		int mode;
		int voltage_mv;

		if (sscanf(text, "{\"mode\":%d,\"voltage_mv\":%d}%n", &mode,
			   &voltage_mv, &consumed) != 2 ||
		    consumed != (int)len) {
			return -EINVAL;
		}
		*(int *)((char *)value + descr[0].offset) = mode;
		*(int *)((char *)value + descr[1].offset) = voltage_mv;
		return BIT64(0) | BIT64(1);
	}

	return -EINVAL;
}

int linkr_debugger_net_format_ipv4(uint8_t subnet, uint8_t host,
				   char *buf, size_t cap)
{
	int len = snprintf(buf, cap, "172.29.%u.%u",
			   (unsigned int)subnet, (unsigned int)host);

	return len < 0 || (size_t)len >= cap ? -ENOSPC : 0;
}

uint8_t linkr_debugger_network_subnet(void)
{
	return current_subnet;
}

int linkr_debugger_network_set_subnet_with_result(uint8_t subnet,
						  bool *reconnect_required)
{
	setter_calls++;
	setter_subnet = subnet;
	*reconnect_required = current_subnet != subnet;
	current_subnet = subnet;
	return 0;
}

int linkr_debugger_pd_get_status_short(struct linkr_debugger_pd_status *status)
{
	*status = pd_status;
	return pd_status_result;
}

int linkr_debugger_pd_get_status(struct linkr_debugger_pd_status *status)
{
	pd_status_calls++;
	*status = pd_status;
	return pd_status_result;
}

int linkr_debugger_pd_set_pps(uint16_t voltage_mv)
{
	pd_pps_calls++;
	pd_pps_mv = voltage_mv;
	return 0;
}

int linkr_debugger_pd_set_fixed(uint16_t voltage_mv)
{
	pd_fixed_calls++;
	pd_fixed_mv = voltage_mv;
	return 0;
}

static void reset_test(void)
{
	static const char *const directions[] = { "input", "output" };

	setter_calls = 0U;
	setter_subnet = 0U;
	current_subnet = 203U;
	pd_fixed_calls = 0U;
	pd_pps_calls = 0U;
	pd_status_calls = 0U;
	pd_fixed_mv = 0U;
	pd_pps_mv = 0U;
	pd_status_result = 0;
	memset(&pd_status, 0, sizeof(pd_status));
	pd_status.available = true;
	pd_status.mode = LINKR_DEBUGGER_PD_MODE_FIXED;
	pd_status.protocol = 4U;
	pd_status.pd_active = true;
	pd_status.qc2 = true;
	pd_status.qc3 = true;
	pd_status.bc = true;
	pd_status.epr = true;
	pd_status.avs = true;
	pd_status.gear = 5U;
	pd_status.voltage_mv = 20000U;
	pd_status.pps_mv = 11000U;
	pd_status.max_current_ma = 5000U;
	pd_status.capabilities_valid = true;
	pd_status.fixed_count = 7U;
	for (size_t i = 0U; i < pd_status.fixed_count; i++) {
		pd_status.fixed[i].voltage_mv = (uint16_t)(5000U + i * 2500U);
		pd_status.fixed[i].max_current_ma = (uint16_t)(5000U - i * 250U);
	}
	pd_status.pps_count = 2U;
	for (size_t i = 0U; i < pd_status.pps_count; i++) {
		pd_status.pps[i].min_voltage_mv = (uint16_t)(3300U + i * 1700U);
		pd_status.pps[i].max_voltage_mv = (uint16_t)(11000U + i * 10000U);
		pd_status.pps[i].step_mv = 100U;
		pd_status.pps[i].max_current_ma = (uint16_t)(5000U - i * 2000U);
	}
	pd_status.avs_count = 1U;
	pd_status.avs_ranges[0].min_voltage_mv = 15000U;
	pd_status.avs_ranges[0].max_voltage_mv = 28000U;
	pd_status.avs_ranges[0].step_mv = 100U;
	pd_status.avs_ranges[0].max_power_w = 140U;

	for (size_t i = 0U; i < ARRAY_SIZE(http_test_power_states); i++) {
		http_test_power_states[i] = (i % 2U) == 0U;
	}
	for (size_t i = 0U; i < ARRAY_SIZE(http_test_gpio_values); i++) {
		http_test_gpio_values[i] = (int)(i % 2U);
		http_test_gpio_directions[i] = directions[i % ARRAY_SIZE(directions)];
	}
	http_test_watchdog_status = (struct linkr_debugger_watchdog_status){
		.supported = true,
		.armed = true,
		.automatic = true,
		.healthy = false,
		.timeout_ms = 8388U,
		.bootloader_on_timeout = true,
		.fault_injection_available = true,
		.fault_injection_armed = true,
		.failing_service = "adc-sampler",
	};
	http_test_monitoring_snapshot = (struct linkr_debugger_monitoring_snapshot){
		.temperature = {
			.available = true,
			.reason = "ready",
			.source = "rp2350-die-temperature",
			.celsius_val1 = 47,
			.celsius_val2 = 625000,
		},
		.heap = {
			.available = true,
			.reason = "ready",
			.source = "system-heap-runtime-stats",
			.free_bytes = 32768U,
			.allocated_bytes = 24576U,
			.max_allocated_bytes = 28672U,
			.total_bytes = 57344U,
		},
		.memory = {
			.available = true,
			.reason = "ready",
			.source = "linker-and-thread-analyzer",
			.coverage = "system-heap-and-all-runtime-threads",
			.limiting_component = "thread_stack",
			.limiting_name = "linkr-debugger-websocket-client-3",
			.pressure_pct_x100 = 8125U,
			.system_heap_pressure_pct_x100 = 4285U,
			.physical = {
				.total_bytes = 520192U,
				.image_reserved_bytes = 212992U,
				.reserved_pct_x100 = 4094U,
			},
			.stacks = {
				.thread_count = 18U,
				.measured_count = 18U,
				.error_count = 0U,
				.total_bytes = 49152U,
				.used_high_water_bytes = 39936U,
				.max_pressure_pct_x100 = 8125U,
				.max_pressure_thread = "linkr-debugger-websocket-client-3",
			},
			.current_pressure = {
				.available = true,
				.reason = "ready",
				.coverage = "all-runtime-components",
				.limiting_component = "net_buf_rx_data",
				.limiting_name = "network-rx-data-pool",
				.pressure_pct_x100 = 7343U,
				.tie_count = 1U,
			},
			.peak_pressure = {
				.available = true,
				.reason = "ready",
				.coverage = "all-runtime-components",
				.limiting_component = "thread_stack",
				.limiting_name = "linkr-debugger-websocket-client-3",
				.pressure_pct_x100 = 8125U,
				.tie_count = 1U,
			},
		},
		.runtime = {
			.available = true,
			.reason = "ready",
			.uptime_ms = 123456789,
			.uptime_seconds = 123456U,
		},
		.cpu = {
			.available = true,
			.reason = "ready",
			.active_pct_x100 = 3725U,
			.window_ms = 1000U,
			.busy_cycles_delta = 55875000U,
			.total_cycles_delta = 150000000U,
		},
	};
	http_test_config_status = (struct linkr_debugger_config_service_status){
		.available = true,
		.reason = "ready",
		.snapshot_present = true,
		.snapshot_version = 1U,
		.item_count = 18U,
		.saved_count = 18U,
		.applied_count = 15U,
		.pending_count = 2U,
		.failed_count = 1U,
		.failed_errno = -EIO,
	};
	linkr_debugger_gpio_layout_groups[0].name = "J8";
	linkr_debugger_gpio_layout_groups[0].edge = "right";
	linkr_debugger_gpio_layout_groups[1].name = "J13";
	linkr_debugger_gpio_layout_groups[1].edge = "right";
	linkr_debugger_http_body_reset_all();
}

static void route(struct http_client_ctx *client,
		  enum http_transaction_status status,
		  const char *fragment, struct http_response_ctx *response)
{
	struct http_request_ctx request = {
		.data = (char *)fragment,
		.data_len = fragment != NULL ? strlen(fragment) : 0U,
	};

	assert(linkr_debugger_http_route_request(client, status, &request,
						 response, NULL) == 0);
}

static size_t count_substring(const char *text, const char *needle)
{
	size_t count = 0U;
	size_t needle_len = strlen(needle);

	while ((text = strstr(text, needle)) != NULL) {
		count++;
		text += needle_len;
	}
	return count;
}

static struct http_client_ctx net_put_client(void)
{
	struct http_client_ctx client = { .method = HTTP_PUT };

	strcpy(client.url_buffer, "/api/v1/net");
	return client;
}

static struct http_client_ctx pd_put_client(void)
{
	struct http_client_ctx client = { .method = HTTP_PUT };

	strcpy(client.url_buffer, "/api/v1/pd");
	return client;
}

static void test_fragmented_put_executes_once_with_complete_json(void)
{
	struct http_client_ctx client = net_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"subnet\":", &response);
	assert(setter_calls == 0U);
	assert(!response.final_chunk);

	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "204}", &response);
	assert(setter_calls == 1U);
	assert(setter_subnet == 204U);
	assert(response.status == HTTP_200_OK);
	assert(response.final_chunk);
}

static void test_aborted_and_complete_clear_without_executing(void)
{
	struct http_client_ctx client = net_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"subnet\":", &response);
	route(&client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, &response);
	assert(setter_calls == 0U);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "204}", &response);
	assert(setter_calls == 0U);
	assert(response.status == HTTP_400_BAD_REQUEST);

	memset(&response, 0, sizeof(response));
	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"subnet\":", &response);
	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	assert(setter_calls == 0U);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "204}", &response);
	assert(setter_calls == 0U);
	assert(response.status == HTTP_400_BAD_REQUEST);
}

static void test_invalid_prefix_cannot_be_replaced_by_valid_final_fragment(void)
{
	struct http_client_ctx client = net_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "garbage", &response);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "{\"subnet\":204}", &response);
	assert(setter_calls == 0U);
	assert(response.status == HTTP_400_BAD_REQUEST);
}

static void test_pd_fragmented_put_executes_once_with_complete_json(void)
{
	struct http_client_ctx client = pd_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"mode\":0,\"voltage_mv\":",
	      &response);
	assert(pd_fixed_calls == 0U);
	assert(pd_pps_calls == 0U);
	assert(!response.final_chunk);

	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "9000}", &response);
	assert(pd_fixed_calls == 1U);
	assert(pd_fixed_mv == 9000U);
	assert(pd_pps_calls == 0U);
	assert(pd_status_calls == 1U);
	assert(response.status == HTTP_200_OK);
	assert(response.final_chunk);
	assert(strstr((const char *)response.body, "\"action\":\"set\"") != NULL);
}

static void test_pd_aborted_request_clears_partial_body(void)
{
	struct http_client_ctx client = pd_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"mode\":0,", &response);
	route(&client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, &response);
	assert(pd_fixed_calls == 0U);
	assert(pd_pps_calls == 0U);

	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
	      "\"voltage_mv\":9000}", &response);
	assert(pd_fixed_calls == 0U);
	assert(pd_pps_calls == 0U);
	assert(response.status == HTTP_400_BAD_REQUEST);

	memset(&response, 0, sizeof(response));
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"mode\":0,\"voltage_mv\":",
	      &response);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "9000}", &response);
	assert(pd_fixed_calls == 1U);
	assert(pd_fixed_mv == 9000U);
	assert(response.status == HTTP_200_OK);
}

static void test_pd_complete_request_clears_partial_body(void)
{
	struct http_client_ctx client = pd_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"mode\":0,", &response);
	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	assert(pd_fixed_calls == 0U);
	assert(pd_pps_calls == 0U);

	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
	      "\"voltage_mv\":12000}", &response);
	assert(pd_fixed_calls == 0U);
	assert(pd_pps_calls == 0U);
	assert(response.status == HTTP_400_BAD_REQUEST);

	memset(&response, 0, sizeof(response));
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"mode\":0,\"voltage_mv\":",
	      &response);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "12000}", &response);
	assert(pd_fixed_calls == 1U);
	assert(pd_fixed_mv == 12000U);
	assert(response.status == HTTP_200_OK);
}

static void test_pd_invalid_prefix_cannot_be_replaced_by_valid_final_fragment(void)
{
	struct http_client_ctx client = pd_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "garbage", &response);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
	      "{\"mode\":0,\"voltage_mv\":9000}", &response);
	assert(pd_fixed_calls == 0U);
	assert(pd_pps_calls == 0U);
	assert(response.status == HTTP_400_BAD_REQUEST);
	assert(strstr((const char *)response.body, "\"code\":\"invalid_body\"") != NULL);
}

static void test_gpio_layout_group_encoder_is_generic_and_bounded(void)
{
	static const struct linkr_debugger_gpio_layout_group_desc groups[] = {
		{ .name = "P\"\\\n\x01" "\xe9\x9b\xaa",
		  .edge = "right\t\r\b\f", .order = 7U },
	};
	static const struct linkr_debugger_gpio_layout_group_desc null_name[] = {
		{ .name = NULL, .edge = "right", .order = 0U },
	};
	static const struct linkr_debugger_gpio_layout_group_desc invalid_utf8[] = {
		{ .name = "\xc0\x80", .edge = "right", .order = 0U },
	};
	static const char expected[] =
		"[{\"name\":\"P\\\"\\\\\\n\\u0001\xe9\x9b\xaa\","
		"\"edge\":\"right\\t\\r\\b\\f\",\"order\":7}]";
	char encoded[sizeof(expected) + 16U];
	char exact[sizeof(expected)];
	char short_buffer[sizeof(expected) - 1U];
	char empty[3];
	struct linkr_debugger_http_env env = {
		.buf = encoded,
		.cap = sizeof(encoded),
	};
	struct linkr_debugger_http_env exact_env = {
		.buf = exact,
		.cap = sizeof(exact),
	};
	struct linkr_debugger_http_env short_env = {
		.buf = short_buffer,
		.cap = sizeof(short_buffer),
	};
	struct linkr_debugger_http_env empty_env = {
		.buf = empty,
		.cap = sizeof(empty),
	};
	struct linkr_debugger_http_env invalid_env = {
		.buf = encoded,
		.cap = sizeof(encoded),
	};

	assert(linkr_debugger_http_json_gpio_layout_groups_from(
		       &env, groups, ARRAY_SIZE(groups)) == 0);
	assert(strcmp(encoded, expected) == 0);
	assert(env.len == sizeof(expected) - 1U);
	assert(linkr_debugger_http_json_gpio_layout_groups_from(
		       &exact_env, groups, ARRAY_SIZE(groups)) == 0);
	assert(strcmp(exact, expected) == 0);
	assert(linkr_debugger_http_json_gpio_layout_groups_from(
		       &short_env, groups, ARRAY_SIZE(groups)) == -ENOMEM);
	assert(short_env.truncated);
	assert(linkr_debugger_http_json_gpio_layout_groups_from(
		       &empty_env, NULL, 0U) == 0);
	assert(strcmp(empty, "[]") == 0);
	assert(linkr_debugger_http_json_gpio_layout_groups_from(&env, NULL, 1U) ==
	       -EINVAL);
	assert(linkr_debugger_http_json_gpio_layout_groups_from(
		       &invalid_env, null_name, ARRAY_SIZE(null_name)) == -EINVAL);
	assert(invalid_env.len == 0U);
	assert(linkr_debugger_http_json_gpio_layout_groups_from(
		       &invalid_env, invalid_utf8, ARRAY_SIZE(invalid_utf8)) == -EINVAL);
	assert(invalid_env.len == 0U);
}

static void test_gpio_layout_groups_are_top_level_http_contract(void)
{
	static const char expected_groups[] =
		"\"gpio_layout_groups\":[{\"name\":\"J8\",\"edge\":\"right\","
		"\"order\":0},{\"name\":\"J13\",\"edge\":\"right\",\"order\":1}]";
	struct http_response_ctx response = { 0 };
	struct http_client_ctx client = { .method = HTTP_GET };
	const char *body;
	const char *match;

	reset_test();
	strcpy(client.url_buffer, "/api/v1/status");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_200_OK);
	assert(response.final_chunk);
	assert(response.body_len == strlen((const char *)response.body));
	assert(response.body_len < LINKR_DEBUGGER_HTTP_STATUS_JSON_BUFSZ);
	body = (const char *)response.body;
	assert(count_substring(body, "\"gp\":") == HTTP_TEST_RAIL_COUNT);
	assert(count_substring(body, "\"adc_index\":") == HTTP_TEST_ADC_COUNT);
	assert(count_substring(body, "\"layoutGroup\":") == HTTP_TEST_MAPPED_GPIO_COUNT);
	assert(count_substring(body, "\"direction\":") == HTTP_TEST_SAFE_GPIO_COUNT);
	assert(strstr(body,
		      "\"note\":\"J8_PIN1\",\"layoutGroup\":\"J8\","
		      "\"layoutLabel\":\"GP12\",\"layoutRow\":9,\"layoutColumn\":0") != NULL);
	assert(strstr(body,
		      "\"note\":\"CON_USER\",\"layoutGroup\":\"J13\","
		      "\"layoutLabel\":\"USER\",\"layoutRow\":0,\"layoutColumn\":0") != NULL);
	assert(strstr(body,
		      "{\"name\":\"GP18\",\"pin\":18,\"note\":\"GP18\","
		      "\"value\":0,\"direction\":\"input\"}") != NULL);
	assert(strstr(body,
		      "{\"name\":\"GP17\",\"pin\":17,\"note\":\"GP17\","
		      "\"value\":1,\"direction\":\"output\"}") != NULL);
	assert(strstr(body, "\"fixed_profiles\":[") != NULL);
	assert(strstr(body, "\"avs_ranges\":[") != NULL);
	assert(strstr(body, "\"board_monitoring\":{\"temperature\":{\"available\":true") != NULL);
	assert(strstr(body, "\"config\":{\"available\":true,\"reason\":\"ready\"") != NULL);
	match = strstr(body, expected_groups);
	assert(match != NULL);
	assert(strstr(match + 1, "\"gpio_layout_groups\"") == NULL);
	printf("HTTP status full fixture: rails=%u adc=%u gpio=%u mapped=%u groups=%zu, %zu/%u bytes\n",
	       HTTP_TEST_RAIL_COUNT, HTTP_TEST_ADC_COUNT, HTTP_TEST_SAFE_GPIO_COUNT,
	       HTTP_TEST_MAPPED_GPIO_COUNT, linkr_debugger_gpio_layout_group_count, response.body_len,
	       LINKR_DEBUGGER_HTTP_STATUS_JSON_BUFSZ);

	memset(&response, 0, sizeof(response));
	strcpy(client.url_buffer, "/api/v1/gpio");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_200_OK);
	body = (const char *)response.body;
	assert(strstr(body, "\"action\":\"list\"") != NULL);
	assert(count_substring(body, "\"layoutGroup\":") == HTTP_TEST_MAPPED_GPIO_COUNT);
	assert(count_substring(body, "\"direction\":") == HTTP_TEST_SAFE_GPIO_COUNT);
	match = strstr(body, expected_groups);
	assert(match != NULL);
	assert(strstr(match + 1, "\"gpio_layout_groups\"") == NULL);

	memset(&response, 0, sizeof(response));
	strcpy(client.url_buffer, "/api/v1/gpio/GP17");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_200_OK);
	body = (const char *)response.body;
	assert(strstr(body, "\"name\":\"GP17\"") != NULL);
	assert(strstr(body, "\"note\":\"GP17\"") != NULL);
	assert(strstr(body, "\"layoutGroup\"") == NULL);
	assert(strstr(body, "\"layoutRow\"") == NULL);
	assert(strstr(body, "\"layoutColumn\"") == NULL);
}

static void test_status_budget_rejects_inflated_group_metadata(void)
{
	char inflated_name[5000];
	struct http_response_ctx response = { 0 };
	struct http_client_ctx client = { .method = HTTP_GET };

	reset_test();
	memset(inflated_name, 'G', sizeof(inflated_name) - 1U);
	inflated_name[sizeof(inflated_name) - 1U] = '\0';
	linkr_debugger_gpio_layout_groups[0].name = inflated_name;
	strcpy(client.url_buffer, "/api/v1/status");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_500_INTERNAL_SERVER_ERROR);
	assert(strstr((const char *)response.body,
		      "\"code\":\"response_too_large\"") != NULL);
	linkr_debugger_gpio_layout_groups[0].name = "J8";
}

int main(void)
{
	test_fragmented_put_executes_once_with_complete_json();
	test_aborted_and_complete_clear_without_executing();
	test_invalid_prefix_cannot_be_replaced_by_valid_final_fragment();
	test_pd_fragmented_put_executes_once_with_complete_json();
	test_pd_aborted_request_clears_partial_body();
	test_pd_complete_request_clears_partial_body();
	test_pd_invalid_prefix_cannot_be_replaced_by_valid_final_fragment();
	test_gpio_layout_group_encoder_is_generic_and_bounded();
	test_gpio_layout_groups_are_top_level_http_contract();
	test_status_budget_rejects_inflated_group_metadata();
	return 0;
}
