/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "test_linkr_debugger_http_net_stubs.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the production fallback router and actual route handlers directly. */
#include "../../src/linkr_debugger_serial_http.c"
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
static struct linkr_debugger_serial_status serial_status;
static struct linkr_debugger_serial_net_status serial_net_status;
static struct linkr_debugger_serial_config serial_config_request;
static int serial_status_result;
static int serial_config_result;
static unsigned int serial_config_calls;
static uint64_t serial_config_expected_generation;
static uint64_t serial_config_request_id = 77U;

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
	if (descr_len == 6U) {
		long long baud;
		long long data_bits;
		long long stop_bits;
		unsigned long long expected_generation;
		char parity[8];
		char flow_control[12];

		if (sscanf(text,
			   "{\"baud\":%lld,\"data_bits\":%lld,\"parity\":\"%7[^\"]\","
			   "\"stop_bits\":%lld,\"flow_control\":\"%11[^\"]\","
			   "\"expected_generation\":%llu}%n",
			   &baud, &data_bits, parity, &stop_bits, flow_control,
			   &expected_generation, &consumed) != 6 || consumed != (int)len) {
			return -EINVAL;
		}
		*(int64_t *)((char *)value + descr[0].offset) = (int64_t)baud;
		*(int64_t *)((char *)value + descr[1].offset) = (int64_t)data_bits;
		strcpy((char *)value + descr[2].offset, parity);
		*(int64_t *)((char *)value + descr[3].offset) = (int64_t)stop_bits;
		strcpy((char *)value + descr[4].offset, flow_control);
		*(uint64_t *)((char *)value + descr[5].offset) =
			(uint64_t)expected_generation;
		return BIT64(0) | BIT64(1) | BIT64(2) | BIT64(3) |
		       BIT64(4) | BIT64(5);
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

int linkr_debugger_serial_status_snapshot(
	struct linkr_debugger_serial_status *status)
{
	*status = serial_status;
	return serial_status_result;
}

int linkr_debugger_serial_net_status_snapshot(
	struct linkr_debugger_serial_net_status *status)
{
	*status = serial_net_status;
	return 0;
}

int linkr_debugger_serial_config_cas(
	const struct linkr_debugger_serial_config *config,
	uint64_t expected_generation, uint64_t *request_id)
{
	serial_config_calls++;
	serial_config_request = *config;
	serial_config_expected_generation = expected_generation;
	*request_id = serial_config_request_id;
	return serial_config_result;
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
	serial_status_result = 0;
	serial_config_result = 0;
	serial_config_calls = 0U;
	serial_config_expected_generation = 0U;
	memset(&serial_config_request, 0, sizeof(serial_config_request));
	serial_status = (struct linkr_debugger_serial_status){
		.current_config = {
			.baud = 115200U,
			.data_bits = 8U,
			.parity = LINKR_DEBUGGER_SERIAL_PARITY_NONE,
			.stop_bits = 1U,
			.flow_control = LINKR_DEBUGGER_SERIAL_FLOW_NONE,
		},
		.runtime_counters = {
			.uart_rx_bytes = 1234U,
			.uart_rx_pause_dropped = 5U,
			.cdc_rx_dropped = 6U,
			.cdc_tx_stale_dropped = 7U,
			.completion_overflow = 2U,
		},
		.generation = 9U,
		.rx_head = 1234U,
		.tx_bytes_committed = 456U,
		.tx_accepted = 12U,
		.tx_completed = 10U,
		.tx_canceled = 2U,
		.last_config_request_id = 70U,
		.last_config_result = 0,
		.state = LINKR_DEBUGGER_SERIAL_READY,
		.tx_used_slots = 1U,
		.last_config_done = true,
		.dtr = true,
		.rts = false,
		.usb_configured = true,
	};
	memset(&serial_net_status, 0, sizeof(serial_net_status));
	serial_net_status.initialized = true;
	serial_net_status.listening = true;
	serial_net_status.tcp_port = 5555U;
	serial_net_status.ws_attached = 1U;
	serial_net_status.tcp_connected = 2U;
	serial_net_status.tcp[0].connected = 3U;
	serial_net_status.tcp[0].tx_bytes = 44U;
	serial_net_status.tcp[1].inferred_rx_drops = 8U;
	serial_net_status.ws[0].rx_bytes = 55U;
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
		.item_count = 16U,
		.saved_count = 16U,
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
	memset(serial_http_poison, 0, sizeof(serial_http_poison));
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

static void test_shared_json_workspace_static_ram_contract(void)
{
	char path[sizeof(__FILE__) + 64U] = __FILE__;
	char *filename = strrchr(path, '/');
	char *source;
	FILE *file;
	long size;
	size_t compact_len = 0U;

	assert(LINKR_DEBUGGER_HTTP_JSON_BUFSZ == 4096U);
	assert(LINKR_DEBUGGER_HTTP_STATUS_JSON_BUFSZ == 6656U);
	assert(sizeof(linkr_debugger_http_json_workspace) == 6656U);
	assert(filename != NULL);
	strcpy(filename + 1, "../../src/linkr_debugger_http.c");
	file = fopen(path, "rb");
	assert(file != NULL);
	assert(fseek(file, 0L, SEEK_END) == 0);
	size = ftell(file);
	assert(size > 0L);
	rewind(file);
	source = malloc((size_t)size + 1U);
	assert(source != NULL);
	assert(fread(source, 1U, (size_t)size, file) == (size_t)size);
	assert(fclose(file) == 0);
	for (size_t i = 0U; i < (size_t)size; i++) {
		if (!isspace((unsigned char)source[i])) {
			source[compact_len++] = source[i];
		}
	}
	source[compact_len] = '\0';
	assert(count_substring(source,
		"staticuint8_tlinkr_debugger_http_json_workspace[") == 1U);
	/* Include the optional fault-injection handler, even in production builds.
	 * The separate 256-byte reboot response is intentionally unchanged.
	 */
	assert(count_substring(source,
		"uint8_t*constjson_buf=linkr_debugger_http_json_workspace;") == 11U);
	assert(count_substring(source, ".cap=LINKR_DEBUGGER_HTTP_JSON_BUFSZ,") == 10U);
	assert(count_substring(source, ".cap=LINKR_DEBUGGER_HTTP_STATUS_JSON_BUFSZ,") == 1U);
	assert(count_substring(source, "staticuint8_tjson_buf[") == 1U);
	assert(strstr(source, "staticuint8_tjson_buf[256];") != NULL);
	assert(strstr(source, "staticcharjson_buf[") == NULL);
	free(source);
}

static void test_handlers_share_workspace_until_synchronous_send_finishes(void)
{
	static const struct {
		const char *path;
		enum http_method method;
		enum http_status status;
	} cases[] = {
		{ "/api/v1/status", HTTP_GET, HTTP_200_OK },
		{ "/api/v1/power", HTTP_GET, HTTP_200_OK },
		{ "/api/v1/adc/read", HTTP_GET, HTTP_200_OK },
		{ "/api/v1/switch", HTTP_GET, HTTP_200_OK },
		{ "/api/v1/gpio", HTTP_GET, HTTP_200_OK },
		{ "/api/v1/gpio/layout", HTTP_GET, HTTP_200_OK },
		{ "/api/v1/bootloader", HTTP_GET, HTTP_405_METHOD_NOT_ALLOWED },
		{ "/api/v1/watchdog", HTTP_GET, HTTP_200_OK },
		{ "/api/v1/pd", HTTP_GET, HTTP_200_OK },
		{ "/api/v1/net", HTTP_GET, HTTP_200_OK },
		{ "/api/v1/live-sessions", HTTP_GET, HTTP_400_BAD_REQUEST },
		{ "/api/v1/live-sessions/1", HTTP_GET, HTTP_404_NOT_FOUND },
		{ "/api/v1/live-sessions/1", HTTP_DELETE, HTTP_500_INTERNAL_SERVER_ERROR },
	};
	char sent[LINKR_DEBUGGER_HTTP_STATUS_JSON_BUFSZ];
	struct http_client_ctx client = { .method = HTTP_GET };

	reset_test();
	for (size_t i = 0U; i < ARRAY_SIZE(cases); i++) {
		struct http_response_ctx response = { 0 };
		struct http_response_ctx next_response = { 0 };
		struct http_client_ctx next_client = { .method = HTTP_GET };

		strcpy(client.url_buffer, cases[i].path);
		client.method = cases[i].method;
		route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
		assert(response.status == cases[i].status);
		assert(response.body == linkr_debugger_http_json_workspace);
		assert(response.final_chunk);
		assert(response.body_len == strlen((const char *)response.body));
		assert(response.body_len < sizeof(sent));
		assert(response.body_len > 0U);
		assert(response.body[response.body_len - 1U] == '\n');
		assert(response.header_count == 1U);
		assert(strcmp(response.headers[0].name, "Cache-Control") == 0);
		assert(strcmp(response.headers[0].value, "no-store") == 0);
		/* Zephyr consumes the body AFTER the callback returns, but BEFORE
		 * dispatching another handler. Model that transport copy explicitly;
		 * response.body is borrowed storage, not a retained per-client body.
		 */
		memcpy(sent, response.body, response.body_len + 1U);
		route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
		assert(strcmp(sent, (const char *)response.body) == 0);
		strcpy(next_client.url_buffer, "/api/v1/pd");
		pd_status_result = -ENODEV;
		route(&next_client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &next_response);
		pd_status_result = 0;
		assert(next_response.status == HTTP_503_SERVICE_UNAVAILABLE);
		assert(next_response.body == response.body);
		assert(strstr((const char *)next_response.body,
			      "\"code\":\"unavailable\"") != NULL);
		assert(strcmp(sent, (const char *)next_response.body) != 0);
		route(&next_client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, &next_response);
		assert(strstr((const char *)next_response.body,
			      "\"code\":\"unavailable\"") != NULL);
	}
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

static void test_serial_status_config_and_methods(void)
{
	static const char config_json[] =
		"{\"baud\":230400,\"data_bits\":8,\"parity\":\"even\","
		"\"stop_bits\":1,\"flow_control\":\"none\","
		"\"expected_generation\":9}";
	struct http_response_ctx response = { 0 };
	struct http_client_ctx client = { .method = HTTP_GET };
	const char *body;

	reset_test();
	strcpy(client.url_buffer, "/api/v1/serial/j4");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_200_OK);
	assert(response.body_len < 4096U);
	body = (const char *)response.body;
	assert(strstr(body, "\"available\":true,\"state\":\"ready\"") != NULL);
	assert(strstr(body, "\"baud\":115200,\"data_bits\":8") != NULL);
	assert(strstr(body, "\"generation\":9") != NULL);
	assert(strstr(body, "\"dtr\":true,\"rts\":false,\"usb_configured\":true") != NULL);
	assert(strstr(body, "\"total\":4,\"cdc\":1,\"ws\":1,\"tcp\":2") != NULL);
	assert(strstr(body, "\"bind\":\"0.0.0.0\",\"port\":5555,\"max_clients\":2") != NULL);
	assert(strstr(body, "\"drops\":8") != NULL);

	memset(&response, 0, sizeof(response));
	client.method = HTTP_POST;
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_405_METHOD_NOT_ALLOWED);

	memset(&response, 0, sizeof(response));
	client.method = HTTP_PUT;
	strcpy(client.url_buffer, "/api/v1/serial/j4/config");
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE,
	      "{\"baud\":230400,\"data_bits\":8,\"parity\":\"even\",",
	      &response);
	assert(serial_config_calls == 0U);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
	      "\"stop_bits\":1,\"flow_control\":\"none\","
	      "\"expected_generation\":9}", &response);
	assert(response.status == HTTP_202_ACCEPTED);
	assert(serial_config_calls == 1U);
	assert(serial_config_request.baud == 230400U);
	assert(serial_config_request.parity == LINKR_DEBUGGER_SERIAL_PARITY_EVEN);
	assert(serial_config_expected_generation == 9U);
	assert(strstr((const char *)response.body,
		      "\"accepted\":true,\"request_id\":77") != NULL);

	memset(&response, 0, sizeof(response));
	serial_status.generation = 10U;
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, config_json, &response);
	assert(response.status == HTTP_409_CONFLICT);
	assert(serial_config_calls == 1U);
	assert(strstr((const char *)response.body,
		      "\"code\":\"generation_conflict\"") != NULL);

	memset(&response, 0, sizeof(response));
	serial_status.generation = 9U;
	serial_config_result = -EBUSY;
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, config_json, &response);
	assert(response.status == HTTP_409_CONFLICT);
	assert(serial_config_calls == 2U);
	assert(strstr((const char *)response.body,
		      "\"code\":\"configuration_busy\"") != NULL);

	memset(&response, 0, sizeof(response));
	serial_config_result = 0;
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
	      "{\"baud\":230400,\"data_bits\":8,\"parity\":\"none\","
	      "\"stop_bits\":1,\"flow_control\":\"rts_cts\","
	      "\"expected_generation\":9}", &response);
	assert(response.status == HTTP_400_BAD_REQUEST);
	assert(serial_config_calls == 2U);

	memset(&response, 0, sizeof(response));
	client.method = HTTP_GET;
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_405_METHOD_NOT_ALLOWED);
}

