/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_flash_arbiter.h"
#include "linkr_debugger_ota.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

static struct flash_area fake_area;
static int fake_area_open_result;
static int fake_flash_init_result;
static int fake_write_result;
static int fake_check_result;
static int fake_header_result;
static int fake_schedule_result;
static int fake_upgrade_result;
static int fake_confirm_result;
static int fake_vsnprintk_fail_count;
static bool fake_header_valid;
static bool fake_marker_present;
static bool fake_image_confirmed;
static int fake_reboot_count;
static int fake_cancel_count;
static int fake_upgrade_call_count;
static int fake_confirm_call_count;
static int fake_auto_confirm_schedule_count;
static struct k_work_delayable *fake_reboot_work;
static struct k_work_delayable *fake_auto_confirm_work;
static pthread_mutex_t fake_reboot_sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_reboot_sync_cond = PTHREAD_COND_INITIALIZER;
static pthread_t fake_reboot_thread;
static bool fake_reboot_thread_created;
static bool fake_reboot_start_on_schedule;
static bool fake_reboot_lock_attempted;
static bool fake_reboot_thread_finished;
static bool fake_reboot_hold_before_mutex;
static bool fake_reboot_allow_mutex;
static _Thread_local bool fake_reboot_thread_context;

static const struct http_header good_headers[] = {
	{ LINKR_DEBUGGER_OTA_CONTENT_TYPE_HEADER, LINKR_DEBUGGER_OTA_CONTENT_TYPE },
	{ LINKR_DEBUGGER_OTA_SIZE_HEADER, "4" },
	{ LINKR_DEBUGGER_OTA_SHA256_HEADER,
	  "0000000000000000000000000000000000000000000000000000000000000000" },
};

void k_mutex_init(struct k_mutex *mutex)
{
	if (mutex->initialized) {
		assert(pthread_mutex_destroy(&mutex->native) == 0);
	}
	assert(pthread_mutex_init(&mutex->native, NULL) == 0);
	mutex->initialized = true;
}

void k_mutex_lock(struct k_mutex *mutex, int timeout)
{
	ARG_UNUSED(timeout);
	if (fake_reboot_thread_context) {
		assert(pthread_mutex_lock(&fake_reboot_sync_lock) == 0);
		fake_reboot_lock_attempted = true;
		assert(pthread_cond_broadcast(&fake_reboot_sync_cond) == 0);
		while (fake_reboot_hold_before_mutex && !fake_reboot_allow_mutex) {
			assert(pthread_cond_wait(&fake_reboot_sync_cond,
						 &fake_reboot_sync_lock) == 0);
		}
		assert(pthread_mutex_unlock(&fake_reboot_sync_lock) == 0);
	}
	assert(pthread_mutex_lock(&mutex->native) == 0);
}

void k_mutex_unlock(struct k_mutex *mutex)
{
	assert(pthread_mutex_unlock(&mutex->native) == 0);
}

static void *run_scheduled_reboot_work(void *arg)
{
	struct k_work_delayable *work = arg;

	fake_reboot_thread_context = true;
	work->work.handler(&work->work);
	assert(pthread_mutex_lock(&fake_reboot_sync_lock) == 0);
	fake_reboot_thread_finished = true;
	assert(pthread_cond_broadcast(&fake_reboot_sync_cond) == 0);
	assert(pthread_mutex_unlock(&fake_reboot_sync_lock) == 0);
	return NULL;
}

void k_work_init_delayable(struct k_work_delayable *work,
			   void (*handler)(struct k_work *work))
{
	work->work.handler = handler;
	if (fake_reboot_work == NULL) {
		fake_reboot_work = work;
	} else {
		fake_auto_confirm_work = work;
	}
}

int k_work_reschedule(struct k_work_delayable *work, int delay_ms)
{
	ARG_UNUSED(delay_ms);
	if (work == fake_auto_confirm_work) {
		fake_auto_confirm_schedule_count++;
	} else if (fake_reboot_start_on_schedule && fake_schedule_result >= 0) {
		assert(!fake_reboot_thread_created);
		fake_reboot_thread_created = true;
		assert(pthread_create(&fake_reboot_thread, NULL, run_scheduled_reboot_work,
				      work) == 0);
		assert(pthread_mutex_lock(&fake_reboot_sync_lock) == 0);
		while (!fake_reboot_lock_attempted && !fake_reboot_thread_finished) {
			assert(pthread_cond_wait(&fake_reboot_sync_cond,
						 &fake_reboot_sync_lock) == 0);
		}
		assert(pthread_mutex_unlock(&fake_reboot_sync_lock) == 0);
	}
	return fake_schedule_result;
}

