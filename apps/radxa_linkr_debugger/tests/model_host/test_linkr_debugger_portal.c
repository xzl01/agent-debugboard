/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "linkr_debugger_captive_portal.h"
#include "linkr_debugger_dns.h"

#define DNS_HEADER_LEN 12U
#define DNS_TYPE_A 1U
#define DNS_TYPE_AAAA 28U
#define DNS_CLASS_IN 1U
#define DNS_CLASS_CH 3U

static uint16_t read_u16(const uint8_t *buf)
{
	return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}

static void write_u16(uint8_t *buf, uint16_t value)
{
	buf[0] = (uint8_t)(value >> 8);
	buf[1] = (uint8_t)value;
}

static size_t append_name(uint8_t *buf, size_t pos, const char *name)
{
	const char *label = name;

	while (*label != '\0') {
		const char *dot = strchr(label, '.');
		size_t len = dot != NULL ? (size_t)(dot - label) : strlen(label);

		assert(len <= 63U);
		buf[pos++] = (uint8_t)len;
		memcpy(&buf[pos], label, len);
		pos += len;
		if (dot == NULL) {
			break;
		}
		label = dot + 1;
	}

	buf[pos++] = 0U;
	return pos;
}

static size_t build_query(uint8_t *buf, const char *name, uint16_t qtype,
				  uint16_t qclass, uint16_t flags, bool edns0)
{
	size_t pos;

	memset(buf, 0, 512U);
	write_u16(&buf[0], 0x1234U);
	write_u16(&buf[2], flags);
	write_u16(&buf[4], 1U);
	write_u16(&buf[10], edns0 ? 1U : 0U);
	pos = append_name(buf, DNS_HEADER_LEN, name);
	write_u16(&buf[pos], qtype);
	pos += 2U;
	write_u16(&buf[pos], qclass);
	pos += 2U;

	if (edns0) {
		buf[pos++] = 0U;
		write_u16(&buf[pos], 41U);
		pos += 2U;
		write_u16(&buf[pos], 4096U);
		pos += 2U;
		memset(&buf[pos], 0, 6U);
		pos += 6U;
	}

	return pos;
}

static void assert_a_response(const uint8_t *response, size_t response_len)
{
	assert(response_len >= DNS_HEADER_LEN + 16U);
	assert(read_u16(&response[0]) == 0x1234U);
	assert((read_u16(&response[2]) & 0x8400U) == 0x8400U);
	assert(read_u16(&response[4]) == 1U);
	assert(read_u16(&response[6]) == 1U);
	assert(response[response_len - 4U] == 172U);
	assert(response[response_len - 3U] == 29U);
	assert(response[response_len - 2U] == 203U);
	assert(response[response_len - 1U] == 1U);
}

static void test_dns_a_wildcard(void)
{
	uint8_t query[512];
	uint8_t response[512];
	size_t response_len;
	size_t query_len = build_query(query, "example.com", DNS_TYPE_A, DNS_CLASS_IN, 0U, false);

	assert(linkr_debugger_dns_build_response(query, query_len, response, sizeof(response),
						   &response_len) == 0);
	assert_a_response(response, response_len);
	assert(memcmp(&response[DNS_HEADER_LEN], &query[DNS_HEADER_LEN],
		      query_len - DNS_HEADER_LEN) == 0);
}

static void test_dns_aaaa_nodata(void)
{
	uint8_t query[512];
	uint8_t response[512];
	size_t response_len;
	size_t query_len = build_query(query, "ipv6.example", DNS_TYPE_AAAA,
				       DNS_CLASS_IN, 0U, false);

	assert(linkr_debugger_dns_build_response(query, query_len, response, sizeof(response),
						   &response_len) == 0);
	assert(read_u16(&response[6]) == 0U);
	assert(response_len == query_len);
}

static void test_dns_valid_unknown_names(void)
{
	uint8_t query[512];
	uint8_t response[512];
	size_t response_len;
	size_t query_len = build_query(query, "unknown.invalid", DNS_TYPE_A,
				       DNS_CLASS_IN, 0x0100U, false);

	assert(linkr_debugger_dns_build_response(query, query_len, response, sizeof(response),
						   &response_len) == 0);
	assert_a_response(response, response_len);
	assert((read_u16(&response[2]) & 0x0100U) != 0U);
}