static void test_serial_status_reports_unavailable_without_secrets(void)
{
	struct http_response_ctx response = { 0 };
	struct http_client_ctx client = { .method = HTTP_GET };

	reset_test();
	serial_status_result = -ENODEV;
	strcpy(client.url_buffer, "/api/v1/serial/j4");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_200_OK);
	assert(strstr((const char *)response.body,
		      "\"available\":false,\"state\":\"unavailable\"") != NULL);
	assert(strstr((const char *)response.body, "password") == NULL);
	assert(strstr((const char *)response.body, "token") == NULL);
}

static void test_serial_config_poison_survives_final_until_complete(void)
{
	static const char legal[] =
		"{\"baud\":115200,\"data_bits\":8,\"parity\":\"none\","
		"\"stop_bits\":1,\"flow_control\":\"none\","
		"\"expected_generation\":9}";
	char overflow[LINKR_DEBUGGER_HTTP_BODY_CAP + 2U];
	struct http_response_ctx response = { 0 };
	struct http_client_ctx client = { .method = HTTP_PUT };

	reset_test();
	memset(overflow, 'x', sizeof(overflow));
	overflow[sizeof(overflow) - 1U] = '\0';
	strcpy(client.url_buffer, "/api/v1/serial/j4/config");
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, overflow, &response);
	assert(response.status == HTTP_413_PAYLOAD_TOO_LARGE);
	assert(serial_config_calls == 0U);

	/* The body accumulator cleared its slot on overflow, but the serial
	 * route poison prevents this legal FINAL fragment becoming a request.
	 */
	memset(&response, 0, sizeof(response));
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, legal, &response);
	assert(!response.final_chunk);
	assert(serial_config_calls == 0U);
	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);

	memset(&response, 0, sizeof(response));
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, legal, &response);
	assert(response.status == HTTP_202_ACCEPTED);
	assert(serial_config_calls == 1U);

	memset(&response, 0, sizeof(response));
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, overflow, &response);
	assert(response.status == HTTP_413_PAYLOAD_TOO_LARGE);
	route(&client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, &response);
	memset(&response, 0, sizeof(response));
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, legal, &response);
	assert(response.status == HTTP_202_ACCEPTED);
	assert(serial_config_calls == 2U);
}

