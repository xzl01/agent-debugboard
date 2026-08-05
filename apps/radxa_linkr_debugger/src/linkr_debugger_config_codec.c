/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_config_codec.h"

#include <string.h>

#ifdef __ZEPHYR__
#include <zephyr/settings/settings.h>
#else
#define SETTINGS_MAX_VAL_LEN 256
#endif

#define ARRAY_SIZE_LOCAL(array) (sizeof(array) / sizeof((array)[0]))
#define POWER_ITEM(item_id, name) { LINKR_DEBUGGER_CONFIG_DOMAIN_POWER, item_id, "power/" name }
#define SWITCH_ITEM(item_id, name) { LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH, item_id, "switch/" name }

_Static_assert(SETTINGS_MAX_VAL_LEN == LINKR_DEBUGGER_CONFIG_SETTINGS_MAX_VAL_LEN,
	       "Zephyr Settings value limit changed");

const struct linkr_debugger_config_item_desc linkr_debugger_config_items[] = {
	POWER_ITEM(LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID, "12v_out"),
	POWER_ITEM(LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID, "5v_out"),
	POWER_ITEM(LINKR_DEBUGGER_CONFIG_POWER_VDD_5V_ID, "vdd_5v"),
	POWER_ITEM(LINKR_DEBUGGER_CONFIG_POWER_20V_OUT_ID, "20v_out"),
	SWITCH_ITEM(LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID, "sd"),
	SWITCH_ITEM(LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID, "usb"),
	SWITCH_ITEM(LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID, "tf_wp"),
	SWITCH_ITEM(LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID, "vin"),
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 7U, "gpio/GP7" },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 8U, "gpio/GP8" },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 9U, "gpio/GP9" },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 10U, "gpio/GP10" },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 11U, "gpio/GP11" },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 12U, "gpio/GP12" },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 13U, "gpio/GP13" },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 14U, "gpio/GP14" },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 15U, "gpio/GP15" },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 16U, "gpio/GP16" },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 17U, "gpio/GP17" },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 18U, "gpio/GP18" },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 19U, "gpio/GP19" },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 20U, "gpio/GP20" },
	{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 29U, "gpio/GP29" },
};

const size_t linkr_debugger_config_item_count = ARRAY_SIZE_LOCAL(linkr_debugger_config_items);

_Static_assert(ARRAY_SIZE_LOCAL(linkr_debugger_config_items) ==
	       LINKR_DEBUGGER_CONFIG_MAX_ENTRIES, "persistent catalog is incomplete");

void linkr_debugger_config_snapshot_clear(struct linkr_debugger_config_snapshot *snapshot)
{
	if (snapshot != NULL) {
		memset(snapshot, 0, sizeof(*snapshot));
	}
}

const struct linkr_debugger_config_item_desc *linkr_debugger_config_find_item(
	uint8_t domain, uint8_t item_id)
{
	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		if (linkr_debugger_config_items[i].domain == domain &&
		    linkr_debugger_config_items[i].item_id == item_id) {
			return &linkr_debugger_config_items[i];
		}
	}

	return NULL;
}

const struct linkr_debugger_config_item_desc *linkr_debugger_config_find_item_by_name(
	const char *id)
{
	if (id == NULL) {
		return NULL;
	}

	for (size_t i = 0U; i < linkr_debugger_config_item_count; i++) {
		if (strcmp(linkr_debugger_config_items[i].id, id) == 0) {
			return &linkr_debugger_config_items[i];
		}
	}

	return NULL;
}

static bool switch_value_valid(uint8_t item_id, uint8_t value)
{
	switch (item_id) {
	case LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID:
		return value == LINKR_DEBUGGER_CONFIG_SD_TARGET ||
		       value == LINKR_DEBUGGER_CONFIG_SD_USB_READER;
	case LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID:
		return value == LINKR_DEBUGGER_CONFIG_USB_PC ||
		       value == LINKR_DEBUGGER_CONFIG_USB_TARGET;
	case LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID:
		return value == LINKR_DEBUGGER_CONFIG_TF_WP_WRITABLE ||
		       value == LINKR_DEBUGGER_CONFIG_TF_WP_PROTECTED;
	case LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID:
		return value == LINKR_DEBUGGER_CONFIG_VIN_1V8 ||
		       value == LINKR_DEBUGGER_CONFIG_VIN_3V3;
	default:
		return false;
	}
}

