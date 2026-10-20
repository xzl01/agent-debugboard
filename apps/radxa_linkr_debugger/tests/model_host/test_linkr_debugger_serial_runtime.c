/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_serial.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>

#define LINKR_DEBUGGER_SERIAL_NET_HOST_TEST 1
#include "../../src/linkr_debugger_serial_net.c"

#define FAKE_BUFFER_CAPACITY 4096U

struct fake_endpoint {
	struct device device;
	linkr_debugger_serial_irq_callback_t callback;
	void *callback_data;
	uint8_t rx[FAKE_BUFFER_CAPACITY];
	uint8_t tx[FAKE_BUFFER_CAPACITY];
	size_t rx_length;
	size_t rx_offset;
	size_t tx_length;
	int fill_limit;
	int fill_calls_until_block;
	int error_flags;
	unsigned int callback_set_count;
	unsigned int callback_count;
	unsigned int rx_enable_count;
	unsigned int rx_disable_count;
	unsigned int tx_enable_count;
	unsigned int tx_disable_count;
	bool rx_enabled;
	bool tx_enabled;
	bool tx_ready;
	bool wire_complete;
};

struct fake_hal {
	struct fake_endpoint uart;
	struct fake_endpoint cdc;
	struct linkr_debugger_serial_config cdc_config;
	struct linkr_debugger_serial_config configured;
	size_t uart_tx_length_at_configure;
	unsigned int configure_count;
	unsigned int configure_failures;
	unsigned int callback_depth;
	unsigned int max_callback_depth;
	uint32_t dtr;
	uint32_t rts;
	bool synchronous_tx_enable;
};

static const struct linkr_debugger_serial_config config_115200_8n1 = {
    .baud = 115200U,
    .data_bits = 8U,
    .parity = LINKR_DEBUGGER_SERIAL_PARITY_NONE,
    .stop_bits = 1U,
    .flow_control = LINKR_DEBUGGER_SERIAL_FLOW_NONE,
};

void (*linkr_debugger_serial_test_sem_take_hook)(void);
static struct fake_hal *pause_completion_hal;

static uint8_t fake_socket_rx[LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX];
static uint8_t fake_socket_tx[4096U];
static size_t fake_socket_rx_length;
static size_t fake_socket_tx_length;
static size_t fake_socket_send_limit = SIZE_MAX;
static bool fake_socket_send_eagain;
static unsigned int fake_socket_recv_calls;
static unsigned int serial_wake_calls[LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS];

void linkr_debugger_ws_publish_serial(uint8_t slot)
{
	assert(slot < ARRAY_SIZE(serial_wake_calls));
	serial_wake_calls[slot]++;
}

int zsock_socket(int family, int type, int protocol)
{
	(void)family;
	(void)type;
	(void)protocol;
	return 10;
}

int zsock_setsockopt(int fd, int level, int option, const void *value,
		     socklen_t length)
{
	(void)fd;
	(void)level;
	(void)option;
	(void)value;
	(void)length;
	return 0;
}

int zsock_fcntl(int fd, int command, int flags)
{
	(void)fd;
	(void)command;
	(void)flags;
	return 0;
}

int zsock_bind(int fd, const struct sockaddr *address, socklen_t length)
{
	(void)fd;
	(void)address;
	(void)length;
	return 0;
}

int zsock_listen(int fd, int backlog)
{
	(void)fd;
	(void)backlog;
	return 0;
}

int zsock_accept(int fd, struct sockaddr *address, socklen_t *length)
{
	(void)fd;
	(void)address;
	(void)length;
	errno = EAGAIN;
	return -1;
}

int zsock_poll(struct zsock_pollfd *fds, int count, int timeout)
{
	(void)fds;
	(void)count;
	(void)timeout;
	return 0;
}

ssize_t zsock_recv(int fd, void *buffer, size_t length, int flags)
{
	size_t count;

	(void)fd;
	(void)flags;
	fake_socket_recv_calls++;
	if (fake_socket_rx_length == 0U) {
		errno = EAGAIN;
		return -1;
	}
	count = MIN(length, fake_socket_rx_length);
	memcpy(buffer, fake_socket_rx, count);
	fake_socket_rx_length = 0U;
	return (ssize_t)count;
}

ssize_t zsock_send(int fd, const void *buffer, size_t length, int flags)
{
	size_t count = MIN(length, fake_socket_send_limit);

	(void)fd;
	(void)flags;
	if (fake_socket_send_eagain) {
		fake_socket_send_eagain = false;
		errno = EAGAIN;
		return -1;
	}
	assert(fake_socket_tx_length + count <= sizeof(fake_socket_tx));
	memcpy(fake_socket_tx + fake_socket_tx_length, buffer, count);
	fake_socket_tx_length += count;
	return (ssize_t)count;
}

int zsock_shutdown(int fd, int how)
{
	(void)fd;
	(void)how;
	return 0;
}

int zsock_close(int fd)
{
	(void)fd;
	return 0;
}

static struct fake_endpoint *endpoint_for(struct fake_hal *hal,
					  const struct device *dev)
{
	if (dev == &hal->uart.device) {
		return &hal->uart;
	}
	assert(dev == &hal->cdc.device);
	return &hal->cdc;
}

static bool fake_device_ready(void *context, const struct device *dev)
{
	(void)context;
	return dev != NULL && dev->ready;
}

static int fake_irq_callback_set(void *context, const struct device *dev,
				 linkr_debugger_serial_irq_callback_t callback,
				 void *user_data)
{
	struct fake_endpoint *endpoint = endpoint_for(context, dev);

	endpoint->callback = callback;
	endpoint->callback_data = user_data;
	endpoint->callback_set_count++;
	return 0;
}

static void fake_irq_update(void *context, const struct device *dev)
{
	(void)context;
	(void)dev;
}

static int fake_irq_is_pending(void *context, const struct device *dev)
{
	struct fake_endpoint *endpoint = endpoint_for(context, dev);

	return (endpoint->rx_enabled &&
		endpoint->rx_offset < endpoint->rx_length) ||
	       (endpoint->tx_enabled && endpoint->tx_ready);
}

static int fake_irq_rx_ready(void *context, const struct device *dev)
{
	struct fake_endpoint *endpoint = endpoint_for(context, dev);

	return endpoint->rx_enabled &&
	       endpoint->rx_offset < endpoint->rx_length;
}

static int fake_fifo_read(void *context, const struct device *dev,
			  uint8_t *data, int capacity)
{
	struct fake_endpoint *endpoint = endpoint_for(context, dev);
	size_t available = endpoint->rx_length - endpoint->rx_offset;
	size_t count;

	assert(capacity >= 0);
	count = available < (size_t)capacity ? available : (size_t)capacity;
	if (count != 0U) {
		memcpy(data, endpoint->rx + endpoint->rx_offset, count);
		endpoint->rx_offset += count;
	}
	return (int)count;
}

static int fake_err_check(void *context, const struct device *dev)
{
	struct fake_endpoint *endpoint = endpoint_for(context, dev);
	int errors = endpoint->error_flags;

	endpoint->error_flags = 0;
	return errors;
}

