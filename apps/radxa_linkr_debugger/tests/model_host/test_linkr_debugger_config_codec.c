/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/linkr_debugger_config_codec.h"

#define ARRAY_SIZE_LOCAL(array) (sizeof(array) / sizeof((array)[0]))

static const uint8_t known_vector[] = {
	0x4c, 0x52, 0x43, 0x46, 0x01, 0x04, 0x07, 0x00,
	0x28, 0x00, 0x00, 0x00,
	0x01, 0x01, 0x01, 0x00,
	0x01, 0x02, 0x00, 0x00,
	0x02, 0x01, 0x01, 0x00,
	0x02, 0x02, 0x01, 0x00,
	0x02, 0x04, 0x01, 0x00,
	0x03, 0x07, 0x02, 0x00,
	0x03, 0x0a, 0x01, 0x00,
};

static const uint8_t legacy_gp29_output_vector[] = {
	0x4c, 0x52, 0x43, 0x46, 0x01, 0x04, 0x01, 0x00,
	0x10, 0x00, 0x00, 0x00,
	0x03, 0x1d, 0x03, 0x00,
};

static const struct linkr_debugger_config_snapshot known_snapshot = {
	.entry_count = 7U,
	.entries = {
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		  LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
		  LINKR_DEBUGGER_CONFIG_POWER_ON },
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		  LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID,
		  LINKR_DEBUGGER_CONFIG_POWER_OFF },
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		  LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID,
		  LINKR_DEBUGGER_CONFIG_SD_USB_READER },
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		  LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID,
		  LINKR_DEBUGGER_CONFIG_USB_TARGET },
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		  LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID,
		  LINKR_DEBUGGER_CONFIG_VIN_3V3 },
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 7U,
		  LINKR_DEBUGGER_CONFIG_GPIO_LEVEL },
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 10U,
		  LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT },
	},
};

static void assert_snapshot_empty(const struct linkr_debugger_config_snapshot *snapshot)
{
	const struct linkr_debugger_config_snapshot empty = {0};

	assert(memcmp(snapshot, &empty, sizeof(empty)) == 0);
}

static void assert_snapshots_equal(const struct linkr_debugger_config_snapshot *actual,
				   const struct linkr_debugger_config_snapshot *expected)
{
	assert(actual->entry_count == expected->entry_count);
	for (size_t i = 0; i < expected->entry_count; i++) {
		assert(actual->entries[i].domain == expected->entries[i].domain);
		assert(actual->entries[i].item_id == expected->entries[i].item_id);
		assert(actual->entries[i].value == expected->entries[i].value);
	}
}

static void assert_decode_result(const uint8_t *data, size_t length,
				 enum linkr_debugger_config_codec_result expected)
{
	struct linkr_debugger_config_snapshot decoded;

	memset(&decoded, 0xa5, sizeof(decoded));
	assert(linkr_debugger_config_decode(data, length, &decoded) == expected);
	assert_snapshot_empty(&decoded);
}

static void append_entry(struct linkr_debugger_config_snapshot *snapshot,
			 uint8_t domain, uint8_t item_id, uint8_t value)
{
	struct linkr_debugger_config_entry *entry;

	assert(snapshot->entry_count < LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	entry = &snapshot->entries[snapshot->entry_count++];
	entry->domain = domain;
	entry->item_id = item_id;
	entry->value = value;
}

static void build_max_snapshot(struct linkr_debugger_config_snapshot *snapshot)
{
	linkr_debugger_config_snapshot_clear(snapshot);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_OFF);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_OFF);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_VDD_5V_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_OFF);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		     LINKR_DEBUGGER_CONFIG_POWER_20V_OUT_ID,
		     LINKR_DEBUGGER_CONFIG_POWER_OFF);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID,
		     LINKR_DEBUGGER_CONFIG_SD_TARGET);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID,
		     LINKR_DEBUGGER_CONFIG_USB_PC);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID,
		     LINKR_DEBUGGER_CONFIG_TF_WP_WRITABLE);
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		     LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID,
		     LINKR_DEBUGGER_CONFIG_VIN_3V3);
	for (uint8_t pin = 7U; pin <= 20U; pin++) {
		append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, pin,
			     (uint8_t)(pin & LINKR_DEBUGGER_CONFIG_GPIO_VALUE_MASK));
	}
	append_entry(snapshot, LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 29U,
		     LINKR_DEBUGGER_CONFIG_GPIO_LEVEL);
	assert(snapshot->entry_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
}

