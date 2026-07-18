/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_rigol.h"

#include "linkr_debugger_logic_analyzer.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/poll.h>
#include <zephyr/posix/sys/socket.h>

LOG_MODULE_REGISTER(linkr_debugger_rigol, CONFIG_LINKR_DEBUGGER_LOG_LEVEL);

#define LINKR_DEBUGGER_RIGOL_BIND_ADDR "172.29.203.1"
#define LINKR_DEBUGGER_RIGOL_HTTP_UPSTREAM_ADDR "127.0.0.1"
#define LINKR_DEBUGGER_RIGOL_HTTP_UPSTREAM_PORT 8080U
#define LINKR_DEBUGGER_RIGOL_PUMP_SLOTS 4U
#define LINKR_DEBUGGER_RIGOL_PUMP_BUF 1024U
#define LINKR_DEBUGGER_RIGOL_RECV_TIMEOUT_MS 2000U
#define LINKR_DEBUGGER_RIGOL_IDN "Rigol Technologies,DS1102D,DS1ZA999000001,00.04.04"
#define LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES 600U
#define LINKR_DEBUGGER_RIGOL_LIVE_FRAME_BYTES (LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES * 2U)
#define LINKR_DEBUGGER_RIGOL_HORIZONTAL_DIVS 12U
#define LINKR_DEBUGGER_RIGOL_MIN_TIMEBASE_S 0.000000002
#define LINKR_DEBUGGER_RIGOL_TRIG_WAIT_MS 30000U
#define LINKR_DEBUGGER_RIGOL_ARM_TIMEOUT_MS 10000U
#define LINKR_DEBUGGER_RIGOL_GP29_ADC_CAP_HZ 10000U

static const struct adc_dt_spec linkr_debugger_rigol_adc_gp29 =
	ADC_DT_SPEC_STRUCT(DT_NODELABEL(adc), 3);

static const uint8_t linkr_debugger_rigol_pins[15] = {
	7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 29,
};

static uint16_t linkr_debugger_rigol_buf[LINKR_DEBUGGER_RIGOL_BUFFER_SAMPLES];
static uint8_t linkr_debugger_rigol_analog_buf[LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES];

struct linkr_debugger_rigol_state {
	double timebase;
	uint32_t trigger_source;
	enum linkr_debugger_la_trigger_type trigger_slope;
	uint32_t digital_enabled;
	bool armed;
	bool sweep_single;
	bool frame_ready;
	bool capture_pending;
	bool capture_done;
	bool want_arm;
	int64_t arm_deadline;
	uint32_t capture_rate;
};

struct linkr_debugger_rigol_pump_pair {
	int client_fd;
	int upstream_fd;
	int pending_first;
};

struct linkr_debugger_rigol_fifo_node {
	void *fifo_reserved;
	void *data;
	void *first;
};

static struct linkr_debugger_rigol_pump_pair linkr_debugger_rigol_pump_pairs[
	LINKR_DEBUGGER_RIGOL_PUMP_SLOTS];
static struct linkr_debugger_rigol_fifo_node linkr_debugger_rigol_fifo_nodes[
	LINKR_DEBUGGER_RIGOL_PUMP_SLOTS];
static K_FIFO_DEFINE(linkr_debugger_rigol_pump_fifo);

static bool linkr_debugger_rigol_first_byte_is_http(uint8_t byte)
{
	return byte >= 'A' && byte <= 'Z';
}

static void linkr_debugger_rigol_close_fd(int *fd)
{
	if (*fd >= 0) {
		(void)zsock_close(*fd);
		*fd = -1;
	}
}

static int linkr_debugger_rigol_send_all(int fd, const void *data, size_t len)
{
	const uint8_t *ptr = data;

	while (len > 0U) {
		ssize_t ret = zsock_send(fd, ptr, len, 0);

		if (ret < 0) {
			return -1;
		}
		if (ret == 0) {
			k_msleep(5);
			continue;
		}
		ptr += (size_t)ret;
		len -= (size_t)ret;
	}
	return 0;
}

static int linkr_debugger_rigol_send_str(int fd, const char *str)
{
	return linkr_debugger_rigol_send_all(fd, str, strlen(str));
}

static int linkr_debugger_rigol_format_double(char *buf, size_t cap, double value)
{
	uint64_t scaled;

	if (value < 0.0) {
		value = -value;
	}
	scaled = (uint64_t)(value * 1000000.0 + 0.5);
	return snprintk(buf, cap, "%u.%06u",
		(unsigned)(scaled / 1000000U), (unsigned)(scaled % 1000000U));
}

static void linkr_debugger_rigol_pump_pair_close(size_t index)
{
	struct linkr_debugger_rigol_pump_pair *pair = &linkr_debugger_rigol_pump_pairs[index];

	linkr_debugger_rigol_close_fd(&pair->client_fd);
	linkr_debugger_rigol_close_fd(&pair->upstream_fd);
}

