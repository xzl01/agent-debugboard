/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_rigol.h"

#include "linkr_debugger_logic_analyzer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/gpio.h>

#include <hardware/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/websocket.h>
#include <zephyr/posix/poll.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/storage/flash_map.h>

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
#define LINKR_DEBUGGER_RIGOL_ANALOG_PRE (LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES / 2U)
#define LINKR_DEBUGGER_RIGOL_DEFAULT_TRIG_LEVEL 1.65f

static const struct adc_dt_spec linkr_debugger_rigol_adc_gp29 =
	ADC_DT_SPEC_STRUCT(DT_NODELABEL(adc), 3);

#define LINKR_DEBUGGER_RIGOL_PIN_COUNT 15U
#define LINKR_DEBUGGER_RIGOL_WINDOW_BASE 7U
#define LINKR_DEBUGGER_RIGOL_WINDOW_COUNT 14U
static const uint8_t linkr_debugger_rigol_pins[LINKR_DEBUGGER_RIGOL_PIN_COUNT] = {
	10, 16, 11, 17, 12, 18, 13, 19, 14, 20, 15, 29, 7, 8, 9,
};

static uint16_t linkr_debugger_rigol_remap_pins(const uint8_t *pins, uint8_t count,
	uint16_t window, bool gp29_high)
{
	uint16_t out = 0U;

	for (uint8_t i = 0U; i < count; i++) {
		uint8_t pin = pins[i];
		uint16_t bit;

		if (pin == 29U) {
			bit = gp29_high ? 1U : 0U;
		} else {
			bit = (uint16_t)((window >> (pin - LINKR_DEBUGGER_RIGOL_WINDOW_BASE)) & 1U);
		}
		out |= (uint16_t)(bit << i);
	}
	return out;
}

static uint16_t linkr_debugger_rigol_remap(uint16_t window, bool gp29_high)
{
	return linkr_debugger_rigol_remap_pins(linkr_debugger_rigol_pins,
		LINKR_DEBUGGER_RIGOL_PIN_COUNT, window, gp29_high);
}

static uint16_t linkr_debugger_rigol_buf[LINKR_DEBUGGER_RIGOL_BUFFER_SAMPLES];
static uint8_t linkr_debugger_rigol_analog_buf[LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES];
static uint8_t linkr_debugger_rigol_analog_ring[LINKR_DEBUGGER_RIGOL_ANALOG_PRE];

struct linkr_debugger_rigol_state;

static int linkr_debugger_rigol_arm_analog(struct linkr_debugger_rigol_state *state,
	uint32_t rate_hz);
static int linkr_debugger_rigol_pump_analog(struct linkr_debugger_rigol_state *state,
	uint32_t budget_ms);
static void linkr_debugger_rigol_cancel_capture(
	struct linkr_debugger_rigol_state *state);
static void linkr_debugger_rigol_deep_abort(void);