static int fake_irq_tx_ready(void *context, const struct device *dev)
{
	struct fake_endpoint *endpoint = endpoint_for(context, dev);

	return endpoint->tx_enabled && endpoint->tx_ready;
}

static int fake_fifo_fill(void *context, const struct device *dev,
			  const uint8_t *data, int length)
{
	struct fake_endpoint *endpoint = endpoint_for(context, dev);
	size_t count;

	assert(length >= 0);
	count = (size_t)length;
	if (endpoint->fill_limit >= 0 && count > (size_t)endpoint->fill_limit) {
		count = (size_t)endpoint->fill_limit;
	}
	assert(endpoint->tx_length + count <= sizeof(endpoint->tx));
	memcpy(endpoint->tx + endpoint->tx_length, data, count);
	endpoint->tx_length += count;
	if (endpoint == &((struct fake_hal *)context)->uart && count != 0U) {
		endpoint->wire_complete = false;
	}
	if (endpoint->fill_calls_until_block > 0) {
		endpoint->fill_calls_until_block--;
		if (endpoint->fill_calls_until_block == 0) {
			endpoint->tx_ready = false;
		}
	}
	return (int)count;
}

static int fake_irq_tx_complete(void *context, const struct device *dev)
{
	struct fake_endpoint *endpoint = endpoint_for(context, dev);

	return endpoint->wire_complete ? 1 : 0;
}

static void fake_irq_rx_enable(void *context, const struct device *dev)
{
	struct fake_endpoint *endpoint = endpoint_for(context, dev);

	endpoint->rx_enabled = true;
	endpoint->rx_enable_count++;
}

static void fake_irq_rx_disable(void *context, const struct device *dev)
{
	struct fake_endpoint *endpoint = endpoint_for(context, dev);

	endpoint->rx_enabled = false;
	endpoint->rx_disable_count++;
}

static void fake_invoke(struct fake_hal *hal, struct fake_endpoint *endpoint)
{
	if (endpoint->callback == NULL) {
		return;
	}
	hal->callback_depth++;
	if (hal->callback_depth > hal->max_callback_depth) {
		hal->max_callback_depth = hal->callback_depth;
	}
	endpoint->callback_count++;
	endpoint->callback(&endpoint->device, endpoint->callback_data);
	hal->callback_depth--;
}

static void fake_irq_tx_enable(void *context, const struct device *dev)
{
	struct fake_hal *hal = context;
	struct fake_endpoint *endpoint = endpoint_for(hal, dev);
	unsigned int synchronous_callbacks = 0U;

	endpoint->tx_enabled = true;
	endpoint->tx_enable_count++;
	if (hal->synchronous_tx_enable && endpoint == &hal->uart) {
		while (endpoint->tx_enabled) {
			assert(synchronous_callbacks++ < 64U);
			fake_invoke(hal, endpoint);
		}
	} else if (hal->synchronous_tx_enable) {
		fake_invoke(hal, endpoint);
	}
}

static void fake_irq_tx_disable(void *context, const struct device *dev)
{
	struct fake_endpoint *endpoint = endpoint_for(context, dev);

	endpoint->tx_enabled = false;
	endpoint->tx_disable_count++;
}

static int fake_config_get(void *context, const struct device *dev,
			   struct linkr_debugger_serial_config *config)
{
	struct fake_hal *hal = context;

	assert(dev == &hal->cdc.device);
	*config = hal->cdc_config;
	return 0;
}

static int fake_configure(void *context, const struct device *dev,
			  const struct linkr_debugger_serial_config *config)
{
	struct fake_hal *hal = context;

	assert(dev == &hal->uart.device);
	hal->uart_tx_length_at_configure = hal->uart.tx_length;
	hal->configure_count++;
	if (hal->configure_failures != 0U) {
		hal->configure_failures--;
		return -EIO;
	}
	hal->configured = *config;
	return 0;
}

static int
fake_line_control_get(void *context, const struct device *dev,
		      enum linkr_debugger_serial_line_control control,
		      uint32_t *value)
{
	struct fake_hal *hal = context;

	assert(dev == &hal->cdc.device);
	*value =
	    control == LINKR_DEBUGGER_SERIAL_LINE_DTR ? hal->dtr : hal->rts;
	return 0;
}

static const struct linkr_debugger_serial_hal_ops fake_ops = {
    .device_ready = fake_device_ready,
    .irq_callback_set = fake_irq_callback_set,
    .irq_update = fake_irq_update,
    .irq_is_pending = fake_irq_is_pending,
    .irq_rx_ready = fake_irq_rx_ready,
    .fifo_read = fake_fifo_read,
    .err_check = fake_err_check,
    .irq_tx_ready = fake_irq_tx_ready,
    .fifo_fill = fake_fifo_fill,
    .irq_tx_complete = fake_irq_tx_complete,
    .irq_rx_enable = fake_irq_rx_enable,
    .irq_rx_disable = fake_irq_rx_disable,
    .irq_tx_enable = fake_irq_tx_enable,
    .irq_tx_disable = fake_irq_tx_disable,
    .config_get = fake_config_get,
    .configure = fake_configure,
    .line_control_get = fake_line_control_get,
};

static void fake_append_rx(struct fake_endpoint *endpoint, const uint8_t *data,
			   size_t length)
{
	if (endpoint->rx_offset == endpoint->rx_length) {
		endpoint->rx_offset = 0U;
		endpoint->rx_length = 0U;
	}
	assert(endpoint->rx_length + length <= sizeof(endpoint->rx));
	memcpy(endpoint->rx + endpoint->rx_length, data, length);
	endpoint->rx_length += length;
}

static void fake_allow_fill(struct fake_endpoint *endpoint, int limit,
			    int calls_until_block)
{
	endpoint->fill_limit = limit;
	endpoint->fill_calls_until_block = calls_until_block;
	endpoint->tx_ready = true;
}

static void complete_uart_wire(struct fake_hal *hal)
{
	hal->uart.wire_complete = true;
	fake_invoke(hal, &hal->uart);
	hal->uart.wire_complete = false;
	linkr_debugger_serial_process_events();
}

static void complete_uart_during_pause(void)
{
	struct linkr_debugger_serial_status status;

	assert(pause_completion_hal != NULL);
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.state == LINKR_DEBUGGER_SERIAL_READY);
	assert(status.admission_frozen);
	assert(status.pause_waiting_for_tx);
	assert(status.pause_wait_budget_ms > 0U &&
	       status.pause_wait_budget_ms <= 650U);
	pause_completion_hal->uart.wire_complete = true;
	fake_invoke(pause_completion_hal, &pause_completion_hal->uart);
	pause_completion_hal->uart.wire_complete = false;
}

static void drain_completions(void)
{
	struct linkr_debugger_serial_completion completion;

	while (linkr_debugger_serial_completion_poll(&completion) == 0) {
	}
}

static void assert_status_state(enum linkr_debugger_serial_state state)
{
	struct linkr_debugger_serial_status status;

	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.state == state);
}

