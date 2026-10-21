/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_serial_net.h"

#ifndef LINKR_DEBUGGER_SERIAL_NET_HOST_TEST
#include "linkr_debugger_ws.h"
#else
void linkr_debugger_ws_publish_serial(uint8_t slot);
#endif

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(linkr_debugger_serial_net, CONFIG_LINKR_DEBUGGER_LOG_LEVEL);

#define SERIAL_NET_PRIORITY K_PRIO_PREEMPT(9)
#define SERIAL_NET_POLL_MS 10
#define SERIAL_NET_RETRY_MS 100
#define SERIAL_NET_COMPLETION_BUDGET 16U
#define SERIAL_NET_STATIC_MAX (11U * 1024U)

struct serial_net_endpoint {
	struct linkr_debugger_serial_net_endpoint_counters counters;
	uint64_t rx_position;
	uint64_t tx_event_id;
	bool attached;
	bool tx_event_valid;
};

struct serial_net_ws_endpoint {
	struct serial_net_endpoint endpoint;
	struct linkr_debugger_serial_completion completion;
	bool completion_valid;
};

struct serial_net_tcp_endpoint {
	struct serial_net_endpoint endpoint;
	struct linkr_debugger_serial_sink_pending pending;
	uint8_t rx_buffer[LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX];
	size_t rx_length;
	int fd;
};

struct serial_net_runtime {
	struct k_mutex lock;
	struct k_thread thread;
	struct linkr_debugger_serial_net_counters counters;
	struct serial_net_ws_endpoint ws[LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS];
	struct serial_net_tcp_endpoint
		tcp[LINKR_DEBUGGER_SERIAL_RAW_TCP_MAX_CLIENTS];
	struct linkr_debugger_serial_status observed_status;
	struct linkr_debugger_serial_terminal_responder terminal_responder;
	uint64_t responder_generation;
	int64_t listener_retry_at_ms;
	int listen_fd;
	bool observed_status_valid;
	bool listening;
};

static struct serial_net_runtime serial_net;
static atomic_t serial_net_init_state = ATOMIC_INIT(0);
K_THREAD_STACK_DEFINE(serial_net_stack, LINKR_DEBUGGER_SERIAL_NET_STACK_SIZE);

BUILD_ASSERT(LINKR_DEBUGGER_SERIAL_RAW_TCP_PORT != 5556U,
	     "J4 raw TCP must not collide with the sigrok port");
BUILD_ASSERT(SERIAL_NET_POLL_MS <= 20,
	     "serial status and sink probes must run at least every 20 ms");
BUILD_ASSERT(sizeof(struct serial_net_runtime) +
		     K_THREAD_STACK_SIZEOF(serial_net_stack) +
		     sizeof(serial_net_init_state) <= SERIAL_NET_STATIC_MAX,
	     "serial network adapter exceeds its static memory budget");

static void counter_add(uint64_t *counter, uint64_t amount)
{
	if (UINT64_MAX - *counter < amount) {
		*counter = UINT64_MAX;
	} else {
		*counter += amount;
	}
}

static enum linkr_debugger_serial_sink ws_sink(uint8_t slot);

static struct linkr_debugger_serial_terminal_responder
serial_net_responder_select_locked(void)
{
	struct linkr_debugger_serial_terminal_responder next = { 0 };

	if (serial_net.observed_status_valid && serial_net.observed_status.dtr) {
		next.valid = true;
		next.transport = LINKR_DEBUGGER_SERIAL_TERMINAL_RESPONDER_CDC;
		next.slot = 0;
	} else {
		for (size_t i = 0U; i < ARRAY_SIZE(serial_net.ws); ++i) {
			if (serial_net.ws[i].endpoint.attached) {
				next.valid = true;
				next.transport =
					LINKR_DEBUGGER_SERIAL_TERMINAL_RESPONDER_WEBSOCKET;
				next.slot = (int8_t)i;
				break;
			}
		}
	}
	return next;
}

