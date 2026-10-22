/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_net_settings.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * Fake settings backend. The blob is stored atomically as one value, so the
 * fake models one replaceable slot.
 */
static uint8_t fake_blob[LINKR_DEBUGGER_NET_IDENTITY_BLOB_LEN];
static size_t fake_blob_len;
static bool fake_blob_present;
static int fake_load_result;
static int fake_save_result;
static unsigned int fake_load_calls;
static unsigned int fake_save_calls;

static void fake_storage_reset(void)
{
	memset(fake_blob, 0, sizeof(fake_blob));
	fake_blob_len = 0U;
	fake_blob_present = false;
	fake_load_result = 0;
	fake_save_result = 0;
	fake_load_calls = 0U;
	fake_save_calls = 0U;
}

int linkr_debugger_net_settings_storage_load(uint8_t *buf, size_t cap, size_t *len)
{
	fake_load_calls++;

	if (fake_load_result < 0) {
		return fake_load_result;
	}

	if (!fake_blob_present) {
		return -ENOENT;
	}

	if (cap < fake_blob_len) {
		return -EINVAL;
	}

	memcpy(buf, fake_blob, fake_blob_len);
	*len = fake_blob_len;
	return 0;
}

int linkr_debugger_net_settings_storage_save(const uint8_t *buf, size_t len)
{
	fake_save_calls++;

	if (fake_save_result < 0) {
		return fake_save_result;
	}

	if (len > sizeof(fake_blob)) {
		return -EINVAL;
	}

	memcpy(fake_blob, buf, len);
	fake_blob_len = len;
	fake_blob_present = true;
	return 0;
}

static void test_mac_validity(void)
{
	const uint8_t good[LINKR_DEBUGGER_NET_MAC_LEN] = { 0x4E, 0x9B, 0xBE, 0x2D, 0x0F, 0xAB };
	const uint8_t all_zero[LINKR_DEBUGGER_NET_MAC_LEN] = { 0 };
	const uint8_t all_ones[LINKR_DEBUGGER_NET_MAC_LEN] = {
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
	};
	const uint8_t multicast[LINKR_DEBUGGER_NET_MAC_LEN] = {
		0x01, 0x22, 0x33, 0x44, 0x55, 0x66
	};
	const uint8_t universal[LINKR_DEBUGGER_NET_MAC_LEN] = {
		0x4C, 0x22, 0x33, 0x44, 0x55, 0x66
	};
	const uint8_t local_unicast[LINKR_DEBUGGER_NET_MAC_LEN] = {
		0x02, 0x00, 0x00, 0x00, 0x00, 0x01
	};

	assert(linkr_debugger_net_mac_valid(good));
	assert(linkr_debugger_net_mac_valid(local_unicast));
	assert(!linkr_debugger_net_mac_valid(all_zero));
	assert(!linkr_debugger_net_mac_valid(all_ones));
	assert(!linkr_debugger_net_mac_valid(multicast));
	assert(!linkr_debugger_net_mac_valid(universal));
	assert(!linkr_debugger_net_mac_valid(NULL));
}

static void test_network_validity(void)
{
	assert(linkr_debugger_net_network_valid(0x0A000000u)); /* 10.0.0.0 */
	assert(linkr_debugger_net_network_valid(0x0AFFFFFCu)); /* 10.255.255.252 */
	assert(linkr_debugger_net_network_valid(0xAC100000u)); /* 172.16.0.0 */
	assert(linkr_debugger_net_network_valid(0xAC1FFFFCu)); /* 172.31.255.252 */
	assert(linkr_debugger_net_network_valid(0xC0A80000u)); /* 192.168.0.0 */
	assert(linkr_debugger_net_network_valid(0xC0A8FFFCu)); /* 192.168.255.252 */

	assert(!linkr_debugger_net_network_valid(0x0A000001u)); /* misaligned */
	assert(!linkr_debugger_net_network_valid(0x0A000002u)); /* misaligned */
	assert(!linkr_debugger_net_network_valid(0x08080800u)); /* public */
	assert(!linkr_debugger_net_network_valid(0xAC200000u)); /* 172.32.0.0 */
	assert(!linkr_debugger_net_network_valid(0xC0A90000u)); /* 192.169.0.0 */
	assert(!linkr_debugger_net_network_valid(0x00000000u)); /* 0.0.0.0 */
}