int k_work_cancel_delayable(struct k_work_delayable *work)
{
	ARG_UNUSED(work);
	fake_cancel_count++;
	return 0;
}

uint8_t flash_img_get_upload_slot(void)
{
	return 1U;
}

uint32_t boot_get_image_start_offset(uint8_t area_id)
{
	ARG_UNUSED(area_id);
	return 0U;
}

int flash_area_open(uint8_t area_id, const struct flash_area **area)
{
	ARG_UNUSED(area_id);
	if (fake_area_open_result < 0) {
		return fake_area_open_result;
	}
	*area = &fake_area;
	return 0;
}

void flash_area_close(const struct flash_area *area)
{
	ARG_UNUSED(area);
}

int flash_img_init(struct flash_img_context *context)
{
	if (fake_flash_init_result < 0) {
		return fake_flash_init_result;
	}
	context->flash_area = &fake_area;
	return 0;
}

int flash_img_buffered_write(struct flash_img_context *context,
			     const uint8_t *data, size_t len, bool final)
{
	ARG_UNUSED(context);
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	ARG_UNUSED(final);
	return fake_write_result;
}

int flash_img_check(struct flash_img_context *context,
		    const struct flash_img_check *check, uint8_t area_id)
{
	ARG_UNUSED(context);
	ARG_UNUSED(check);
	ARG_UNUSED(area_id);
	return fake_check_result;
}

int boot_read_bank_header(uint8_t area_id, struct mcuboot_img_header *header,
			  size_t header_size)
{
	ARG_UNUSED(area_id);
	ARG_UNUSED(header_size);
	if (fake_header_result < 0) {
		return fake_header_result;
	}
	header->mcuboot_version = fake_header_valid ? 1U : 0U;
	header->h.v1.image_size = fake_header_valid ? 4U : 0U;
	return 0;
}

int mcuboot_swap_type(void)
{
	return 0;
}

bool boot_is_img_confirmed(void)
{
	return fake_image_confirmed;
}

int boot_write_img_confirmed(void)
{
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_OTA);
	fake_confirm_call_count++;
	return fake_confirm_result;
}

int boot_request_upgrade(int upgrade_type)
{
	ARG_UNUSED(upgrade_type);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_OTA);
	fake_upgrade_call_count++;
	return fake_upgrade_result;
}

void sys_reboot(int type)
{
	ARG_UNUSED(type);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_NONE);
	fake_reboot_count++;
}

const char *linkr_debugger_json_schema(void)
{
	return "radxa-linkr-debugger.v1";
}

bool linkr_debugger_watchdog_ota_test_marker_present(void)
{
	return fake_marker_present;
}

void linkr_debugger_watchdog_ota_test_marker_set(void)
{
	fake_marker_present = true;
}

void linkr_debugger_watchdog_ota_test_marker_clear(void)
{
	fake_marker_present = false;
}

void linkr_debugger_watchdog_status_get(struct linkr_debugger_watchdog_status *status)
{
	status->supported = true;
	status->armed = true;
	status->healthy = true;
}

int linkr_debugger_watchdog_prepare_planned_reboot(void)
{
	return 0;
}

int test_vsnprintk(char *buffer, size_t size, const char *format, va_list args)
{
	if (fake_vsnprintk_fail_count > 0) {
		fake_vsnprintk_fail_count--;
		return -ENOMEM;
	}
	return vsnprintf(buffer, size, format, args);
}