static void test_constants_and_catalog(void)
{
	static const struct {
		uint8_t domain;
		uint8_t item_id;
		const char *id;
	} expected[] = {
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		  LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID, "power/12v_out" },
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		  LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID, "power/5v_out" },
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		  LINKR_DEBUGGER_CONFIG_POWER_VDD_5V_ID, "power/vdd_5v" },
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
		  LINKR_DEBUGGER_CONFIG_POWER_20V_OUT_ID, "power/20v_out" },
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		  LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID, "switch/sd" },
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		  LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID, "switch/usb" },
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		  LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID, "switch/tf_wp" },
		{ LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
		  LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID, "switch/vin" },
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

	assert(LINKR_DEBUGGER_CONFIG_VERSION == 1U);
	assert(LINKR_DEBUGGER_CONFIG_HEADER_SIZE == 12U);
	assert(LINKR_DEBUGGER_CONFIG_ENTRY_SIZE == 4U);
	assert(LINKR_DEBUGGER_CONFIG_MAX_ENTRIES == 23U);
	assert(LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE == 104U);
	assert(LINKR_DEBUGGER_CONFIG_SETTINGS_MAX_VAL_LEN == 256U);
	assert(linkr_debugger_config_item_count == ARRAY_SIZE_LOCAL(expected));

	for (size_t i = 0; i < ARRAY_SIZE_LOCAL(expected); i++) {
		const struct linkr_debugger_config_item_desc *item;

		item = &linkr_debugger_config_items[i];
		assert(item->domain == expected[i].domain);
		assert(item->item_id == expected[i].item_id);
		assert(strcmp(item->id, expected[i].id) == 0);
		assert(linkr_debugger_config_find_item(item->domain, item->item_id) == item);
		assert(linkr_debugger_config_find_item_by_name(item->id) == item);
	}

	assert(linkr_debugger_config_find_item(0U, 0U) == NULL);
	assert(linkr_debugger_config_find_item(LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 21U) == NULL);
	assert(linkr_debugger_config_find_item_by_name(NULL) == NULL);
	assert(linkr_debugger_config_find_item_by_name("gpio/GP21") == NULL);
}

static void assert_classification(uint8_t domain, uint8_t item_id, uint8_t value,
				  bool expected)
{
	const struct linkr_debugger_config_entry entry = { domain, item_id, value };
	bool requires_confirmation = !expected;

	assert(linkr_debugger_config_classify_entry(&entry, &requires_confirmation) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_OK);
	assert(requires_confirmation == expected);
}

