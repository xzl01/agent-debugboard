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
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Exercise production resource registration, fallback router and handlers. */
#include "../../src/linkr_debugger_serial_http.c"
#include "../../src/linkr_debugger_http_task_response.c"
#include "../../src/linkr_debugger_task_http.c"
#include "../../src/linkr_debugger_http.c"

static unsigned int setter_calls;
static struct linkr_debugger_net_identity setter_identity;
static struct linkr_debugger_net_identity current_identity = {
	.mac = { 0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U },
	.network = 0x0a000000U,
	.origin = LINKR_DEBUGGER_NET_ORIGIN_DERIVED,
};
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

/* Frozen firmware JSON vectors, also consumed by taskV2.test.ts. ir_bytes is
 * the standalone header + task section, not the section's bytes in full IR.
 */
#define TASK_TEST_ID "00010203-0405-0607-0809-0a0b0c0d0e0f"
#define TASK_TEST_CATALOG "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
#define TASK_TEST_A_SOURCE_DIGEST "bfd3006056fd18bce3a2b1db4818f96079030b0976d846b040efe2a72ee9e1f1"
#define TASK_TEST_A_IR_DIGEST "5c227cbb48ebc9e91d4a4c2fa60f9d5f4490f51e361110763ebc22cf641d4ef0"
#define TASK_TEST_B_IR_DIGEST "f7633e2af94cbd64a4fc28378a7790b4051f1576604ab287dc621dc13b5363ae"
#define TASK_TEST_FULL_IR_DIGEST "b2298f2e1f8b31e99a63a7916460941eea303dc34dc032b1b66a7e63021c5d3f"
#define TASK_TEST_IDENTITY \
	"\"task_store_id\":\"" TASK_TEST_ID "\",\"revision\":\"9007199254740993\"," \
	"\"catalog_fingerprint\":\"" TASK_TEST_CATALOG "\""
#define TASK_TEST_STORAGE \
	"\"storage\":{\"total_bytes\":131072,\"used_bytes\":8192," \
	"\"free_bytes\":122880,\"task_bytes\":1028," \
	"\"reserve_bytes\":8192,\"measured\":true,\"save_allowed\":true}"
#define TASK_TEST_A_METADATA \
	"{\"slot\":0,\"id\":\"A\",\"request_count\":1,\"source_state\":\"present\"," \
	"\"source_bytes\":148,\"source_digest\":\"" TASK_TEST_A_SOURCE_DIGEST "\"," \
	"\"ir_bytes\":100,\"task_ir_digest\":\"" TASK_TEST_A_IR_DIGEST "\"}"
static const char task_test_golden_list[] =
	"{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,\"command\":\"tasks\","
	"\"action\":\"list\"," TASK_TEST_IDENTITY "," TASK_TEST_STORAGE ",\"full_ir_bytes\":184,"
	"\"full_ir_digest\":\"" TASK_TEST_FULL_IR_DIGEST "\",\"tasks\":["
	TASK_TEST_A_METADATA ",{\"slot\":3,\"id\":\"B\",\"request_count\":1,"
	"\"source_state\":\"absent\",\"source_bytes\":0,\"source_digest\":null,"
	"\"ir_bytes\":100,\"task_ir_digest\":\"" TASK_TEST_B_IR_DIGEST "\"}]}\n";
static const char task_test_golden_upsert[] =
	"{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,\"command\":\"tasks\","
	"\"action\":\"upsert\"," TASK_TEST_IDENTITY "," TASK_TEST_STORAGE ",\"task\":" TASK_TEST_A_METADATA "}\n";
static const char task_test_golden_delete[] =
	"{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,\"command\":\"tasks\","
	"\"action\":\"delete\"," TASK_TEST_IDENTITY "," TASK_TEST_STORAGE ",\"task\":null}\n";
static const char task_test_golden_conflict[] =
	"{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":false,\"command\":\"tasks\","
	"\"error\":{\"code\":\"revision_conflict\",\"message\":\"task revision changed\"},"
	TASK_TEST_IDENTITY "}\n";
static const char task_test_source[] =
	"{\"schema\":\"linkr-test.v1\",\"name\":\"A\",\"version\":\"1.0\","
	"\"created\":\"2026-10-22T00:00:00.000Z\"}\n"
	"{\"id\":\"s1\",\"params\":{\"rail\":\"5v_out\"},\"type\":\"power_on\"}\n";
static const char task_test_ir[] =
	"# linkr-task.v1\n# task A\n"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":\"{\\\"state\\\":\\\"on\\\"}\"}\n"
	"# task B\n"
	"{\"method\":\"PUT\",\"path\":\"/api/v1/power/5v_out\",\"body\":\"{\\\"state\\\":\\\"on\\\"}\"}\n";
static const char task_test_mutation_template[] =
	"{\"schema\":\"radxa-linkr-debugger.v1\",\"action\":\"%s\","
	"\"expected_revision\":\"9007199254740993\",\"catalog_fingerprint\":\""
	TASK_TEST_CATALOG "\"}";
static char task_test_mutation[256];
static size_t task_test_mutation_len;
static struct linkr_debugger_task_status task_test_status;
static enum linkr_debugger_task_result task_test_mutation_result;
static enum linkr_debugger_task_result task_test_source_result;
static unsigned int task_test_mutation_calls;
static unsigned int task_test_source_calls;
static uint8_t task_test_source_slot;

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
	/* Like Zephyr, decode the FIRST object and do not enforce end-of-input.
	 * Production envelope validation, not a stricter stub, must reject suffixes.
	 */
	char *object = text;
	while (*object == ' ' || *object == '\t' || *object == '\r' || *object == '\n') {
		object++;
	}

	if (descr_len == 1U) {
		if (descr[0].type == JSON_TOK_STRING_BUF) {
			char route[16];

			/* Deliberately leave unknown-field/container validation to the
			 * production scanner. Requiring '}' here would mask the pinned
			 * Zephyr skip_field() mismatched-container regression.
			 */
			if ((sscanf(object, "{\"route\":\"%15[^\"]\"%n", route, &consumed) != 1 ||
			     consumed == 0) &&
			    (sscanf(object, "{\"state\":\"%15[^\"]\"%n", route, &consumed) != 1 ||
			     consumed == 0)) {
				return -EINVAL;
			}
			strcpy((char *)value + descr[0].offset, route);
			return BIT64(0);
		}
		return -EINVAL;
	}
	if (descr_len == 2U && descr[0].type == JSON_TOK_STRING_BUF) {
		if (descr[1].type == JSON_TOK_NUMBER) {
			char direction[8];
			int level;

			if (sscanf(object, "{\"direction\":\"%7[^\"]\",\"value\":%d}%n", direction,
				   &level, &consumed) != 2 ||
			    consumed == 0) {
				return -EINVAL;
			}
			strcpy((char *)value + descr[0].offset, direction);
			*(int *)((char *)value + descr[1].offset) = level;
			return BIT64(0) | BIT64(1);
		}
		char mac[LINKR_DEBUGGER_NET_MAC_STR_LEN] = { 0 };
		char network[LINKR_DEBUGGER_NET_NETWORK_STR_LEN] = { 0 };
		int mac_consumed = 0;
		int network_consumed = 0;
		int matched;

		matched = sscanf(object, "{\"mac\":\"%17[^\"]\",\"network\":\"%19[^\"]\"}%n", mac,
				 network, &consumed);
		if (matched == 2 && consumed > 0) {
			strcpy((char *)value + descr[0].offset, mac);
			strcpy((char *)value + descr[1].offset, network);
			return BIT64(0) | BIT64(1);
		}
		matched = sscanf(object, "{\"mac\":\"%17[^\"]\"}%n", mac, &mac_consumed);
		if (matched == 1 && mac_consumed > 0) {
			strcpy((char *)value + descr[0].offset, mac);
			return BIT64(0);
		}
		matched =
			sscanf(object, "{\"network\":\"%19[^\"]\"}%n", network, &network_consumed);
		if (matched == 1 && network_consumed > 0) {
			strcpy((char *)value + descr[1].offset, network);
			return BIT64(1);
		}
		return -EINVAL;
	}
	if (descr_len == 2U) {
		int mode;
		int voltage_mv;

		if (sscanf(object, "{\"mode\":%d,\"voltage_mv\":%d}%n", &mode, &voltage_mv,
			   &consumed) != 2 ||
		    consumed == 0) {
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

		if (sscanf(object,
			   "{\"baud\":%lld,\"data_bits\":%lld,\"parity\":\"%7[^\"]\","
			   "\"stop_bits\":%lld,\"flow_control\":\"%11[^\"]\","
			   "\"expected_generation\":%llu}%n",
			   &baud, &data_bits, parity, &stop_bits, flow_control,
			   &expected_generation, &consumed) != 6 ||
		    consumed == 0) {
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

int linkr_debugger_net_format_ipv4(uint32_t network, uint8_t host,
				   char *buf, size_t cap)
{
	uint32_t addr = network + host;
	int len = snprintf(buf, cap, "%u.%u.%u.%u",
			   (unsigned int)((addr >> 24) & 0xffU),
			   (unsigned int)((addr >> 16) & 0xffU),
			   (unsigned int)((addr >> 8) & 0xffU),
			   (unsigned int)(addr & 0xffU));

	return len < 0 || (size_t)len >= cap ? -ENOSPC : 0;
}

int linkr_debugger_net_format_mac(const uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN],
				  char *buf, size_t cap)
{
	int len = snprintf(buf, cap, "%02x:%02x:%02x:%02x:%02x:%02x",
			   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	return len < 0 || (size_t)len >= cap ? -ENOSPC : 0;
}

int linkr_debugger_net_format_network(uint32_t network, char *buf, size_t cap)
{
	char ip[LINKR_DEBUGGER_NET_IPV4_STR_LEN];
	int len;

	if (linkr_debugger_net_format_ipv4(network, 0U, ip, sizeof(ip)) < 0) {
		return -ENOSPC;
	}
	len = snprintf(buf, cap, "%s/30", ip);
	return len < 0 || (size_t)len >= cap ? -ENOSPC : 0;
}

int linkr_debugger_net_format_url(uint32_t network, char *buf, size_t cap)
{
	char ip[LINKR_DEBUGGER_NET_IPV4_STR_LEN];
	int len;

	if (linkr_debugger_net_format_ipv4(network, LINKR_DEBUGGER_NET_DEVICE_HOST,
					   ip, sizeof(ip)) < 0) {
		return -ENOSPC;
	}
	len = snprintf(buf, cap, "http://%s/", ip);
	return len < 0 || (size_t)len >= cap ? -ENOSPC : 0;
}

int linkr_debugger_net_parse_mac(const char *text,
				 uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN])
{
	unsigned int b[LINKR_DEBUGGER_NET_MAC_LEN];
	char tail;

	if (text == NULL || mac == NULL ||
	    sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2],
		   &b[3], &b[4], &b[5], &tail) != 6) {
		return -EINVAL;
	}
	for (size_t i = 0U; i < LINKR_DEBUGGER_NET_MAC_LEN; i++) {
		mac[i] = (uint8_t)b[i];
	}
	return (mac[0] & 0x03U) == 0x02U ? 0 : -EINVAL;
}

