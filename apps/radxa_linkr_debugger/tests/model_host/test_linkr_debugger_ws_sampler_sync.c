/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_ws_sampler_sync.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE 512U
struct linkr_debugger_ws_client {
	uint8_t tx_buffer[LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE];
};

static unsigned int reboot_schedule_calls;
static unsigned int reboot_accepted;
static unsigned int send_calls;
static int reboot_error;
static int send_result;
static int32_t send_timeout_ms;
static char sent_json[LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE];

static const char *linkr_debugger_json_schema(void)
{
	return "radxa-linkr-debugger.v1";
}

static int linkr_debugger_ws_send_json_timeout(struct linkr_debugger_ws_client *client,
					     const char *payload, int32_t timeout_ms)
{
	assert(reboot_schedule_calls == send_calls + 1U);
	assert(payload == (const char *)client->tx_buffer);
	assert(strlen(payload) < sizeof(sent_json));
	strcpy(sent_json, payload);
	send_calls++;
	send_timeout_ms = timeout_ms;
	return send_result;
}

static int linkr_debugger_ws_send_json(struct linkr_debugger_ws_client *client,
				     const char *payload)
{
	return linkr_debugger_ws_send_json_timeout(client, payload, -1);
}

#define snprintk snprintf
#define LINKR_DEBUGGER_WS_SAMPLER_POLICY_HOST_TEST 1
#include "linkr_debugger_ws.c"
#undef snprintk

int linkr_debugger_reboot_schedule(void)
{
	reboot_schedule_calls++;
	if (reboot_error != 0) {
		return reboot_error;
	}
	if (reboot_accepted != 0U) {
		return -EALREADY;
	}
	reboot_accepted++;
	return 0;
}

static void reset_reboot(void)
{
	reboot_schedule_calls = 0U;
	reboot_accepted = 0U;
	send_calls = 0U;
	reboot_error = 0;
	send_result = 0;
	send_timeout_ms = 0;
	sent_json[0] = '\0';
}

static void test_reboot_acknowledges_normal_reboot_then_pending(void)
{
	struct linkr_debugger_ws_client client = {0};

	reset_reboot();
	assert(LINKR_DEBUGGER_REBOOT_DELAY_MS == 250U);
	assert(linkr_debugger_ws_handle_reboot(&client, "r1") == 0);
	assert(strcmp(sent_json,
		"{\"type\":\"result\",\"schema\":\"radxa-linkr-debugger.v1\",\"command\":\"reboot\","
		"\"id\":\"r1\",\"ok\":true,\"status\":\"rebooting\",\"mode\":\"normal\","
		"\"reboot_delay_ms\":250,\"message\":\"normal cold reboot (not BOOTSEL)\"}") == 0);
	assert(send_timeout_ms > 0);
	assert(send_timeout_ms < (int32_t)LINKR_DEBUGGER_REBOOT_DELAY_MS);
	assert(reboot_schedule_calls == 1U);
	assert(reboot_accepted == 1U);
	assert(send_calls == 1U);

	assert(linkr_debugger_ws_handle_reboot(&client, "r2") == 0);
	assert(strstr(sent_json, "\"type\":\"error\"") != NULL);
	assert(strstr(sent_json, "\"command\":\"reboot\"") != NULL);
	assert(strstr(sent_json, "\"code\":\"reboot_pending\"") != NULL);
	assert(reboot_schedule_calls == 2U);
	assert(reboot_accepted == 1U);
	assert(send_calls == 2U);
}

static void test_reboot_reports_schedule_and_send_errors(void)
{
	struct linkr_debugger_ws_client client = {0};

	reset_reboot();
	reboot_error = -EIO;
	assert(linkr_debugger_ws_handle_reboot(&client, "r1") == 0);
	assert(strstr(sent_json, "\"type\":\"error\"") != NULL);
	assert(strstr(sent_json, "\"command\":\"reboot\"") != NULL);
	assert(strstr(sent_json, "\"code\":\"reboot_failed\"") != NULL);
	assert(reboot_accepted == 0U);
	assert(send_calls == 1U);

	reset_reboot();
	send_result = -EPIPE;
	assert(linkr_debugger_ws_handle_reboot(&client, NULL) == -EPIPE);
	assert(strstr(sent_json, "\"id\":\"\"") != NULL);
	assert(reboot_accepted == 1U);
	assert(send_calls == 1U);
}

static void test_old_resume_does_not_release_new_pause(void)
{
	struct linkr_debugger_ws_sampler_sync sync = {0};
	uint32_t first = linkr_debugger_ws_sampler_sync_request_pause(&sync);
	uint32_t second;

	assert(!linkr_debugger_ws_sampler_sync_is_resumed(&sync, first));
	linkr_debugger_ws_sampler_sync_resume_current(&sync);
	assert(linkr_debugger_ws_sampler_sync_is_resumed(&sync, first));

	second = linkr_debugger_ws_sampler_sync_request_pause(&sync);
	assert(linkr_debugger_ws_sampler_sync_is_resumed(&sync, first));
	assert(!linkr_debugger_ws_sampler_sync_is_resumed(&sync, second));

	linkr_debugger_ws_sampler_sync_resume_current(&sync);
	assert(linkr_debugger_ws_sampler_sync_is_resumed(&sync, second));
}

static void test_pause_generation_never_uses_zero(void)
{
	struct linkr_debugger_ws_sampler_sync sync = {
		.pause_generation = UINT32_MAX,
	};

	assert(linkr_debugger_ws_sampler_sync_request_pause(&sync) == 1U);
}

static void test_adc_sample_rate_budget(void)
{
	assert(!linkr_debugger_ws_adc_sample_rate_supported(0));
	assert(linkr_debugger_ws_adc_sample_rate_supported(1));
	assert(linkr_debugger_ws_adc_sample_rate_supported(250));
	assert(!linkr_debugger_ws_adc_sample_rate_supported(251));
	assert(!linkr_debugger_ws_adc_sample_rate_supported(1000));
}

static void test_sampler_deadline_excludes_read_time(void)
{
	int64_t deadline_us = 10000;

	assert(linkr_debugger_ws_sampler_advance_deadline(
		&deadline_us, 12000, 10000) == 0U);
	assert(deadline_us == 20000);
	assert(linkr_debugger_ws_sampler_delay_us(deadline_us, 12000) == 8000U);
}

static void test_sampler_deadline_skips_overrun_without_catch_up(void)
{
	int64_t deadline_us = 10000;

	assert(linkr_debugger_ws_sampler_advance_deadline(
		&deadline_us, 35000, 10000) == 2U);
	assert(deadline_us == 40000);
	assert(linkr_debugger_ws_sampler_delay_us(deadline_us, 35000) == 5000U);

	assert(linkr_debugger_ws_sampler_advance_deadline(
		&deadline_us, 50000, 10000) == 1U);
	assert(deadline_us == 60000);
	assert(linkr_debugger_ws_sampler_delay_us(deadline_us, 50000) == 10000U);
}

int main(void)
{
	test_old_resume_does_not_release_new_pause();
	test_pause_generation_never_uses_zero();
	test_adc_sample_rate_budget();
	test_sampler_deadline_excludes_read_time();
	test_sampler_deadline_skips_overrun_without_catch_up();
	test_reboot_acknowledges_normal_reboot_then_pending();
	test_reboot_reports_schedule_and_send_errors();
	return 0;
}