static int linkr_debugger_rigol_pump_pair_open(int client_fd, int pending_first)
{
	size_t free_slot = LINKR_DEBUGGER_RIGOL_PUMP_SLOTS;
	int upstream_fd;
	struct sockaddr_in addr;

	for (size_t i = 0U; i < LINKR_DEBUGGER_RIGOL_PUMP_SLOTS; i++) {
		if (linkr_debugger_rigol_pump_pairs[i].client_fd < 0) {
			free_slot = i;
			break;
		}
	}
	if (free_slot == LINKR_DEBUGGER_RIGOL_PUMP_SLOTS) {
		return -1;
	}

	upstream_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (upstream_fd < 0) {
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(LINKR_DEBUGGER_RIGOL_HTTP_UPSTREAM_PORT);
	(void)zsock_inet_pton(NET_AF_INET, LINKR_DEBUGGER_RIGOL_HTTP_UPSTREAM_ADDR, &addr.sin_addr);
	if (zsock_connect(upstream_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		linkr_debugger_rigol_close_fd(&upstream_fd);
		return -1;
	}

	if (pending_first >= 0) {
		uint8_t first = (uint8_t)pending_first;

		if (linkr_debugger_rigol_send_all(upstream_fd, &first, 1U) < 0) {
			linkr_debugger_rigol_close_fd(&upstream_fd);
			return -1;
		}
	}

	linkr_debugger_rigol_pump_pairs[free_slot].client_fd = client_fd;
	linkr_debugger_rigol_pump_pairs[free_slot].upstream_fd = upstream_fd;
	linkr_debugger_rigol_pump_pairs[free_slot].pending_first = -1;
	return 0;
}

static void linkr_debugger_rigol_pump_thread(void *p1, void *p2, void *p3)
{
	struct zsock_pollfd fds[LINKR_DEBUGGER_RIGOL_PUMP_SLOTS * 2U];
	uint8_t buf[LINKR_DEBUGGER_RIGOL_PUMP_BUF];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (size_t i = 0U; i < LINKR_DEBUGGER_RIGOL_PUMP_SLOTS; i++) {
		linkr_debugger_rigol_pump_pairs[i].client_fd = -1;
		linkr_debugger_rigol_pump_pairs[i].upstream_fd = -1;
		linkr_debugger_rigol_pump_pairs[i].pending_first = -1;
	}

	while (true) {
		int accepted = -1;
		int pending_first = -1;
		struct linkr_debugger_rigol_fifo_node *incoming =
			k_fifo_get(&linkr_debugger_rigol_pump_fifo, K_NO_WAIT);

		if (incoming != NULL) {
			accepted = (int)(intptr_t)incoming->data;
			pending_first = (int)(intptr_t)incoming->first;
			incoming->data = NULL;
			incoming->first = NULL;
		}
		if (accepted >= 0 &&
		    linkr_debugger_rigol_pump_pair_open(accepted, pending_first) < 0) {
			linkr_debugger_rigol_close_fd(&accepted);
		}

		size_t nfds = 0U;
		size_t fd_to_pair[LINKR_DEBUGGER_RIGOL_PUMP_SLOTS * 2U];

		for (size_t i = 0U; i < LINKR_DEBUGGER_RIGOL_PUMP_SLOTS; i++) {
			if (linkr_debugger_rigol_pump_pairs[i].client_fd < 0) {
				continue;
			}
			fds[nfds].fd = linkr_debugger_rigol_pump_pairs[i].client_fd;
			fds[nfds].events = ZSOCK_POLLIN;
			fds[nfds].revents = 0U;
			fd_to_pair[nfds] = i;
			nfds++;
			fds[nfds].fd = linkr_debugger_rigol_pump_pairs[i].upstream_fd;
			fds[nfds].events = ZSOCK_POLLIN;
			fds[nfds].revents = 0U;
			fd_to_pair[nfds] = i;
			nfds++;
		}

		if (nfds == 0U) {
			k_msleep(50);
			continue;
		}

		int pret = zsock_poll(fds, nfds, 200);

		if (pret <= 0) {
			continue;
		}

		for (size_t i = 0U; i < nfds; i++) {
			size_t pair_index = fd_to_pair[i];
			struct linkr_debugger_rigol_pump_pair *pair =
				&linkr_debugger_rigol_pump_pairs[pair_index];
			int src = fds[i].fd;
			int dst = (src == pair->client_fd) ? pair->upstream_fd : pair->client_fd;

			if ((fds[i].revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) != 0) {
				linkr_debugger_rigol_pump_pair_close(pair_index);
				continue;
			}
			if ((fds[i].revents & ZSOCK_POLLIN) == 0) {
				continue;
			}

			ssize_t got = zsock_recv(src, buf, sizeof(buf), 0);

			if (got <= 0) {
				linkr_debugger_rigol_pump_pair_close(pair_index);
				continue;
			}
			if (linkr_debugger_rigol_send_all(dst, buf, (size_t)got) < 0) {
				linkr_debugger_rigol_pump_pair_close(pair_index);
			}
		}
	}
}

static bool linkr_debugger_rigol_read_line(int fd, char *line, size_t cap,
	size_t *len, bool *have_first, uint8_t *first)
{
	size_t used = 0U;

	if (*have_first) {
		line[used++] = (char)*first;
		*have_first = false;
	}

	while (used + 1U < cap) {
		uint8_t ch;
		ssize_t ret = zsock_recv(fd, &ch, 1U, 0);

		if (ret <= 0) {
			return false;
		}
		if (ch == '\n') {
			break;
		}
		if (ch != '\r') {
			line[used++] = (char)ch;
		}
	}
	line[used] = '\0';
	*len = used;
	return true;
}

static int linkr_debugger_rigol_read_gp29_raw(void)
{
	int16_t raw = 0;
	struct adc_sequence sequence = { 0 };

	(void)adc_sequence_init_dt(&linkr_debugger_rigol_adc_gp29, &sequence);
	sequence.buffer = &raw;
	sequence.buffer_size = sizeof(raw);
	if (adc_read_dt(&linkr_debugger_rigol_adc_gp29, &sequence) < 0) {
		return -1;
	}
	return (int)raw;
}

static uint8_t linkr_debugger_rigol_gp29_to_scope(int raw)
{
	int value = 128 - (raw * 2) / 97;

	if (raw < 0) {
		raw = 0;
	}
	if (value < 0) {
		value = 0;
	}
	if (value > 255) {
		value = 255;
	}
	return (uint8_t)value;
}

static int linkr_debugger_rigol_produce_analog_frame(int fd, uint32_t rate_hz)
{
	uint32_t pace_us = rate_hz > 0U && rate_hz <= LINKR_DEBUGGER_RIGOL_GP29_ADC_CAP_HZ ?
		1000000U / rate_hz : 0U;
	uint8_t header[16];
	int header_len;

	for (uint32_t i = 0U; i < LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES; i++) {
		int raw = linkr_debugger_rigol_read_gp29_raw();

		linkr_debugger_rigol_analog_buf[i] = linkr_debugger_rigol_gp29_to_scope(raw);
		if (pace_us > 0U) {
			k_busy_wait(pace_us);
		}
	}

	header_len = snprintk(header, sizeof(header), "#3%u",
		(unsigned)LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES);
	if (header_len <= 0 ||
	    linkr_debugger_rigol_send_all(fd, header, (size_t)header_len) < 0) {
		return -1;
	}
	return linkr_debugger_rigol_send_all(fd, linkr_debugger_rigol_analog_buf,
		LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES);
}

static uint32_t linkr_debugger_rigol_rate_from_timebase(double timebase)
{
	double rate = (double)LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES /
		((double)LINKR_DEBUGGER_RIGOL_HORIZONTAL_DIVS * timebase);

	if (rate > (double)LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ) {
		rate = LINKR_DEBUGGER_LA_MAX_SAMPLE_RATE_HZ;
	}
	if (rate < (double)LINKR_DEBUGGER_LA_MIN_SAMPLE_RATE_HZ) {
		rate = LINKR_DEBUGGER_LA_MIN_SAMPLE_RATE_HZ;
	}
	return (uint32_t)(rate + 0.5);
}

static void linkr_debugger_rigol_fill_la_config(struct linkr_debugger_la_config *la,
	uint32_t rate_hz)
{
	memset(la, 0, sizeof(*la));
	for (uint8_t i = 0U; i < 15U; i++) {
		la->selected_pins[i] = linkr_debugger_rigol_pins[i];
	}
	la->selected_pin_count = 15U;
	la->pin_count = 15U;
	la->pin_base = la->selected_pins[0];
	la->sample_rate_hz = rate_hz;
	la->post_samples = LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES;
}

static uint32_t linkr_debugger_rigol_capture_contiguous(uint32_t rate_hz,
	uint32_t count);
static void linkr_debugger_rigol_align_frame(uint32_t *io_count, uint32_t want,
	int edge_idx);

static int64_t linkr_debugger_rigol_auto_timeout_ms(uint32_t rate_hz)
{
	uint32_t t = 2000U;

	if (rate_hz > 0U) {
		t = 2U * 1000U * LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES / rate_hz + 100U;
	}
	if (t < 100U) {
		t = 100U;
	}
	if (t > 2000U) {
		t = 2000U;
	}
	return (int64_t)t;
}

static int linkr_debugger_rigol_arm_async(struct linkr_debugger_rigol_state *state,
	uint32_t rate_hz)
{
	struct linkr_debugger_la_config la;
	int ret;

	if (state->trigger_source >= 15U ||
	    rate_hz > LINKR_DEBUGGER_LA_MAX_STREAM_RATE_HZ) {
		return -ENOTSUP;
	}
	linkr_debugger_rigol_fill_la_config(&la, rate_hz);
	la.trigger = state->trigger_slope;
	la.trigger_pin = state->trigger_source;
	la.pre_samples = LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES / 2U;
	la.post_samples = LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES - la.pre_samples;

	ret = linkr_debugger_logic_analyzer_arm(&la);
	if (ret < 0) {
		LOG_WRN("rigol: async arm failed %d (src %u, rate %u)", ret,
			(unsigned)state->trigger_source, (unsigned)rate_hz);
		return ret;
	}
	state->capture_pending = true;
	state->capture_done = false;
	state->capture_rate = rate_hz;
	state->arm_deadline = k_uptime_get() +
		linkr_debugger_rigol_auto_timeout_ms(rate_hz);
	return 0;
}

static int linkr_debugger_rigol_poll_capture(struct linkr_debugger_rigol_state *state)
{
	static struct linkr_debugger_la_sample snapshot[LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES];
	struct linkr_debugger_la_capture capture;
	enum linkr_debugger_la_state la_state;
	uint32_t real;
	uint16_t last;
	int ret;

	if (!state->capture_pending) {
		return state->capture_done ? 1 : 0;
	}
	la_state = linkr_debugger_logic_analyzer_get_state();
	if (la_state == LINKR_DEBUGGER_LA_STATE_CAPTURING) {
		return 0;
	}
	if (la_state == LINKR_DEBUGGER_LA_STATE_ARMED) {
		uint32_t count;

		if (k_uptime_get() < state->arm_deadline) {
			return 0;
		}
		(void)linkr_debugger_logic_analyzer_cancel();
		state->capture_pending = false;
		count = linkr_debugger_rigol_capture_contiguous(state->capture_rate,
			LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES);
		if (count == 0U) {
			return -1;
		}
		linkr_debugger_rigol_align_frame(&count,
			LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES, -1);
		state->capture_done = true;
		return 1;
	}
	state->capture_pending = false;
	if (la_state != LINKR_DEBUGGER_LA_STATE_DONE) {
		(void)linkr_debugger_logic_analyzer_cancel();
		LOG_WRN("rigol: async capture failed, la state %d", (int)la_state);
		return -1;
	}
	capture.samples = snapshot;
	ret = linkr_debugger_logic_analyzer_get_capture(&capture, snapshot,
		LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES);
	if (ret < 0) {
		return -1;
	}
	real = capture.sample_count;
	for (uint32_t i = 0U; i < real; i++) {
		linkr_debugger_rigol_buf[i] = snapshot[i].values;
	}
	linkr_debugger_logic_analyzer_release();
	last = real > 0U ? linkr_debugger_rigol_buf[real - 1U] : 0U;
	for (uint32_t i = real; i < LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES; i++) {
		linkr_debugger_rigol_buf[i] = last;
	}
	state->capture_done = true;
	return 1;
}

struct linkr_debugger_rigol_stream_sink {
	uint32_t count;
	uint32_t wanted;
};

static void linkr_debugger_rigol_stream_callback(
	const struct linkr_debugger_la_stream_chunk *chunk, void *user_data)
{
	struct linkr_debugger_rigol_stream_sink *sink = user_data;
	uint32_t room = LINKR_DEBUGGER_RIGOL_BUFFER_SAMPLES - sink->count;
	uint32_t n = chunk->sample_count < room ? chunk->sample_count : room;

	memcpy(&linkr_debugger_rigol_buf[sink->count], chunk->values, n * sizeof(uint16_t));
	sink->count += n;
}

static uint32_t linkr_debugger_rigol_capture_burst(uint32_t rate_hz, uint32_t count)
{
	static struct linkr_debugger_la_sample snapshot[LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES];
	struct linkr_debugger_la_config la;
	struct linkr_debugger_la_capture capture;
	enum linkr_debugger_la_state la_state;
	uint32_t real;
	int64_t deadline;
	int ret;

	linkr_debugger_rigol_fill_la_config(&la, rate_hz);
	la.post_samples = count < LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES ?
		count : LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES;

	ret = linkr_debugger_logic_analyzer_arm(&la);
	if (ret < 0) {
		LOG_WRN("rigol: burst arm failed %d (rate %u)", ret, (unsigned)rate_hz);
		return 0U;
	}
	deadline = k_uptime_get() + LINKR_DEBUGGER_RIGOL_ARM_TIMEOUT_MS;
	la_state = linkr_debugger_logic_analyzer_get_state();
	while (la_state != LINKR_DEBUGGER_LA_STATE_DONE &&
	       la_state != LINKR_DEBUGGER_LA_STATE_ERROR &&
	       k_uptime_get() < deadline) {
		k_msleep(5);
		la_state = linkr_debugger_logic_analyzer_get_state();
	}
	if (la_state != LINKR_DEBUGGER_LA_STATE_DONE) {
		(void)linkr_debugger_logic_analyzer_cancel();
		return 0U;
	}
	capture.samples = snapshot;
	ret = linkr_debugger_logic_analyzer_get_capture(&capture, snapshot,
		LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES);
	if (ret < 0) {
		return 0U;
	}
	real = capture.sample_count;
	for (uint32_t i = 0U; i < real; i++) {
		linkr_debugger_rigol_buf[i] = snapshot[i].values;
	}
	linkr_debugger_logic_analyzer_release();
	return real;
}

static uint32_t linkr_debugger_rigol_capture_contiguous(uint32_t rate_hz, uint32_t count)
{
	struct linkr_debugger_la_config la;
	struct linkr_debugger_rigol_stream_sink sink = { 0 };
	int ret;

	if (rate_hz > LINKR_DEBUGGER_LA_MAX_STREAM_RATE_HZ) {
		return linkr_debugger_rigol_capture_burst(rate_hz, count);
	}

	if (count > LINKR_DEBUGGER_RIGOL_BUFFER_SAMPLES) {
		count = LINKR_DEBUGGER_RIGOL_BUFFER_SAMPLES;
	}
	sink.wanted = count;

	linkr_debugger_rigol_fill_la_config(&la, rate_hz);
	ret = linkr_debugger_logic_analyzer_start_stream(&la,
		linkr_debugger_rigol_stream_callback, &sink);
	if (ret < 0) {
		LOG_WRN("rigol: start_stream failed %d (rate %u)", ret, (unsigned)rate_hz);
		return 0U;
	}

	int64_t deadline = k_uptime_get() + LINKR_DEBUGGER_RIGOL_ARM_TIMEOUT_MS;

	while (sink.count < count && k_uptime_get() < deadline) {
		k_msleep(2);
	}
	(void)linkr_debugger_logic_analyzer_stop_stream();

	return sink.count;
}

static int linkr_debugger_rigol_find_edge(uint32_t count, uint8_t channel,
	enum linkr_debugger_la_trigger_type slope)
{
	uint16_t mask = (uint16_t)(1U << channel);
	uint16_t prev = linkr_debugger_rigol_buf[0] & mask;

	for (uint32_t i = 1U; i < count; i++) {
		uint16_t level = linkr_debugger_rigol_buf[i] & mask;
		bool rising = prev == 0U && level != 0U;
		bool falling = prev != 0U && level == 0U;
		bool match = slope == LINKR_DEBUGGER_LA_TRIGGER_RISING ? rising :
			slope == LINKR_DEBUGGER_LA_TRIGGER_FALLING ? falling :
			rising || falling;

		if (match) {
			return (int)i;
		}
		prev = level;
	}
	return -1;
}

static void linkr_debugger_rigol_align_frame(uint32_t *io_count, uint32_t want,
	int edge_idx)
{
	uint32_t count = *io_count;
	uint32_t pre = want / 2U;
	uint32_t start = 0U;

	if (edge_idx >= 0 && (uint32_t)edge_idx > pre) {
		start = (uint32_t)edge_idx - pre;
		if (start + want > count) {
			start = count > want ? count - want : 0U;
		}
	}
	if (start > 0U) {
		memmove(linkr_debugger_rigol_buf, &linkr_debugger_rigol_buf[start],
			(count - start) * sizeof(uint16_t));
		count -= start;
	} else if (edge_idx >= 0 && (uint32_t)edge_idx < pre) {
		uint32_t shift = pre - (uint32_t)edge_idx;
		uint16_t first = linkr_debugger_rigol_buf[0];
		uint32_t keep = count > want - shift ? want - shift : count;

		memmove(&linkr_debugger_rigol_buf[shift], linkr_debugger_rigol_buf,
			keep * sizeof(uint16_t));
		for (uint32_t i = 0U; i < shift; i++) {
			linkr_debugger_rigol_buf[i] = first;
		}
		count = keep + shift;
	}
	if (count < want) {
		uint16_t last = count > 0U ? linkr_debugger_rigol_buf[count - 1U] : 0U;

		for (uint32_t i = count; i < want; i++) {
			linkr_debugger_rigol_buf[i] = last;
		}
		count = want;
	}
	*io_count = count;
}

static int linkr_debugger_rigol_send_frame(int fd, const uint16_t *samples)
{
	uint8_t header[16];
	int header_len = snprintk(header, sizeof(header), "#4%u",
		(unsigned)LINKR_DEBUGGER_RIGOL_LIVE_FRAME_BYTES);

	if (header_len <= 0 ||
	    linkr_debugger_rigol_send_all(fd, header, (size_t)header_len) < 0) {
		return -1;
	}
	return linkr_debugger_rigol_send_all(fd, samples,
		LINKR_DEBUGGER_RIGOL_LIVE_FRAME_BYTES);
}

static int linkr_debugger_rigol_capture_triggered(
	struct linkr_debugger_rigol_state *state, uint32_t rate_hz)
{
	int64_t deadline;
	int p;

	if (rate_hz > LINKR_DEBUGGER_LA_MAX_STREAM_RATE_HZ) {
		uint32_t count = linkr_debugger_rigol_capture_contiguous(rate_hz,
			LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES);
		int edge_idx;

		if (count == 0U) {
			return -1;
		}
		edge_idx = linkr_debugger_rigol_find_edge(count,
			(uint8_t)state->trigger_source, state->trigger_slope);
		linkr_debugger_rigol_align_frame(&count,
			LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES, edge_idx);
		return (int)count;
	}

	if (linkr_debugger_rigol_arm_async(state, rate_hz) < 0) {
		return -1;
	}
	deadline = k_uptime_get() + LINKR_DEBUGGER_RIGOL_ARM_TIMEOUT_MS;
	p = linkr_debugger_rigol_poll_capture(state);
	while (p == 0 && k_uptime_get() < deadline) {
		k_msleep(10);
		p = linkr_debugger_rigol_poll_capture(state);
	}
	if (p != 1) {
		if (state->capture_pending) {
			(void)linkr_debugger_logic_analyzer_cancel();
			state->capture_pending = false;
		}
		LOG_WRN("rigol: trigger timeout");
		return -1;
	}
	return (int)LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES;
}

static int linkr_debugger_rigol_produce_frame(int fd,
	struct linkr_debugger_rigol_state *state, bool use_trigger)
{
	uint32_t rate_hz = linkr_debugger_rigol_rate_from_timebase(state->timebase);
	int count;
	int ret;

	use_trigger = use_trigger && state->trigger_source < 15U;
	if (use_trigger) {
		if (!state->capture_done) {
			if (state->capture_pending) {
				int64_t deadline = k_uptime_get() +
					LINKR_DEBUGGER_RIGOL_ARM_TIMEOUT_MS;
				int p = linkr_debugger_rigol_poll_capture(state);

				while (p == 0 && k_uptime_get() < deadline) {
					k_msleep(10);
					p = linkr_debugger_rigol_poll_capture(state);
				}
				if (p != 1) {
					if (state->capture_pending) {
						(void)linkr_debugger_logic_analyzer_cancel();
						state->capture_pending = false;
					}
					return -1;
				}
			} else if (linkr_debugger_rigol_capture_triggered(state,
				rate_hz) < 0) {
				return -1;
			}
		}
		state->capture_done = false;
		ret = linkr_debugger_rigol_send_frame(fd, linkr_debugger_rigol_buf);
		if (state->armed) {
			state->want_arm = true;
		}
		return ret;
	}

	count = (int)linkr_debugger_rigol_capture_contiguous(rate_hz,
		LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES);
	if (count == 0) {
		return -1;
	}
	linkr_debugger_rigol_align_frame((uint32_t *)&count,
		LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES, -1);

	return linkr_debugger_rigol_send_frame(fd, linkr_debugger_rigol_buf);
}

static const char *linkr_debugger_rigol_trig_source_name(uint32_t source)
{
	switch (source) {
	case 16U: return "CH1";
	case 17U: return "CH2";
	case 18U: return "EXT";
	case 19U: return "ACL";
	default: break;
	}
	return NULL;
}

static int linkr_debugger_rigol_handle_query(int fd, const char *line,
	struct linkr_debugger_rigol_state *state)
{
	char resp[64];

	if (strcmp(line, "*IDN?") == 0) {
		return linkr_debugger_rigol_send_str(fd, LINKR_DEBUGGER_RIGOL_IDN "\n");
	}
	if (strcmp(line, "*ESR?") == 0) {
		return linkr_debugger_rigol_send_str(fd, "0\n");
	}
	if (strcmp(line, "*OPC?") == 0) {
		return linkr_debugger_rigol_send_str(fd, "1\n");
	}
	if (strcmp(line, ":CHAN1:DISP?") == 0) {
		return linkr_debugger_rigol_send_str(fd, "1\n");
	}
	if (strcmp(line, ":CHAN2:DISP?") == 0) {
		return linkr_debugger_rigol_send_str(fd, "0\n");
	}
	if (strncmp(line, ":CHAN", 5) == 0 && strstr(line, ":PROB?") != NULL) {
		return linkr_debugger_rigol_send_str(fd, "1X\n");
	}
	if (strncmp(line, ":CHAN", 5) == 0 && strstr(line, ":SCAL?") != NULL) {
		return linkr_debugger_rigol_send_str(fd, "1.0\n");
	}
	if (strncmp(line, ":CHAN", 5) == 0 && strstr(line, ":OFFS?") != NULL) {
		return linkr_debugger_rigol_send_str(fd, "0.0\n");
	}
	if (strncmp(line, ":CHAN", 5) == 0 && strstr(line, ":COUP?") != NULL) {
		return linkr_debugger_rigol_send_str(fd, "DC\n");
	}
	if (strcmp(line, ":TIM:SCAL?") == 0) {
		int len = linkr_debugger_rigol_format_double(resp, sizeof(resp),
			state->timebase);

		if (len <= 0) {
			return -1;
		}
		resp[len++] = '\n';
		return linkr_debugger_rigol_send_all(fd, resp, (size_t)len);
	}
	if (strcmp(line, ":TIM:OFFS?") == 0) {
		return linkr_debugger_rigol_send_str(fd, "0.0\n");
	}
	if (strcmp(line, ":LA:DISP?") == 0 || strcmp(line, ":LA:STAT?") == 0) {
		return linkr_debugger_rigol_send_str(fd, "1\n");
	}
	if (strncmp(line, ":DIG", 4) == 0 && strstr(line, ":TURN?") != NULL) {
		uint32_t ch = (uint32_t)(line[3] >= '0' && line[3] <= '9' ? line[3] - '0' : 0U);
		uint32_t idx = ch;
		const char *num = line + 3;

		if (*num >= '0' && *num <= '9') {
			num++;
			if (*num >= '0' && *num <= '9') {
				idx = idx * 10U + (uint32_t)(*num - '0');
			}
		}
		bool enabled = idx < 15U && (state->digital_enabled & (1U << idx)) != 0U;

		return linkr_debugger_rigol_send_str(fd, enabled ? "1\n" : "0\n");
	}
	if (strcmp(line, ":TRIG:EDGE:SOUR?") == 0) {
		const char *name = linkr_debugger_rigol_trig_source_name(state->trigger_source);
		int len;

		if (name == NULL) {
			len = snprintk(resp, sizeof(resp), "D%u\n", (unsigned)state->trigger_source);
		} else {
			len = snprintk(resp, sizeof(resp), "%s\n", name);
		}
		if (len <= 0) {
			return -1;
		}
		return linkr_debugger_rigol_send_all(fd, resp, (size_t)len);
	}
	if (strcmp(line, ":TRIG:EDGE:SLOP?") == 0) {
		return linkr_debugger_rigol_send_str(fd,
			state->trigger_slope == LINKR_DEBUGGER_LA_TRIGGER_FALLING ? "NEG\n" : "POS\n");
	}
	if (strcmp(line, ":TRIG:EDGE:LEV?") == 0) {
		return linkr_debugger_rigol_send_str(fd, "0.0\n");
	}
	if (strcmp(line, ":TRIG:MODE?") == 0) {
		return linkr_debugger_rigol_send_str(fd, "EDGE\n");
	}
	if (strcmp(line, ":TRIG:STAT?") == 0) {
		if (state->armed && state->want_arm && !state->capture_pending &&
		    !state->capture_done) {
			uint32_t rate_hz = linkr_debugger_rigol_rate_from_timebase(
				state->timebase);

			state->want_arm = false;
			if (linkr_debugger_rigol_arm_async(state, rate_hz) < 0) {
				state->frame_ready = true;
			}
		}
		if (state->capture_pending &&
		    linkr_debugger_rigol_poll_capture(state) == 0) {
			return linkr_debugger_rigol_send_str(fd, "RUN\n");
		}
		if (state->frame_ready || state->capture_done) {
			return linkr_debugger_rigol_send_str(fd,
				state->sweep_single ? "STOP\n" : "TD\n");
		}
		if (state->armed) {
			return linkr_debugger_rigol_send_str(fd, "RUN\n");
		}
		return linkr_debugger_rigol_send_str(fd, "TD\n");
	}
	if (strcmp(line, ":ACQ:MDEP?") == 0) {
		return linkr_debugger_rigol_send_str(fd, "600\n");
	}
	if (strcmp(line, ":WAV:XINC?") == 0) {
		uint32_t rate_hz = linkr_debugger_rigol_rate_from_timebase(state->timebase);
		double xinc = rate_hz > 0U ? 1.0 / (double)rate_hz : 1e-6;
		int len = linkr_debugger_rigol_format_double(resp, sizeof(resp), xinc);

		if (len <= 0) {
			return -1;
		}
		resp[len++] = '\n';
		return linkr_debugger_rigol_send_all(fd, resp, (size_t)len);
	}
	if (strcmp(line, ":WAV:DATA? CHAN1") == 0) {
		uint32_t rate_hz = linkr_debugger_rigol_rate_from_timebase(state->timebase);

		return linkr_debugger_rigol_produce_analog_frame(fd, rate_hz);
	}
	if (strcmp(line, ":WAV:DATA? DIG") == 0) {
		return linkr_debugger_rigol_produce_frame(fd, state, state->armed);
	}
	return 1;
}

static void linkr_debugger_rigol_handle_set(const char *line,
	struct linkr_debugger_rigol_state *state)
{
	if (strncmp(line, ":TIM:SCAL ", 10) == 0) {
		double value = 0.0;
		const char *p = line + 10;
		double frac = 0.0, scale = 0.1;
		bool neg = false;

		if (*p == '-') {
			neg = true;
			p++;
		}
		while (*p >= '0' && *p <= '9') {
			value = value * 10.0 + (double)(*p - '0');
			p++;
		}
		if (*p == '.') {
			p++;
			while (*p >= '0' && *p <= '9') {
				frac += scale * (double)(*p - '0');
				scale /= 10.0;
				p++;
			}
		}
		if (*p == 'e' || *p == 'E') {
			p++;
			bool eneg = false;
			int ev = 0;

			if (*p == '-') {
				eneg = true;
				p++;
			} else if (*p == '+') {
				p++;
			}
			while (*p >= '0' && *p <= '9') {
				ev = ev * 10 + (*p - '0');
				p++;
			}
			while (ev-- > 0) {
				value *= 10.0;
			}
			while (ev++ < 0) {
				/* not reached */
			}
			if (eneg) {
				int count = -ev - 1;
				while (count-- > 0) {
					value /= 10.0;
				}
			}
		}
		value += frac;
		if (neg) {
			value = -value;
		}
		if (value > 0.0) {
			state->timebase = value;
		}
		return;
	}
	if (strncmp(line, ":TRIG:EDGE:SOUR ", 16) == 0) {
		const char *p = line + 16;

		if (strcmp(p, "CH1") == 0 || strcmp(p, "CHAN1") == 0) {
			state->trigger_source = 16U;
		} else if (strcmp(p, "CH2") == 0 || strcmp(p, "CHAN2") == 0) {
			state->trigger_source = 17U;
		} else if (strcmp(p, "EXT") == 0) {
			state->trigger_source = 18U;
		} else if (strcmp(p, "ACL") == 0) {
			state->trigger_source = 19U;
		} else if (p[0] == 'D') {
			uint32_t idx = 0U;

			p++;
			while (*p >= '0' && *p <= '9') {
				idx = idx * 10U + (uint32_t)(*p - '0');
				p++;
			}
			if (idx < 15U) {
				state->trigger_source = idx;
			}
		}
		return;
	}
	if (strncmp(line, ":TRIG:EDGE:SLOP ", 16) == 0) {
		state->trigger_slope = strncmp(line + 16, "NEG", 3) == 0 ?
			LINKR_DEBUGGER_LA_TRIGGER_FALLING : LINKR_DEBUGGER_LA_TRIGGER_RISING;
		return;
	}
	if (strncmp(line, ":DIG", 4) == 0 && strstr(line, ":TURN ") != NULL) {
		const char *p = line + 4;
		uint32_t idx = 0U;

		while (*p >= '0' && *p <= '9') {
			idx = idx * 10U + (uint32_t)(*p - '0');
			p++;
		}
		if (idx < 15U) {
			if (strstr(line, "ON") != NULL) {
				state->digital_enabled |= 1U << idx;
			} else {
				state->digital_enabled &= ~(1U << idx);
			}
		}
		return;
	}
	if (strcmp(line, ":RUN") == 0) {
		state->armed = true;
		state->frame_ready = false;
		state->capture_done = false;
		if (state->capture_pending) {
			(void)linkr_debugger_logic_analyzer_cancel();
			state->capture_pending = false;
		}
		state->want_arm = true;
		return;
	}
	if (strcmp(line, ":SINGL") == 0 || strstr(line, ":SWE SING") != NULL) {
		state->sweep_single = true;
		state->armed = true;
		state->frame_ready = false;
		state->capture_done = false;
		if (state->capture_pending) {
			(void)linkr_debugger_logic_analyzer_cancel();
			state->capture_pending = false;
		}
		state->want_arm = true;
		return;
	}
	if (strcmp(line, ":STOP") == 0) {
		state->armed = false;
		state->sweep_single = false;
		state->want_arm = false;
		if (state->capture_pending) {
			(void)linkr_debugger_logic_analyzer_cancel();
			state->capture_pending = false;
		}
		state->capture_done = false;
		return;
	}
	if (strcmp(line, "*RST") == 0) {
		state->armed = false;
		state->sweep_single = false;
		state->frame_ready = true;
		state->want_arm = false;
		state->timebase = 0.0001;
		state->trigger_slope = LINKR_DEBUGGER_LA_TRIGGER_RISING;
		state->trigger_source = 0U;
		state->digital_enabled = 0xffffU;
		if (state->capture_pending) {
			(void)linkr_debugger_logic_analyzer_cancel();
			state->capture_pending = false;
		}
		state->capture_done = false;
		return;
	}
}

static void linkr_debugger_rigol_session(int fd, uint8_t first)
{
	struct linkr_debugger_rigol_state state = {
		.timebase = 0.0001,
		.trigger_source = 0U,
		.trigger_slope = LINKR_DEBUGGER_LA_TRIGGER_RISING,
		.digital_enabled = 0xffffU,
		.armed = false,
		.sweep_single = false,
		.frame_ready = true,
		.capture_pending = false,
		.capture_done = false,
		.want_arm = false,
	};
	struct zsock_timeval tv;
	bool have_first = true;

	tv.tv_sec = LINKR_DEBUGGER_RIGOL_RECV_TIMEOUT_MS / 1000U;
	tv.tv_usec = 0;
	(void)zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	while (true) {
		char line[128];
		size_t len = 0U;

		if (!linkr_debugger_rigol_read_line(fd, line, sizeof(line), &len,
			&have_first, &first)) {
			break;
		}
		if (len == 0U) {
			continue;
		}

		int qret = linkr_debugger_rigol_handle_query(fd, line, &state);

		if (qret < 0) {
			break;
		}
		if (qret > 0) {
			linkr_debugger_rigol_handle_set(line, &state);
		}

		if (state.armed && !state.frame_ready && !state.capture_pending &&
		    !state.capture_done && !state.want_arm) {
			state.frame_ready = true;
		}
	}
	if (state.capture_pending) {
		(void)linkr_debugger_logic_analyzer_cancel();
	}
}

static void linkr_debugger_rigol_dispatch_thread(void *p1, void *p2, void *p3)
{
	struct sockaddr_in addr;
	int listen_fd = -1;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (listen_fd < 0) {
		listen_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listen_fd < 0) {
			k_msleep(500);
			continue;
		}
		int reuse = 1;

		(void)zsock_setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
			&reuse, sizeof(reuse));
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(LINKR_DEBUGGER_RIGOL_SERVER_PORT);
		(void)zsock_inet_pton(NET_AF_INET, LINKR_DEBUGGER_RIGOL_BIND_ADDR, &addr.sin_addr);
		if (zsock_bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
		    zsock_listen(listen_fd, 4U) < 0) {
			linkr_debugger_rigol_close_fd(&listen_fd);
			k_msleep(500);
		}
	}
	LOG_INF("rigol: dispatcher listening on %s:%u", LINKR_DEBUGGER_RIGOL_BIND_ADDR,
		LINKR_DEBUGGER_RIGOL_SERVER_PORT);

	while (true) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client_fd = zsock_accept(listen_fd,
			(struct sockaddr *)&client_addr, &client_len);

		if (client_fd < 0) {
			k_msleep(20);
			continue;
		}

		uint8_t first = 0U;
		struct zsock_pollfd first_fd = { .fd = client_fd, .events = ZSOCK_POLLIN };
		ssize_t got = -1;

		if (zsock_poll(&first_fd, 1U, 2000) > 0) {
			got = zsock_recv(client_fd, &first, 1U, 0);
		}

		if (got > 0 && !linkr_debugger_rigol_first_byte_is_http(first)) {
			linkr_debugger_rigol_session(client_fd, first);
			linkr_debugger_rigol_close_fd(&client_fd);
			continue;
		}
		if (got <= 0) {
			linkr_debugger_rigol_close_fd(&client_fd);
			continue;
		}

		struct linkr_debugger_rigol_fifo_node *node = NULL;

		for (size_t i = 0U; i < LINKR_DEBUGGER_RIGOL_PUMP_SLOTS; i++) {
			if (linkr_debugger_rigol_fifo_nodes[i].data == NULL) {
				node = &linkr_debugger_rigol_fifo_nodes[i];
				break;
			}
		}
		if (node == NULL) {
			linkr_debugger_rigol_close_fd(&client_fd);
			continue;
		}
		node->data = (void *)(intptr_t)client_fd;
		node->first = (void *)(intptr_t)first;
		k_fifo_put(&linkr_debugger_rigol_pump_fifo, node);
	}
}