int linkr_debugger_net_parse_network(const char *text, uint32_t *network)
{
	unsigned int a, b, c, d;
	int used = 0;
	uint32_t value;

	if (text == NULL || network == NULL) {
		return -EINVAL;
	}
	if (sscanf(text, "%u.%u.%u.%u%n", &a, &b, &c, &d, &used) != 4 ||
	    used <= 0 || (text[used] != '\0' && strcmp(text + used, "/30") != 0) ||
	    a > 255U || b > 255U || c > 255U || d > 255U) {
		return -EINVAL;
	}
	value = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
		((uint32_t)c << 8) | (uint32_t)d;
	if ((value & 0x03U) != 0U ||
	    !((value & 0xFF000000U) == 0x0A000000U ||
	      (value & 0xFFF00000U) == 0xAC100000U ||
	      (value & 0xFFFF0000U) == 0xC0A80000U)) {
		return -EINVAL;
	}
	*network = value;
	return 0;
}

int linkr_debugger_network_identity(struct linkr_debugger_net_identity *identity)
{
	if (identity == NULL) {
		return -EINVAL;
	}
	*identity = current_identity;
	return 0;
}

int linkr_debugger_network_set_identity_with_result(
	const struct linkr_debugger_net_identity *identity, bool *reconnect_required)
{
	setter_calls++;
	setter_identity = *identity;
	if (reconnect_required != NULL) {
		*reconnect_required =
			memcmp(current_identity.mac, identity->mac,
			       LINKR_DEBUGGER_NET_MAC_LEN) != 0 ||
			current_identity.network != identity->network;
	}
	current_identity = *identity;
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

/* Storage is isolated here; the callback, response encoder, registered
 * resources and fallback dispatch are production implementations.
 */
enum linkr_debugger_task_result linkr_debugger_task_status_get(
	struct linkr_debugger_task_status *status)
{
	*status = task_test_status;
	return LINKR_DEBUGGER_TASK_OK;
}

enum linkr_debugger_task_result linkr_debugger_task_ir_snapshot(
	uint8_t *ir, size_t cap, size_t *len)
{
	assert(sizeof(task_test_ir) - 1U <= cap);
	*len = sizeof(task_test_ir) - 1U;
	memcpy(ir, task_test_ir, *len);
	return LINKR_DEBUGGER_TASK_OK;
}

enum linkr_debugger_task_result linkr_debugger_task_source_snapshot(
	uint8_t slot, uint8_t *source, size_t cap, size_t *len)
{
	task_test_source_calls++;
	task_test_source_slot = slot;
	assert(sizeof(task_test_source) - 1U <= cap);
	*len = sizeof(task_test_source) - 1U;
	memcpy(source, task_test_source, *len);
	return task_test_source_result;
}

enum linkr_debugger_task_result linkr_debugger_task_mutation_execute(
	uint8_t *body, size_t len, struct linkr_debugger_task_mutation_outcome *outcome,
	enum linkr_debugger_task_mutation_opcode *opcode)
{
	task_test_mutation_calls++;
	assert(len == task_test_mutation_len);
	assert(memcmp(body, task_test_mutation, len) == 0);
	if (strstr((const char *)body, "\"action\":\"upsert\"") != NULL) {
		*opcode = LINKR_DEBUGGER_TASK_MUTATION_UPSERT;
	} else if (strstr((const char *)body, "\"action\":\"delete\"") != NULL) {
		*opcode = LINKR_DEBUGGER_TASK_MUTATION_DELETE;
	} else {
		*opcode = LINKR_DEBUGGER_TASK_MUTATION_CLEAR;
	}
	outcome->status = task_test_status;
	outcome->affected_task_present = *opcode == LINKR_DEBUGGER_TASK_MUTATION_UPSERT;
	outcome->affected_slot = 0U;
	return task_test_mutation_result;
}

static void task_test_set_mutation_body(const char *action)
{
	task_test_mutation_len = (size_t)snprintf(task_test_mutation,
		sizeof(task_test_mutation), task_test_mutation_template, action);
}

const uint8_t *linkr_debugger_task_catalog_json(size_t *len)
{
	static const uint8_t catalog[] =
		"{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,"
		"\"command\":\"tasks\",\"action\":\"catalog\",\"tasks\":[]}\n";

	*len = sizeof(catalog) - 1U;
	return catalog;
}

static void task_test_digest(uint8_t bytes[32], const char *hex)
{
	for (size_t i = 0U; i < 32U; i++) {
		unsigned int byte;

		assert(sscanf(hex + 2U * i, "%2x", &byte) == 1);
		bytes[i] = (uint8_t)byte;
	}
}

static void task_test_reset(void)
{
	task_test_status = (struct linkr_debugger_task_status){
		.backend_available = true,
		.storage = {
			.total_bytes = 131072U,
			.used_bytes = 8192U,
			.free_bytes = 122880U,
			.task_bytes = 1028U,
			.reserve_bytes = 8192U,
			.measured = true,
			.save_allowed = true,
		},
		.revision = UINT64_C(9007199254740993),
		.full_ir_len = sizeof(task_test_ir) - 1U,
		.task_count = 2U,
		.tasks = {
			{ .slot = 0U, .id = "A", .request_count = 1U,
			  .source_present = true, .source_len = sizeof(task_test_source) - 1U,
			  .ir_len = 100U },
			{ .slot = 3U, .id = "B", .request_count = 1U, .ir_len = 100U },
		},
	};
	for (size_t i = 0U; i < sizeof(task_test_status.task_store_id); i++) {
		task_test_status.task_store_id[i] = (uint8_t)i;
	}
	task_test_digest(task_test_status.catalog_fingerprint, TASK_TEST_CATALOG);
	task_test_digest(task_test_status.full_ir_digest, TASK_TEST_FULL_IR_DIGEST);
	task_test_digest(task_test_status.tasks[0].source_digest, TASK_TEST_A_SOURCE_DIGEST);
	task_test_digest(task_test_status.tasks[0].ir_digest, TASK_TEST_A_IR_DIGEST);
	task_test_digest(task_test_status.tasks[1].ir_digest, TASK_TEST_B_IR_DIGEST);
	task_test_mutation_result = LINKR_DEBUGGER_TASK_OK;
	task_test_source_result = LINKR_DEBUGGER_TASK_OK;
	task_test_set_mutation_body("clear");
	task_test_mutation_calls = 0U;
	task_test_source_calls = 0U;
	task_test_source_slot = UINT8_MAX;
	memset(&task_input_state, 0, sizeof(task_input_state));
	task_response_owner = NULL;
}

static void reset_test(void)
{
	static const char *const directions[] = { "input", "output" };

	setter_calls = 0U;
	http_test_uptime_ms = 1;
	http_test_control_calls = 0U;
	http_test_gpio_set_result = 0;
	memset(&setter_identity, 0, sizeof(setter_identity));
	current_identity = (struct linkr_debugger_net_identity){
		.mac = { 0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U },
		.network = 0x0a000000U,
		.origin = LINKR_DEBUGGER_NET_ORIGIN_DERIVED,
	};
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
			.invalid_line_coding = 3U,
			.config_mailbox_busy = 4U,
		},
		.config_counters = {
			.cdc_requests = 5U,
			.web_requests = 6U,
			.invalid = 7U,
			.conflicts = 8U,
			.no_change = 9U,
			.configure_count = 10U,
			.apply_succeeded = 11U,
			.apply_failed = 12U,
			.rollback_succeeded = 13U,
			.rollback_failed = 14U,
		},
		.generation = 9U,
		.cdc_responder_generation = 15U,
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
	for (const struct linkr_debugger_http_resource_stub *resource = __start_http_test_resources;
	     resource < __stop_http_test_resources; resource++) {
		struct http_resource_detail_common *common = resource->detail;

		if (common->type == HTTP_RESOURCE_TYPE_DYNAMIC) {
			((struct http_resource_detail_dynamic *)common)->holder = NULL;
		}
	}
	fallback_resource_detail.holder = NULL;
	task_test_reset();
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

/* Model the pinned Zephyr HTTP/1 contract, not direct router calls:
 * get_resource_detail() matches exact paths (ignoring query), then fallback;
 * handle_http1_dynamic_resource() acquires holder before delivering data;
 * client_release_resources() only scans REGISTERED dynamic details, clears
 * holder, then calls ABORTED before the context can be reinitialized/reused.
 */
static struct http_resource_detail_dynamic *transport_detail(const char *path)
{
	size_t len = strcspn(path, "?");
	struct http_resource_detail_common *common = linkr_debugger_http_service.res_fallback;

	for (const struct linkr_debugger_http_resource_stub *resource = __start_http_test_resources;
	     resource < __stop_http_test_resources; resource++) {
		if (strlen(resource->path) == len && memcmp(path, resource->path, len) == 0) {
			common = resource->detail;
			break;
		}
	}
	assert(common != NULL && common->type == HTTP_RESOURCE_TYPE_DYNAMIC);
	return (struct http_resource_detail_dynamic *)common;
}

static void transport_route(struct http_client_ctx *client, enum http_transaction_status status,
			    const char *fragment, struct http_response_ctx *response)
{
	struct http_resource_detail_dynamic *detail = transport_detail(client->url_buffer);
	struct http_request_ctx request = {
		.data = (char *)fragment,
		.data_len = fragment != NULL ? strlen(fragment) : 0U,
	};

	assert(detail->common.bitmask_of_supported_http_methods & BIT(client->method));
	assert(detail->holder == NULL || detail->holder == client);
	detail->holder = client;
	assert(detail->cb(client, status, &request, response, detail->user_data) == 0);
	if (status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		detail->holder = NULL;
	}
}

static size_t transport_disconnect(struct http_client_ctx *client)
{
	size_t callbacks = 0U;

	for (const struct linkr_debugger_http_resource_stub *resource = __start_http_test_resources;
	     resource < __stop_http_test_resources; resource++) {
		struct http_resource_detail_common *common = resource->detail;
		struct http_resource_detail_dynamic *detail = resource->detail;
		struct http_request_ctx request = {0};
		struct http_response_ctx response = {0};

		if (common->type != HTTP_RESOURCE_TYPE_DYNAMIC || detail->holder != client) {
			continue;
		}
		detail->holder = NULL;
		assert(detail->cb(client, HTTP_SERVER_TRANSACTION_ABORTED, &request, &response,
				  detail->user_data) == 0);
		assert(!response.final_chunk);
		callbacks++;
	}
	client->fd = -1;
	return callbacks;
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
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"mac\":\"02:aa:bb:cc:dd:ee\",", &response);
	assert(setter_calls == 0U);
	assert(!response.final_chunk);

	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
	      "\"network\":\"10.20.30.0\"}", &response);
	assert(setter_calls == 1U);
	assert(memcmp(setter_identity.mac,
		      (const uint8_t[]){ 0x02U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU },
		      LINKR_DEBUGGER_NET_MAC_LEN) == 0);
	assert(setter_identity.network == 0x0a141e00U);
	assert(setter_identity.origin == LINKR_DEBUGGER_NET_ORIGIN_MANUAL);
	assert(response.status == HTTP_200_OK);
	assert(response.final_chunk);
}

