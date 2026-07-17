/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_OTA_H_
#define RADXA_LINKR_DEBUGGER_OTA_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef LINKR_DEBUGGER_OTA_HOST_TEST
#include <zephyr/net/http/server.h>
#endif

#define LINKR_DEBUGGER_OTA_SIZE_HEADER "X-Linkr-Ota-Size"
#define LINKR_DEBUGGER_OTA_SHA256_HEADER "X-Linkr-Ota-Sha256"
#define LINKR_DEBUGGER_OTA_CONTENT_TYPE_HEADER "Content-Type"
#define LINKR_DEBUGGER_OTA_CONTENT_TYPE "application/octet-stream"
#define LINKR_DEBUGGER_OTA_SHA256_HEX_LEN 64U
#define LINKR_DEBUGGER_OTA_SHA256_LEN 32U

enum linkr_debugger_ota_route {
	LINKR_DEBUGGER_OTA_ROUTE_NONE,
	LINKR_DEBUGGER_OTA_ROUTE_STATUS,
	LINKR_DEBUGGER_OTA_ROUTE_UPLOAD,
	LINKR_DEBUGGER_OTA_ROUTE_TEST,
	LINKR_DEBUGGER_OTA_ROUTE_CONFIRM,
};

int linkr_debugger_ota_parse_size_header(const char *value, size_t *size);
int linkr_debugger_ota_parse_sha256_header(const char *value,
					   uint8_t sha256[LINKR_DEBUGGER_OTA_SHA256_LEN]);
bool linkr_debugger_ota_content_type_is_octet_stream(const char *value);
enum linkr_debugger_ota_route linkr_debugger_ota_route_from_path(const char *path);
bool linkr_debugger_ota_path_is_handled(const char *path);

void linkr_debugger_ota_init(void);
void linkr_debugger_ota_auto_confirm_ready(void);
#ifndef LINKR_DEBUGGER_OTA_HOST_TEST
int linkr_debugger_ota_http_handle(struct http_client_ctx *client,
				  enum http_transaction_status status,
				  const struct http_request_ctx *request_ctx,
				  struct http_response_ctx *response_ctx,
				  void *user_data);
#endif

#endif /* RADXA_LINKR_DEBUGGER_OTA_H_ */