static void test_chip_id_validity(void)
{
	const uint8_t id[LINKR_DEBUGGER_NET_CHIP_ID_LEN] = {
		0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF
	};
	const uint8_t zeros[LINKR_DEBUGGER_NET_CHIP_ID_LEN] = { 0 };
	const uint8_t ones[LINKR_DEBUGGER_NET_CHIP_ID_LEN] = {
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
	};
	const uint8_t short_id[4] = { 0x01, 0x23, 0x45, 0x67 };

	assert(linkr_debugger_net_chip_id_valid(id, sizeof(id)));
	assert(linkr_debugger_net_chip_id_valid(id, sizeof(id) + 4U));
	assert(!linkr_debugger_net_chip_id_valid(zeros, sizeof(zeros)));
	assert(!linkr_debugger_net_chip_id_valid(ones, sizeof(ones)));
	assert(!linkr_debugger_net_chip_id_valid(short_id, sizeof(short_id)));
	assert(!linkr_debugger_net_chip_id_valid(NULL, sizeof(id)));
}

static void test_parse_and_format_mac(void)
{
	uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN];
	char buf[LINKR_DEBUGGER_NET_MAC_STR_LEN];

	assert(linkr_debugger_net_parse_mac("4e:9b:be:2d:0f:ab", mac) == 0);
	assert(mac[0] == 0x4EU && mac[5] == 0xABU);
	assert(linkr_debugger_net_format_mac(mac, buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "4e:9b:be:2d:0f:ab") == 0);

	/* Uppercase hex is accepted and formatted lowercase. */
	assert(linkr_debugger_net_parse_mac("4E:9B:BE:2D:0F:AB", mac) == 0);
	assert(linkr_debugger_net_format_mac(mac, buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "4e:9b:be:2d:0f:ab") == 0);

	assert(linkr_debugger_net_parse_mac("02:00:00:00:00:01", mac) == 0);

	/* Invalid syntax. */
	assert(linkr_debugger_net_parse_mac("", mac) == -EINVAL);
	assert(linkr_debugger_net_parse_mac("4e:9b:be:2d:0f", mac) == -EINVAL);
	assert(linkr_debugger_net_parse_mac("4e:9b:be:2d:0f:ab:12", mac) == -EINVAL);
	assert(linkr_debugger_net_parse_mac("4e-9b-be-2d-0f-ab", mac) == -EINVAL);
	assert(linkr_debugger_net_parse_mac("4e:9b:be:2d:0f:a", mac) == -EINVAL);
	assert(linkr_debugger_net_parse_mac("4e:9b:be:2d:0f:ag", mac) == -EINVAL);
	assert(linkr_debugger_net_parse_mac(" 4e:9b:be:2d:0f:ab", mac) == -EINVAL);
	assert(linkr_debugger_net_parse_mac("4e:9b:be:2d:0f:ab ", mac) == -EINVAL);
	assert(linkr_debugger_net_parse_mac(NULL, mac) == -EINVAL);
	assert(linkr_debugger_net_parse_mac("4e:9b:be:2d:0f:ab", NULL) == -EINVAL);

	/* Valid syntax but not a locally administered unicast address. */
	assert(linkr_debugger_net_parse_mac("00:00:00:00:00:00", mac) == -EINVAL);
	assert(linkr_debugger_net_parse_mac("ff:ff:ff:ff:ff:ff", mac) == -EINVAL);
	assert(linkr_debugger_net_parse_mac("01:22:33:44:55:66", mac) == -EINVAL);
	assert(linkr_debugger_net_parse_mac("4c:22:33:44:55:66", mac) == -EINVAL);

	{
		const uint8_t bad_mac[LINKR_DEBUGGER_NET_MAC_LEN] = { 0 };

		assert(linkr_debugger_net_format_mac(bad_mac, buf, sizeof(buf)) ==
		       -EINVAL);
	}
	assert(linkr_debugger_net_format_mac((const uint8_t[]){ 0x02, 0x00, 0x00, 0x00,
							      0x00, 0x01 },
					    buf, 8U) == -ENOSPC);
	assert(linkr_debugger_net_format_mac((const uint8_t[]){ 0x02, 0x00, 0x00, 0x00,
							      0x00, 0x01 },
					    NULL, sizeof(buf)) == -EINVAL);
}

