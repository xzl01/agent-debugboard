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
#include <stdlib.h>
#include <string.h>

#include "linkr_debugger_model.h"
#include "linkr_debugger_gpio_error.h"

/* Only the control/RTOS dependencies are stubbed: the WS sample, workspace,
 * ring store/get and both encoders below are included from production. */
struct sensor_value {
	int32_t val1;
	int32_t val2;
};

struct linkr_debugger_current_sample {
	bool rail_enabled;
	bool raw_available;
	bool voltage_valid;
	int32_t raw;
	int32_t mv;
	int32_t voltage_uv;
	int32_t current_ua;
	struct sensor_value value;
};

#define LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE 6144U
#define BUILD_ASSERT(condition, ...) _Static_assert(condition, #condition)
#define DIV_ROUND_UP(value, divisor) (((value) + (divisor) - 1U) / (divisor))
#define BIT(index) (1U << (index))
#define K_FOREVER (-1)

struct k_mutex {
	bool locked;
};

static void k_mutex_lock(struct k_mutex *lock, int timeout)
{
	assert(timeout == K_FOREVER);
	assert(!lock->locked);
	lock->locked = true;
}

static void k_mutex_unlock(struct k_mutex *lock)
{
	assert(lock->locked);
	lock->locked = false;
}

static struct k_mutex linkr_debugger_ws_sample_ring_lock;
static bool linkr_debugger_ws_sampler_pause_requested;
static bool linkr_debugger_ws_arena_quiesced;
static uint64_t linkr_debugger_ws_latest_sample_sequence;
static struct linkr_debugger_ws_sampler_workspace linkr_debugger_ws_sampler_workspace;
struct linkr_debugger_ws_adc_sample;
static void linkr_debugger_capture_ingest_sample(
	const struct linkr_debugger_ws_adc_sample *sample);

struct linkr_debugger_ws_client {
	uint8_t tx_buffer[LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE];
	int telemetry_rate_hz;
	uint8_t telemetry_batch_size;
	uint64_t next_sample_sequence;
	uint64_t telemetry_dropped_samples;
	int64_t next_sample_due_us;
};

static unsigned int reboot_schedule_calls;
static unsigned int reboot_accepted;
static unsigned int send_calls;
static int reboot_error;
static int send_result;
static int32_t send_timeout_ms;
static char sent_json[LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE];
static bool expect_reboot_send;

static const char *linkr_debugger_json_schema(void)
{
	return "radxa-linkr-debugger.v1";
}