static void test_invalid_network_forms_are_rejected(void)
{
	static const char *const bodies[] = {
		"{\"network\":\"10.0.0.0/24\"}",
		"{\"network\":\"10.0.0.0/31\"}",
		"{\"network\":\"10.0.0.1/30\"}",
		"{\"network\":\"8.8.8.0/30\"}",
	};

	for (size_t i = 0U; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
		struct http_client_ctx client = net_put_client();
		struct http_response_ctx response = { 0 };

		reset_test();
		route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, bodies[i], &response);
		assert(setter_calls == 0U);
		assert(response.status == HTTP_400_BAD_REQUEST);
		assert(strstr((const char *)response.body,
			      "\"code\":\"invalid_network\"") != NULL);
	}
}

static void test_unchanged_identity_preserves_derived_origin(void)
{
	struct http_client_ctx client = net_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
	      "{\"mac\":\"02:11:22:33:44:55\",\"network\":\"10.0.0.0\"}",
	      &response);
	assert(setter_calls == 1U);
	assert(setter_identity.origin == LINKR_DEBUGGER_NET_ORIGIN_DERIVED);
	assert(response.status == HTTP_200_OK);
}

static void test_aborted_and_complete_clear_without_executing(void)
{
	struct http_client_ctx client = net_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"mac\":\"02:aa:bb:cc:dd:ee\",", &response);
	route(&client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, &response);
	assert(setter_calls == 0U);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "\"network\":\"10.20.30.0\"}", &response);
	assert(setter_calls == 0U);
	assert(response.status == HTTP_400_BAD_REQUEST);

	memset(&response, 0, sizeof(response));
	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"mac\":\"02:aa:bb:cc:dd:ee\",", &response);
	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	assert(setter_calls == 0U);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "\"network\":\"10.20.30.0\"}", &response);
	assert(setter_calls == 0U);
	assert(response.status == HTTP_400_BAD_REQUEST);
}

static void test_invalid_prefix_cannot_be_replaced_by_valid_final_fragment(void)
{
	struct http_client_ctx client = net_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "garbage", &response);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "{\"network\":\"10.20.30.0\"}", &response);
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

	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
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

	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
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
		"{\"baud\":57600,\"data_bits\":8,\"parity\":\"even\","
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
	assert(strstr(body, "\"responder_generation\":15") != NULL);
	assert(strstr(body,
	      "\"terminal_responder\":{\"transport\":\"cdc\",\"slot\":0}") !=
	       NULL);
	assert(strstr(body,
	      "\"config_counters\":{\"cdc_requests\":5,\"web_requests\":6,"
	      "\"invalid\":7,\"conflicts\":8,\"no_change\":9,"
	      "\"configure_count\":10,\"apply_succeeded\":11,"
	      "\"apply_failed\":12,\"rollback_succeeded\":13,"
	      "\"rollback_failed\":14}") != NULL);
	assert(strstr(body, "\"invalid_line_coding\":3") != NULL);
	assert(strstr(body, "\"config_mailbox_busy\":4") != NULL);
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
	      "{\"baud\":57600,\"data_bits\":8,\"parity\":\"even\",",
	      &response);
	assert(serial_config_calls == 0U);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
	      "\"stop_bits\":1,\"flow_control\":\"none\","
	      "\"expected_generation\":9}", &response);
	assert(response.status == HTTP_202_ACCEPTED);
	assert(serial_config_calls == 1U);
	assert(serial_config_request.baud == 57600U);
	assert(serial_config_request.parity == LINKR_DEBUGGER_SERIAL_PARITY_EVEN);
	assert(serial_config_expected_generation == 9U);
	assert(strstr((const char *)response.body,
		      "\"accepted\":true,\"request_id\":77") != NULL);

	memset(&response, 0, sizeof(response));
	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	serial_status.generation = 10U;
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, config_json, &response);
	assert(response.status == HTTP_409_CONFLICT);
	assert(serial_config_calls == 1U);
	assert(strstr((const char *)response.body,
		      "\"code\":\"generation_conflict\"") != NULL);

	memset(&response, 0, sizeof(response));
	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	serial_status.generation = 9U;
	serial_config_result = -EBUSY;
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, config_json, &response);
	assert(response.status == HTTP_409_CONFLICT);
	assert(serial_config_calls == 2U);
	assert(strstr((const char *)response.body,
		      "\"code\":\"configuration_busy\"") != NULL);

	memset(&response, 0, sizeof(response));
	serial_config_result = 0;
	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
	      "{\"baud\":57600,\"data_bits\":8,\"parity\":\"none\","
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