static void test_serial_config_rejects_wide_values_before_narrowing(void)
{
	static const char *const invalid[] = {
		"{\"baud\":4294967296,\"data_bits\":8,\"parity\":\"none\","
		"\"stop_bits\":1,\"flow_control\":\"none\","
		"\"expected_generation\":9}",
		"{\"baud\":115200,\"data_bits\":261,\"parity\":\"none\","
		"\"stop_bits\":1,\"flow_control\":\"none\","
		"\"expected_generation\":9}",
		"{\"baud\":115200,\"data_bits\":8,\"parity\":\"none\","
		"\"stop_bits\":257,\"flow_control\":\"none\","
		"\"expected_generation\":9}",
		"{\"baud\":115200,\"data_bits\":-1,\"parity\":\"none\","
		"\"stop_bits\":1,\"flow_control\":\"none\","
		"\"expected_generation\":9}",
	};
	struct http_client_ctx client = { .method = HTTP_PUT };

	reset_test();
	strcpy(client.url_buffer, "/api/v1/serial/j4/config");
	for (size_t i = 0U; i < ARRAY_SIZE(invalid); ++i) {
		struct http_response_ctx response = { 0 };

		route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, invalid[i], &response);
		assert(response.status == HTTP_400_BAD_REQUEST);
		assert(serial_config_calls == 0U);
		route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	}
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
	assert(LINKR_DEBUGGER_HTTP_STATUS_JSON_BUFSZ - response.body_len >= 256U);
	body = (const char *)response.body;
	assert(count_substring(body, "\"gp\":") == HTTP_TEST_RAIL_COUNT);
	assert(count_substring(body, "\"adc_index\":") == HTTP_TEST_ADC_COUNT);
	assert(count_substring(body, "\"note\":\"PD input divider; ADC pad voltage\"") == 1U);
	assert(count_substring(body, "\"layoutGroup\":") == HTTP_TEST_MAPPED_GPIO_COUNT);
	assert(count_substring(body, "\"direction\":") == HTTP_TEST_SAFE_GPIO_COUNT);
	assert(strstr(body,
		      "\"note\":\"J8_PIN1\",\"layoutGroup\":\"J8\","
		      "\"layoutLabel\":\"GP12\",\"layoutRow\":9,\"layoutColumn\":0") != NULL);
	assert(strstr(body,
		      "\"note\":\"CON_USER\",\"layoutGroup\":\"J13\","
		      "\"layoutLabel\":\"GP9 / USER\",\"layoutRow\":0,"
		      "\"layoutColumn\":0") != NULL);
	assert(strstr(body, "\"name\":\"GP17\"") == NULL);
	assert(strstr(body, "\"name\":\"GP18\"") == NULL);
	assert(strstr(body, "\"fixed_profiles\":[") != NULL);
	assert(strstr(body, "\"avs_ranges\":[") != NULL);
	assert(strstr(body, "\"board_monitoring\":{\"temperature\":{\"available\":true") != NULL);
	assert(strstr(body, "\"config\":{\"available\":true,\"reason\":\"ready\"") != NULL);
	{
		const char *adc_names[] = {
			"\"name\":\"5v_out\"", "\"name\":\"12v_out\"",
			"\"name\":\"20v_out\"", "\"name\":\"vdd_5v\"",
			"\"name\":\"adc0\"", "\"name\":\"adc1\"",
			"\"name\":\"adc2\"", "\"name\":\"adc3\"",
		};
		const char *position = strstr(body, "\"adc_channels\":[");

		assert(position != NULL);
		for (size_t i = 0U; i < ARRAY_SIZE(adc_names); i++) {
			position = strstr(position, adc_names[i]);
			assert(position != NULL);
			position++;
		}
		assert(strstr(body,
			"\"name\":\"adc0\",\"signal\":\"GPIO26_ADC0\",\"adc_index\":0,"
			"\"sensor\":\"ADC\",\"sensor_channel\":\"voltage\",\"unit\":\"V\","
			"\"note\":\"PD input divider; ADC pad voltage\"") != NULL);
	}
	match = strstr(body, expected_groups);
	assert(match != NULL);
	assert(strstr(match + 1, "\"gpio_layout_groups\"") == NULL);
	printf("HTTP status full fixture: rails=%u adc=%u gpio=%u mapped=%u groups=%zu, "
	       "%zu/%u bytes (%zu-byte margin)\n",
	       HTTP_TEST_RAIL_COUNT, HTTP_TEST_ADC_COUNT, HTTP_TEST_SAFE_GPIO_COUNT,
	       HTTP_TEST_MAPPED_GPIO_COUNT, linkr_debugger_gpio_layout_group_count, response.body_len,
	       LINKR_DEBUGGER_HTTP_STATUS_JSON_BUFSZ,
	       LINKR_DEBUGGER_HTTP_STATUS_JSON_BUFSZ - response.body_len);

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
	assert(response.status == HTTP_403_FORBIDDEN);
	body = (const char *)response.body;
	assert(strstr(body, "\"code\":\"not_allowed\"") != NULL);

	memset(&response, 0, sizeof(response));
	strcpy(client.url_buffer, "/api/v1/gpio/GP18");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_403_FORBIDDEN);
	assert(strstr((const char *)response.body, "\"code\":\"not_allowed\"") != NULL);
}