static enum linkr_debugger_config_codec_result validate_entry(
	const struct linkr_debugger_config_entry *entry)
{
	if (linkr_debugger_config_find_item(entry->domain, entry->item_id) == NULL) {
		return LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED;
	}

	switch (entry->domain) {
	case LINKR_DEBUGGER_CONFIG_DOMAIN_POWER:
		return entry->value == LINKR_DEBUGGER_CONFIG_POWER_OFF ||
		       entry->value == LINKR_DEBUGGER_CONFIG_POWER_ON ?
		       LINKR_DEBUGGER_CONFIG_CODEC_OK : LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED;
	case LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH:
		return switch_value_valid(entry->item_id, entry->value) ?
		       LINKR_DEBUGGER_CONFIG_CODEC_OK : LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED;
	case LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO:
		return (entry->value & ~LINKR_DEBUGGER_CONFIG_GPIO_VALUE_MASK) == 0U ?
		       LINKR_DEBUGGER_CONFIG_CODEC_OK : LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED;
	default:
		return LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED;
	}
}

enum linkr_debugger_config_codec_result linkr_debugger_config_classify_entry(
	const struct linkr_debugger_config_entry *entry, bool *requires_confirmation)
{
	enum linkr_debugger_config_codec_result result;

	if (entry == NULL || requires_confirmation == NULL) {
		return LINKR_DEBUGGER_CONFIG_CODEC_INVALID_ARGUMENT;
	}
	*requires_confirmation = false;
	result = validate_entry(entry);
	if (result != LINKR_DEBUGGER_CONFIG_CODEC_OK) {
		return result;
	}

	*requires_confirmation =
		(entry->domain == LINKR_DEBUGGER_CONFIG_DOMAIN_POWER &&
		 entry->value == LINKR_DEBUGGER_CONFIG_POWER_ON) ||
		(entry->domain == LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH &&
		 (entry->item_id == LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID ||
		  (entry->item_id == LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID &&
		   entry->value == LINKR_DEBUGGER_CONFIG_VIN_1V8))) ||
		(entry->domain == LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO &&
		 (entry->value & LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT) != 0U);
	return LINKR_DEBUGGER_CONFIG_CODEC_OK;
}

static bool entry_follows(const struct linkr_debugger_config_entry *entry,
			  const struct linkr_debugger_config_entry *previous)
{
	return entry->domain > previous->domain ||
	       (entry->domain == previous->domain && entry->item_id > previous->item_id);
}

static enum linkr_debugger_config_codec_result validate_snapshot(
	const struct linkr_debugger_config_snapshot *snapshot)
{
	if (snapshot->entry_count == 0U) {
		return LINKR_DEBUGGER_CONFIG_CODEC_EMPTY_SELECTION;
	}
	if (snapshot->entry_count > LINKR_DEBUGGER_CONFIG_MAX_ENTRIES) {
		return LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED;
	}

	for (size_t i = 0U; i < snapshot->entry_count; i++) {
		const struct linkr_debugger_config_entry *entry = &snapshot->entries[i];

		if (validate_entry(entry) != LINKR_DEBUGGER_CONFIG_CODEC_OK) {
			return LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED;
		}
		if (i > 0U && !entry_follows(entry, &snapshot->entries[i - 1U])) {
			return LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED;
		}
	}

	return LINKR_DEBUGGER_CONFIG_CODEC_OK;
}