static void test_risk_classification(void)
{
	for (uint8_t id = LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID;
	     id <= LINKR_DEBUGGER_CONFIG_POWER_20V_OUT_ID; id++) {
		assert_classification(LINKR_DEBUGGER_CONFIG_DOMAIN_POWER, id,
				      LINKR_DEBUGGER_CONFIG_POWER_OFF, false);
		assert_classification(LINKR_DEBUGGER_CONFIG_DOMAIN_POWER, id,
				      LINKR_DEBUGGER_CONFIG_POWER_ON, true);
	}

	assert_classification(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			      LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID,
			      LINKR_DEBUGGER_CONFIG_SD_TARGET, false);
	assert_classification(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			      LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID,
			      LINKR_DEBUGGER_CONFIG_SD_USB_READER, false);
	assert_classification(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			      LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID,
			      LINKR_DEBUGGER_CONFIG_USB_PC, true);
	assert_classification(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			      LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID,
			      LINKR_DEBUGGER_CONFIG_USB_TARGET, true);
	assert_classification(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			      LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID,
			      LINKR_DEBUGGER_CONFIG_TF_WP_WRITABLE, false);
	assert_classification(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			      LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID,
			      LINKR_DEBUGGER_CONFIG_TF_WP_PROTECTED, false);
	assert_classification(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			      LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID,
			      LINKR_DEBUGGER_CONFIG_VIN_1V8, true);
	assert_classification(LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
			      LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID,
			      LINKR_DEBUGGER_CONFIG_VIN_3V3, false);

	for (size_t i = 0; i < linkr_debugger_config_item_count; i++) {
		if (linkr_debugger_config_items[i].domain != LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO) {
			continue;
		}
		assert_classification(LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO,
				      linkr_debugger_config_items[i].item_id, 0U, false);
		assert_classification(LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO,
				      linkr_debugger_config_items[i].item_id,
				      LINKR_DEBUGGER_CONFIG_GPIO_LEVEL, false);
		assert_classification(LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO,
				      linkr_debugger_config_items[i].item_id,
				      LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT, true);
		assert_classification(LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO,
				      linkr_debugger_config_items[i].item_id,
				      LINKR_DEBUGGER_CONFIG_GPIO_VALUE_MASK, true);
	}
}

static void test_known_vector(void)
{
	struct linkr_debugger_config_snapshot decoded;
	uint8_t encoded[LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE];
	size_t encoded_len = 0U;

	memset(encoded, 0xa5, sizeof(encoded));
	assert(linkr_debugger_config_encode(&known_snapshot, encoded, sizeof(encoded),
					   &encoded_len) == LINKR_DEBUGGER_CONFIG_CODEC_OK);
	assert(encoded_len == sizeof(known_vector));
	assert(memcmp(encoded, known_vector, sizeof(known_vector)) == 0);

	assert(linkr_debugger_config_decode(known_vector, sizeof(known_vector), &decoded) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_OK);
	assert_snapshots_equal(&decoded, &known_snapshot);
}

static void test_maximum_round_trip(void)
{
	struct linkr_debugger_config_snapshot source;
	struct linkr_debugger_config_snapshot decoded;
	uint8_t encoded[LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE];
	size_t encoded_len = 0U;

	build_max_snapshot(&source);
	assert(linkr_debugger_config_encode(&source, encoded, sizeof(encoded), &encoded_len) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_OK);
	assert(encoded_len == LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE);
	assert(encoded[8] == 0x68U);
	assert(encoded[9] == 0x00U);
	for (size_t i = 0; i < source.entry_count; i++) {
		const size_t offset = LINKR_DEBUGGER_CONFIG_HEADER_SIZE +
			(i * LINKR_DEBUGGER_CONFIG_ENTRY_SIZE);

		assert(encoded[offset] == source.entries[i].domain);
		assert(encoded[offset + 1U] == source.entries[i].item_id);
		assert(encoded[offset + 2U] == source.entries[i].value);
		assert(encoded[offset + 3U] == 0U);
	}
	assert(linkr_debugger_config_decode(encoded, encoded_len, &decoded) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_OK);
	assert_snapshots_equal(&decoded, &source);
}

static void test_gp29_output_snapshot_round_trip(void)
{
	const struct linkr_debugger_config_snapshot expected = {
		.entry_count = 1U,
		.entries = {
			{ LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO, 29U,
			  LINKR_DEBUGGER_CONFIG_GPIO_OUTPUT |
			  LINKR_DEBUGGER_CONFIG_GPIO_LEVEL },
		},
	};
	struct linkr_debugger_config_snapshot decoded;
	uint8_t encoded[LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE];
	size_t encoded_len = 0U;

	assert(linkr_debugger_config_decode(legacy_gp29_output_vector,
					   sizeof(legacy_gp29_output_vector), &decoded) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_OK);
	assert_snapshots_equal(&decoded, &expected);
	assert(linkr_debugger_config_encode(&decoded, encoded, sizeof(encoded), &encoded_len) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_OK);
	assert(encoded_len == sizeof(legacy_gp29_output_vector));
	assert(memcmp(encoded, legacy_gp29_output_vector, encoded_len) == 0);
}