struct linkr_debugger_rigol_state {
	double timebase;
	uint32_t trigger_source;
	enum linkr_debugger_la_trigger_type trigger_slope;
	float trigger_level;
	uint32_t digital_enabled;
	bool armed;
	bool sweep_single;
	bool frame_ready;
	bool capture_pending;
	bool capture_done;
	bool capture_owns_la;
	bool want_arm;
	bool chan1_enabled;
	int64_t arm_deadline;
	uint32_t capture_rate;
	uint16_t analog_ring_head;
	uint16_t analog_ring_count;
	uint16_t analog_post_count;
	bool analog_triggered;
	int analog_prev_raw;
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

struct linkr_debugger_rigol_io {
	int (*recv)(void *ctx, uint8_t *buf, size_t max);
	int (*send)(void *ctx, const uint8_t *buf, size_t len);
	void *ctx;
};

struct linkr_debugger_rigol_ws_rx {
	int sock;
	bool closed;
	uint16_t pos;
	uint16_t len;
	uint8_t buf[512];
};

static struct linkr_debugger_rigol_io linkr_debugger_rigol_io_current;
static bool linkr_debugger_rigol_io_active;
static K_MUTEX_DEFINE(linkr_debugger_rigol_scpi_lock);

static int linkr_debugger_rigol_tcp_recv(void *ctx, uint8_t *buf, size_t max)
{
	ssize_t ret = zsock_recv((int)(intptr_t)ctx, buf, max, 0);

	if (ret < 0) {
		return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
	}
	return ret == 0 ? -1 : (int)ret;
}

static int linkr_debugger_rigol_tcp_send(void *ctx, const uint8_t *buf, size_t len)
{
	ssize_t ret = zsock_send((int)(intptr_t)ctx, buf, len, 0);

	return ret < 0 ? -1 : (int)ret;
}

static int linkr_debugger_rigol_ws_recv(void *ctx, uint8_t *out, size_t max)
{
	struct linkr_debugger_rigol_ws_rx *rx = ctx;

	if (rx->pos >= rx->len) {
		uint32_t message_type = 0U;
		uint64_t remaining = 0U;
		int ret;

		if (rx->closed) {
			return -1;
		}
		ret = websocket_recv_msg(rx->sock, rx->buf, sizeof(rx->buf),
					 &message_type, &remaining, 2000);
		if (ret == -EAGAIN) {
			return 0;
		}
		if (ret < 0 || (message_type & WEBSOCKET_FLAG_CLOSE) != 0U) {
			rx->closed = true;
			return -1;
		}
		if (remaining != 0U || ret == 0) {
			rx->closed = true;
			return -1;
		}
		rx->pos = 0U;
		rx->len = (uint16_t)ret;
	}
	size_t n = rx->len - rx->pos;

	if (n > max) {
		n = max;
	}
	memcpy(out, &rx->buf[rx->pos], n);
	rx->pos += (uint16_t)n;
	return (int)n;
}

static int linkr_debugger_rigol_ws_send(void *ctx, const uint8_t *buf, size_t len)
{
	struct linkr_debugger_rigol_ws_rx *rx = ctx;
	int ret = websocket_send_msg(rx->sock, buf, len,
		WEBSOCKET_OPCODE_DATA_BINARY, false, true, 2000);

	return ret < 0 ? -1 : ret;
}

static int linkr_debugger_rigol_sock_send_all(int fd, const void *data, size_t len)
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

static int linkr_debugger_rigol_send_all(int fd, const void *data, size_t len)
{
	const uint8_t *ptr = data;

	ARG_UNUSED(fd);
	while (len > 0U) {
		int ret = linkr_debugger_rigol_io_current.send(
			linkr_debugger_rigol_io_current.ctx, ptr, len);

		if (ret <= 0) {
			return -1;
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

static bool linkr_debugger_rigol_scpi_try_lock(void)
{
	bool ok = false;

	k_mutex_lock(&linkr_debugger_rigol_scpi_lock, K_FOREVER);
	if (!linkr_debugger_rigol_io_active) {
		linkr_debugger_rigol_io_active = true;
		ok = true;
	}
	k_mutex_unlock(&linkr_debugger_rigol_scpi_lock);
	return ok;
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

		if (linkr_debugger_rigol_sock_send_all(upstream_fd, &first, 1U) < 0) {
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
			if (linkr_debugger_rigol_sock_send_all(dst, buf, (size_t)got) < 0) {
				linkr_debugger_rigol_pump_pair_close(pair_index);
			}
		}
	}
}

static bool linkr_debugger_rigol_read_line(int fd, char *line, size_t cap,
	size_t *len, bool *have_first, uint8_t *first)
{
	size_t used = 0U;

	ARG_UNUSED(fd);
	if (*have_first) {
		line[used++] = (char)*first;
		*have_first = false;
	}

	while (used + 1U < cap) {
		uint8_t ch;
		int ret = linkr_debugger_rigol_io_current.recv(
			linkr_debugger_rigol_io_current.ctx, &ch, 1U);

		if (ret < 0) {
			return false;
		}
		if (ret == 0) {
			continue;
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

static uint32_t linkr_debugger_rigol_analog_pace_us(uint32_t rate_hz)
{
	return rate_hz > 0U && rate_hz <= LINKR_DEBUGGER_RIGOL_GP29_ADC_CAP_HZ ?
		1000000U / rate_hz : 0U;
}

static void linkr_debugger_rigol_fill_analog(struct linkr_debugger_rigol_state *state,
	uint32_t rate_hz)
{
	uint32_t pace_us = linkr_debugger_rigol_analog_pace_us(rate_hz);

	ARG_UNUSED(state);
	for (uint32_t i = 0U; i < LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES; i++) {
		int raw = linkr_debugger_rigol_read_gp29_raw();

		linkr_debugger_rigol_analog_buf[i] = linkr_debugger_rigol_gp29_to_scope(raw);
		if (pace_us > 0U) {
			k_busy_wait(pace_us);
		}
	}
}

static int linkr_debugger_rigol_send_analog_frame(int fd)
{
	uint8_t header[16];
	int header_len = snprintk(header, sizeof(header), "#3%u",
		(unsigned)LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES);

	if (header_len <= 0 ||
	    linkr_debugger_rigol_send_all(fd, header, (size_t)header_len) < 0) {
		return -1;
	}
	return linkr_debugger_rigol_send_all(fd, linkr_debugger_rigol_analog_buf,
		LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES);
}

static int linkr_debugger_rigol_produce_analog_frame(int fd,
	struct linkr_debugger_rigol_state *state, uint32_t rate_hz)
{
	if (state->armed && state->trigger_source == 16U) {
		int64_t deadline;
		int p;

		if (!state->capture_pending && !state->capture_done &&
		    linkr_debugger_rigol_arm_analog(state, rate_hz) < 0) {
			return -1;
		}
		deadline = k_uptime_get() + LINKR_DEBUGGER_RIGOL_ARM_TIMEOUT_MS;
		p = linkr_debugger_rigol_pump_analog(state, 10U);
		while (p == 0 && k_uptime_get() < deadline) {
			p = linkr_debugger_rigol_pump_analog(state, 10U);
		}
		if (p != 1) {
			linkr_debugger_rigol_cancel_capture(state);
			return -1;
		}
	} else {
		linkr_debugger_rigol_fill_analog(state, rate_hz);
	}
	state->capture_done = false;
	if (state->armed) {
		state->want_arm = true;
	}
	return linkr_debugger_rigol_send_analog_frame(fd);
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
	for (uint8_t i = 0U; i < LINKR_DEBUGGER_RIGOL_WINDOW_COUNT; i++) {
		la->selected_pins[i] = (uint8_t)(LINKR_DEBUGGER_RIGOL_WINDOW_BASE + i);
	}
	la->selected_pin_count = (uint8_t)LINKR_DEBUGGER_RIGOL_WINDOW_COUNT;
	la->pin_count = (uint8_t)LINKR_DEBUGGER_RIGOL_WINDOW_COUNT;
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
		t = 4U * 1000U * LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES / rate_hz + 20U;
	}
	if (t < 20U) {
		t = 20U;
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

	if (state->trigger_source >= LINKR_DEBUGGER_RIGOL_PIN_COUNT ||
	    linkr_debugger_rigol_pins[state->trigger_source] == 29U ||
	    rate_hz > LINKR_DEBUGGER_LA_MAX_STREAM_RATE_HZ) {
		return -ENOTSUP;
	}
	linkr_debugger_rigol_fill_la_config(&la, rate_hz);
	la.trigger = state->trigger_slope;
	la.trigger_pin = linkr_debugger_rigol_pins[state->trigger_source] -
		LINKR_DEBUGGER_RIGOL_WINDOW_BASE;
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
	state->capture_owns_la = true;
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
	{
		bool gp29_high = (gpio_get_all() & (1UL << 29)) != 0U;

		for (uint32_t i = 0U; i < real; i++) {
			linkr_debugger_rigol_buf[i] =
				linkr_debugger_rigol_remap(snapshot[i].values, gp29_high);
		}
	}
	linkr_debugger_logic_analyzer_release();
	last = real > 0U ? linkr_debugger_rigol_buf[real - 1U] : 0U;
	for (uint32_t i = real; i < LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES; i++) {
		linkr_debugger_rigol_buf[i] = last;
	}
	state->capture_done = true;
	return 1;
}

static void linkr_debugger_rigol_cancel_capture(
	struct linkr_debugger_rigol_state *state)
{
	if (state->capture_pending && state->capture_owns_la) {
		(void)linkr_debugger_logic_analyzer_cancel();
	}
	state->capture_pending = false;
	state->capture_owns_la = false;
	linkr_debugger_rigol_deep_abort();
}

static int linkr_debugger_rigol_arm_analog(struct linkr_debugger_rigol_state *state,
	uint32_t rate_hz)
{
	if (state->trigger_source != 16U) {
		return -ENOTSUP;
	}
	state->analog_ring_head = 0U;
	state->analog_ring_count = 0U;
	state->analog_post_count = 0U;
	state->analog_triggered = false;
	state->analog_prev_raw = -1;
	state->capture_pending = true;
	state->capture_done = false;
	state->capture_owns_la = false;
	state->capture_rate = rate_hz;
	state->arm_deadline = k_uptime_get() +
		linkr_debugger_rigol_auto_timeout_ms(rate_hz);
	return 0;
}

static int linkr_debugger_rigol_pump_analog(struct linkr_debugger_rigol_state *state,
	uint32_t budget_ms)
{
	uint32_t pace_us = linkr_debugger_rigol_analog_pace_us(state->capture_rate);
	int level_raw = (int)(state->trigger_level * 4095.0f / 3.3f);
	int64_t end = k_uptime_get() + (int64_t)budget_ms;

	if (!state->capture_pending) {
		return state->capture_done ? 1 : 0;
	}
	if (level_raw < 0) {
		level_raw = 0;
	} else if (level_raw > 4095) {
		level_raw = 4095;
	}

	while (state->capture_pending) {
		if (state->analog_triggered &&
		    state->analog_post_count >= LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES -
					    LINKR_DEBUGGER_RIGOL_ANALOG_PRE) {
			state->capture_pending = false;
			state->capture_done = true;
			return 1;
		}
		if (!state->analog_triggered &&
		    k_uptime_get() >= state->arm_deadline) {
			linkr_debugger_rigol_fill_analog(state, state->capture_rate);
			state->capture_pending = false;
			state->capture_done = true;
			return 1;
		}
		if (k_uptime_get() >= end) {
			return 0;
		}

		int raw = linkr_debugger_rigol_read_gp29_raw();
		uint8_t v;

		if (raw < 0) {
			raw = state->analog_prev_raw >= 0 ? state->analog_prev_raw : 0;
		}
		v = linkr_debugger_rigol_gp29_to_scope(raw);

		if (!state->analog_triggered) {
			bool rising = state->analog_prev_raw >= 0 &&
				state->analog_prev_raw < level_raw && raw >= level_raw;
			bool falling = state->analog_prev_raw >= level_raw &&
				raw < level_raw;
			bool hit = state->trigger_slope == LINKR_DEBUGGER_LA_TRIGGER_RISING ? rising :
				state->trigger_slope == LINKR_DEBUGGER_LA_TRIGGER_FALLING ? falling :
				rising || falling;

			linkr_debugger_rigol_analog_ring[state->analog_ring_head] = v;
			state->analog_ring_head = (state->analog_ring_head + 1U) %
				LINKR_DEBUGGER_RIGOL_ANALOG_PRE;
			if (state->analog_ring_count < LINKR_DEBUGGER_RIGOL_ANALOG_PRE) {
				state->analog_ring_count++;
			}
			if (hit) {
				uint16_t n = state->analog_ring_count;
				uint16_t pre_n = n > 1U ? (uint16_t)(n - 1U) : 0U;
				uint16_t back = LINKR_DEBUGGER_RIGOL_ANALOG_PRE - pre_n;
				uint16_t start = (state->analog_ring_head +
					LINKR_DEBUGGER_RIGOL_ANALOG_PRE - n) %
					LINKR_DEBUGGER_RIGOL_ANALOG_PRE;
				uint8_t first = linkr_debugger_rigol_analog_ring[start];

				for (uint32_t i = 0U; i < LINKR_DEBUGGER_RIGOL_ANALOG_PRE; i++) {
					linkr_debugger_rigol_analog_buf[i] = i < back ? first :
						linkr_debugger_rigol_analog_ring[
							(start + i - back) %
							LINKR_DEBUGGER_RIGOL_ANALOG_PRE];
				}
				linkr_debugger_rigol_analog_buf[
					LINKR_DEBUGGER_RIGOL_ANALOG_PRE] = v;
				state->analog_post_count = 1U;
				state->analog_triggered = true;
			}
		} else if (state->analog_post_count <
			   LINKR_DEBUGGER_RIGOL_LIVE_SAMPLES - LINKR_DEBUGGER_RIGOL_ANALOG_PRE) {
			linkr_debugger_rigol_analog_buf[LINKR_DEBUGGER_RIGOL_ANALOG_PRE +
				state->analog_post_count] = v;
			state->analog_post_count++;
		}
		state->analog_prev_raw = raw;
		if (pace_us > 0U) {
			k_busy_wait(pace_us);
		}
	}
	return state->capture_done ? 1 : 0;
}

#define LINKR_DEBUGGER_RIGOL_DEEP_SAMPLES 1048576U
#define LINKR_DEBUGGER_RIGOL_DEEP_DIGITAL_BYTES (LINKR_DEBUGGER_RIGOL_DEEP_SAMPLES * 2U)
#define LINKR_DEBUGGER_RIGOL_DEEP_MAX_RATE_HZ 25000U
#define LINKR_DEBUGGER_RIGOL_DEEP_AUTO_MS 2200U
#define LINKR_DEBUGGER_RIGOL_DEEP_STAGE_BYTES 8192U
#define LINKR_DEBUGGER_RIGOL_DEEP_SECTOR 4096U
#define LINKR_DEBUGGER_RIGOL_DEEP_PAGE 256U
#define LINKR_DEBUGGER_RIGOL_BL_PORT 5555U
#define LINKR_DEBUGGER_RIGOL_BL_MAX_RATE_HZ 204800U
#define LINKR_DEBUGGER_RIGOL_BL_CHANNEL_COUNT 14U
#define LINKR_DEBUGGER_RIGOL_BL_BUFFER_SIZE 2097152U

enum linkr_debugger_rigol_bl_sampleunit {
	LINKR_DEBUGGER_RIGOL_BL_UNIT_16_BITS = 0,
	LINKR_DEBUGGER_RIGOL_BL_UNIT_8_BITS = 1,
};

enum linkr_debugger_rigol_staging_owner {
	LINKR_DEBUGGER_RIGOL_STAGING_NONE = 0,
	LINKR_DEBUGGER_RIGOL_STAGING_DEEP,
	LINKR_DEBUGGER_RIGOL_STAGING_BL,
};

struct linkr_debugger_rigol_bl_state {
	int listen_fd;
	int client_fd;
	uint32_t rate_hz;
	uint32_t sampleunit;
	uint32_t triggerflags;
	bool streaming;
	bool use_la;
	uint32_t dropped;
};

static struct linkr_debugger_rigol_bl_state linkr_debugger_rigol_bl = {
	.listen_fd = -1,
	.client_fd = -1,
	.rate_hz = 100000U,
	.sampleunit = LINKR_DEBUGGER_RIGOL_BL_UNIT_16_BITS,
	.triggerflags = 1U,
	.streaming = false,
	.dropped = 0U,
};
static enum linkr_debugger_rigol_staging_owner linkr_debugger_rigol_staging_owner =
	LINKR_DEBUGGER_RIGOL_STAGING_NONE;

static const uint8_t linkr_debugger_rigol_bl_pins[
	LINKR_DEBUGGER_RIGOL_BL_CHANNEL_COUNT] = {
	10, 16, 11, 17, 12, 18, 13, 19, 14, 20, 15, 29, 7, 8,
};

enum linkr_debugger_rigol_deep_state {
	LINKR_DEBUGGER_RIGOL_DEEP_IDLE = 0,
	LINKR_DEBUGGER_RIGOL_DEEP_PREPARING,
	LINKR_DEBUGGER_RIGOL_DEEP_CAPTURING,
	LINKR_DEBUGGER_RIGOL_DEEP_DONE,
};

struct linkr_debugger_rigol_deep {
	enum linkr_debugger_rigol_deep_state state;
	const struct flash_area *area;
	uint32_t rate_hz;
	uint32_t written_samples;
	uint32_t window_bytes;
	uint32_t limit_samples;
	uint32_t trigger_sample;
	uint32_t post_remaining;
	uint32_t read_sample;
	uint32_t read_byte;
	uint32_t erase_offset;
	uint32_t program_offset;
	uint32_t dropped;
	bool analog;
	bool triggered;
	bool stop_requested;
	uint8_t last_level;
	uint8_t last_bytes[2];
	uint32_t trigger_source;
	enum linkr_debugger_la_trigger_type trigger_slope;
	int trigger_level_raw;
	int analog_prev_raw;
	uint32_t stage_head;
	uint32_t stage_tail;
	struct k_mutex stage_lock;
	struct k_sem stage_data;
	uint8_t stage[LINKR_DEBUGGER_RIGOL_DEEP_STAGE_BYTES];
};

static struct linkr_debugger_rigol_deep linkr_debugger_rigol_deep_inst;
static uint8_t linkr_debugger_rigol_deep_block[LINKR_DEBUGGER_RIGOL_LIVE_FRAME_BYTES];

static uint32_t linkr_debugger_rigol_deep_stage_used(void)
{
	struct linkr_debugger_rigol_deep *deep = &linkr_debugger_rigol_deep_inst;

	return (deep->stage_head + LINKR_DEBUGGER_RIGOL_DEEP_STAGE_BYTES - deep->stage_tail) %
		LINKR_DEBUGGER_RIGOL_DEEP_STAGE_BYTES;
}

static void linkr_debugger_rigol_deep_stage_push(const uint8_t *data, uint32_t len)
{
	struct linkr_debugger_rigol_deep *deep = &linkr_debugger_rigol_deep_inst;
	bool wake = false;

	k_mutex_lock(&deep->stage_lock, K_FOREVER);
	if (LINKR_DEBUGGER_RIGOL_DEEP_STAGE_BYTES - 1U -
	    linkr_debugger_rigol_deep_stage_used() < len) {
		deep->dropped += len;
		wake = true;
	} else {
		for (uint32_t i = 0U; i < len; i++) {
			deep->stage[deep->stage_head] = data[i];
			deep->stage_head = (deep->stage_head + 1U) % LINKR_DEBUGGER_RIGOL_DEEP_STAGE_BYTES;
		}
		wake = true;
	}
	k_mutex_unlock(&deep->stage_lock);
	if (wake) {
		k_sem_give(&deep->stage_data);
	}
}


static int linkr_debugger_rigol_deep_flash_write(uint32_t offset,
	const uint8_t *data, uint32_t len)
{
	struct linkr_debugger_rigol_deep *deep = &linkr_debugger_rigol_deep_inst;
	int ret = 0;

	while (len > 0U) {
		while (offset + LINKR_DEBUGGER_RIGOL_DEEP_PAGE > deep->erase_offset) {
			if (deep->erase_offset >= LINKR_DEBUGGER_RIGOL_DEEP_DIGITAL_BYTES) {
				return -ENOSPC;
			}
			ret = flash_area_erase(deep->area, deep->erase_offset,
				LINKR_DEBUGGER_RIGOL_DEEP_SECTOR);
			if (ret < 0) {
				return ret;
			}
			deep->erase_offset += LINKR_DEBUGGER_RIGOL_DEEP_SECTOR;
		}
		uint32_t chunk = len < LINKR_DEBUGGER_RIGOL_DEEP_PAGE ?
			len : LINKR_DEBUGGER_RIGOL_DEEP_PAGE;

		ret = flash_area_write(deep->area, offset, data, chunk);
		if (ret < 0) {
			return ret;
		}
		offset += chunk;
		data += chunk;
		len -= chunk;
	}
	return 0;
}

static void linkr_debugger_rigol_bl_produce(void)
{
	struct linkr_debugger_rigol_bl_state *bl = &linkr_debugger_rigol_bl;
	static uint8_t local[256];
	static uint32_t used;
	static int64_t next_due_us;
	uint32_t period_us = bl->rate_hz > 0U ? 1000000U / bl->rate_hz : 1U;
	uint32_t port;
	uint16_t v;

	if (used == 0U) {
		next_due_us = (int64_t)k_cyc_to_us_near32(k_cycle_get_32()) + period_us;
	}
	for (uint8_t i = 0U; i < 32U && bl->streaming; i++) {
		v = 0U;
		port = gpio_get_all();
		for (uint8_t ch = 0U; ch < LINKR_DEBUGGER_RIGOL_BL_CHANNEL_COUNT; ch++) {
			if ((port & (1UL << linkr_debugger_rigol_bl_pins[ch])) != 0U) {
				v |= (uint16_t)(1U << ch);
			}
		}
		local[used++] = (uint8_t)(v & 0xffU);
		if (bl->sampleunit != LINKR_DEBUGGER_RIGOL_BL_UNIT_8_BITS) {
			local[used++] = (uint8_t)(v >> 8U);
		}
		if (used >= sizeof(local)) {
			linkr_debugger_rigol_deep_stage_push(local, used);
			used = 0U;
		}
		while ((int64_t)k_cyc_to_us_near32(k_cycle_get_32()) < next_due_us) {
		}
		next_due_us += period_us;
	}
	if (used >= 64U) {
		linkr_debugger_rigol_deep_stage_push(local, used);
		used = 0U;
	}
}

static void linkr_debugger_rigol_deep_produce_digital(
	struct linkr_debugger_rigol_deep *deep)
{
	static const struct device *gpio0;
	gpio_port_value_t port = 0;
	uint16_t v = 0U;
	uint8_t packed[2];

	if (gpio0 == NULL) {
		gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	}
	(void)gpio_port_get_raw(gpio0, &port);
	for (uint8_t i = 0U; i < 15U; i++) {
		if ((port & ((gpio_port_value_t)1U << linkr_debugger_rigol_pins[i])) != 0U) {
			v |= (uint16_t)(1U << i);
		}
	}
	if (!deep->triggered && deep->trigger_source < 15U) {
		uint16_t mask = (uint16_t)(1U << deep->trigger_source);
		bool level = (v & mask) != 0U;
		bool rising = deep->last_level == 0U && level;
		bool falling = deep->last_level != 0U && !level;
		bool hit = deep->trigger_slope == LINKR_DEBUGGER_LA_TRIGGER_RISING ? rising :
			deep->trigger_slope == LINKR_DEBUGGER_LA_TRIGGER_FALLING ? falling :
			rising || falling;

		if (hit && deep->written_samples > 0U) {
			deep->triggered = true;
			deep->trigger_sample = deep->written_samples;
			deep->post_remaining = deep->limit_samples / 2U;
		}
		deep->last_level = level ? 1U : 0U;
	}
	deep->last_bytes[0] = (uint8_t)(v & 0xffU);
	deep->last_bytes[1] = (uint8_t)(v >> 8U);
	packed[0] = deep->last_bytes[0];
	packed[1] = deep->last_bytes[1];
	linkr_debugger_rigol_deep_stage_push(packed, 2U);
	deep->written_samples++;
	if (deep->triggered && deep->post_remaining > 0U) {
		deep->post_remaining--;
		if (deep->post_remaining == 0U) {
			deep->stop_requested = true;
		}
	}
	if (deep->stop_requested || deep->written_samples >= deep->limit_samples) {
		deep->state = LINKR_DEBUGGER_RIGOL_DEEP_DONE;
	}
	if (deep->rate_hz > 0U) {
		k_busy_wait(1000000U / deep->rate_hz);
	}
}

static void linkr_debugger_rigol_deep_flash_thread(void *p1, void *p2, void *p3)
{
	struct linkr_debugger_rigol_deep *deep = &linkr_debugger_rigol_deep_inst;
	uint8_t page[LINKR_DEBUGGER_RIGOL_DEEP_PAGE];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		if (deep->state == LINKR_DEBUGGER_RIGOL_DEEP_PREPARING) {
			if (deep->area == NULL &&
			    flash_area_open(PARTITION_ID(storage_partition), &deep->area) < 0) {
				deep->state = LINKR_DEBUGGER_RIGOL_DEEP_IDLE;
				continue;
			}
			if (deep->erase_offset < deep->window_bytes) {
				if (flash_area_erase(deep->area, deep->erase_offset,
				    LINKR_DEBUGGER_RIGOL_DEEP_SECTOR) < 0) {
					LOG_WRN("rigol deep: erase failed at %u",
						(unsigned)deep->erase_offset);
					deep->state = LINKR_DEBUGGER_RIGOL_DEEP_IDLE;
					continue;
				}
				deep->erase_offset += LINKR_DEBUGGER_RIGOL_DEEP_SECTOR;
			} else {
				deep->state = LINKR_DEBUGGER_RIGOL_DEEP_CAPTURING;
			}
			continue;
		}
		if (deep->state == LINKR_DEBUGGER_RIGOL_DEEP_DONE &&
		    linkr_debugger_rigol_staging_owner == LINKR_DEBUGGER_RIGOL_STAGING_DEEP) {
			if (linkr_debugger_rigol_deep_stage_used() == 0U) {
				linkr_debugger_rigol_staging_owner =
					LINKR_DEBUGGER_RIGOL_STAGING_NONE;
			}
		}
		if (deep->state == LINKR_DEBUGGER_RIGOL_DEEP_CAPTURING &&
		    !deep->analog) {
			linkr_debugger_rigol_deep_produce_digital(deep);
			if (k_sem_take(&deep->stage_data, K_NO_WAIT) != 0) {
				continue;
			}
		} else if (linkr_debugger_rigol_bl.streaming &&
		    !linkr_debugger_rigol_bl.use_la) {
			linkr_debugger_rigol_bl_produce();
			if (k_sem_take(&deep->stage_data, K_NO_WAIT) != 0) {
				continue;
			}
		} else if (deep->state == LINKR_DEBUGGER_RIGOL_DEEP_CAPTURING &&
		    deep->analog) {
			uint32_t pace_us = linkr_debugger_rigol_analog_pace_us(deep->rate_hz);
			int raw = linkr_debugger_rigol_read_gp29_raw();
			uint8_t v;

			if (deep->area == NULL &&
			    flash_area_open(PARTITION_ID(storage_partition), &deep->area) < 0) {
				deep->state = LINKR_DEBUGGER_RIGOL_DEEP_IDLE;
				continue;
			}
			if (raw < 0) {
				raw = deep->analog_prev_raw >= 0 ? deep->analog_prev_raw : 0;
			}
			v = linkr_debugger_rigol_gp29_to_scope(raw);
			if (!deep->triggered) {
				bool rising = deep->analog_prev_raw >= 0 &&
					deep->analog_prev_raw < deep->trigger_level_raw &&
					raw >= deep->trigger_level_raw;
				bool falling = deep->analog_prev_raw >= deep->trigger_level_raw &&
					raw < deep->trigger_level_raw;
				bool hit = deep->trigger_slope == LINKR_DEBUGGER_LA_TRIGGER_RISING ? rising :
					deep->trigger_slope == LINKR_DEBUGGER_LA_TRIGGER_FALLING ? falling :
					rising || falling;

				if (hit && deep->written_samples > 0U) {
					deep->triggered = true;
					deep->trigger_sample = deep->written_samples;
					deep->post_remaining =
						LINKR_DEBUGGER_RIGOL_DEEP_SAMPLES / 2U;
				}
			}
			deep->analog_prev_raw = raw;
			deep->last_bytes[0] = v;
			linkr_debugger_rigol_deep_stage_push(&v, 1U);
			deep->written_samples++;
			if (deep->triggered && deep->post_remaining > 0U) {
				deep->post_remaining--;
				if (deep->post_remaining == 0U) {
					deep->stop_requested = true;
				}
			}
			if (deep->stop_requested ||
			    deep->written_samples >= deep->limit_samples) {
				deep->state = LINKR_DEBUGGER_RIGOL_DEEP_DONE;
			}
			if (pace_us > 0U) {
				k_busy_wait(pace_us);
			}
			if (k_sem_take(&deep->stage_data, K_NO_WAIT) != 0) {
				continue;
			}
		} else {
			k_sem_take(&deep->stage_data, K_FOREVER);
		}
		if (linkr_debugger_rigol_staging_owner == LINKR_DEBUGGER_RIGOL_STAGING_BL) {
			static uint8_t bl_carry[2048];
			static uint32_t bl_carry_len;

			for (;;) {
				k_mutex_lock(&deep->stage_lock, K_FOREVER);
				while (bl_carry_len < sizeof(bl_carry) &&
				       linkr_debugger_rigol_deep_stage_used() > 0U) {
					bl_carry[bl_carry_len++] = deep->stage[deep->stage_tail];
					deep->stage_tail = (deep->stage_tail + 1U) %
						LINKR_DEBUGGER_RIGOL_DEEP_STAGE_BYTES;
				}
				uint32_t still = linkr_debugger_rigol_deep_stage_used();
				k_mutex_unlock(&deep->stage_lock);

				if (bl_carry_len >= 512U ||
				    (bl_carry_len > 0U && still == 0U)) {
					uint32_t n = bl_carry_len;
					int sent = 0;

					for (uint8_t attempt = 0U; attempt < 50U; attempt++) {
						sent = linkr_debugger_rigol_sock_send_all(
							linkr_debugger_rigol_bl.client_fd,
							bl_carry, n);
						if (sent >= 0) {
							break;
						}
						if (errno != ENOMEM && errno != EAGAIN &&
						    errno != EWOULDBLOCK) {
							break;
						}
						k_msleep(2);
					}
					if (sent < 0) {
						LOG_WRN("rigol bl: send failed ret=%d errno=%d",
							sent, errno);
						linkr_debugger_rigol_bl.streaming = false;
						if (linkr_debugger_rigol_bl.use_la) {
							(void)linkr_debugger_logic_analyzer_stop_stream();
							linkr_debugger_rigol_bl.use_la = false;
						}
						linkr_debugger_rigol_staging_owner =
							LINKR_DEBUGGER_RIGOL_STAGING_NONE;
						break;
					}
					memmove(bl_carry, &bl_carry[n], bl_carry_len - n);
					bl_carry_len -= n;
					continue;
				}
				break;
			}
			continue;
		}
		if (deep->area == NULL &&
		    flash_area_open(PARTITION_ID(storage_partition), &deep->area) < 0) {
			k_mutex_lock(&deep->stage_lock, K_FOREVER);
			deep->stage_tail = deep->stage_head;
			k_mutex_unlock(&deep->stage_lock);
			continue;
		}

		uint32_t base;
		uint32_t n = 0U;

		k_mutex_lock(&deep->stage_lock, K_FOREVER);
		while (n < sizeof(page) && linkr_debugger_rigol_deep_stage_used() > 0U) {
			page[n++] = deep->stage[deep->stage_tail];
			deep->stage_tail = (deep->stage_tail + 1U) %
				LINKR_DEBUGGER_RIGOL_DEEP_STAGE_BYTES;
		}
		base = deep->program_offset;
		deep->program_offset += n;
		k_mutex_unlock(&deep->stage_lock);

		if (n > 0U && base + n <= LINKR_DEBUGGER_RIGOL_DEEP_DIGITAL_BYTES) {
			if (linkr_debugger_rigol_deep_flash_write(base, page, n) < 0) {
				LOG_WRN("rigol deep: flash write failed at %u",
					(unsigned)base);
			}
		}
	}
}

static void linkr_debugger_rigol_deep_reset(void)
{
	struct linkr_debugger_rigol_deep *deep = &linkr_debugger_rigol_deep_inst;

	deep->state = LINKR_DEBUGGER_RIGOL_DEEP_IDLE;
	deep->written_samples = 0U;
	deep->trigger_sample = 0U;
	deep->post_remaining = 0U;
	deep->read_sample = 0U;
	deep->read_byte = 0U;
	deep->erase_offset = 0U;
	deep->program_offset = 0U;
	deep->dropped = 0U;
	deep->window_bytes = 0U;
	deep->limit_samples = 0U;
	deep->triggered = false;
	deep->stop_requested = false;
	deep->analog = false;
	deep->last_level = 0U;
	k_mutex_lock(&deep->stage_lock, K_FOREVER);
	deep->stage_head = 0U;
	deep->stage_tail = 0U;
	k_mutex_unlock(&deep->stage_lock);
	k_sem_reset(&deep->stage_data);
}

static int linkr_debugger_rigol_deep_start(struct linkr_debugger_rigol_state *state,
	uint32_t rate_hz, uint32_t duration_s)
{
	struct linkr_debugger_rigol_deep *deep = &linkr_debugger_rigol_deep_inst;
	uint32_t bps;
	uint64_t want;

	if (linkr_debugger_rigol_staging_owner == LINKR_DEBUGGER_RIGOL_STAGING_BL) {
		return -EBUSY;
	}
	linkr_debugger_rigol_staging_owner = LINKR_DEBUGGER_RIGOL_STAGING_DEEP;
	linkr_debugger_rigol_deep_reset();
	deep->rate_hz = rate_hz > LINKR_DEBUGGER_RIGOL_DEEP_MAX_RATE_HZ ?
		LINKR_DEBUGGER_RIGOL_DEEP_MAX_RATE_HZ : rate_hz;
	if (deep->rate_hz == 0U) {
		deep->rate_hz = 1U;
	}
	deep->analog = state->digital_enabled == 0U && state->chan1_enabled;
	if (deep->analog && deep->rate_hz > LINKR_DEBUGGER_RIGOL_GP29_ADC_CAP_HZ) {
		deep->rate_hz = LINKR_DEBUGGER_RIGOL_GP29_ADC_CAP_HZ;
	}
	deep->trigger_source = state->trigger_source;
	deep->trigger_slope = state->trigger_slope;
	deep->trigger_level_raw = (int)(state->trigger_level * 4095.0f / 3.3f);
	if (deep->trigger_level_raw < 0) {
		deep->trigger_level_raw = 0;
	} else if (deep->trigger_level_raw > 4095) {
		deep->trigger_level_raw = 4095;
	}
	deep->analog_prev_raw = -1;
	bps = deep->analog ? 1U : 2U;
	want = (uint64_t)deep->rate_hz * (uint64_t)(duration_s > 0U ? duration_s : 2U);
	if (want == 0U) {
		want = 1U;
	}
	if (want > LINKR_DEBUGGER_RIGOL_DEEP_DIGITAL_BYTES / bps) {
		want = LINKR_DEBUGGER_RIGOL_DEEP_DIGITAL_BYTES / bps;
	}
	deep->limit_samples = (uint32_t)want;
	deep->window_bytes = (uint32_t)((want * bps +
		LINKR_DEBUGGER_RIGOL_DEEP_SECTOR - 1U) /
		LINKR_DEBUGGER_RIGOL_DEEP_SECTOR * LINKR_DEBUGGER_RIGOL_DEEP_SECTOR);
	if (deep->window_bytes > LINKR_DEBUGGER_RIGOL_DEEP_DIGITAL_BYTES) {
		deep->window_bytes = LINKR_DEBUGGER_RIGOL_DEEP_DIGITAL_BYTES;
	}
	deep->state = LINKR_DEBUGGER_RIGOL_DEEP_PREPARING;
	return 0;
}

static void linkr_debugger_rigol_deep_abort(void)
{
	linkr_debugger_rigol_deep_inst.state = LINKR_DEBUGGER_RIGOL_DEEP_IDLE;
	if (linkr_debugger_rigol_staging_owner == LINKR_DEBUGGER_RIGOL_STAGING_DEEP) {
		linkr_debugger_rigol_staging_owner = LINKR_DEBUGGER_RIGOL_STAGING_NONE;
	}
}

static void linkr_debugger_rigol_deep_poll(struct linkr_debugger_rigol_state *state)
{
	ARG_UNUSED(state);
}

static int linkr_debugger_rigol_deep_serve_data(int fd, uint32_t off, uint32_t count)
{
	struct linkr_debugger_rigol_deep *deep = &linkr_debugger_rigol_deep_inst;
	uint32_t bps = deep->analog ? 1U : 2U;
	uint32_t total = count * bps;
	uint32_t sent = 0U;
	uint8_t header[20];
	int header_len;

	if (count == 0U || total > (1U << 20)) {
		return linkr_debugger_rigol_send_str(fd, "ERR\n");
	}

	int64_t deadline = k_uptime_get() + 2000;

	while (deep->program_offset < (uint64_t)off * bps + total &&
	       deep->program_offset < (uint64_t)deep->written_samples * bps &&
	       k_uptime_get() < deadline) {
		k_msleep(5);
	}

	header_len = snprintk(header, sizeof(header), "#%u%u",
		total >= 1000000U ? 7U : total >= 100000U ? 6U :
		total >= 10000U ? 5U : total >= 1000U ? 4U :
		total >= 100U ? 3U : total >= 10U ? 2U : 1U,
		(unsigned)total);
	if (header_len <= 0 ||
	    linkr_debugger_rigol_send_all(fd, header, (size_t)header_len) < 0) {
		return -1;
	}

	while (sent < total) {
		uint32_t chunk = total - sent;
		uint32_t sample_pos = off + sent / bps;
		uint32_t real = 0U;

		if (chunk > sizeof(linkr_debugger_rigol_deep_block)) {
			chunk = sizeof(linkr_debugger_rigol_deep_block);
		}
		if (sample_pos < deep->written_samples) {
			uint32_t avail = (deep->written_samples - sample_pos) * bps;

			real = chunk < avail ? chunk : avail;
		}
		if (real > 0U && deep->area != NULL &&
		    flash_area_read(deep->area, (uint32_t)(off * bps) + sent,
			linkr_debugger_rigol_deep_block, real) < 0) {
			memset(linkr_debugger_rigol_deep_block, 0, real);
		}
		if (deep->analog) {
			memset(&linkr_debugger_rigol_deep_block[real], deep->last_bytes[0],
				chunk - real);
		} else {
			for (uint32_t i = real; i < chunk; i += 2U) {
				linkr_debugger_rigol_deep_block[i] = deep->last_bytes[0];
				linkr_debugger_rigol_deep_block[i + 1U] = deep->last_bytes[1];
			}
		}
		if (linkr_debugger_rigol_send_all(fd, linkr_debugger_rigol_deep_block,
			chunk) < 0) {
			return -1;
		}
		sent += chunk;
	}
	return 0;
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
	bool gp29_high = (gpio_get_all() & (1UL << 29)) != 0U;

	for (uint32_t i = 0U; i < n; i++) {
		linkr_debugger_rigol_buf[sink->count + i] =
			linkr_debugger_rigol_remap(chunk->values[i], gp29_high);
	}
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
	{
		bool gp29_high = (gpio_get_all() & (1UL << 29)) != 0U;

		for (uint32_t i = 0U; i < real; i++) {
			linkr_debugger_rigol_buf[i] =
				linkr_debugger_rigol_remap(snapshot[i].values, gp29_high);
		}
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
		linkr_debugger_rigol_cancel_capture(state);
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

	use_trigger = use_trigger && state->trigger_source < LINKR_DEBUGGER_RIGOL_PIN_COUNT &&
		linkr_debugger_rigol_pins[state->trigger_source] != 29U;
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
					linkr_debugger_rigol_cancel_capture(state);
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
		bool enabled = idx < LINKR_DEBUGGER_RIGOL_PIN_COUNT && (state->digital_enabled & (1U << idx)) != 0U;

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
		int len = linkr_debugger_rigol_format_double(resp, sizeof(resp),
			(double)state->trigger_level);

		if (len <= 0) {
			return -1;
		}
		resp[len++] = '\n';
		return linkr_debugger_rigol_send_all(fd, resp, (size_t)len);
	}
	if (strcmp(line, ":TRIG:MODE?") == 0) {
		return linkr_debugger_rigol_send_str(fd, "EDGE\n");
	}
	if (strcmp(line, ":TRIG:STAT?") == 0) {
		if (state->armed && state->want_arm && !state->capture_pending &&
		    !state->capture_done) {
			uint32_t rate_hz = linkr_debugger_rigol_rate_from_timebase(
				state->timebase);
			int rc;

			state->want_arm = false;
			if (state->trigger_source == 16U) {
				rc = linkr_debugger_rigol_arm_analog(state, rate_hz);
			} else {
				rc = linkr_debugger_rigol_arm_async(state, rate_hz);
			}
			if (rc < 0) {
				state->frame_ready = true;
			}
		}
		if (state->capture_pending) {
			int p = state->trigger_source == 16U ?
				linkr_debugger_rigol_pump_analog(state, 4U) :
				linkr_debugger_rigol_poll_capture(state);

			if (p == 0) {
				return linkr_debugger_rigol_send_str(fd, "RUN\n");
			}
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

		return linkr_debugger_rigol_produce_analog_frame(fd, state, rate_hz);
	}
	if (strcmp(line, ":WAV:DATA? DIG") == 0) {
		return linkr_debugger_rigol_produce_frame(fd, state, state->armed);
	}
	if (strncmp(line, ":LINKR:DEEP:START", 17) == 0) {
		const char *p = line + 17;
		uint32_t rate = (uint32_t)strtoul(p, (char **)&p, 10);
		uint32_t duration_s = (uint32_t)strtoul(p, NULL, 10);

		if (rate == 0U) {
			rate = LINKR_DEBUGGER_RIGOL_DEEP_MAX_RATE_HZ;
		}
		if (duration_s > 30U) {
			duration_s = 30U;
		}
		if (linkr_debugger_rigol_deep_inst.state ==
		    LINKR_DEBUGGER_RIGOL_DEEP_CAPTURING ||
		    linkr_debugger_rigol_deep_inst.state ==
		    LINKR_DEBUGGER_RIGOL_DEEP_PREPARING) {
			return linkr_debugger_rigol_send_str(fd, "BUSY\n");
		}
		linkr_debugger_rigol_cancel_capture(state);
		if (linkr_debugger_rigol_deep_start(state, rate, duration_s) < 0) {
			return linkr_debugger_rigol_send_str(fd, "ERR\n");
		}
		return linkr_debugger_rigol_send_str(fd, "OK\n");
	}
	if (strcmp(line, ":LINKR:DEEP:STOP") == 0) {
		linkr_debugger_rigol_deep_abort();
		return linkr_debugger_rigol_send_str(fd, "OK\n");
	}
	if (strcmp(line, ":LINKR:DEEP:STATUS?") == 0) {
		char resp[80];
		int len;
		struct linkr_debugger_rigol_deep *deep = &linkr_debugger_rigol_deep_inst;

		linkr_debugger_rigol_deep_poll(state);
		if (deep->state == LINKR_DEBUGGER_RIGOL_DEEP_PREPARING) {
			len = snprintk(resp, sizeof(resp), "PREPARING %u %u\n",
				(unsigned)deep->erase_offset, (unsigned)deep->window_bytes);
		} else if (deep->state == LINKR_DEBUGGER_RIGOL_DEEP_CAPTURING) {
			len = snprintk(resp, sizeof(resp), "CAPTURING %u\n",
				(unsigned)deep->written_samples);
		} else if (deep->state == LINKR_DEBUGGER_RIGOL_DEEP_DONE) {
			len = snprintk(resp, sizeof(resp), "DONE %u %ld %u %u\n",
				(unsigned)deep->written_samples,
				deep->triggered ? (long)deep->trigger_sample : -1L,
				(unsigned)deep->rate_hz, (unsigned)deep->dropped);
		} else {
			len = snprintk(resp, sizeof(resp), "IDLE\n");
		}
		if (len <= 0) {
			return -1;
		}
		return linkr_debugger_rigol_send_all(fd, resp, (size_t)len);
	}
	if (strncmp(line, ":LINKR:DEEP:DATA?", 17) == 0) {
		const char *p = line + 17;
		uint32_t off = (uint32_t)strtoul(p, (char **)&p, 10);
		uint32_t count = (uint32_t)strtoul(p, (char **)&p, 10);

		linkr_debugger_rigol_deep_poll(state);
		return linkr_debugger_rigol_deep_serve_data(fd, off, count);
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

		linkr_debugger_rigol_cancel_capture(state);

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
	if (strncmp(line, ":CHAN1:DISP ", 12) == 0) {
		state->chan1_enabled = strstr(line + 12, "ON") != NULL;
		return;
	}
	if (strncmp(line, ":TRIG:EDGE:LEV ", 15) == 0) {
		char *end = NULL;
		float v = strtof(line + 15, &end);

		if (end != line + 15) {
			state->trigger_level = v;
		}
		return;
	}
	if (strncmp(line, ":DIG", 4) == 0 && strstr(line, ":TURN ") != NULL) {
		const char *p = line + 4;
		uint32_t idx = 0U;

		while (*p >= '0' && *p <= '9') {
			idx = idx * 10U + (uint32_t)(*p - '0');
			p++;
		}
		if (idx < LINKR_DEBUGGER_RIGOL_PIN_COUNT) {
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
		linkr_debugger_rigol_cancel_capture(state);
		state->want_arm = true;
		return;
	}
	if (strcmp(line, ":SINGL") == 0 || strstr(line, ":SWE SING") != NULL) {
		state->sweep_single = true;
		state->armed = true;
		state->frame_ready = false;
		state->capture_done = false;
		linkr_debugger_rigol_cancel_capture(state);
		state->want_arm = true;
		return;
	}
	if (strcmp(line, ":STOP") == 0) {
		state->armed = false;
		state->sweep_single = false;
		state->want_arm = false;
		linkr_debugger_rigol_cancel_capture(state);
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
		state->trigger_level = LINKR_DEBUGGER_RIGOL_DEFAULT_TRIG_LEVEL;
		state->digital_enabled = 0xffffU;
		state->chan1_enabled = true;
		linkr_debugger_rigol_cancel_capture(state);
		state->capture_done = false;
		return;
	}
}

static void linkr_debugger_rigol_scpi_run(const struct linkr_debugger_rigol_io *io,
	bool have_first, uint8_t first)
{
	struct linkr_debugger_rigol_state state = {
		.timebase = 0.0001,
		.trigger_source = 0U,
		.trigger_slope = LINKR_DEBUGGER_LA_TRIGGER_RISING,
		.trigger_level = LINKR_DEBUGGER_RIGOL_DEFAULT_TRIG_LEVEL,
		.digital_enabled = 0xffffU,
		.armed = false,
		.sweep_single = false,
		.frame_ready = true,
		.capture_pending = false,
		.capture_done = false,
		.capture_owns_la = false,
		.want_arm = false,
		.chan1_enabled = true,
	};

	linkr_debugger_rigol_io_current = *io;

	while (true) {
		char line[128];
		size_t len = 0U;

		if (!linkr_debugger_rigol_read_line(0, line, sizeof(line), &len,
			&have_first, &first)) {
			break;
		}
		if (len == 0U) {
			continue;
		}

		int qret = linkr_debugger_rigol_handle_query(0, line, &state);

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
	linkr_debugger_rigol_cancel_capture(&state);
	linkr_debugger_rigol_io_current.recv = NULL;
	linkr_debugger_rigol_io_current.send = NULL;
	linkr_debugger_rigol_io_current.ctx = NULL;
	linkr_debugger_rigol_io_active = false;
}

static void linkr_debugger_rigol_tcp_session(int client_fd, uint8_t first)
{
	struct linkr_debugger_rigol_io io = {
		.recv = linkr_debugger_rigol_tcp_recv,
		.send = linkr_debugger_rigol_tcp_send,
		.ctx = (void *)(intptr_t)client_fd,
	};
	struct zsock_timeval tv;

	tv.tv_sec = LINKR_DEBUGGER_RIGOL_RECV_TIMEOUT_MS / 1000U;
	tv.tv_usec = 0;
	(void)zsock_setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	if (!linkr_debugger_rigol_scpi_try_lock()) {
		return;
	}
	linkr_debugger_rigol_scpi_run(&io, true, first);
}

static K_THREAD_STACK_DEFINE(linkr_debugger_rigol_ws_scpi_stack, 4096);
static struct k_thread linkr_debugger_rigol_ws_scpi_thread_data;
static int linkr_debugger_rigol_ws_scpi_sock = -1;

static void linkr_debugger_rigol_ws_scpi_thread(void *p1, void *p2, void *p3)
{
	struct linkr_debugger_rigol_ws_rx rx = {
		.sock = linkr_debugger_rigol_ws_scpi_sock,
	};
	struct linkr_debugger_rigol_io io = {
		.recv = linkr_debugger_rigol_ws_recv,
		.send = linkr_debugger_rigol_ws_send,
		.ctx = &rx,
	};

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	linkr_debugger_rigol_scpi_run(&io, false, 0U);
	(void)websocket_unregister(rx.sock);
	linkr_debugger_rigol_ws_scpi_sock = -1;
}

int linkr_debugger_rigol_scpi_ws_setup(int ws_socket,
	struct http_request_ctx *request_ctx, void *user_data)
{
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (!linkr_debugger_rigol_scpi_try_lock()) {
		(void)websocket_send_msg(ws_socket, (const uint8_t *)"busy\n", 5U,
			WEBSOCKET_OPCODE_DATA_TEXT, false, true, 1000);
		return -EBUSY;
	}
	linkr_debugger_rigol_ws_scpi_sock = ws_socket;
	(void)k_thread_create(&linkr_debugger_rigol_ws_scpi_thread_data,
		linkr_debugger_rigol_ws_scpi_stack,
		K_THREAD_STACK_SIZEOF(linkr_debugger_rigol_ws_scpi_stack),
		linkr_debugger_rigol_ws_scpi_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	(void)k_thread_name_set(&linkr_debugger_rigol_ws_scpi_thread_data,
		"rigol_scpi_ws");
	return 0;
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
			linkr_debugger_rigol_tcp_session(client_fd, first);
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

static int linkr_debugger_rigol_bl_send_str(int fd, const char *str)
{
	return linkr_debugger_rigol_sock_send_all(fd, str, strlen(str));
}

static bool linkr_debugger_rigol_bl_read_line(int fd, char *line, size_t cap,
	bool streaming)
{
	size_t used = 0U;

	while (used + 1U < cap) {
		uint8_t ch;
		struct zsock_pollfd pfd = { .fd = fd, .events = ZSOCK_POLLIN };
		int pret = zsock_poll(&pfd, 1U, streaming ? 20 : 2000);
		ssize_t ret;

		if (pret <= 0) {
			if (streaming && linkr_debugger_rigol_bl.streaming) {
				continue;
			}
			return false;
		}
		ret = zsock_recv(fd, &ch, 1U, 0);
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
	return true;
}

static void linkr_debugger_rigol_bl_stream_callback(
	const struct linkr_debugger_la_stream_chunk *chunk, void *user_data)
{
	bool gp29_high = (gpio_get_all() & (1UL << 29)) != 0U;

	ARG_UNUSED(user_data);

	if (linkr_debugger_rigol_bl.sampleunit == LINKR_DEBUGGER_RIGOL_BL_UNIT_8_BITS) {
		uint8_t packed[64];
		uint32_t n = 0U;

		for (uint32_t i = 0U; i < chunk->sample_count; i++) {
			uint16_t v = linkr_debugger_rigol_remap_pins(
				linkr_debugger_rigol_bl_pins,
				LINKR_DEBUGGER_RIGOL_BL_CHANNEL_COUNT,
				chunk->values[i], gp29_high);

			packed[n++] = (uint8_t)(v & 0xffU);
			if (n == sizeof(packed)) {
				linkr_debugger_rigol_deep_stage_push(packed, n);
				n = 0U;
			}
		}
		if (n > 0U) {
			linkr_debugger_rigol_deep_stage_push(packed, n);
		}
		return;
	}
	{
		uint8_t packed[64];
		uint32_t n = 0U;

		for (uint32_t i = 0U; i < chunk->sample_count; i++) {
			uint16_t v = linkr_debugger_rigol_remap_pins(
				linkr_debugger_rigol_bl_pins,
				LINKR_DEBUGGER_RIGOL_BL_CHANNEL_COUNT,
				chunk->values[i], gp29_high);

			packed[n++] = (uint8_t)(v & 0xffU);
			packed[n++] = (uint8_t)(v >> 8U);
			if (n >= sizeof(packed) - 1U) {
				linkr_debugger_rigol_deep_stage_push(packed, n);
				n = 0U;
			}
		}
		if (n > 0U) {
			linkr_debugger_rigol_deep_stage_push(packed, n);
		}
	}
}

static void linkr_debugger_rigol_bl_stream_start(void)
{
	struct linkr_debugger_rigol_deep *deep = &linkr_debugger_rigol_deep_inst;

	if (linkr_debugger_rigol_staging_owner != LINKR_DEBUGGER_RIGOL_STAGING_NONE) {
		LOG_WRN("rigol bl: stream start while staging busy (owner %d)",
			(int)linkr_debugger_rigol_staging_owner);
		return;
	}
	linkr_debugger_rigol_staging_owner = LINKR_DEBUGGER_RIGOL_STAGING_BL;
	k_mutex_lock(&deep->stage_lock, K_FOREVER);
	deep->stage_head = 0U;
	deep->stage_tail = 0U;
	k_mutex_unlock(&deep->stage_lock);
	k_sem_reset(&deep->stage_data);
	linkr_debugger_rigol_bl.streaming = true;
	linkr_debugger_rigol_bl.dropped = 0U;
	linkr_debugger_rigol_bl.use_la = false;
	if (linkr_debugger_rigol_bl.rate_hz >=
	    LINKR_DEBUGGER_LA_MIN_SAMPLE_RATE_HZ) {
		struct linkr_debugger_la_config la;

		memset(&la, 0, sizeof(la));
		for (uint8_t i = 0U; i < LINKR_DEBUGGER_RIGOL_WINDOW_COUNT; i++) {
			la.selected_pins[i] = (uint8_t)(LINKR_DEBUGGER_RIGOL_WINDOW_BASE + i);
		}
		la.selected_pin_count = (uint8_t)LINKR_DEBUGGER_RIGOL_WINDOW_COUNT;
		la.pin_count = (uint8_t)LINKR_DEBUGGER_RIGOL_WINDOW_COUNT;
		la.pin_base = LINKR_DEBUGGER_RIGOL_WINDOW_BASE;
		la.sample_rate_hz = linkr_debugger_rigol_bl.rate_hz;
		la.post_samples = LINKR_DEBUGGER_LA_MAX_EXPORTED_SAMPLES;
		if (linkr_debugger_logic_analyzer_start_stream(&la,
			linkr_debugger_rigol_bl_stream_callback, NULL) == 0) {
			linkr_debugger_rigol_bl.use_la = true;
		} else {
			LOG_WRN("rigol bl: LA stream start failed, GPIO loop fallback");
		}
	}
}

static void linkr_debugger_rigol_bl_stream_stop(void)
{
	linkr_debugger_rigol_bl.streaming = false;
	if (linkr_debugger_rigol_bl.use_la) {
		(void)linkr_debugger_logic_analyzer_stop_stream();
		linkr_debugger_rigol_bl.use_la = false;
	}
	if (linkr_debugger_rigol_staging_owner == LINKR_DEBUGGER_RIGOL_STAGING_BL) {
		linkr_debugger_rigol_staging_owner = LINKR_DEBUGGER_RIGOL_STAGING_NONE;
	}
}

static void linkr_debugger_rigol_bl_session(int fd)
{
	struct zsock_timeval tv;

	tv.tv_sec = LINKR_DEBUGGER_RIGOL_RECV_TIMEOUT_MS / 1000U;
	tv.tv_usec = 0;
	(void)zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	while (true) {
		char line[64];
		char resp[24];
		int len;

		if (!linkr_debugger_rigol_bl_read_line(fd, line, sizeof(line),
			linkr_debugger_rigol_bl.streaming)) {
			break;
		}
		if (line[0] == '\0') {
			continue;
		}
		if (strcmp(line, "version") == 0) {
			if (linkr_debugger_rigol_bl_send_str(fd,
				"BeagleLogic LinkrDebugger 1.0\n") < 0) {
				break;
			}
			continue;
		}
		if (strcmp(line, "get") == 0) {
			linkr_debugger_rigol_bl_stream_start();
			continue;
		}
		if (strcmp(line, "close") == 0) {
			linkr_debugger_rigol_bl_stream_stop();
			continue;
		}
		if (strncmp(line, "samplerate ", 11) == 0) {
			uint32_t rate = (uint32_t)strtoul(line + 11, NULL, 10);

			if (rate == 0U) {
				rate = 1U;
			}
			if (rate > LINKR_DEBUGGER_RIGOL_BL_MAX_RATE_HZ) {
				rate = LINKR_DEBUGGER_RIGOL_BL_MAX_RATE_HZ;
			}
			linkr_debugger_rigol_bl.rate_hz = rate;
			if (linkr_debugger_rigol_bl_send_str(fd, "ok\n") < 0) {
				break;
			}
			continue;
		}
		if (strcmp(line, "samplerate") == 0) {
			len = snprintk(resp, sizeof(resp), "%u\n",
				(unsigned)linkr_debugger_rigol_bl.rate_hz);
			if (len <= 0 || linkr_debugger_rigol_sock_send_all(fd,
				resp, (size_t)len) < 0) {
				break;
			}
			continue;
		}
		if (strncmp(line, "sampleunit ", 11) == 0) {
			linkr_debugger_rigol_bl.sampleunit =
				strtoul(line + 11, NULL, 10) == 1U ?
				LINKR_DEBUGGER_RIGOL_BL_UNIT_8_BITS :
				LINKR_DEBUGGER_RIGOL_BL_UNIT_16_BITS;
			if (linkr_debugger_rigol_bl_send_str(fd, "ok\n") < 0) {
				break;
			}
			continue;
		}
		if (strcmp(line, "sampleunit") == 0) {
			len = snprintk(resp, sizeof(resp), "%u\n",
				(unsigned)linkr_debugger_rigol_bl.sampleunit);
			if (len <= 0 || linkr_debugger_rigol_sock_send_all(fd,
				resp, (size_t)len) < 0) {
				break;
			}
			continue;
		}
		if (strncmp(line, "triggerflags ", 13) == 0) {
			linkr_debugger_rigol_bl.triggerflags =
				(uint32_t)strtoul(line + 13, NULL, 10);
			if (linkr_debugger_rigol_bl_send_str(fd, "ok\n") < 0) {
				break;
			}
			continue;
		}
		if (strcmp(line, "triggerflags") == 0) {
			len = snprintk(resp, sizeof(resp), "%u\n",
				(unsigned)linkr_debugger_rigol_bl.triggerflags);
			if (len <= 0 || linkr_debugger_rigol_sock_send_all(fd,
				resp, (size_t)len) < 0) {
				break;
			}
			continue;
		}
		if (strncmp(line, "memalloc ", 9) == 0) {
			if (linkr_debugger_rigol_bl_send_str(fd, "ok\n") < 0) {
				break;
			}
			continue;
		}
		if (strcmp(line, "memalloc") == 0) {
			len = snprintk(resp, sizeof(resp), "%u\n",
				(unsigned)LINKR_DEBUGGER_RIGOL_BL_BUFFER_SIZE);
			if (len <= 0 || linkr_debugger_rigol_sock_send_all(fd,
				resp, (size_t)len) < 0) {
				break;
			}
			continue;
		}
		if (strncmp(line, "bufunitsize ", 12) == 0) {
			if (linkr_debugger_rigol_bl_send_str(fd, "ok\n") < 0) {
				break;
			}
			continue;
		}
		if (strcmp(line, "bufunitsize") == 0) {
			if (linkr_debugger_rigol_bl_send_str(fd, "65536\n") < 0) {
				break;
			}
			continue;
		}
		if (linkr_debugger_rigol_bl_send_str(fd, "ERR\n") < 0) {
			break;
		}
	}
	linkr_debugger_rigol_bl_stream_stop();
}

static K_THREAD_STACK_DEFINE(linkr_debugger_rigol_bl_stack, 2048U);
static struct k_thread linkr_debugger_rigol_bl_thread_data;

static void linkr_debugger_rigol_bl_thread(void *p1, void *p2, void *p3)
{
	struct sockaddr_in addr;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (linkr_debugger_rigol_bl.listen_fd < 0) {
		int fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

		if (fd < 0) {
			k_msleep(500);
			continue;
		}
		int reuse = 1;

		(void)zsock_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
			&reuse, sizeof(reuse));
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(LINKR_DEBUGGER_RIGOL_BL_PORT);
		(void)zsock_inet_pton(NET_AF_INET, LINKR_DEBUGGER_RIGOL_BIND_ADDR,
			&addr.sin_addr);
		if (zsock_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
		    zsock_listen(fd, 1U) < 0) {
			linkr_debugger_rigol_close_fd(&fd);
			k_msleep(500);
			continue;
		}
		linkr_debugger_rigol_bl.listen_fd = fd;
	}

	while (true) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client_fd = zsock_accept(linkr_debugger_rigol_bl.listen_fd,
			(struct sockaddr *)&client_addr, &client_len);

		if (client_fd < 0) {
			k_msleep(20);
			continue;
		}
		if (linkr_debugger_rigol_bl.client_fd >= 0) {
			linkr_debugger_rigol_close_fd(&client_fd);
			continue;
		}
		linkr_debugger_rigol_bl.client_fd = client_fd;
		linkr_debugger_rigol_bl_session(client_fd);
		linkr_debugger_rigol_close_fd(&linkr_debugger_rigol_bl.client_fd);
	}
}

#define LINKR_DEBUGGER_RIGOL_THREAD_STACK 4096U
static K_THREAD_STACK_DEFINE(linkr_debugger_rigol_dispatch_stack,
	LINKR_DEBUGGER_RIGOL_THREAD_STACK);
static K_THREAD_STACK_DEFINE(linkr_debugger_rigol_pump_stack,
	LINKR_DEBUGGER_RIGOL_THREAD_STACK);
static K_THREAD_STACK_DEFINE(linkr_debugger_rigol_deep_stack, 2048U);
static struct k_thread linkr_debugger_rigol_dispatch_thread_data;
static struct k_thread linkr_debugger_rigol_pump_thread_data;
static struct k_thread linkr_debugger_rigol_deep_thread_data;

void linkr_debugger_rigol_server_init(void)
{
	if (!adc_is_ready_dt(&linkr_debugger_rigol_adc_gp29) ||
	    adc_channel_setup_dt(&linkr_debugger_rigol_adc_gp29) < 0) {
		LOG_WRN("rigol: GP29 ADC setup failed, CH1 will read zero");
	}

	k_mutex_init(&linkr_debugger_rigol_deep_inst.stage_lock);
	k_sem_init(&linkr_debugger_rigol_deep_inst.stage_data, 0U,
		LINKR_DEBUGGER_RIGOL_DEEP_STAGE_BYTES);

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

	(void)k_thread_create(&linkr_debugger_rigol_deep_thread_data,
		linkr_debugger_rigol_deep_stack, 2048U,
		linkr_debugger_rigol_deep_flash_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
	(void)k_thread_name_set(&linkr_debugger_rigol_deep_thread_data, "rigol_deep");

	(void)k_thread_create(&linkr_debugger_rigol_bl_thread_data,
		linkr_debugger_rigol_bl_stack, 2048U,
		linkr_debugger_rigol_bl_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
	(void)k_thread_name_set(&linkr_debugger_rigol_bl_thread_data, "rigol_bl");
}