static void test_dns_malformed_label(void)
{
	uint8_t query[512] = { 0 };
	uint8_t response[512];
	size_t response_len = 99U;

	write_u16(&query[4], 1U);
	query[DNS_HEADER_LEN] = 64U;
	assert(linkr_debugger_dns_build_response(query, DNS_HEADER_LEN + 1U, response,
						   sizeof(response), &response_len) == -EINVAL);
	assert(response_len == 0U);
}

static void test_dns_truncation_rejected(void)
{
	uint8_t query[512];
	uint8_t response[512];
	size_t response_len;
	size_t query_len = build_query(query, "truncated.example", DNS_TYPE_A,
				       DNS_CLASS_IN, 0x0200U, false);

	assert(linkr_debugger_dns_build_response(query, query_len, response, sizeof(response),
						   &response_len) == -EINVAL);
}

static void test_dns_compression_pointer_loop_rejected(void)
{
	uint8_t query[512] = { 0 };
	uint8_t response[512];
	size_t response_len;
	size_t pos = DNS_HEADER_LEN;

	write_u16(&query[4], 1U);
	query[pos++] = 0xc0U;
	query[pos++] = DNS_HEADER_LEN;
	write_u16(&query[pos], DNS_TYPE_A);
	pos += 2U;
	write_u16(&query[pos], DNS_CLASS_IN);
	pos += 2U;

	assert(linkr_debugger_dns_build_response(query, pos, response, sizeof(response),
						   &response_len) == -EINVAL);
}

static void test_dns_compressed_question_pointer_rejected(void)
{
	uint8_t query[512] = { 0 };
	uint8_t response[512];
	size_t response_len;
	size_t target_pos = 32U;
	size_t pos = DNS_HEADER_LEN;

	write_u16(&query[0], 0x1234U);
	write_u16(&query[4], 1U);
	query[pos++] = 0xc0U;
	query[pos++] = (uint8_t)target_pos;
	write_u16(&query[pos], DNS_TYPE_A);
	pos += 2U;
	write_u16(&query[pos], DNS_CLASS_IN);
	pos += 2U;
	pos = append_name(query, target_pos, "target.example");

	assert(linkr_debugger_dns_build_response(query, pos, response, sizeof(response),
						   &response_len) == -EINVAL);
}

static void test_dns_qdcount_rejected(void)
{
	uint8_t query[512];
	uint8_t response[512];
	size_t response_len;
	size_t query_len = build_query(query, "multi.example", DNS_TYPE_A,
				       DNS_CLASS_IN, 0U, false);

	write_u16(&query[4], 2U);
	assert(linkr_debugger_dns_build_response(query, query_len, response, sizeof(response),
						   &response_len) == -EINVAL);
}

static void test_dns_unsupported_opcode_and_class(void)
{
	uint8_t query[512];
	uint8_t response[512];
	size_t response_len;
	size_t query_len = build_query(query, "opcode.example", DNS_TYPE_A,
				       DNS_CLASS_IN, 0x0800U, false);

	assert(linkr_debugger_dns_build_response(query, query_len, response, sizeof(response),
						   &response_len) == -EINVAL);

	query_len = build_query(query, "class.example", DNS_TYPE_A, DNS_CLASS_CH, 0U, false);
	assert(linkr_debugger_dns_build_response(query, query_len, response, sizeof(response),
						   &response_len) == -EINVAL);
}

static void test_dns_small_response_buffer(void)
{
	uint8_t query[512];
	uint8_t response[16];
	size_t response_len;
	size_t query_len = build_query(query, "small.example", DNS_TYPE_A,
				       DNS_CLASS_IN, 0U, false);

	assert(linkr_debugger_dns_build_response(query, query_len, response, sizeof(response),
						   &response_len) == -ENOSPC);
}

