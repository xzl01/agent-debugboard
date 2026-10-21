/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_serial_model.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#ifndef BUILD_ASSERT
#define BUILD_ASSERT(condition, message) _Static_assert(condition, message)
#endif

BUILD_ASSERT(sizeof(size_t) <= sizeof(uint64_t),
	     "serial model requires size_t to fit in uint64_t");
BUILD_ASSERT(sizeof(struct linkr_debugger_serial_model) <=
		     LINKR_DEBUGGER_SERIAL_STATIC_MAX,
	     "serial model exceeds its static memory budget");

const uint32_t
	linkr_debugger_serial_baud_presets[LINKR_DEBUGGER_SERIAL_BAUD_PRESET_COUNT] = {
		9600U, 19200U, 38400U, 57600U, 115200U,
	};

static bool sink_valid(enum linkr_debugger_serial_sink sink)
{
	return sink >= LINKR_DEBUGGER_SERIAL_SINK_CDC &&
	       sink < LINKR_DEBUGGER_SERIAL_SINK_COUNT;
}

static bool tx_source_valid(enum linkr_debugger_serial_tx_source source)
{
	return source >= LINKR_DEBUGGER_SERIAL_TX_CDC &&
	       source < LINKR_DEBUGGER_SERIAL_TX_SOURCE_COUNT;
}

static void counter_add(uint64_t *counter, uint64_t amount)
{
	if (UINT64_MAX - *counter < amount) {
		*counter = UINT64_MAX;
	} else {
		*counter += amount;
	}
}

static bool baud_valid(uint32_t baud)
{
	return baud >= LINKR_DEBUGGER_SERIAL_BAUD_MIN &&
	       baud <= LINKR_DEBUGGER_SERIAL_BAUD_MAX;
}

bool linkr_debugger_serial_config_valid(
	const struct linkr_debugger_serial_config *config)
{
	if (config == NULL || !baud_valid(config->baud)) {
		return false;
	}
	if (config->data_bits < 5U || config->data_bits > 8U) {
		return false;
	}
	if (config->parity < LINKR_DEBUGGER_SERIAL_PARITY_NONE ||
	    config->parity > LINKR_DEBUGGER_SERIAL_PARITY_EVEN) {
		return false;
	}
	if (config->stop_bits != 1U && config->stop_bits != 2U) {
		return false;
	}
	return config->flow_control == LINKR_DEBUGGER_SERIAL_FLOW_NONE;
}

bool linkr_debugger_serial_config_equal(
	const struct linkr_debugger_serial_config *left,
	const struct linkr_debugger_serial_config *right)
{
	return left != NULL && right != NULL && left->baud == right->baud &&
	       left->data_bits == right->data_bits &&
	       left->parity == right->parity &&
	       left->stop_bits == right->stop_bits &&
	       left->flow_control == right->flow_control;
}

int linkr_debugger_serial_model_init(
	struct linkr_debugger_serial_model *model,
	const struct linkr_debugger_serial_config *initial_config)
{
	struct linkr_debugger_serial_config initial;

	if (model == NULL || !linkr_debugger_serial_config_valid(initial_config)) {
		return -EINVAL;
	}

	initial = *initial_config;
	memset(model, 0, sizeof(*model));
	model->tx.active_slot = -1;
	model->current = initial;
	model->last_good = initial;
	model->generation = 1U;
	model->state = LINKR_DEBUGGER_SERIAL_READY;
	return 0;
}

int linkr_debugger_serial_rx_sink_register(
	struct linkr_debugger_serial_model *model,
	enum linkr_debugger_serial_sink sink)
{
	struct linkr_debugger_serial_rx_sink_state *state;

	if (model == NULL || !sink_valid(sink)) {
		return -EINVAL;
	}
	state = &model->rx.sinks[sink];
	if (state->registered) {
		return -EALREADY;
	}

	state->registered = true;
	state->attached = false;
	state->cursor = model->rx.head;
	state->epoch = 1U;
	return 0;
}

int linkr_debugger_serial_rx_sink_attach(
	struct linkr_debugger_serial_model *model,
	enum linkr_debugger_serial_sink sink)
{
	struct linkr_debugger_serial_rx_sink_state *state;

