/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_ota.h"
#include "linkr_debugger_flash_arbiter.h"

#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#if !defined(LINKR_DEBUGGER_OTA_HOST_TEST)

#include "linkr_debugger_control.h"

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/parser.h>
#include <zephyr/net/http/status.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#endif

#if !defined(LINKR_DEBUGGER_OTA_HOST_TEST) || defined(LINKR_DEBUGGER_OTA_FULL_HOST_TEST)

LOG_MODULE_REGISTER(linkr_debugger_ota, CONFIG_LINKR_DEBUGGER_LOG_LEVEL);

HTTP_SERVER_REGISTER_HEADER_CAPTURE(linkr_debugger_ota_size_header_capture,
					    LINKR_DEBUGGER_OTA_SIZE_HEADER);
HTTP_SERVER_REGISTER_HEADER_CAPTURE(linkr_debugger_ota_sha256_header_capture,
					    LINKR_DEBUGGER_OTA_SHA256_HEADER);
HTTP_SERVER_REGISTER_HEADER_CAPTURE(linkr_debugger_ota_content_type_header_capture,
					    LINKR_DEBUGGER_OTA_CONTENT_TYPE_HEADER);

#define LINKR_DEBUGGER_OTA_JSON_BUFSZ 512U
#define LINKR_DEBUGGER_OTA_REBOOT_DELAY_MS 750U
#define LINKR_DEBUGGER_OTA_AUTO_CONFIRM_DELAY_MS 16000U

enum linkr_debugger_ota_state {
	LINKR_DEBUGGER_OTA_STATE_IDLE,
	LINKR_DEBUGGER_OTA_STATE_UPLOADING,
	LINKR_DEBUGGER_OTA_STATE_VERIFIED,
	LINKR_DEBUGGER_OTA_STATE_PENDING_TEST,
	LINKR_DEBUGGER_OTA_STATE_REBOOTING,
	LINKR_DEBUGGER_OTA_STATE_FAILED,
};

struct linkr_debugger_ota_env {
	char *buf;
	size_t cap;
	size_t len;
	bool truncated;
};

struct linkr_debugger_ota_status {
	enum linkr_debugger_ota_state state;
	size_t expected_size;
	size_t written_size;
	size_t max_size;
	uint8_t upload_area_id;
	int last_error;
	char last_error_code[32];
	bool current_image_confirmed;
	int swap_type;
};

static struct k_mutex linkr_debugger_ota_lock;
static struct k_work_delayable linkr_debugger_ota_reboot_work;
static struct k_work_delayable linkr_debugger_ota_auto_confirm_work;
static struct flash_img_context linkr_debugger_ota_flash_ctx;
static struct http_client_ctx *linkr_debugger_ota_upload_client;
static struct http_client_ctx *linkr_debugger_ota_failed_client;
static struct linkr_debugger_ota_status linkr_debugger_ota = {
	.state = LINKR_DEBUGGER_OTA_STATE_IDLE,
	.upload_area_id = 0xff,
};
static uint8_t linkr_debugger_ota_expected_sha[LINKR_DEBUGGER_OTA_SHA256_LEN];

#endif

static bool linkr_debugger_ota_hex_digit(char ch, uint8_t *value)
{
	if (ch >= '0' && ch <= '9') {
		*value = (uint8_t)(ch - '0');
		return true;
	}
	if (ch >= 'a' && ch <= 'f') {
		*value = (uint8_t)(10 + ch - 'a');
		return true;
	}
	if (ch >= 'A' && ch <= 'F') {
		*value = (uint8_t)(10 + ch - 'A');
		return true;
	}
	return false;
}

int linkr_debugger_ota_parse_size_header(const char *value, size_t *size)
{
	size_t parsed = 0U;

	if (value == NULL || value[0] == '\0' || size == NULL) {
		return -EINVAL;
	}

	for (const char *p = value; *p != '\0'; p++) {
		if (*p < '0' || *p > '9') {
			return -EINVAL;
		}
		if (parsed > (SIZE_MAX - (size_t)(*p - '0')) / 10U) {
			return -EOVERFLOW;
		}
		parsed = (parsed * 10U) + (size_t)(*p - '0');
	}

	if (parsed == 0U) {
		return -EINVAL;
	}

	*size = parsed;
	return 0;
}

int linkr_debugger_ota_parse_sha256_header(const char *value,
					   uint8_t sha256[LINKR_DEBUGGER_OTA_SHA256_LEN])
{
	if (value == NULL || sha256 == NULL || strlen(value) != LINKR_DEBUGGER_OTA_SHA256_HEX_LEN) {
		return -EINVAL;
	}

	for (size_t i = 0U; i < LINKR_DEBUGGER_OTA_SHA256_LEN; i++) {
		uint8_t high;
		uint8_t low;

		if (!linkr_debugger_ota_hex_digit(value[i * 2U], &high) ||
		    !linkr_debugger_ota_hex_digit(value[(i * 2U) + 1U], &low)) {
			return -EINVAL;
		}
		sha256[i] = (uint8_t)((high << 4) | low);
	}

	return 0;
}

bool linkr_debugger_ota_content_type_is_octet_stream(const char *value)
{
	const char *start;
	const char *end;
	size_t len;

	if (value == NULL) {
		return false;
	}

	start = value;
	while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n' ||
	       *start == '\f' || *start == '\v') {
		start++;
	}

	end = start + strlen(start);
	while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
			       end[-1] == '\n' || end[-1] == '\f' || end[-1] == '\v')) {
		end--;
	}

	len = (size_t)(end - start);
	return len == strlen(LINKR_DEBUGGER_OTA_CONTENT_TYPE) &&
	       strncasecmp(start, LINKR_DEBUGGER_OTA_CONTENT_TYPE, len) == 0;
}

