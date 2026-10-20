/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_SERIAL_NET_H_
#define RADXA_LINKR_DEBUGGER_SERIAL_NET_H_

#include "linkr_debugger_serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINKR_DEBUGGER_SERIAL_RAW_TCP_PORT 5555U
#define LINKR_DEBUGGER_SERIAL_RAW_TCP_MAX_CLIENTS 2U
#define LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS 4U
#define LINKR_DEBUGGER_SERIAL_NET_STACK_SIZE 2560U

struct linkr_debugger_serial_net_endpoint_counters {
	uint64_t connected;
	uint64_t disconnected;
	uint64_t rx_bytes;
	uint64_t rx_dropped;
	uint64_t tx_bytes;
	uint64_t rx_messages;
	uint64_t tx_queue_waits;
	uint64_t send_would_block;
	uint64_t send_failures;
	uint64_t inferred_rx_drops;
	uint64_t stale_completions;
};

struct linkr_debugger_serial_net_counters {
	uint64_t listener_starts;
	uint64_t listener_failures;
	uint64_t accept_failures;
	uint64_t refused_clients;
	uint64_t completion_dispatches;
	uint64_t completion_orphans;
	uint64_t poll_failures;
};

struct linkr_debugger_serial_net_status {
	struct linkr_debugger_serial_net_counters counters;
	struct linkr_debugger_serial_net_endpoint_counters
		ws[LINKR_DEBUGGER_SERIAL_WS_MAX_CLIENTS];
	struct linkr_debugger_serial_net_endpoint_counters
		tcp[LINKR_DEBUGGER_SERIAL_RAW_TCP_MAX_CLIENTS];
	uint16_t tcp_port;
	uint8_t ws_attached;
	uint8_t tcp_connected;
	bool initialized;
	bool listening;
};

int linkr_debugger_serial_net_init(void);
int linkr_debugger_serial_net_status_snapshot(
	struct linkr_debugger_serial_net_status *status);

/*
 * Each WebSocket slot has a fixed serial sink and TX source. The adapter is
 * the sole consumer of the runtime's global completion queue and retains at
 * most one completion per WS slot, so one client cannot consume another
 * client's completion. The reviewed serial core has no per-source cancel API;
 * detach therefore invalidates ownership and a later completion is discarded
 * as stale rather than being reported to a replacement session.
 */
int linkr_debugger_serial_net_ws_attach(uint8_t slot);
int linkr_debugger_serial_net_ws_detach(uint8_t slot);
int linkr_debugger_serial_net_ws_read(
	uint8_t slot, struct linkr_debugger_serial_sink_pending *pending,
	size_t capacity, const uint8_t **data, size_t *length);
int linkr_debugger_serial_net_ws_consume(
	uint8_t slot, struct linkr_debugger_serial_sink_pending *pending,
	size_t accepted);
int linkr_debugger_serial_net_ws_write(
	uint8_t slot, const uint8_t *data, size_t length, uint64_t *event_id);
int linkr_debugger_serial_net_ws_completion_poll(
	uint8_t slot, struct linkr_debugger_serial_completion *completion);

#endif /* RADXA_LINKR_DEBUGGER_SERIAL_NET_H_ */
