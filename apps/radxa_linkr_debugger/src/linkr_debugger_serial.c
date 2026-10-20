/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_serial.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#define SERIAL_CONTROL_EVENT_USB BIT(0)
#define SERIAL_CONTROL_EVENT_CONFIG BIT(1)
#define SERIAL_CONTROL_EVENT_TX_CHANGE BIT(2)
#define SERIAL_CONTROL_EVENT_ALL UINT32_MAX

#define SERIAL_IRQ_MAX_PASSES 16U
#define SERIAL_UART_RX_CHUNK 64U
#define SERIAL_UART_RX_MAX_CHUNKS 8U
#define SERIAL_DRAIN_MAX_CHUNKS 8U
#define SERIAL_COMPLETION_DEPTH 16U
#define SERIAL_PAUSE_MIN_BAUD 9600U
#define SERIAL_PAUSE_MAX_BITS_PER_CHAR (1U + 8U + 1U + 2U)
#define SERIAL_PAUSE_WIRE_GUARD_MS 50U
#define SERIAL_PAUSE_MAX_WAIT_MS                                               \
	(((LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX *                              \
	   SERIAL_PAUSE_MAX_BITS_PER_CHAR * 1000U) +                           \
	  SERIAL_PAUSE_MIN_BAUD - 1U) /                                        \
	     SERIAL_PAUSE_MIN_BAUD +                                           \
	 SERIAL_PAUSE_WIRE_GUARD_MS)
#define SERIAL_WATCHDOG_TIMEOUT_MS 5000U
#define SERIAL_RUNTIME_ADDITIONAL_MAX (5U * 1024U)

struct serial_config_request {
	struct linkr_debugger_serial_config config;
	uint64_t expected_generation;
	uint64_t request_id;
	bool valid;
};

struct serial_runtime {
	struct linkr_debugger_serial_model model;
	const struct device *uart;
	const struct device *cdc;
	const struct linkr_debugger_serial_hal_ops *ops;
	void *hal_context;
	struct k_spinlock lock;
	struct k_mutex control_mutex;
	struct k_event control_event;
	struct k_sem wire_complete_sem;
	struct k_timer tx_probe_timer;
	struct k_thread control_thread;
	struct linkr_debugger_serial_runtime_counters counters;
	struct linkr_debugger_serial_completion
	    completions[SERIAL_COMPLETION_DEPTH];
	struct serial_config_request web_request;
	struct linkr_debugger_serial_config cdc_config;
	uint8_t cdc_pending[LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX];
	uint64_t cdc_pending_epoch;
	uint64_t wire_event_id;
	uint64_t next_config_request_id;
	uint64_t last_config_request_id;
	size_t cdc_pending_length;
	size_t cdc_pending_offset;
	uint8_t completion_head;
	uint8_t completion_count;
	int last_config_result;
	bool sink_registered[LINKR_DEBUGGER_SERIAL_SINK_COUNT];
	bool cdc_config_valid;
	bool wire_event_valid;
	bool wire_fully_committed;
	bool last_config_done;
	bool admission_frozen;
	bool pause_freeze;
	bool pause_waiting_for_tx;
	bool tx_probe_suppressed;
	bool cdc_rx_backpressured;
	bool cdc_gate_dtr;
	bool cdc_detach_pending;
	bool staged_control_valid;
	bool staged_dtr;
	bool staged_rts;
	bool staged_usb_valid;
	bool staged_usb_configured;
	bool rts;
	bool usb_configured;
};

static struct serial_runtime serial;
static atomic_t serial_init_state = ATOMIC_INIT(0);
K_THREAD_STACK_DEFINE(serial_control_stack,
		      LINKR_DEBUGGER_SERIAL_RUNTIME_STACK_SIZE);

BUILD_ASSERT(sizeof(struct serial_runtime) -
		     sizeof(struct linkr_debugger_serial_model) +
		     K_THREAD_STACK_SIZEOF(serial_control_stack) +
		     sizeof(serial_init_state) <=
		 SERIAL_RUNTIME_ADDITIONAL_MAX,
	     "serial runtime exceeds its additional static memory budget");
BUILD_ASSERT(sizeof(struct serial_runtime) +
		     K_THREAD_STACK_SIZEOF(serial_control_stack) +
		     sizeof(serial_init_state) <=
		 LINKR_DEBUGGER_SERIAL_STATIC_MAX,
	     "serial model, runtime, and stack exceed 14 KiB");
BUILD_ASSERT(SERIAL_PAUSE_MAX_WAIT_MS <= 650U,
	     "serial pause wait exceeds its reviewed bound");
BUILD_ASSERT(SERIAL_PAUSE_MAX_WAIT_MS < SERIAL_WATCHDOG_TIMEOUT_MS,
	     "serial pause wait must remain below the watchdog timeout");

static void serial_uart_callback(const struct device *dev, void *user_data);
static void serial_cdc_callback(const struct device *dev, void *user_data);
static bool serial_ready(void);

static void serial_tx_probe_timer(struct k_timer *timer)
{
	k_spinlock_key_t key;
	bool probe;

	ARG_UNUSED(timer);
	key = k_spin_lock(&serial.lock);
	probe = serial_ready() && !serial.tx_probe_suppressed &&
		serial.model.state == LINKR_DEBUGGER_SERIAL_READY &&
		serial.model.tx.used_slots != 0U &&
		(!serial.admission_frozen || serial.model.tx.active_slot >= 0);
	k_spin_unlock(&serial.lock, key);
	if (!probe) {
		return;
	}
	/* PL011 may synchronously enter the callback; no runtime lock is held.
	 */
	serial.ops->irq_tx_enable(serial.hal_context, serial.uart);
}

static void counter_add(uint64_t *counter, uint64_t amount)
{
	if (UINT64_MAX - *counter < amount) {
		*counter = UINT64_MAX;
	} else {
		*counter += amount;
	}
}

static void serial_refresh_admission_locked(void)
{
	serial.admission_frozen =
	    serial.pause_freeze || serial.cdc_config_valid ||
	    serial.web_request.valid ||
	    serial.model.state == LINKR_DEBUGGER_SERIAL_RECONFIG ||
	    serial.model.state == LINKR_DEBUGGER_SERIAL_FAULTED;
}

static uint32_t
serial_pause_wait_ms(const struct linkr_debugger_serial_config *config)
{
	uint32_t bits_per_char = 1U + config->data_bits + config->stop_bits;
	uint64_t wire_ms;

	if (config->parity != LINKR_DEBUGGER_SERIAL_PARITY_NONE) {
		bits_per_char++;
	}
	if (config->baud < SERIAL_PAUSE_MIN_BAUD) {
		return SERIAL_PAUSE_MAX_WAIT_MS;
	}
	wire_ms =
	    (LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX * bits_per_char * 1000ULL +
	     config->baud - 1U) /
	    config->baud;
	return (uint32_t)wire_ms + SERIAL_PAUSE_WIRE_GUARD_MS;
}

static bool serial_ready(void) { return atomic_get(&serial_init_state) == 2; }

static bool default_device_ready(void *context, const struct device *dev)
{
	ARG_UNUSED(context);
	return device_is_ready(dev);
}

static int
default_irq_callback_set(void *context, const struct device *dev,
			 linkr_debugger_serial_irq_callback_t callback,
			 void *user_data)
{
	ARG_UNUSED(context);
	return uart_irq_callback_user_data_set(
	    dev, (uart_irq_callback_user_data_t)callback, user_data);
}

static void default_irq_update(void *context, const struct device *dev)
{
	ARG_UNUSED(context);
	uart_irq_update(dev);
}

static int default_irq_is_pending(void *context, const struct device *dev)
{
	ARG_UNUSED(context);
	return uart_irq_is_pending(dev);
}

static int default_irq_rx_ready(void *context, const struct device *dev)
{
	ARG_UNUSED(context);
	return uart_irq_rx_ready(dev);
}