enum linkr_debugger_ota_route linkr_debugger_ota_route_from_path(const char *path)
{
	if (path == NULL) {
		return LINKR_DEBUGGER_OTA_ROUTE_NONE;
	}
	if (strcmp(path, "/api/v1/ota") == 0) {
		return LINKR_DEBUGGER_OTA_ROUTE_STATUS;
	}
	if (strcmp(path, "/api/v1/ota/upload") == 0) {
		return LINKR_DEBUGGER_OTA_ROUTE_UPLOAD;
	}
	if (strcmp(path, "/api/v1/ota/test") == 0) {
		return LINKR_DEBUGGER_OTA_ROUTE_TEST;
	}
	if (strcmp(path, "/api/v1/ota/confirm") == 0) {
		return LINKR_DEBUGGER_OTA_ROUTE_CONFIRM;
	}

	return LINKR_DEBUGGER_OTA_ROUTE_NONE;
}

bool linkr_debugger_ota_path_is_handled(const char *path)
{
	return linkr_debugger_ota_route_from_path(path) != LINKR_DEBUGGER_OTA_ROUTE_NONE;
}

#if !defined(LINKR_DEBUGGER_OTA_HOST_TEST) || defined(LINKR_DEBUGGER_OTA_FULL_HOST_TEST)

static const char *linkr_debugger_ota_state_name(enum linkr_debugger_ota_state state)
{
	switch (state) {
	case LINKR_DEBUGGER_OTA_STATE_IDLE:
		return "idle";
	case LINKR_DEBUGGER_OTA_STATE_UPLOADING:
		return "uploading";
	case LINKR_DEBUGGER_OTA_STATE_VERIFIED:
		return "verified";
	case LINKR_DEBUGGER_OTA_STATE_PENDING_TEST:
		return "pending_test";
	case LINKR_DEBUGGER_OTA_STATE_REBOOTING:
		return "rebooting";
	case LINKR_DEBUGGER_OTA_STATE_FAILED:
		return "failed";
	default:
		return "unknown";
	}
}

static int linkr_debugger_ota_append(struct linkr_debugger_ota_env *env, const char *fmt, ...)
{
	va_list args;
	int written;

	if (env->len >= env->cap) {
		env->truncated = true;
		return -ENOMEM;
	}

	va_start(args, fmt);
	written = vsnprintk(env->buf + env->len, env->cap - env->len, fmt, args);
	va_end(args);

	if (written < 0) {
		return written;
	}
	if ((size_t)written >= env->cap - env->len) {
		env->len = env->cap - 1U;
		env->truncated = true;
		return -ENOMEM;
	}

	env->len += (size_t)written;
	return 0;
}

static void linkr_debugger_ota_set_json_response(struct http_response_ctx *response_ctx,
						 const uint8_t *body, size_t body_len,
						 enum http_status status)
{
	response_ctx->status = status;
	response_ctx->body = body;
	response_ctx->body_len = body_len;
	response_ctx->final_chunk = true;
}

static void linkr_debugger_ota_error(struct http_response_ctx *response_ctx, uint8_t *buf,
				     size_t buf_len, enum http_status status, const char *command,
				     const char *code, const char *message)
{
	int len = snprintk((char *)buf, buf_len,
			    "{\"schema\":\"%s\",\"ok\":false,\"command\":\"%s\"," \
			    "\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}\n",
			    linkr_debugger_json_schema(), command, code, message);

	if (len < 0) {
		len = 0;
	} else if ((size_t)len >= buf_len) {
		len = (int)buf_len - 1;
	}

	linkr_debugger_ota_set_json_response(response_ctx, buf, (size_t)len, status);
}

static void linkr_debugger_ota_upload_error(struct http_response_ctx *response_ctx,
					    uint8_t *buf, size_t buf_len)
{
	enum http_status status = HTTP_400_BAD_REQUEST;
	const char *code = linkr_debugger_ota.last_error_code[0] != '\0' ?
			   linkr_debugger_ota.last_error_code : "upload_failed";
	const char *message = "OTA upload failed validation";

	if (linkr_debugger_ota.last_error == -EBUSY) {
		status = HTTP_409_CONFLICT;
	} else if (linkr_debugger_ota.last_error == -EFBIG) {
		status = HTTP_413_PAYLOAD_TOO_LARGE;
	} else if (linkr_debugger_ota.last_error == -EPROTONOSUPPORT) {
		status = HTTP_415_UNSUPPORTED_MEDIA_TYPE;
		message = "OTA upload must use Content-Type application/octet-stream";
	}

	linkr_debugger_ota_error(response_ctx, buf, buf_len, status, "ota", code, message);
}

static const char *linkr_debugger_ota_header_value(const struct http_request_ctx *request_ctx,
						 const char *name)
{
	if (request_ctx == NULL || request_ctx->headers == NULL) {
		return NULL;
	}

	for (size_t i = 0U; i < request_ctx->header_count; i++) {
		if (request_ctx->headers[i].name != NULL &&
		    strcasecmp(request_ctx->headers[i].name, name) == 0) {
			return request_ctx->headers[i].value;
		}
	}

	return NULL;
}