static void test_parse_and_format_network(void)
{
	uint32_t network;
	char buf[LINKR_DEBUGGER_NET_NETWORK_STR_LEN];

	assert(linkr_debugger_net_parse_network("10.0.0.0", &network) == 0);
	assert(network == 0x0A000000u);
	assert(linkr_debugger_net_parse_network("10.201.110.252", &network) == 0);
	assert(network == 0x0AC96EFCu);
	assert(linkr_debugger_net_parse_network("192.168.7.8/30", &network) == 0);
	assert(network == 0xC0A80708u);
	assert(linkr_debugger_net_parse_network("172.16.0.4", &network) == 0);
	assert(network == 0xAC100004u);

	assert(linkr_debugger_net_parse_network("10.0.0.1", &network) == -EINVAL);
	assert(linkr_debugger_net_parse_network("10.0.0.0/24", &network) == -EINVAL);
	assert(linkr_debugger_net_parse_network("10.0.0.0/31", &network) == -EINVAL);
	assert(linkr_debugger_net_parse_network("10.0.0.0/300", &network) == -EINVAL);
	assert(linkr_debugger_net_parse_network("8.8.8.8", &network) == -EINVAL);
	assert(linkr_debugger_net_parse_network("172.32.0.0", &network) == -EINVAL);
	assert(linkr_debugger_net_parse_network("10.0.0", &network) == -EINVAL);
	assert(linkr_debugger_net_parse_network("256.0.0.0", &network) == -EINVAL);
	assert(linkr_debugger_net_parse_network(" 10.0.0.0", &network) == -EINVAL);
	assert(linkr_debugger_net_parse_network("10.0.0.0 ", &network) == -EINVAL);
	assert(linkr_debugger_net_parse_network("+10.0.0.0", &network) == -EINVAL);
	assert(linkr_debugger_net_parse_network("", &network) == -EINVAL);
	assert(linkr_debugger_net_parse_network(NULL, &network) == -EINVAL);
	assert(linkr_debugger_net_parse_network("10.0.0.0", NULL) == -EINVAL);

	assert(linkr_debugger_net_format_network(0x0A000000u, buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "10.0.0.0/30") == 0);
	assert(linkr_debugger_net_format_network(0xC0A80708u, buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "192.168.7.8/30") == 0);
	assert(linkr_debugger_net_format_network(0x0A000001u, buf, sizeof(buf)) == -EINVAL);
	assert(linkr_debugger_net_format_network(0x0A000000u, buf, 8U) == -ENOSPC);
}

static void test_format_ipv4_and_url(void)
{
	char buf[LINKR_DEBUGGER_NET_URL_STR_LEN];

	assert(linkr_debugger_net_format_ipv4(0x0AC96EFCu, LINKR_DEBUGGER_NET_DEVICE_HOST,
					     buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "10.201.110.253") == 0);
	assert(linkr_debugger_net_format_ipv4(0x0AC96EFCu,
					     LINKR_DEBUGGER_NET_DHCP_BASE_HOST,
					     buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "10.201.110.254") == 0);
	assert(linkr_debugger_net_format_ipv4(0x0AC96EFCu, 0U, buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "10.201.110.252") == 0);
	assert(linkr_debugger_net_format_ipv4(0x0AC96EFCu, 1U, NULL, sizeof(buf)) == -EINVAL);
	assert(linkr_debugger_net_format_ipv4(0x0AC96EFCu, 1U, buf, 8U) == -ENOSPC);
	assert(linkr_debugger_net_format_ipv4(0x0AC96EFDu, 1U, buf, sizeof(buf)) == -EINVAL);

	assert(linkr_debugger_net_format_url(0x0AC96EFCu, buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "http://10.201.110.253/") == 0);
	assert(linkr_debugger_net_format_url(0x0AC96EFCu, buf, 8U) == -ENOSPC);
	assert(linkr_debugger_net_format_url(0x08080800u, buf, sizeof(buf)) == -EINVAL);
}

/*
 * Frozen golden vectors for the deterministic derivation. Changing these
 * values changes every board's first-boot identity and must never happen
 * silently.
 */