static void reset_fixture(void)
{
	assert(!fake_reboot_thread_created);
	fake_area.fa_size = 8U;
	fake_area_open_result = 0;
	fake_flash_init_result = 0;
	fake_write_result = 0;
	fake_check_result = 0;
	fake_header_result = 0;
	fake_schedule_result = 0;
	fake_upgrade_result = 0;
	fake_confirm_result = 0;
	fake_vsnprintk_fail_count = 0;
	fake_header_valid = true;
	fake_marker_present = false;
	fake_image_confirmed = true;
	fake_reboot_count = 0;
	fake_cancel_count = 0;
	fake_upgrade_call_count = 0;
	fake_confirm_call_count = 0;
	fake_auto_confirm_schedule_count = 0;
	fake_reboot_work = NULL;
	fake_auto_confirm_work = NULL;
	fake_reboot_start_on_schedule = false;
	fake_reboot_lock_attempted = false;
	fake_reboot_thread_finished = false;
	fake_reboot_hold_before_mutex = false;
	fake_reboot_allow_mutex = false;
	linkr_debugger_flash_arbiter_reset();
	linkr_debugger_ota_init();
}

static void init_client(struct http_client_ctx *client, const char *path,
			enum http_method method)
{
	memset(client, 0, sizeof(*client));
	assert(strlen(path) < sizeof(client->url_buffer));
	memcpy(client->url_buffer, path, strlen(path) + 1U);
	client->method = method;
}

static void call_ota(struct http_client_ctx *client, enum http_transaction_status status,
		     const struct http_header *headers, size_t header_count,
		     enum http_header_status headers_status, const uint8_t *data,
		     size_t data_len, struct http_response_ctx *response)
{
	struct http_request_ctx request = {
		.headers = headers,
		.header_count = header_count,
		.headers_status = headers_status,
		.data = data,
		.data_len = data_len,
	};

	memset(response, 0, sizeof(*response));
	assert(linkr_debugger_ota_http_handle(client, status, &request, response, NULL) == 0);
}

static void assert_owner_recoverable(void)
{
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_NONE);
	assert(linkr_debugger_flash_arbiter_try_acquire(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));
	assert(linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));
}

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

static void test_content_type_and_routes(void)
{
	assert(linkr_debugger_ota_content_type_is_octet_stream(LINKR_DEBUGGER_OTA_CONTENT_TYPE));
	assert(linkr_debugger_ota_content_type_is_octet_stream("APPLICATION/OCTET-STREAM"));
	assert(linkr_debugger_ota_content_type_is_octet_stream(" application/octet-stream \t"));
	assert(linkr_debugger_ota_content_type_is_octet_stream(" APPLICATION/OCTET-STREAM\t"));
	assert(!linkr_debugger_ota_content_type_is_octet_stream(NULL));
	assert(!linkr_debugger_ota_content_type_is_octet_stream("text/plain"));
	assert(!linkr_debugger_ota_content_type_is_octet_stream(
		"application/octet-stream; charset=utf-8"));
	assert(!linkr_debugger_ota_content_type_is_octet_stream("application/octet-streamx"));
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
	assert(!linkr_debugger_ota_path_is_handled("/api/v1/ota/upload/"));
	assert(linkr_debugger_ota_route_from_path("/api/v1/ota?verbose=1") ==
	       LINKR_DEBUGGER_OTA_ROUTE_NONE);
	assert(!linkr_debugger_ota_path_is_handled("/api/v1/ota/test?now=1"));
}

static void assert_begin_failure_releases(const struct http_header *headers,
				  size_t header_count,
				  enum http_header_status header_status,
				  enum http_status expected_status)
{
	struct http_client_ctx client;
	struct http_response_ctx response;
	const uint8_t data[4] = { 0 };

	init_client(&client, "/api/v1/ota/upload", HTTP_POST);
	call_ota(&client, HTTP_SERVER_REQUEST_DATA_FINAL, headers, header_count,
		 header_status, data, sizeof(data), &response);
	assert(response.status == expected_status);
	assert_owner_recoverable();
}

