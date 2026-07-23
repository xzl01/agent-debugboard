/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_capture_engine.h"

#include <errno.h>
#include <string.h>

static uint8_t linkr_debugger_capture_engine_shared_arena[
	LINKR_DEBUGGER_CAPTURE_ENGINE_ARENA_BYTES] __attribute__((aligned(4)));

struct linkr_debugger_logic_session_runtime {
	enum linkr_debugger_logic_session_state state;
	enum linkr_debugger_logic_session_owner owner;
	uint32_t session_id;
	uint32_t pre_samples;
	uint32_t ring_samples;
	uint8_t bytes_per_sample;
	uint64_t writer_seq;
	uint64_t reader_seq;
	uint64_t eligible_seq;
	uint64_t trigger_seq;
	uint64_t last_sent_seq;
	uint64_t last_captured_seq;
	bool triggered;
	bool terminal;
	bool overrun;
	const char *backend;
	const char *reason;
};

static struct linkr_debugger_logic_session_runtime logic_session;

uint8_t *linkr_debugger_capture_engine_arena(void)
{
	return linkr_debugger_capture_engine_shared_arena;
}

uint32_t linkr_debugger_capture_engine_arena_size(void)
{
	return LINKR_DEBUGGER_CAPTURE_ENGINE_ARENA_BYTES;
}

const char *linkr_debugger_capture_engine_backend_name(
	enum linkr_debugger_capture_engine_backend backend)
{
	switch (backend) {
	case LINKR_DEBUGGER_CAPTURE_ENGINE_BACKEND_FAST8:
		return "rp2350-pio2-dma-fast8";
	case LINKR_DEBUGGER_CAPTURE_ENGINE_BACKEND_WIDE16:
		return "rp2350-pio2-dma-wide16";
	case LINKR_DEBUGGER_CAPTURE_ENGINE_BACKEND_SPARSE16:
		return "rp2350-pio2-dma-sparse16";
	default:
		return "unsupported";
	}
}

uint32_t linkr_debugger_capture_engine_capacity_samples(uint8_t bytes_per_sample)
{
	if (bytes_per_sample == 0U) {
		return 0U;
	}

	return LINKR_DEBUGGER_CAPTURE_ENGINE_LA_RING_SAMPLES;
}

uint32_t linkr_debugger_capture_engine_sample_bytes(uint32_t sample_count,
	uint8_t bytes_per_sample)
{
	uint64_t bytes = (uint64_t)sample_count * (uint64_t)bytes_per_sample;

	if (bytes_per_sample == 0U || bytes > LINKR_DEBUGGER_CAPTURE_ENGINE_ARENA_BYTES) {
		return 0U;
	}

	return (uint32_t)bytes;
}

static bool capture_engine_is_fast8(const uint8_t *pins, uint8_t pin_count)
{
	if (pin_count == 0U || pin_count > LINKR_DEBUGGER_CAPTURE_ENGINE_FAST_PIN_COUNT) {
		return false;
	}

	for (uint8_t i = 0U; i < pin_count; i++) {
		if (pins[i] != (uint8_t)(LINKR_DEBUGGER_CAPTURE_ENGINE_FAST_BASE_PIN + i)) {
			return false;
		}
	}

	return true;
}

static bool capture_engine_is_wide16(const uint8_t *pins, uint8_t pin_count)
{
	if (pin_count == 0U || pin_count > 12U) {
		return false;
	}

	for (uint8_t i = 0U; i < pin_count; i++) {
		uint8_t expected;

		if (i < LINKR_DEBUGGER_CAPTURE_ENGINE_WIDE_LINEAR_PIN_COUNT) {
			expected = (uint8_t)(LINKR_DEBUGGER_CAPTURE_ENGINE_WIDE_BASE_PIN + i);
		} else {
			expected = LINKR_DEBUGGER_CAPTURE_ENGINE_WIDE_EXTRA_PIN;
		}
		if (pins[i] != expected) {
			return false;
		}
	}

	return true;
}

int linkr_debugger_capture_engine_select_backend(const uint8_t *pins, uint8_t pin_count,
	struct linkr_debugger_capture_engine_layout *layout)
{
	struct linkr_debugger_capture_engine_layout selected;

	if (pins == NULL || layout == NULL || pin_count == 0U) {
		return -EINVAL;
	}