static void test_derive_identity_golden(void)
{
	static const struct {
		uint8_t chip_id[LINKR_DEBUGGER_NET_CHIP_ID_LEN];
		uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN];
		uint32_t network;
	} vectors[] = {
		{
			{ 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 },
			{ 0x4E, 0x9B, 0xBE, 0x2D, 0x0F, 0xAB },
			0x0AC96EFCu, /* 10.201.110.252 */
		},
		{
			{ 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF },
			{ 0x2E, 0x08, 0x7E, 0x82, 0x7F, 0x4A },
			0x0A04797Cu, /* 10.4.121.124 */
		},
		{
			{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE },
			{ 0xE6, 0xF1, 0xCF, 0xE8, 0x88, 0x54 },
			0x0A9EE150u, /* 10.158.225.80 */
		},
		{
			{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 },
			{ 0xE6, 0xDB, 0xF3, 0xC8, 0xC3, 0x26 },
			0x0A7BD75Cu, /* 10.123.215.92 */
		},
		{
			{ 0x5A, 0xA5, 0x00, 0xFF, 0x12, 0x34, 0x56, 0x78 },
			{ 0x4E, 0xC4, 0x19, 0x26, 0x3A, 0x8A },
			0x0A7CE2FCu, /* 10.124.226.252 */
		},
	};

	for (size_t i = 0U; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		struct linkr_debugger_net_identity identity;

		assert(linkr_debugger_net_derive_identity(vectors[i].chip_id,
							 sizeof(vectors[i].chip_id),
							 &identity) == 0);
		assert(memcmp(identity.mac, vectors[i].mac,
			      LINKR_DEBUGGER_NET_MAC_LEN) == 0);
		assert(identity.network == vectors[i].network);
		assert(identity.origin == LINKR_DEBUGGER_NET_ORIGIN_DERIVED);
		assert(linkr_debugger_net_identity_valid(&identity));

		/* Deterministic: identical input yields identical output. */
		{
			struct linkr_debugger_net_identity again;

			assert(linkr_debugger_net_derive_identity(
				       vectors[i].chip_id, sizeof(vectors[i].chip_id),
				       &again) == 0);
			assert(memcmp(&identity, &again, sizeof(identity)) == 0);
		}
	}
}

static void test_derive_identity_rejects_bad_input(void)
{
	struct linkr_debugger_net_identity identity;
	const uint8_t id[LINKR_DEBUGGER_NET_CHIP_ID_LEN] = {
		0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF
	};
	const uint8_t zeros[LINKR_DEBUGGER_NET_CHIP_ID_LEN] = { 0 };
	const uint8_t ones[LINKR_DEBUGGER_NET_CHIP_ID_LEN] = {
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
	};

	assert(linkr_debugger_net_derive_identity(id, sizeof(id), NULL) == -EINVAL);
	assert(linkr_debugger_net_derive_identity(NULL, sizeof(id), &identity) == -EINVAL);
	assert(linkr_debugger_net_derive_identity(id, 4U, &identity) == -EINVAL);
	assert(linkr_debugger_net_derive_identity(zeros, sizeof(zeros), &identity) ==
	       -EINVAL);
	assert(linkr_debugger_net_derive_identity(ones, sizeof(ones), &identity) ==
	       -EINVAL);
}

static void test_identity_blob_codec(void)
{
	struct linkr_debugger_net_identity identity = {
		.mac = { 0x4E, 0x9B, 0xBE, 0x2D, 0x0F, 0xAB },
		.network = 0x0AC96EFCu,
		.origin = LINKR_DEBUGGER_NET_ORIGIN_MANUAL,
	};
	struct linkr_debugger_net_identity decoded;
	uint8_t blob[LINKR_DEBUGGER_NET_IDENTITY_BLOB_LEN];
	size_t len = 0U;

	assert(linkr_debugger_net_identity_encode(&identity, blob, sizeof(blob),
						  &len) == 0);
	assert(len == LINKR_DEBUGGER_NET_IDENTITY_BLOB_LEN);
	assert(blob[0] == 1U);
	assert(blob[1] == 1U);
	assert(memcmp(&blob[2], identity.mac, LINKR_DEBUGGER_NET_MAC_LEN) == 0);
	assert(blob[8] == 0x0AU && blob[9] == 0xC9U && blob[10] == 0x6EU &&
	       blob[11] == 0xFCU);

	assert(linkr_debugger_net_identity_decode(blob, len, &decoded) == 0);
	assert(memcmp(&identity, &decoded, sizeof(identity)) == 0);

	/* Strict blob contract: exact length and version. */
	assert(linkr_debugger_net_identity_decode(blob, len - 1U, &decoded) == -EINVAL);
	assert(linkr_debugger_net_identity_decode(blob, len + 1U, &decoded) == -EINVAL);
	assert(linkr_debugger_net_identity_decode(NULL, len, &decoded) == -EINVAL);
	assert(linkr_debugger_net_identity_decode(blob, len, NULL) == -EINVAL);

	blob[0] = 2U; /* unknown version */
	assert(linkr_debugger_net_identity_decode(blob, len, &decoded) == -EINVAL);
	blob[0] = 1U;

	blob[1] = 2U; /* unknown origin */
	assert(linkr_debugger_net_identity_decode(blob, len, &decoded) == -EINVAL);
	blob[1] = 1U;

	blob[2] = 0x00U; /* invalid MAC (universal) */
	assert(linkr_debugger_net_identity_decode(blob, len, &decoded) == -EINVAL);
	blob[2] = 0x4EU;

	blob[11] = 0xFDU; /* misaligned network */
	assert(linkr_debugger_net_identity_decode(blob, len, &decoded) == -EINVAL);
	blob[11] = 0xFCU;

	assert(linkr_debugger_net_identity_decode(blob, len, &decoded) == 0);

	/* Encoding rejects invalid identities. */
	identity.network = 0x0A000001u;
	assert(linkr_debugger_net_identity_encode(&identity, blob, sizeof(blob),
						  &len) == -EINVAL);
	assert(linkr_debugger_net_identity_encode(&identity, blob, 4U, &len) == -EINVAL);
	assert(linkr_debugger_net_identity_encode(&identity, NULL, sizeof(blob),
						  &len) == -EINVAL);
}

