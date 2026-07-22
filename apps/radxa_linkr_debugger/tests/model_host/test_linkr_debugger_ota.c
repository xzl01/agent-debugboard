/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "linkr_debugger_ota.h"

static void test_size_header_parser(void)
{
	size_t size = 0U;

	assert(linkr_debugger_ota_parse_size_header("480100", &size) == 0);
	assert(size == 480100U);
	assert(linkr_debugger_ota_parse_size_header("", &size) == -EINVAL);
	assert(linkr_debugger_ota_parse_size_header("0", &size) == -EINVAL);
	assert(linkr_debugger_ota_parse_size_header("12x", &size) == -EINVAL);
	assert(linkr_debugger_ota_parse_size_header("184467440737095516160", &size) == -EOVERFLOW);
	assert(linkr_debugger_ota_parse_size_header(NULL, &size) == -EINVAL);
}

static void test_sha256_header_parser(void)
{
	uint8_t sha[LINKR_DEBUGGER_OTA_SHA256_LEN];
	const char *hex = "000102030405060708090a0b0c0d0e0f"
			  "101112131415161718191a1b1c1d1e1f";

	memset(sha, 0xff, sizeof(sha));
	assert(linkr_debugger_ota_parse_sha256_header(hex, sha) == 0);
	for (size_t i = 0U; i < sizeof(sha); i++) {
		assert(sha[i] == i);
	}
	assert(linkr_debugger_ota_parse_sha256_header("00", sha) == -EINVAL);
	assert(linkr_debugger_ota_parse_sha256_header(
		"000102030405060708090a0b0c0d0e0f"
		"101112131415161718191a1b1c1d1e1x", sha) == -EINVAL);
	assert(linkr_debugger_ota_parse_sha256_header(NULL, sha) == -EINVAL);
}

static void test_content_type_validation(void)
{
	assert(linkr_debugger_ota_content_type_is_octet_stream(LINKR_DEBUGGER_OTA_CONTENT_TYPE));
	assert(linkr_debugger_ota_content_type_is_octet_stream("APPLICATION/OCTET-STREAM"));
	assert(linkr_debugger_ota_content_type_is_octet_stream(" application/octet-stream \t"));
	assert(!linkr_debugger_ota_content_type_is_octet_stream(NULL));
	assert(!linkr_debugger_ota_content_type_is_octet_stream("text/plain"));
	assert(!linkr_debugger_ota_content_type_is_octet_stream(
		"application/octet-stream; charset=utf-8"));
	assert(!linkr_debugger_ota_content_type_is_octet_stream("application/octet-streamx"));
}

static void test_ota_route_matching(void)
{
	assert(linkr_debugger_ota_route_from_path("/api/v1/ota") ==
	       LINKR_DEBUGGER_OTA_ROUTE_STATUS);
	assert(linkr_debugger_ota_route_from_path("/api/v1/ota/upload") ==
	       LINKR_DEBUGGER_OTA_ROUTE_UPLOAD);
	assert(linkr_debugger_ota_route_from_path("/api/v1/ota/test") ==
	       LINKR_DEBUGGER_OTA_ROUTE_TEST);
	assert(linkr_debugger_ota_route_from_path("/api/v1/ota/confirm") ==
	       LINKR_DEBUGGER_OTA_ROUTE_CONFIRM);
	assert(linkr_debugger_ota_path_is_handled("/api/v1/ota/upload"));

	assert(linkr_debugger_ota_route_from_path(NULL) == LINKR_DEBUGGER_OTA_ROUTE_NONE);
	assert(linkr_debugger_ota_route_from_path("/api/v1/ota/uploadXYZ") ==
	       LINKR_DEBUGGER_OTA_ROUTE_NONE);
	assert(linkr_debugger_ota_route_from_path("/api/v1/ota/upload/") ==
	       LINKR_DEBUGGER_OTA_ROUTE_NONE);
	assert(linkr_debugger_ota_route_from_path("/api/v1/ota?verbose=1") ==
	       LINKR_DEBUGGER_OTA_ROUTE_NONE);
	assert(!linkr_debugger_ota_path_is_handled("/api/v1/ota/test?now=1"));
}

int main(void)
{
	test_size_header_parser();
	test_sha256_header_parser();
	test_content_type_validation();
	test_ota_route_matching();
	return 0;
}