static void test_uart_rx_fanout_and_cdc_pending(struct fake_hal *hal)
{
	static const uint8_t payload[] = {0x00U, 0xffU, 0x41U, 0x42U, 0x7fU};
	struct linkr_debugger_serial_sink_pending pending = {0};
	const uint8_t *data;
	size_t length;

	assert(linkr_debugger_serial_runtime_sink_register(
		   LINKR_DEBUGGER_SERIAL_SINK_WS0) == 0);
	assert(linkr_debugger_serial_runtime_sink_attach(
		   LINKR_DEBUGGER_SERIAL_SINK_WS0) == 0);
	hal->dtr = 1U;
	hal->rts = 1U;
	assert(linkr_debugger_serial_cdc_control_line(&hal->cdc.device) == 0);
	linkr_debugger_serial_process_events();

	fake_allow_fill(&hal->cdc, 2, 1);
	fake_append_rx(&hal->uart, payload, sizeof(payload));
	fake_invoke(hal, &hal->uart);
	assert(hal->cdc.tx_length == 2U);
	assert(memcmp(hal->cdc.tx, payload, 2U) == 0);

	assert(linkr_debugger_serial_runtime_sink_read(
		   LINKR_DEBUGGER_SERIAL_SINK_WS0, &pending, sizeof(payload),
		   &data, &length) == 0);
	assert(length == sizeof(payload));
	assert(memcmp(data, payload, sizeof(payload)) == 0);
	assert(linkr_debugger_serial_runtime_sink_consume(&pending, 2U) == 0);
	assert(linkr_debugger_serial_runtime_sink_read(
		   LINKR_DEBUGGER_SERIAL_SINK_WS0, &pending, sizeof(payload),
		   &data, &length) == 0);
	assert(length == sizeof(payload) - 2U);
	assert(memcmp(data, payload + 2U, length) == 0);
	assert(linkr_debugger_serial_runtime_sink_consume(&pending, length) ==
	       0);

	fake_allow_fill(&hal->cdc, 8, -1);
	fake_invoke(hal, &hal->cdc);
	assert(hal->cdc.tx_length == sizeof(payload));
	assert(memcmp(hal->cdc.tx, payload, sizeof(payload)) == 0);
	assert(linkr_debugger_serial_runtime_sink_unregister(
		   LINKR_DEBUGGER_SERIAL_SINK_WS0) == 0);
}

static void test_dtr_detach_has_no_stale_replay(struct fake_hal *hal)
{
	static const uint8_t partial[] = {0x10U, 0x11U, 0x12U};
	static const uint8_t detached[] = {0x20U, 0x21U};
	static const uint8_t fresh[] = {0x30U, 0x31U};
	size_t start = hal->cdc.tx_length;

	fake_allow_fill(&hal->cdc, 1, 1);
	fake_append_rx(&hal->uart, partial, sizeof(partial));
	fake_invoke(hal, &hal->uart);
	assert(hal->cdc.tx_length == start + 1U);

	hal->dtr = 0U;
	assert(linkr_debugger_serial_cdc_control_line(&hal->cdc.device) == 0);
	linkr_debugger_serial_process_events();
	fake_append_rx(&hal->uart, detached, sizeof(detached));
	fake_invoke(hal, &hal->uart);

	hal->dtr = 1U;
	assert(linkr_debugger_serial_cdc_control_line(&hal->cdc.device) == 0);
	linkr_debugger_serial_process_events();
	fake_allow_fill(&hal->cdc, 8, -1);
	fake_append_rx(&hal->uart, fresh, sizeof(fresh));
	fake_invoke(hal, &hal->uart);
	assert(hal->cdc.tx_length == start + 1U + sizeof(fresh));
	assert(memcmp(hal->cdc.tx + start + 1U, fresh, sizeof(fresh)) == 0);
}

static void test_cdc_detach_edges_survive_event_coalescing(struct fake_hal *hal)
{
	static const uint8_t stale_dtr[] = {0x40U, 0x41U, 0x42U};
	static const uint8_t fresh_dtr[] = {0x43U, 0x44U};
	static const uint8_t stale_reset[] = {0x50U, 0x51U, 0x52U};
	static const uint8_t fresh_reset[] = {0x53U, 0x54U};
	struct linkr_debugger_serial_status status;
	size_t start = hal->cdc.tx_length;

	/* LOW followed by HIGH before the worker must still detach first. */
	fake_allow_fill(&hal->cdc, 1, 1);
	fake_append_rx(&hal->uart, stale_dtr, sizeof(stale_dtr));
	fake_invoke(hal, &hal->uart);
	assert(hal->cdc.tx_length == start + 1U);
	hal->dtr = 0U;
	assert(linkr_debugger_serial_cdc_control_line(&hal->cdc.device) == 0);
	hal->dtr = 1U;
	assert(linkr_debugger_serial_cdc_control_line(&hal->cdc.device) == 0);
	fake_allow_fill(&hal->cdc, 8, -1);
	linkr_debugger_serial_process_events();
	assert(hal->cdc.tx_length == start + 1U);
	fake_append_rx(&hal->uart, fresh_dtr, sizeof(fresh_dtr));
	fake_invoke(hal, &hal->uart);
	assert(hal->cdc.tx_length == start + 1U + sizeof(fresh_dtr));
	assert(memcmp(hal->cdc.tx + start + 1U, fresh_dtr, sizeof(fresh_dtr)) ==
	       0);

	/* RESET, CONFIGURATION, and DTR HIGH may also coalesce into one worker
	 * pass without retaining the pre-reset pending bytes.
	 */
	start = hal->cdc.tx_length;
	fake_allow_fill(&hal->cdc, 1, 1);
	fake_append_rx(&hal->uart, stale_reset, sizeof(stale_reset));
	fake_invoke(hal, &hal->uart);
	assert(hal->cdc.tx_length == start + 1U);
	assert(linkr_debugger_serial_cdc_usb_event(
		   &hal->cdc.device, LINKR_DEBUGGER_SERIAL_USB_RESET, false) ==
	       0);
	assert(linkr_debugger_serial_cdc_usb_event(
		   &hal->cdc.device, LINKR_DEBUGGER_SERIAL_USB_CONFIGURATION,
		   true) == 0);
	hal->dtr = 1U;
	assert(linkr_debugger_serial_cdc_control_line(&hal->cdc.device) == 0);
	fake_allow_fill(&hal->cdc, 8, -1);
	linkr_debugger_serial_process_events();
	assert(hal->cdc.tx_length == start + 1U);
	fake_append_rx(&hal->uart, fresh_reset, sizeof(fresh_reset));
	fake_invoke(hal, &hal->uart);
	assert(hal->cdc.tx_length == start + 1U + sizeof(fresh_reset));
	assert(memcmp(hal->cdc.tx + start + 1U, fresh_reset,
		      sizeof(fresh_reset)) == 0);
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.dtr);
	assert(status.usb_configured);
}