static void assert_encode_malformed(struct linkr_debugger_config_snapshot *snapshot)
{
	uint8_t encoded[LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE];
	size_t encoded_len = 99U;

	assert(linkr_debugger_config_encode(snapshot, encoded, sizeof(encoded), &encoded_len) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED);
	assert(encoded_len == 0U);
}

static void test_encode_rejections(void)
{
	struct linkr_debugger_config_snapshot snapshot = {0};
	uint8_t encoded[LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE];
	size_t encoded_len = 99U;

	assert(linkr_debugger_config_encode(&snapshot, encoded, sizeof(encoded), &encoded_len) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_EMPTY_SELECTION);
	assert(encoded_len == 0U);

	snapshot = known_snapshot;
	snapshot.entry_count = LINKR_DEBUGGER_CONFIG_MAX_ENTRIES + 1U;
	assert_encode_malformed(&snapshot);

	snapshot = known_snapshot;
	snapshot.entries[1] = snapshot.entries[0];
	assert_encode_malformed(&snapshot);

	snapshot = known_snapshot;
	snapshot.entries[0].item_id = LINKR_DEBUGGER_CONFIG_POWER_5V_OUT_ID;
	snapshot.entries[1].item_id = LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID;
	assert_encode_malformed(&snapshot);

	snapshot = known_snapshot;
	snapshot.entries[0].domain = 4U;
	assert_encode_malformed(&snapshot);

	snapshot = known_snapshot;
	snapshot.entries[0].item_id = 5U;
	assert_encode_malformed(&snapshot);

	snapshot = known_snapshot;
	snapshot.entries[0].value = 2U;
	assert_encode_malformed(&snapshot);

	encoded_len = 99U;
	assert(linkr_debugger_config_encode(&known_snapshot, encoded,
					   sizeof(known_vector) - 1U, &encoded_len) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_NO_SPACE);
	assert(encoded_len == 0U);

	assert(linkr_debugger_config_encode(NULL, encoded, sizeof(encoded), &encoded_len) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_INVALID_ARGUMENT);
	assert(linkr_debugger_config_encode(&known_snapshot, NULL, sizeof(encoded),
					   &encoded_len) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_INVALID_ARGUMENT);
	assert(linkr_debugger_config_encode(&known_snapshot, encoded, sizeof(encoded), NULL) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_INVALID_ARGUMENT);
}

static void test_header_rejections(void)
{
	uint8_t mutated[sizeof(known_vector)];
	uint8_t empty[] = {
		0x4c, 0x52, 0x43, 0x46, 0x01, 0x04, 0x00, 0x00,
		0x0c, 0x00, 0x00, 0x00,
	};
	uint8_t empty_with_trailing[] = {
		0x4c, 0x52, 0x43, 0x46, 0x01, 0x04, 0x00, 0x00,
		0x0c, 0x00, 0x00, 0x00, 0x00,
	};

	memcpy(mutated, known_vector, sizeof(mutated));
	mutated[0] ^= 0x01U;
	assert_decode_result(mutated, sizeof(mutated), LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED);

	memcpy(mutated, known_vector, sizeof(mutated));
	mutated[4] = 2U;
	assert_decode_result(mutated, sizeof(mutated),
			     LINKR_DEBUGGER_CONFIG_CODEC_NOT_APPLICABLE);

	memcpy(mutated, known_vector, sizeof(mutated));
	mutated[5] = 3U;
	assert_decode_result(mutated, sizeof(mutated), LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED);

	assert_decode_result(empty, sizeof(empty),
			     LINKR_DEBUGGER_CONFIG_CODEC_EMPTY_SELECTION);
	empty[8] = 0x0dU;
	assert_decode_result(empty, sizeof(empty), LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED);
	assert_decode_result(empty_with_trailing, sizeof(empty_with_trailing),
			     LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED);

	memcpy(mutated, known_vector, sizeof(mutated));
	mutated[6] = LINKR_DEBUGGER_CONFIG_MAX_ENTRIES + 1U;
	assert_decode_result(mutated, sizeof(mutated), LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED);

	for (size_t offset = 7U; offset <= 11U; offset++) {
		memcpy(mutated, known_vector, sizeof(mutated));
		mutated[offset] ^= 0x01U;
		assert_decode_result(mutated, sizeof(mutated),
				     LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED);
	}
}