static void test_gpio_layout_catalog_encoder_is_exact_bounded_and_utf8_safe(void)
{
	static const struct linkr_debugger_gpio_layout_group_desc groups[] = {
		{ .name = "J\"\\\n\xe9\x9b\xaa", .edge = "right", .order = 7U,
		  .rows = 1U, .columns = 1U },
	};
	static const struct linkr_debugger_gpio_layout_cell_desc cells[] = {
		{ .group_order = 7U, .pin_number = 1U, .row = 0U, .column = 0U,
		  .label = "L\"\\\n\x01\xe9\x9b\xaa", .signal = "GP09_USER",
		  .role = "gpio", .controllable = true, .output_capable = false,
		  .gpio_pin = 9U },
	};
	static const struct linkr_debugger_gpio_layout_cell_desc invalid_utf8[] = {
		{ .group_order = 7U, .pin_number = 1U, .row = 0U, .column = 0U,
		  .label = "\xc0\x80", .signal = "GP09_USER", .role = "gpio" },
	};
	static const char expected[] =
		"{\"view\":\"component-top\",\"groups\":[{\"name\":\"J\\\"\\\\\\n"
		"\xe9\x9b\xaa\",\"edge\":\"right\",\"order\":7,\"rows\":1,"
		"\"columns\":1,\"cells\":[{\"pinNumber\":1,\"row\":0,\"column\":0,"
		"\"label\":\"L\\\"\\\\\\n\\u0001\xe9\x9b\xaa\","
		"\"signal\":\"GP09_USER\",\"role\":\"gpio\",\"controllable\":true,"
		"\"outputCapable\":false,\"gpioName\":\"GP9\"}]}]}";
	char exact[sizeof(expected)];
	char short_buffer[sizeof(expected) - 1U];
	struct linkr_debugger_http_env exact_env = {
		.buf = exact,
		.cap = sizeof(exact),
	};
	struct linkr_debugger_http_env short_env = {
		.buf = short_buffer,
		.cap = sizeof(short_buffer),
	};
	struct linkr_debugger_http_env invalid_env = {
		.buf = exact,
		.cap = sizeof(exact),
	};

	assert(linkr_debugger_http_json_gpio_layout_catalog_from(
		       &exact_env, "component-top", groups, ARRAY_SIZE(groups),
		       cells, ARRAY_SIZE(cells)) == 0);
	assert(exact_env.len == sizeof(expected) - 1U);
	assert(strcmp(exact, expected) == 0);
	assert(linkr_debugger_http_json_gpio_layout_catalog_from(
		       &short_env, "component-top", groups, ARRAY_SIZE(groups),
		       cells, ARRAY_SIZE(cells)) == -ENOMEM);
	assert(short_env.truncated);
	assert(linkr_debugger_http_json_gpio_layout_catalog_from(
		       &invalid_env, "component-top", groups, ARRAY_SIZE(groups),
		       invalid_utf8, ARRAY_SIZE(invalid_utf8)) == -EINVAL);
	assert(invalid_env.len == 0U);
}