static int default_fifo_read(void *context, const struct device *dev,
			     uint8_t *data, int capacity)
{
	ARG_UNUSED(context);
	return uart_fifo_read(dev, data, capacity);
}

static int default_err_check(void *context, const struct device *dev)
{
	ARG_UNUSED(context);
	return uart_err_check(dev);
}

static int default_irq_tx_ready(void *context, const struct device *dev)
{
	ARG_UNUSED(context);
	return uart_irq_tx_ready(dev);
}

static int default_fifo_fill(void *context, const struct device *dev,
			     const uint8_t *data, int length)
{
	ARG_UNUSED(context);
	return uart_fifo_fill(dev, data, length);
}

static int default_irq_tx_complete(void *context, const struct device *dev)
{
	ARG_UNUSED(context);
	return uart_irq_tx_complete(dev);
}

static void default_irq_rx_enable(void *context, const struct device *dev)
{
	ARG_UNUSED(context);
	uart_irq_rx_enable(dev);
}

static void default_irq_rx_disable(void *context, const struct device *dev)
{
	ARG_UNUSED(context);
	uart_irq_rx_disable(dev);
}

static void default_irq_tx_enable(void *context, const struct device *dev)
{
	ARG_UNUSED(context);
	uart_irq_tx_enable(dev);
}

static void default_irq_tx_disable(void *context, const struct device *dev)
{
	ARG_UNUSED(context);
	uart_irq_tx_disable(dev);
}

static int uart_config_from_zephyr(const struct uart_config *input,
				   struct linkr_debugger_serial_config *output)
{
	output->baud = input->baudrate;
	switch (input->data_bits) {
	case UART_CFG_DATA_BITS_5:
		output->data_bits = 5U;
		break;
	case UART_CFG_DATA_BITS_6:
		output->data_bits = 6U;
		break;
	case UART_CFG_DATA_BITS_7:
		output->data_bits = 7U;
		break;
	case UART_CFG_DATA_BITS_8:
		output->data_bits = 8U;
		break;
	default:
		return -EINVAL;
	}

	switch (input->parity) {
	case UART_CFG_PARITY_NONE:
		output->parity = LINKR_DEBUGGER_SERIAL_PARITY_NONE;
		break;
	case UART_CFG_PARITY_ODD:
		output->parity = LINKR_DEBUGGER_SERIAL_PARITY_ODD;
		break;
	case UART_CFG_PARITY_EVEN:
		output->parity = LINKR_DEBUGGER_SERIAL_PARITY_EVEN;
		break;
	default:
		return -EINVAL;
	}

	switch (input->stop_bits) {
	case UART_CFG_STOP_BITS_1:
		output->stop_bits = 1U;
		break;
	case UART_CFG_STOP_BITS_2:
		output->stop_bits = 2U;
		break;
	default:
		return -EINVAL;
	}

	if (input->flow_ctrl != UART_CFG_FLOW_CTRL_NONE) {
		return -EINVAL;
	}
	output->flow_control = LINKR_DEBUGGER_SERIAL_FLOW_NONE;
	return 0;
}

static int
uart_config_to_zephyr(const struct linkr_debugger_serial_config *input,
		      struct uart_config *output)
{
	if (!linkr_debugger_serial_config_valid(input)) {
		return -EINVAL;
	}

	output->baudrate = input->baud;
	switch (input->data_bits) {
	case 5U:
		output->data_bits = UART_CFG_DATA_BITS_5;
		break;
	case 6U:
		output->data_bits = UART_CFG_DATA_BITS_6;
		break;
	case 7U:
		output->data_bits = UART_CFG_DATA_BITS_7;
		break;
	case 8U:
		output->data_bits = UART_CFG_DATA_BITS_8;
		break;
	default:
		return -EINVAL;
	}

	switch (input->parity) {
	case LINKR_DEBUGGER_SERIAL_PARITY_NONE:
		output->parity = UART_CFG_PARITY_NONE;
		break;
	case LINKR_DEBUGGER_SERIAL_PARITY_ODD:
		output->parity = UART_CFG_PARITY_ODD;
		break;
	case LINKR_DEBUGGER_SERIAL_PARITY_EVEN:
		output->parity = UART_CFG_PARITY_EVEN;
		break;
	default:
		return -EINVAL;
	}

	output->stop_bits = input->stop_bits == 1U ? UART_CFG_STOP_BITS_1
						   : UART_CFG_STOP_BITS_2;
	output->flow_ctrl = UART_CFG_FLOW_CTRL_NONE;
	return 0;
}

static int default_config_get(void *context, const struct device *dev,
			      struct linkr_debugger_serial_config *config)
{
	struct uart_config uart_config;
	int ret;

	ARG_UNUSED(context);
	ret = uart_config_get(dev, &uart_config);
	if (ret < 0) {
		return ret;
	}
	return uart_config_from_zephyr(&uart_config, config);
}

static int default_configure(void *context, const struct device *dev,
			     const struct linkr_debugger_serial_config *config)
{
	struct uart_config uart_config;
	int ret;

	ARG_UNUSED(context);
	ret = uart_config_to_zephyr(config, &uart_config);
	if (ret < 0) {
		return ret;
	}
	return uart_configure(dev, &uart_config);
}

static int
default_line_control_get(void *context, const struct device *dev,
			 enum linkr_debugger_serial_line_control control,
			 uint32_t *value)
{
	uint32_t zephyr_control;

	ARG_UNUSED(context);
	zephyr_control = control == LINKR_DEBUGGER_SERIAL_LINE_DTR
			     ? UART_LINE_CTRL_DTR
			     : UART_LINE_CTRL_RTS;
	return uart_line_ctrl_get(dev, zephyr_control, value);
}

static const struct linkr_debugger_serial_hal_ops default_hal_ops = {
    .device_ready = default_device_ready,
    .irq_callback_set = default_irq_callback_set,
    .irq_update = default_irq_update,
    .irq_is_pending = default_irq_is_pending,
    .irq_rx_ready = default_irq_rx_ready,
    .fifo_read = default_fifo_read,
    .err_check = default_err_check,
    .irq_tx_ready = default_irq_tx_ready,
    .fifo_fill = default_fifo_fill,
    .irq_tx_complete = default_irq_tx_complete,
    .irq_rx_enable = default_irq_rx_enable,
    .irq_rx_disable = default_irq_rx_disable,
    .irq_tx_enable = default_irq_tx_enable,
    .irq_tx_disable = default_irq_tx_disable,
    .config_get = default_config_get,
    .configure = default_configure,
    .line_control_get = default_line_control_get,
};

static bool hal_valid(const struct linkr_debugger_serial_hal_ops *ops)
{
	return ops != NULL && ops->device_ready != NULL &&
	       ops->irq_callback_set != NULL && ops->irq_update != NULL &&
	       ops->irq_is_pending != NULL && ops->irq_rx_ready != NULL &&
	       ops->fifo_read != NULL && ops->err_check != NULL &&
	       ops->irq_tx_ready != NULL && ops->fifo_fill != NULL &&
	       ops->irq_tx_complete != NULL && ops->irq_rx_enable != NULL &&
	       ops->irq_rx_disable != NULL && ops->irq_tx_enable != NULL &&
	       ops->irq_tx_disable != NULL && ops->config_get != NULL &&
	       ops->configure != NULL && ops->line_control_get != NULL;
}

static void wake_control(uint32_t events)
{
	(void)k_event_post(&serial.control_event, events);
}

static void cdc_pending_clear_locked(bool stale)
{
	size_t remaining =
	    serial.cdc_pending_length - serial.cdc_pending_offset;

	if (stale) {
		counter_add(&serial.counters.cdc_tx_stale_dropped, remaining);
	}
	serial.cdc_pending_length = 0U;
	serial.cdc_pending_offset = 0U;
	serial.cdc_pending_epoch = 0U;
}