static void
test_uart_partial_no_interleave_and_wire_completion(struct fake_hal *hal)
{
	static const uint8_t first[] = {1U, 2U, 3U, 4U, 5U};
	static const uint8_t second[] = {0xa0U, 0xa1U, 0xa2U};
	struct linkr_debugger_serial_completion completion;
	size_t start = hal->uart.tx_length;
	uint64_t first_id;
	uint64_t second_id;

	fake_allow_fill(&hal->uart, 2, 1);
	assert(linkr_debugger_serial_runtime_tx_enqueue(
		   LINKR_DEBUGGER_SERIAL_TX_WS0, first, sizeof(first),
		   &first_id) == 0);
	assert(hal->uart.tx_length == start + 2U);
	assert(!hal->uart.tx_enabled);
	assert(linkr_debugger_serial_runtime_tx_enqueue(
		   LINKR_DEBUGGER_SERIAL_TX_TCP0, second, sizeof(second),
		   &second_id) == 0);

	fake_allow_fill(&hal->uart, 16, -1);
	linkr_debugger_serial_process_events();
	assert(hal->uart.tx_length == start + sizeof(first));
	assert(!hal->uart.tx_enabled);
	assert(memcmp(hal->uart.tx + start, first, sizeof(first)) == 0);
	assert(linkr_debugger_serial_completion_poll(&completion) == -EAGAIN);

	fake_invoke(hal, &hal->uart);
	assert(hal->uart.tx_length == start + sizeof(first));
	complete_uart_wire(hal);
	assert(hal->uart.tx_length == start + sizeof(first) + sizeof(second));
	assert(memcmp(hal->uart.tx + start + sizeof(first), second,
		      sizeof(second)) == 0);
	assert(linkr_debugger_serial_completion_poll(&completion) == 0);
	assert(completion.event_id == first_id);
	assert(completion.disposition == LINKR_DEBUGGER_SERIAL_COMPLETED);
	assert(linkr_debugger_serial_completion_poll(&completion) == -EAGAIN);

	complete_uart_wire(hal);
	assert(linkr_debugger_serial_completion_poll(&completion) == 0);
	assert(completion.event_id == second_id);
	assert(completion.disposition == LINKR_DEBUGGER_SERIAL_COMPLETED);
	assert(hal->max_callback_depth >= 1U);
}

static void test_cdc_rx_backpressure_and_resume(struct fake_hal *hal)
{
	static const uint8_t one = 0x61U;
	static const uint8_t two = 0x62U;
	static const uint8_t three = 0x63U;
	struct linkr_debugger_serial_status status;
	size_t start = hal->uart.tx_length;

	hal->uart.tx_ready = false;
	fake_append_rx(&hal->cdc, &one, 1U);
	fake_invoke(hal, &hal->cdc);
	fake_append_rx(&hal->cdc, &two, 1U);
	fake_invoke(hal, &hal->cdc);
	assert(!hal->cdc.rx_enabled);
	fake_append_rx(&hal->cdc, &three, 1U);
	fake_invoke(hal, &hal->cdc);
	assert(hal->cdc.rx_offset + 1U == hal->cdc.rx_length);
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.cdc_rx_backpressured);

	fake_allow_fill(&hal->uart, 8, -1);
	linkr_debugger_serial_process_events();
	complete_uart_wire(hal);
	assert(hal->cdc.rx_enabled);
	fake_invoke(hal, &hal->cdc);
	complete_uart_wire(hal);
	complete_uart_wire(hal);
	assert(hal->uart.tx_length == start + 3U);
	assert(hal->uart.tx[start] == one);
	assert(hal->uart.tx[start + 1U] == two);
	assert(hal->uart.tx[start + 2U] == three);
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.runtime_counters.cdc_rx_backpressure >= 1U);
	assert(!status.cdc_rx_backpressured);
	drain_completions();
}

static void test_pause_freezes_next_frame(struct fake_hal *hal)
{
	static const uint8_t active[] = {0x71U, 0x72U};
	static const uint8_t queued[] = {0x81U, 0x82U};
	struct linkr_debugger_serial_completion completion;
	size_t start = hal->uart.tx_length;
	uint64_t active_id;
	uint64_t queued_id;

	fake_allow_fill(&hal->uart, 8, -1);
	assert(linkr_debugger_serial_runtime_tx_enqueue(
		   LINKR_DEBUGGER_SERIAL_TX_WS2, active, sizeof(active),
		   &active_id) == 0);
	assert(linkr_debugger_serial_runtime_tx_enqueue(
		   LINKR_DEBUGGER_SERIAL_TX_TCP1, queued, sizeof(queued),
		   &queued_id) == 0);
	assert(hal->uart.tx_length == start + sizeof(active));

	pause_completion_hal = hal;
	linkr_debugger_serial_test_sem_take_hook = complete_uart_during_pause;
	assert(linkr_debugger_serial_runtime_pause() == 0);
	pause_completion_hal = NULL;
	assert_status_state(LINKR_DEBUGGER_SERIAL_PAUSED);
	assert(!hal->uart.rx_enabled);
	assert(hal->uart.tx_length == start + sizeof(active));
	assert(linkr_debugger_serial_completion_poll(&completion) == 0);
	assert(completion.event_id == active_id);
	assert(completion.disposition == LINKR_DEBUGGER_SERIAL_COMPLETED);
	assert(linkr_debugger_serial_completion_poll(&completion) == 0);
	assert(completion.event_id == queued_id);
	assert(completion.disposition == LINKR_DEBUGGER_SERIAL_CANCELED);
	assert(linkr_debugger_serial_runtime_resume() == 0);
}