	if (model == NULL || !sink_valid(sink)) {
		return -EINVAL;
	}
	state = &model->rx.sinks[sink];
	if (!state->registered) {
		return -ENOENT;
	}
	if (state->attached) {
		return 0;
	}
	if (state->epoch == UINT64_MAX) {
		return -EOVERFLOW;
	}

	state->cursor = model->rx.head;
	state->attached = true;
	state->epoch++;
	return 0;
}

int linkr_debugger_serial_rx_sink_detach(
	struct linkr_debugger_serial_model *model,
	enum linkr_debugger_serial_sink sink)
{
	struct linkr_debugger_serial_rx_sink_state *state;

	if (model == NULL || !sink_valid(sink)) {
		return -EINVAL;
	}
	state = &model->rx.sinks[sink];
	if (!state->registered) {
		return -ENOENT;
	}
	if (!state->attached) {
		return 0;
	}
	if (state->epoch == UINT64_MAX) {
		return -EOVERFLOW;
	}

	state->cursor = model->rx.head;
	state->attached = false;
	state->epoch++;
	return 0;
}

int linkr_debugger_serial_rx_sink_rearm(
	struct linkr_debugger_serial_model *model,
	enum linkr_debugger_serial_sink sink)
{
	struct linkr_debugger_serial_rx_sink_state *state;

	if (model == NULL || !sink_valid(sink)) {
		return -EINVAL;
	}
	state = &model->rx.sinks[sink];
	if (!state->registered) {
		return -ENOENT;
	}
	if (!state->attached) {
		return -ENOTCONN;
	}
	if (state->epoch == UINT64_MAX) {
		return -EOVERFLOW;
	}
	state->cursor = model->rx.head;
	state->epoch++;
	return 0;
}

static void rx_sink_settle_overflow(
	const struct linkr_debugger_serial_model *model,
	struct linkr_debugger_serial_rx_sink_state *sink)
{
	uint64_t oldest = model->rx.head > LINKR_DEBUGGER_SERIAL_RX_CAPACITY ?
			  model->rx.head - LINKR_DEBUGGER_SERIAL_RX_CAPACITY :
			  0U;

	if (sink->cursor < oldest) {
		counter_add(&sink->dropped, oldest - sink->cursor);
		sink->cursor = oldest;
	}
}

int linkr_debugger_serial_rx_write(struct linkr_debugger_serial_model *model,
				   const uint8_t *data, size_t length)
{
	uint64_t position;
	size_t i;

	if (model == NULL || (data == NULL && length != 0U)) {
		return -EINVAL;
	}
	if (model->state == LINKR_DEBUGGER_SERIAL_PAUSED) {
		counter_add(&model->rx.paused_dropped, length);
		return 0;
	}
	if ((uint64_t)length > UINT64_MAX - model->rx.head) {
		return -EOVERFLOW;
	}

	position = model->rx.head;
	for (i = 0U; i < length; ++i) {
		model->rx.data[position % LINKR_DEBUGGER_SERIAL_RX_CAPACITY] = data[i];
		position++;
	}
	model->rx.head = position;
	for (i = 0U; i < LINKR_DEBUGGER_SERIAL_SINK_COUNT; ++i) {
		if (model->rx.sinks[i].registered && model->rx.sinks[i].attached) {
			rx_sink_settle_overflow(model, &model->rx.sinks[i]);
		}
	}
	return 0;
}