static void test_begin_failures_release_owner(void)
{
	struct http_header headers[3];

	memcpy(headers, good_headers, sizeof(headers));
	reset_fixture();
	assert_begin_failure_releases(headers, 3U, HTTP_HEADER_STATUS_DROPPED,
				      HTTP_400_BAD_REQUEST);

	reset_fixture();
	headers[0].value = "text/plain";
	assert_begin_failure_releases(headers, 3U, HTTP_HEADER_STATUS_OK,
				      HTTP_415_UNSUPPORTED_MEDIA_TYPE);

	reset_fixture();
	memcpy(headers, good_headers, sizeof(headers));
	headers[1].value = "bad";
	assert_begin_failure_releases(headers, 3U, HTTP_HEADER_STATUS_OK,
				      HTTP_400_BAD_REQUEST);

	reset_fixture();
	memcpy(headers, good_headers, sizeof(headers));
	headers[2].value = "00";
	assert_begin_failure_releases(headers, 3U, HTTP_HEADER_STATUS_OK,
				      HTTP_400_BAD_REQUEST);

	reset_fixture();
	fake_area_open_result = -EIO;
	assert_begin_failure_releases(good_headers, 3U, HTTP_HEADER_STATUS_OK,
				      HTTP_400_BAD_REQUEST);

	reset_fixture();
	fake_area.fa_size = 2U;
	assert_begin_failure_releases(good_headers, 3U, HTTP_HEADER_STATUS_OK,
				      HTTP_413_PAYLOAD_TOO_LARGE);

	reset_fixture();
	fake_flash_init_result = -EIO;
	assert_begin_failure_releases(good_headers, 3U, HTTP_HEADER_STATUS_OK,
				      HTTP_400_BAD_REQUEST);
}

static void test_config_owner_rejects_ota_without_unlocking(void)
{
	struct http_client_ctx client;
	struct http_response_ctx response;
	const uint8_t data[4] = { 0 };

	reset_fixture();
	assert(linkr_debugger_flash_arbiter_try_acquire(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));
	init_client(&client, "/api/v1/ota/upload", HTTP_POST);
	call_ota(&client, HTTP_SERVER_REQUEST_DATA_FINAL, good_headers, 3U,
		 HTTP_HEADER_STATUS_OK, data, sizeof(data), &response);
	assert(response.status == HTTP_409_CONFLICT);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_CONFIG);
	assert(linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));
}

static void run_final_upload(enum http_status expected_status, size_t data_len)
{
	struct http_client_ctx client;
	struct http_response_ctx response;
	const uint8_t data[5] = { 0 };

	init_client(&client, "/api/v1/ota/upload", HTTP_POST);
	call_ota(&client, HTTP_SERVER_REQUEST_DATA_FINAL, good_headers, 3U,
		 HTTP_HEADER_STATUS_OK, data, data_len, &response);
	assert(response.status == expected_status);
	assert_owner_recoverable();
}

static void test_write_and_verify_terminals_release_owner(void)
{
	reset_fixture();
	run_final_upload(HTTP_413_PAYLOAD_TOO_LARGE, 5U);

	reset_fixture();
	fake_write_result = -EIO;
	run_final_upload(HTTP_400_BAD_REQUEST, 4U);

	reset_fixture();
	run_final_upload(HTTP_400_BAD_REQUEST, 2U);

	reset_fixture();
	fake_check_result = -EIO;
	run_final_upload(HTTP_400_BAD_REQUEST, 4U);

	reset_fixture();
	fake_header_result = -EIO;
	run_final_upload(HTTP_400_BAD_REQUEST, 4U);

	reset_fixture();
	fake_header_valid = false;
	run_final_upload(HTTP_400_BAD_REQUEST, 4U);

	reset_fixture();
	run_final_upload(HTTP_200_OK, 4U);
}

static void start_partial_upload(struct http_client_ctx *client)
{
	struct http_response_ctx response;
	const uint8_t data = 0U;

	init_client(client, "/api/v1/ota/upload", HTTP_POST);
	call_ota(client, HTTP_SERVER_REQUEST_DATA_MORE, good_headers, 3U,
		 HTTP_HEADER_STATUS_OK, &data, 1U, &response);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_OTA);
}