static void test_entry_rejections(void)
{
	uint8_t mutated[sizeof(known_vector)];
	static const struct {
		size_t offset;
		uint8_t value;
	} mutations[] = {
		{ 12U, 0U },
		{ 12U, 4U },
		{ 13U, 5U },
		{ 21U, 5U },
		{ 33U, 6U },
		{ 14U, 2U },
		{ 22U, 2U },
		{ 34U, 4U },
		{ 15U, 1U },
	};

	for (size_t i = 0; i < ARRAY_SIZE_LOCAL(mutations); i++) {
		memcpy(mutated, known_vector, sizeof(mutated));
		mutated[mutations[i].offset] = mutations[i].value;
		assert_decode_result(mutated, sizeof(mutated),
				     LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED);
	}

	memcpy(mutated, known_vector, sizeof(mutated));
	memcpy(&mutated[16], &mutated[12], LINKR_DEBUGGER_CONFIG_ENTRY_SIZE);
	assert_decode_result(mutated, sizeof(mutated), LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED);

	memcpy(mutated, known_vector, sizeof(mutated));
	memcpy(&mutated[12], &known_vector[16], LINKR_DEBUGGER_CONFIG_ENTRY_SIZE);
	memcpy(&mutated[16], &known_vector[12], LINKR_DEBUGGER_CONFIG_ENTRY_SIZE);
	assert_decode_result(mutated, sizeof(mutated), LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED);
}

static void test_switch_value_rejections(void)
{
	static const uint8_t switch_ids[] = {
		LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID,
		LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID,
		LINKR_DEBUGGER_CONFIG_SWITCH_TF_WP_ID,
		LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID,
	};

	for (size_t i = 0; i < ARRAY_SIZE_LOCAL(switch_ids); i++) {
		const struct linkr_debugger_config_entry entry = {
			LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH, switch_ids[i], 2U,
		};
		bool requires_confirmation = true;

		assert(linkr_debugger_config_classify_entry(&entry, &requires_confirmation) ==
		       LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED);
		assert(!requires_confirmation);
	}
}

static void test_all_boundary_lengths_and_trailing_data(void)
{
	struct linkr_debugger_config_snapshot snapshot;
	uint8_t encoded[LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE + 1U];
	size_t encoded_len = 0U;

	build_max_snapshot(&snapshot);
	assert(linkr_debugger_config_encode(&snapshot, encoded,
					   LINKR_DEBUGGER_CONFIG_MAX_ENCODED_SIZE,
					   &encoded_len) == LINKR_DEBUGGER_CONFIG_CODEC_OK);
	for (size_t length = 0U; length < encoded_len; length++) {
		assert_decode_result(encoded, length, LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED);
		assert_decode_result(encoded, length, LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED);
	}

	encoded[encoded_len] = 0U;
	assert_decode_result(encoded, encoded_len + 1U,
			     LINKR_DEBUGGER_CONFIG_CODEC_MALFORMED);
	assert_decode_result(NULL, encoded_len, LINKR_DEBUGGER_CONFIG_CODEC_INVALID_ARGUMENT);
	assert(linkr_debugger_config_decode(encoded, encoded_len, NULL) ==
	       LINKR_DEBUGGER_CONFIG_CODEC_INVALID_ARGUMENT);
}

int main(void)
{
	test_constants_and_catalog();
	test_risk_classification();
	test_known_vector();
	test_maximum_round_trip();
	test_gp29_output_snapshot_round_trip();
	test_encode_rejections();
	test_header_rejections();
	test_entry_rejections();
	test_switch_value_rejections();
	test_all_boundary_lengths_and_trailing_data();

	puts("linkr_debugger_config_codec: all tests passed");
	return 0;
}