int linkr_debugger_serial_rx_read(struct linkr_debugger_serial_model *model,
				  enum linkr_debugger_serial_sink sink,
				  uint8_t *data, size_t capacity,
				  size_t *length)
{
	struct linkr_debugger_serial_rx_sink_state *state;
	uint64_t available;
	size_t count;
	size_t first;

	if (model == NULL || !sink_valid(sink) || length == NULL ||
	    (data == NULL && capacity != 0U)) {
		return -EINVAL;
	}
	*length = 0U;
	if (model->state == LINKR_DEBUGGER_SERIAL_PAUSED) {
		return -EAGAIN;
	}
	state = &model->rx.sinks[sink];
	if (!state->registered) {
		return -ENOENT;
	}
	if (!state->attached) {
		return -ENOTCONN;
	}

	rx_sink_settle_overflow(model, state);

	available = model->rx.head - state->cursor;
	count = available < (uint64_t)capacity ? (size_t)available : capacity;
	if (count == 0U) {
		return 0;
	}

	first = LINKR_DEBUGGER_SERIAL_RX_CAPACITY -
		(size_t)(state->cursor % LINKR_DEBUGGER_SERIAL_RX_CAPACITY);
	if (first > count) {
		first = count;
	}
	memcpy(data,
	       &model->rx.data[state->cursor % LINKR_DEBUGGER_SERIAL_RX_CAPACITY],
	       first);
	if (first < count) {
		memcpy(data + first, model->rx.data, count - first);
	}
	state->cursor += count;
	counter_add(&state->delivered, count);
	*length = count;
	return 0;
}

static uint8_t tx_source_quota(enum linkr_debugger_serial_tx_source source)
{
	return source == LINKR_DEBUGGER_SERIAL_TX_CDC ? 2U : 1U;
}

static uint8_t tx_source_used(const struct linkr_debugger_serial_model *model,
			      enum linkr_debugger_serial_tx_source source)
{
	uint8_t used = 0U;
	size_t i;

	for (i = 0U; i < LINKR_DEBUGGER_SERIAL_TX_SLOT_COUNT; ++i) {
		if (model->tx.slots[i].used && model->tx.slots[i].source == source) {
			used++;
		}
	}
	return used;
}

static void tx_rejected(struct linkr_debugger_serial_model *model,
			enum linkr_debugger_serial_tx_source source)
{
	counter_add(&model->tx.rejected, 1U);
	counter_add(&model->tx.sources[source].rejected, 1U);
}

int linkr_debugger_serial_tx_enqueue(
	struct linkr_debugger_serial_model *model,
	enum linkr_debugger_serial_tx_source source, const uint8_t *data,
	size_t length, uint64_t *event_id)
{
	struct linkr_debugger_serial_tx_slot *slot = NULL;
	size_t i;

	if (model == NULL || !tx_source_valid(source) || event_id == NULL) {
		return -EINVAL;
	}
	if (data == NULL || length == 0U) {
		tx_rejected(model, source);
		return -EINVAL;
	}
	if (length > LINKR_DEBUGGER_SERIAL_TX_MESSAGE_MAX) {
		tx_rejected(model, source);
		return -EMSGSIZE;
	}
	if (model->tx.last_event_id == UINT64_MAX) {
		return -EOVERFLOW;
	}
	if (model->state == LINKR_DEBUGGER_SERIAL_PAUSED ||
	    model->state == LINKR_DEBUGGER_SERIAL_FAULTED ||
	    model->tx.used_slots >= LINKR_DEBUGGER_SERIAL_TX_SLOT_COUNT ||
	    tx_source_used(model, source) >= tx_source_quota(source)) {
		tx_rejected(model, source);
		return -EAGAIN;
	}

	for (i = 0U; i < LINKR_DEBUGGER_SERIAL_TX_SLOT_COUNT; ++i) {
		if (!model->tx.slots[i].used) {
			slot = &model->tx.slots[i];
			break;
		}
	}
	if (slot == NULL) {
		tx_rejected(model, source);
		return -EAGAIN;
	}

	memcpy(slot->data, data, length);
	slot->length = (uint16_t)length;
	slot->offset = 0U;
	slot->event_id = ++model->tx.last_event_id;
	slot->source = source;
	slot->used = true;
	model->tx.used_slots++;
	counter_add(&model->tx.accepted, 1U);
	counter_add(&model->tx.sources[source].accepted, 1U);
	*event_id = slot->event_id;
	return 0;
}

static int tx_oldest_slot(const struct linkr_debugger_serial_model *model,
			  bool exclude_active)
{
	uint64_t oldest_id = 0U;
	bool found = false;
	int oldest = -1;
	size_t i;

	for (i = 0U; i < LINKR_DEBUGGER_SERIAL_TX_SLOT_COUNT; ++i) {
		if (!model->tx.slots[i].used ||
		    (exclude_active && (int)i == model->tx.active_slot)) {
			continue;
		}
		if (!found || model->tx.slots[i].event_id < oldest_id) {
			oldest_id = model->tx.slots[i].event_id;
			oldest = (int)i;
			found = true;
		}
	}
	return oldest;
}