static void test_gpio_layout_endpoint_is_static_complete_and_get_only(void)
{
	struct http_response_ctx response = { 0 };
	struct http_client_ctx client = { .method = HTTP_GET };
	const char *body;

	reset_test();
	strcpy(client.url_buffer, "/api/v1/gpio/layout");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_200_OK);
	assert(response.final_chunk);
	assert(response.body_len == strlen((const char *)response.body));
	assert(response.body_len < LINKR_DEBUGGER_HTTP_JSON_BUFSZ);
	printf("HTTP GPIO layout fixture: %zu/%u bytes (%zu-byte margin)\n",
	       response.body_len, LINKR_DEBUGGER_HTTP_JSON_BUFSZ,
	       LINKR_DEBUGGER_HTTP_JSON_BUFSZ - response.body_len);
	fflush(stdout);
	assert(LINKR_DEBUGGER_HTTP_JSON_BUFSZ - response.body_len >= 512U);
	body = (const char *)response.body;
	assert(strstr(body,
		      "{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,"
		      "\"command\":\"gpio\",\"action\":\"layout\",\"layout\":{"
		      "\"view\":\"component-top\",\"groups\":[") == body);
	assert(count_substring(body, "\"pinNumber\":") == HTTP_TEST_LAYOUT_CELL_COUNT);
	assert(count_substring(body, "\"gpioName\":") == HTTP_TEST_SAFE_GPIO_COUNT);
	assert(count_substring(body, "\"controllable\":true") == HTTP_TEST_SAFE_GPIO_COUNT);
	assert(strstr(body,
		      "{\"pinNumber\":10,\"row\":0,\"column\":0,"
		      "\"label\":\"GP21 / SDA\",\"signal\":\"RP2354_SDA\","
		      "\"role\":\"i2c\",\"controllable\":false,"
		      "\"outputCapable\":false}") != NULL);
	assert(strstr(body,
		      "{\"pinNumber\":6,\"row\":4,\"column\":0,"
		      "\"label\":\"GP29 / ADC3\",\"signal\":\"GPIO29_ADC3\","
		      "\"role\":\"adc\",\"controllable\":true,"
		      "\"outputCapable\":false,\"gpioName\":\"GP29\"}") != NULL);
	assert(strstr(body, "\"direction\":") == NULL);
	assert(strstr(body, "\"value\":") == NULL);
	memset(&response, 0, sizeof(response));
	client.method = HTTP_POST;
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_405_METHOD_NOT_ALLOWED);
	assert(strstr((const char *)response.body,
		      "\"code\":\"method_not_allowed\"") != NULL);
}