static void
test_config_retries_after_active_wire_completion(struct fake_hal *hal)
{
	static const uint8_t active[] = {0x91U, 0x92U};
	static const uint8_t queued_first[] = {0x93U, 0x94U};
	static const uint8_t queued_second[] = {0x95U, 0x96U};
	static const uint8_t producer = 0x97U;
	struct linkr_debugger_serial_completion completion;
	struct linkr_debugger_serial_status status;
	size_t start = hal->uart.tx_length;
	uint64_t active_id;
	uint64_t first_id;
	uint64_t second_id;
	uint64_t rejected_id;
	unsigned int configure_count = hal->configure_count;
	size_t i;

	fake_allow_fill(&hal->uart, 8, -1);
	assert(linkr_debugger_serial_runtime_tx_enqueue(
		   LINKR_DEBUGGER_SERIAL_TX_WS3, active, sizeof(active),
		   &active_id) == 0);
	assert(linkr_debugger_serial_runtime_tx_enqueue(
		   LINKR_DEBUGGER_SERIAL_TX_WS0, queued_first,
		   sizeof(queued_first), &first_id) == 0);
	assert(linkr_debugger_serial_runtime_tx_enqueue(
		   LINKR_DEBUGGER_SERIAL_TX_TCP1, queued_second,
		   sizeof(queued_second), &second_id) == 0);
	assert(hal->uart.tx_length == start + sizeof(active));
	hal->cdc_config = config_115200_8n1;
	hal->cdc_config.baud = 230400U;
	assert(linkr_debugger_serial_cdc_line_coding(&hal->cdc.device) == 0);
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.admission_frozen);
	for (i = 0U; i < 16U; ++i) {
		rejected_id = UINT64_MAX;
		assert(linkr_debugger_serial_runtime_tx_enqueue(
			   LINKR_DEBUGGER_SERIAL_TX_WS2, &producer, 1U,
			   &rejected_id) == -EAGAIN);
		assert(rejected_id == UINT64_MAX);
	}
	linkr_debugger_serial_process_events();
	assert(hal->configure_count == configure_count);
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.active_event_id == active_id);
	assert(status.admission_frozen);
	assert(hal->uart.tx_length == start + sizeof(active));

	complete_uart_wire(hal);
	assert(hal->configure_count == configure_count + 1U);
	assert(hal->uart_tx_length_at_configure == start + sizeof(active));
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.current_config.baud == 230400U);
	assert(status.generation == 2U);
	assert(!status.admission_frozen);
	assert(linkr_debugger_serial_completion_poll(&completion) == 0);
	assert(completion.event_id == active_id);
	assert(hal->uart.tx_length ==
	       start + sizeof(active) + sizeof(queued_first));
	assert(memcmp(hal->uart.tx + start + sizeof(active), queued_first,
		      sizeof(queued_first)) == 0);
	complete_uart_wire(hal);
	assert(linkr_debugger_serial_completion_poll(&completion) == 0);
	assert(completion.event_id == first_id);
	assert(hal->uart.tx_length == start + sizeof(active) +
					  sizeof(queued_first) +
					  sizeof(queued_second));
	assert(
	    memcmp(hal->uart.tx + start + sizeof(active) + sizeof(queued_first),
		   queued_second, sizeof(queued_second)) == 0);
	complete_uart_wire(hal);
	assert(linkr_debugger_serial_completion_poll(&completion) == 0);
	assert(completion.event_id == second_id);
}

static void test_line_coding_control_and_config_cas(struct fake_hal *hal)
{
	struct linkr_debugger_serial_config next = config_115200_8n1;
	struct linkr_debugger_serial_config invalid = config_115200_8n1;
	struct linkr_debugger_serial_status status;
	uint64_t request_id;
	uint64_t generation;
	unsigned int configure_count = hal->configure_count;
	unsigned int uart_rx_disables = hal->uart.rx_disable_count;
	unsigned int uart_rx_enables = hal->uart.rx_enable_count;

	hal->rts = 0U;
	assert(linkr_debugger_serial_cdc_control_line(&hal->cdc.device) == 0);
	linkr_debugger_serial_process_events();
	assert(hal->configure_count == configure_count);

	hal->cdc_config = config_115200_8n1;
	hal->cdc_config.baud = 921600U;
	assert(linkr_debugger_serial_cdc_line_coding(&hal->cdc.device) == 0);
	linkr_debugger_serial_process_events();
	assert(hal->configure_count == configure_count + 1U);
	assert(hal->uart.rx_disable_count == uart_rx_disables + 1U);
	assert(hal->uart.rx_enable_count == uart_rx_enables + 1U);
	assert(hal->configured.baud == 921600U);
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.generation == 3U);
	assert(status.dtr);
	assert(!status.rts);
	assert(linkr_debugger_serial_cdc_usb_event(
		   &hal->cdc.device, LINKR_DEBUGGER_SERIAL_USB_CONFIGURATION,
		   true) == 0);
	linkr_debugger_serial_process_events();
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.usb_configured);

	hal->cdc_config.baud = 12345U;
	assert(linkr_debugger_serial_cdc_line_coding(&hal->cdc.device) ==
	       -EINVAL);
	linkr_debugger_serial_process_events();
	assert(hal->configure_count == configure_count + 1U);
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.runtime_counters.invalid_line_coding == 1U);

	invalid.baud = 12345U;
	assert(linkr_debugger_serial_config_cas(&invalid, status.generation,
						&request_id) == -EINVAL);
	next.baud = 460800U;
	assert(linkr_debugger_serial_config_cas(&next, 1U, &request_id) == 0);
	linkr_debugger_serial_process_events();
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.last_config_done);
	assert(status.last_config_request_id == request_id);
	assert(status.last_config_result ==
	       -EBUSY); /* DTR owns configuration. */

	hal->dtr = 0U;
	assert(linkr_debugger_serial_cdc_usb_event(
		   &hal->cdc.device, LINKR_DEBUGGER_SERIAL_USB_RESET, false) ==
	       0);
	linkr_debugger_serial_process_events();
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(!status.dtr);
	assert(!status.usb_configured);
	assert(linkr_debugger_serial_config_cas(&next, 1U, &request_id) == 0);
	linkr_debugger_serial_process_events();
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.last_config_result == -ESTALE);
	generation = status.generation;
	assert(linkr_debugger_serial_config_cas(&next, generation,
						&request_id) == 0);
	linkr_debugger_serial_process_events();
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.last_config_result == 0);
	assert(status.generation == generation + 1U);
	assert(status.current_config.baud == next.baud);
}

static void test_config_rollback_unfreezes_queued_tx(struct fake_hal *hal)
{
	static const uint8_t active[] = {0xa8U, 0xa9U};
	static const uint8_t queued[] = {0xaaU, 0xabU};
	static const uint8_t producer = 0xacU;
	struct linkr_debugger_serial_config next;
	struct linkr_debugger_serial_status before;
	struct linkr_debugger_serial_status after;
	struct linkr_debugger_serial_completion completion;
	size_t start = hal->uart.tx_length;
	uint64_t active_id;
	uint64_t queued_id;
	uint64_t request_id;
	uint64_t rejected_id;

	assert(linkr_debugger_serial_status_snapshot(&before) == 0);
	next = before.current_config;
	next.baud = 115200U;
	fake_allow_fill(&hal->uart, 8, -1);
	assert(linkr_debugger_serial_runtime_tx_enqueue(
		   LINKR_DEBUGGER_SERIAL_TX_WS2, active, sizeof(active),
		   &active_id) == 0);
	assert(linkr_debugger_serial_runtime_tx_enqueue(
		   LINKR_DEBUGGER_SERIAL_TX_TCP0, queued, sizeof(queued),
		   &queued_id) == 0);
	assert(linkr_debugger_serial_config_cas(&next, before.generation,
						&request_id) == 0);
	assert(linkr_debugger_serial_status_snapshot(&after) == 0);
	assert(after.admission_frozen);
	rejected_id = UINT64_MAX;
	assert(linkr_debugger_serial_runtime_tx_enqueue(
		   LINKR_DEBUGGER_SERIAL_TX_WS1, &producer, 1U, &rejected_id) ==
	       -EAGAIN);
	assert(rejected_id == UINT64_MAX);

	hal->configure_failures = 1U;
	complete_uart_wire(hal);
	assert(linkr_debugger_serial_status_snapshot(&after) == 0);
	assert(after.state == LINKR_DEBUGGER_SERIAL_READY);
	assert(!after.admission_frozen);
	assert(after.last_config_done);
	assert(after.last_config_request_id == request_id);
	assert(after.last_config_result == -EIO);
	assert(after.generation == before.generation);
	assert(linkr_debugger_serial_config_equal(&after.current_config,
						  &before.current_config));
	assert(after.config_counters.apply_failed ==
	       before.config_counters.apply_failed + 1U);
	assert(after.config_counters.rollback_succeeded ==
	       before.config_counters.rollback_succeeded + 1U);
	assert(hal->uart_tx_length_at_configure == start + sizeof(active));
	assert(linkr_debugger_serial_completion_poll(&completion) == 0);
	assert(completion.event_id == active_id);
	assert(hal->uart.tx_length == start + sizeof(active) + sizeof(queued));
	assert(memcmp(hal->uart.tx + start + sizeof(active), queued,
		      sizeof(queued)) == 0);
	complete_uart_wire(hal);
	assert(linkr_debugger_serial_completion_poll(&completion) == 0);
	assert(completion.event_id == queued_id);
}