	memset(&selected, 0, sizeof(selected));
	selected.channel_count = pin_count;
	if (capture_engine_is_fast8(pins, pin_count)) {
		selected.backend = LINKR_DEBUGGER_CAPTURE_ENGINE_BACKEND_FAST8;
		selected.bytes_per_sample = 1U;
		selected.storage_bytes_per_sample = sizeof(uint32_t);
		selected.max_rate_hz = LINKR_DEBUGGER_CAPTURE_ENGINE_FAST_MAX_RATE_HZ;
	} else if (capture_engine_is_wide16(pins, pin_count)) {
		selected.backend = LINKR_DEBUGGER_CAPTURE_ENGINE_BACKEND_WIDE16;
		selected.bytes_per_sample = 2U;
		selected.storage_bytes_per_sample = sizeof(uint32_t);
		selected.max_rate_hz = LINKR_DEBUGGER_CAPTURE_ENGINE_WIDE_MAX_RATE_HZ;
	} else {
		selected.backend = LINKR_DEBUGGER_CAPTURE_ENGINE_BACKEND_SPARSE16;
		selected.bytes_per_sample = 2U;
		selected.storage_bytes_per_sample = sizeof(uint32_t);
		selected.max_rate_hz = LINKR_DEBUGGER_CAPTURE_ENGINE_SPARSE_MAX_RATE_HZ;
	}
	selected.name = linkr_debugger_capture_engine_backend_name(selected.backend);
	selected.capacity_samples = linkr_debugger_capture_engine_capacity_samples(
		selected.bytes_per_sample);
	*layout = selected;
	return 0;
}

int linkr_debugger_capture_engine_plan_window(uint32_t write_index, uint32_t capacity_samples,
	uint32_t pre_samples, uint32_t post_samples,
	struct linkr_debugger_capture_engine_window *window)
{
	uint32_t total = pre_samples + post_samples;

	if (window == NULL || capacity_samples == 0U || total == 0U || total > capacity_samples) {
		return -EINVAL;
	}

	window->sample_count = total;
	window->trigger_index = pre_samples;
	window->start_index = (write_index + capacity_samples - pre_samples) % capacity_samples;
	return 0;
}

int linkr_debugger_capture_engine_pack_u16_le(uint8_t *dst, size_t dst_len,
	const uint16_t *samples, uint32_t sample_count)
{
	if (dst == NULL || (sample_count > 0U && samples == NULL) ||
	    dst_len < (size_t)sample_count * 2U) {
		return -EINVAL;
	}

	for (uint32_t i = 0U; i < sample_count; i++) {
		dst[i * 2U] = (uint8_t)(samples[i] & 0xffU);
		dst[(i * 2U) + 1U] = (uint8_t)(samples[i] >> 8);
	}

	return 0;
}

int linkr_debugger_capture_engine_export_marked_window(uint8_t *dst, size_t dst_len,
	const uint32_t *ring, uint32_t ring_samples, uint32_t final_write_index,
	uint32_t pre_samples, uint32_t post_samples, uint8_t bytes_per_sample,
	uint32_t sample_mask, uint32_t *sample_bytes)
{
	uint32_t total = pre_samples + post_samples;
	struct linkr_debugger_capture_engine_window window;
	size_t needed;

	if (sample_bytes == NULL) {
		return -EINVAL;
	}
	*sample_bytes = 0U;
	if (dst == NULL || ring == NULL || bytes_per_sample == 0U || bytes_per_sample > 2U) {
		return -EINVAL;
	}
	if (linkr_debugger_capture_engine_plan_window(final_write_index, ring_samples,
	    pre_samples, post_samples, &window) < 0) {
		return -EINVAL;
	}

	needed = (size_t)total * bytes_per_sample;
	if (needed > dst_len) {
		return -ENOSPC;
	}

	for (uint32_t i = 0U; i < total; i++) {
		uint32_t raw = ring[(window.start_index + i) % ring_samples] & sample_mask;

		if (bytes_per_sample == 1U) {
			dst[i] = (uint8_t)raw;
		} else {
			dst[i * 2U] = (uint8_t)(raw & 0xffU);
			dst[(i * 2U) + 1U] = (uint8_t)((raw >> 8) & 0xffU);
		}
	}

	*sample_bytes = (uint32_t)needed;
	return 0;
}