static void test_dns_edns0_is_ignored_after_validation(void)
{
	uint8_t query[512];
	uint8_t response[512];
	size_t response_len;
	size_t query_len = build_query(query, "edns.example", DNS_TYPE_A,
				       DNS_CLASS_IN, 0U, true);

	assert(linkr_debugger_dns_build_response(query, query_len, response, sizeof(response),
						   &response_len) == 0);
	assert_a_response(response, response_len);
	assert(read_u16(&response[10]) == 0U);
}

static void test_captive_non_api_paths_redirect(void)
{
	const char *paths[] = {
		"/",
		"/generate_204",
		"/hotspot-detect.html",
		"/connecttest.txt",
		"/ncsi.txt",
		"/unknown?x=1",
		"/api/v1statusjunk",
	};

	for (size_t i = 0U; i < sizeof(paths) / sizeof(paths[0]); i++) {
		assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_GET,
							       paths[i]) ==
		       LINKR_DEBUGGER_CAPTIVE_ACTION_REDIRECT);
	}
}

static void test_captive_api_path(void)
{
	assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_GET,
						       "/captive-portal/api") ==
	       LINKR_DEBUGGER_CAPTIVE_ACTION_CAPPORT_JSON);
	assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_GET,
						       "/captive-portal/api?x=1") ==
	       LINKR_DEBUGGER_CAPTIVE_ACTION_CAPPORT_JSON);
	assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_HEAD,
						       "/captive-portal/api") ==
	       LINKR_DEBUGGER_CAPTIVE_ACTION_CAPPORT_JSON);
	assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_GET,
						       "/captive-portal/apix") ==
	       LINKR_DEBUGGER_CAPTIVE_ACTION_REDIRECT);
}

static void test_captive_unknown_api_paths_json_404(void)
{
	const char *paths[] = {
		"/api/v1",
		"/api/v1/",
		"/api/v1/unknown",
		"/api/v1/statusjunk",
	};

	for (size_t i = 0U; i < sizeof(paths) / sizeof(paths[0]); i++) {
		assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_GET,
							       paths[i]) ==
		       LINKR_DEBUGGER_CAPTIVE_ACTION_API_NOT_FOUND);
	}
}

static void test_captive_methods(void)
{
	assert(linkr_debugger_captive_method_has_body(LINKR_DEBUGGER_CAPTIVE_METHOD_GET));
	assert(!linkr_debugger_captive_method_has_body(LINKR_DEBUGGER_CAPTIVE_METHOD_HEAD));
	assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_HEAD,
						       "/does-not-exist") ==
	       LINKR_DEBUGGER_CAPTIVE_ACTION_REDIRECT);
	assert(linkr_debugger_captive_select_action(LINKR_DEBUGGER_CAPTIVE_METHOD_OTHER,
						       "/captive-portal/api") ==
	       LINKR_DEBUGGER_CAPTIVE_ACTION_METHOD_NOT_ALLOWED);
}

static void test_captive_canonical_url(void)
{
	assert(strcmp(linkr_debugger_captive_portal_url(), "http://172.29.203.1/") == 0);
	assert(strstr(linkr_debugger_captive_capport_body(), ":8080") == NULL);
	assert(strstr(linkr_debugger_captive_capport_body(),
		      "\"user-portal-url\":\"http://172.29.203.1/\"") != NULL);
}

int main(void)
{
	test_dns_a_wildcard();
	test_dns_aaaa_nodata();
	test_dns_valid_unknown_names();
	test_dns_malformed_label();
	test_dns_truncation_rejected();
	test_dns_compression_pointer_loop_rejected();
	test_dns_compressed_question_pointer_rejected();
	test_dns_qdcount_rejected();
	test_dns_unsupported_opcode_and_class();
	test_dns_small_response_buffer();
	test_dns_edns0_is_ignored_after_validation();
	test_captive_non_api_paths_redirect();
	test_captive_api_path();
	test_captive_unknown_api_paths_json_404();
	test_captive_methods();
	test_captive_canonical_url();
	printf("linkr_debugger_portal_test: OK\n");
	return 0;
}