static int tx_ready(const struct linkr_debugger_serial_model *model)
{
	if (model->state == LINKR_DEBUGGER_SERIAL_FAULTED) {
		return -EIO;
	}
	if (model->state == LINKR_DEBUGGER_SERIAL_RECONFIG) {
		return -EBUSY;
	}
	if (model->state != LINKR_DEBUGGER_SERIAL_READY) {
		return -EAGAIN;
	}
	return 0;
}

int linkr_debugger_serial_tx_peek(
	struct linkr_debugger_serial_model *model, const uint8_t **data,
	size_t *length,
	enum linkr_debugger_serial_tx_source *source, uint64_t *event_id)
{
	struct linkr_debugger_serial_tx_slot *slot;
	int ret;

	if (model == NULL || data == NULL || length == NULL ||
	    source == NULL || event_id == NULL) {
		return -EINVAL;
	}
	*data = NULL;
	*length = 0U;
	ret = tx_ready(model);
	if (ret < 0) {
		return ret;
	}
	if (model->tx.active_slot < 0) {
		model->tx.active_slot = (int8_t)tx_oldest_slot(model, false);
	}
	if (model->tx.active_slot < 0) {
		return -EAGAIN;
	}

	slot = &model->tx.slots[(size_t)model->tx.active_slot];
	*source = slot->source;
	*event_id = slot->event_id;
	*length = (size_t)slot->length - slot->offset;
	if (*length == 0U) {
		return -EAGAIN;
	}
	*data = slot->data + slot->offset;
	return 0;
}

int linkr_debugger_serial_tx_commit(
	struct linkr_debugger_serial_model *model, uint64_t event_id,
	size_t accepted)
{
	struct linkr_debugger_serial_tx_slot *slot;
	size_t remaining;
	int ret;

	if (model == NULL) {
		return -EINVAL;
	}
	ret = tx_ready(model);
	if (ret < 0) {
		return ret;
	}
	if (model->tx.active_slot < 0) {
		return -ENOENT;
	}
	slot = &model->tx.slots[(size_t)model->tx.active_slot];
	if (slot->event_id != event_id) {
		return -ESTALE;
	}
	remaining = (size_t)slot->length - slot->offset;
	if (accepted > remaining) {
		return -EINVAL;
	}

	slot->offset = (uint16_t)(slot->offset + accepted);
	counter_add(&model->tx.bytes_committed, accepted);
	return 0;
}

static void tx_release_slot(struct linkr_debugger_serial_model *model,
			    size_t slot_index, bool completed)
{
	struct linkr_debugger_serial_tx_slot *slot = &model->tx.slots[slot_index];
	struct linkr_debugger_serial_tx_source_counters *source =
		&model->tx.sources[slot->source];

	if (completed) {
		counter_add(&model->tx.completed, 1U);
		counter_add(&source->completed, 1U);
		model->tx.last_completed_event_id = slot->event_id;
	} else {
		counter_add(&model->tx.canceled, 1U);
		counter_add(&source->canceled, 1U);
		model->tx.last_canceled_event_id = slot->event_id;
	}
	memset(slot, 0, sizeof(*slot));
	model->tx.used_slots--;
}

int linkr_debugger_serial_tx_finish_active(
	struct linkr_debugger_serial_model *model, uint64_t event_id,
	bool wire_complete)
{
	struct linkr_debugger_serial_tx_slot *slot;
	size_t index;

	if (model == NULL) {
		return -EINVAL;
	}
	if (model->state == LINKR_DEBUGGER_SERIAL_FAULTED) {
		return -EIO;
	}
	if (model->state == LINKR_DEBUGGER_SERIAL_RECONFIG) {
		return -EBUSY;
	}
	if (model->tx.active_slot < 0) {
		return -ENOENT;
	}
	index = (size_t)model->tx.active_slot;
	slot = &model->tx.slots[index];
	if (slot->event_id != event_id) {
		return -ESTALE;
	}
	if (slot->offset != slot->length) {
		return -EAGAIN;
	}
	if (!wire_complete) {
		return -EBUSY;
	}

	tx_release_slot(model, index, true);
	model->tx.active_slot = -1;
	return 0;
}

