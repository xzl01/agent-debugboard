/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_SERIAL_H_
#define RADXA_LINKR_DEBUGGER_SERIAL_H_

#include "linkr_debugger_serial_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct device;

#define LINKR_DEBUGGER_SERIAL_RUNTIME_STACK_SIZE 2048U
#define LINKR_DEBUGGER_SERIAL_SINK_PENDING_MAX                                 \
	LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX

typedef void (*linkr_debugger_serial_irq_callback_t)(const struct device *dev,
						     void *user_data);

enum linkr_debugger_serial_line_control {
	LINKR_DEBUGGER_SERIAL_LINE_DTR = 0,
	LINKR_DEBUGGER_SERIAL_LINE_RTS,
};

enum linkr_debugger_serial_usb_event {
	LINKR_DEBUGGER_SERIAL_USB_RESET = 0,
	LINKR_DEBUGGER_SERIAL_USB_CONFIGURATION,
};

enum linkr_debugger_serial_cdc_phase {
	LINKR_DEBUGGER_SERIAL_CDC_DOWN = 0,
	LINKR_DEBUGGER_SERIAL_CDC_PROMOTING,
	LINKR_DEBUGGER_SERIAL_CDC_UP,
	LINKR_DEBUGGER_SERIAL_CDC_BLOCKED,
};

enum linkr_debugger_serial_cdc_block_reason {
	LINKR_DEBUGGER_SERIAL_CDC_BLOCK_NONE = 0,
	LINKR_DEBUGGER_SERIAL_CDC_BLOCK_LOW_DETACH_PENDING,
	LINKR_DEBUGGER_SERIAL_CDC_BLOCK_DTR_SAMPLE_UNKNOWN_WHILE_RECORDED_LOW,
	LINKR_DEBUGGER_SERIAL_CDC_BLOCK_DTR_SAMPLE_UNKNOWN_WHILE_RECORDED_HIGH,
	LINKR_DEBUGGER_SERIAL_CDC_BLOCK_CONFIG_FAILED,
	LINKR_DEBUGGER_SERIAL_CDC_BLOCK_ATTACH_FAILED,
	LINKR_DEBUGGER_SERIAL_CDC_BLOCK_DETACH_FAILED,
	LINKR_DEBUGGER_SERIAL_CDC_BLOCK_FAULTED,
	LINKR_DEBUGGER_SERIAL_CDC_BLOCK_EPOCH_EXHAUSTED,
	LINKR_DEBUGGER_SERIAL_CDC_BLOCK_NOTIFICATION_EXHAUSTED,
};

/* cdc_phase is authoritative: published DTR is true only in CDC_UP, and a
 * pending DTR promotion exists only in CDC_PROMOTING. cdc_block_reason defines
 * recovery from CDC_BLOCKED: normally a real LOW/reset boundary, with safe
 * same-LOW recovery for an unknown LOW sample or a failed detach. Exhaustion
 * reasons are permanent.
 */

enum linkr_debugger_serial_completion_disposition {
	LINKR_DEBUGGER_SERIAL_COMPLETED = 0,
	LINKR_DEBUGGER_SERIAL_CANCELED,
};

/*
 * The HAL exists so the same production runtime can be driven by host fakes.
 * Production callers normally leave config.ops NULL and use the Zephyr UART
 * adapters in linkr_debugger_serial.c.
 */
struct linkr_debugger_serial_hal_ops {
	bool (*device_ready)(void *context, const struct device *dev);
	int (*irq_callback_set)(void *context, const struct device *dev,
				linkr_debugger_serial_irq_callback_t callback,
				void *user_data);
	void (*irq_update)(void *context, const struct device *dev);
	int (*irq_is_pending)(void *context, const struct device *dev);
	int (*irq_rx_ready)(void *context, const struct device *dev);
	int (*fifo_read)(void *context, const struct device *dev, uint8_t *data,
			 int capacity);
	int (*err_check)(void *context, const struct device *dev);
	int (*irq_tx_ready)(void *context, const struct device *dev);
	int (*fifo_fill)(void *context, const struct device *dev,
			 const uint8_t *data, int length);
	int (*irq_tx_complete)(void *context, const struct device *dev);
	void (*irq_rx_enable)(void *context, const struct device *dev);
	void (*irq_rx_disable)(void *context, const struct device *dev);
	void (*irq_tx_enable)(void *context, const struct device *dev);
	void (*irq_tx_disable)(void *context, const struct device *dev);
	int (*config_get)(void *context, const struct device *dev,
			  struct linkr_debugger_serial_config *config);
	int (*configure)(void *context, const struct device *dev,
			 const struct linkr_debugger_serial_config *config);
	int (*line_control_get)(void *context, const struct device *dev,
				enum linkr_debugger_serial_line_control control,
				uint32_t *value);
};

struct linkr_debugger_serial_runtime_config {
	const struct device *uart;
	const struct device *cdc;
	struct linkr_debugger_serial_config initial_config;
	const struct linkr_debugger_serial_hal_ops *ops;
	void *hal_context;
};

/* Caller-owned storage makes a destructive model read safe across partial I/O.
 */
struct linkr_debugger_serial_sink_pending {
	uint8_t data[LINKR_DEBUGGER_SERIAL_SINK_PENDING_MAX];
	uint64_t epoch;
	size_t length;
	size_t offset;
	enum linkr_debugger_serial_sink sink;
	bool valid;
};

struct linkr_debugger_serial_completion {
	uint64_t event_id;
	enum linkr_debugger_serial_tx_source source;
	enum linkr_debugger_serial_completion_disposition disposition;
};