static void test_abort_complete_and_reset_release_owner(void)
{
	struct http_client_ctx client;
	struct http_client_ctx other_client;
	struct http_response_ctx response;

	reset_fixture();
	start_partial_upload(&client);
	init_client(&other_client, "/api/v1/ota/upload", HTTP_POST);
	call_ota(&other_client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, 0U,
		 HTTP_HEADER_STATUS_OK, NULL, 0U, &response);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_OTA);
	call_ota(&client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, 0U,
		 HTTP_HEADER_STATUS_OK, NULL, 0U, &response);
	assert_owner_recoverable();

	reset_fixture();
	start_partial_upload(&client);
	call_ota(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, 0U,
		 HTTP_HEADER_STATUS_OK, NULL, 0U, &response);
	assert_owner_recoverable();

	reset_fixture();
	start_partial_upload(&client);
	linkr_debugger_ota_init();
	assert_owner_recoverable();
}

static void test_confirm_does_not_release_active_upload(void)
{
	struct http_client_ctx upload_client;
	struct http_client_ctx confirm_client;
	struct http_response_ctx response;

	reset_fixture();
	start_partial_upload(&upload_client);
	init_client(&confirm_client, "/api/v1/ota/confirm", HTTP_POST);
	call_ota(&confirm_client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U,
		 HTTP_HEADER_STATUS_OK, NULL, 0U, &response);
	assert(response.status == HTTP_409_CONFLICT);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_OTA);
	call_ota(&upload_client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, 0U,
		 HTTP_HEADER_STATUS_OK, NULL, 0U, &response);
	assert_owner_recoverable();
}

static void prepare_verified_upload(void)
{
	run_final_upload(HTTP_200_OK, 4U);
}

static void call_test_route(struct http_response_ctx *response)
{
	struct http_client_ctx client;

	init_client(&client, "/api/v1/ota/test", HTTP_POST);
	call_ota(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U,
		 HTTP_HEADER_STATUS_OK, NULL, 0U, response);
}

static void call_confirm_route(struct http_response_ctx *response)
{
	struct http_client_ctx client;

	init_client(&client, "/api/v1/ota/confirm", HTTP_POST);
	call_ota(&client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U,
		 HTTP_HEADER_STATUS_OK, NULL, 0U, response);
}

static void call_status_route(struct http_client_ctx *client,
			      struct http_response_ctx *response)
{
	init_client(client, "/api/v1/ota", HTTP_GET);
	call_ota(client, HTTP_SERVER_REQUEST_DATA_FINAL, NULL, 0U,
		 HTTP_HEADER_STATUS_OK, NULL, 0U, response);
}

static void assert_response_contains(const struct http_response_ctx *response,
				     const char *expected)
{
	size_t expected_len = strlen(expected);
	bool found = false;

	for (size_t i = 0U; i + expected_len <= response->body_len; i++) {
		if (memcmp(response->body + i, expected, expected_len) == 0) {
			found = true;
			break;
		}
	}
	if (!found) {
		fprintf(stderr, "expected OTA response to contain %s, got: %.*s\n",
			expected, (int)response->body_len, response->body);
	}
	assert(found);
}

static void test_status_exposes_ota_test_marker(void)
{
	struct http_client_ctx client;
	struct http_response_ctx response;

	reset_fixture();
	call_status_route(&client, &response);
	assert(response.status == HTTP_200_OK);
	assert_response_contains(&response, "\"test_marker_present\":false");

	fake_marker_present = true;
	fake_image_confirmed = false;
	call_status_route(&client, &response);
	assert(response.status == HTTP_200_OK);
	assert_response_contains(&response, "\"state\":\"pending_test\"");
	assert_response_contains(&response, "\"current_image_confirmed\":false");
	assert_response_contains(&response, "\"test_marker_present\":true");

	fake_marker_present = true;
	fake_image_confirmed = true;
	linkr_debugger_ota_init();
	call_status_route(&client, &response);
	assert(response.status == HTTP_200_OK);
	assert_response_contains(&response, "\"state\":\"idle\"");
	assert_response_contains(&response, "\"current_image_confirmed\":true");
	assert_response_contains(&response, "\"test_marker_present\":true");
}

static void run_reboot_work(void)
{
	assert(fake_reboot_work != NULL);
	fake_reboot_work->work.handler(&fake_reboot_work->work);
}

static void allow_scheduled_reboot_work_to_lock(void)
{
	assert(pthread_mutex_lock(&fake_reboot_sync_lock) == 0);
	fake_reboot_allow_mutex = true;
	assert(pthread_cond_broadcast(&fake_reboot_sync_cond) == 0);
	assert(pthread_mutex_unlock(&fake_reboot_sync_lock) == 0);
}