int linkr_debugger_serial_tx_cancel_active(
	struct linkr_debugger_serial_model *model, uint64_t event_id,
	bool driver_stopped)
{
	struct linkr_debugger_serial_tx_slot *slot;
	size_t index;

	if (model == NULL) {
		return -EINVAL;
	}
	if (model->tx.active_slot < 0) {
		return -ENOENT;
	}
	index = (size_t)model->tx.active_slot;
	slot = &model->tx.slots[index];
	if (slot->event_id != event_id) {
		return -ESTALE;
	}
	if (!driver_stopped) {
		return -EBUSY;
	}

	tx_release_slot(model, index, false);
	model->tx.active_slot = -1;
	return 0;
}

int linkr_debugger_serial_tx_cancel_all(
	struct linkr_debugger_serial_model *model, bool driver_stopped,
	struct linkr_debugger_serial_tx_cancel_batch *canceled)
{
	struct linkr_debugger_serial_tx_slot *slot;
	int index;

	if (model == NULL || canceled == NULL) {
		return -EINVAL;
	}
	canceled->count = 0U;
	if (model->tx.active_slot >= 0 && !driver_stopped) {
		return -EBUSY;
	}

	for (;;) {
		index = tx_oldest_slot(model, false);
		if (index < 0) {
			break;
		}
		slot = &model->tx.slots[(size_t)index];
		canceled->events[canceled->count].event_id = slot->event_id;
		canceled->events[canceled->count].source = slot->source;
		canceled->count++;
		if (index == model->tx.active_slot) {
			model->tx.active_slot = -1;
		}
		tx_release_slot(model, (size_t)index, false);
	}
	return 0;
}

static int rx_boundary_preflight(
	const struct linkr_debugger_serial_model *model)
{
	size_t i;

	for (i = 0U; i < LINKR_DEBUGGER_SERIAL_SINK_COUNT; ++i) {
		if (model->rx.sinks[i].registered && model->rx.sinks[i].attached &&
		    model->rx.sinks[i].epoch == UINT64_MAX) {
			return -EOVERFLOW;
		}
	}
	return 0;
}

static void rx_pause_boundary(struct linkr_debugger_serial_model *model)
{
	struct linkr_debugger_serial_rx_sink_state *sink;
	size_t i;

	for (i = 0U; i < LINKR_DEBUGGER_SERIAL_SINK_COUNT; ++i) {
		sink = &model->rx.sinks[i];
		if (!sink->registered || !sink->attached) {
			continue;
		}
		rx_sink_settle_overflow(model, sink);
		if (sink->cursor < model->rx.head) {
			counter_add(&sink->dropped, model->rx.head - sink->cursor);
		}
		sink->cursor = model->rx.head;
		sink->epoch++;
	}
}

static void rx_resume_boundary(struct linkr_debugger_serial_model *model)
{
	size_t i;

	for (i = 0U; i < LINKR_DEBUGGER_SERIAL_SINK_COUNT; ++i) {
		if (!model->rx.sinks[i].registered ||
		    !model->rx.sinks[i].attached) {
			continue;
		}
		model->rx.sinks[i].cursor = model->rx.head;
		model->rx.sinks[i].epoch++;
	}
}

int linkr_debugger_serial_pause(
	struct linkr_debugger_serial_model *model,
	struct linkr_debugger_serial_tx_cancel_batch *canceled)
{
	struct linkr_debugger_serial_tx_slot *slot;
	int index;

	if (model == NULL || canceled == NULL) {
		return -EINVAL;
	}
	canceled->count = 0U;
	if (model->state == LINKR_DEBUGGER_SERIAL_PAUSED) {
		return 0;
	}
	if (model->state == LINKR_DEBUGGER_SERIAL_RECONFIG) {
		return -EBUSY;
	}
	if (model->state == LINKR_DEBUGGER_SERIAL_FAULTED) {
		return -EIO;
	}
	if (rx_boundary_preflight(model) < 0) {
		return -EOVERFLOW;
	}