static void completion_push_locked(
    uint64_t event_id, enum linkr_debugger_serial_tx_source source,
    enum linkr_debugger_serial_completion_disposition disposition)
{
	struct linkr_debugger_serial_completion *completion;
	uint8_t index;

	if (serial.completion_count == SERIAL_COMPLETION_DEPTH) {
		serial.completion_head =
		    (uint8_t)((serial.completion_head + 1U) %
			      SERIAL_COMPLETION_DEPTH);
		serial.completion_count--;
		counter_add(&serial.counters.completion_overflow, 1U);
	}
	index = (uint8_t)((serial.completion_head + serial.completion_count) %
			  SERIAL_COMPLETION_DEPTH);
	completion = &serial.completions[index];
	completion->event_id = event_id;
	completion->source = source;
	completion->disposition = disposition;
	serial.completion_count++;
}

static void canceled_push_locked(
    const struct linkr_debugger_serial_tx_cancel_batch *canceled)
{
	size_t i;

	for (i = 0U; i < canceled->count; ++i) {
		completion_push_locked(canceled->events[i].event_id,
				       canceled->events[i].source,
				       LINKR_DEBUGGER_SERIAL_CANCELED);
	}
}

static bool cdc_can_accept_locked(void)
{
	uint8_t cdc_used = 0U;
	size_t i;

	if (serial.admission_frozen ||
	    serial.model.state != LINKR_DEBUGGER_SERIAL_READY ||
	    serial.model.tx.used_slots >= LINKR_DEBUGGER_SERIAL_TX_SLOT_COUNT) {
		return false;
	}
	for (i = 0U; i < LINKR_DEBUGGER_SERIAL_TX_SLOT_COUNT; ++i) {
		if (serial.model.tx.slots[i].used &&
		    serial.model.tx.slots[i].source ==
			LINKR_DEBUGGER_SERIAL_TX_CDC) {
			cdc_used++;
		}
	}
	return cdc_used < 2U;
}

static void uart_errors_record(int errors)
{
	k_spinlock_key_t key;

	if (errors == 0 || errors == -ENOSYS || errors == -ENOTSUP) {
		return;
	}
	key = k_spin_lock(&serial.lock);
	if (errors < 0) {
		counter_add(&serial.counters.uart_rx_read_failures, 1U);
	} else {
		counter_add(&serial.counters.uart_rx_error_events, 1U);
		serial.counters.uart_rx_error_mask |=
		    (uint64_t)(unsigned int)errors;
	}
	k_spin_unlock(&serial.lock, key);
}

static void uart_drain_stale(void)
{
	uint8_t buffer[SERIAL_UART_RX_CHUNK];
	size_t chunks;
	int received;

	uart_errors_record(
	    serial.ops->err_check(serial.hal_context, serial.uart));
	for (chunks = 0U; chunks < SERIAL_DRAIN_MAX_CHUNKS; ++chunks) {
		received =
		    serial.ops->fifo_read(serial.hal_context, serial.uart,
					  buffer, (int)sizeof(buffer));
		if (received <= 0) {
			if (received < 0) {
				uart_errors_record(received);
			}
			break;
		}
		{
			k_spinlock_key_t key = k_spin_lock(&serial.lock);

			counter_add(&serial.counters.uart_rx_pause_dropped,
				    (uint64_t)received);
			k_spin_unlock(&serial.lock, key);
		}
		if (received < (int)sizeof(buffer)) {
			break;
		}
	}
}

static void uart_finish_wire_if_complete(bool *finished)
{
	enum linkr_debugger_serial_tx_source source;
	k_spinlock_key_t key;
	uint64_t event_id;
	bool candidate;
	int complete;

	*finished = false;
	key = k_spin_lock(&serial.lock);
	candidate = serial.wire_event_valid && serial.wire_fully_committed;
	event_id = serial.wire_event_id;
	k_spin_unlock(&serial.lock, key);
	if (!candidate) {
		return;
	}

	complete = serial.ops->irq_tx_complete(serial.hal_context, serial.uart);
	if (complete <= 0) {
		return;
	}

	key = k_spin_lock(&serial.lock);
	if (!serial.wire_event_valid || !serial.wire_fully_committed ||
	    serial.wire_event_id != event_id ||
	    serial.model.tx.active_slot < 0) {
		k_spin_unlock(&serial.lock, key);
		return;
	}
	source =
	    serial.model.tx.slots[(size_t)serial.model.tx.active_slot].source;
	if (linkr_debugger_serial_tx_finish_active(&serial.model, event_id,
						   true) == 0) {
		completion_push_locked(event_id, source,
				       LINKR_DEBUGGER_SERIAL_COMPLETED);
		serial.wire_event_valid = false;
		serial.wire_fully_committed = false;
		serial.wire_event_id = 0U;
		*finished = true;
	}
	k_spin_unlock(&serial.lock, key);
}

static void serial_uart_callback(const struct device *dev, void *user_data)
{
	struct serial_runtime *runtime = user_data;
	uint8_t buffer[SERIAL_UART_RX_CHUNK];
	bool enable_cdc_tx = false;
	bool finished_any = false;
	bool disable_tx = false;
	bool arm_tx_probe = false;
	bool tx_progress = false;
	bool wire_wait = false;
	size_t pass;

	if (!serial_ready() || runtime != &serial || dev != serial.uart) {
		return;
	}

	uart_errors_record(serial.ops->err_check(serial.hal_context, dev));
	for (pass = 0U; pass < SERIAL_IRQ_MAX_PASSES; ++pass) {
		bool finished = false;
		int pending;

		serial.ops->irq_update(serial.hal_context, dev);
		uart_finish_wire_if_complete(&finished);
		finished_any = finished_any || finished;
		pending = serial.ops->irq_is_pending(serial.hal_context, dev);
		if (pending <= 0) {
			break;
		}

		if (serial.ops->irq_rx_ready(serial.hal_context, dev) > 0) {
			size_t chunk;

			for (chunk = 0U; chunk < SERIAL_UART_RX_MAX_CHUNKS;
			     ++chunk) {
				int received = serial.ops->fifo_read(
				    serial.hal_context, dev, buffer,
				    (int)sizeof(buffer));

				if (received <= 0) {
					if (received < 0) {
						uart_errors_record(received);
					}
					break;
				}
				{
					k_spinlock_key_t key =
					    k_spin_lock(&serial.lock);

					(void)linkr_debugger_serial_rx_write(
					    &serial.model, buffer,
					    (size_t)received);
					counter_add(
					    &serial.counters.uart_rx_bytes,
					    (uint64_t)received);
					enable_cdc_tx =
					    enable_cdc_tx ||
					    (serial.cdc_gate_dtr &&
					     serial.model.dtr &&
					     serial.model.state !=
						 LINKR_DEBUGGER_SERIAL_PAUSED &&
					     serial.model.state !=
						 LINKR_DEBUGGER_SERIAL_FAULTED);
					k_spin_unlock(&serial.lock, key);
				}
				if (received < (int)sizeof(buffer)) {
					break;
				}
			}
		}

		if (serial.ops->irq_tx_ready(serial.hal_context, dev) > 0) {
			const uint8_t *data;
			enum linkr_debugger_serial_tx_source source;
			uint64_t event_id;
			size_t length;
			int filled = 0;
			int ret;
			k_spinlock_key_t key = k_spin_lock(&serial.lock);

			ret = serial.admission_frozen &&
				      serial.model.tx.active_slot < 0
				  ? -EAGAIN
				  : linkr_debugger_serial_tx_peek(
					&serial.model, &data, &length, &source,
					&event_id);
			if (ret == 0) {
				if (length > (size_t)INT_MAX) {
					length = (size_t)INT_MAX;
				}
				/* The model pointer stays protected through
				 * fill+commit. */
				filled = serial.ops->fifo_fill(
				    serial.hal_context, dev, data, (int)length);
				if (filled > 0 && (size_t)filled <= length &&
				    linkr_debugger_serial_tx_commit(
					&serial.model, event_id,
					(size_t)filled) == 0) {
					serial.wire_event_valid = true;
					serial.wire_event_id = event_id;
					tx_progress = true;
					if ((size_t)filled == length) {
						serial.wire_fully_committed =
						    true;
					}
				} else if (filled < 0 ||
					   (size_t)filled > length) {
					counter_add(&serial.counters
							 .uart_tx_fill_failures,
						    1U);
				}
			}
			k_spin_unlock(&serial.lock, key);
			if (ret == 0 && filled == 0) {
				break;
			}
		}
	}

	if (enable_cdc_tx) {
		/* CDC may invoke its callback synchronously; no model lock is
		 * held. */
		serial.ops->irq_tx_enable(serial.hal_context, serial.cdc);
	}
	if (finished_any) {
		k_sem_give(&serial.wire_complete_sem);
		wake_control(SERIAL_CONTROL_EVENT_TX_CHANGE |
			     SERIAL_CONTROL_EVENT_CONFIG);
	}

	{
		k_spinlock_key_t key = k_spin_lock(&serial.lock);
		bool no_active = serial.model.tx.active_slot < 0;

		arm_tx_probe =
		    !serial.tx_probe_suppressed &&
		    serial.model.state == LINKR_DEBUGGER_SERIAL_READY &&
		    serial.model.tx.used_slots != 0U &&
		    (!serial.admission_frozen || !no_active);
		wire_wait =
		    serial.wire_event_valid && serial.wire_fully_committed;
		disable_tx =
		    arm_tx_probe ||
		    (!serial.wire_event_valid &&
		     ((serial.admission_frozen && no_active) ||
		      serial.model.state != LINKR_DEBUGGER_SERIAL_READY ||
		      serial.model.tx.used_slots == 0U));
		k_spin_unlock(&serial.lock, key);
	}
	if (disable_tx) {
		serial.ops->irq_tx_disable(serial.hal_context, dev);
	}
	if (arm_tx_probe) {
		k_timer_start(&serial.tx_probe_timer,
			      wire_wait || !tx_progress ? K_MSEC(1) : K_NO_WAIT,
			      K_NO_WAIT);
	}
}