static int linkr_debugger_ota_max_upload_size(size_t *max_size, uint8_t *area_id)
{
	const struct flash_area *area;
	uint8_t id = flash_img_get_upload_slot();
	uint32_t start_offset = boot_get_image_start_offset(id);
	int ret;

	ret = flash_area_open(id, &area);
	if (ret < 0) {
		return ret;
	}

	if (start_offset >= area->fa_size) {
		flash_area_close(area);
		return -ENOSPC;
	}

	*max_size = area->fa_size - start_offset;
	*area_id = id;
	flash_area_close(area);
	return 0;
}

static int linkr_debugger_ota_flash_owner_acquire(void)
{
	return linkr_debugger_flash_arbiter_try_acquire(LINKR_DEBUGGER_FLASH_OWNER_OTA) ?
		0 : -EBUSY;
}

static void linkr_debugger_ota_flash_owner_release(void)
{
	(void)linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_OTA);
}

static void linkr_debugger_ota_set_failed_locked(int error, const char *code)
{
	linkr_debugger_ota.state = LINKR_DEBUGGER_OTA_STATE_FAILED;
	linkr_debugger_ota.last_error = error;
	(void)snprintk(linkr_debugger_ota.last_error_code,
		       sizeof(linkr_debugger_ota.last_error_code), "%s", code);
	linkr_debugger_ota_flash_owner_release();
}

static void linkr_debugger_ota_flash_ctx_cleanup(void)
{
	if (linkr_debugger_ota_flash_ctx.flash_area != NULL) {
		flash_area_close(linkr_debugger_ota_flash_ctx.flash_area);
		linkr_debugger_ota_flash_ctx.flash_area = NULL;
	}
	linkr_debugger_ota_upload_client = NULL;
}

static void linkr_debugger_ota_refresh_persistent_state_locked(void)
{
	bool marker_present = linkr_debugger_watchdog_ota_test_marker_present();
	bool confirmed = boot_is_img_confirmed();

	if (marker_present) {
		if (!confirmed && linkr_debugger_ota.state != LINKR_DEBUGGER_OTA_STATE_UPLOADING &&
		    linkr_debugger_ota.state != LINKR_DEBUGGER_OTA_STATE_VERIFIED &&
		    linkr_debugger_ota.state != LINKR_DEBUGGER_OTA_STATE_REBOOTING) {
			linkr_debugger_ota.state = LINKR_DEBUGGER_OTA_STATE_PENDING_TEST;
		}
		return;
	}

	if (confirmed && linkr_debugger_ota.state == LINKR_DEBUGGER_OTA_STATE_PENDING_TEST) {
		linkr_debugger_ota.state = LINKR_DEBUGGER_OTA_STATE_IDLE;
	}
}

static int linkr_debugger_ota_begin_locked(const struct http_request_ctx *request_ctx)
{
	const char *size_header;
	const char *sha_header;
	const char *content_type;
	size_t expected_size;
	size_t max_size;
	uint8_t area_id;
	uint8_t sha[LINKR_DEBUGGER_OTA_SHA256_LEN];
	int ret;

	if (linkr_debugger_ota.state == LINKR_DEBUGGER_OTA_STATE_UPLOADING ||
	    linkr_debugger_ota.state == LINKR_DEBUGGER_OTA_STATE_PENDING_TEST ||
	    linkr_debugger_ota.state == LINKR_DEBUGGER_OTA_STATE_REBOOTING) {
		return -EBUSY;
	}
	ret = linkr_debugger_ota_flash_owner_acquire();
	if (ret < 0) {
		return ret;
	}

	if (request_ctx->headers_status == HTTP_HEADER_STATUS_DROPPED) {
		linkr_debugger_ota_set_failed_locked(-ENOBUFS, "headers_dropped");
		return -ENOBUFS;
	}

	content_type = linkr_debugger_ota_header_value(request_ctx,
						       LINKR_DEBUGGER_OTA_CONTENT_TYPE_HEADER);
	if (!linkr_debugger_ota_content_type_is_octet_stream(content_type)) {
		linkr_debugger_ota_set_failed_locked(-EPROTONOSUPPORT, "unsupported_content_type");
		return -EPROTONOSUPPORT;
	}

	size_header = linkr_debugger_ota_header_value(request_ctx, LINKR_DEBUGGER_OTA_SIZE_HEADER);
	sha_header = linkr_debugger_ota_header_value(request_ctx, LINKR_DEBUGGER_OTA_SHA256_HEADER);
	ret = linkr_debugger_ota_parse_size_header(size_header, &expected_size);
	if (ret < 0) {
		linkr_debugger_ota_set_failed_locked(ret, "invalid_size_header");
		return ret;
	}
	ret = linkr_debugger_ota_parse_sha256_header(sha_header, sha);
	if (ret < 0) {
		linkr_debugger_ota_set_failed_locked(ret, "invalid_sha256_header");
		return ret;
	}
	ret = linkr_debugger_ota_max_upload_size(&max_size, &area_id);
	if (ret < 0) {
		linkr_debugger_ota_set_failed_locked(ret, "slot_unavailable");
		return ret;
	}
	if (expected_size > max_size) {
		linkr_debugger_ota.expected_size = expected_size;
		linkr_debugger_ota.max_size = max_size;
		linkr_debugger_ota_set_failed_locked(-EFBIG, "image_too_large");
		return -EFBIG;
	}

	ret = flash_img_init(&linkr_debugger_ota_flash_ctx);
	if (ret < 0) {
		linkr_debugger_ota_set_failed_locked(ret, "flash_init_failed");
		linkr_debugger_ota_flash_ctx_cleanup();
		return ret;
	}

	memcpy(linkr_debugger_ota_expected_sha, sha, sizeof(linkr_debugger_ota_expected_sha));
	linkr_debugger_ota.state = LINKR_DEBUGGER_OTA_STATE_UPLOADING;
	linkr_debugger_ota.expected_size = expected_size;
	linkr_debugger_ota.written_size = 0U;
	linkr_debugger_ota.max_size = max_size;
	linkr_debugger_ota.upload_area_id = area_id;
	linkr_debugger_ota.last_error = 0;
	linkr_debugger_ota.last_error_code[0] = '\0';
	return 0;
}