static void test_adc_read_route_emits_eight_readings_and_adc0_note(void)
{
	struct http_response_ctx response = { 0 };
	struct http_client_ctx client = { .method = HTTP_GET };
	const char *body;

	reset_test();
	strcpy(client.url_buffer, "/api/v1/adc/read");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_200_OK);
	assert(LINKR_DEBUGGER_HTTP_JSON_BUFSZ == 4096U);
	assert(response.body_len < LINKR_DEBUGGER_HTTP_JSON_BUFSZ);
	body = (const char *)response.body;
	assert(count_substring(body, "\"sensor_channel\":") == HTTP_TEST_ADC_COUNT);
	assert(count_substring(body, "\"sensor_channel\":\"current\"") == 4U);
	assert(count_substring(body, "\"sensor_channel\":\"voltage\"") == 4U);
	assert(count_substring(body, "\"note\":\"PD input divider; ADC pad voltage\"") == 1U);
	{
		const char *adc_names[] = {
			"\"name\":\"5v_out\"", "\"name\":\"12v_out\"",
			"\"name\":\"20v_out\"", "\"name\":\"vdd_5v\"",
			"\"name\":\"adc0\"", "\"name\":\"adc1\"",
			"\"name\":\"adc2\"", "\"name\":\"adc3\"",
		};
		const char *position = body;

		for (size_t i = 0U; i < ARRAY_SIZE(adc_names); i++) {
			position = strstr(position, adc_names[i]);
			assert(position != NULL);
			position++;
		}
	}
	assert(strstr(body,
		"{\"name\":\"adc0\",\"signal\":\"GPIO26_ADC0\","
		"\"note\":\"PD input divider; ADC pad voltage\",\"raw\":2000,"
		"\"mv\":1200,\"sensor_channel\":\"voltage\",\"unit\":\"V\","
		"\"sensor_value\":{\"val1\":1,\"val2\":200000}}") != NULL);
	assert(strstr(body, "\"name\":\"adc3\",\"signal\":\"GPIO29_ADC3\"") != NULL);

	memset(&response, 0, sizeof(response));
	strcpy(client.url_buffer, "/api/v1/adc/read?channel=adc0");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_200_OK);
	body = (const char *)response.body;
	assert(count_substring(body, "\"sensor_channel\":") == 1U);
	assert(strstr(body, "\"name\":\"adc0\"") != NULL);
	assert(strstr(body, "\"name\":\"adc1\"") == NULL);
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