static void serial_cdc_rx(void)
{
	uint8_t buffer[LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX];
	bool disable_rx = false;
	bool enable_uart_tx = false;
	int received;
	int ret;
	uint64_t event_id;
	k_spinlock_key_t key;

	if (serial.ops->irq_rx_ready(serial.hal_context, serial.cdc) <= 0) {
		return;
	}

	key = k_spin_lock(&serial.lock);
	if (serial.model.state == LINKR_DEBUGGER_SERIAL_PAUSED ||
	    serial.model.state == LINKR_DEBUGGER_SERIAL_FAULTED) {
		k_spin_unlock(&serial.lock, key);
		received = serial.ops->fifo_read(serial.hal_context, serial.cdc,
						 buffer, (int)sizeof(buffer));
		key = k_spin_lock(&serial.lock);
		if (received > 0) {
			counter_add(&serial.counters.cdc_rx_dropped,
				    (uint64_t)received);
		} else if (received < 0) {
			counter_add(&serial.counters.cdc_rx_read_failures, 1U);
		}
		k_spin_unlock(&serial.lock, key);
		return;
	}
	if (!cdc_can_accept_locked()) {
		if (!serial.cdc_rx_backpressured) {
			counter_add(&serial.counters.cdc_rx_backpressure, 1U);
		}
		serial.cdc_rx_backpressured = true;
		k_spin_unlock(&serial.lock, key);
		serial.ops->irq_rx_disable(serial.hal_context, serial.cdc);
		return;
	}

	/* Keep the quota check, destructive FIFO read, and enqueue indivisible.
	 */
	received = serial.ops->fifo_read(serial.hal_context, serial.cdc, buffer,
					 (int)sizeof(buffer));
	if (received > 0) {
		ret = linkr_debugger_serial_tx_enqueue(
		    &serial.model, LINKR_DEBUGGER_SERIAL_TX_CDC, buffer,
		    (size_t)received, &event_id);
		if (ret == 0) {
			counter_add(&serial.counters.cdc_rx_bytes,
				    (uint64_t)received);
			enable_uart_tx = true;
		} else {
			counter_add(&serial.counters.cdc_rx_dropped,
				    (uint64_t)received);
		}
		if (!cdc_can_accept_locked()) {
			serial.cdc_rx_backpressured = true;
			counter_add(&serial.counters.cdc_rx_backpressure, 1U);
			disable_rx = true;
		}
	} else if (received < 0) {
		counter_add(&serial.counters.cdc_rx_read_failures, 1U);
	}
	k_spin_unlock(&serial.lock, key);

	if (disable_rx) {
		serial.ops->irq_rx_disable(serial.hal_context, serial.cdc);
	}
	if (enable_uart_tx) {
		/* PL011 may call back before this function returns. */
		serial.ops->irq_tx_enable(serial.hal_context, serial.uart);
	}
}

static int serial_cdc_tx(size_t *budget)
{
	const struct linkr_debugger_serial_rx_sink_state *sink;
	size_t available;
	size_t read_length;
	int filled = 0;
	int ret;
	k_spinlock_key_t key;

	if (*budget == 0U ||
	    serial.ops->irq_tx_ready(serial.hal_context, serial.cdc) <= 0) {
		return 0;
	}

	key = k_spin_lock(&serial.lock);
	sink = &serial.model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_CDC];
	if (!serial.cdc_gate_dtr || !serial.model.dtr || !sink->attached ||
	    serial.model.state == LINKR_DEBUGGER_SERIAL_PAUSED ||
	    serial.model.state == LINKR_DEBUGGER_SERIAL_FAULTED) {
		cdc_pending_clear_locked(true);
		k_spin_unlock(&serial.lock, key);
		return 0;
	}

	if (serial.cdc_pending_length != 0U &&
	    serial.cdc_pending_epoch != sink->epoch) {
		cdc_pending_clear_locked(true);
	}
	if (serial.cdc_pending_length == 0U) {
		ret = linkr_debugger_serial_rx_read(
		    &serial.model, LINKR_DEBUGGER_SERIAL_SINK_CDC,
		    serial.cdc_pending,
		    MIN(*budget, sizeof(serial.cdc_pending)), &read_length);
		if (ret < 0 || read_length == 0U) {
			k_spin_unlock(&serial.lock, key);
			return 0;
		}
		serial.cdc_pending_length = read_length;
		serial.cdc_pending_offset = 0U;
		serial.cdc_pending_epoch = sink->epoch;
	}

	available = serial.cdc_pending_length - serial.cdc_pending_offset;
	if (available > *budget) {
		available = *budget;
	}
	filled = serial.ops->fifo_fill(
	    serial.hal_context, serial.cdc,
	    serial.cdc_pending + serial.cdc_pending_offset, (int)available);
	if (filled > 0 && (size_t)filled <= available) {
		serial.cdc_pending_offset += (size_t)filled;
		*budget -= (size_t)filled;
		counter_add(&serial.counters.cdc_tx_bytes, (uint64_t)filled);
		if (serial.cdc_pending_offset == serial.cdc_pending_length) {
			cdc_pending_clear_locked(false);
		}
	} else if (filled < 0 || (size_t)filled > available) {
		counter_add(&serial.counters.cdc_tx_fill_failures, 1U);
		k_spin_unlock(&serial.lock, key);
		return -EIO;
	}
	k_spin_unlock(&serial.lock, key);
	return filled > 0 ? 1 : 0;
}