static void test_serial_status_all_u64_max_fits_static_buffer(void)
{
	struct http_response_ctx response = { 0 };
	struct http_client_ctx client = { .method = HTTP_GET };

	reset_test();
	memset(&serial_status.runtime_counters, 0xff,
	       sizeof(serial_status.runtime_counters));
	memset(&serial_status.config_counters, 0xff,
	       sizeof(serial_status.config_counters));
	serial_status.generation = UINT64_MAX;
	serial_status.cdc_responder_generation = UINT64_MAX;
	serial_status.rx_head = UINT64_MAX;
	serial_status.tx_bytes_committed = UINT64_MAX;
	serial_status.tx_accepted = UINT64_MAX;
	serial_status.tx_completed = UINT64_MAX;
	serial_status.tx_canceled = UINT64_MAX;
	serial_status.last_config_request_id = UINT64_MAX;
	serial_status.active_event_id = UINT64_MAX;
	serial_status.last_config_result = INT_MAX;
	serial_status.dtr = false;
	memset(&serial_net_status.counters, 0xff,
	       sizeof(serial_net_status.counters));
	memset(serial_net_status.ws, 0xff, sizeof(serial_net_status.ws));
	memset(serial_net_status.tcp, 0xff, sizeof(serial_net_status.tcp));
	serial_net_status.terminal_responder =
		(struct linkr_debugger_serial_terminal_responder){
			.transport =
				LINKR_DEBUGGER_SERIAL_TERMINAL_RESPONDER_WEBSOCKET,
			.slot = 3,
			.valid = true,
		};

	strcpy(client.url_buffer, "/api/v1/serial/j4");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_200_OK);
	assert(response.body == serial_http_json);
	assert(response.body_len == strlen((const char *)response.body));
	assert(response.body_len < sizeof(serial_http_json));
	assert(strstr((const char *)response.body, "18446744073709551615") !=
	       NULL);
	assert(strstr((const char *)response.body,
	      "\"terminal_responder\":{\"transport\":\"websocket\","
	      "\"slot\":3}") != NULL);
	fprintf(stderr, "Serial HTTP all-U64 fixture: %zu/%zu bytes "
			"(%zu-byte margin)\n",
		response.body_len, sizeof(serial_http_json),
		sizeof(serial_http_json) - response.body_len);
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

	/* The shared accumulator releases storage on overflow, but retains the
	 * transaction tombstone so a legal FINAL cannot become a new request.
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

	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
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

static void test_serial_config_accepts_continuous_baud_range(void)
{
	struct http_response_ctx response = { 0 };
	struct http_client_ctx client = { .method = HTTP_PUT };

	reset_test();
	serial_status.dtr = false;
	strcpy(client.url_buffer, "/api/v1/serial/j4/config");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
	      "{\"baud\":5200,\"data_bits\":8,\"parity\":\"none\","
	      "\"stop_bits\":1,\"flow_control\":\"none\","
	      "\"expected_generation\":9}", &response);
	assert(response.status == HTTP_202_ACCEPTED);
	assert(serial_config_calls == 1U);
	assert(serial_config_request.baud == 5200U);
	assert(serial_config_expected_generation == 9U);

	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	memset(&response, 0, sizeof(response));
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
	      "{\"baud\":115201,\"data_bits\":8,\"parity\":\"none\","
	      "\"stop_bits\":1,\"flow_control\":\"none\","
	      "\"expected_generation\":9}", &response);
	assert(response.status == HTTP_400_BAD_REQUEST);
	assert(serial_config_calls == 1U);
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
	assert(strstr(body,
		      "\"name\":\"5v_usb_hub\",\"signal\":\"GPIO24_USB_CRL\","
		      "\"gp\":24,\"controllable\":true") != NULL);
	assert(count_substring(body, "\"adc_index\":") == HTTP_TEST_ADC_COUNT);
	assert(strstr(strstr(body, "\"adc_channels\":["), "\"name\":\"5v_usb_hub\"") == NULL);
	assert(count_substring(body, "\"note\":\"PD input divider; ADC pad voltage\"") == 1U);
	assert(count_substring(body, "\"role\":\"pd_input\"") == 1U);
	assert(strstr(body, "\"role\":\"native\"") == NULL);
	assert(count_substring(body, "\"layoutGroup\":") == HTTP_TEST_MAPPED_GPIO_COUNT);
	assert(count_substring(body, "\"direction\":") == HTTP_TEST_SAFE_GPIO_COUNT);
	assert(strstr(body,
		      "\"note\":\"J8_PIN1\",\"layoutGroup\":\"J8\","
		      "\"layoutLabel\":\"GP12\",\"layoutRow\":9,\"layoutColumn\":0") != NULL);
	assert(strstr(body,
		      "\"note\":\"CON_USER\",\"layoutGroup\":\"J13\","
		      "\"layoutLabel\":\"GP9 / USER\",\"layoutRow\":0,"
		      "\"layoutColumn\":0") != NULL);
	assert(strstr(body,
		      "\"name\":\"GP5\",\"pin\":5,\"note\":\"J8_PIN16\","
		      "\"layoutGroup\":\"J8\",\"layoutLabel\":\"GP5\","
		      "\"layoutRow\":4,\"layoutColumn\":1") != NULL);
	assert(strstr(body,
		      "\"name\":\"GP6\",\"pin\":6,\"note\":\"J8_PIN15\","
		      "\"layoutGroup\":\"J8\",\"layoutLabel\":\"GP6\","
		      "\"layoutRow\":5,\"layoutColumn\":1") != NULL);
	assert(strstr(body, "\"name\":\"GP17\"") == NULL);
	assert(strstr(body, "\"name\":\"GP18\"") == NULL);
	assert(strstr(body, "\"fixed_profiles\":[") != NULL);
	assert(strstr(body, "\"avs_ranges\":[") != NULL);
	assert(strstr(body, "\"switches\":{\"sd\":") != NULL);
	assert(strstr(body, "\"tf_wp\":{\"route\":") != NULL);
	assert(strstr(body, "\"usb\":{\"route\":") == NULL);
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
			"\"role\":\"pd_input\",\"note\":\"PD input divider; ADC pad voltage\"") != NULL);
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
	assert(strstr(body,
		      "\"reserved\":\"GP00-GP04 GP10-GP11 GP19 GP22-GP25 GP26-GP28\"") != NULL);
	match = strstr(body, expected_groups);
	assert(match != NULL);
	assert(strstr(match + 1, "\"gpio_layout_groups\"") == NULL);

	memset(&response, 0, sizeof(response));
	strcpy(client.url_buffer, "/api/v1/gpio/J8_PIN16");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_200_OK);
	body = (const char *)response.body;
	assert(strstr(body, "\"name\":\"GP5\"") != NULL);
	assert(strstr(body, "\"note\":\"J8_PIN16\"") != NULL);

	memset(&response, 0, sizeof(response));
	strcpy(client.url_buffer, "/api/v1/gpio/J8_PIN15");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_200_OK);
	body = (const char *)response.body;
	assert(strstr(body, "\"name\":\"GP6\"") != NULL);
	assert(strstr(body, "\"note\":\"J8_PIN15\"") != NULL);

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

static void test_removed_usb_switch_is_unadvertised_and_not_found(void)
{
	struct http_response_ctx response = {0};
	struct http_client_ctx client = { .method = HTTP_GET };
	struct http_request_ctx request = {0};
	const char *body;

	reset_test();
	strcpy(client.url_buffer, "/api/v1/switch");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_200_OK);
	body = (const char *)response.body;
	assert(strstr(body, "\"sd\":") != NULL);
	assert(strstr(body, "\"tf_wp\":") != NULL);
	assert(strstr(body, "\"usb\":") == NULL);

	memset(&response, 0, sizeof(response));
	strcpy(client.url_buffer, "/api/v1/switch/usb");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_404_NOT_FOUND);
	assert(strstr((const char *)response.body, "\"code\":\"not_found\"") != NULL);

	memset(&response, 0, sizeof(response));
	client.method = HTTP_PUT;
	request.data = "{\"route\":\"target\"}";
	request.data_len = strlen(request.data);
	assert(linkr_debugger_http_route_request(
		       &client, HTTP_SERVER_REQUEST_DATA_FINAL, &request,
		       &response, NULL) == 0);
	assert(response.status == HTTP_404_NOT_FOUND);
	assert(strstr((const char *)response.body, "\"code\":\"not_found\"") != NULL);
}

static void test_usb_hub_power_endpoint_get_and_put(void)
{
	struct http_response_ctx response = {0};
	struct http_client_ctx client = { .method = HTTP_GET };
	struct http_request_ctx request = {
		.data = "{\"state\":\"on\"}",
		.data_len = strlen("{\"state\":\"on\"}"),
	};

	reset_test();
	strcpy(client.url_buffer, "/api/v1/power/5v_usb_hub");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.status == HTTP_200_OK);
	assert(strstr((const char *)response.body,
		      "\"name\":\"5v_usb_hub\"") != NULL);
	assert(strstr((const char *)response.body,
		      "\"signal\":\"GPIO24_USB_CRL\"") != NULL);

	memset(&response, 0, sizeof(response));
	client.method = HTTP_PUT;
	assert(linkr_debugger_http_route_request(
		       &client, HTTP_SERVER_REQUEST_DATA_FINAL, &request,
		       &response, NULL) == 0);
	assert(response.status == HTTP_200_OK);
	assert(strstr((const char *)response.body, "\"state\":\"on\"") != NULL);
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
	assert(strstr(body,
		      "{\"pinNumber\":16,\"row\":4,\"column\":1,"
		      "\"label\":\"GP5\",\"signal\":\"GPIO5_CON\","
		      "\"role\":\"gpio\",\"controllable\":true,"
		      "\"outputCapable\":true,\"gpioName\":\"GP5\"}") != NULL);
	assert(strstr(body,
		      "{\"pinNumber\":15,\"row\":5,\"column\":1,"
		      "\"label\":\"GP6\",\"signal\":\"GPIO6_CON\","
		      "\"role\":\"gpio\",\"controllable\":true,"
		      "\"outputCapable\":true,\"gpioName\":\"GP6\"}") != NULL);
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
	assert(count_substring(body, "\"role\":\"pd_input\"") == 1U);
	assert(count_substring(body, "\"role\":\"native\"") == 7U);
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
		"\"note\":\"PD input divider; ADC pad voltage\",\"role\":\"pd_input\","
		"\"raw\":3237,\"mv\":2608,\"sensor_channel\":\"voltage\",\"unit\":\"V\","
		"\"sensor_value\":{\"val1\":20,\"val2\":2}}") != NULL);
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

static const struct {
	const struct linkr_debugger_http_resource_stub *resource;
	const char *path;
	enum http_method method;
	const char *content_type;
	enum linkr_debugger_task_http_route route;
} task_test_resources[] = {
	{ &linkr_debugger_tasks_resource, "/api/v1/tasks", HTTP_GET,
	  "application/json", LINKR_DEBUGGER_TASK_HTTP_ROUTE_TASKS },
	{ &linkr_debugger_task_catalog_resource, "/api/v1/tasks/catalog", HTTP_GET,
	  "application/json", LINKR_DEBUGGER_TASK_HTTP_ROUTE_CATALOG },
	{ &linkr_debugger_task_ir_resource, "/api/v1/tasks/ir", HTTP_GET,
	  "application/vnd.radxa.linkr-task-ir.v1", LINKR_DEBUGGER_TASK_HTTP_ROUTE_IR },
	{ &linkr_debugger_task_mutations_resource, "/api/v1/tasks/mutations", HTTP_POST,
	  "application/json", LINKR_DEBUGGER_TASK_HTTP_ROUTE_MUTATIONS },
	{ &linkr_debugger_task_source_0_resource, "/api/v1/tasks/slots/0/source", HTTP_GET,
	  "application/vnd.radxa.linkr-test.v1+ndjson", LINKR_DEBUGGER_TASK_HTTP_ROUTE_SOURCE_0 },
	{ &linkr_debugger_task_source_1_resource, "/api/v1/tasks/slots/1/source", HTTP_GET,
	  "application/vnd.radxa.linkr-test.v1+ndjson", LINKR_DEBUGGER_TASK_HTTP_ROUTE_SOURCE_1 },
	{ &linkr_debugger_task_source_2_resource, "/api/v1/tasks/slots/2/source", HTTP_GET,
	  "application/vnd.radxa.linkr-test.v1+ndjson", LINKR_DEBUGGER_TASK_HTTP_ROUTE_SOURCE_2 },
	{ &linkr_debugger_task_source_3_resource, "/api/v1/tasks/slots/3/source", HTTP_GET,
	  "application/vnd.radxa.linkr-test.v1+ndjson", LINKR_DEBUGGER_TASK_HTTP_ROUTE_SOURCE_3 },
};

static void task_route(struct http_client_ctx *client, enum http_transaction_status status,
		       const void *data, size_t len, const char *content_type,
		       struct http_response_ctx *response)
{
	struct http_header header = { .name = "Content-Type", .value = content_type };
	struct http_request_ctx request = {
		.data = (char *)data,
		.data_len = len,
		.headers = content_type != NULL ? &header : NULL,
		.header_count = content_type != NULL ? 1U : 0U,
		.headers_status = HTTP_HEADER_STATUS_OK,
	};

	assert(linkr_debugger_http_route_request(client, status, &request, response, NULL) == 0);
}

static void test_task_resources_methods_types_and_exact_fallback_routes(void)
{
	static const enum http_method methods[] = {
		HTTP_GET, HTTP_HEAD, HTTP_POST, HTTP_PUT, HTTP_DELETE,
	};
	struct http_header header = {
		.name = "Content-Type", .value = LINKR_DEBUGGER_TASK_MUTATION_CONTENT_TYPE,
	};
	struct http_request_ctx request = {
		.data = (char *)task_test_mutation, .data_len = task_test_mutation_len,
		.headers = &header, .header_count = 1U, .headers_status = HTTP_HEADER_STATUS_OK,
	};

	assert(ARRAY_SIZE(task_test_resources) == 8U);
	assert(ARRAY_SIZE(linkr_debugger_task_http_resources) == ARRAY_SIZE(task_test_resources));
	for (size_t i = 0U; i < ARRAY_SIZE(task_test_resources); i++) {
		struct http_resource_detail_dynamic *detail = task_test_resources[i].resource->detail;

		assert(strcmp(task_test_resources[i].resource->path, task_test_resources[i].path) == 0);
		assert(detail->common.type == HTTP_RESOURCE_TYPE_DYNAMIC);
		assert(detail->common.bitmask_of_supported_http_methods == BIT(task_test_resources[i].method));
		assert(strcmp(detail->common.content_type, task_test_resources[i].content_type) == 0);
		assert(detail->cb == linkr_debugger_task_http_handle);
		assert(*(const enum linkr_debugger_task_http_route *)detail->user_data ==
		       task_test_resources[i].route);
		for (size_t fallback = 0U; fallback < 2U; fallback++) {
			for (size_t m = 0U; m < ARRAY_SIZE(methods); m++) {
				struct http_client_ctx client = { .method = methods[m] };
				struct http_response_ctx response = { 0 };
				bool allowed = methods[m] == task_test_resources[i].method;

				reset_test();
				(void)snprintf(client.url_buffer, sizeof(client.url_buffer), "%s%s",
					       task_test_resources[i].path, fallback ? "?test=1" : "");
				if (fallback) {
					assert(linkr_debugger_http_route_request(&client,
						HTTP_SERVER_REQUEST_DATA_FINAL, &request, &response, NULL) == 0);
				} else {
					assert(detail->cb(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
						&request, &response, detail->user_data) == 0);
				}
				assert(response.status == (allowed ? HTTP_200_OK : HTTP_405_METHOD_NOT_ALLOWED));
				assert(response.final_chunk);
				assert(task_test_mutation_calls ==
				       (allowed && methods[m] == HTTP_POST ? 1U : 0U));
				if (allowed && task_test_resources[i].route >= LINKR_DEBUGGER_TASK_HTTP_ROUTE_SOURCE_0) {
					assert(task_test_source_calls == 1U);
					assert(task_test_source_slot == task_test_resources[i].route -
					       LINKR_DEBUGGER_TASK_HTTP_ROUTE_SOURCE_0);
				}
				if (fallback) {
					route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
				} else {
					assert(detail->cb(&client, HTTP_SERVER_TRANSACTION_COMPLETE,
						NULL, &response, detail->user_data) == 0);
				}
				assert(task_response_owner == NULL && task_input_state.client == NULL);
			}
		}
	}
}

static void test_task_unknown_children_never_use_legacy_delete_fallback(void)
{
	static const char *const paths[] = {
		"/api/v1/tasks/A", "/api/v1/tasks/..", "/api/v1/tasks/%2e%2e",
		"/api/v1/tasks/ir/child", "/api/v1/tasks/mutations/child",
		"/api/v1/tasks/catalog/child", "/api/v1/tasks/slots/4/source",
		"/api/v1/tasks/slots/00/source", "/api/v1/tasks/slots/0/source/child",
		"/api/v1/tasks/slots/0", "/api/v1/tasks-old",
	};
	static const enum http_method methods[] = { HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE };

	reset_test();
	for (size_t i = 0U; i < ARRAY_SIZE(paths); i++) {
		for (size_t m = 0U; m < ARRAY_SIZE(methods); m++) {
			struct http_client_ctx client = { .method = methods[m] };
			struct http_response_ctx response = { 0 };

			strcpy(client.url_buffer, paths[i]);
			route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "not a mutation", &response);
			assert(response.status == HTTP_404_NOT_FOUND);
		}
	}
	assert(task_test_mutation_calls == 0U && task_test_source_calls == 0U);
	assert(task_response_owner == NULL && task_input_state.client == NULL);
}

static void test_task_raw_arenas_live_until_complete_or_abort(void)
{
	static const enum http_transaction_status endings[] = {
		HTTP_SERVER_TRANSACTION_COMPLETE, HTTP_SERVER_TRANSACTION_ABORTED,
	};

	for (size_t i = 0U; i < ARRAY_SIZE(task_test_resources); i++) {
		if (task_test_resources[i].route == LINKR_DEBUGGER_TASK_HTTP_ROUTE_CATALOG ||
		    task_test_resources[i].route == LINKR_DEBUGGER_TASK_HTTP_ROUTE_MUTATIONS) {
			continue;
		}
		for (size_t e = 0U; e < ARRAY_SIZE(endings); e++) {
			struct http_client_ctx first = { .method = HTTP_GET };
			struct http_client_ctx second = { .method = HTTP_GET };
			struct http_response_ctx response = { 0 };
			struct http_response_ctx blocked = { 0 };
			uint8_t saved[LINKR_DEBUGGER_TASK_RAW_BUFFER_SIZE];

			reset_test();
			strcpy(first.url_buffer, task_test_resources[i].path);
			strcpy(second.url_buffer, task_test_resources[i].path);
			route(&first, HTTP_SERVER_REQUEST_DATA_MORE, "ignored", &response);
			assert(task_response_owner == NULL && task_input_state.client == NULL);
			route(&first, HTTP_SERVER_REQUEST_DATA_FINAL, "ignored", &response);
			assert(response.status == HTTP_200_OK && response.body_len < sizeof(saved));
			memcpy(saved, response.body, response.body_len);
			route(&second, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &blocked);
			assert(blocked.status == HTTP_409_CONFLICT);
			route(&second, endings[e], NULL, &blocked);
			assert(memcmp(saved, response.body, response.body_len) == 0);
			assert(task_response_owner == &first || task_input_state.client == &first);
			route(&first, endings[e], NULL, &response);
			assert(task_response_owner == NULL && task_input_state.client == NULL);
			memset(&blocked, 0, sizeof(blocked));
			route(&second, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &blocked);
			assert(blocked.status == HTTP_200_OK);
			route(&second, endings[e], NULL, &blocked);
			assert(task_test_mutation_calls == 0U);
		}
	}
}

static void test_task_mutation_json_body_headers_and_lifecycle(void)
{
	static const enum http_transaction_status endings[] = {
		HTTP_SERVER_TRANSACTION_COMPLETE, HTTP_SERVER_TRANSACTION_ABORTED,
	};
	struct http_client_ctx client = { .method = HTTP_POST };
	struct http_client_ctx other = { .method = HTTP_POST };
	struct http_response_ctx response = { 0 };
	struct http_response_ctx blocked = { 0 };

	strcpy(client.url_buffer, "/api/v1/tasks/mutations");
	strcpy(other.url_buffer, client.url_buffer);
	for (size_t e = 0U; e < ARRAY_SIZE(endings); e++) {
		reset_test();
		task_route(&client, HTTP_SERVER_REQUEST_DATA_MORE, task_test_mutation, 3U,
			   LINKR_DEBUGGER_TASK_MUTATION_CONTENT_TYPE, &response);
		assert(task_test_mutation_calls == 0U && task_input_state.client == &client);
		task_route(&other, HTTP_SERVER_REQUEST_DATA_FINAL, task_test_mutation,
			   task_test_mutation_len, LINKR_DEBUGGER_TASK_MUTATION_CONTENT_TYPE, &blocked);
		assert(blocked.status == HTTP_409_CONFLICT && task_test_mutation_calls == 0U);
		route(&other, endings[e], NULL, &blocked);
		assert(task_input_state.client == &client);
		route(&client, endings[e], NULL, &response);
		assert(task_input_state.client == NULL && task_response_owner == NULL);

		memset(&response, 0, sizeof(response));
		task_route(&client, HTTP_SERVER_REQUEST_DATA_MORE, task_test_mutation, 3U,
			   LINKR_DEBUGGER_TASK_MUTATION_CONTENT_TYPE, &response);
		task_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, task_test_mutation + 3U,
			   task_test_mutation_len - 3U, NULL, &response);
		assert(response.status == HTTP_200_OK && task_test_mutation_calls == 1U);
		assert(task_response_owner == &client);
		route(&client, endings[e], NULL, &response);
		assert(task_response_owner == NULL && task_input_state.client == NULL);
	}

	reset_test();
	task_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, task_test_mutation,
		   task_test_mutation_len, "text/plain", &response);
	assert(response.status == HTTP_400_BAD_REQUEST && task_test_mutation_calls == 0U);
	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	task_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, task_test_mutation,
		   task_test_mutation_len, NULL, &response);
	assert(response.status == HTTP_400_BAD_REQUEST && task_test_mutation_calls == 0U);
	route(&client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, &response);
}

static void test_task_firmware_json_golden_vectors(void)
{
	struct http_client_ctx client = { .method = HTTP_GET };
	struct http_response_ctx response = { 0 };

	reset_test();
	strcpy(client.url_buffer, "/api/v1/tasks");
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, &response);
	assert(response.body_len == sizeof(task_test_golden_list) - 1U);
	assert(memcmp(response.body, task_test_golden_list, response.body_len) == 0);
	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	strcpy(client.url_buffer, "/api/v1/tasks/mutations");
	client.method = HTTP_POST;
	task_test_set_mutation_body("upsert");
	task_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, (uint8_t *)task_test_mutation,
		   task_test_mutation_len,
		   LINKR_DEBUGGER_TASK_MUTATION_CONTENT_TYPE, &response);
	assert(strcmp((const char *)response.body, task_test_golden_upsert) == 0);
	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	task_test_set_mutation_body("delete");
	task_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, (uint8_t *)task_test_mutation,
		   task_test_mutation_len,
		   LINKR_DEBUGGER_TASK_MUTATION_CONTENT_TYPE, &response);
	assert(strcmp((const char *)response.body, task_test_golden_delete) == 0);
	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	task_test_mutation_result = LINKR_DEBUGGER_TASK_REVISION_CONFLICT;
	task_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, (uint8_t *)task_test_mutation,
		   task_test_mutation_len,
		   LINKR_DEBUGGER_TASK_MUTATION_CONTENT_TYPE, &response);
	assert(response.status == HTTP_409_CONFLICT);
	assert(strcmp((const char *)response.body, task_test_golden_conflict) == 0);
	route(&client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, &response);
}

static unsigned int mutation_calls(void)
{
	return http_test_control_calls + setter_calls + pd_fixed_calls + pd_pps_calls +
		serial_config_calls;
}

static const struct {
	const char *path;
	const char *old_body;
	const char *new_body;
} control_lifecycle_cases[] = {
	{"/api/v1/power/5v_out", "{\"state\":\"on\"}", "{\"state\":\"off\"}"},
	{"/api/v1/power/vdd_5v", "{\"state\":\"on\"}", "{\"state\":\"off\"}"},
	{"/api/v1/switch/sd", "{\"route\":\"target\"}", "{\"route\":\"usb-reader\"}"},
	{"/api/v1/switch/tf_wp", "{\"route\":\"writable\"}", "{\"route\":\"protected\"}"},
	{"/api/v1/gpio/GP5", "{\"direction\":\"output\",\"value\":1}",
	 "{\"direction\":\"output\",\"value\":0}"},
	{"/api/v1/gpio/%47P6", "{\"direction\":\"output\",\"value\":1}",
	 "{\"direction\":\"output\",\"value\":0}"},
};

static void test_registered_fallback_detail_owns_children_until_disconnect(void)
{
	static const char *const parents[] = {
		"/api/v1/power",
		"/api/v1/switch",
		"/api/v1/gpio",
	};

	reset_test();
	assert(linkr_debugger_http_service.res_fallback == &fallback_resource_detail.common);
	for (size_t i = 0U; i < ARRAY_SIZE(parents); i++) {
		size_t registered = 0U;

		for (const struct linkr_debugger_http_resource_stub *resource =
			     __start_http_test_resources;
		     resource < __stop_http_test_resources; resource++) {
			if (strcmp(resource->path, parents[i]) == 0) {
				assert(resource->detail == &fallback_resource_detail);
				registered++;
			}
		}
		assert(registered == 1U);
		assert(transport_detail(parents[i]) == &fallback_resource_detail);
	}
	for (size_t i = 0U; i < ARRAY_SIZE(control_lifecycle_cases); i++) {
		struct http_client_ctx client = {.method = HTTP_PUT, .fd = 42};
		struct http_response_ctx response = {0};

		snprintf(client.url_buffer, sizeof(client.url_buffer), "%s?lifecycle=1",
			 control_lifecycle_cases[i].path);
		assert(transport_detail(client.url_buffer) == &fallback_resource_detail);
		transport_route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{", &response);
		assert(fallback_resource_detail.holder == &client);
		/* Even a long but still live upload must not lose its cleanup owner. */
		http_test_uptime_ms += 60000;
		assert(fallback_resource_detail.holder == &client);
		assert(transport_disconnect(&client) == 1U);
		assert(fallback_resource_detail.holder == NULL);
		assert(mutation_calls() == 0U);
	}
}

