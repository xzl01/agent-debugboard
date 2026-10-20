/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_SERIAL_MODEL_H_
#define RADXA_LINKR_DEBUGGER_SERIAL_MODEL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * This is a pure state model. It performs no locking, allocation, logging, or
 * I/O. Callers must serialize every access to an instance, including reads of
 * its public counters and state.
 */

#define LINKR_DEBUGGER_SERIAL_RX_CAPACITY 4096U
#define LINKR_DEBUGGER_SERIAL_TX_SLOT_COUNT 8U
#define LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX 480U
#define LINKR_DEBUGGER_SERIAL_STATIC_MAX (14U * 1024U)
#define LINKR_DEBUGGER_SERIAL_BAUD_COUNT 9U

enum linkr_debugger_serial_sink {
	LINKR_DEBUGGER_SERIAL_SINK_CDC = 0,
	LINKR_DEBUGGER_SERIAL_SINK_WS0,
	LINKR_DEBUGGER_SERIAL_SINK_WS1,
	LINKR_DEBUGGER_SERIAL_SINK_WS2,
	LINKR_DEBUGGER_SERIAL_SINK_WS3,
	LINKR_DEBUGGER_SERIAL_SINK_TCP0,
	LINKR_DEBUGGER_SERIAL_SINK_TCP1,
	LINKR_DEBUGGER_SERIAL_SINK_COUNT,
};

enum linkr_debugger_serial_tx_source {
	LINKR_DEBUGGER_SERIAL_TX_CDC = 0,
	LINKR_DEBUGGER_SERIAL_TX_WS0,
	LINKR_DEBUGGER_SERIAL_TX_WS1,
	LINKR_DEBUGGER_SERIAL_TX_WS2,
	LINKR_DEBUGGER_SERIAL_TX_WS3,
	LINKR_DEBUGGER_SERIAL_TX_TCP0,
	LINKR_DEBUGGER_SERIAL_TX_TCP1,
	LINKR_DEBUGGER_SERIAL_TX_SOURCE_COUNT,
};

enum linkr_debugger_serial_parity {
	LINKR_DEBUGGER_SERIAL_PARITY_NONE = 0,
	LINKR_DEBUGGER_SERIAL_PARITY_ODD,
	LINKR_DEBUGGER_SERIAL_PARITY_EVEN,
};

enum linkr_debugger_serial_flow_control {
	LINKR_DEBUGGER_SERIAL_FLOW_NONE = 0,
	LINKR_DEBUGGER_SERIAL_FLOW_RTS_CTS,
};

enum linkr_debugger_serial_state {
	LINKR_DEBUGGER_SERIAL_READY = 0,
	LINKR_DEBUGGER_SERIAL_RECONFIG,
	LINKR_DEBUGGER_SERIAL_PAUSED,
	LINKR_DEBUGGER_SERIAL_FAULTED,
};

enum linkr_debugger_serial_config_decision {
	LINKR_DEBUGGER_SERIAL_CONFIG_NO_CHANGE = 0,
	LINKR_DEBUGGER_SERIAL_CONFIG_APPLY,
};

enum linkr_debugger_serial_config_origin {
	LINKR_DEBUGGER_SERIAL_CONFIG_ORIGIN_CDC = 0,
	LINKR_DEBUGGER_SERIAL_CONFIG_ORIGIN_WEB,
};

struct linkr_debugger_serial_config {
	uint32_t baud;
	uint8_t data_bits;
	enum linkr_debugger_serial_parity parity;
	uint8_t stop_bits;
	enum linkr_debugger_serial_flow_control flow_control;
};

struct linkr_debugger_serial_rx_sink_state {
	bool registered;
	bool attached;
	uint64_t cursor;
	uint64_t dropped;
	uint64_t delivered;
	uint64_t epoch;
};

struct linkr_debugger_serial_rx_state {
	uint8_t data[LINKR_DEBUGGER_SERIAL_RX_CAPACITY];
	uint64_t head;
	uint64_t paused_dropped;
	struct linkr_debugger_serial_rx_sink_state
		sinks[LINKR_DEBUGGER_SERIAL_SINK_COUNT];
};

struct linkr_debugger_serial_tx_slot {
	uint8_t data[LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX];
	uint16_t length;
	uint16_t offset;
	uint64_t event_id;
	enum linkr_debugger_serial_tx_source source;
	bool used;
};

struct linkr_debugger_serial_tx_source_counters {
	uint64_t accepted;
	uint64_t completed;
	uint64_t canceled;
	uint64_t rejected;
};

struct linkr_debugger_serial_tx_state {
	struct linkr_debugger_serial_tx_slot
		slots[LINKR_DEBUGGER_SERIAL_TX_SLOT_COUNT];
	struct linkr_debugger_serial_tx_source_counters
		sources[LINKR_DEBUGGER_SERIAL_TX_SOURCE_COUNT];
	int8_t active_slot;
	uint8_t used_slots;
	uint64_t last_event_id;
	uint64_t last_completed_event_id;
	uint64_t last_canceled_event_id;
	uint64_t accepted;
	uint64_t completed;
	uint64_t canceled;
	uint64_t rejected;
	uint64_t bytes_committed;
};

struct linkr_debugger_serial_tx_event {
	uint64_t event_id;
	enum linkr_debugger_serial_tx_source source;
};

struct linkr_debugger_serial_tx_cancel_batch {
	struct linkr_debugger_serial_tx_event
		events[LINKR_DEBUGGER_SERIAL_TX_SLOT_COUNT];
	size_t count;
};

struct linkr_debugger_serial_config_counters {
	uint64_t cdc_requests;
	uint64_t web_requests;
	uint64_t configure_count;
	uint64_t no_change;
	uint64_t invalid;
	uint64_t conflicts;
	uint64_t apply_succeeded;
	uint64_t apply_failed;
	uint64_t rollback_succeeded;
	uint64_t rollback_failed;
	uint64_t control_line_changes;
};