static void join_scheduled_reboot_work(void)
{
	assert(fake_reboot_thread_created);
	assert(pthread_join(fake_reboot_thread, NULL) == 0);
	fake_reboot_thread_created = false;
}

static void test_reboot_work_serializes_with_test_setup(void)
{
	struct http_response_ctx response;

	reset_fixture();
	prepare_verified_upload();
	fake_reboot_start_on_schedule = true;
	call_test_route(&response);
	assert(response.status == HTTP_202_ACCEPTED);
	assert(fake_reboot_lock_attempted);
	assert(fake_upgrade_call_count == 1);
	join_scheduled_reboot_work();
	assert(fake_reboot_count == 1);
	assert_owner_recoverable();

	reset_fixture();
	prepare_verified_upload();
	fake_upgrade_result = -EIO;
	fake_reboot_start_on_schedule = true;
	fake_reboot_hold_before_mutex = true;
	call_test_route(&response);
	assert(response.status == HTTP_500_INTERNAL_SERVER_ERROR);
	assert(fake_cancel_count == 1);
	assert(fake_reboot_lock_attempted);
	assert(linkr_debugger_flash_arbiter_try_acquire(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));
	allow_scheduled_reboot_work_to_lock();
	join_scheduled_reboot_work();
	assert(fake_reboot_count == 0);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_CONFIG);
	assert(linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));
}

static void test_test_and_reboot_handoff_do_not_leak(void)
{
	struct http_response_ctx response;

	reset_fixture();
	assert(linkr_debugger_flash_arbiter_try_acquire(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));
	run_reboot_work();
	assert(fake_reboot_count == 0);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_CONFIG);
	assert(linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));

	reset_fixture();
	prepare_verified_upload();
	call_test_route(&response);
	assert(response.status == HTTP_202_ACCEPTED);
	assert(fake_upgrade_call_count == 1);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_OTA);
	assert(!linkr_debugger_flash_arbiter_try_acquire(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));
	run_reboot_work();
	assert(fake_reboot_count == 1);
	assert_owner_recoverable();

	reset_fixture();
	prepare_verified_upload();
	assert(linkr_debugger_flash_arbiter_try_acquire(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));
	call_test_route(&response);
	assert(response.status == HTTP_409_CONFLICT);
	assert(fake_upgrade_call_count == 0);
	assert(fake_auto_confirm_schedule_count == 0);
	assert(!fake_marker_present);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_CONFIG);
	assert(linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));
	call_test_route(&response);
	assert(response.status == HTTP_202_ACCEPTED);
	run_reboot_work();
	assert_owner_recoverable();

	reset_fixture();
	prepare_verified_upload();
	fake_vsnprintk_fail_count = 1;
	call_test_route(&response);
	assert(response.status == HTTP_500_INTERNAL_SERVER_ERROR);
	assert(fake_upgrade_call_count == 0);
	assert_owner_recoverable();
	call_test_route(&response);
	assert(response.status == HTTP_202_ACCEPTED);
	run_reboot_work();

	reset_fixture();
	prepare_verified_upload();
	fake_schedule_result = -EIO;
	call_test_route(&response);
	assert(response.status == HTTP_500_INTERNAL_SERVER_ERROR);
	assert_owner_recoverable();
	fake_schedule_result = 0;
	call_test_route(&response);
	assert(response.status == HTTP_202_ACCEPTED);
	run_reboot_work();

	reset_fixture();
	prepare_verified_upload();
	fake_upgrade_result = -EIO;
	call_test_route(&response);
	assert(response.status == HTTP_500_INTERNAL_SERVER_ERROR);
	assert(fake_cancel_count == 1);
	assert_owner_recoverable();
	fake_upgrade_result = 0;
	call_test_route(&response);
	assert(response.status == HTTP_202_ACCEPTED);
	run_reboot_work();
}