static void test_disconnect_recycled_context_never_inherits_body_or_tombstone(void)
{
	char oversized[LINKR_DEBUGGER_HTTP_BODY_CAP + 2U];

	memset(oversized, 'x', sizeof(oversized) - 1U);
	oversized[sizeof(oversized) - 1U] = '\0';
	for (size_t old = 0U; old < ARRAY_SIZE(control_lifecycle_cases); old++) {
		for (size_t next = 0U; next < ARRAY_SIZE(control_lifecycle_cases); next++) {
			for (size_t overflow = 0U; overflow < 2U; overflow++) {
				struct http_client_ctx client = {.method = HTTP_PUT, .fd = 42};
				struct http_response_ctx response = {0};

				reset_test();
				strcpy(client.url_buffer, control_lifecycle_cases[old].path);
				transport_route(&client, HTTP_SERVER_REQUEST_DATA_MORE,
						overflow ? oversized
							 : control_lifecycle_cases[old].old_body,
						&response);
				assert(mutation_calls() == 0U);
				assert(response.final_chunk == (overflow != 0U));
				assert(transport_disconnect(&client) == 1U);

				/* Same context address AND fd: neither is a generation token. */
				client = (struct http_client_ctx){.method = HTTP_PUT, .fd = 42};
				strcpy(client.url_buffer, control_lifecycle_cases[next].path);
				response = (struct http_response_ctx){0};
				transport_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
						control_lifecycle_cases[next].new_body, &response);
				assert(mutation_calls() == 1U && response.status == HTTP_200_OK);
				if (next < 2U) {
					assert(!linkr_debugger_power_output_enabled(
						linkr_debugger_find_rail(
							strrchr(client.url_buffer, '/') + 1)));
				}
				assert(transport_disconnect(&client) == 1U);
			}
		}
	}
}

