/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_LOGIC_ANALYZER_H_
#define RADXA_LINKR_DEBUGGER_LOGIC_ANALYZER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINKR_DEBUGGER_LA_MAX_CHANNELS 16
#define LINKR_DEBUGGER_LA_DEFAULT_BUFFER_SIZE (32 * 1024)
#define LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES 512
#define LINKR_DEBUGGER_LA_STREAM_HALF_SAMPLES 1024
#define LINKR_DEBUGGER_LA_MIN_SAMPLE_RATE_HZ 100000U
#define LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ 125000000U
#define LINKR_DEBUGGER_LA_MAX_STREAM_RATE_HZ 25000000U

enum linkr_debugger_la_trigger_type {
	LINKR_DEBUGGER_LA_TRIGGER_NONE = 0,
	LINKR_DEBUGGER_LA_TRIGGER_RISING,
	LINKR_DEBUGGER_LA_TRIGGER_FALLING,
	LINKR_DEBUGGER_LA_TRIGGER_EITHER,
};

struct linkr_debugger_la_config {
	uint8_t pin_base;
	uint8_t pin_count;
	uint8_t selected_pins[LINKR_DEBUGGER_LA_MAX_CHANNELS];
	uint8_t selected_pin_count;
	uint32_t sample_rate_hz;
	uint32_t pre_samples;
	uint32_t post_samples;
	enum linkr_debugger_la_trigger_type trigger;
	uint8_t trigger_pin;
};

enum linkr_debugger_la_state {
	LINKR_DEBUGGER_LA_STATE_IDLE = 0,
	LINKR_DEBUGGER_LA_STATE_ARMED,
	LINKR_DEBUGGER_LA_STATE_CAPTURING,
	LINKR_DEBUGGER_LA_STATE_DONE,
	LINKR_DEBUGGER_LA_STATE_ERROR,
	LINKR_DEBUGGER_LA_STATE_STREAMING,
};

struct linkr_debugger_la_sample {
	uint32_t timestamp_us;
	uint16_t values;
	uint16_t reserved;
};

struct linkr_debugger_la_capture {
	enum linkr_debugger_la_state state;
	struct linkr_debugger_la_config config;
	uint32_t sample_count;
	uint32_t trigger_index;
	uint32_t requested_sample_rate_hz;
	uint32_t actual_sample_rate_hz;
	uint64_t sample_period_ps;
	const char *backend;
	struct linkr_debugger_la_sample *samples;
};

int linkr_debugger_logic_analyzer_init(void);
int linkr_debugger_logic_analyzer_arm(const struct linkr_debugger_la_config *config);
int linkr_debugger_logic_analyzer_cancel(void);
enum linkr_debugger_la_state linkr_debugger_logic_analyzer_get_state(void);
int linkr_debugger_logic_analyzer_get_capture(
	struct linkr_debugger_la_capture *capture,
	struct linkr_debugger_la_sample *samples,
	size_t sample_capacity);
void linkr_debugger_logic_analyzer_release(void);
uint32_t linkr_debugger_logic_analyzer_max_samples(uint8_t pin_count, uint32_t buffer_size);
uint32_t linkr_debugger_logic_analyzer_actual_rate(uint32_t requested_rate);
uint64_t linkr_debugger_logic_analyzer_sample_period_ps(uint32_t actual_rate_hz);
uint32_t linkr_debugger_logic_analyzer_dma_block_size(uint32_t sample_count);
uint16_t linkr_debugger_logic_analyzer_compress_raw_sample(
	uint32_t raw, const struct linkr_debugger_la_config *config);
int linkr_debugger_logic_analyzer_validate_config(
	const struct linkr_debugger_la_config *config, uint32_t capacity_samples);
const char *linkr_debugger_logic_analyzer_backend(void);

int linkr_debugger_logic_analyzer_build_either_trigger_program(
	uint8_t offset, uint16_t *instructions, size_t instruction_count);

/* Continuous streaming mode */
struct linkr_debugger_la_stream_chunk {
	uint32_t sequence;
	uint32_t sample_count;
	uint64_t timestamp_us;
	uint16_t *values;
};

typedef void (*linkr_debugger_la_stream_callback_t)(
	const struct linkr_debugger_la_stream_chunk *chunk, void *user_data);

int linkr_debugger_logic_analyzer_start_stream(
	const struct linkr_debugger_la_config *config,
	linkr_debugger_la_stream_callback_t callback,
	void *user_data);
int linkr_debugger_logic_analyzer_stop_stream(void);
bool linkr_debugger_logic_analyzer_is_streaming(void);

struct linkr_debugger_la_debug {
	uint32_t stream_irqs;
	uint32_t stream_chunks;
	uint32_t pre_write_index;
	uint32_t pre_post_remaining;
	uint16_t stream_values_or;
	uint16_t stream_values_and;
	bool pre_active;
	bool pre_triggered;
	bool stream_active;
};

void linkr_debugger_logic_analyzer_get_debug(struct linkr_debugger_la_debug *out);

#if defined(LINKR_DEBUGGER_LA_HOST_TEST)
int linkr_debugger_logic_analyzer_host_set_capture(
	const struct linkr_debugger_la_capture *capture,
	const struct linkr_debugger_la_sample *samples,
	size_t sample_count);
#endif

#endif /* RADXA_LINKR_DEBUGGER_LOGIC_ANALYZER_H_ */