static int linkr_debugger_ota_write_locked(const uint8_t *data, size_t len, bool final)
{
	struct flash_img_check check = {
		.match = linkr_debugger_ota_expected_sha,
		.clen = linkr_debugger_ota.expected_size,
	};
	struct mcuboot_img_header header;
	int ret;

	if (linkr_debugger_ota.state != LINKR_DEBUGGER_OTA_STATE_UPLOADING) {
		return -EALREADY;
	}
	if (len > linkr_debugger_ota.expected_size - linkr_debugger_ota.written_size) {
		linkr_debugger_ota_set_failed_locked(-EFBIG, "upload_too_large");
		linkr_debugger_ota_flash_ctx_cleanup();
		return -EFBIG;
	}

	ret = flash_img_buffered_write(&linkr_debugger_ota_flash_ctx, data, len, final);
	if (ret < 0) {
		linkr_debugger_ota_set_failed_locked(ret, "flash_write_failed");
		linkr_debugger_ota_flash_ctx_cleanup();
		return ret;
	}

	linkr_debugger_ota.written_size += len;
	if (!final) {
		return 0;
	}
	if (linkr_debugger_ota.written_size != linkr_debugger_ota.expected_size) {
		linkr_debugger_ota_set_failed_locked(-EMSGSIZE, "size_mismatch");
		linkr_debugger_ota_flash_ctx_cleanup();
		return -EMSGSIZE;
	}

	ret = flash_img_check(&linkr_debugger_ota_flash_ctx, &check, linkr_debugger_ota.upload_area_id);
	if (ret < 0) {
		linkr_debugger_ota_set_failed_locked(ret, "sha256_mismatch");
		linkr_debugger_ota_flash_ctx_cleanup();
		return ret;
	}
	linkr_debugger_ota_flash_ctx_cleanup();

	ret = boot_read_bank_header(linkr_debugger_ota.upload_area_id, &header, sizeof(header));
	if (ret < 0 || header.mcuboot_version != 1U || header.h.v1.image_size == 0U ||
	    header.h.v1.image_size > linkr_debugger_ota.expected_size) {
		linkr_debugger_ota_set_failed_locked(ret < 0 ? ret : -EINVAL, "invalid_mcuboot_header");
		linkr_debugger_ota_flash_ctx_cleanup();
		return ret < 0 ? ret : -EINVAL;
	}

	linkr_debugger_ota.state = LINKR_DEBUGGER_OTA_STATE_VERIFIED;
	linkr_debugger_ota_flash_owner_release();
	return 0;
}

static void linkr_debugger_ota_abort_locked(void)
{
	if (linkr_debugger_ota.state == LINKR_DEBUGGER_OTA_STATE_UPLOADING) {
		linkr_debugger_ota_set_failed_locked(-ECANCELED, "upload_aborted");
		linkr_debugger_ota_flash_ctx_cleanup();
	}
}

static int linkr_debugger_ota_encode_status(struct linkr_debugger_ota_env *env)
{
	int swap_type = mcuboot_swap_type();
	bool confirmed = boot_is_img_confirmed();

	return linkr_debugger_ota_append(env,
		"{\"schema\":\"%s\",\"ok\":true,\"command\":\"ota\"," \
		"\"state\":\"%s\",\"expected_size\":%u,\"written_size\":%u," \
		"\"max_size\":%u,\"upload_area_id\":%u,\"swap_type\":%d," \
		"\"current_image_confirmed\":%s,\"metadata\":{" \
		"\"size_header\":\"%s\",\"sha256_header\":\"%s\"}",
		linkr_debugger_json_schema(), linkr_debugger_ota_state_name(linkr_debugger_ota.state),
		(unsigned int)linkr_debugger_ota.expected_size,
		(unsigned int)linkr_debugger_ota.written_size,
		(unsigned int)linkr_debugger_ota.max_size,
		(unsigned int)linkr_debugger_ota.upload_area_id, swap_type,
		confirmed ? "true" : "false", LINKR_DEBUGGER_OTA_SIZE_HEADER,
		LINKR_DEBUGGER_OTA_SHA256_HEADER);
}

static int linkr_debugger_ota_finish_status(struct linkr_debugger_ota_env *env)
{
	if (linkr_debugger_ota.state == LINKR_DEBUGGER_OTA_STATE_FAILED) {
		return linkr_debugger_ota_append(env, ",\"last_error\":{\"code\":\"%s\",\"errno\":%d}}\n",
					       linkr_debugger_ota.last_error_code,
					       linkr_debugger_ota.last_error);
	}

	return linkr_debugger_ota_append(env, "}\n");
}

static void linkr_debugger_ota_auto_confirm_work_handler(struct k_work *work);