static void test_control_trailing_final_and_keepalive_transaction_boundary(void)
{
	char oversized[LINKR_DEBUGGER_HTTP_BODY_CAP + 2U];

	memset(oversized, 'x', sizeof(oversized) - 1U);
	oversized[sizeof(oversized) - 1U] = '\0';
	for (size_t i = 0U; i < ARRAY_SIZE(control_lifecycle_cases); i++) {
		struct http_client_ctx client = {.method = HTTP_PUT, .fd = 42};
		struct http_response_ctx response = {0};

		reset_test();
		strcpy(client.url_buffer, control_lifecycle_cases[i].path);
		transport_route(&client, HTTP_SERVER_REQUEST_DATA_MORE, oversized, &response);
		assert(response.status == HTTP_413_PAYLOAD_TOO_LARGE);
		response = (struct http_response_ctx){0};
		transport_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
				control_lifecycle_cases[i].new_body, &response);
		assert(mutation_calls() == 0U && !response.final_chunk);
		transport_route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
		assert(fallback_resource_detail.holder == NULL);

		/* On HTTP/1 keepalive the URL can change only AFTER COMPLETE. */
		size_t next = (i + 1U) % ARRAY_SIZE(control_lifecycle_cases);
		const char *body = control_lifecycle_cases[next].new_body;
		char prefix[6];

		strcpy(client.url_buffer, control_lifecycle_cases[next].path);
		memcpy(prefix, body, 5U);
		prefix[5] = '\0';
		transport_route(&client, HTTP_SERVER_REQUEST_DATA_MORE, prefix, &response);
		assert(mutation_calls() == 0U);
		transport_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, body + 5U, &response);
		assert(mutation_calls() == 1U && response.status == HTTP_200_OK);
		response = (struct http_response_ctx){0};
		transport_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, body, &response);
		assert(mutation_calls() == 1U && !response.final_chunk);
		transport_route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	}
}