static void test_status_budget_rejects_inflated_adc_note_metadata(void)
{
	char inflated_note[5000];
	struct http_response_ctx response = { 0 };
	struct http_client_ctx client = { .method = HTTP_GET };
	const char *note = linkr_debugger_currents[4].note;

	reset_test();
	memset(inflated_note, 'N', sizeof(inflated_note) - 1U);
	inflated_note[sizeof(inflated_note) - 1U] = '\0';
	linkr_debugger_currents[4].note = inflated_note;
	strcpy(client.url_buffer, "/api/v1/status");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_500_INTERNAL_SERVER_ERROR);
	assert(strstr((const char *)response.body,
		      "\"code\":\"response_too_large\"") != NULL);
	linkr_debugger_currents[4].note = note;
}

static void test_gpio_workspace_preserves_4096_encoding_limit(void)
{
	char inflated_name[4096];
	struct http_response_ctx response = { 0 };
	struct http_client_ctx client = { .method = HTTP_GET };

	reset_test();
	memset(inflated_name, 'G', sizeof(inflated_name) - 1U);
	inflated_name[sizeof(inflated_name) - 1U] = '\0';
	linkr_debugger_gpio_layout_groups[0].name = inflated_name;
	memset(linkr_debugger_http_json_workspace, 0xa5,
	       sizeof(linkr_debugger_http_json_workspace));
	strcpy(client.url_buffer, "/api/v1/gpio");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_500_INTERNAL_SERVER_ERROR);
	assert(response.body == linkr_debugger_http_json_workspace);
	assert(linkr_debugger_http_json_workspace[4096U] == 0xa5);
	assert(strstr((const char *)response.body,
		      "\"code\":\"response_too_large\"") != NULL);
	linkr_debugger_gpio_layout_groups[0].name = "J8";
}