static void linkr_debugger_ota_reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	k_mutex_lock(&linkr_debugger_ota_lock, K_FOREVER);
	if (linkr_debugger_ota.state != LINKR_DEBUGGER_OTA_STATE_REBOOTING) {
		k_mutex_unlock(&linkr_debugger_ota_lock);
		return;
	}
	if (!linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_OTA)) {
		k_mutex_unlock(&linkr_debugger_ota_lock);
		LOG_ERR("OTA reboot skipped because flash ownership was lost");
		return;
	}
	k_mutex_unlock(&linkr_debugger_ota_lock);
	sys_reboot(SYS_REBOOT_COLD);
}

void linkr_debugger_ota_init(void)
{
#ifdef LINKR_DEBUGGER_OTA_FULL_HOST_TEST
	linkr_debugger_ota_flash_ctx_cleanup();
	memset(&linkr_debugger_ota, 0, sizeof(linkr_debugger_ota));
	linkr_debugger_ota.state = LINKR_DEBUGGER_OTA_STATE_IDLE;
	linkr_debugger_ota.upload_area_id = 0xff;
	linkr_debugger_ota_failed_client = NULL;
	memset(linkr_debugger_ota_expected_sha, 0, sizeof(linkr_debugger_ota_expected_sha));
#endif
	linkr_debugger_ota_flash_owner_release();
	k_mutex_init(&linkr_debugger_ota_lock);
	k_work_init_delayable(&linkr_debugger_ota_reboot_work, linkr_debugger_ota_reboot_work_handler);
	k_work_init_delayable(&linkr_debugger_ota_auto_confirm_work,
				linkr_debugger_ota_auto_confirm_work_handler);
	k_mutex_lock(&linkr_debugger_ota_lock, K_FOREVER);
	linkr_debugger_ota_refresh_persistent_state_locked();
	k_mutex_unlock(&linkr_debugger_ota_lock);
}

void linkr_debugger_ota_auto_confirm_ready(void)
{
	if (!linkr_debugger_watchdog_ota_test_marker_present()) {
		return;
	}

	(void)k_work_reschedule(&linkr_debugger_ota_auto_confirm_work,
				 K_MSEC(LINKR_DEBUGGER_OTA_AUTO_CONFIRM_DELAY_MS));
}

static void linkr_debugger_ota_auto_confirm_work_handler(struct k_work *work)
{
	struct linkr_debugger_watchdog_status watchdog;
	bool retry = false;
	int ret;

	ARG_UNUSED(work);

	if (!linkr_debugger_watchdog_ota_test_marker_present()) {
		return;
	}

	k_mutex_lock(&linkr_debugger_ota_lock, K_FOREVER);
	linkr_debugger_ota_refresh_persistent_state_locked();
	if (!linkr_debugger_watchdog_ota_test_marker_present()) {
		k_mutex_unlock(&linkr_debugger_ota_lock);
		return;
	}
	if (linkr_debugger_ota.state != LINKR_DEBUGGER_OTA_STATE_PENDING_TEST) {
		retry = true;
	}
	/* After an MCUboot rollback the running image is already confirmed, but the
	 * retained OTA marker still has to be cleared without another confirm write.
	 */
	if (boot_is_img_confirmed() &&
	    linkr_debugger_ota.state == LINKR_DEBUGGER_OTA_STATE_IDLE) {
		if (!linkr_debugger_watchdog_fault_injection_confirm_begin()) {
			k_mutex_unlock(&linkr_debugger_ota_lock);
			(void)k_work_reschedule(&linkr_debugger_ota_auto_confirm_work,
						 K_MSEC(LINKR_DEBUGGER_OTA_AUTO_CONFIRM_DELAY_MS));
			return;
		}
		linkr_debugger_watchdog_ota_test_marker_clear();
		linkr_debugger_watchdog_fault_injection_confirm_end();
		linkr_debugger_ota.state = LINKR_DEBUGGER_OTA_STATE_IDLE;
		k_mutex_unlock(&linkr_debugger_ota_lock);
		return;
	} else if (retry) {
		k_mutex_unlock(&linkr_debugger_ota_lock);
		(void)k_work_reschedule(&linkr_debugger_ota_auto_confirm_work,
			 K_MSEC(LINKR_DEBUGGER_OTA_AUTO_CONFIRM_DELAY_MS));
		return;
	}
	k_mutex_unlock(&linkr_debugger_ota_lock);

	linkr_debugger_watchdog_status_get(&watchdog);
	if (!watchdog.supported || !watchdog.armed || !watchdog.healthy) {
		LOG_WRN("MCUboot image auto-confirm skipped: watchdog supported=%d armed=%d healthy=%d",
			watchdog.supported ? 1 : 0, watchdog.armed ? 1 : 0,
			watchdog.healthy ? 1 : 0);
		(void)k_work_reschedule(&linkr_debugger_ota_auto_confirm_work,
					 K_MSEC(LINKR_DEBUGGER_OTA_AUTO_CONFIRM_DELAY_MS));
		return;
	}

	k_mutex_lock(&linkr_debugger_ota_lock, K_FOREVER);
	linkr_debugger_ota_refresh_persistent_state_locked();
	if (!linkr_debugger_watchdog_ota_test_marker_present()) {
		k_mutex_unlock(&linkr_debugger_ota_lock);
		return;
	}
	if (boot_is_img_confirmed() &&
	    linkr_debugger_ota.state == LINKR_DEBUGGER_OTA_STATE_IDLE) {
		if (!linkr_debugger_watchdog_fault_injection_confirm_begin()) {
			k_mutex_unlock(&linkr_debugger_ota_lock);
			(void)k_work_reschedule(&linkr_debugger_ota_auto_confirm_work,
						 K_MSEC(LINKR_DEBUGGER_OTA_AUTO_CONFIRM_DELAY_MS));
			return;
		}
		linkr_debugger_watchdog_ota_test_marker_clear();
		linkr_debugger_watchdog_fault_injection_confirm_end();
		linkr_debugger_ota.state = LINKR_DEBUGGER_OTA_STATE_IDLE;
		k_mutex_unlock(&linkr_debugger_ota_lock);
		return;
	}
	if (linkr_debugger_ota.state != LINKR_DEBUGGER_OTA_STATE_PENDING_TEST ||
	    linkr_debugger_ota_flash_owner_acquire() < 0) {
		k_mutex_unlock(&linkr_debugger_ota_lock);
		(void)k_work_reschedule(&linkr_debugger_ota_auto_confirm_work,
					 K_MSEC(LINKR_DEBUGGER_OTA_AUTO_CONFIRM_DELAY_MS));
		return;
	}
	if (!linkr_debugger_watchdog_fault_injection_confirm_begin()) {
		linkr_debugger_ota_flash_owner_release();
		k_mutex_unlock(&linkr_debugger_ota_lock);
		(void)k_work_reschedule(&linkr_debugger_ota_auto_confirm_work,
					 K_MSEC(LINKR_DEBUGGER_OTA_AUTO_CONFIRM_DELAY_MS));
		return;
	}
	ret = boot_write_img_confirmed();
	if (ret < 0) {
		linkr_debugger_watchdog_fault_injection_confirm_end();
		linkr_debugger_ota_flash_owner_release();
		k_mutex_unlock(&linkr_debugger_ota_lock);
		LOG_WRN("MCUboot image auto-confirm failed: %d", ret);
		return;
	}

	linkr_debugger_watchdog_ota_test_marker_clear();
	linkr_debugger_watchdog_fault_injection_confirm_end();
	linkr_debugger_ota_flash_owner_release();
	linkr_debugger_ota.state = LINKR_DEBUGGER_OTA_STATE_IDLE;
	k_mutex_unlock(&linkr_debugger_ota_lock);
	LOG_INF("MCUboot test image confirmed after watchdog health gate");
}