static int linkr_debugger_ws_send_json_timeout(struct linkr_debugger_ws_client *client,
					     const char *payload, int32_t timeout_ms)
{
	if (expect_reboot_send) {
		assert(reboot_schedule_calls == send_calls + 1U);
	}
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
#define vsnprintk vsnprintf
#define LINKR_DEBUGGER_WS_SAMPLER_POLICY_HOST_TEST 1
#include "linkr_debugger_ws.c"
#undef snprintk
#undef vsnprintk

const struct linkr_debugger_current_desc linkr_debugger_currents[] = {
	{ .name = "5v_out", .signal = "INA226_5V",
	  .kind = LINKR_DEBUGGER_ADC_KIND_CURRENT,
	  .role = LINKR_DEBUGGER_ADC_ROLE_NATIVE, .unit = "uA" },
	{ .name = "12v_out", .signal = "INA226_12V",
	  .kind = LINKR_DEBUGGER_ADC_KIND_CURRENT,
	  .role = LINKR_DEBUGGER_ADC_ROLE_NATIVE, .unit = "uA" },
	{ .name = "20v_out", .signal = "INA226_20V",
	  .kind = LINKR_DEBUGGER_ADC_KIND_CURRENT,
	  .role = LINKR_DEBUGGER_ADC_ROLE_NATIVE, .unit = "uA" },
	{ .name = "vdd_5v", .signal = "INA226_VDD_5V",
	  .kind = LINKR_DEBUGGER_ADC_KIND_CURRENT,
	  .role = LINKR_DEBUGGER_ADC_ROLE_NATIVE, .unit = "uA" },
	{ .name = "adc0", .signal = "GPIO26_ADC0",
	  .note = "PD input divider; ADC pad voltage",
	  .kind = LINKR_DEBUGGER_ADC_KIND_VOLTAGE,
	  .role = LINKR_DEBUGGER_ADC_ROLE_PD_INPUT, .unit = "uV" },
	{ .name = "adc1", .signal = "GPIO27_ADC1",
	  .kind = LINKR_DEBUGGER_ADC_KIND_VOLTAGE,
	  .role = LINKR_DEBUGGER_ADC_ROLE_NATIVE, .unit = "uV" },
	{ .name = "adc2", .signal = "GPIO28_ADC2",
	  .kind = LINKR_DEBUGGER_ADC_KIND_VOLTAGE,
	  .role = LINKR_DEBUGGER_ADC_ROLE_NATIVE, .unit = "uV" },
	{ .name = "adc3", .signal = "GPIO29_ADC3",
	  .kind = LINKR_DEBUGGER_ADC_KIND_VOLTAGE,
	  .role = LINKR_DEBUGGER_ADC_ROLE_NATIVE, .unit = "uV" },
};
const size_t linkr_debugger_adc_count = 8U;
const size_t linkr_debugger_current_count = 4U;

static struct linkr_debugger_ws_adc_sample sample_ring[LINKR_DEBUGGER_WS_SAMPLE_RING_SIZE];
static struct linkr_debugger_ws_adc_sample last_ingested_sample;
static unsigned int ingest_calls;

void *linkr_debugger_capture_arena_ws_sample_ring(void)
{
	return sample_ring;
}

bool linkr_debugger_capture_arena_ws_sample_read_allowed(bool paused, bool quiesced)
{
	return !paused && !quiesced;
}

static void linkr_debugger_capture_ingest_sample(
	const struct linkr_debugger_ws_adc_sample *sample)
{
	assert(!linkr_debugger_ws_sample_ring_lock.locked);
	last_ingested_sample = *sample;
	ingest_calls++;
}

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
	expect_reboot_send = true;
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

static struct linkr_debugger_ws_client reset_adc(void)
{
	reset_reboot();
	expect_reboot_send = false;
	memset(sample_ring, 0, sizeof(sample_ring));
	memset(&linkr_debugger_ws_sampler_workspace, 0,
	       sizeof(linkr_debugger_ws_sampler_workspace));
	linkr_debugger_ws_latest_sample_sequence = 0U;
	linkr_debugger_ws_sampler_pause_requested = false;
	linkr_debugger_ws_arena_quiesced = false;
	ingest_calls = 0U;
	return (struct linkr_debugger_ws_client){
		.telemetry_rate_hz = 250,
		.telemetry_batch_size = 1U,
		.next_sample_sequence = 1U,
	};
}

static void store_sample(const struct linkr_debugger_current_sample readings[8],
			 int64_t timestamp_us, uint32_t source_drops)
{
	struct linkr_debugger_current_sample row[1][8];

	memcpy(row[0], readings, sizeof(row[0]));
	linkr_debugger_ws_store_adc_samples(row, &timestamp_us, 1U, 250, source_drops);
	assert(memcmp(last_ingested_sample.readings, readings, sizeof(row[0])) == 0);
	assert(last_ingested_sample.uptime_us == timestamp_us);
	assert(last_ingested_sample.source_dropped_before == source_drops);
}

static void expect_text(struct linkr_debugger_json_cursor *json, const char *text)
{
	size_t len = strlen(text);

	assert(strncmp(json->text + json->offset, text, len) == 0);
	json->offset += len;
}

static void expect_string(struct linkr_debugger_json_cursor *json, const char *value)
{
	char decoded[128];

	assert(linkr_debugger_json_parse_string(json, decoded, sizeof(decoded)));
	assert(strcmp(decoded, value) == 0);
}

static void expect_signed(struct linkr_debugger_json_cursor *json, int64_t value)
{
	const char *start = json->text + json->offset;
	char *end;

	errno = 0;
	assert(strtoll(start, &end, 10) == value);
	assert(errno == 0 && end > start);
	json->offset += (size_t)(end - start);
}

static void expect_unsigned(struct linkr_debugger_json_cursor *json, uint64_t value)
{
	const char *start = json->text + json->offset;
	char *end;

	errno = 0;
	assert(strtoull(start, &end, 10) == value);
	assert(errno == 0 && end > start);
	json->offset += (size_t)(end - start);
}

static void expect_channel(struct linkr_debugger_json_cursor *json, size_t index)
{
	const struct linkr_debugger_current_desc *desc = &linkr_debugger_currents[index];

	expect_text(json, "{\"name\":");
	expect_string(json, desc->name);
	expect_text(json, ",\"signal\":");
	expect_string(json, desc->signal);
	expect_text(json, ",\"kind\":");
	expect_string(json, index < 4U ? "current" : "voltage");
	expect_text(json, ",\"unit\":");
	expect_string(json, index < 4U ? "uA" : "uV");
	expect_text(json, ",\"role\":");
	expect_string(json, index == 4U ? "pd_input" : "native");
	if (index == 4U) {
		expect_text(json, ",\"note\":");
		expect_string(json, "PD input divider; ADC pad voltage");
	}
}

static void test_eight_channel_single_encoder(void)
{
	static const int32_t expected_values[8] = {
		-1234, -1235, -1236, -1237, 20000002, -2002350, -3002351, -4002352,
	};
	struct linkr_debugger_ws_client client = reset_adc();
	struct linkr_debugger_current_sample readings[8] = {0};
	struct linkr_debugger_json_cursor json = { .text = sent_json };

	assert(linkr_debugger_ws_emit_adc_sample(&client) == 0);
	assert(send_calls == 0U);
	for (size_t i = 0U; i < 8U; i++) {
		readings[i].rail_enabled = i >= 4U || i % 2U == 0U;
		readings[i].voltage_valid = i >= 4U || i % 2U != 0U;
		readings[i].current_ua = -1234 - (int32_t)i;
		readings[i].voltage_uv = 5000000 + (int32_t)i;
		if (i >= 4U) {
			readings[i].value.val1 = -(int32_t)(i - 3U);
			readings[i].value.val2 = -2345 - (int32_t)i;
		}
	}
	readings[4].value.val1 = 20;
	readings[4].value.val2 = 2;
	store_sample(readings, 123456789, 7U);
	assert(linkr_debugger_ws_emit_adc_sample(&client) == 0);
	assert(send_calls == 1U);
	assert(client.telemetry_dropped_samples == 7U);
	expect_text(&json, "{\"type\":\"telemetry\",\"topic\":\"adc\","
		    "\"schema\":\"radxa-linkr-debugger.v1\",\"sequence\":1,"
		    "\"uptime_us\":123456789,\"sample_sequence\":1,"
		    "\"device_t_mono_us\":123456789,\"dropped_samples\":7,\"readings\":[");
	for (size_t i = 0U; i < 8U; i++) {
		if (i > 0U) {
			expect_text(&json, ",");
		}
		expect_channel(&json, i);
		if (i < 4U) {
			expect_text(&json, ",\"power_enabled\":");
			expect_text(&json, i % 2U == 0U ? "true" : "false");
			expect_text(&json, ",\"current_ua\":");
			expect_signed(&json, expected_values[i]);
			expect_text(&json, ",\"voltage_valid\":");
			expect_text(&json, i % 2U != 0U ? "true" : "false");
			expect_text(&json, ",\"voltage_uv\":");
			expect_signed(&json, 5000000 + (int32_t)i);
		}
		/* Voltage objects must go straight to value, without rail fields. */
		expect_text(&json, ",\"value\":");
		expect_signed(&json, expected_values[i]);
		expect_text(&json, "}");
	}
	expect_text(&json, "]}");
	assert(json.offset == strlen(sent_json));
	assert(linkr_debugger_ws_emit_adc_sample(&client) == 0);
	assert(send_calls == 1U);

	store_sample(readings, 123460789, 0U);
	assert(linkr_debugger_ws_emit_adc_sample(&client) == 0);
	assert(strstr(sent_json, "\"dropped_samples\"") == NULL);
	assert(client.telemetry_dropped_samples == 7U);
}

static void expect_batch(size_t count, uint64_t first_sequence, int64_t timestamp,
			 uint32_t drops, unsigned int power_mask, unsigned int valid_mask,
			 const int32_t values[8], const int32_t voltages[4])
{
	struct linkr_debugger_json_cursor json = { .text = sent_json };

	expect_text(&json, "{\"type\":\"telemetry-batch\",\"topic\":\"adc\","
		    "\"schema\":\"radxa-linkr-debugger.v1\",\"dropped_samples\":");
	expect_unsigned(&json, drops);
	expect_text(&json, ",\"channels\":[");
	for (size_t i = 0U; i < 8U; i++) {
		if (i > 0U) {
			expect_text(&json, ",");
		}
		expect_channel(&json, i);
		expect_text(&json, "}");
	}
	expect_text(&json, "],\"samples\":[");
	for (size_t i = 0U; i < count; i++) {
		if (i > 0U) {
			expect_text(&json, ",");
		}
		expect_text(&json, "{\"sequence\":");
		expect_unsigned(&json, first_sequence + i);
		expect_text(&json, ",\"uptime_us\":");
		expect_signed(&json, timestamp);
		expect_text(&json, ",\"power_enabled_mask\":");
		expect_unsigned(&json, power_mask);
		expect_text(&json, ",\"voltage_valid_mask\":");
		expect_unsigned(&json, valid_mask);
		expect_text(&json, ",\"values\":[");
		for (size_t j = 0U; j < 8U; j++) {
			if (j > 0U) {
				expect_text(&json, ",");
			}
			expect_signed(&json, values[j]);
		}
		expect_text(&json, "],\"voltages_uv\":[");
		for (size_t j = 0U; j < 4U; j++) {
			if (j > 0U) {
				expect_text(&json, ",");
			}
			expect_signed(&json, voltages[j]);
		}
		expect_text(&json, "]}");
	}
	expect_text(&json, "]}");
	assert(json.offset == strlen(sent_json));
}

static void test_eight_channel_batch_encoder_masks_and_source_drops(void)
{
	static const int32_t values[8] = { -1, 2, -3, 4, 20000002, 1000006, -7, 8 };
	static const int32_t voltages[4] = { -9, 10, -11, 12 };
	struct linkr_debugger_ws_client client = reset_adc();
	struct linkr_debugger_current_sample readings[8] = {0};

	client.telemetry_batch_size = 2U;
	assert(linkr_debugger_ws_emit_adc_batch(&client) == 0);
	assert(send_calls == 0U);
	for (size_t i = 0U; i < 8U; i++) {
		readings[i].rail_enabled = i >= 4U || i % 2U == 0U;
		readings[i].voltage_valid = i >= 4U || i % 2U != 0U;
		readings[i].current_ua = values[i];
		readings[i].value.val1 = values[i] / 1000000;
		readings[i].value.val2 = values[i] % 1000000;
		readings[i].voltage_uv = i < 4U ? voltages[i] : INT32_MAX;
	}
	store_sample(readings, 42, 7U);
	store_sample(readings, 42, 11U);
	assert(linkr_debugger_ws_emit_adc_batch(&client) == 0);
	assert(send_calls == 1U);
	assert(client.telemetry_dropped_samples == 18U);
	expect_batch(2U, 1U, 42, 18U, 5U, 10U, values, voltages);
	assert(linkr_debugger_ws_emit_adc_batch(&client) == 0);
	assert(send_calls == 1U);
}

static void test_eight_channel_batch_actual_frame_budget(void)
{
	static const int32_t values[8] = {
		INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN,
		INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN,
	};
	static const int32_t voltages[4] = { INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN };
	struct linkr_debugger_ws_client client = reset_adc();
	struct linkr_debugger_current_sample readings[8] = {0};
	const uint64_t first_sequence = UINT64_MAX - 1000U;

	assert(LINKR_DEBUGGER_WS_ADC_MAX_BATCH_SIZE == 18U);
	assert(LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE == 6144U);
	assert(LINKR_DEBUGGER_WS_ADC_FRAME_BUFFER_SIZE == sizeof(client.tx_buffer));
	for (size_t i = 0U; i < 8U; i++) {
		readings[i].rail_enabled = true;
		readings[i].voltage_valid = true;
		readings[i].current_ua = INT32_MIN;
		readings[i].voltage_uv = INT32_MIN;
		readings[i].value.val1 = INT32_MIN / 1000000;
		readings[i].value.val2 = INT32_MIN % 1000000;
	}
	linkr_debugger_ws_latest_sample_sequence = first_sequence - 1U;
	client.next_sample_sequence = first_sequence;
	for (size_t i = 0U; i < 19U; i++) {
		store_sample(readings, INT64_MIN, i == 0U ? UINT32_MAX : 0U);
	}
	/* The actual production entry rejects 19 before consuming any sample. */
	client.telemetry_batch_size = 19U;
	assert(linkr_debugger_ws_emit_adc_batch(&client) == -ENOMEM);
	assert(send_calls == 0U);
	assert(client.next_sample_sequence == first_sequence);
	assert(client.telemetry_dropped_samples == 0U);
	client.telemetry_batch_size = 18U;
	assert(linkr_debugger_ws_emit_adc_batch(&client) == 0);
	assert(send_calls == 1U);
	assert(strlen(sent_json) == 6002U);
	assert(strlen(sent_json) < sizeof(client.tx_buffer));
	assert(strlen(sent_json) == linkr_debugger_ws_adc_batch_worst_case_size_from(
		linkr_debugger_currents, 8U, 4U, 18U, linkr_debugger_json_schema()));
	assert(client.next_sample_sequence == first_sequence + 18U);
	assert(client.telemetry_dropped_samples == UINT32_MAX);
	expect_batch(18U, first_sequence, INT64_MIN, UINT32_MAX, 15U, 15U,
		     values, voltages);
	printf("WS ADC production encoder: max 18-sample frame = %zu bytes / %zu; "
	       "19 samples rejected before consumption\n", strlen(sent_json),
	       sizeof(client.tx_buffer));
	assert(linkr_debugger_ws_emit_adc_batch(&client) == 0);
	expect_batch(1U, first_sequence + 18U, INT64_MIN, 0U, 15U, 15U,
		     values, voltages);
}

static void test_adc_ring_wrap_oldest_and_complete_samples(void)
{
	struct linkr_debugger_ws_client client = reset_adc();
	struct linkr_debugger_current_sample readings[8] = {0};
	struct linkr_debugger_ws_adc_sample sample;
	uint32_t dropped;

	assert(LINKR_DEBUGGER_WS_SAMPLE_RING_SIZE == 173U);
	assert(sizeof(sample) == 248U);
	assert(sizeof(sample_ring) == 42904U);
	assert(LINKR_DEBUGGER_CAPTURE_ARENA_WS_SAMPLE_RING_BYTES == 43008U);
	assert(sizeof(linkr_debugger_ws_sampler_workspace) == 480U);
	for (unsigned int sequence = 1U; sequence <= 353U; sequence++) {
		memset(readings, 0, sizeof(readings));
		for (size_t i = 0U; i < 8U; i++) {
			readings[i].current_ua = (int32_t)(sequence * 100U + i);
			readings[i].value.val2 = -(int32_t)(sequence * 100U + i);
		}
		store_sample(readings, (int64_t)sequence * 4000, sequence == 181U ? 9U : 0U);
		memset(readings, 0xa5, sizeof(readings));
		if (sequence == 173U || sequence == 174U) {
			client.next_sample_sequence = 1U;
			assert(linkr_debugger_ws_sample_get(&client, &sample, &dropped));
			assert(sample.sequence == (sequence == 173U ? 1U : 2U));
			assert(dropped == (sequence == 173U ? 0U : 1U));
		}
	}
	assert(ingest_calls == 353U);
	assert(linkr_debugger_ws_latest_sample_sequence == 353U);
	client.next_sample_sequence = 1U;
	for (uint64_t sequence = 181U; sequence <= 353U; sequence++) {
		assert(linkr_debugger_ws_sample_get(&client, &sample, &dropped));
		assert(sample.sequence == sequence);
		assert(sample.uptime_us == (int64_t)sequence * 4000);
		assert(sample.source_rate_hz == 250);
		assert(dropped == (sequence == 181U ? 189U : 0U));
		for (size_t i = 0U; i < 8U; i++) {
			assert(sample.readings[i].current_ua == (int32_t)(sequence * 100U + i));
			assert(sample.readings[i].value.val2 == -(int32_t)(sequence * 100U + i));
		}
	}
	assert(client.next_sample_sequence == 354U);
	assert(!linkr_debugger_ws_sample_get(&client, &sample, &dropped));
	assert(dropped == 0U);
}

static void test_adc_json_writer_preserves_canonical_escaping_and_utf8(void)
{
	static const struct linkr_debugger_current_desc desc = {
		.note = "P\"\\\n\t\r\b\f\x01" "\xe9\x9b\xaa",
	};
	static const char expected[] =
		",\"note\":\"P\\\"\\\\\\n\\t\\r\\b\\f\\u0001\xe9\x9b\xaa\"";
	char encoded[sizeof(expected)];
	char short_buffer[sizeof(expected) - 1U];
	size_t cursor = 0U;

	assert(linkr_debugger_ws_append_adc_note(encoded, sizeof(encoded), &cursor, &desc) == 0);
	assert(cursor == sizeof(expected) - 1U);
	assert(strcmp(encoded, expected) == 0);
	assert(linkr_debugger_json_utf8_valid(encoded, cursor));
	cursor = 0U;
	assert(linkr_debugger_ws_append_adc_note(
		short_buffer, sizeof(short_buffer), &cursor, &desc) == -ENOMEM);
	assert(cursor < sizeof(short_buffer));
	assert(short_buffer[cursor] == '\0');
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

static void test_serial_live_and_sigrok_modes_are_mutually_exclusive(void)
{
	enum linkr_debugger_ws_stream_mode next;

	assert(linkr_debugger_ws_subscribe_mode(
		       LINKR_DEBUGGER_WS_MODE_IDLE, "serial.j4", &next) == 0);
	assert(next == LINKR_DEBUGGER_WS_MODE_SERIAL_J4);
	assert(linkr_debugger_ws_subscribe_mode(
		       LINKR_DEBUGGER_WS_MODE_SERIAL_J4, "live", &next) ==
	       -EBUSY);
	assert(linkr_debugger_ws_subscribe_mode(
		       LINKR_DEBUGGER_WS_MODE_LIVE_ADC, "serial.j4", &next) ==
	       -EBUSY);
	assert(linkr_debugger_ws_subscribe_mode(
		       LINKR_DEBUGGER_WS_MODE_SIGROK, "serial.j4", &next) ==
	       -EBUSY);
	assert(linkr_debugger_ws_subscribe_mode(
		       LINKR_DEBUGGER_WS_MODE_IDLE, "unknown", &next) == -EINVAL);
	assert(linkr_debugger_ws_binary_mode(
		       LINKR_DEBUGGER_WS_MODE_IDLE, &next) == 0);
	assert(next == LINKR_DEBUGGER_WS_MODE_SIGROK);
	assert(linkr_debugger_ws_binary_mode(
		       LINKR_DEBUGGER_WS_MODE_SERIAL_J4, &next) == 0);
	assert(next == LINKR_DEBUGGER_WS_MODE_SERIAL_J4);
	assert(linkr_debugger_ws_binary_mode(
		       LINKR_DEBUGGER_WS_MODE_LIVE_ADC, &next) == -EBUSY);
}

static int test_ws_rx_feed(struct linkr_debugger_ws_rx_assembly *assembly,
			   uint8_t scratch[512], const uint8_t *data,
			   size_t length, bool metadata_valid,
			   uint32_t frame_flags, uint64_t remaining,
			   const uint8_t **message, size_t *message_length)
{
	return linkr_debugger_ws_rx_assembly_feed(
		assembly, scratch, 512U, data, length, metadata_valid,
		frame_flags, remaining, message, message_length);
}

static void test_ws_serial_frame_assembly_waits_for_complete_final_message(void)
{
	struct linkr_debugger_ws_rx_assembly assembly = { 0 };
	uint8_t scratch[512] = { 0 };
	uint8_t first[200];
	uint8_t second[280];
	const uint8_t *message;
	size_t message_length;

	for (size_t i = 0U; i < sizeof(first); i++) {
		first[i] = (uint8_t)i;
	}
	for (size_t i = 0U; i < sizeof(second); i++) {
		second[i] = (uint8_t)(i + sizeof(first));
	}
	first[0] = 0x00U;
	first[1] = 0xffU;
	second[sizeof(second) - 2U] = 0x00U;
	second[sizeof(second) - 1U] = 0xffU;

	/* Pinned Zephyr may return bytes while leaving output metadata untouched
	 * after a nonblocking short read. No message is dispatchable yet.
	 */
	assert(test_ws_rx_feed(
		       &assembly, scratch, first, sizeof(first), false, 0U, UINT64_MAX,
		       &message, &message_length) == LINKR_DEBUGGER_WS_RX_MORE);
	assert(message == NULL && message_length == 0U);
	assert(test_ws_rx_feed(
		       &assembly, scratch, second, sizeof(second), true,
		       LINKR_DEBUGGER_WS_FRAME_BINARY |
			       LINKR_DEBUGGER_WS_FRAME_FINAL,
		       0U, &message, &message_length) ==
	       LINKR_DEBUGGER_WS_RX_BINARY);
	assert(message_length == LINKR_DEBUGGER_WS_MESSAGE_MAX);
	assert(memcmp(message, first, sizeof(first)) == 0);
	assert(memcmp(message + sizeof(first), second, sizeof(second)) == 0);
}

static void test_ws_frame_assembly_tracks_type_fin_and_continuations(void)
{
	struct linkr_debugger_ws_rx_assembly assembly = { 0 };
	uint8_t scratch[512] = { 0 };
	static const uint8_t text[] = { '{', '}' };
	static const uint8_t binary[] = { 0x00U, 0xffU };
	static const uint8_t continuation[] = { 0x7eU, 0x00U, 0xffU };
	uint8_t partial[480] = { 0 };
	const uint8_t *message;
	size_t message_length;

	assert(test_ws_rx_feed(
		       &assembly, scratch, text, sizeof(text), true,
		       LINKR_DEBUGGER_WS_FRAME_TEXT |
			       LINKR_DEBUGGER_WS_FRAME_FINAL,
		       0U, &message, &message_length) ==
	       LINKR_DEBUGGER_WS_RX_TEXT);
	assert(message_length == sizeof(text));
	assert(test_ws_rx_feed(
		       &assembly, scratch, binary, sizeof(binary), true,
		       LINKR_DEBUGGER_WS_FRAME_BINARY |
			       LINKR_DEBUGGER_WS_FRAME_FINAL,
		       0U, &message, &message_length) ==
	       LINKR_DEBUGGER_WS_RX_BINARY);
	assert(message_length == sizeof(binary));
	assert(memcmp(message, binary, sizeof(binary)) == 0);

	/* A non-FIN BINARY frame is not a message. A FIN CONTINUE frame closes
	 * it while retaining the original BINARY type.
	 */
	assert(test_ws_rx_feed(
		       &assembly, scratch, binary, sizeof(binary), true,
		       LINKR_DEBUGGER_WS_FRAME_BINARY, 0U,
		       &message, &message_length) == LINKR_DEBUGGER_WS_RX_MORE);
	assert(test_ws_rx_feed(
		       &assembly, scratch, continuation, sizeof(continuation), true,
		       LINKR_DEBUGGER_WS_FRAME_FINAL, 0U,
		       &message, &message_length) == LINKR_DEBUGGER_WS_RX_BINARY);
	assert(message_length == sizeof(binary) + sizeof(continuation));
	assert(memcmp(message, binary, sizeof(binary)) == 0);
	assert(memcmp(message + sizeof(binary), continuation,
		      sizeof(continuation)) == 0);

	/* Once metadata reports 280 remaining bytes, delayed chunks must account
	 * for exactly those bytes before the final message is released.
	 */
	assert(test_ws_rx_feed(
		       &assembly, scratch, partial, 200U, true,
		       LINKR_DEBUGGER_WS_FRAME_BINARY |
			       LINKR_DEBUGGER_WS_FRAME_FINAL,
		       280U, &message, &message_length) ==
	       LINKR_DEBUGGER_WS_RX_MORE);
	assert(test_ws_rx_feed(
		       &assembly, scratch, partial + 200U, 100U, false, 0U, UINT64_MAX,
		       &message, &message_length) == LINKR_DEBUGGER_WS_RX_MORE);
	assert(test_ws_rx_feed(
		       &assembly, scratch, partial + 300U, 180U, true,
		       LINKR_DEBUGGER_WS_FRAME_BINARY |
			       LINKR_DEBUGGER_WS_FRAME_FINAL,
		       0U, &message, &message_length) ==
	       LINKR_DEBUGGER_WS_RX_BINARY);
	assert(message_length == sizeof(partial));
}

static void test_ws_frame_assembly_drains_oversize_before_reset(void)
{
	struct linkr_debugger_ws_rx_assembly assembly = { 0 };
	uint8_t scratch[512] = { 0 };
	uint8_t chunk[300] = { 0 };
	static const uint8_t legal[] = { 0x00U, 0xffU };
	const uint8_t *message;
	size_t message_length;

	assert(test_ws_rx_feed(
		       &assembly, scratch, chunk, sizeof(chunk), false, 0U, UINT64_MAX,
		       &message, &message_length) == LINKR_DEBUGGER_WS_RX_MORE);
	assert(test_ws_rx_feed(
		       &assembly, scratch, chunk, 200U, true,
		       LINKR_DEBUGGER_WS_FRAME_BINARY |
			       LINKR_DEBUGGER_WS_FRAME_FINAL,
		       0U, &message, &message_length) == -EMSGSIZE);
	assert(test_ws_rx_feed(
		       &assembly, scratch, legal, sizeof(legal), true,
		       LINKR_DEBUGGER_WS_FRAME_BINARY |
			       LINKR_DEBUGGER_WS_FRAME_FINAL,
		       0U, &message, &message_length) ==
	       LINKR_DEBUGGER_WS_RX_BINARY);
	assert(message_length == sizeof(legal));
	assert(memcmp(message, legal, sizeof(legal)) == 0);
}

struct test_ws_pong_capture {
	uint8_t payload[LINKR_DEBUGGER_WS_CONTROL_PAYLOAD_MAX];
	size_t length;
	unsigned int calls;
	int result;
};

static int test_ws_send_pong(void *context, const uint8_t *payload,
			     size_t length)
{
	struct test_ws_pong_capture *capture = context;

	assert(length <= sizeof(capture->payload));
	memcpy(capture->payload, payload, length);
	capture->length = length;
	capture->calls++;
	return capture->result;
}

static int test_ws_rx_client_step(
	struct linkr_debugger_ws_rx_assembly *assembly, uint8_t scratch[512],
	const uint8_t *data, size_t length, bool metadata_valid,
	uint32_t frame_flags, uint64_t remaining,
	struct test_ws_pong_capture *pong, const uint8_t **message,
	size_t *message_length)
{
	return linkr_debugger_ws_rx_client_step(
		assembly, scratch, 512U, data, length, metadata_valid,
		frame_flags, remaining, test_ws_send_pong, pong, message,
		message_length);
}

static void test_ws_control_frames_preserve_fragmented_binary_message(void)
{
	struct linkr_debugger_ws_rx_assembly assembly = { 0 };
	struct test_ws_pong_capture pong = { 0 };
	uint8_t scratch[512] = { 0 };
	uint8_t first[200];
	uint8_t second[280];
	static const uint8_t ping_a[] = { 0x00U, 0xffU };
	static const uint8_t ping_b[] = { 0x7eU };
	static const uint8_t pong_payload[] = { 0x12U, 0x34U };
	const uint8_t *message;
	size_t message_length;

	for (size_t i = 0U; i < sizeof(first); ++i) {
		first[i] = (uint8_t)i;
	}
	for (size_t i = 0U; i < sizeof(second); ++i) {
		second[i] = (uint8_t)(i + sizeof(first));
	}
	assert(test_ws_rx_client_step(
		       &assembly, scratch, first, sizeof(first), true,
		       LINKR_DEBUGGER_WS_FRAME_BINARY, 0U, &pong, &message,
		       &message_length) == LINKR_DEBUGGER_WS_RX_MORE);
	assert(assembly.active && assembly.length == sizeof(first));

	/* Zephyr can return control payload bytes before setting type/remaining. */
	assert(test_ws_rx_client_step(
		       &assembly, scratch, ping_a, sizeof(ping_a), false, 0U,
		       UINT64_MAX, &pong, &message, &message_length) ==
	       LINKR_DEBUGGER_WS_RX_MORE);
	assert(pong.calls == 0U && assembly.length == sizeof(first));
	assert(test_ws_rx_client_step(
		       &assembly, scratch, ping_b, sizeof(ping_b), true,
		       LINKR_DEBUGGER_WS_FRAME_PING |
			       LINKR_DEBUGGER_WS_FRAME_FINAL,
		       0U, &pong, &message, &message_length) ==
	       LINKR_DEBUGGER_WS_RX_MORE);
	assert(pong.calls == 1U);
	assert(pong.length == sizeof(ping_a) + sizeof(ping_b));
	assert(memcmp(pong.payload, ping_a, sizeof(ping_a)) == 0);
	assert(memcmp(pong.payload + sizeof(ping_a), ping_b, sizeof(ping_b)) == 0);
	assert(assembly.active && assembly.length == sizeof(first));

	assert(test_ws_rx_client_step(
		       &assembly, scratch, pong_payload, sizeof(pong_payload), true,
		       LINKR_DEBUGGER_WS_FRAME_PONG |
			       LINKR_DEBUGGER_WS_FRAME_FINAL,
		       0U, &pong, &message, &message_length) ==
	       LINKR_DEBUGGER_WS_RX_MORE);
	assert(pong.calls == 1U);
	assert(assembly.active && assembly.length == sizeof(first));

	assert(test_ws_rx_client_step(
		       &assembly, scratch, second, sizeof(second), true,
		       LINKR_DEBUGGER_WS_FRAME_FINAL, 0U, &pong, &message,
		       &message_length) == LINKR_DEBUGGER_WS_RX_BINARY);
	assert(message_length == LINKR_DEBUGGER_WS_MESSAGE_MAX);
	assert(memcmp(message, first, sizeof(first)) == 0);
	assert(memcmp(message + sizeof(first), second, sizeof(second)) == 0);
}

static void test_ws_control_close_and_malformed_frames(void)
{
	struct linkr_debugger_ws_rx_assembly assembly = { 0 };
	struct test_ws_pong_capture pong = { 0 };
	uint8_t scratch[512] = { 0 };
	uint8_t oversized[LINKR_DEBUGGER_WS_CONTROL_PAYLOAD_MAX + 1U] = { 0 };
	static const uint8_t close_payload[] = { 0x03U, 0xe8U };
	static const uint8_t byte = 0x55U;
	const uint8_t *message;
	size_t message_length;

	assert(test_ws_rx_client_step(
		       &assembly, scratch, close_payload, sizeof(close_payload), true,
		       LINKR_DEBUGGER_WS_FRAME_CLOSE |
			       LINKR_DEBUGGER_WS_FRAME_FINAL,
		       0U, &pong, &message, &message_length) ==
	       LINKR_DEBUGGER_WS_RX_CLOSE);
	assert(pong.calls == 0U);
	assert(!assembly.active && assembly.length == 0U &&
	       assembly.unreported_length == 0U);

	linkr_debugger_ws_rx_reset(&assembly);
	assert(test_ws_rx_client_step(
		       &assembly, scratch, &byte, 1U, true,
		       LINKR_DEBUGGER_WS_FRAME_PING, 0U, &pong, &message,
		       &message_length) == -EPROTO);
	linkr_debugger_ws_rx_reset(&assembly);
	assert(test_ws_rx_client_step(
		       &assembly, scratch, oversized, sizeof(oversized), true,
		       LINKR_DEBUGGER_WS_FRAME_PING |
			       LINKR_DEBUGGER_WS_FRAME_FINAL,
		       0U, &pong, &message, &message_length) == -EPROTO);
	linkr_debugger_ws_rx_reset(&assembly);
	assert(test_ws_rx_client_step(
		       &assembly, scratch, &byte, 1U, true,
		       LINKR_DEBUGGER_WS_FRAME_PING |
			       LINKR_DEBUGGER_WS_FRAME_FINAL,
		       1U, &pong, &message, &message_length) == -EPROTO);
	linkr_debugger_ws_rx_reset(&assembly);
	assert(test_ws_rx_client_step(
		       &assembly, scratch, &byte, 1U, true,
		       LINKR_DEBUGGER_WS_FRAME_PING |
			       LINKR_DEBUGGER_WS_FRAME_BINARY |
			       LINKR_DEBUGGER_WS_FRAME_FINAL,
		       0U, &pong, &message, &message_length) == -EPROTO);
	linkr_debugger_ws_rx_reset(&assembly);
	assert(test_ws_rx_client_step(
		       &assembly, scratch, &byte, 1U, true,
		       LINKR_DEBUGGER_WS_FRAME_CLOSE |
			       LINKR_DEBUGGER_WS_FRAME_FINAL,
		       0U, &pong, &message, &message_length) == -EPROTO);
}

static void test_serial_overrun_contract_is_delta_plus_lifetime_total(void)
{
	assert(linkr_debugger_ws_serial_drop_delta(10U, 15U) == 5U);
	/* If a slot counter is reset, the new total itself is the delta. */
	assert(linkr_debugger_ws_serial_drop_delta(15U, 3U) == 3U);
}

static void test_serial_terminal_responder_projection(void)
{
	struct linkr_debugger_serial_terminal_responder responder = { 0 };
	struct linkr_debugger_serial_terminal_responder effective;
	struct linkr_debugger_serial_status serial = { 0 };
	int8_t responder_id;
	char buffer[160];
	size_t cursor = 0U;

	assert(!linkr_debugger_ws_is_terminal_responder(&responder, 0U));
	assert(linkr_debugger_ws_append_terminal_responder(
		       buffer, sizeof(buffer), &cursor, &responder, 0U) == 0);
	assert(strcmp(buffer,
	      "\"responder\":null,\"terminal_responder\":false") == 0);

	responder.valid = true;
	responder.transport =
		LINKR_DEBUGGER_SERIAL_TERMINAL_RESPONDER_WEBSOCKET;
	responder.slot = 0;
	cursor = 0U;
	assert(linkr_debugger_ws_is_terminal_responder(&responder, 0U));
	assert(!linkr_debugger_ws_is_terminal_responder(&responder, 1U));
	assert(linkr_debugger_ws_append_terminal_responder(
		       buffer, sizeof(buffer), &cursor, &responder, 0U) == 0);
	assert(strstr(buffer, "\"terminal_responder\":true") != NULL);
	cursor = 0U;
	assert(linkr_debugger_ws_append_terminal_responder(
		       buffer, sizeof(buffer), &cursor, &responder, 1U) == 0);
	assert(strcmp(
		       buffer,
		       "\"responder\":{\"transport\":\"websocket\",\"slot\":0},"
		       "\"terminal_responder\":false") == 0);

	responder.transport = LINKR_DEBUGGER_SERIAL_TERMINAL_RESPONDER_CDC;
	responder.slot = 0;
	cursor = 0U;
	assert(!linkr_debugger_ws_is_terminal_responder(&responder, 0U));
	assert(linkr_debugger_ws_append_terminal_responder(
		       buffer, sizeof(buffer), &cursor, &responder, 0U) == 0);
	assert(strcmp(buffer,
	      "\"responder\":{\"transport\":\"cdc\",\"slot\":0},"
	      "\"terminal_responder\":false") == 0);

	/* Immediate DTR is authoritative even if the network projection still
	 * contains the previous WS responder.
	 */
	responder.transport =
		LINKR_DEBUGGER_SERIAL_TERMINAL_RESPONDER_WEBSOCKET;
	responder.slot = 0;
	serial.dtr = true;
	effective = linkr_debugger_ws_effective_terminal_responder(
		&serial, &responder);
	assert(effective.valid);
	assert(effective.transport ==
	       LINKR_DEBUGGER_SERIAL_TERMINAL_RESPONDER_CDC);
	assert(!linkr_debugger_ws_is_terminal_responder(&effective, 0U));
	serial.dtr = false;
	responder_id = linkr_debugger_ws_serial_responder_id(&responder);
	serial.cdc_responder_generation = UINT64_MAX;
	assert(responder_id == 1);
	assert(serial.cdc_responder_generation == UINT64_MAX);
}

struct serial_guard_fixture {
	char order[16];
	size_t length;
	unsigned int refresh_calls;
	unsigned int validate_calls;
	bool stale_on_first_validate;
};

static int serial_guard_refresh(void *context)
{
	struct serial_guard_fixture *fixture = context;

	fixture->refresh_calls++;
	fixture->order[fixture->length++] = 'S';
	fixture->order[fixture->length] = '\0';
	return 0;
}

static int serial_guard_validate(void *context)
{
	struct serial_guard_fixture *fixture = context;

	fixture->validate_calls++;
	fixture->order[fixture->length++] = 'V';
	fixture->order[fixture->length] = '\0';
	return fixture->stale_on_first_validate &&
		       fixture->validate_calls == 1U ? -ESTALE : 0;
}

static void test_serial_post_read_guard_orders_state_before_binary(void)
{
	struct serial_guard_fixture fixture = {
		.order = "IR",
		.length = 2U,
	};

	/* I=initial snapshot, R=ws_read, S=state refresh/emit, V=epoch
	 * validation, B=binary send. A DTR change injected at R therefore emits
	 * its state before B.
	 */
	assert(linkr_debugger_ws_serial_post_read_guard(
		       serial_guard_refresh, serial_guard_validate, &fixture) == 0);
	fixture.order[fixture.length++] = 'B';
	fixture.order[fixture.length] = '\0';
	assert(strcmp(fixture.order, "IRSVSVSVB") == 0);

	memset(&fixture, 0, sizeof(fixture));
	fixture.stale_on_first_validate = true;
	assert(linkr_debugger_ws_serial_post_read_guard(
		       serial_guard_refresh, serial_guard_validate, &fixture) ==
	       -ESTALE);
	assert(strchr(fixture.order, 'B') == NULL);
}

static void test_ws_serial_sidecars_are_isolated_and_reset_on_slot_reuse(void)
{
	struct linkr_debugger_ws_client_sidecar *slot0;
	struct linkr_debugger_ws_client_sidecar *slot1;

	assert(linkr_debugger_ws_sidecar_reset_slot(0U));
	assert(linkr_debugger_ws_sidecar_reset_slot(1U));
	assert(!linkr_debugger_ws_sidecar_reset_slot(
		LINKR_DEBUGGER_WS_SIDECAR_COUNT));
	assert(linkr_debugger_ws_sidecar_at(
		       LINKR_DEBUGGER_WS_SIDECAR_COUNT) == NULL);
	slot0 = linkr_debugger_ws_sidecar_at(0U);
	slot1 = linkr_debugger_ws_sidecar_at(1U);
	assert(slot0 != NULL && slot1 != NULL && slot0 != slot1);

	slot0->rx_assembly.data[0] = 0xffU;
	slot0->rx_assembly.length = 1U;
	slot0->serial_pending.data[0] = 0x7eU;
	slot0->serial_pending.length = 1U;
	slot0->serial_pending.valid = true;
	slot0->serial_signature.generation = 42U;
	slot0->serial_last_drops = 9U;
	slot0->stream_mode = LINKR_DEBUGGER_WS_MODE_SERIAL_J4;
	slot0->serial_state_valid = true;
	slot1->stream_mode = LINKR_DEBUGGER_WS_MODE_LIVE_ADC;
	slot1->serial_last_drops = 3U;

	assert(slot1->rx_assembly.length == 0U);
	assert(!slot1->serial_pending.valid);
	assert(slot1->serial_signature.generation == 0U);
	assert(slot1->serial_last_drops == 3U);
	assert(slot1->stream_mode == LINKR_DEBUGGER_WS_MODE_LIVE_ADC);

	/* Reset models session deletion followed by reuse of the immutable slot.
	 * No assembly, pending write, signature, mode, or overrun state survives.
	 */
	assert(linkr_debugger_ws_sidecar_reset_slot(0U));
	assert(slot0->rx_assembly.length == 0U);
	assert(slot0->rx_assembly.data[0] == 0U);
	assert(!slot0->serial_pending.valid);
	assert(slot0->serial_pending.length == 0U);
	assert(slot0->serial_signature.generation == 0U);
	assert(slot0->serial_last_drops == 0U);
	assert(slot0->stream_mode == LINKR_DEBUGGER_WS_MODE_IDLE);
	assert(!slot0->serial_state_valid);
	/* Resetting/reusing slot 0 cannot disturb slot 1. */
	assert(slot1->stream_mode == LINKR_DEBUGGER_WS_MODE_LIVE_ADC);
	assert(slot1->serial_last_drops == 3U);
}

static void test_gpio_busy_error_uses_shared_wire_code(void)
{
	struct linkr_debugger_ws_client client = { 0 };

	expect_reboot_send = false;
	send_calls = 0U;
	send_result = 0;
	for (unsigned int output = 0U; output < 2U; output++) {
		const struct linkr_debugger_gpio_error *error =
			linkr_debugger_gpio_configure_error(output != 0U, -EBUSY);

		assert(linkr_debugger_ws_emit_error(&client, "gpio", error->code,
			error->message) == 0);
		assert(strstr(sent_json, "\"type\":\"error\"") != NULL);
		assert(strstr(sent_json, "\"command\":\"gpio\"") != NULL);
		assert(strstr(sent_json, "\"code\":\"busy\"") != NULL);
		assert(strstr(sent_json, "GPIO is reserved by a capture") != NULL);
	}
	assert(send_calls == 2U);
}

int main(void)
{
	test_gpio_busy_error_uses_shared_wire_code();
	test_old_resume_does_not_release_new_pause();
	test_pause_generation_never_uses_zero();
	test_adc_sample_rate_budget();
	test_eight_channel_single_encoder();
	test_eight_channel_batch_encoder_masks_and_source_drops();
	test_eight_channel_batch_actual_frame_budget();
	test_adc_ring_wrap_oldest_and_complete_samples();
	test_adc_json_writer_preserves_canonical_escaping_and_utf8();
	test_sampler_deadline_excludes_read_time();
	test_sampler_deadline_skips_overrun_without_catch_up();
	test_serial_live_and_sigrok_modes_are_mutually_exclusive();
	test_ws_serial_frame_assembly_waits_for_complete_final_message();
	test_ws_frame_assembly_tracks_type_fin_and_continuations();
	test_ws_frame_assembly_drains_oversize_before_reset();
	test_ws_control_frames_preserve_fragmented_binary_message();
	test_ws_control_close_and_malformed_frames();
	test_serial_overrun_contract_is_delta_plus_lifetime_total();
	test_serial_terminal_responder_projection();
	test_serial_post_read_guard_orders_state_before_binary();
	test_ws_serial_sidecars_are_isolated_and_reset_on_slot_reuse();
	test_reboot_acknowledges_normal_reboot_then_pending();
	test_reboot_reports_schedule_and_send_errors();
	test_gpio_layout_group_encoder_is_generic_and_bounded();
	test_safe_gpio_layout_omits_unmapped_coordinates();
	return 0;
}