static bool serial_net_responder_recompute(void)
{
	for (size_t attempt = 0U; attempt < 3U; ++attempt) {
		struct linkr_debugger_serial_terminal_responder current;
		struct linkr_debugger_serial_terminal_responder next;
		uint64_t current_generation;
		uint64_t next_generation;
		bool descriptor_changed;
		bool force_ws_boundary;
		bool promote_ws;

		k_mutex_lock(&serial_net.lock, K_FOREVER);
		current = serial_net.terminal_responder;
		next = serial_net_responder_select_locked();
		current_generation = serial_net.responder_generation;
		next_generation =
			serial_net.observed_status.cdc_responder_generation;
		k_mutex_unlock(&serial_net.lock);
		descriptor_changed = memcmp(&current, &next, sizeof(next)) != 0;
		force_ws_boundary = next.valid &&
			next.transport ==
				LINKR_DEBUGGER_SERIAL_TERMINAL_RESPONDER_WEBSOCKET &&
			next_generation != current_generation;
		if (!descriptor_changed && !force_ws_boundary) {
			if (next_generation != current_generation) {
				k_mutex_lock(&serial_net.lock, K_FOREVER);
				current = serial_net_responder_select_locked();
				if (memcmp(&current, &next, sizeof(next)) == 0 &&
				    serial_net.observed_status.cdc_responder_generation ==
					    next_generation) {
					serial_net.responder_generation =
						next_generation;
				}
				k_mutex_unlock(&serial_net.lock);
			}
			return false;
		}
		promote_ws = next.valid &&
			next.transport ==
				LINKR_DEBUGGER_SERIAL_TERMINAL_RESPONDER_WEBSOCKET;
		/* Never hold the network mutex while acquiring the serial core
		 * spinlock. Promotion is published only after the fresh boundary.
		 */
		if (promote_ws && linkr_debugger_serial_runtime_sink_rearm(
				  ws_sink((uint8_t)next.slot)) < 0) {
			return false;
		}

		k_mutex_lock(&serial_net.lock, K_FOREVER);
		current = serial_net_responder_select_locked();
		if (memcmp(&current, &next, sizeof(next)) == 0 &&
		    serial_net.observed_status.cdc_responder_generation ==
			    next_generation) {
			serial_net.terminal_responder = next;
			serial_net.responder_generation = next_generation;
			k_mutex_unlock(&serial_net.lock);
			return true;
		}
		k_mutex_unlock(&serial_net.lock);
	}
	return false;
}

static void serial_net_wake_all_ws(void)
{
	bool wake[LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS];

	k_mutex_lock(&serial_net.lock, K_FOREVER);
	for (size_t i = 0U; i < ARRAY_SIZE(serial_net.ws); ++i) {
		wake[i] = serial_net.ws[i].endpoint.attached;
	}
	k_mutex_unlock(&serial_net.lock);
	for (size_t i = 0U; i < ARRAY_SIZE(wake); ++i) {
		if (wake[i]) {
			linkr_debugger_ws_publish_serial((uint8_t)i);
		}
	}
}

static enum linkr_debugger_serial_sink ws_sink(uint8_t slot)
{
	return (enum linkr_debugger_serial_sink)(LINKR_DEBUGGER_SERIAL_SINK_WS0 +
						  slot);
}

static enum linkr_debugger_serial_tx_source ws_source(uint8_t slot)
{
	return (enum linkr_debugger_serial_tx_source)(
		LINKR_DEBUGGER_SERIAL_TX_WS0 + slot);
}

static enum linkr_debugger_serial_sink tcp_sink(uint8_t slot)
{
	return (enum linkr_debugger_serial_sink)(LINKR_DEBUGGER_SERIAL_SINK_TCP0 +
						  slot);
}

static enum linkr_debugger_serial_tx_source tcp_source(uint8_t slot)
{
	return (enum linkr_debugger_serial_tx_source)(
		LINKR_DEBUGGER_SERIAL_TX_TCP0 + slot);
}

static bool socket_would_block(void)
{
	return errno == EAGAIN || errno == EWOULDBLOCK;
}

static void endpoint_infer_drops_locked(
	struct serial_net_endpoint *endpoint, uint64_t rx_head)
{
	uint64_t oldest;

	if (rx_head <= LINKR_DEBUGGER_SERIAL_RX_CAPACITY) {
		oldest = 0U;
	} else {
		oldest = rx_head - LINKR_DEBUGGER_SERIAL_RX_CAPACITY;
	}
	if (endpoint->rx_position < oldest) {
		counter_add(&endpoint->counters.inferred_rx_drops,
			    oldest - endpoint->rx_position);
		endpoint->rx_position = oldest;
	}
}

static int endpoint_read_locked(
	struct serial_net_endpoint *endpoint, enum linkr_debugger_serial_sink sink,
	struct linkr_debugger_serial_sink_pending *pending, size_t capacity,
	const uint8_t **data, size_t *length)
{
	struct linkr_debugger_serial_status status;
	uint64_t old_epoch = pending->epoch;
	bool old_valid = pending->valid;
	int ret;

	ret = linkr_debugger_serial_status_snapshot(&status);
	if (ret < 0) {
		return ret;
	}
	endpoint_infer_drops_locked(endpoint, status.rx_head);
	ret = linkr_debugger_serial_runtime_sink_read(
		sink, pending, capacity, data, length);
	if (ret == 0 && pending->valid &&
	    (!old_valid || old_epoch != pending->epoch)) {
		endpoint->rx_position += pending->length;
	}
	return ret;
}