static int linkr_debugger_ota_handle_status(struct http_response_ctx *response_ctx,
						   uint8_t *json_buf, size_t json_buf_len)
{
	struct linkr_debugger_ota_env env = {
		.buf = (char *)json_buf,
		.cap = json_buf_len,
	};

	k_mutex_lock(&linkr_debugger_ota_lock, K_FOREVER);
	linkr_debugger_ota_refresh_persistent_state_locked();
	if (linkr_debugger_ota_encode_status(&env) < 0 ||
	    linkr_debugger_ota_append(&env, ",\"test_marker_present\":%s",
				       linkr_debugger_watchdog_ota_test_marker_present() ?
				       "true" : "false") < 0 ||
	    linkr_debugger_ota_finish_status(&env) < 0) {
		k_mutex_unlock(&linkr_debugger_ota_lock);
		linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
				       HTTP_500_INTERNAL_SERVER_ERROR, "ota", "response_too_large",
				       "failed to encode OTA status");
		return 0;
	}
	k_mutex_unlock(&linkr_debugger_ota_lock);

	linkr_debugger_ota_set_json_response(response_ctx, json_buf, env.len, HTTP_200_OK);
	return 0;
}

static int linkr_debugger_ota_handle_test(struct http_response_ctx *response_ctx, uint8_t *json_buf,
						 size_t json_buf_len)
{
	struct linkr_debugger_ota_env env = {
		.buf = (char *)json_buf,
		.cap = json_buf_len,
	};
	int ret;

	k_mutex_lock(&linkr_debugger_ota_lock, K_FOREVER);
	if (linkr_debugger_ota.state != LINKR_DEBUGGER_OTA_STATE_VERIFIED) {
		k_mutex_unlock(&linkr_debugger_ota_lock);
		linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
					       HTTP_409_CONFLICT, "ota", "no_verified_image",
					       "no verified OTA image is ready for test upgrade");
		return 0;
	}

	ret = linkr_debugger_ota_flash_owner_acquire();
	if (ret < 0) {
		k_mutex_unlock(&linkr_debugger_ota_lock);
		linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
				       HTTP_409_CONFLICT, "ota", "flash_busy",
				       "flash is busy with another operation");
		return 0;
	}

	linkr_debugger_ota.state = LINKR_DEBUGGER_OTA_STATE_REBOOTING;
	if (linkr_debugger_ota_encode_status(&env) < 0 ||
	    linkr_debugger_ota_append(&env, ",\"reboot_delay_ms\":%u}\n",
				     LINKR_DEBUGGER_OTA_REBOOT_DELAY_MS) < 0) {
		linkr_debugger_ota.state = LINKR_DEBUGGER_OTA_STATE_VERIFIED;
		linkr_debugger_ota_flash_owner_release();
		k_mutex_unlock(&linkr_debugger_ota_lock);
		linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
					       HTTP_500_INTERNAL_SERVER_ERROR, "ota", "response_too_large",
					       "failed to encode OTA test response");
		return 0;
	}

	ret = k_work_reschedule(&linkr_debugger_ota_reboot_work,
				K_MSEC(LINKR_DEBUGGER_OTA_REBOOT_DELAY_MS));
	if (ret < 0) {
		linkr_debugger_ota.state = LINKR_DEBUGGER_OTA_STATE_VERIFIED;
		linkr_debugger_ota_flash_owner_release();
		k_mutex_unlock(&linkr_debugger_ota_lock);
		linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
				       HTTP_500_INTERNAL_SERVER_ERROR, "ota", "schedule_reboot_failed",
				       "failed to schedule OTA test reboot");
		return 0;
	}

	linkr_debugger_watchdog_ota_test_marker_set();
	ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (ret < 0) {
		(void)k_work_cancel_delayable(&linkr_debugger_ota_reboot_work);
		linkr_debugger_watchdog_ota_test_marker_clear();
		linkr_debugger_ota.state = LINKR_DEBUGGER_OTA_STATE_VERIFIED;
		linkr_debugger_ota_flash_owner_release();
		k_mutex_unlock(&linkr_debugger_ota_lock);
		linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
					       HTTP_500_INTERNAL_SERVER_ERROR, "ota", "request_upgrade_failed",
				       "failed to mark OTA image for test upgrade");
		return 0;
	}

	ret = linkr_debugger_watchdog_prepare_planned_reboot();
	if (ret < 0) {
		LOG_ERR("watchdog disable failed after OTA test request was committed: %d", ret);
	}
	k_mutex_unlock(&linkr_debugger_ota_lock);

	linkr_debugger_ota_set_json_response(response_ctx, json_buf, env.len, HTTP_202_ACCEPTED);
	return 0;
}