static void serial_cdc_callback(const struct device *dev, void *user_data)
{
	struct serial_runtime *runtime = user_data;
	size_t tx_budget = LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX;
	size_t pass;
	bool tx_progress = false;
	bool disable_tx;
	bool tx_error = false;
	bool rx_handled = false;

	if (!serial_ready() || runtime != &serial || dev != serial.cdc) {
		return;
	}

	for (pass = 0U; pass < SERIAL_IRQ_MAX_PASSES; ++pass) {
		serial.ops->irq_update(serial.hal_context, dev);
		if (serial.ops->irq_is_pending(serial.hal_context, dev) <= 0) {
			break;
		}
		if (!rx_handled) {
			serial_cdc_rx();
			rx_handled = true;
		}
		int tx_ret = serial_cdc_tx(&tx_budget);

		if (tx_ret > 0) {
			tx_progress = true;
		} else if (tx_ret < 0) {
			tx_error = true;
			break;
		} else if (tx_budget == 0U) {
			break;
		}
	}

	{
		k_spinlock_key_t key = k_spin_lock(&serial.lock);
		const struct linkr_debugger_serial_rx_sink_state *sink =
		    &serial.model.rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_CDC];

		disable_tx =
		    tx_error || !serial.cdc_gate_dtr || !serial.model.dtr ||
		    !sink->attached ||
		    serial.model.state == LINKR_DEBUGGER_SERIAL_PAUSED ||
		    serial.model.state == LINKR_DEBUGGER_SERIAL_FAULTED ||
		    (!tx_progress && serial.cdc_pending_length == 0U);
		k_spin_unlock(&serial.lock, key);
	}
	if (disable_tx) {
		serial.ops->irq_tx_disable(serial.hal_context, dev);
	}
}

static void worker_apply_config(bool cdc_request)
{
	struct serial_config_request request;
	struct linkr_debugger_serial_config pending;
	struct linkr_debugger_serial_config rollback;
	struct linkr_debugger_serial_tx_cancel_batch canceled;
	enum linkr_debugger_serial_config_decision decision =
	    LINKR_DEBUGGER_SERIAL_CONFIG_NO_CHANGE;
	k_spinlock_key_t key;
	bool apply = false;
	bool faulted = false;
	int apply_ret;
	int request_ret;
	int rollback_ret = 0;

	key = k_spin_lock(&serial.lock);
	if (cdc_request) {
		if (!serial.cdc_config_valid) {
			k_spin_unlock(&serial.lock, key);
			return;
		}
		memset(&request, 0, sizeof(request));
		request.config = serial.cdc_config;
	} else {
		if (!serial.web_request.valid) {
			k_spin_unlock(&serial.lock, key);
			return;
		}
		request = serial.web_request;
	}

	if (serial.pause_freeze ||
	    serial.model.state == LINKR_DEBUGGER_SERIAL_PAUSED ||
	    serial.model.tx.active_slot >= 0) {
		k_spin_unlock(&serial.lock, key);
		return;
	}
	if (cdc_request) {
		request_ret = linkr_debugger_serial_config_request_cdc(
		    &serial.model, &request.config, &decision);
	} else {
		request_ret = linkr_debugger_serial_config_request_web(
		    &serial.model, &request.config, request.expected_generation,
		    &decision);
	}
	if (request_ret == -EBUSY &&
	    serial.model.state == LINKR_DEBUGGER_SERIAL_RECONFIG) {
		k_spin_unlock(&serial.lock, key);
		return;
	}
	if (cdc_request) {
		serial.cdc_config_valid = false;
	} else {
		serial.web_request.valid = false;
		serial.last_config_request_id = request.request_id;
		serial.last_config_result = request_ret;
		serial.last_config_done =
		    request_ret < 0 ||
		    decision == LINKR_DEBUGGER_SERIAL_CONFIG_NO_CHANGE;
	}
	if (request_ret == 0 &&
	    decision == LINKR_DEBUGGER_SERIAL_CONFIG_APPLY) {
		pending = serial.model.pending;
		rollback = serial.model.last_good;
		apply = true;
	}
	serial_refresh_admission_locked();
	k_spin_unlock(&serial.lock, key);
	if (!apply) {
		return;
	}

	/* Only this control-worker path is allowed to reconfigure PL011. */
	k_timer_stop(&serial.tx_probe_timer);
	serial.ops->irq_tx_disable(serial.hal_context, serial.uart);
	serial.ops->irq_rx_disable(serial.hal_context, serial.uart);
	apply_ret =
	    serial.ops->configure(serial.hal_context, serial.uart, &pending);
	key = k_spin_lock(&serial.lock);
	(void)linkr_debugger_serial_config_apply_result(&serial.model,
							apply_ret == 0);
	k_spin_unlock(&serial.lock, key);
	if (apply_ret != 0) {
		rollback_ret = serial.ops->configure(serial.hal_context,
						     serial.uart, &rollback);
		key = k_spin_lock(&serial.lock);
		(void)linkr_debugger_serial_config_rollback_result(
		    &serial.model, rollback_ret == 0);
		faulted = rollback_ret != 0;
		k_spin_unlock(&serial.lock, key);
	}

	if (faulted) {
		k_timer_stop(&serial.tx_probe_timer);
		serial.ops->irq_tx_disable(serial.hal_context, serial.uart);
		serial.ops->irq_rx_disable(serial.hal_context, serial.uart);
		key = k_spin_lock(&serial.lock);
		serial_refresh_admission_locked();
		(void)linkr_debugger_serial_tx_cancel_all(&serial.model, false,
							  &canceled);
		canceled_push_locked(&canceled);
		serial.wire_event_valid = false;
		serial.wire_fully_committed = false;
		serial.wire_event_id = 0U;
		k_spin_unlock(&serial.lock, key);
	}

	key = k_spin_lock(&serial.lock);
	serial_refresh_admission_locked();
	if (!cdc_request) {
		serial.last_config_result =
		    apply_ret != 0 ? apply_ret : rollback_ret;
		serial.last_config_done = true;
	}
	k_spin_unlock(&serial.lock, key);
	if (!faulted) {
		serial.ops->irq_rx_enable(serial.hal_context, serial.uart);
	}
}

static void serial_process_events(void)
{
	bool control_valid;
	bool detach_pending;
	bool dtr = false;
	bool enable_cdc_tx = false;
	bool disable_cdc_tx = false;
	bool resume_cdc_rx = false;
	bool probe_uart_tx = false;
	k_spinlock_key_t key;

	if (!serial_ready()) {
		return;
	}
	(void)k_mutex_lock(&serial.control_mutex, K_FOREVER);

	key = k_spin_lock(&serial.lock);
	detach_pending = serial.cdc_detach_pending;
	serial.cdc_detach_pending = false;
	control_valid = serial.staged_control_valid;
	if (detach_pending) {
		(void)linkr_debugger_serial_set_dtr(&serial.model, false);
		cdc_pending_clear_locked(true);
		serial.cdc_gate_dtr = false;
		disable_cdc_tx = true;
	}
	if (control_valid) {
		dtr = serial.staged_dtr;
		serial.rts = serial.staged_rts;
		serial.staged_control_valid = false;
		(void)linkr_debugger_serial_set_dtr(&serial.model, dtr);
		serial.cdc_gate_dtr = dtr;
		if (!dtr) {
			cdc_pending_clear_locked(true);
			disable_cdc_tx = true;
		} else {
			enable_cdc_tx = true;
		}
	}
	if (serial.staged_usb_valid) {
		serial.usb_configured = serial.staged_usb_configured;
		serial.staged_usb_valid = false;
	}
	k_spin_unlock(&serial.lock, key);

	if (disable_cdc_tx) {
		serial.ops->irq_tx_disable(serial.hal_context, serial.cdc);
	}
	if (enable_cdc_tx) {
		serial.ops->irq_tx_enable(serial.hal_context, serial.cdc);
	}

	worker_apply_config(true);
	worker_apply_config(false);

	key = k_spin_lock(&serial.lock);
	if (serial.cdc_rx_backpressured && cdc_can_accept_locked()) {
		serial.cdc_rx_backpressured = false;
		resume_cdc_rx = true;
	}
	probe_uart_tx =
	    serial.model.state == LINKR_DEBUGGER_SERIAL_READY &&
	    !serial.tx_probe_suppressed && serial.model.tx.used_slots != 0U &&
	    (!serial.admission_frozen || serial.model.tx.active_slot >= 0);
	k_spin_unlock(&serial.lock, key);
	if (resume_cdc_rx) {
		serial.ops->irq_rx_enable(serial.hal_context, serial.cdc);
	}
	if (probe_uart_tx) {
		serial.ops->irq_tx_enable(serial.hal_context, serial.uart);
	}

	(void)k_mutex_unlock(&serial.control_mutex);
}