int main(void)
{
	test_shared_json_workspace_static_ram_contract();
	test_handlers_share_workspace_until_synchronous_send_finishes();
	test_fragmented_put_executes_once_with_complete_json();
	test_aborted_and_complete_clear_without_executing();
	test_invalid_prefix_cannot_be_replaced_by_valid_final_fragment();
	test_pd_fragmented_put_executes_once_with_complete_json();
	test_pd_aborted_request_clears_partial_body();
	test_pd_complete_request_clears_partial_body();
	test_pd_invalid_prefix_cannot_be_replaced_by_valid_final_fragment();
	test_serial_status_config_and_methods();
	test_serial_status_reports_unavailable_without_secrets();
	test_serial_config_poison_survives_final_until_complete();
	test_serial_config_rejects_wide_values_before_narrowing();
	test_gpio_layout_group_encoder_is_generic_and_bounded();
	test_gpio_layout_catalog_encoder_is_exact_bounded_and_utf8_safe();
	test_gpio_layout_endpoint_is_static_complete_and_get_only();
	test_gpio_layout_groups_are_top_level_http_contract();
	test_adc_read_route_emits_eight_readings_and_adc0_note();
	test_status_budget_rejects_inflated_group_metadata();
	test_status_budget_rejects_inflated_adc_note_metadata();
	test_gpio_workspace_preserves_4096_encoding_limit();
	return 0;
}