	model->state = LINKR_DEBUGGER_SERIAL_PAUSED;
	rx_pause_boundary(model);
	for (;;) {
		index = tx_oldest_slot(model, true);
		if (index < 0) {
			break;
		}
		slot = &model->tx.slots[(size_t)index];
		canceled->events[canceled->count].event_id = slot->event_id;
		canceled->events[canceled->count].source = slot->source;
		canceled->count++;
		tx_release_slot(model, (size_t)index, false);
	}
	return 0;
}

int linkr_debugger_serial_resume(struct linkr_debugger_serial_model *model)
{
	if (model == NULL) {
		return -EINVAL;
	}
	if (model->state == LINKR_DEBUGGER_SERIAL_READY) {
		return 0;
	}
	if (model->state != LINKR_DEBUGGER_SERIAL_PAUSED) {
		return model->state == LINKR_DEBUGGER_SERIAL_FAULTED ? -EIO : -EBUSY;
	}
	if (rx_boundary_preflight(model) < 0) {
		return -EOVERFLOW;
	}
	rx_resume_boundary(model);
	model->state = LINKR_DEBUGGER_SERIAL_READY;
	return 0;
}

int linkr_debugger_serial_mark_faulted(
	struct linkr_debugger_serial_model *model)
{
	if (model == NULL) {
		return -EINVAL;
	}
	model->state = LINKR_DEBUGGER_SERIAL_FAULTED;
	model->pending_valid = false;
	model->rollback_pending = false;
	return 0;
}

int linkr_debugger_serial_set_dtr(struct linkr_debugger_serial_model *model,
				  bool asserted)
{
	struct linkr_debugger_serial_rx_sink_state *cdc;
	int ret = 0;

	if (model == NULL) {
		return -EINVAL;
	}
	cdc = &model->rx.sinks[LINKR_DEBUGGER_SERIAL_SINK_CDC];
	if (cdc->registered) {
		ret = asserted ?
			linkr_debugger_serial_rx_sink_attach(
				model, LINKR_DEBUGGER_SERIAL_SINK_CDC) :
			linkr_debugger_serial_rx_sink_detach(
				model, LINKR_DEBUGGER_SERIAL_SINK_CDC);
		if (ret < 0) {
			return ret;
		}
	}
	if (model->dtr != asserted) {
		model->dtr = asserted;
		counter_add(&model->config_counters.control_line_changes, 1U);
	}
	return 0;
}

static int config_request_common(
	struct linkr_debugger_serial_model *model,
	const struct linkr_debugger_serial_config *config,
	enum linkr_debugger_serial_config_origin origin,
	enum linkr_debugger_serial_config_decision *decision)
{
	if (!linkr_debugger_serial_config_valid(config)) {
		counter_add(&model->config_counters.invalid, 1U);
		return -EINVAL;
	}
	if (model->state == LINKR_DEBUGGER_SERIAL_RECONFIG) {
		counter_add(&model->config_counters.conflicts, 1U);
		return -EBUSY;
	}
	if (model->state == LINKR_DEBUGGER_SERIAL_PAUSED) {
		counter_add(&model->config_counters.conflicts, 1U);
		return -EAGAIN;
	}
	if (model->state == LINKR_DEBUGGER_SERIAL_FAULTED) {
		counter_add(&model->config_counters.conflicts, 1U);
		return -EIO;
	}
	if (linkr_debugger_serial_config_equal(config, &model->current)) {
		counter_add(&model->config_counters.no_change, 1U);
		*decision = LINKR_DEBUGGER_SERIAL_CONFIG_NO_CHANGE;
		return 0;
	}
	if (model->tx.active_slot >= 0) {
		counter_add(&model->config_counters.conflicts, 1U);
		return -EBUSY;
	}
	if (model->generation == UINT64_MAX) {
		counter_add(&model->config_counters.conflicts, 1U);
		return -EOVERFLOW;
	}