static int linkr_debugger_ota_handle_confirm(struct http_response_ctx *response_ctx,
					     uint8_t *json_buf, size_t json_buf_len)
{
	struct linkr_debugger_ota_env env = {
		.buf = (char *)json_buf,
		.cap = json_buf_len,
	};
	int ret;

	k_mutex_lock(&linkr_debugger_ota_lock, K_FOREVER);
	if (linkr_debugger_ota.state == LINKR_DEBUGGER_OTA_STATE_UPLOADING) {
		k_mutex_unlock(&linkr_debugger_ota_lock);
		linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
					       HTTP_409_CONFLICT, "ota", "upload_in_progress",
					       "another OTA operation is already active");
		return 0;
	}
	if (!linkr_debugger_watchdog_fault_injection_confirm_begin()) {
		k_mutex_unlock(&linkr_debugger_ota_lock);
		linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
					       HTTP_409_CONFLICT, "ota", "fault_injection_armed",
					       "watchdog fault injection is active; confirm is blocked");
		return 0;
	}
	ret = linkr_debugger_ota_flash_owner_acquire();
	if (ret < 0) {
		linkr_debugger_watchdog_fault_injection_confirm_end();
		k_mutex_unlock(&linkr_debugger_ota_lock);
		linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
					       HTTP_409_CONFLICT, "ota", "flash_busy",
					       "flash is busy with another operation");
		return 0;
	}

	ret = boot_write_img_confirmed();

	if (ret < 0) {
		linkr_debugger_watchdog_fault_injection_confirm_end();
		linkr_debugger_ota_flash_owner_release();
		k_mutex_unlock(&linkr_debugger_ota_lock);
		linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
				       HTTP_500_INTERNAL_SERVER_ERROR, "ota", "confirm_failed",
				       "failed to confirm running MCUboot image");
		return 0;
	}

	linkr_debugger_watchdog_ota_test_marker_clear();
	linkr_debugger_watchdog_fault_injection_confirm_end();

	linkr_debugger_ota.state = LINKR_DEBUGGER_OTA_STATE_IDLE;
	if (linkr_debugger_ota_encode_status(&env) < 0 ||
	    linkr_debugger_ota_finish_status(&env) < 0) {
		linkr_debugger_ota_flash_owner_release();
		k_mutex_unlock(&linkr_debugger_ota_lock);
		linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
				       HTTP_500_INTERNAL_SERVER_ERROR, "ota", "response_too_large",
				       "failed to encode OTA confirm response");
		return 0;
	}
	linkr_debugger_ota_flash_owner_release();
	k_mutex_unlock(&linkr_debugger_ota_lock);

	linkr_debugger_ota_set_json_response(response_ctx, json_buf, env.len, HTTP_200_OK);
	return 0;
}