static void serial_control_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		(void)k_event_wait_safe(&serial.control_event,
					SERIAL_CONTROL_EVENT_ALL, false,
					K_FOREVER);
		serial_process_events();
	}
}

#ifdef LINKR_DEBUGGER_SERIAL_HOST_TEST
void linkr_debugger_serial_process_events(void) { serial_process_events(); }
#endif

int linkr_debugger_serial_init(
    const struct linkr_debugger_serial_runtime_config *config)
{
	const struct linkr_debugger_serial_hal_ops *ops;
	int ret;

	if (config == NULL || config->uart == NULL || config->cdc == NULL ||
	    config->uart == config->cdc ||
	    !linkr_debugger_serial_config_valid(&config->initial_config)) {
		return -EINVAL;
	}
	ops = config->ops != NULL ? config->ops : &default_hal_ops;
	if (!hal_valid(ops)) {
		return -EINVAL;
	}
	if (!atomic_cas(&serial_init_state, 0, 1)) {
		return -EALREADY;
	}

	memset(&serial, 0, sizeof(serial));
	serial.uart = config->uart;
	serial.cdc = config->cdc;
	serial.ops = ops;
	serial.hal_context = config->hal_context;
	if (!ops->device_ready(serial.hal_context, serial.uart) ||
	    !ops->device_ready(serial.hal_context, serial.cdc)) {
		atomic_set(&serial_init_state, 0);
		return -ENODEV;
	}
	ret = linkr_debugger_serial_model_init(&serial.model,
					       &config->initial_config);
	if (ret < 0) {
		atomic_set(&serial_init_state, 0);
		return ret;
	}
	k_mutex_init(&serial.control_mutex);
	k_event_init(&serial.control_event);
	k_sem_init(&serial.wire_complete_sem, 0U, 1U);
	k_timer_init(&serial.tx_probe_timer, serial_tx_probe_timer, NULL);
	ret = linkr_debugger_serial_rx_sink_register(
	    &serial.model, LINKR_DEBUGGER_SERIAL_SINK_CDC);
	if (ret < 0) {
		atomic_set(&serial_init_state, 0);
		return ret;
	}
	serial.sink_registered[LINKR_DEBUGGER_SERIAL_SINK_CDC] = true;
	serial.last_config_done = true;

	ret = ops->irq_callback_set(serial.hal_context, serial.uart,
				    serial_uart_callback, &serial);
	if (ret < 0) {
		atomic_set(&serial_init_state, 0);
		return ret;
	}
	ret = ops->irq_callback_set(serial.hal_context, serial.cdc,
				    serial_cdc_callback, &serial);
	if (ret < 0) {
		(void)ops->irq_callback_set(serial.hal_context, serial.uart,
					    NULL, NULL);
		atomic_set(&serial_init_state, 0);
		return ret;
	}

	(void)k_thread_create(&serial.control_thread, serial_control_stack,
			      K_THREAD_STACK_SIZEOF(serial_control_stack),
			      serial_control_thread_entry, NULL, NULL, NULL,
			      K_PRIO_PREEMPT(9), 0U, K_NO_WAIT);
	atomic_set(&serial_init_state, 2);
	ops->irq_rx_enable(serial.hal_context, serial.uart);
	ops->irq_rx_enable(serial.hal_context, serial.cdc);
	return 0;
}

int linkr_debugger_serial_runtime_sink_register(
    enum linkr_debugger_serial_sink sink)
{
	k_spinlock_key_t key;
	int ret = 0;

	if (!serial_ready()) {
		return -ENODEV;
	}
	if (sink <= LINKR_DEBUGGER_SERIAL_SINK_CDC ||
	    sink >= LINKR_DEBUGGER_SERIAL_SINK_COUNT) {
		return -EINVAL;
	}
	key = k_spin_lock(&serial.lock);
	if (serial.sink_registered[sink]) {
		ret = -EALREADY;
	} else if (serial.model.rx.sinks[sink].registered) {
		serial.sink_registered[sink] = true;
	} else {
		ret =
		    linkr_debugger_serial_rx_sink_register(&serial.model, sink);
		if (ret == 0) {
			serial.sink_registered[sink] = true;
		}
	}
	k_spin_unlock(&serial.lock, key);
	return ret;
}

int linkr_debugger_serial_runtime_sink_unregister(
    enum linkr_debugger_serial_sink sink)
{
	k_spinlock_key_t key;
	int ret;

	if (!serial_ready()) {
		return -ENODEV;
	}
	if (sink <= LINKR_DEBUGGER_SERIAL_SINK_CDC ||
	    sink >= LINKR_DEBUGGER_SERIAL_SINK_COUNT) {
		return -EINVAL;
	}
	key = k_spin_lock(&serial.lock);
	if (!serial.sink_registered[sink]) {
		ret = -ENOENT;
	} else {
		ret = linkr_debugger_serial_rx_sink_detach(&serial.model, sink);
		if (ret == 0) {
			serial.sink_registered[sink] = false;
		}
	}
	k_spin_unlock(&serial.lock, key);
	return ret;
}

int linkr_debugger_serial_runtime_sink_attach(
    enum linkr_debugger_serial_sink sink)
{
	k_spinlock_key_t key;
	int ret;

	if (!serial_ready()) {
		return -ENODEV;
	}
	if (sink <= LINKR_DEBUGGER_SERIAL_SINK_CDC ||
	    sink >= LINKR_DEBUGGER_SERIAL_SINK_COUNT) {
		return -EINVAL;
	}
	key = k_spin_lock(&serial.lock);
	ret = serial.sink_registered[sink]
		  ? linkr_debugger_serial_rx_sink_attach(&serial.model, sink)
		  : -ENOENT;
	k_spin_unlock(&serial.lock, key);
	return ret;
}

int linkr_debugger_serial_runtime_sink_detach(
    enum linkr_debugger_serial_sink sink)
{
	k_spinlock_key_t key;
	int ret;

	if (!serial_ready()) {
		return -ENODEV;
	}
	if (sink <= LINKR_DEBUGGER_SERIAL_SINK_CDC ||
	    sink >= LINKR_DEBUGGER_SERIAL_SINK_COUNT) {
		return -EINVAL;
	}
	key = k_spin_lock(&serial.lock);
	ret = serial.sink_registered[sink]
		  ? linkr_debugger_serial_rx_sink_detach(&serial.model, sink)
		  : -ENOENT;
	k_spin_unlock(&serial.lock, key);
	return ret;
}

int linkr_debugger_serial_runtime_sink_read(
    enum linkr_debugger_serial_sink sink,
    struct linkr_debugger_serial_sink_pending *pending, size_t capacity,
    const uint8_t **data, size_t *length)
{
	struct linkr_debugger_serial_rx_sink_state *sink_state;
	size_t request;
	int ret = 0;
	k_spinlock_key_t key;

	if (!serial_ready()) {
		return -ENODEV;
	}
	if (sink <= LINKR_DEBUGGER_SERIAL_SINK_CDC ||
	    sink >= LINKR_DEBUGGER_SERIAL_SINK_COUNT || pending == NULL ||
	    data == NULL || length == NULL) {
		return -EINVAL;
	}
	*data = NULL;
	*length = 0U;
	key = k_spin_lock(&serial.lock);
	if (!serial.sink_registered[sink]) {
		ret = -ENOENT;
		goto serial_read_done;
	}
	sink_state = &serial.model.rx.sinks[sink];
	if (pending->valid &&
	    (pending->sink != sink || pending->epoch != sink_state->epoch)) {
		memset(pending, 0, sizeof(*pending));
	}
	if (!pending->valid || pending->offset == pending->length) {
		memset(pending, 0, sizeof(*pending));
		request = MIN(capacity, sizeof(pending->data));
		ret = linkr_debugger_serial_rx_read(&serial.model, sink,
						    pending->data, request,
						    &pending->length);
		if (ret < 0 || pending->length == 0U) {
			goto serial_read_done;
		}
		pending->sink = sink;
		pending->epoch = sink_state->epoch;
		pending->valid = true;
	}
	*length = MIN(capacity, pending->length - pending->offset);
	*data = pending->data + pending->offset;

serial_read_done:
	k_spin_unlock(&serial.lock, key);
	return ret;
}