static void test_manual_confirm_ownership(void)
{
	struct http_response_ctx response;

	reset_fixture();
	fake_marker_present = true;
	fake_image_confirmed = false;
	call_confirm_route(&response);
	assert(response.status == HTTP_200_OK);
	assert(fake_confirm_call_count == 1);
	assert(!fake_marker_present);
	assert_owner_recoverable();

	reset_fixture();
	fake_marker_present = true;
	fake_image_confirmed = false;
	fake_confirm_result = -EIO;
	call_confirm_route(&response);
	assert(response.status == HTTP_500_INTERNAL_SERVER_ERROR);
	assert(fake_confirm_call_count == 1);
	assert(fake_marker_present);
	assert_owner_recoverable();

	reset_fixture();
	assert(linkr_debugger_flash_arbiter_try_acquire(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));
	call_confirm_route(&response);
	assert(response.status == HTTP_409_CONFLICT);
	assert(fake_confirm_call_count == 0);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_CONFIG);
	assert(linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));

	reset_fixture();
	assert(linkr_debugger_flash_arbiter_try_acquire(LINKR_DEBUGGER_FLASH_OWNER_OTA));
	call_confirm_route(&response);
	assert(response.status == HTTP_409_CONFLICT);
	assert(fake_confirm_call_count == 0);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_OTA);
	assert(linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_OTA));
}

static void run_auto_confirm_work(void)
{
	assert(fake_auto_confirm_work != NULL);
	fake_auto_confirm_work->work.handler(&fake_auto_confirm_work->work);
}

static void test_auto_confirm_ownership_and_retry(void)
{
	struct http_client_ctx upload_client;
	struct http_response_ctx response;

	reset_fixture();
	fake_marker_present = true;
	fake_image_confirmed = false;
	run_auto_confirm_work();
	assert(fake_confirm_call_count == 1);
	assert(!fake_marker_present);
	assert_owner_recoverable();

	reset_fixture();
	fake_marker_present = true;
	fake_image_confirmed = false;
	fake_confirm_result = -EIO;
	run_auto_confirm_work();
	assert(fake_confirm_call_count == 1);
	assert(fake_marker_present);
	assert_owner_recoverable();

	reset_fixture();
	fake_marker_present = true;
	fake_image_confirmed = false;
	assert(linkr_debugger_flash_arbiter_try_acquire(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));
	run_auto_confirm_work();
	assert(fake_confirm_call_count == 0);
	assert(fake_auto_confirm_schedule_count == 1);
	assert(fake_marker_present);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_CONFIG);
	assert(linkr_debugger_flash_arbiter_release(LINKR_DEBUGGER_FLASH_OWNER_CONFIG));
	run_auto_confirm_work();
	assert(fake_confirm_call_count == 1);
	assert(!fake_marker_present);
	assert_owner_recoverable();

	reset_fixture();
	start_partial_upload(&upload_client);
	fake_marker_present = true;
	fake_image_confirmed = false;
	run_auto_confirm_work();
	assert(fake_confirm_call_count == 0);
	assert(fake_auto_confirm_schedule_count == 1);
	assert(fake_marker_present);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_OTA);
	call_ota(&upload_client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, 0U,
		 HTTP_HEADER_STATUS_OK, NULL, 0U, &response);
	assert_owner_recoverable();

	reset_fixture();
	prepare_verified_upload();
	call_test_route(&response);
	assert(response.status == HTTP_202_ACCEPTED);
	assert(fake_marker_present);
	run_auto_confirm_work();
	assert(fake_confirm_call_count == 0);
	assert(fake_auto_confirm_schedule_count == 1);
	assert(fake_marker_present);
	assert(linkr_debugger_flash_arbiter_owner() == LINKR_DEBUGGER_FLASH_OWNER_OTA);
	run_reboot_work();
	assert_owner_recoverable();
}

int main(void)
{
	test_size_header_parser();
	test_sha256_header_parser();
	test_content_type_and_routes();
	test_begin_failures_release_owner();
	test_config_owner_rejects_ota_without_unlocking();
	test_write_and_verify_terminals_release_owner();
	test_abort_complete_and_reset_release_owner();
	test_confirm_does_not_release_active_upload();
	test_reboot_work_serializes_with_test_setup();
	test_test_and_reboot_handoff_do_not_leak();
	test_manual_confirm_ownership();
	test_auto_confirm_ownership_and_retry();
	test_status_exposes_ota_test_marker();
	return 0;
}
