/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_capture_gpio_guard.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "linkr_debugger_logic_analyzer.h"

/* Compile the actual control-layer claim/release, GPIO setters and cached
 * readback; only device operations and the control mutex are modeled. */
#include "../../src/linkr_debugger_control.c"

const struct linkr_debugger_safe_gpio_desc linkr_debugger_safe_gpios[] = {
	{ .pin = 12U, .output_capable = true },
	{ .pin = 13U, .output_capable = true },
	{ .pin = 20U, .output_capable = false },
};
const size_t linkr_debugger_safe_gpio_count = 3U;

int linkr_debugger_logic_analyzer_host_j4_capture_prepare(
	const struct linkr_debugger_la_config *config, bool fail_after_mux);
void linkr_debugger_logic_analyzer_host_j4_capture_cleanup(void);

#ifndef LINKR_DEBUGGER_GPIO_DIR_INPUT
#define LINKR_DEBUGGER_GPIO_DIR_INPUT 1U
#endif

#ifndef LINKR_DEBUGGER_GPIO_DIR_OUTPUT
#define LINKR_DEBUGGER_GPIO_DIR_OUTPUT 2U
#endif

static void test_no_conflict_for_input_pins(void)
{
	const struct linkr_debugger_safe_gpio_desc safe_gpios[] = {
		{ .pin = 10U },
		{ .pin = 13U },
		{ .pin = 29U },
	};
	const uint8_t directions[] = {
		LINKR_DEBUGGER_GPIO_DIR_INPUT,
		LINKR_DEBUGGER_GPIO_DIR_OUTPUT,
		LINKR_DEBUGGER_GPIO_DIR_INPUT,
	};

	assert(!linkr_debugger_capture_gpio_conflicts_with_output(
		BIT(10) | BIT(11) | BIT(29), safe_gpios, directions, 3U, NULL));
}

static void test_conflict_reports_first_output_pin(void)
{
	const struct linkr_debugger_safe_gpio_desc safe_gpios[] = {
		{ .pin = 10U },
		{ .pin = 13U },
		{ .pin = 29U },
	};
	const uint8_t directions[] = {
		LINKR_DEBUGGER_GPIO_DIR_INPUT,
		LINKR_DEBUGGER_GPIO_DIR_OUTPUT,
		LINKR_DEBUGGER_GPIO_DIR_OUTPUT,
	};
	uint8_t conflict_pin = 0U;

	assert(linkr_debugger_capture_gpio_conflicts_with_output(
		BIT(13) | BIT(29), safe_gpios, directions, 3U, &conflict_pin));
	assert(conflict_pin == 13U);
}

static void test_duplicate_capture_pins_and_empty_inputs(void)
{
	const struct linkr_debugger_safe_gpio_desc safe_gpios[] = {
		{ .pin = 13U },
	};
	const uint8_t directions[] = { LINKR_DEBUGGER_GPIO_DIR_OUTPUT };

	assert(!linkr_debugger_capture_gpio_conflicts_with_output(
		0U, safe_gpios, directions, 1U, NULL));
	assert(linkr_debugger_capture_gpio_conflicts_with_output(
		BIT(13), safe_gpios, directions, 1U, NULL));
}