static void endpoints_reset_position_locked(uint64_t rx_head)
{
	for (size_t i = 0U; i < ARRAY_SIZE(serial_net.ws); ++i) {
		if (serial_net.ws[i].endpoint.attached) {
			serial_net.ws[i].endpoint.rx_position = rx_head;
		}
	}
	for (size_t i = 0U; i < ARRAY_SIZE(serial_net.tcp); ++i) {
		if (serial_net.tcp[i].endpoint.attached) {
			serial_net.tcp[i].endpoint.rx_position = rx_head;
			memset(&serial_net.tcp[i].pending, 0,
			       sizeof(serial_net.tcp[i].pending));
		}
	}
}

static void serial_net_dispatch_completions(void)
{
	struct linkr_debugger_serial_completion completion;
	bool wake[LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS] = { false };
	size_t count;

	for (count = 0U; count < SERIAL_NET_COMPLETION_BUDGET; ++count) {
		int ret = linkr_debugger_serial_completion_poll(&completion);
		int index;

		if (ret != 0) {
			break;
		}
		k_mutex_lock(&serial_net.lock, K_FOREVER);
		counter_add(&serial_net.counters.completion_dispatches, 1U);
		if (completion.source >= LINKR_DEBUGGER_SERIAL_TX_WS0 &&
		    completion.source <= LINKR_DEBUGGER_SERIAL_TX_WS3) {
			index = (int)completion.source -
				(int)LINKR_DEBUGGER_SERIAL_TX_WS0;
			if (serial_net.ws[index].endpoint.attached &&
			    serial_net.ws[index].endpoint.tx_event_valid &&
			    serial_net.ws[index].endpoint.tx_event_id ==
				    completion.event_id &&
			    !serial_net.ws[index].completion_valid) {
				serial_net.ws[index].endpoint.tx_event_valid = false;
				serial_net.ws[index].completion = completion;
				serial_net.ws[index].completion_valid = true;
				wake[index] = true;
			} else {
				counter_add(&serial_net.ws[index].endpoint.counters
						 .stale_completions,
					    1U);
			}
		} else if (completion.source >=
			       LINKR_DEBUGGER_SERIAL_TX_TCP0 &&
			   completion.source <= LINKR_DEBUGGER_SERIAL_TX_TCP1) {
			index = (int)completion.source -
				(int)LINKR_DEBUGGER_SERIAL_TX_TCP0;
			if (serial_net.tcp[index].endpoint.tx_event_valid &&
			    serial_net.tcp[index].endpoint.tx_event_id ==
				    completion.event_id) {
				serial_net.tcp[index].endpoint.tx_event_valid = false;
			} else {
				counter_add(&serial_net.tcp[index].endpoint.counters
						 .stale_completions,
					    1U);
			}
		} else {
			/* CDC is intentionally completion-silent, but the adapter still
			 * owns and drains the global queue.
			 */
			counter_add(&serial_net.counters.completion_orphans, 1U);
		}
		k_mutex_unlock(&serial_net.lock);
	}

	for (size_t i = 0U; i < ARRAY_SIZE(wake); ++i) {
		if (wake[i]) {
			linkr_debugger_ws_publish_serial((uint8_t)i);
		}
	}
}

static void serial_net_tcp_close(uint8_t slot)
{
	struct serial_net_tcp_endpoint *client = &serial_net.tcp[slot];
	int fd = client->fd;

	if (fd >= 0) {
		(void)zsock_shutdown(fd, ZSOCK_SHUT_RDWR);
		(void)zsock_close(fd);
	}
	if (client->endpoint.attached) {
		(void)linkr_debugger_serial_runtime_sink_unregister(tcp_sink(slot));
	}
	k_mutex_lock(&serial_net.lock, K_FOREVER);
	if (client->endpoint.attached) {
		counter_add(&client->endpoint.counters.disconnected, 1U);
	}
	counter_add(&client->endpoint.counters.rx_dropped, client->rx_length);
	client->fd = -1;
	client->endpoint.attached = false;
	client->endpoint.tx_event_valid = false;
	client->rx_length = 0U;
	memset(&client->pending, 0, sizeof(client->pending));
	k_mutex_unlock(&serial_net.lock);
}