#define LINKR_DEBUGGER_RIGOL_THREAD_STACK 4096U
static K_THREAD_STACK_DEFINE(linkr_debugger_rigol_dispatch_stack,
	LINKR_DEBUGGER_RIGOL_THREAD_STACK);
static K_THREAD_STACK_DEFINE(linkr_debugger_rigol_pump_stack,
	LINKR_DEBUGGER_RIGOL_THREAD_STACK);
static struct k_thread linkr_debugger_rigol_dispatch_thread_data;
static struct k_thread linkr_debugger_rigol_pump_thread_data;

void linkr_debugger_rigol_server_init(void)
{
	if (!adc_is_ready_dt(&linkr_debugger_rigol_adc_gp29) ||
	    adc_channel_setup_dt(&linkr_debugger_rigol_adc_gp29) < 0) {
		LOG_WRN("rigol: GP29 ADC setup failed, CH1 will read zero");
	}

	(void)k_thread_create(&linkr_debugger_rigol_pump_thread_data,
		linkr_debugger_rigol_pump_stack, LINKR_DEBUGGER_RIGOL_THREAD_STACK,
		linkr_debugger_rigol_pump_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
	(void)k_thread_name_set(&linkr_debugger_rigol_pump_thread_data, "rigol_pump");

	(void)k_thread_create(&linkr_debugger_rigol_dispatch_thread_data,
		linkr_debugger_rigol_dispatch_stack, LINKR_DEBUGGER_RIGOL_THREAD_STACK,
		linkr_debugger_rigol_dispatch_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
	(void)k_thread_name_set(&linkr_debugger_rigol_dispatch_thread_data, "rigol_dispatch");
}