static int linkr_debugger_ota_handle_upload(struct http_client_ctx *client,
					    enum http_transaction_status status,
					    const struct http_request_ctx *request_ctx,
					    struct http_response_ctx *response_ctx,
					    uint8_t *json_buf, size_t json_buf_len)
{
	struct linkr_debugger_ota_env env = {
		.buf = (char *)json_buf,
		.cap = json_buf_len,
	};
	int ret;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED) {
		k_mutex_lock(&linkr_debugger_ota_lock, K_FOREVER);
		if (client == linkr_debugger_ota_upload_client) {
			linkr_debugger_ota_abort_locked();
		}
		if (client == linkr_debugger_ota_failed_client) {
			linkr_debugger_ota_failed_client = NULL;
		}
		k_mutex_unlock(&linkr_debugger_ota_lock);
		return 0;
	}
	if (status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		k_mutex_lock(&linkr_debugger_ota_lock, K_FOREVER);
		if (client == linkr_debugger_ota_upload_client) {
			linkr_debugger_ota_abort_locked();
		}
		if (client == linkr_debugger_ota_failed_client) {
			linkr_debugger_ota_failed_client = NULL;
		}
		k_mutex_unlock(&linkr_debugger_ota_lock);
		return 0;
	}

	k_mutex_lock(&linkr_debugger_ota_lock, K_FOREVER);
	if (request_ctx->headers != NULL) {
		ret = linkr_debugger_ota_begin_locked(request_ctx);
		if (ret < 0) {
			if (linkr_debugger_ota.state != LINKR_DEBUGGER_OTA_STATE_FAILED) {
				k_mutex_unlock(&linkr_debugger_ota_lock);
				linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
						       HTTP_409_CONFLICT, "ota", "upload_in_progress",
						       "another OTA operation is already active");
				return 0;
			}
			linkr_debugger_ota_failed_client = client;
			if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
				linkr_debugger_ota_upload_error(response_ctx, json_buf, json_buf_len);
			}
			k_mutex_unlock(&linkr_debugger_ota_lock);
			return 0;
		}
		linkr_debugger_ota_failed_client = NULL;
		linkr_debugger_ota_upload_client = client;
	} else if (linkr_debugger_ota.state == LINKR_DEBUGGER_OTA_STATE_FAILED &&
		   client == linkr_debugger_ota_failed_client) {
		if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
			linkr_debugger_ota_upload_error(response_ctx, json_buf, json_buf_len);
		}
		k_mutex_unlock(&linkr_debugger_ota_lock);
		return 0;
	} else if (linkr_debugger_ota.state != LINKR_DEBUGGER_OTA_STATE_UPLOADING) {
		linkr_debugger_ota_set_failed_locked(-EPROTO, "upload_not_started");
		k_mutex_unlock(&linkr_debugger_ota_lock);
		linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
				       HTTP_400_BAD_REQUEST, "ota", "upload_not_started",
				       "OTA upload metadata was not received");
		return 0;
	} else if (client != linkr_debugger_ota_upload_client) {
		k_mutex_unlock(&linkr_debugger_ota_lock);
		linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
				       HTTP_409_CONFLICT, "ota", "upload_in_progress",
				       "another client owns the active OTA upload");
		return 0;
	}

	ret = linkr_debugger_ota_write_locked(request_ctx->data, request_ctx->data_len,
					       status == HTTP_SERVER_REQUEST_DATA_FINAL);
	if (ret < 0) {
		linkr_debugger_ota_failed_client = client;
		if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
			linkr_debugger_ota_upload_error(response_ctx, json_buf, json_buf_len);
		}
		k_mutex_unlock(&linkr_debugger_ota_lock);
		return 0;
	}

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		k_mutex_unlock(&linkr_debugger_ota_lock);
		return 0;
	}

	if (linkr_debugger_ota_encode_status(&env) < 0 ||
	    linkr_debugger_ota_finish_status(&env) < 0) {
		k_mutex_unlock(&linkr_debugger_ota_lock);
		linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
				       HTTP_500_INTERNAL_SERVER_ERROR, "ota", "response_too_large",
				       "failed to encode OTA upload response");
		return 0;
	}
	k_mutex_unlock(&linkr_debugger_ota_lock);

	linkr_debugger_ota_set_json_response(response_ctx, json_buf, env.len, HTTP_200_OK);
	return 0;
}

int linkr_debugger_ota_http_handle(struct http_client_ctx *client,
				  enum http_transaction_status status,
				  const struct http_request_ctx *request_ctx,
				  struct http_response_ctx *response_ctx,
				  void *user_data)
{
	uint8_t *json_buf = client->buffer;
	size_t json_buf_len = sizeof(client->buffer);
	char *path = (char *)client->url_buffer;
	enum linkr_debugger_ota_route route = user_data != NULL ?
		*(const enum linkr_debugger_ota_route *)user_data :
		linkr_debugger_ota_route_from_path(path);

	if (route == LINKR_DEBUGGER_OTA_ROUTE_UPLOAD) {
		if (client->method != HTTP_POST) {
			linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
					       HTTP_405_METHOD_NOT_ALLOWED, "ota", "method_not_allowed",
					       "method not allowed");
			return 0;
		}
		return linkr_debugger_ota_handle_upload(client, status, request_ctx, response_ctx,
						       json_buf, json_buf_len);
	}

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	if (route == LINKR_DEBUGGER_OTA_ROUTE_STATUS) {
		if (client->method != HTTP_GET) {
			linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
					       HTTP_405_METHOD_NOT_ALLOWED, "ota", "method_not_allowed",
					       "method not allowed");
			return 0;
		}
		return linkr_debugger_ota_handle_status(response_ctx, json_buf, json_buf_len);
	}

	if (route == LINKR_DEBUGGER_OTA_ROUTE_TEST) {
		if (client->method != HTTP_POST) {
			linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
					       HTTP_405_METHOD_NOT_ALLOWED, "ota", "method_not_allowed",
					       "method not allowed");
			return 0;
		}
		return linkr_debugger_ota_handle_test(response_ctx, json_buf, json_buf_len);
	}

	if (route == LINKR_DEBUGGER_OTA_ROUTE_CONFIRM) {
		if (client->method != HTTP_POST) {
			linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len,
					       HTTP_405_METHOD_NOT_ALLOWED, "ota", "method_not_allowed",
					       "method not allowed");
			return 0;
		}
		return linkr_debugger_ota_handle_confirm(response_ctx, json_buf, json_buf_len);
	}

	linkr_debugger_ota_error(response_ctx, json_buf, json_buf_len, HTTP_404_NOT_FOUND,
			       "ota", "not_found", "unknown OTA path");
	return 0;
}

#else

void linkr_debugger_ota_init(void) {}
void linkr_debugger_ota_auto_confirm_ready(void) {}

#endif