struct linkr_debugger_serial_model {
	struct linkr_debugger_serial_rx_state rx;
	struct linkr_debugger_serial_tx_state tx;
	struct linkr_debugger_serial_config current;
	struct linkr_debugger_serial_config last_good;
	struct linkr_debugger_serial_config pending;
	struct linkr_debugger_serial_config_counters config_counters;
	uint64_t generation;
	enum linkr_debugger_serial_state state;
	enum linkr_debugger_serial_config_origin pending_origin;
	bool dtr;
	bool pending_valid;
	bool rollback_pending;
};

extern const uint32_t
	linkr_debugger_serial_baud_whitelist[LINKR_DEBUGGER_SERIAL_BAUD_COUNT];

bool linkr_debugger_serial_config_valid(
	const struct linkr_debugger_serial_config *config);
bool linkr_debugger_serial_config_equal(
	const struct linkr_debugger_serial_config *left,
	const struct linkr_debugger_serial_config *right);

int linkr_debugger_serial_model_init(
	struct linkr_debugger_serial_model *model,
	const struct linkr_debugger_serial_config *initial_config);

int linkr_debugger_serial_rx_sink_register(
	struct linkr_debugger_serial_model *model,
	enum linkr_debugger_serial_sink sink);
int linkr_debugger_serial_rx_sink_attach(
	struct linkr_debugger_serial_model *model,
	enum linkr_debugger_serial_sink sink);
int linkr_debugger_serial_rx_sink_detach(
	struct linkr_debugger_serial_model *model,
	enum linkr_debugger_serial_sink sink);
/* While PAUSED, writes are discarded into rx.paused_dropped; head does not move. */
int linkr_debugger_serial_rx_write(struct linkr_debugger_serial_model *model,
				   const uint8_t *data, size_t length);
int linkr_debugger_serial_rx_read(struct linkr_debugger_serial_model *model,
				  enum linkr_debugger_serial_sink sink,
				  uint8_t *data, size_t capacity,
				  size_t *length);

int linkr_debugger_serial_tx_enqueue(
	struct linkr_debugger_serial_model *model,
	enum linkr_debugger_serial_tx_source source, const uint8_t *data,
	size_t length, uint64_t *event_id);
/*
 * peek() exposes uncommitted bytes without consuming them. commit() advances
 * only bytes the UART driver actually accepted. Both require READY. The peek
 * pointer is model-owned and valid only while the caller retains its lock and
 * makes no intervening model mutation.
 */
int linkr_debugger_serial_tx_peek(
	struct linkr_debugger_serial_model *model, const uint8_t **data,
	size_t *length,
	enum linkr_debugger_serial_tx_source *source, uint64_t *event_id);
int linkr_debugger_serial_tx_commit(
	struct linkr_debugger_serial_model *model, uint64_t event_id,
	size_t accepted);
/*
 * Committing the complete payload is not physical completion. finish_active()
 * succeeds only when the adapter reports the UART TX-complete condition.
 * A fully committed active frame may finish while PAUSED; no new bytes commit.
 * Cancellation likewise requires proof that the driver stopped or TX finished.
 * FAULTED blocks peek/commit; cancel_all() then reports every accepted event.
 */
int linkr_debugger_serial_tx_finish_active(
	struct linkr_debugger_serial_model *model, uint64_t event_id,
	bool wire_complete);
int linkr_debugger_serial_tx_cancel_active(
	struct linkr_debugger_serial_model *model, uint64_t event_id,
	bool driver_stopped);
int linkr_debugger_serial_tx_cancel_all(
	struct linkr_debugger_serial_model *model, bool driver_stopped,
	struct linkr_debugger_serial_tx_cancel_batch *canceled);

/*
 * pause/resume each advance every attached RX sink epoch and place its cursor
 * at the current head. pause counts unread pre-pause bytes as sink drops.
 */
int linkr_debugger_serial_pause(
	struct linkr_debugger_serial_model *model,
	struct linkr_debugger_serial_tx_cancel_batch *canceled);
int linkr_debugger_serial_resume(struct linkr_debugger_serial_model *model);
/* Does not complete or cancel accepted TX; the stopped driver must be reported. */
int linkr_debugger_serial_mark_faulted(
	struct linkr_debugger_serial_model *model);

/* DTR only controls CDC RX attachment. It never requests UART reconfiguration. */
int linkr_debugger_serial_set_dtr(struct linkr_debugger_serial_model *model,
				  bool asserted);

/*
 * A changed request cannot return APPLY while a TX frame is active; that frame
 * must first be committed and physically completed. APPLY leaves the model in
 * RECONFIG, where tx_peek() cannot start queued frames. The caller applies the
 * pending tuple to the driver and reports the result with apply_result(). A
 * failed apply must be followed by rollback_result().
 */
int linkr_debugger_serial_config_request_cdc(
	struct linkr_debugger_serial_model *model,
	const struct linkr_debugger_serial_config *config,
	enum linkr_debugger_serial_config_decision *decision);
int linkr_debugger_serial_config_request_web(
	struct linkr_debugger_serial_model *model,
	const struct linkr_debugger_serial_config *config,
	uint64_t expected_generation,
	enum linkr_debugger_serial_config_decision *decision);
int linkr_debugger_serial_config_apply_result(
	struct linkr_debugger_serial_model *model, bool succeeded);
int linkr_debugger_serial_config_rollback_result(
	struct linkr_debugger_serial_model *model, bool succeeded);

#endif /* RADXA_LINKR_DEBUGGER_SERIAL_MODEL_H_ */
