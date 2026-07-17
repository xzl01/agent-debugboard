/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_logic_analyzer.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#ifndef BIT
#define BIT(n) (1UL << (n))
#endif

static struct linkr_debugger_la_config base_config(void)
{
	struct linkr_debugger_la_config config;

	memset(&config, 0, sizeof(config));
	config.pin_base = 7U;
	config.pin_count = 4U;
	config.sample_rate_hz = 1000000U;
	config.post_samples = 16U;
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_NONE;
	return config;
}

static void test_rate_quantization(void)
{
	assert(linkr_debugger_logic_analyzer_actual_rate(1000000U) == 1000000U);
	assert(linkr_debugger_logic_analyzer_actual_rate(125000000U) == 125000000U);
	assert(linkr_debugger_logic_analyzer_actual_rate(100000000U) == 100000000U);
	assert(linkr_debugger_logic_analyzer_actual_rate(999999U) == 0U);
	assert(linkr_debugger_logic_analyzer_actual_rate(125000001U) == 0U);
	assert(linkr_debugger_logic_analyzer_sample_period_ps(1000000U) == 1000000ULL);
	assert(linkr_debugger_logic_analyzer_sample_period_ps(125000000U) == 8000ULL);
}

static void test_dma_block_size(void)
{
	assert(linkr_debugger_logic_analyzer_dma_block_size(0U) == 0U);
	assert(linkr_debugger_logic_analyzer_dma_block_size(1U) == 4U);
	assert(linkr_debugger_logic_analyzer_dma_block_size(512U) == 2048U);
	assert(linkr_debugger_logic_analyzer_dma_block_size(513U) == 0U);
	assert(linkr_debugger_logic_analyzer_max_samples(4U, 2048U) == 512U);
	assert(linkr_debugger_logic_analyzer_max_samples(0U, 2048U) == 0U);
}

static void test_validation(void)
{
	struct linkr_debugger_la_config config = base_config();

	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == 0);

	config = base_config();
	config.selected_pins[0] = 7U;
	config.selected_pins[1] = 10U;
	config.selected_pins[2] = 29U;
	config.selected_pin_count = 3U;
	config.pin_count = 3U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == 0);

	config = base_config();
	config.pin_base = 6U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.pin_base = 21U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.selected_pins[0] = 7U;
	config.selected_pins[1] = 7U;
	config.selected_pin_count = 2U;
	config.pin_count = 2U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.pre_samples = 1U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	config.trigger_pin = 4U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	config.trigger_pin = 1U;
	config.pre_samples = 8U;
	config.post_samples = 8U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == 0);

	config = base_config();
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_EITHER;
	config.trigger_pin = 1U;
	config.pre_samples = 8U;
	config.post_samples = 8U;
	config.sample_rate_hz = 25000000U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == 0);

	config = base_config();
	config.trigger = LINKR_DEBUGGER_LA_TRIGGER_RISING;
	config.trigger_pin = 1U;
	config.pre_samples = 8U;
	config.post_samples = 8U;
	config.sample_rate_hz = 25000001U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.sample_rate_hz = 125000001U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);

	config = base_config();
	config.post_samples = 513U;
	assert(linkr_debugger_logic_analyzer_validate_config(&config, 512U) == -EINVAL);
}

static void test_compression(void)
{
	struct linkr_debugger_la_config config = base_config();

	assert(linkr_debugger_logic_analyzer_compress_raw_sample(
		(uint32_t)(BIT(7) | BIT(9)), &config) == 5U);
	assert(linkr_debugger_logic_analyzer_compress_raw_sample(
		(uint32_t)BIT(10), &config) == 8U);

	config.selected_pins[0] = 7U;
	config.selected_pins[1] = 10U;
	config.selected_pins[2] = 29U;
	config.selected_pin_count = 3U;
	config.pin_count = 3U;

	assert(linkr_debugger_logic_analyzer_compress_raw_sample(
		(uint32_t)(BIT(7) | BIT(29)), &config) == 5U);
	assert(linkr_debugger_logic_analyzer_compress_raw_sample(
		(uint32_t)BIT(10), &config) == 2U);
}

static void test_either_trigger_program_uses_absolute_offset(void)
{
	uint16_t instructions[5] = {0};

	assert(linkr_debugger_logic_analyzer_build_either_trigger_program(
		0U, instructions, 5U) == 0);
	assert(instructions[0] == 0x00c3U);
	assert(instructions[2] == 0x0004U);

	memset(instructions, 0, sizeof(instructions));
	assert(linkr_debugger_logic_analyzer_build_either_trigger_program(
		8U, instructions, 5U) == 0);
	assert(instructions[0] == 0x00cbU);
	assert(instructions[2] == 0x000cU);
	assert(linkr_debugger_logic_analyzer_build_either_trigger_program(
		28U, instructions, 5U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_build_either_trigger_program(
		0U, instructions, 4U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_build_either_trigger_program(
		0U, NULL, 5U) == -EINVAL);
}

static void test_capture_snapshot_copy_contract(void)
{
	struct linkr_debugger_la_capture capture;
	struct linkr_debugger_la_capture snapshot;
	struct linkr_debugger_la_sample source[3];
	struct linkr_debugger_la_sample copied[3];
	struct linkr_debugger_la_sample too_small[2];

	memset(&capture, 0, sizeof(capture));
	memset(source, 0, sizeof(source));
	capture.state = LINKR_DEBUGGER_LA_STATE_DONE;
	capture.sample_count = 3U;
	capture.trigger_index = 1U;
	capture.requested_sample_rate_hz = 1000000U;
	capture.actual_sample_rate_hz = 1000000U;
	capture.sample_period_ps = 1000000ULL;
	capture.backend = "host-test";
	source[0].timestamp_us = 1U;
	source[0].values = 0x0001U;
	source[1].timestamp_us = 2U;
	source[1].values = 0x0002U;
	source[2].timestamp_us = 3U;
	source[2].values = 0x0004U;

	assert(linkr_debugger_logic_analyzer_get_capture(&snapshot, copied, 3U) == -ENODATA);
	assert(linkr_debugger_logic_analyzer_host_set_capture(&capture, source, 3U) == 0);
	assert(linkr_debugger_logic_analyzer_get_capture(&snapshot, too_small, 2U) == -ENOSPC);
	assert(linkr_debugger_logic_analyzer_get_capture(NULL, copied, 3U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_get_capture(&snapshot, NULL, 3U) == -EINVAL);
	assert(linkr_debugger_logic_analyzer_get_capture(&snapshot, copied, 0U) == -EINVAL);

	assert(linkr_debugger_logic_analyzer_get_capture(&snapshot, copied, 3U) == 0);
	assert(snapshot.samples == copied);
	assert(snapshot.sample_count == 3U);
	assert(copied[0].timestamp_us == 1U);
	assert(copied[1].values == 0x0002U);
	source[1].values = 0xffffU;
	assert(copied[1].values == 0x0002U);
}

int main(void)
{
	test_rate_quantization();
	test_dma_block_size();
	test_validation();
	test_compression();
	test_either_trigger_program_uses_absolute_offset();
	test_capture_snapshot_copy_contract();
	return 0;
}