int linkr_debugger_serial_runtime_sink_consume(
    struct linkr_debugger_serial_sink_pending *pending, size_t accepted)
{
	if (pending == NULL || !pending->valid ||
	    accepted > pending->length - pending->offset) {
		return -EINVAL;
	}
	pending->offset += accepted;
	if (pending->offset == pending->length) {
		memset(pending, 0, sizeof(*pending));
	}
	return 0;
}

int linkr_debugger_serial_runtime_tx_enqueue(
    enum linkr_debugger_serial_tx_source source, const uint8_t *data,
    size_t length, uint64_t *event_id)
{
	k_spinlock_key_t key;
	int ret;

	if (!serial_ready()) {
		return -ENODEV;
	}
	key = k_spin_lock(&serial.lock);
	ret = serial.admission_frozen
		  ? -EAGAIN
		  : linkr_debugger_serial_tx_enqueue(&serial.model, source,
						     data, length, event_id);
	k_spin_unlock(&serial.lock, key);
	if (ret == 0) {
		/* PL011 can synchronously enter serial_uart_callback(). */
		serial.ops->irq_tx_enable(serial.hal_context, serial.uart);
	}
	return ret;
}

int linkr_debugger_serial_completion_poll(
    struct linkr_debugger_serial_completion *completion)
{
	k_spinlock_key_t key;

	if (!serial_ready()) {
		return -ENODEV;
	}
	if (completion == NULL) {
		return -EINVAL;
	}
	key = k_spin_lock(&serial.lock);
	if (serial.completion_count == 0U) {
		k_spin_unlock(&serial.lock, key);
		return -EAGAIN;
	}
	*completion = serial.completions[serial.completion_head];
	serial.completion_head =
	    (uint8_t)((serial.completion_head + 1U) % SERIAL_COMPLETION_DEPTH);
	serial.completion_count--;
	k_spin_unlock(&serial.lock, key);
	return 0;
}

int linkr_debugger_serial_config_cas(
    const struct linkr_debugger_serial_config *config,
    uint64_t expected_generation, uint64_t *request_id)
{
	k_spinlock_key_t key;
	int ret = 0;

	if (!serial_ready()) {
		return -ENODEV;
	}
	if (!linkr_debugger_serial_config_valid(config) || request_id == NULL) {
		return -EINVAL;
	}
	key = k_spin_lock(&serial.lock);
	if (serial.web_request.valid) {
		counter_add(&serial.counters.config_mailbox_busy, 1U);
		ret = -EBUSY;
	} else if (serial.admission_frozen) {
		ret = -EAGAIN;
	} else if (serial.next_config_request_id == UINT64_MAX) {
		ret = -EOVERFLOW;
	} else {
		serial.web_request.config = *config;
		serial.web_request.expected_generation = expected_generation;
		serial.web_request.request_id = ++serial.next_config_request_id;
		serial.web_request.valid = true;
		serial.last_config_done = false;
		serial_refresh_admission_locked();
		*request_id = serial.web_request.request_id;
	}
	k_spin_unlock(&serial.lock, key);
	if (ret == 0) {
		wake_control(SERIAL_CONTROL_EVENT_CONFIG);
	}
	return ret;
}

int linkr_debugger_serial_status_snapshot(
    struct linkr_debugger_serial_status *status)
{
	k_spinlock_key_t key;

	if (!serial_ready()) {
		return -ENODEV;
	}
	if (status == NULL) {
		return -EINVAL;
	}
	key = k_spin_lock(&serial.lock);
	memset(status, 0, sizeof(*status));
	status->current_config = serial.model.current;
	status->config_counters = serial.model.config_counters;
	status->runtime_counters = serial.counters;
	status->generation = serial.model.generation;
	status->rx_head = serial.model.rx.head;
	status->tx_bytes_committed = serial.model.tx.bytes_committed;
	status->tx_accepted = serial.model.tx.accepted;
	status->tx_completed = serial.model.tx.completed;
	status->tx_canceled = serial.model.tx.canceled;
	status->last_config_request_id = serial.last_config_request_id;
	status->active_event_id =
	    serial.wire_event_valid ? serial.wire_event_id : 0U;
	status->pause_wait_budget_ms =
	    serial_pause_wait_ms(&serial.model.current);
	status->last_config_result = serial.last_config_result;
	status->state = serial.model.state;
	status->tx_used_slots = serial.model.tx.used_slots;
	status->last_config_done = serial.last_config_done;
	status->dtr = serial.model.dtr;
	status->rts = serial.rts;
	status->usb_configured = serial.usb_configured;
	status->admission_frozen = serial.admission_frozen;
	status->pause_waiting_for_tx = serial.pause_waiting_for_tx;
	status->cdc_rx_backpressured = serial.cdc_rx_backpressured;
	k_spin_unlock(&serial.lock, key);
	return 0;
}

int linkr_debugger_serial_cdc_line_coding(const struct device *dev)
{
	struct linkr_debugger_serial_config config;
	k_spinlock_key_t key;
	int ret;

	if (!serial_ready() || dev != serial.cdc) {
		return -ENODEV;
	}
	ret = serial.ops->config_get(serial.hal_context, dev, &config);
	if (ret < 0 || !linkr_debugger_serial_config_valid(&config)) {
		key = k_spin_lock(&serial.lock);
		counter_add(&serial.counters.invalid_line_coding, 1U);
		k_spin_unlock(&serial.lock, key);
		return ret < 0 ? ret : -EINVAL;
	}

	key = k_spin_lock(&serial.lock);
	if (serial.cdc_config_valid) {
		counter_add(&serial.counters.config_mailbox_busy, 1U);
	}
	serial.cdc_config = config;
	serial.cdc_config_valid = true;
	serial_refresh_admission_locked();
	k_spin_unlock(&serial.lock, key);
	wake_control(SERIAL_CONTROL_EVENT_CONFIG);
	return 0;
}

int linkr_debugger_serial_cdc_control_line(const struct device *dev)
{
	uint32_t dtr;
	uint32_t rts;
	k_spinlock_key_t key;
	int ret;

	if (!serial_ready() || dev != serial.cdc) {
		return -ENODEV;
	}
	ret = serial.ops->line_control_get(
	    serial.hal_context, dev, LINKR_DEBUGGER_SERIAL_LINE_DTR, &dtr);
	if (ret < 0) {
		return ret;
	}
	ret = serial.ops->line_control_get(
	    serial.hal_context, dev, LINKR_DEBUGGER_SERIAL_LINE_RTS, &rts);
	if (ret < 0) {
		return ret;
	}

	key = k_spin_lock(&serial.lock);
	serial.staged_control_valid = true;
	serial.staged_dtr = dtr != 0U;
	serial.staged_rts = rts != 0U;
	if (dtr == 0U) {
		serial.cdc_detach_pending = true;
		serial.cdc_gate_dtr = false;
	} else if (!serial.cdc_detach_pending) {
		serial.cdc_gate_dtr = true;
	}
	k_spin_unlock(&serial.lock, key);
	wake_control(SERIAL_CONTROL_EVENT_USB);
	return 0;
}