static void test_save_and_load(void)
{
	struct linkr_debugger_net_identity identity = {
		.mac = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 },
		.network = 0xC0A80708u,
		.origin = LINKR_DEBUGGER_NET_ORIGIN_MANUAL,
	};
	struct linkr_debugger_net_identity loaded;

	fake_storage_reset();

	assert(linkr_debugger_net_identity_load(&loaded) == -ENOENT);
	assert(fake_load_calls == 1U);

	assert(linkr_debugger_net_identity_save(&identity) == 0);
	assert(fake_save_calls == 1U);
	assert(fake_blob_len == LINKR_DEBUGGER_NET_IDENTITY_BLOB_LEN);

	assert(linkr_debugger_net_identity_load(&loaded) == 0);
	assert(memcmp(&loaded, &identity, sizeof(loaded)) == 0);

	identity.origin = (enum linkr_debugger_net_origin)5;
	assert(linkr_debugger_net_identity_save(&identity) == -EINVAL);
	assert(fake_save_calls == 1U);
}

static void test_resolve_first_init_derives_and_persists(void)
{
	static const uint8_t chip_id[LINKR_DEBUGGER_NET_CHIP_ID_LEN] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
	};
	struct linkr_debugger_net_identity identity;
	struct linkr_debugger_net_identity expected;

	fake_storage_reset();
	assert(linkr_debugger_net_derive_identity(chip_id, sizeof(chip_id),
						 &expected) == 0);

	/* First boot: derive once and persist immediately. */
	assert(linkr_debugger_net_identity_resolve(chip_id, sizeof(chip_id),
						   &identity) == 0);
	assert(fake_load_calls == 1U);
	assert(fake_save_calls == 1U);
	assert(memcmp(&identity, &expected, sizeof(identity)) == 0);
	assert(identity.origin == LINKR_DEBUGGER_NET_ORIGIN_DERIVED);

	/* Later boots: load the persisted identity, never rederive or rewrite. */
	memset(&identity, 0, sizeof(identity));
	assert(linkr_debugger_net_identity_resolve(chip_id, sizeof(chip_id),
						   &identity) == 0);
	assert(fake_load_calls == 2U);
	assert(fake_save_calls == 1U);
	assert(memcmp(&identity, &expected, sizeof(identity)) == 0);
}

static void test_resolve_manual_identity_wins(void)
{
	static const uint8_t chip_id[LINKR_DEBUGGER_NET_CHIP_ID_LEN] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
	};
	struct linkr_debugger_net_identity manual = {
		.mac = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x09 },
		.network = 0xC0A80708u,
		.origin = LINKR_DEBUGGER_NET_ORIGIN_MANUAL,
	};
	struct linkr_debugger_net_identity identity;

	fake_storage_reset();
	assert(linkr_debugger_net_identity_save(&manual) == 0);
	assert(fake_save_calls == 1U);

	/* A persisted manual identity is authoritative and is never overwritten. */
	assert(linkr_debugger_net_identity_resolve(chip_id, sizeof(chip_id),
						   &identity) == 0);
	assert(fake_save_calls == 1U);
	assert(memcmp(&identity, &manual, sizeof(identity)) == 0);
	assert(identity.origin == LINKR_DEBUGGER_NET_ORIGIN_MANUAL);
}