static int serial_net_tcp_attach(uint8_t slot, int fd)
{
	struct linkr_debugger_serial_status status;
	int ret;

	ret = linkr_debugger_serial_runtime_sink_register(tcp_sink(slot));
	if (ret < 0) {
		return ret;
	}
	ret = linkr_debugger_serial_runtime_sink_attach(tcp_sink(slot));
	if (ret < 0) {
		(void)linkr_debugger_serial_runtime_sink_unregister(tcp_sink(slot));
		return ret;
	}
	ret = linkr_debugger_serial_status_snapshot(&status);
	if (ret < 0) {
		(void)linkr_debugger_serial_runtime_sink_unregister(tcp_sink(slot));
		return ret;
	}

	k_mutex_lock(&serial_net.lock, K_FOREVER);
	serial_net.tcp[slot].fd = fd;
	serial_net.tcp[slot].endpoint.attached = true;
	serial_net.tcp[slot].endpoint.tx_event_valid = false;
	serial_net.tcp[slot].endpoint.rx_position = status.rx_head;
	serial_net.tcp[slot].rx_length = 0U;
	memset(&serial_net.tcp[slot].pending, 0,
	       sizeof(serial_net.tcp[slot].pending));
	counter_add(&serial_net.tcp[slot].endpoint.counters.connected, 1U);
	k_mutex_unlock(&serial_net.lock);
	return 0;
}

static void serial_net_accept(void)
{
	struct sockaddr_in peer;
	socklen_t peer_length = sizeof(peer);
	int slot = -1;
	int fd;

	fd = zsock_accept(serial_net.listen_fd, (struct sockaddr *)&peer,
			  &peer_length);
	if (fd < 0) {
		if (!socket_would_block()) {
			k_mutex_lock(&serial_net.lock, K_FOREVER);
			counter_add(&serial_net.counters.accept_failures, 1U);
			k_mutex_unlock(&serial_net.lock);
		}
		return;
	}
	(void)zsock_fcntl(fd, ZVFS_F_SETFL, ZVFS_O_NONBLOCK);

	k_mutex_lock(&serial_net.lock, K_FOREVER);
	for (size_t i = 0U; i < ARRAY_SIZE(serial_net.tcp); ++i) {
		if (!serial_net.tcp[i].endpoint.attached) {
			slot = (int)i;
			break;
		}
	}
	if (slot < 0) {
		counter_add(&serial_net.counters.refused_clients, 1U);
	}
	k_mutex_unlock(&serial_net.lock);
	if (slot < 0 || serial_net_tcp_attach((uint8_t)slot, fd) < 0) {
		(void)zsock_close(fd);
	}
}

static void serial_net_tcp_retry_pending(uint8_t slot)
{
	struct serial_net_tcp_endpoint *client = &serial_net.tcp[slot];
	uint64_t event_id;
	int ret;

	if (client->rx_length == 0U || client->endpoint.tx_event_valid) {
		return;
	}
	ret = linkr_debugger_serial_runtime_tx_enqueue(
		tcp_source(slot), client->rx_buffer, client->rx_length, &event_id);
	k_mutex_lock(&serial_net.lock, K_FOREVER);
	if (ret == 0) {
		client->endpoint.tx_event_id = event_id;
		client->endpoint.tx_event_valid = true;
		counter_add(&client->endpoint.counters.rx_bytes,
			    client->rx_length);
		counter_add(&client->endpoint.counters.rx_messages, 1U);
		client->rx_length = 0U;
	} else if (ret == -EAGAIN) {
		counter_add(&client->endpoint.counters.tx_queue_waits, 1U);
	}
	k_mutex_unlock(&serial_net.lock);
	if (ret != 0 && ret != -EAGAIN) {
		serial_net_tcp_close(slot);
	}
}

static void serial_net_tcp_receive(uint8_t slot)
{
	struct serial_net_tcp_endpoint *client = &serial_net.tcp[slot];
	struct linkr_debugger_serial_status status;
	int received;

	if (linkr_debugger_serial_status_snapshot(&status) < 0 ||
	    status.state != LINKR_DEBUGGER_SERIAL_READY) {
		return;
	}
	if (client->rx_length != 0U || client->endpoint.tx_event_valid) {
		return;
	}
	received = zsock_recv(client->fd, client->rx_buffer,
			      sizeof(client->rx_buffer), ZSOCK_MSG_DONTWAIT);
	if (received == 0) {
		serial_net_tcp_close(slot);
		return;
	}
	if (received < 0) {
		if (!socket_would_block()) {
			serial_net_tcp_close(slot);
		}
		return;
	}
	client->rx_length = (size_t)received;
	serial_net_tcp_retry_pending(slot);
}