const char *linkr_debugger_logic_session_state_name(
	enum linkr_debugger_logic_session_state state)
{
	switch (state) {
	case LINKR_DEBUGGER_LOGIC_SESSION_IDLE:
		return "idle";
	case LINKR_DEBUGGER_LOGIC_SESSION_ARMED_PRETRIGGER:
		return "armed_pretrigger";
	case LINKR_DEBUGGER_LOGIC_SESSION_LIVE_UNTRIGGERED:
		return "live_untriggered";
	case LINKR_DEBUGGER_LOGIC_SESSION_TRIGGER_DRAIN:
		return "trigger_drain";
	case LINKR_DEBUGGER_LOGIC_SESSION_LIVE_TRIGGERED:
		return "live_triggered";
	case LINKR_DEBUGGER_LOGIC_SESSION_STOPPING:
		return "stopping";
	case LINKR_DEBUGGER_LOGIC_SESSION_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

void linkr_debugger_logic_session_init(void)
{
	memset(&logic_session, 0, sizeof(logic_session));
	logic_session.state = LINKR_DEBUGGER_LOGIC_SESSION_IDLE;
	logic_session.reason = "idle";
}

static const char *logic_session_encoding(uint8_t bytes_per_sample)
{
	return bytes_per_sample == 1U ? "u8" : "u16le";
}

static void logic_session_set_error(const char *reason)
{
	logic_session.state = LINKR_DEBUGGER_LOGIC_SESSION_ERROR;
	logic_session.reason = reason;
	logic_session.terminal = true;
	logic_session.overrun = strcmp(reason, "overrun") == 0;
	logic_session.last_captured_seq = logic_session.writer_seq;
}

int linkr_debugger_logic_session_start(
	const struct linkr_debugger_logic_session_config *config)
{
	if (config == NULL || config->owner == LINKR_DEBUGGER_LOGIC_SESSION_OWNER_NONE ||
	    config->ring_samples == 0U || config->ring_samples >
	    LINKR_DEBUGGER_CAPTURE_ENGINE_ARENA_BYTES / sizeof(uint32_t) ||
	    config->bytes_per_sample == 0U || config->bytes_per_sample > 2U ||
	    config->pre_samples >= config->ring_samples) {
		return -EINVAL;
	}
	if (logic_session.state != LINKR_DEBUGGER_LOGIC_SESSION_IDLE) {
		return -EBUSY;
	}

	memset(&logic_session, 0, sizeof(logic_session));
	logic_session.owner = config->owner;
	logic_session.session_id = config->session_id;
	logic_session.pre_samples = config->pre_samples;
	logic_session.ring_samples = config->ring_samples;
	logic_session.bytes_per_sample = config->bytes_per_sample;
	logic_session.backend = config->backend;
	logic_session.reason = "running";
	logic_session.state = config->trigger == LINKR_DEBUGGER_LOGIC_SESSION_TRIGGER_NONE ?
		LINKR_DEBUGGER_LOGIC_SESSION_LIVE_TRIGGERED :
		LINKR_DEBUGGER_LOGIC_SESSION_ARMED_PRETRIGGER;
	logic_session.triggered = config->trigger == LINKR_DEBUGGER_LOGIC_SESSION_TRIGGER_NONE;
	logic_session.trigger_seq = UINT64_MAX;
	logic_session.eligible_seq = logic_session.triggered ? 0U : UINT64_MAX;
	return 0;
}

int linkr_debugger_logic_session_commit(uint32_t sample_count, bool trigger_seen,
	uint32_t trigger_offset)
{
	uint64_t first_seq;
	uint64_t next_writer;
	uint64_t oldest_after_write;

	if (sample_count == 0U) {
		return 0;
	}
	if (logic_session.state == LINKR_DEBUGGER_LOGIC_SESSION_IDLE ||
	    logic_session.state == LINKR_DEBUGGER_LOGIC_SESSION_ERROR ||
	    logic_session.state == LINKR_DEBUGGER_LOGIC_SESSION_STOPPING) {
		return -EINVAL;
	}
	if (sample_count > logic_session.ring_samples ||
	    (trigger_seen && trigger_offset >= sample_count)) {
		logic_session_set_error("overrun");
		return -EOVERFLOW;
	}

	first_seq = logic_session.writer_seq;
	next_writer = logic_session.writer_seq + sample_count;
	oldest_after_write = next_writer > logic_session.ring_samples ?
		next_writer - logic_session.ring_samples : 0U;

	if (logic_session.triggered && logic_session.reader_seq < oldest_after_write) {
		logic_session.writer_seq = next_writer;
		logic_session_set_error("overrun");
		return -EOVERFLOW;
	}
	if (!logic_session.triggered && trigger_seen) {
		uint64_t trigger_seq = first_seq + trigger_offset;
		uint64_t reader_seq = trigger_seq > logic_session.pre_samples ?
			trigger_seq - logic_session.pre_samples : 0U;

		if (reader_seq < oldest_after_write) {
			logic_session.writer_seq = next_writer;
			logic_session_set_error("overrun");
			return -EOVERFLOW;
		}
		logic_session.triggered = true;
		logic_session.trigger_seq = trigger_seq;
		logic_session.reader_seq = reader_seq;
		logic_session.eligible_seq = next_writer;
		logic_session.state = LINKR_DEBUGGER_LOGIC_SESSION_TRIGGER_DRAIN;
	} else if (logic_session.triggered) {
		logic_session.eligible_seq = next_writer;
		if (logic_session.state == LINKR_DEBUGGER_LOGIC_SESSION_TRIGGER_DRAIN) {
			logic_session.state = LINKR_DEBUGGER_LOGIC_SESSION_LIVE_TRIGGERED;
		}
	} else if (logic_session.pre_samples == 0U) {
		logic_session.state = LINKR_DEBUGGER_LOGIC_SESSION_LIVE_UNTRIGGERED;
	}

	logic_session.writer_seq = next_writer;
	logic_session.last_captured_seq = next_writer;
	return 0;
}

int linkr_debugger_logic_session_stop(enum linkr_debugger_logic_session_owner owner,
	uint32_t session_id, const char *reason)
{
	if (logic_session.state == LINKR_DEBUGGER_LOGIC_SESSION_IDLE) {
		return 0;
	}
	if (logic_session.owner != owner || logic_session.session_id != session_id) {
		return -EPERM;
	}

	logic_session.state = LINKR_DEBUGGER_LOGIC_SESSION_STOPPING;
	logic_session.reason = reason != NULL ? reason : "stopped";
	logic_session.terminal = true;
	logic_session.last_captured_seq = logic_session.writer_seq;
	return 0;
}

int linkr_debugger_logic_session_disconnect(enum linkr_debugger_logic_session_owner owner,
	uint32_t session_id)
{
	return linkr_debugger_logic_session_stop(owner, session_id, "disconnect");
}

int linkr_debugger_logic_session_read(uint8_t *dst, size_t dst_len, uint32_t max_samples,
	struct linkr_debugger_logic_session_chunk *chunk)
{
	uint64_t available;
	uint32_t count;
	uint32_t trigger_offset = UINT32_MAX;
	size_t needed;

	if (dst == NULL || chunk == NULL || max_samples == 0U) {
		return -EINVAL;
	}
	memset(chunk, 0, sizeof(*chunk));
	if (!logic_session.triggered || logic_session.reader_seq >= logic_session.eligible_seq) {
		return 0;
	}

	available = logic_session.eligible_seq - logic_session.reader_seq;
	count = available > max_samples ? max_samples : (uint32_t)available;
	needed = (size_t)count * logic_session.bytes_per_sample;
	if (needed > dst_len) {
		return -ENOSPC;
	}

	for (uint32_t i = 0U; i < count; i++) {
		uint64_t seq = logic_session.reader_seq + i;
		uint32_t raw = ((const uint32_t *)linkr_debugger_capture_engine_shared_arena)[
			seq % logic_session.ring_samples];

		if (logic_session.bytes_per_sample == 1U) {
			dst[i] = (uint8_t)raw;
		} else {
			dst[i * 2U] = (uint8_t)(raw & 0xffU);
			dst[(i * 2U) + 1U] = (uint8_t)((raw >> 8) & 0xffU);
		}
		if (seq == logic_session.trigger_seq) {
			trigger_offset = i;
		}
	}

	chunk->first_seq = logic_session.reader_seq;
	chunk->sample_count = count;
	chunk->trigger_offset = trigger_offset;
	chunk->has_trigger = trigger_offset != UINT32_MAX;
	chunk->encoding = logic_session_encoding(logic_session.bytes_per_sample);
	logic_session.reader_seq += count;
	logic_session.last_sent_seq = logic_session.reader_seq;
	if (logic_session.state == LINKR_DEBUGGER_LOGIC_SESSION_TRIGGER_DRAIN &&
	    logic_session.reader_seq > logic_session.trigger_seq) {
		logic_session.state = LINKR_DEBUGGER_LOGIC_SESSION_LIVE_TRIGGERED;
	}
	return 0;
}

void linkr_debugger_logic_session_get_status(
	struct linkr_debugger_logic_session_status *status)
{
	if (status == NULL) {
		return;
	}

	memset(status, 0, sizeof(*status));
	status->state = logic_session.state;
	status->owner = logic_session.owner;
	status->session_id = logic_session.session_id;
	status->reader_seq = logic_session.reader_seq;
	status->eligible_seq = logic_session.eligible_seq;
	status->writer_seq = logic_session.writer_seq;
	status->trigger_seq = logic_session.trigger_seq;
	status->last_sent_seq = logic_session.last_sent_seq;
	status->last_captured_seq = logic_session.last_captured_seq;
	status->pre_samples = logic_session.pre_samples;
	status->ring_samples = logic_session.ring_samples;
	status->bytes_per_sample = logic_session.bytes_per_sample;
	status->triggered = logic_session.triggered;
	status->terminal = logic_session.terminal;
	status->overrun = logic_session.overrun;
	status->backend = logic_session.backend;
	status->reason = logic_session.reason;
}