static void test_control_json_requires_exactly_one_object(void)
{
	static const char *const suffixes[] = {"{}", " \r\n{}", "garbage", "\v", "\f"};
	char body[256];
	struct linkr_debugger_http_power_set_request parsed = {0};
	char concatenated[] = "{\"state\":\"on\"}{\"state\":\"off\"}";
	char embedded_nul[] = "{\"state\":\"on\"}\0{\"state\":\"off\"}";

	/* Establish the regression oracle: the stub, like Zephyr, accepts a prefix. */
	assert(json_obj_parse(concatenated, strlen(concatenated), power_set_request_descr,
			      ARRAY_SIZE(power_set_request_descr), &parsed) == BIT64(0));
	assert(strcmp(parsed.state, "on") == 0);
	assert(linkr_debugger_http_control_power("5v_out", embedded_nul,
						 sizeof(embedded_nul) - 1U) == -EINVAL);
	for (size_t i = 0U; i < ARRAY_SIZE(control_lifecycle_cases); i++) {
		struct http_client_ctx client = {.method = HTTP_PUT, .fd = 42};
		struct http_response_ctx response = {0};

		for (size_t suffix = 0U; suffix < ARRAY_SIZE(suffixes); suffix++) {
			reset_test();
			strcpy(client.url_buffer, control_lifecycle_cases[i].path);
			/* A complete-looking MORE object must not execute if FINAL adds junk. */
			transport_route(&client, HTTP_SERVER_REQUEST_DATA_MORE,
					control_lifecycle_cases[i].old_body, &response);
			transport_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, suffixes[suffix],
					&response);
			assert(mutation_calls() == 0U && response.status == HTTP_400_BAD_REQUEST);
			assert(transport_disconnect(&client) == 1U);
		}
		reset_test();
		strcpy(client.url_buffer, control_lifecycle_cases[i].path);
		snprintf(body, sizeof(body), " \t\r\n%s \t\r\n",
			 control_lifecycle_cases[i].new_body);
		transport_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, body, &response);
		assert(mutation_calls() == 1U && response.status == HTTP_200_OK);
		assert(transport_disconnect(&client) == 1U);
	}
}

static void test_net_pd_use_the_same_complete_object_guard(void)
{
	static const struct {
		const char *path;
		const char *body;
	} cases[] = {
		{"/api/v1/net", "{\"network\":\"10.20.30.0\"}"},
		{"/api/v1/pd", "{\"mode\":0,\"voltage_mv\":9000}"},
	};

	for (size_t i = 0U; i < ARRAY_SIZE(cases); i++) {
		struct http_client_ctx client = {.method = HTTP_PUT, .fd = 42};
		struct http_response_ctx response = {0};

		reset_test();
		strcpy(client.url_buffer, cases[i].path);
		transport_route(&client, HTTP_SERVER_REQUEST_DATA_MORE, cases[i].body, &response);
		transport_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "{}", &response);
		assert(mutation_calls() == 0U && response.status == HTTP_400_BAD_REQUEST);
		assert(transport_disconnect(&client) == 1U);
	}
}

static void assert_power_json_container_result(const char *body, bool valid)
{
	for (size_t fragmented = 0U; fragmented < 2U; fragmented++) {
		struct http_client_ctx client = {.method = HTTP_PUT, .fd = 42};
		struct http_response_ctx response = {0};
		char prefix[LINKR_DEBUGGER_HTTP_BODY_CAP + 1U];
		size_t split = fragmented ? strlen(body) / 2U : 0U;

		reset_test();
		strcpy(client.url_buffer, "/api/v1/power/5v_out");
		assert(split < sizeof(prefix));
		if (fragmented) {
			memcpy(prefix, body, split);
			prefix[split] = '\0';
			transport_route(&client, HTTP_SERVER_REQUEST_DATA_MORE, prefix, &response);
			assert(mutation_calls() == 0U && !response.final_chunk);
		}
		transport_route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, body + split, &response);
		assert(response.status == (valid ? HTTP_200_OK : HTTP_400_BAD_REQUEST));
		assert(mutation_calls() == (valid ? 1U : 0U));
		assert(linkr_debugger_power_output_enabled(linkr_debugger_find_rail("5v_out")) ==
		       valid);
		assert(transport_disconnect(&client) == 1U);
	}
}

static void test_json_container_types_depth_and_quoted_delimiters(void)
{
	static const char *const invalid[] = {
		"{\"state\":\"on\",\"ignored\":{]}{\"state\":\"off\"}}",
		"{\"state\":\"on\",\"ignored\":[}",
		"{\"state\":\"on\",\"ignored\":{]}",
		"{\"state\":\"on\"}]",
		"{\"state\":\"on\"}}",
		"{\"state\":\"on\",\"ignored\":{}",
		"{\"state\":\"on\",\"ignored\":[[]]",
		"{\"state\":\"on\",\"ignored\":\"unterminated\\\"}",
	};
	static const char *const valid[] = {
		"{\"state\":\"on\",\"ignored\":{}}",
		"{\"state\":\"on\",\"ignored\":[]}",
		"{\"state\":\"on\",\"ignored\":{\"nested\":[{},[]]}} \t\r\n",
		"{\"state\":\"on\",\"ignored\":{\"escaped\":\"}\\\"{\",\"array\":[{},1]}}",
		"{\"state\":\"on\",\"ignored\":[\"\\\\\",{\"text\":\"[}]\"},[]]}",
	};
	char nested[128];
	char mismatched[] = "{\"state\":\"on\",\"ignored\":{]}{\"state\":\"off\"}}";
	struct linkr_debugger_http_power_set_request decoded = {0};

	/* Both the permissive stub and pinned json.c accept this malformed input.
	 * It is the production scanner, not the decoder stub, that must reject it.
	 */
	assert(json_obj_parse(mismatched, sizeof(mismatched) - 1U, power_set_request_descr,
			      ARRAY_SIZE(power_set_request_descr), &decoded) == BIT64(0));
	assert(strcmp(decoded.state, "on") == 0);
	for (size_t i = 0U; i < ARRAY_SIZE(invalid); i++) {
		assert_power_json_container_result(invalid[i], false);
	}
	for (size_t i = 0U; i < ARRAY_SIZE(valid); i++) {
		assert_power_json_container_result(valid[i], true);
	}
	/* Root counts as one container: accept the limit, reject one beyond it. */
	for (size_t depth = LINKR_DEBUGGER_HTTP_JSON_MAX_DEPTH;
	     depth <= LINKR_DEBUGGER_HTTP_JSON_MAX_DEPTH + 1U; depth++) {
		size_t len =
			(size_t)snprintf(nested, sizeof(nested), "{\"state\":\"on\",\"ignored\":");

		assert(len + 2U * depth + 1U < sizeof(nested));
		memset(nested + len, '[', depth - 1U);
		len += depth - 1U;
		nested[len++] = '0';
		memset(nested + len, ']', depth - 1U);
		len += depth - 1U;
		nested[len++] = '}';
		nested[len] = '\0';
		assert_power_json_container_result(nested,
						   depth == LINKR_DEBUGGER_HTTP_JSON_MAX_DEPTH);
	}
}