static void serial_net_tcp_send(uint8_t slot)
{
	struct serial_net_tcp_endpoint *client = &serial_net.tcp[slot];
	const uint8_t *data = NULL;
	size_t length = 0U;
	int sent;
	int ret;

	k_mutex_lock(&serial_net.lock, K_FOREVER);
	ret = endpoint_read_locked(&client->endpoint, tcp_sink(slot),
				   &client->pending,
				   LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX,
				   &data, &length);
	k_mutex_unlock(&serial_net.lock);
	if (ret < 0 || length == 0U) {
		if (ret < 0 && ret != -EAGAIN) {
			serial_net_tcp_close(slot);
		}
		return;
	}

	sent = zsock_send(client->fd, data, length, ZSOCK_MSG_DONTWAIT);
	if (sent < 0) {
		bool would_block = socket_would_block();

		k_mutex_lock(&serial_net.lock, K_FOREVER);
		if (would_block) {
			counter_add(&client->endpoint.counters.send_would_block, 1U);
			k_mutex_unlock(&serial_net.lock);
			return;
		}
		counter_add(&client->endpoint.counters.send_failures, 1U);
		k_mutex_unlock(&serial_net.lock);
		serial_net_tcp_close(slot);
		return;
	}
	if (sent == 0 || (size_t)sent > length) {
		k_mutex_lock(&serial_net.lock, K_FOREVER);
		counter_add(&client->endpoint.counters.send_failures, 1U);
		k_mutex_unlock(&serial_net.lock);
		serial_net_tcp_close(slot);
		return;
	}
	(void)linkr_debugger_serial_runtime_sink_consume(&client->pending,
						 (size_t)sent);
	k_mutex_lock(&serial_net.lock, K_FOREVER);
	counter_add(&client->endpoint.counters.tx_bytes, (uint64_t)sent);
	k_mutex_unlock(&serial_net.lock);
}

static int serial_net_listener_open(void)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = htons(LINKR_DEBUGGER_SERIAL_RAW_TCP_PORT),
		.sin_addr = { .s_addr = htonl(INADDR_ANY) },
	};
	int reuse = 1;
	int fd;

	fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		return -errno;
	}
	(void)zsock_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
			       sizeof(reuse));
	(void)zsock_fcntl(fd, ZVFS_F_SETFL, ZVFS_O_NONBLOCK);
	if (zsock_bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    zsock_listen(fd, LINKR_DEBUGGER_SERIAL_RAW_TCP_MAX_CLIENTS) < 0) {
		int error = errno;

		(void)zsock_close(fd);
		return -error;
	}
	return fd;
}

static void serial_net_observe_status(
	const struct linkr_debugger_serial_status *status)
{
	bool wake[LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS] = { false };
	bool responder_changed;
	bool changed;

	k_mutex_lock(&serial_net.lock, K_FOREVER);
	changed = !serial_net.observed_status_valid ||
		  memcmp(&serial_net.observed_status, status, sizeof(*status)) != 0;
	if (status->state == LINKR_DEBUGGER_SERIAL_PAUSED ||
	    status->state == LINKR_DEBUGGER_SERIAL_FAULTED) {
		endpoints_reset_position_locked(status->rx_head);
	}
	serial_net.observed_status = *status;
	serial_net.observed_status_valid = true;
	if (changed) {
		for (size_t i = 0U; i < ARRAY_SIZE(serial_net.ws); ++i) {
			wake[i] = serial_net.ws[i].endpoint.attached;
		}
	}
	k_mutex_unlock(&serial_net.lock);
	responder_changed = serial_net_responder_recompute();
	for (size_t i = 0U; i < ARRAY_SIZE(wake); ++i) {
		if (wake[i]) {
			linkr_debugger_ws_publish_serial((uint8_t)i);
		}
	}
	if (responder_changed && !changed) {
		serial_net_wake_all_ws();
	}
}

int linkr_debugger_serial_net_responder_sync(
	const struct linkr_debugger_serial_status *status)
{
	if (status == NULL) {
		return -EINVAL;
	}
	if (atomic_get(&serial_net_init_state) != 2) {
		return -ENODEV;
	}
	k_mutex_lock(&serial_net.lock, K_FOREVER);
	serial_net.observed_status = *status;
	serial_net.observed_status_valid = true;
	k_mutex_unlock(&serial_net.lock);
	if (serial_net_responder_recompute()) {
		serial_net_wake_all_ws();
	}
	return 0;
}

/* One deterministic client-lane iteration used by the production thread and
 * host tests. The unconditional bounded sink probe while READY avoids relying
 * on a non-atomic status.rx_head snapshot taken after sink attachment.
 */