static void test_control_and_la_physical_lane_ownership(void)
{
	const struct linkr_debugger_safe_gpio_desc *gpio = &linkr_debugger_safe_gpios[0];
	struct linkr_debugger_la_config config = {
		.sample_rate_hz = 100000U, .post_samples = 64U,
		.selected_pin_count = 2U, .selected_pins = { 13U, 14U },
	};
	struct linkr_debugger_la_start_prepare prepare, stale;
	unsigned int calls;
	uint32_t generation;
	int value;

	assert(linkr_debugger_logic_analyzer_init() == 0);
	assert(linkr_debugger_gpio_set_output(gpio, true) == 0);
	calls = gpio_configure_calls;
	/* GP12 is NOT selected, but FAST8's physical lane owns GP10..17. */
	assert(linkr_debugger_logic_analyzer_host_j4_capture_prepare(&config, false) == -EBUSY);
	assert(linkr_debugger_logic_analyzer_prepare_wide11_burst_start(&config, &prepare) == -EBUSY);
	assert(linkr_debugger_gpio_get(gpio, &value) == 0 && value == 1);
	assert(gpio_configure_calls == calls && capture_gpio_mask == 0U);
	assert(linkr_debugger_gpio_set_input(gpio) == 0);
	assert(linkr_debugger_logic_analyzer_host_j4_capture_prepare(&config, true) == -EIO);
	assert(capture_gpio_mask == 0U); /* failure after mux must release */
	assert(linkr_debugger_logic_analyzer_host_j4_capture_prepare(&config, false) == 0);
	assert(capture_gpio_mask == (UINT32_C(0xff) << 10));
	calls = gpio_configure_calls;
	assert(linkr_debugger_gpio_set_output(gpio, false) == -EBUSY);
	assert(linkr_debugger_gpio_set_input(gpio) == -EBUSY);
	assert(gpio_configure_calls == calls);
	assert(linkr_debugger_gpio_get(gpio, &value) == 0 && value == 0);
	generation = capture_gpio_generation;
	linkr_debugger_logic_analyzer_host_j4_capture_cleanup();
	assert(capture_gpio_mask == 0U);
	assert(linkr_debugger_logic_analyzer_prepare_wide11_burst_start(&config, &prepare) == 0);
	stale = prepare;
	assert(linkr_debugger_logic_analyzer_start_prepare_cancel(&prepare) == 0);
	assert(capture_gpio_mask == 0U);
	assert(linkr_debugger_logic_analyzer_prepare_wide11_burst_start(&config, &prepare) == 0);
	linkr_debugger_capture_gpio_release(generation);
	assert(linkr_debugger_logic_analyzer_start_prepare_cancel(&stale) == -ESTALE);
	assert(linkr_debugger_gpio_set_output(gpio, true) == -EBUSY);
	assert(linkr_debugger_logic_analyzer_start_prepare_mark_response_sent(&prepare) == 0);
	assert(linkr_debugger_logic_analyzer_start_prepare_go(&prepare) == -ENOTSUP);
	assert(capture_gpio_mask == 0U);
	/* Hardware failures also leave cached direction/level unchanged. */
	gpio_configure_result = -EIO;
	assert(linkr_debugger_gpio_set_output(gpio, true) == -EIO);
	assert(linkr_debugger_gpio_get(gpio, &value) == 0 && value == 0);
	gpio_configure_result = 0;
	/* Wide lane includes the read-only GP20 tap, without configuring it. */
	config.selected_pins[0] = 20U;
	assert(linkr_debugger_logic_analyzer_prepare_wide11_burst_start(&config, &prepare) == 0);
	assert(capture_gpio_mask & BIT(20));
	calls = gpio_configure_calls;
	assert(linkr_debugger_gpio_set_input(&linkr_debugger_safe_gpios[2]) == 0);
	assert(linkr_debugger_gpio_set_output(&linkr_debugger_safe_gpios[2], true) == -EPERM);
	assert(gpio_configure_calls == calls);
	assert(linkr_debugger_logic_analyzer_cancel() == 0);
	assert(capture_gpio_mask == 0U);
}

static void assert_call_order(const char *function, const char *first, const char *second)
{
	const char *a = strstr(function, first);
	const char *b = strstr(function, second);
	const char *end = strstr(function, "\n}\n");

	assert(a && b && end && a < b && b < end);
}

static void test_production_lane_claim_and_cleanup_wiring(void)
{
	char path[sizeof(__FILE__) + 80U] = __FILE__;
	char *slash = strrchr(path, '/');
	char *source;
	FILE *file;
	long size;
	const char *burst, *ring, *restore;

	assert(slash != NULL);
	strcpy(slash + 1, "../../src/linkr_debugger_logic_analyzer.c");
	file = fopen(path, "rb");
	assert(file != NULL && fseek(file, 0L, SEEK_END) == 0);
	size = ftell(file);
	assert(size > 0L);
	rewind(file);
	source = malloc((size_t)size + 1U);
	assert(source != NULL && fread(source, 1U, (size_t)size, file) == (size_t)size);
	source[size] = '\0';
	assert(fclose(file) == 0);
	burst = strstr(source, "static int la_packed_burst_configure_locked(");
	ring = strstr(source, "static int la_packed_ring_configure_locked(");
	restore = strstr(source, "static void la_restore_configured_pins(");
	assert(burst && ring && restore);
	assert_call_order(burst, "ret = la_claim_burst_gpio_locked(plan);",
		"ret = la_pause_j4_serial_locked(");
	assert_call_order(ring, "ret = la_claim_ring_gpio_locked(&la_stream_ring_plan);",
		"ret = la_pause_j4_serial_locked(");
	assert_call_order(restore, "la_resume_j4_serial_after_mux_restore_locked();",
		"la_release_gpio_locked();");
	free(source);
}

int main(void)
{
	test_no_conflict_for_input_pins();
	test_conflict_reports_first_output_pin();
	test_duplicate_capture_pins_and_empty_inputs();
	test_control_and_la_physical_lane_ownership();
	test_production_lane_claim_and_cleanup_wiring();
	return 0;
}