static void test_pause_timeout_errors_and_resume(struct fake_hal *hal)
{
	static const uint8_t active[] = {0xb0U, 0xb1U, 0xb2U};
	static const uint8_t queued[] = {0xe0U, 0xe1U};
	static const uint8_t stale_uart[] = {0xc0U, 0xc1U};
	static const uint8_t paused_cdc[] = {0xd0U, 0xd1U};
	struct linkr_debugger_serial_config worst;
	struct linkr_debugger_serial_completion completion;
	struct linkr_debugger_serial_status status;
	size_t uart_start = hal->uart.tx_length;
	uint64_t active_id;
	uint64_t queued_id;
	uint64_t request_id;
	uint64_t canceled_before;
	unsigned int tx_enables;
	unsigned int uart_callback_sets = hal->uart.callback_set_count;
	unsigned int cdc_callback_sets = hal->cdc.callback_set_count;

	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	worst = status.current_config;
	worst.baud = 9600U;
	worst.data_bits = 8U;
	worst.parity = LINKR_DEBUGGER_SERIAL_PARITY_EVEN;
	worst.stop_bits = 2U;
	assert(linkr_debugger_serial_config_cas(&worst, status.generation,
						&request_id) == 0);
	linkr_debugger_serial_process_events();
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.current_config.baud == 9600U);
	assert(status.pause_wait_budget_ms == 650U);
	canceled_before = status.tx_canceled;

	fake_allow_fill(&hal->uart, 1, 1);
	assert(linkr_debugger_serial_runtime_tx_enqueue(
		   LINKR_DEBUGGER_SERIAL_TX_WS1, active, sizeof(active),
		   &active_id) == 0);
	assert(linkr_debugger_serial_runtime_tx_enqueue(
		   LINKR_DEBUGGER_SERIAL_TX_TCP0, queued, sizeof(queued),
		   &queued_id) == 0);
	assert(hal->uart.tx_length == uart_start + 1U);
	tx_enables = hal->uart.tx_enable_count;
	assert(linkr_debugger_serial_runtime_pause() == -ETIMEDOUT);
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.state == LINKR_DEBUGGER_SERIAL_READY);
	assert(!status.admission_frozen);
	assert(!status.pause_waiting_for_tx);
	assert(status.active_event_id == active_id);
	assert(status.tx_used_slots == 2U);
	assert(status.tx_canceled == canceled_before);
	assert(status.pause_wait_budget_ms == 650U);
	assert(status.runtime_counters.pause_wait_timeouts >= 1U);
	assert(hal->uart.rx_enabled);
	assert(hal->cdc.rx_enabled);
	assert(hal->uart.tx_enable_count > tx_enables);
	assert(linkr_debugger_serial_completion_poll(&completion) == -EAGAIN);

	/* The timed-out frame remains active and later completes before the
	 * queued frame, with no cancellation or byte loss.
	 */
	fake_allow_fill(&hal->uart, 8, -1);
	linkr_debugger_serial_process_events();
	assert(hal->uart.tx_length == uart_start + sizeof(active));
	assert(memcmp(hal->uart.tx + uart_start, active, sizeof(active)) == 0);
	complete_uart_wire(hal);
	assert(linkr_debugger_serial_completion_poll(&completion) == 0);
	assert(completion.event_id == active_id);
	assert(completion.disposition == LINKR_DEBUGGER_SERIAL_COMPLETED);
	assert(hal->uart.tx_length ==
	       uart_start + sizeof(active) + sizeof(queued));
	assert(memcmp(hal->uart.tx + uart_start + sizeof(active), queued,
		      sizeof(queued)) == 0);
	complete_uart_wire(hal);
	assert(linkr_debugger_serial_completion_poll(&completion) == 0);
	assert(completion.event_id == queued_id);
	assert(completion.disposition == LINKR_DEBUGGER_SERIAL_COMPLETED);
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.state == LINKR_DEBUGGER_SERIAL_READY);
	assert(status.tx_canceled == canceled_before);

	/* With no active frame the pause is immediate and can safely drain the
	 * UART boundary before the mux changes.
	 */
	fake_append_rx(&hal->uart, stale_uart, sizeof(stale_uart));
	hal->uart.error_flags = 0x5;
	assert(linkr_debugger_serial_runtime_pause() == 0);
	assert_status_state(LINKR_DEBUGGER_SERIAL_PAUSED);
	assert(!hal->uart.rx_enabled);
	fake_append_rx(&hal->cdc, paused_cdc, sizeof(paused_cdc));
	fake_invoke(hal, &hal->cdc);
	assert(hal->cdc.rx_offset == hal->cdc.rx_length);
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.runtime_counters.uart_rx_error_events >= 1U);
	assert((status.runtime_counters.uart_rx_error_mask & 0x5U) == 0x5U);
	assert(status.runtime_counters.uart_rx_pause_dropped >=
	       sizeof(stale_uart));
	assert(status.runtime_counters.cdc_rx_dropped >= sizeof(paused_cdc));

	assert(linkr_debugger_serial_runtime_resume() == 0);
	assert_status_state(LINKR_DEBUGGER_SERIAL_READY);
	assert(hal->uart.rx_enabled);
	assert(hal->cdc.rx_enabled);
	assert(hal->uart.callback_set_count == uart_callback_sets + 1U);
	assert(hal->cdc.callback_set_count == cdc_callback_sets + 1U);
}

static void
serial_adapter_ws_drain(uint8_t slot,
			struct linkr_debugger_serial_sink_pending *pending)
{
	const uint8_t *data;
	size_t length;

	do {
		assert(linkr_debugger_serial_net_ws_read(
			   slot, pending, LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX,
			   &data, &length) == 0);
		if (length != 0U) {
			assert(linkr_debugger_serial_net_ws_consume(
				   slot, pending, length) == 0);
		}
	} while (length != 0U);
}