struct linkr_debugger_serial_runtime_counters {
	uint64_t uart_rx_bytes;
	uint64_t uart_rx_read_failures;
	uint64_t uart_rx_error_events;
	uint64_t uart_rx_error_mask;
	uint64_t uart_rx_pause_dropped;
	uint64_t uart_tx_fill_failures;
	uint64_t cdc_tx_bytes;
	uint64_t cdc_tx_fill_failures;
	uint64_t cdc_tx_stale_dropped;
	uint64_t cdc_rx_bytes;
	uint64_t cdc_rx_dropped;
	uint64_t cdc_rx_read_failures;
	uint64_t cdc_rx_backpressure;
	uint64_t pause_wait_timeouts;
	uint64_t completion_overflow;
	uint64_t invalid_line_coding;
	uint64_t config_mailbox_busy;
};

struct linkr_debugger_serial_status {
	struct linkr_debugger_serial_config current_config;
	struct linkr_debugger_serial_config pending_cdc_config;
	struct linkr_debugger_serial_config_counters config_counters;
	struct linkr_debugger_serial_runtime_counters runtime_counters;
	uint64_t generation;
	uint64_t cdc_responder_generation;
	uint64_t cdc_notification_sequence;
	uint64_t rx_head;
	uint64_t tx_bytes_committed;
	uint64_t tx_accepted;
	uint64_t tx_completed;
	uint64_t tx_canceled;
	uint64_t last_config_request_id;
	uint64_t active_event_id;
	uint32_t pause_wait_budget_ms;
	uint32_t cdc_notifications_inflight;
	int last_config_result;
	enum linkr_debugger_serial_state state;
	enum linkr_debugger_serial_cdc_phase cdc_phase;
	enum linkr_debugger_serial_cdc_block_reason cdc_block_reason;
	uint8_t tx_used_slots;
	bool last_config_done;
	bool dtr;
	bool rts;
	bool usb_configured;
	bool admission_frozen;
	bool pause_waiting_for_tx;
	bool cdc_rx_backpressured;
	bool pending_cdc_config_valid;
	bool cdc_promotion_pending;
	bool cdc_model_dtr;
	bool cdc_sink_attached;
};

int linkr_debugger_serial_init(
    const struct linkr_debugger_serial_runtime_config *config);

int linkr_debugger_serial_runtime_sink_register(
    enum linkr_debugger_serial_sink sink);
int linkr_debugger_serial_runtime_sink_unregister(
    enum linkr_debugger_serial_sink sink);
int linkr_debugger_serial_runtime_sink_attach(
    enum linkr_debugger_serial_sink sink);
int linkr_debugger_serial_runtime_sink_detach(
	enum linkr_debugger_serial_sink sink);
int linkr_debugger_serial_runtime_sink_rearm(
	enum linkr_debugger_serial_sink sink);
int linkr_debugger_serial_runtime_sink_read(
    enum linkr_debugger_serial_sink sink,
    struct linkr_debugger_serial_sink_pending *pending, size_t capacity,
    const uint8_t **data, size_t *length);
int linkr_debugger_serial_runtime_sink_consume(
	struct linkr_debugger_serial_sink_pending *pending, size_t accepted);
int linkr_debugger_serial_runtime_sink_pending_validate(
	enum linkr_debugger_serial_sink sink,
	struct linkr_debugger_serial_sink_pending *pending);

int linkr_debugger_serial_runtime_tx_enqueue(
    enum linkr_debugger_serial_tx_source source, const uint8_t *data,
    size_t length, uint64_t *event_id);
int linkr_debugger_serial_completion_poll(
    struct linkr_debugger_serial_completion *completion);

/*
 * Asynchronous CAS: accepting the mailbox freezes new TX admission until the
 * current physical frame completes and apply/rollback resolves. Completion is
 * reported in status.last_config_* fields.
 */
int linkr_debugger_serial_config_cas(
    const struct linkr_debugger_serial_config *config,
    uint64_t expected_generation, uint64_t *request_id);
int linkr_debugger_serial_status_snapshot(
    struct linkr_debugger_serial_status *status);

/*
 * These USB-notification entry points commit session boundaries under the
 * runtime lock, stage physical work, and wake the worker. Detach/reset edges
 * remain latched across event coalescing and are processed before promotion.
 */
int linkr_debugger_serial_cdc_line_coding(const struct device *dev);
int linkr_debugger_serial_cdc_control_line(const struct device *dev);
int linkr_debugger_serial_cdc_usb_event(
    const struct device *dev, enum linkr_debugger_serial_usb_event event,
    bool configured);

/*
 * Only a zero return establishes PAUSED and permits a mux change. An active
 * frame is given its format-derived physical wire time. -ETIMEDOUT/-EBUSY
 * leaves READY restored and retains all accepted TX in order.
 */
int linkr_debugger_serial_runtime_pause(void);
int linkr_debugger_serial_runtime_resume(void);

#ifdef LINKR_DEBUGGER_SERIAL_HOST_TEST
/* Deterministic host-only replacement for the stubbed control thread. */
void linkr_debugger_serial_process_events(void);
void linkr_debugger_serial_test_fail_next_set_dtr(bool asserted);
void linkr_debugger_serial_test_saturate_config_failure_counters(void);
void linkr_debugger_serial_test_set_session_epoch(uint64_t epoch);
void linkr_debugger_serial_test_set_notification_sequence(uint64_t sequence);
void linkr_debugger_serial_test_set_before_cdc_claim_hook(
	void (*hook)(void *context), void *context);
#endif

#endif /* RADXA_LINKR_DEBUGGER_SERIAL_H_ */