static void serial_net_tcp_prepare_poll(
	uint8_t slot, enum linkr_debugger_serial_state state,
	struct zsock_pollfd *pfd)
{
	struct serial_net_tcp_endpoint *client = &serial_net.tcp[slot];

	memset(pfd, 0, sizeof(*pfd));
	pfd->fd = client->endpoint.attached ? client->fd : -1;
	if (!client->endpoint.attached) {
		return;
	}
	if (state == LINKR_DEBUGGER_SERIAL_READY) {
		serial_net_tcp_retry_pending(slot);
		serial_net_tcp_send(slot);
	} else if (client->pending.valid) {
		/* Revalidate a previously fetched payload against the sink epoch.
		 * PAUSED returns -EAGAIN and keeps the connection intact.
		 */
		serial_net_tcp_send(slot);
	}
	if (!client->endpoint.attached) {
		pfd->fd = -1;
		return;
	}
	if (state == LINKR_DEBUGGER_SERIAL_READY && client->rx_length == 0U &&
	    !client->endpoint.tx_event_valid) {
		pfd->events |= ZSOCK_POLLIN;
	}
	if (client->pending.valid) {
		pfd->events |= ZSOCK_POLLOUT;
	}
}

static void serial_net_tcp_process_poll(
	uint8_t slot, enum linkr_debugger_serial_state state, short revents)
{
	if (!serial_net.tcp[slot].endpoint.attached) {
		return;
	}
	if ((revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) != 0) {
		serial_net_tcp_close(slot);
		return;
	}
	if ((revents & ZSOCK_POLLOUT) != 0) {
		serial_net_tcp_send(slot);
	}
	if ((revents & ZSOCK_POLLIN) != 0 &&
	    state == LINKR_DEBUGGER_SERIAL_READY) {
		serial_net_tcp_receive(slot);
	}
}

static void serial_net_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct linkr_debugger_serial_status status;
		struct zsock_pollfd
			fds[1U + LINKR_DEBUGGER_SERIAL_RAW_TCP_MAX_CLIENTS];
		int ret;

		serial_net_dispatch_completions();
		if (linkr_debugger_serial_status_snapshot(&status) == 0) {
			serial_net_observe_status(&status);
		} else {
			memset(&status, 0, sizeof(status));
			status.state = LINKR_DEBUGGER_SERIAL_FAULTED;
		}

		if (serial_net.listen_fd < 0 &&
		    k_uptime_get() >= serial_net.listener_retry_at_ms) {
			serial_net.listen_fd = serial_net_listener_open();
			k_mutex_lock(&serial_net.lock, K_FOREVER);
			if (serial_net.listen_fd >= 0) {
				serial_net.listening = true;
				counter_add(&serial_net.counters.listener_starts, 1U);
			} else {
				serial_net.listening = false;
				serial_net.listener_retry_at_ms =
					k_uptime_get() + SERIAL_NET_RETRY_MS;
				counter_add(&serial_net.counters.listener_failures, 1U);
			}
			k_mutex_unlock(&serial_net.lock);
		}

		memset(fds, 0, sizeof(fds));
		fds[0].fd = serial_net.listen_fd;
		fds[0].events = serial_net.listen_fd >= 0 ? ZSOCK_POLLIN : 0;
		for (size_t i = 0U; i < ARRAY_SIZE(serial_net.tcp); ++i) {
			serial_net_tcp_prepare_poll((uint8_t)i, status.state,
						    &fds[i + 1U]);
		}

		ret = zsock_poll(fds, ARRAY_SIZE(fds), SERIAL_NET_POLL_MS);
		if (ret < 0) {
			k_mutex_lock(&serial_net.lock, K_FOREVER);
			counter_add(&serial_net.counters.poll_failures, 1U);
			k_mutex_unlock(&serial_net.lock);
			k_sleep(K_MSEC(SERIAL_NET_POLL_MS));
			continue;
		}
		if ((fds[0].revents & ZSOCK_POLLIN) != 0) {
			serial_net_accept();
		}
		if ((fds[0].revents &
		     (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) != 0) {
			(void)zsock_close(serial_net.listen_fd);
			serial_net.listen_fd = -1;
			k_mutex_lock(&serial_net.lock, K_FOREVER);
			serial_net.listening = false;
			k_mutex_unlock(&serial_net.lock);
		}
		for (size_t i = 0U; i < ARRAY_SIZE(serial_net.tcp); ++i) {
			serial_net_tcp_process_poll((uint8_t)i, status.state,
						    fds[i + 1U].revents);
		}
	}
}