static void
test_network_adapter_fanout_completion_and_raw_tcp(struct fake_hal *hal)
{
	static const uint8_t binary[] = {0x00U, 0xffU, 0x41U};
	static const uint8_t downlink[] = {1U, 2U, 3U, 4U, 5U};
	static const uint8_t raw_uplink[] = {0x00U, 0xffU, 0x7eU, 0x0dU};
	uint8_t fill[FAKE_BUFFER_CAPACITY];
	struct linkr_debugger_serial_sink_pending ws0_pending = {0};
	struct linkr_debugger_serial_sink_pending ws1_pending = {0};
	struct linkr_debugger_serial_net_status net_status;
	struct linkr_debugger_serial_status serial_status;
	struct linkr_debugger_serial_completion completion;
	struct zsock_pollfd pfd;
	const uint8_t *data;
	size_t length;
	size_t uart_start;
	unsigned int recv_calls;
	uint64_t ws0_event;
	uint64_t ws1_event;
	uint64_t rejected_event;

	memset(fill, 0xa5, sizeof(fill));
	assert(linkr_debugger_serial_net_init() == 0);
	assert(linkr_debugger_serial_net_init() == -EALREADY);
	assert(linkr_debugger_serial_net_ws_attach(0U) == 0);
	assert(linkr_debugger_serial_net_ws_attach(1U) == 0);
	assert(linkr_debugger_serial_status_snapshot(&serial_status) == 0);
	serial_net_observe_status(&serial_status);
	{
		unsigned int wake0 = serial_wake_calls[0];
		unsigned int wake1 = serial_wake_calls[1];

		hal->dtr = 1U;
		hal->rts = 1U;
		assert(linkr_debugger_serial_cdc_control_line(
			   &hal->cdc.device) == 0);
		linkr_debugger_serial_process_events();
		assert(linkr_debugger_serial_status_snapshot(&serial_status) ==
		       0);
		serial_net_observe_status(&serial_status);
		assert(serial_wake_calls[0] == wake0 + 1U);
		assert(serial_wake_calls[1] == wake1 + 1U);
		assert(serial_status.dtr && serial_status.rts);

		wake0 = serial_wake_calls[0];
		assert(linkr_debugger_serial_cdc_usb_event(
			   &hal->cdc.device,
			   LINKR_DEBUGGER_SERIAL_USB_CONFIGURATION, true) == 0);
		linkr_debugger_serial_process_events();
		assert(linkr_debugger_serial_status_snapshot(&serial_status) ==
		       0);
		serial_net_observe_status(&serial_status);
		assert(serial_wake_calls[0] == wake0 + 1U);
		assert(serial_status.usb_configured);

		wake0 = serial_wake_calls[0];
		serial_status.runtime_counters.completion_overflow++;
		serial_net_observe_status(&serial_status);
		assert(serial_wake_calls[0] == wake0 + 1U);
	}
	/* Leave CDC detached so the large slow-client fixture below does not
	 * consume its bounded fake TX buffer.
	 */
	hal->dtr = 0U;
	hal->rts = 0U;
	assert(linkr_debugger_serial_cdc_usb_event(
		   &hal->cdc.device, LINKR_DEBUGGER_SERIAL_USB_RESET, false) ==
	       0);
	linkr_debugger_serial_process_events();

	/* Distinct source quotas and dispatcher queues prevent one WS client
	 * from stealing another client's physical-wire completion.
	 */
	fake_allow_fill(&hal->uart, 32, -1);
	assert(linkr_debugger_serial_net_ws_write(0U, binary, sizeof(binary),
						  &ws0_event) == 0);
	assert(linkr_debugger_serial_net_ws_write(
		   2U, fill, LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX + 1U,
		   &rejected_event) == -EMSGSIZE);
	assert(linkr_debugger_serial_net_ws_write(0U, binary, sizeof(binary),
						  &rejected_event) == -EAGAIN);
	assert(linkr_debugger_serial_net_ws_write(1U, binary, sizeof(binary),
						  &ws1_event) == 0);
	complete_uart_wire(hal);
	serial_net_dispatch_completions();
	assert(linkr_debugger_serial_net_ws_completion_poll(0U, &completion) ==
	       0);
	assert(completion.event_id == ws0_event);
	assert(linkr_debugger_serial_net_ws_completion_poll(1U, &completion) ==
	       -EAGAIN);
	complete_uart_wire(hal);
	serial_net_dispatch_completions();
	assert(linkr_debugger_serial_net_ws_completion_poll(1U, &completion) ==
	       0);
	assert(completion.event_id == ws1_event);
	assert(serial_wake_calls[0] >= 1U);
	assert(serial_wake_calls[1] >= 1U);

	/* Disconnect invalidates completion ownership. Since the reviewed core
	 * has no per-source cancel call, the old wire event may finish, but it
	 * is counted stale and never acknowledged to the replacement session.
	 */
	assert(linkr_debugger_serial_net_ws_write(0U, binary, sizeof(binary),
						  &ws0_event) == 0);
	assert(linkr_debugger_serial_net_ws_detach(0U) == 0);
	assert(linkr_debugger_serial_net_ws_attach(0U) == 0);
	complete_uart_wire(hal);
	serial_net_dispatch_completions();
	assert(linkr_debugger_serial_net_ws_completion_poll(0U, &completion) ==
	       -EAGAIN);
	assert(linkr_debugger_serial_net_status_snapshot(&net_status) == 0);
	assert(net_status.ws[0].stale_completions == 1U);

	/* NUL and 0xff are delivered unchanged to both independently attached
	 * sinks.
	 */
	fake_append_rx(&hal->uart, binary, sizeof(binary));
	fake_invoke(hal, &hal->uart);
	assert(linkr_debugger_serial_net_ws_read(
		   0U, &ws0_pending, sizeof(binary), &data, &length) == 0);
	assert(length == sizeof(binary) && memcmp(data, binary, length) == 0);
	assert(linkr_debugger_serial_net_ws_consume(0U, &ws0_pending, length) ==
	       0);
	assert(linkr_debugger_serial_net_ws_read(
		   1U, &ws1_pending, sizeof(binary), &data, &length) == 0);
	assert(length == sizeof(binary) && memcmp(data, binary, length) == 0);
	assert(linkr_debugger_serial_net_ws_consume(1U, &ws1_pending, length) ==
	       0);

	/* Keep WS0 current while WS1 stalls for two ring capacities. Only WS1
	 * accrues inferred overwrite drops.
	 */
	for (size_t pass = 0U; pass < 2U; pass++) {
		fake_append_rx(&hal->uart, fill, sizeof(fill));
		fake_invoke(hal, &hal->uart);
		serial_adapter_ws_drain(0U, &ws0_pending);
	}
	assert(linkr_debugger_serial_net_ws_read(
		   1U, &ws1_pending, sizeof(binary), &data, &length) == 0);
	assert(length == sizeof(binary));
	assert(linkr_debugger_serial_net_status_snapshot(&net_status) == 0);
	assert(net_status.ws[0].inferred_rx_drops == 0U);
	assert(net_status.ws[1].inferred_rx_drops == FAKE_BUFFER_CAPACITY);
	assert(linkr_debugger_serial_net_ws_consume(1U, &ws1_pending, length) ==
	       0);

	/* The production TCP drain retains caller-owned pending data across
	 * short writes and consumes only bytes accepted by send().
	 */
	assert(serial_net_tcp_attach(0U, 42) == 0);
	fake_socket_tx_length = 0U;
	fake_socket_send_limit = 2U;
	fake_socket_send_eagain = true;
	fake_append_rx(&hal->uart, downlink, sizeof(downlink));
	fake_invoke(hal, &hal->uart);
	assert(linkr_debugger_serial_status_snapshot(&serial_status) == 0);
	/* Mimic the reviewed attach race: a stale post-attach snapshot claims
	 * the sink is current even though its core cursor still precedes this
	 * byte. The production periodic helper probes the sink regardless of
	 * rx_head.
	 */
	serial_net.tcp[0].endpoint.rx_position = serial_status.rx_head;
	serial_net_tcp_prepare_poll(0U, LINKR_DEBUGGER_SERIAL_READY, &pfd);
	assert(fake_socket_tx_length == 0U);
	assert((pfd.events & ZSOCK_POLLOUT) != 0);
	serial_net_tcp_process_poll(0U, LINKR_DEBUGGER_SERIAL_READY,
				    ZSOCK_POLLOUT);
	serial_net_tcp_process_poll(0U, LINKR_DEBUGGER_SERIAL_READY,
				    ZSOCK_POLLOUT);
	serial_net_tcp_process_poll(0U, LINKR_DEBUGGER_SERIAL_READY,
				    ZSOCK_POLLOUT);
	assert(fake_socket_tx_length == sizeof(downlink));
	assert(memcmp(fake_socket_tx, downlink, sizeof(downlink)) == 0);

	/* One recv (including NUL/0xff) becomes exactly one atomic serial TX;
	 * the socket is not read again while that source event is in flight.
	 */
	uart_start = hal->uart.tx_length;
	memcpy(fake_socket_rx, raw_uplink, sizeof(raw_uplink));
	fake_socket_rx_length = sizeof(raw_uplink);
	serial_net_tcp_receive(0U);
	assert(hal->uart.tx_length == uart_start + sizeof(raw_uplink));
	assert(memcmp(hal->uart.tx + uart_start, raw_uplink,
		      sizeof(raw_uplink)) == 0);
	recv_calls = fake_socket_recv_calls;
	memcpy(fake_socket_rx, binary, sizeof(binary));
	fake_socket_rx_length = sizeof(binary);
	serial_net_tcp_receive(0U);
	assert(fake_socket_recv_calls == recv_calls);
	complete_uart_wire(hal);
	serial_net_dispatch_completions();
	serial_net_tcp_receive(0U);
	assert(fake_socket_recv_calls == recv_calls + 1U);
	complete_uart_wire(hal);
	serial_net_dispatch_completions();
	assert(linkr_debugger_serial_runtime_pause() == 0);
	assert(linkr_debugger_serial_net_ws_read(0U, &ws0_pending,
						 sizeof(binary), &data,
						 &length) == -EAGAIN);
	serial_net_tcp_send(0U);
	assert(serial_net.tcp[0].endpoint.attached);
	recv_calls = fake_socket_recv_calls;
	memcpy(fake_socket_rx, binary, sizeof(binary));
	fake_socket_rx_length = sizeof(binary);
	/* Even a stale POLLIN sampled before PAUSED must not destructively
	 * recv. */
	serial_net_tcp_process_poll(0U, LINKR_DEBUGGER_SERIAL_READY,
				    ZSOCK_POLLIN);
	assert(fake_socket_recv_calls == recv_calls);
	assert(linkr_debugger_serial_runtime_resume() == 0);

	serial_net_tcp_close(0U);
	assert(linkr_debugger_serial_net_ws_detach(0U) == 0);
	assert(linkr_debugger_serial_net_ws_detach(1U) == 0);
}