int linkr_debugger_serial_cdc_usb_event(
    const struct device *dev, enum linkr_debugger_serial_usb_event event,
    bool configured)
{
	k_spinlock_key_t key;

	if (!serial_ready() || dev != serial.cdc) {
		return -ENODEV;
	}
	if (event != LINKR_DEBUGGER_SERIAL_USB_RESET &&
	    event != LINKR_DEBUGGER_SERIAL_USB_CONFIGURATION) {
		return -EINVAL;
	}

	key = k_spin_lock(&serial.lock);
	serial.staged_usb_valid = true;
	serial.staged_usb_configured =
	    event == LINKR_DEBUGGER_SERIAL_USB_CONFIGURATION && configured;
	if (event == LINKR_DEBUGGER_SERIAL_USB_RESET || !configured) {
		serial.cdc_detach_pending = true;
		serial.staged_control_valid = true;
		serial.staged_dtr = false;
		serial.staged_rts = false;
		serial.cdc_gate_dtr = false;
	}
	k_spin_unlock(&serial.lock, key);
	wake_control(SERIAL_CONTROL_EVENT_USB);
	return 0;
}

int linkr_debugger_serial_runtime_pause(void)
{
	struct linkr_debugger_serial_tx_cancel_batch canceled;
	k_spinlock_key_t key;
	bool active;
	bool active_remaining;
	bool enable_cdc_rx;
	bool restore_uart_tx = false;
	uint32_t wait_ms;
	int wait_ret = 0;
	int ret;

	if (!serial_ready()) {
		return -ENODEV;
	}
	(void)k_mutex_lock(&serial.control_mutex, K_FOREVER);
	k_sem_reset(&serial.wire_complete_sem);
	key = k_spin_lock(&serial.lock);
	if (serial.model.state == LINKR_DEBUGGER_SERIAL_PAUSED) {
		k_spin_unlock(&serial.lock, key);
		(void)k_mutex_unlock(&serial.control_mutex);
		return 0;
	}
	if (serial.model.state == LINKR_DEBUGGER_SERIAL_FAULTED) {
		k_spin_unlock(&serial.lock, key);
		(void)k_mutex_unlock(&serial.control_mutex);
		return -EIO;
	}
	serial.pause_freeze = true;
	serial_refresh_admission_locked();
	active = serial.model.tx.active_slot >= 0 || serial.wire_event_valid;
	serial.pause_waiting_for_tx = active;
	wait_ms = active ? serial_pause_wait_ms(&serial.model.current) : 0U;
	k_spin_unlock(&serial.lock, key);

	if (active) {
		wait_ret =
		    k_sem_take(&serial.wire_complete_sem, K_MSEC(wait_ms));
	}
	key = k_spin_lock(&serial.lock);
	serial.pause_waiting_for_tx = false;
	active_remaining =
	    serial.model.tx.active_slot >= 0 || serial.wire_event_valid;
	if (active_remaining) {
		serial.pause_freeze = false;
		serial_refresh_admission_locked();
		serial.tx_probe_suppressed = false;
		if (wait_ret != 0) {
			counter_add(&serial.counters.pause_wait_timeouts, 1U);
		}
		enable_cdc_rx = !serial.cdc_rx_backpressured;
		if (serial.cdc_rx_backpressured && cdc_can_accept_locked()) {
			serial.cdc_rx_backpressured = false;
			enable_cdc_rx = true;
		}
		k_spin_unlock(&serial.lock, key);
		k_sem_reset(&serial.wire_complete_sem);
		serial.ops->irq_rx_enable(serial.hal_context, serial.uart);
		if (enable_cdc_rx) {
			serial.ops->irq_rx_enable(serial.hal_context,
						  serial.cdc);
		}
		/* Re-kick the retained frame; this never proves driver stop. */
		serial.ops->irq_tx_enable(serial.hal_context, serial.uart);
		wake_control(SERIAL_CONTROL_EVENT_TX_CHANGE |
			     SERIAL_CONTROL_EVENT_CONFIG);
		(void)k_mutex_unlock(&serial.control_mutex);
		return wait_ret != 0 ? -ETIMEDOUT : -EBUSY;
	}
	serial.tx_probe_suppressed = true;
	k_spin_unlock(&serial.lock, key);
	k_sem_reset(&serial.wire_complete_sem);
	k_timer_stop(&serial.tx_probe_timer);
	serial.ops->irq_tx_disable(serial.hal_context, serial.uart);
	serial.ops->irq_rx_disable(serial.hal_context, serial.uart);
	uart_drain_stale();

	key = k_spin_lock(&serial.lock);
	ret = linkr_debugger_serial_pause(&serial.model, &canceled);
	if (ret == 0) {
		canceled_push_locked(&canceled);
		cdc_pending_clear_locked(true);
		serial.cdc_rx_backpressured = false;
	} else {
		serial.pause_freeze = false;
		serial_refresh_admission_locked();
		serial.tx_probe_suppressed = false;
		restore_uart_tx = serial.model.tx.used_slots != 0U;
	}
	k_spin_unlock(&serial.lock, key);
	if (ret < 0) {
		serial.ops->irq_rx_enable(serial.hal_context, serial.uart);
		serial.ops->irq_rx_enable(serial.hal_context, serial.cdc);
		if (restore_uart_tx) {
			serial.ops->irq_tx_enable(serial.hal_context,
						  serial.uart);
		}
		wake_control(SERIAL_CONTROL_EVENT_TX_CHANGE |
			     SERIAL_CONTROL_EVENT_CONFIG);
		(void)k_mutex_unlock(&serial.control_mutex);
		return ret;
	}
	serial.ops->irq_tx_disable(serial.hal_context, serial.cdc);
	/* Keep ACM input live while paused so host bytes are drained and
	 * dropped. */
	serial.ops->irq_rx_enable(serial.hal_context, serial.cdc);
	(void)k_mutex_unlock(&serial.control_mutex);
	return 0;
}

int linkr_debugger_serial_runtime_resume(void)
{
	k_spinlock_key_t key;
	int ret;

	if (!serial_ready()) {
		return -ENODEV;
	}
	(void)k_mutex_lock(&serial.control_mutex, K_FOREVER);
	key = k_spin_lock(&serial.lock);
	if (serial.model.state != LINKR_DEBUGGER_SERIAL_PAUSED) {
		ret = serial.model.state == LINKR_DEBUGGER_SERIAL_READY ? 0
									: -EIO;
		k_spin_unlock(&serial.lock, key);
		(void)k_mutex_unlock(&serial.control_mutex);
		return ret;
	}
	k_spin_unlock(&serial.lock, key);

	k_timer_stop(&serial.tx_probe_timer);
	serial.ops->irq_tx_disable(serial.hal_context, serial.uart);
	serial.ops->irq_rx_disable(serial.hal_context, serial.uart);
	ret = serial.ops->irq_callback_set(serial.hal_context, serial.uart,
					   serial_uart_callback, &serial);
	if (ret == 0) {
		ret =
		    serial.ops->irq_callback_set(serial.hal_context, serial.cdc,
						 serial_cdc_callback, &serial);
	}
	if (ret < 0) {
		(void)k_mutex_unlock(&serial.control_mutex);
		return ret;
	}
	uart_drain_stale();
	key = k_spin_lock(&serial.lock);
	ret = linkr_debugger_serial_resume(&serial.model);
	if (ret == 0) {
		serial.pause_freeze = false;
		serial_refresh_admission_locked();
		serial.tx_probe_suppressed = false;
		serial.cdc_rx_backpressured = false;
		cdc_pending_clear_locked(true);
	}
	k_spin_unlock(&serial.lock, key);
	if (ret == 0) {
		serial.ops->irq_rx_enable(serial.hal_context, serial.uart);
		serial.ops->irq_rx_enable(serial.hal_context, serial.cdc);
		wake_control(SERIAL_CONTROL_EVENT_CONFIG |
			     SERIAL_CONTROL_EVENT_TX_CHANGE);
	}
	(void)k_mutex_unlock(&serial.control_mutex);
	return ret;
}