static void test_control_body_transaction_boundaries(void)
{
	static const struct {
		const char *path;
		const char *body;
		enum http_status success;
	} cases[] = {
		{ "/api/v1/power/5v_out", "{\"state\":\"on\"}", HTTP_200_OK },
		{ "/api/v1/switch/sd", "{\"route\":\"target\"}", HTTP_200_OK },
		{ "/api/v1/gpio/GP13", "{\"direction\":\"output\",\"value\":1}", HTTP_200_OK },
		{ "/api/v1/net", "{\"network\":\"10.20.30.0\"}", HTTP_200_OK },
		{ "/api/v1/pd", "{\"mode\":0,\"voltage_mv\":9000}", HTTP_200_OK },
		{ "/api/v1/serial/j4/config", "{\"baud\":57600,\"data_bits\":8,\"parity\":\"even\","
		  "\"stop_bits\":1,\"flow_control\":\"none\",\"expected_generation\":9}", HTTP_202_ACCEPTED },
	};
	char oversized[LINKR_DEBUGGER_HTTP_BODY_CAP + 2U];

	memset(oversized, 'x', sizeof(oversized) - 1U);
	oversized[sizeof(oversized) - 1U] = '\0';
	for (size_t i = 0U; i < ARRAY_SIZE(cases); i++) {
		struct http_client_ctx client = { .method = HTTP_PUT };
		struct http_response_ctx response = { 0 };
		char prefix[16];

		strcpy(client.url_buffer, cases[i].path);
		reset_test();
		memcpy(prefix, cases[i].body, 5U);
		prefix[5] = '\0';
		route(&client, HTTP_SERVER_REQUEST_DATA_MORE, prefix, &response);
		assert(mutation_calls() == 0U && !response.final_chunk);
		route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, cases[i].body + 5U, &response);
		assert(mutation_calls() == 1U && response.status == cases[i].success);
		response = (struct http_response_ctx){ 0 };
		route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, cases[i].body, &response);
		assert(mutation_calls() == 1U && !response.final_chunk);

		reset_test();
		route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "garbage", &response);
		route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, cases[i].body, &response);
		assert(mutation_calls() == 0U && response.status == HTTP_400_BAD_REQUEST);

		reset_test();
		route(&client, HTTP_SERVER_REQUEST_DATA_MORE, oversized, &response);
		assert(response.status == HTTP_413_PAYLOAD_TOO_LARGE);
		response = (struct http_response_ctx){ 0 };
		route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, cases[i].body, &response);
		assert(mutation_calls() == 0U && !response.final_chunk);
		route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
		route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, cases[i].body, &response);
		assert(mutation_calls() == 1U && response.status == cases[i].success);

		reset_test();
		route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "garbage", &response);
		route(&client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, &response);
		assert(mutation_calls() == 0U);
		route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, cases[i].body, &response);
		assert(mutation_calls() == 1U && response.status == cases[i].success);

		reset_test();
		/* Exhaust four body buffers; all four remaining clients must drain
		 * even after those buffers have been freed by other transactions. */
		struct http_client_ctx clients[8];
		for (size_t c = 0U; c < ARRAY_SIZE(clients); c++) {
			clients[c] = client;
			response = (struct http_response_ctx){ 0 };
			route(&clients[c], HTTP_SERVER_REQUEST_DATA_MORE, "{", &response);
			assert(response.final_chunk == (c >= LINKR_DEBUGGER_HTTP_BODY_SLOTS));
		}
		for (size_t c = 0U; c < LINKR_DEBUGGER_HTTP_BODY_SLOTS; c++) {
			route(&clients[c], HTTP_SERVER_TRANSACTION_ABORTED, NULL, &response);
		}
		for (size_t c = LINKR_DEBUGGER_HTTP_BODY_SLOTS; c < ARRAY_SIZE(clients); c++) {
			response = (struct http_response_ctx){ 0 };
			route(&clients[c], HTTP_SERVER_REQUEST_DATA_FINAL, cases[i].body, &response);
			assert(mutation_calls() == 0U && !response.final_chunk);
		}
	}
}

static void test_gpio_uses_real_error_mapping(void)
{
	static const struct {
		const char *direction;
		int error;
		enum http_status status;
		const char *code;
	} cases[] = {
		{ "output", -EPERM, HTTP_403_FORBIDDEN, "input_only" },
		{ "input", -EBUSY, HTTP_409_CONFLICT, "busy" },
		{ "output", -EBUSY, HTTP_409_CONFLICT, "busy" },
		{ "input", -EIO, HTTP_500_INTERNAL_SERVER_ERROR, "configure_failed" },
		{ "output", -EIO, HTTP_500_INTERNAL_SERVER_ERROR, "configure_failed" },
	};
	struct http_client_ctx client = { .method = HTTP_PUT };

	strcpy(client.url_buffer, "/api/v1/gpio/GP13");
	reset_test();
	for (size_t i = 0U; i < ARRAY_SIZE(cases); i++) {
		struct http_response_ctx response = { 0 };
		char body[64];
		char code[64];

		snprintf(body, sizeof(body), "{\"direction\":\"%s\",\"value\":1}", cases[i].direction);
		snprintf(code, sizeof(code), "\"code\":\"%s\"", cases[i].code);
		http_test_gpio_set_result = cases[i].error;
		route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, body, &response);
		assert(response.status == cases[i].status);
		assert(strstr((const char *)response.body, code) != NULL);
		route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	}
}

int main(void)
{
	test_registered_fallback_detail_owns_children_until_disconnect();
	test_disconnect_recycled_context_never_inherits_body_or_tombstone();
	test_control_trailing_final_and_keepalive_transaction_boundary();
	test_control_json_requires_exactly_one_object();
	test_net_pd_use_the_same_complete_object_guard();
	test_json_container_types_depth_and_quoted_delimiters();
	test_control_body_transaction_boundaries();
	test_gpio_uses_real_error_mapping();
	test_shared_json_workspace_static_ram_contract();
	test_handlers_share_workspace_until_synchronous_send_finishes();
	test_fragmented_put_executes_once_with_complete_json();
	test_unchanged_identity_preserves_derived_origin();
	test_invalid_network_forms_are_rejected();
	test_aborted_and_complete_clear_without_executing();
	test_invalid_prefix_cannot_be_replaced_by_valid_final_fragment();
	test_pd_fragmented_put_executes_once_with_complete_json();
	test_pd_aborted_request_clears_partial_body();
	test_pd_complete_request_clears_partial_body();
	test_pd_invalid_prefix_cannot_be_replaced_by_valid_final_fragment();
	test_serial_status_config_and_methods();
	test_serial_status_reports_unavailable_without_secrets();
	test_serial_status_all_u64_max_fits_static_buffer();
	test_serial_config_poison_survives_final_until_complete();
	test_serial_config_rejects_wide_values_before_narrowing();
	test_serial_config_accepts_continuous_baud_range();
	test_gpio_layout_group_encoder_is_generic_and_bounded();
	test_gpio_layout_catalog_encoder_is_exact_bounded_and_utf8_safe();
	test_gpio_layout_endpoint_is_static_complete_and_get_only();
	test_gpio_layout_groups_are_top_level_http_contract();
	test_removed_usb_switch_is_unadvertised_and_not_found();
	test_usb_hub_power_endpoint_get_and_put();
	test_adc_read_route_emits_eight_readings_and_adc0_note();
	test_status_budget_rejects_inflated_group_metadata();
	test_status_budget_rejects_inflated_adc_note_metadata();
	test_gpio_workspace_preserves_4096_encoding_limit();
	test_task_resources_methods_types_and_exact_fallback_routes();
	test_task_unknown_children_never_use_legacy_delete_fallback();
	test_task_raw_arenas_live_until_complete_or_abort();
	test_task_mutation_json_body_headers_and_lifecycle();
	test_task_firmware_json_golden_vectors();
	return 0;
}