enum linkr_debugger_config_codec_result linkr_debugger_config_encode(
	const struct linkr_debugger_config_snapshot *snapshot, uint8_t *buffer,
	size_t buffer_size, size_t *encoded_size)
{
	enum linkr_debugger_config_codec_result result;
	size_t total_size;

	if (encoded_size != NULL) {
		*encoded_size = 0U;
	}
	if (snapshot == NULL || buffer == NULL || encoded_size == NULL) {
		return LINKR_DEBUGGER_CONFIG_CODEC_INVALID_ARGUMENT;
	}
	result = validate_snapshot(snapshot);
	if (result != LINKR_DEBUGGER_CONFIG_CODEC_OK) {
		return result;
	}
	total_size = LINKR_DEBUGGER_CONFIG_HEADER_SIZE +
		(snapshot->entry_count * LINKR_DEBUGGER_CONFIG_ENTRY_SIZE);
	if (buffer_size < total_size) {
		return LINKR_DEBUGGER_CONFIG_CODEC_NO_SPACE;
	}

	memset(buffer, 0, total_size);
	memcpy(buffer, LINKR_DEBUGGER_CONFIG_MAGIC, 4U);
	buffer[4] = LINKR_DEBUGGER_CONFIG_VERSION;
	buffer[5] = LINKR_DEBUGGER_CONFIG_ENTRY_SIZE;
	buffer[6] = (uint8_t)snapshot->entry_count;
	buffer[8] = (uint8_t)total_size;
	buffer[9] = (uint8_t)(total_size >> 8U);
	for (size_t i = 0U; i < snapshot->entry_count; i++) {
		const size_t offset = LINKR_DEBUGGER_CONFIG_HEADER_SIZE +
			(i * LINKR_DEBUGGER_CONFIG_ENTRY_SIZE);

		buffer[offset] = snapshot->entries[i].domain;
		buffer[offset + 1U] = snapshot->entries[i].item_id;
		buffer[offset + 2U] = snapshot->entries[i].value;
	}
	*encoded_size = total_size;
	return LINKR_DEBUGGER_CONFIG_CODEC_OK;
}

enum linkr_debugger_config_codec_result linkr_debugger_config_decode(
	const uint8_t *buffer, size_t buffer_size,
	struct linkr_debugger_config_snapshot *snapshot)
{
	struct linkr_debugger_config_snapshot decoded = {0};
	size_t expected_size, total_size;

	if (snapshot == NULL) {
		return LINKR_DEBUGGER_CONFIG_CODEC_INVALID_ARGUMENT;
	}
	linkr_debugger_config_snapshot_clear(snapshot);
	if (buffer == NULL) {
		return LINKR_DEBUGGER_CONFIG_CODEC_INVALID_ARGUMENT;
	}
	if (buffer_size < LINKR_DEBUGGER_CONFIG_HEADER_SIZE ||
	    memcmp(buffer, LINKR_DEBUGGER_CONFIG_MAGIC, 4U) != 0) {
		return LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED;
	}
	if (buffer[4] != LINKR_DEBUGGER_CONFIG_VERSION) {
		return LINKR_DEBUGGER_CONFIG_CODEC_NOT_APPLICABLE;
	}
	if (buffer[5] != LINKR_DEBUGGER_CONFIG_ENTRY_SIZE ||
	    buffer[6] > LINKR_DEBUGGER_CONFIG_MAX_ENTRIES ||
	    buffer[7] != 0U || buffer[10] != 0U || buffer[11] != 0U) {
		return LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED;
	}
	total_size = (size_t)buffer[8] | ((size_t)buffer[9] << 8U);
	expected_size = LINKR_DEBUGGER_CONFIG_HEADER_SIZE +
		((size_t)buffer[6] * LINKR_DEBUGGER_CONFIG_ENTRY_SIZE);
	if (total_size != expected_size || buffer_size != total_size) {
		return LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED;
	}
	if (buffer[6] == 0U) {
		return LINKR_DEBUGGER_CONFIG_CODEC_EMPTY_SELECTION;
	}
	decoded.entry_count = buffer[6];
	for (size_t i = 0U; i < decoded.entry_count; i++) {
		const size_t offset = LINKR_DEBUGGER_CONFIG_HEADER_SIZE +
			(i * LINKR_DEBUGGER_CONFIG_ENTRY_SIZE);
		struct linkr_debugger_config_entry *entry = &decoded.entries[i];

		entry->domain = buffer[offset];
		entry->item_id = buffer[offset + 1U];
		entry->value = buffer[offset + 2U];
		if (buffer[offset + 3U] != 0U ||
		    validate_entry(entry) != LINKR_DEBUGGER_CONFIG_CODEC_OK ||
		    (i > 0U && !entry_follows(entry, &decoded.entries[i - 1U]))) {
			return LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED;
		}
	}

	*snapshot = decoded;
	return LINKR_DEBUGGER_CONFIG_CODEC_OK;
}