	model->pending = *config;
	model->pending_origin = origin;
	model->pending_valid = true;
	model->rollback_pending = false;
	model->state = LINKR_DEBUGGER_SERIAL_RECONFIG;
	counter_add(&model->config_counters.configure_count, 1U);
	*decision = LINKR_DEBUGGER_SERIAL_CONFIG_APPLY;
	return 0;
}

int linkr_debugger_serial_config_request_cdc(
	struct linkr_debugger_serial_model *model,
	const struct linkr_debugger_serial_config *config,
	enum linkr_debugger_serial_config_decision *decision)
{
	if (model == NULL || decision == NULL) {
		return -EINVAL;
	}
	counter_add(&model->config_counters.cdc_requests, 1U);
	return config_request_common(model, config,
				     LINKR_DEBUGGER_SERIAL_CONFIG_ORIGIN_CDC,
				     decision);
}

int linkr_debugger_serial_config_request_web(
	struct linkr_debugger_serial_model *model,
	const struct linkr_debugger_serial_config *config,
	uint64_t expected_generation,
	enum linkr_debugger_serial_config_decision *decision)
{
	if (model == NULL || decision == NULL) {
		return -EINVAL;
	}
	counter_add(&model->config_counters.web_requests, 1U);
	if (!linkr_debugger_serial_config_valid(config)) {
		counter_add(&model->config_counters.invalid, 1U);
		return -EINVAL;
	}
	if (model->state == LINKR_DEBUGGER_SERIAL_RECONFIG) {
		counter_add(&model->config_counters.conflicts, 1U);
		return -EBUSY;
	}
	if (model->state == LINKR_DEBUGGER_SERIAL_PAUSED) {
		counter_add(&model->config_counters.conflicts, 1U);
		return -EAGAIN;
	}
	if (model->state == LINKR_DEBUGGER_SERIAL_FAULTED) {
		counter_add(&model->config_counters.conflicts, 1U);
		return -EIO;
	}
	if (linkr_debugger_serial_config_equal(config, &model->current)) {
		counter_add(&model->config_counters.no_change, 1U);
		*decision = LINKR_DEBUGGER_SERIAL_CONFIG_NO_CHANGE;
		return 0;
	}
	if (model->dtr) {
		counter_add(&model->config_counters.conflicts, 1U);
		return -EBUSY;
	}
	if (expected_generation != model->generation) {
		counter_add(&model->config_counters.conflicts, 1U);
		return -ESTALE;
	}
	return config_request_common(model, config,
				     LINKR_DEBUGGER_SERIAL_CONFIG_ORIGIN_WEB,
				     decision);
}

int linkr_debugger_serial_config_apply_result(
	struct linkr_debugger_serial_model *model, bool succeeded)
{
	if (model == NULL) {
		return -EINVAL;
	}
	if (model->state != LINKR_DEBUGGER_SERIAL_RECONFIG ||
	    !model->pending_valid || model->rollback_pending) {
		return -EINVAL;
	}

	if (succeeded) {
		model->current = model->pending;
		model->last_good = model->pending;
		model->generation++;
		model->pending_valid = false;
		model->state = LINKR_DEBUGGER_SERIAL_READY;
		counter_add(&model->config_counters.apply_succeeded, 1U);
	} else {
		model->rollback_pending = true;
		counter_add(&model->config_counters.apply_failed, 1U);
	}
	return 0;
}

int linkr_debugger_serial_config_rollback_result(
	struct linkr_debugger_serial_model *model, bool succeeded)
{
	if (model == NULL) {
		return -EINVAL;
	}
	if (model->state != LINKR_DEBUGGER_SERIAL_RECONFIG ||
	    !model->pending_valid || !model->rollback_pending) {
		return -EINVAL;
	}

	model->pending_valid = false;
	model->rollback_pending = false;
	if (succeeded) {
		model->current = model->last_good;
		model->state = LINKR_DEBUGGER_SERIAL_READY;
		counter_add(&model->config_counters.rollback_succeeded, 1U);
	} else {
		model->state = LINKR_DEBUGGER_SERIAL_FAULTED;
		counter_add(&model->config_counters.rollback_failed, 1U);
	}
	return 0;
}
