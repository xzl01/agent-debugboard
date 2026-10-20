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

static void test_gpio_layout_group_encoder_is_generic_and_bounded(void)
{
	static const struct linkr_debugger_gpio_layout_group_desc groups[] = {
		{ .name = "P\"\\\n\x01" "\xe9\x9b\xaa",
		  .edge = "right\t\r\b\f", .order = 7U },
	};
	static const struct linkr_debugger_gpio_layout_group_desc null_edge[] = {
		{ .name = "PORT", .edge = NULL, .order = 0U },
	};
	static const struct linkr_debugger_gpio_layout_group_desc invalid_utf8[] = {
		{ .name = "PORT", .edge = "\xf5\x80\x80\x80", .order = 0U },
	};
	static const char expected[] =
		"[{\"name\":\"P\\\"\\\\\\n\\u0001\xe9\x9b\xaa\","
		"\"edge\":\"right\\t\\r\\b\\f\",\"order\":7}]";
	char encoded[sizeof(expected) + 16U];
	char exact[sizeof(expected)];
	char short_buffer[sizeof(expected) - 1U];
	char empty[3];
	size_t cursor = 0U;
	size_t exact_cursor = 0U;
	size_t short_cursor = 0U;
	size_t empty_cursor = 0U;

	assert(linkr_debugger_ws_append_gpio_layout_groups_from(
		       encoded, sizeof(encoded), &cursor, groups,
		       sizeof(groups) / sizeof(groups[0])) == 0);
	assert(strcmp(encoded, expected) == 0);
	assert(cursor == strlen(encoded));
	assert(linkr_debugger_ws_append_gpio_layout_groups_from(
		       exact, sizeof(exact), &exact_cursor, groups,
		       sizeof(groups) / sizeof(groups[0])) == 0);
	assert(strcmp(exact, expected) == 0);
	assert(exact_cursor == sizeof(exact) - 1U);
	assert(linkr_debugger_ws_append_gpio_layout_groups_from(
		       short_buffer, sizeof(short_buffer), &short_cursor, groups,
		       sizeof(groups) / sizeof(groups[0])) == -ENOMEM);
	assert(short_cursor == sizeof(short_buffer) - 1U);
	assert(linkr_debugger_ws_append_gpio_layout_groups_from(
		       empty, sizeof(empty), &empty_cursor, NULL, 0U) == 0);
	assert(strcmp(empty, "[]") == 0);
	cursor = 0U;
	assert(linkr_debugger_ws_append_gpio_layout_groups_from(
		       encoded, sizeof(encoded), &cursor, NULL, 1U) == -EINVAL);
	assert(cursor == 0U);
	assert(linkr_debugger_ws_append_gpio_layout_groups_from(
		       encoded, sizeof(encoded), &cursor, null_edge,
		       sizeof(null_edge) / sizeof(null_edge[0])) == -EINVAL);
	assert(cursor == 0U);
	assert(linkr_debugger_ws_append_gpio_layout_groups_from(
		       encoded, sizeof(encoded), &cursor, invalid_utf8,
		       sizeof(invalid_utf8) / sizeof(invalid_utf8[0])) == -EINVAL);
	assert(cursor == 0U);
}

static void test_safe_gpio_layout_omits_unmapped_coordinates(void)
{
	static const struct linkr_debugger_safe_gpio_desc mapped = {
		.pin = 12U,
		.layout_group = "J8",
		.layout_label = "GP12",
		.layout_row = 9U,
		.layout_column = 0U,
	};
	static const struct linkr_debugger_safe_gpio_desc unmapped = {
		.pin = 17U,
		.layout_group = NULL,
		.layout_label = "GP17",
		.layout_row = UINT8_MAX,
		.layout_column = UINT8_MAX,
	};
	static const struct linkr_debugger_safe_gpio_desc empty_group = {
		.pin = 18U,
		.layout_group = "",
		.layout_label = "GP18",
		.layout_row = UINT8_MAX,
		.layout_column = UINT8_MAX,
	};
	static const struct linkr_debugger_safe_gpio_desc invalid = {
		.pin = 12U,
		.layout_group = "J8",
		.layout_label = NULL,
	};
	char encoded[128] = "gpio";
	size_t cursor = strlen(encoded);

	assert(linkr_debugger_ws_append_safe_gpio_layout_from(
		       encoded, sizeof(encoded), &cursor, &mapped) == 0);
	assert(strcmp(encoded,
		      "gpio,\"layoutGroup\":\"J8\",\"layoutLabel\":\"GP12\","
		      "\"layoutRow\":9,\"layoutColumn\":0") == 0);

	strcpy(encoded, "unchanged");
	cursor = strlen(encoded);
	assert(linkr_debugger_ws_append_safe_gpio_layout_from(
		       encoded, sizeof(encoded), &cursor, &unmapped) == 0);
	assert(strcmp(encoded, "unchanged") == 0);
	assert(cursor == strlen("unchanged"));
	assert(linkr_debugger_ws_append_safe_gpio_layout_from(
		       encoded, sizeof(encoded), &cursor, &empty_group) == 0);
	assert(strcmp(encoded, "unchanged") == 0);
	assert(linkr_debugger_ws_append_safe_gpio_layout_from(
		       encoded, sizeof(encoded), &cursor, &invalid) == -EINVAL);
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
	test_gpio_layout_group_encoder_is_generic_and_bounded();
	test_safe_gpio_layout_omits_unmapped_coordinates();
	return 0;
}