int linkr_debugger_serial_net_init(void)
{
	if (!atomic_cas(&serial_net_init_state, 0, 1)) {
		return -EALREADY;
	}
	memset(&serial_net, 0, sizeof(serial_net));
	serial_net.listen_fd = -1;
	for (size_t i = 0U; i < ARRAY_SIZE(serial_net.tcp); ++i) {
		serial_net.tcp[i].fd = -1;
	}
	k_mutex_init(&serial_net.lock);
	atomic_set(&serial_net_init_state, 2);
	(void)k_thread_create(&serial_net.thread, serial_net_stack,
			      K_THREAD_STACK_SIZEOF(serial_net_stack),
			      serial_net_thread_entry, NULL, NULL, NULL,
			      SERIAL_NET_PRIORITY, 0U, K_NO_WAIT);
	if (IS_ENABLED(CONFIG_THREAD_NAME)) {
		k_thread_name_set(&serial_net.thread, "linkr_serial_net");
	}
	return 0;
}

int linkr_debugger_serial_net_status_snapshot(
	struct linkr_debugger_serial_net_status *status)
{
	if (status == NULL) {
		return -EINVAL;
	}
	memset(status, 0, sizeof(*status));
	status->tcp_port = LINKR_DEBUGGER_SERIAL_RAW_TCP_PORT;
	status->initialized = atomic_get(&serial_net_init_state) == 2;
	if (!status->initialized) {
		return 0;
	}

	k_mutex_lock(&serial_net.lock, K_FOREVER);
	status->counters = serial_net.counters;
	status->terminal_responder = serial_net.terminal_responder;
	status->listening = serial_net.listening;
	for (size_t i = 0U; i < ARRAY_SIZE(serial_net.ws); ++i) {
		status->ws[i] = serial_net.ws[i].endpoint.counters;
		if (serial_net.ws[i].endpoint.attached) {
			status->ws_attached++;
		}
	}
	for (size_t i = 0U; i < ARRAY_SIZE(serial_net.tcp); ++i) {
		status->tcp[i] = serial_net.tcp[i].endpoint.counters;
		if (serial_net.tcp[i].endpoint.attached) {
			status->tcp_connected++;
		}
	}
	k_mutex_unlock(&serial_net.lock);
	return 0;
}

int linkr_debugger_serial_net_ws_attach(uint8_t slot)
{
	struct linkr_debugger_serial_status status;
	int ret;

	if (slot >= LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS) {
		return -EINVAL;
	}
	if (atomic_get(&serial_net_init_state) != 2) {
		return -ENODEV;
	}
	k_mutex_lock(&serial_net.lock, K_FOREVER);
	if (serial_net.ws[slot].endpoint.attached) {
		k_mutex_unlock(&serial_net.lock);
		return -EALREADY;
	}
	k_mutex_unlock(&serial_net.lock);

	ret = linkr_debugger_serial_runtime_sink_register(ws_sink(slot));
	if (ret < 0) {
		return ret;
	}
	ret = linkr_debugger_serial_runtime_sink_attach(ws_sink(slot));
	if (ret < 0) {
		(void)linkr_debugger_serial_runtime_sink_unregister(ws_sink(slot));
		return ret;
	}
	ret = linkr_debugger_serial_status_snapshot(&status);
	if (ret < 0) {
		(void)linkr_debugger_serial_runtime_sink_unregister(ws_sink(slot));
		return ret;
	}

	k_mutex_lock(&serial_net.lock, K_FOREVER);
	serial_net.ws[slot].endpoint.attached = true;
	serial_net.ws[slot].endpoint.tx_event_valid = false;
	serial_net.ws[slot].endpoint.rx_position = status.rx_head;
	serial_net.ws[slot].completion_valid = false;
	serial_net.observed_status = status;
	serial_net.observed_status_valid = true;
	counter_add(&serial_net.ws[slot].endpoint.counters.connected, 1U);
	k_mutex_unlock(&serial_net.lock);
	if (serial_net_responder_recompute()) {
		serial_net_wake_all_ws();
	}
	return 0;
}

int linkr_debugger_serial_net_ws_detach(uint8_t slot)
{
	bool attached;
	int ret;

	if (slot >= LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS) {
		return -EINVAL;
	}
	if (atomic_get(&serial_net_init_state) != 2) {
		return -ENODEV;
	}
	k_mutex_lock(&serial_net.lock, K_FOREVER);
	attached = serial_net.ws[slot].endpoint.attached;
	k_mutex_unlock(&serial_net.lock);
	if (!attached) {
		return -ENOENT;
	}
	ret = linkr_debugger_serial_runtime_sink_unregister(ws_sink(slot));
	if (ret < 0) {
		return ret;
	}
	k_mutex_lock(&serial_net.lock, K_FOREVER);
	serial_net.ws[slot].endpoint.attached = false;
	serial_net.ws[slot].endpoint.tx_event_valid = false;
	serial_net.ws[slot].completion_valid = false;
	counter_add(&serial_net.ws[slot].endpoint.counters.disconnected, 1U);
	k_mutex_unlock(&serial_net.lock);
	if (serial_net_responder_recompute()) {
		serial_net_wake_all_ws();
	}
	return 0;
}

