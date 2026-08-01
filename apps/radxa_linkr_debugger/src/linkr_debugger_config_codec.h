/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#ifndef RADXA_LINKR_DEBUGGER_CONFIG_CODEC_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_CODEC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINKR_DEBUGGER_CONFIG_MAGIC "LRCF"
#define LINKR_DEBUGGER_CONFIG_VERSION 1U
#define LINKR_DEBUGGER_CONFIG_HEADER_SIZE 12U
#define LINKR_DEBUGGER_CONFIG_ENTRY_SIZE 4U
#define LINKR_DEBUGGER_CONFIG_MAX_ENTRIES 23U
#define LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE 104U
#define LINKR_DEBUGGER_CONFIG_SETTINGS_MAX_VAL_LEN 256U

_Static_assert(LINKR_DEBUGGER_CONFIG_MAX_ENTRIES == 23U,
	       "persistent catalog entry count changed");
_Static_assert(LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE ==
	       LINKR_DEBUGGER_CONFIG_HEADER_SIZE +
	       (LINKR_DEBUGGER_CONFIG_ENTRY_SIZE * LINKR_DEBUGGER_CONFIG_MAX_ENTRIES),
	       "persistent snapshot size changed");
_Static_assert(LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE <=
	       LINKR_DEBUGGER_CONFIG_SETTINGS_MAX_VAL_LEN,
	       "persistent snapshot exceeds Zephyr Settings value limit");

#define LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT (1U << 0)
#define LINKR_DEBUGGER_CONFIG_GPIO_LEVEL (1U << 1)
#define LINKR_DEBUGGER_CONFIG_GPIO_VALUE_MASK \
	(LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT | LINKR_DEBUGGER_CONFIG_GPIO_LEVEL)

enum linkr_debugger_config_domain {
	LINKR_DEBUGGER_CONFIG_DOMAIN_POWER = 1,
	LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH = 2,
	LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO = 3,
};

enum linkr_debugger_config_power_item_id {
	LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID = 1,
	LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID = 2,
	LINKR_DEBUGGER_CONFIG_POWER_VDD_5V_ID = 3,
	LINKR_DEBUGGER_CONFIG_POWER_20V_OUT_ID = 4,
};

enum linkr_debugger_config_switch_item_id {
	LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID = 1,
	LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID = 2,
	LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID = 3,
	LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID = 4,
};

enum linkr_debugger_config_power_value {
	LINKR_DEBUGGER_CONFIG_POWER_OFF = 0,
	LINKR_DEBUGGER_CONFIG_POWER_ON = 1,
};

enum linkr_debugger_config_sd_value {
	LINKR_DEBUGGER_CONFIG_SD_TARGET = 0,
	LINKR_DEBUGGER_CONFIG_SD_USB_READER = 1,
};

enum linkr_debugger_config_usb_value {
	LINKR_DEBUGGER_CONFIG_USB_PC = 0,
	LINKR_DEBUGGER_CONFIG_USB_TARGET = 1,
};

enum linkr_debugger_config_tf_wp_value {
	LINKR_DEBUGGER_CONFIG_TF_WP_WRITABLE = 0,
	LINKR_DEBUGGER_CONFIG_TF_WP_PROTECTED = 1,
};

enum linkr_debugger_config_vin_value {
	LINKR_DEBUGGER_CONFIG_VIN_1V8 = 0,
	LINKR_DEBUGGER_CONFIG_VIN_3V3 = 1,
};

enum linkr_debugger_config_codec_result {
	LINKR_DEBUGGER_CONFIG_CODEC_OK = 0,
	LINKR_DEBUGGER_CONFIG_CODEC_NOT_APPLICABLE,
	LINKR_DEBUGGER_CONFIG_CODEC_INVALID_ARGUMENT,
	LINKR_DEBUGGER_CONFIG_CODEC_EMPTY_SELECTION,
	LINKR_DEBUGGER_CONFIG_CODEC_NO_SPACE,
	LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED,
};

struct linkr_debugger_config_entry {
	uint8_t domain;
	uint8_t item_id;
	uint8_t value;
};

struct linkr_debugger_config_snapshot {
	size_t entry_count;
	struct linkr_debugger_config_entry entries[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES];
};

struct linkr_debugger_config_item_desc {
	uint8_t domain;
	uint8_t item_id;
	const char *id;
};

extern const struct linkr_debugger_config_item_desc linkr_debugger_config_items[];
extern const size_t linkr_debugger_config_item_count;

void linkr_debugger_config_snapshot_clear(struct linkr_debugger_config_snapshot *snapshot);
const struct linkr_debugger_config_item_desc *linkr_debugger_config_find_item(
	uint8_t domain, uint8_t item_id);
const struct linkr_debugger_config_item_desc *linkr_debugger_config_find_item_by_name(
	const char *id);
enum linkr_debugger_config_codec_result linkr_debugger_config_classify_entry(
	const struct linkr_debugger_config_entry *entry, bool *requires_confirmation);
enum linkr_debugger_config_codec_result linkr_debugger_config_encode(
	const struct linkr_debugger_config_snapshot *snapshot, uint8_t *buffer,
	size_t buffer_size, size_t *encoded_size);
enum linkr_debugger_config_codec_result linkr_debugger_config_decode(
	const uint8_t *buffer, size_t buffer_size,
	struct linkr_debugger_config_snapshot *snapshot);

#endif