static void test_resolve_requires_chip_id_without_persisted_identity(void)
{
	static const uint8_t chip_id[LINKR_DEBUGGER_NET_CHIP_ID_LEN] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
	};
	const uint8_t zeros[LINKR_DEBUGGER_NET_CHIP_ID_LEN] = { 0 };
	struct linkr_debugger_net_identity identity;

	fake_storage_reset();
	assert(linkr_debugger_net_identity_resolve(NULL, sizeof(chip_id), &identity) ==
	       -EINVAL);
	assert(linkr_debugger_net_identity_resolve(zeros, sizeof(zeros), &identity) ==
	       -EINVAL);
	assert(linkr_debugger_net_identity_resolve(chip_id, 4U, &identity) == -EINVAL);
	assert(fake_save_calls == 0U);

	/* With a persisted identity the chip ID is never consulted for derivation. */
	{
		struct linkr_debugger_net_identity manual = {
			.mac = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x09 },
			.network = 0xC0A80708u,
			.origin = LINKR_DEBUGGER_NET_ORIGIN_MANUAL,
		};

		assert(linkr_debugger_net_identity_save(&manual) == 0);
		assert(linkr_debugger_net_identity_resolve(zeros, sizeof(zeros),
							  &identity) == 0);
		assert(memcmp(&identity, &manual, sizeof(identity)) == 0);
	}
}

static void test_resolve_corrupt_blob_is_not_overwritten(void)
{
	static const uint8_t chip_id[LINKR_DEBUGGER_NET_CHIP_ID_LEN] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
	};
	struct linkr_debugger_net_identity identity;
	struct linkr_debugger_net_identity expected;
	uint8_t original[LINKR_DEBUGGER_NET_IDENTITY_BLOB_LEN];

	fake_storage_reset();

	/* A corrupt blob is preserved; use a volatile derived identity instead. */
	memset(fake_blob, 0xFF, sizeof(fake_blob));
	memcpy(original, fake_blob, sizeof(original));
	fake_blob_len = sizeof(fake_blob);
	fake_blob_present = true;

	assert(linkr_debugger_net_derive_identity(chip_id, sizeof(chip_id),
						 &expected) == 0);
	assert(linkr_debugger_net_identity_resolve(chip_id, sizeof(chip_id),
						   &identity) == 0);
	assert(fake_save_calls == 0U);
	assert(memcmp(fake_blob, original, sizeof(original)) == 0);
	assert(memcmp(&identity, &expected, sizeof(identity)) == 0);
}

static void test_resolve_transient_load_error_is_not_overwritten(void)
{
	static const uint8_t chip_id[LINKR_DEBUGGER_NET_CHIP_ID_LEN] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
	};
	struct linkr_debugger_net_identity identity;

	fake_storage_reset();
	fake_load_result = -EIO;
	assert(linkr_debugger_net_identity_resolve(chip_id, sizeof(chip_id),
						   &identity) == 0);
	assert(fake_save_calls == 0U);
	assert(!fake_blob_present);
	assert(identity.origin == LINKR_DEBUGGER_NET_ORIGIN_DERIVED);
}

static void test_resolve_tolerates_persist_failure(void)
{
	static const uint8_t chip_id[LINKR_DEBUGGER_NET_CHIP_ID_LEN] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
	};
	struct linkr_debugger_net_identity identity;
	struct linkr_debugger_net_identity expected;

	fake_storage_reset();
	fake_save_result = -EIO;

	/*
	 * Derivation is deterministic, so a failed first-boot write is not
	 * fatal: the boot still gets a usable identity and the next boot
	 * rederives the same values.
	 */
	assert(linkr_debugger_net_derive_identity(chip_id, sizeof(chip_id),
						 &expected) == 0);
	assert(linkr_debugger_net_identity_resolve(chip_id, sizeof(chip_id),
						   &identity) == 0);
	assert(fake_save_calls == 1U);
	assert(memcmp(&identity, &expected, sizeof(identity)) == 0);
}

int main(void)
{
	test_mac_validity();
	test_network_validity();
	test_chip_id_validity();
	test_parse_and_format_mac();
	test_parse_and_format_network();
	test_format_ipv4_and_url();
	test_derive_identity_golden();
	test_derive_identity_rejects_bad_input();
	test_identity_blob_codec();
	test_save_and_load();
	test_resolve_first_init_derives_and_persists();
	test_resolve_manual_identity_wins();
	test_resolve_requires_chip_id_without_persisted_identity();
	test_resolve_corrupt_blob_is_not_overwritten();
	test_resolve_transient_load_error_is_not_overwritten();
	test_resolve_tolerates_persist_failure();
	printf("linkr_debugger_net_settings_test: OK\n");
	return 0;
}