int linkr_debugger_serial_net_ws_read(
	uint8_t slot, struct linkr_debugger_serial_sink_pending *pending,
	size_t capacity, const uint8_t **data, size_t *length)
{
	int ret;

	if (slot >= LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS || pending == NULL ||
	    data == NULL || length == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&serial_net.lock, K_FOREVER);
	if (!serial_net.ws[slot].endpoint.attached) {
		k_mutex_unlock(&serial_net.lock);
		return -ENOENT;
	}
	ret = endpoint_read_locked(&serial_net.ws[slot].endpoint, ws_sink(slot),
				   pending, capacity, data, length);
	k_mutex_unlock(&serial_net.lock);
	return ret;
}

int linkr_debugger_serial_net_ws_consume(
	uint8_t slot, struct linkr_debugger_serial_sink_pending *pending,
	size_t accepted)
{
	int ret;

	if (slot >= LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS || pending == NULL) {
		return -EINVAL;
	}
	ret = linkr_debugger_serial_runtime_sink_consume(pending, accepted);
	if (ret == 0) {
		k_mutex_lock(&serial_net.lock, K_FOREVER);
		counter_add(&serial_net.ws[slot].endpoint.counters.tx_bytes,
			    accepted);
		k_mutex_unlock(&serial_net.lock);
	}
	return ret;
}

int linkr_debugger_serial_net_ws_pending_validate(
	uint8_t slot, struct linkr_debugger_serial_sink_pending *pending)
{
	int ret;

	if (slot >= LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS || pending == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&serial_net.lock, K_FOREVER);
	if (!serial_net.ws[slot].endpoint.attached) {
		k_mutex_unlock(&serial_net.lock);
		return -ENOENT;
	}
	ret = linkr_debugger_serial_runtime_sink_pending_validate(
		ws_sink(slot), pending);
	k_mutex_unlock(&serial_net.lock);
	return ret;
}

int linkr_debugger_serial_net_ws_write(
	uint8_t slot, const uint8_t *data, size_t length, uint64_t *event_id)
{
	int ret;

	if (slot >= LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS || data == NULL ||
	    event_id == NULL || length == 0U ||
	    length > LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX) {
		return length > LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX ? -EMSGSIZE :
								     -EINVAL;
	}
	k_mutex_lock(&serial_net.lock, K_FOREVER);
	if (!serial_net.ws[slot].endpoint.attached) {
		k_mutex_unlock(&serial_net.lock);
		return -ENOENT;
	}
	if (serial_net.ws[slot].endpoint.tx_event_valid ||
	    serial_net.ws[slot].completion_valid) {
		counter_add(&serial_net.ws[slot].endpoint.counters.tx_queue_waits,
			    1U);
		k_mutex_unlock(&serial_net.lock);
		return -EAGAIN;
	}
	ret = linkr_debugger_serial_runtime_tx_enqueue(ws_source(slot), data,
						       length, event_id);
	if (ret == 0) {
		serial_net.ws[slot].endpoint.tx_event_id = *event_id;
		serial_net.ws[slot].endpoint.tx_event_valid = true;
		counter_add(&serial_net.ws[slot].endpoint.counters.rx_bytes,
			    length);
		counter_add(&serial_net.ws[slot].endpoint.counters.rx_messages, 1U);
	} else if (ret == -EAGAIN) {
		counter_add(&serial_net.ws[slot].endpoint.counters.tx_queue_waits,
			    1U);
	}
	k_mutex_unlock(&serial_net.lock);
	return ret;
}

int linkr_debugger_serial_net_ws_completion_poll(
	uint8_t slot, struct linkr_debugger_serial_completion *completion)
{
	if (slot >= LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS || completion == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&serial_net.lock, K_FOREVER);
	if (!serial_net.ws[slot].endpoint.attached) {
		k_mutex_unlock(&serial_net.lock);
		return -ENOENT;
	}
	if (!serial_net.ws[slot].completion_valid) {
		k_mutex_unlock(&serial_net.lock);
		return -EAGAIN;
	}
	*completion = serial_net.ws[slot].completion;
	serial_net.ws[slot].completion_valid = false;
	k_mutex_unlock(&serial_net.lock);
	return 0;
}