static void test_config_rollback_fault(struct fake_hal *hal)
{
	struct linkr_debugger_serial_config next = config_115200_8n1;
	struct linkr_debugger_serial_status status;
	static const uint8_t byte = 0xeeU;
	uint64_t request_id;
	uint64_t event_id;
	uint64_t apply_failed;
	uint64_t rollback_failed;

	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	apply_failed = status.config_counters.apply_failed;
	rollback_failed = status.config_counters.rollback_failed;
	next.baud = 230400U;
	hal->configure_failures = 2U;
	assert(linkr_debugger_serial_config_cas(&next, status.generation,
						&request_id) == 0);
	linkr_debugger_serial_process_events();
	assert(linkr_debugger_serial_status_snapshot(&status) == 0);
	assert(status.state == LINKR_DEBUGGER_SERIAL_FAULTED);
	assert(status.last_config_done);
	assert(status.last_config_result == -EIO);
	assert(status.config_counters.apply_failed == apply_failed + 1U);
	assert(status.config_counters.rollback_failed == rollback_failed + 1U);
	assert(linkr_debugger_serial_runtime_tx_enqueue(
		   LINKR_DEBUGGER_SERIAL_TX_WS0, &byte, 1U, &event_id) ==
	       -EAGAIN);
}

int main(void)
{
	struct fake_hal hal = {
	    .uart =
		{
		    .device = {.name = "fake-uart", .ready = true},
		    .fill_limit = -1,
		},
	    .cdc =
		{
		    .device = {.name = "fake-cdc", .ready = true},
		    .fill_limit = -1,
		},
	    .cdc_config = config_115200_8n1,
	    .synchronous_tx_enable = true,
	};
	struct linkr_debugger_serial_runtime_config config = {
	    .uart = &hal.uart.device,
	    .cdc = &hal.cdc.device,
	    .initial_config = config_115200_8n1,
	    .ops = &fake_ops,
	    .hal_context = &hal,
	};
	struct linkr_debugger_serial_runtime_config invalid = config;

	assert(linkr_debugger_serial_init(NULL) == -EINVAL);
	invalid.cdc = invalid.uart;
	assert(linkr_debugger_serial_init(&invalid) == -EINVAL);
	assert(linkr_debugger_serial_init(&config) == 0);
	assert(linkr_debugger_serial_init(&config) == -EALREADY);
	assert(hal.uart.callback_set_count == 1U);
	assert(hal.cdc.callback_set_count == 1U);
	assert(hal.uart.rx_enabled);
	assert(hal.cdc.rx_enabled);

	test_uart_rx_fanout_and_cdc_pending(&hal);
	test_dtr_detach_has_no_stale_replay(&hal);
	test_cdc_detach_edges_survive_event_coalescing(&hal);
	test_uart_partial_no_interleave_and_wire_completion(&hal);
	test_cdc_rx_backpressure_and_resume(&hal);
	test_pause_freezes_next_frame(&hal);
	test_config_retries_after_active_wire_completion(&hal);
	test_line_coding_control_and_config_cas(&hal);
	test_config_rollback_unfreezes_queued_tx(&hal);
	test_network_adapter_fanout_completion_and_raw_tcp(&hal);
	test_pause_timeout_errors_and_resume(&hal);
	test_config_rollback_fault(&hal);
	return 0;
}
