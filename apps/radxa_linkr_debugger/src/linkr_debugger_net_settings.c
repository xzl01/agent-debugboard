/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_net_settings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef LINKR_DEBUGGER_NET_SETTINGS_HOST_TEST
#include <zephyr/settings/settings.h>
#endif

/*
 * Deterministic derivation (public algorithm, not a security identity).
 *
 * hash(tag, chip_id):
 *   h = 0xcbf29ce484222325
 *   for each ASCII byte b of tag:  h = (h XOR b) * 0x100000001b3  (mod 2^64)
 *   for each byte b of chip_id[0..7]:  h = (h XOR b) * 0x100000001b3  (mod 2^64)
 *
 * MAC (tag "linkr.mac.v1"):
 *   mac[i] = (h >> (8 * i)) & 0xff  for i = 0..5  (low 48 bits, little-endian)
 *   mac[0] = (mac[0] & 0xfc) | 0x02
 *   The first octet fixes I/G = 0 (unicast) and U/L = 1 (locally
 *   administered), leaving 46 derived bits.
 *
 * Network (tag "linkr.net.v1"):
 *   index   = h & 0x003fffff                        (22-bit /30 index)
 *   network = 0x0a000000 + (index << 2)             (10.0.0.0/8 /30 pool)
 *
 * Board and host addresses are network+1 and network+2.
 */
#define NET_FNV64_BASIS 0xcbf29ce484222325ULL
#define NET_FNV64_PRIME 0x00000100000001b3ULL
#define NET_DERIVE_TAG_MAC "linkr.mac.v1"
#define NET_DERIVE_TAG_NET "linkr.net.v1"

#define NET_DERIVE_INDEX_MASK ((1u << LINKR_DEBUGGER_NET_DERIVE_INDEX_BITS) - 1u)

static uint64_t net_fnv1a64(const char *tag, const uint8_t *chip_id)
{
	uint64_t hash = NET_FNV64_BASIS;
	const unsigned char *cursor;

	for (cursor = (const unsigned char *)tag; *cursor != '\0'; cursor++) {
		hash ^= (uint64_t)(*cursor);
		hash *= NET_FNV64_PRIME;
	}

	for (size_t i = 0; i < LINKR_DEBUGGER_NET_CHIP_ID_LEN; i++) {
		hash ^= (uint64_t)chip_id[i];
		hash *= NET_FNV64_PRIME;
	}

	return hash;
}

bool linkr_debugger_net_mac_valid(const uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN])
{
	if (mac == NULL) {
		return false;
	}

	/* I/G = 0 (unicast) and U/L = 1 (locally administered). */
	return (mac[0] & 0x03U) == 0x02U;
}

bool linkr_debugger_net_network_valid(uint32_t network)
{
	/* /30 aligned. */
	if ((network & 0x03U) != 0U) {
		return false;
	}

	/* RFC1918 private IPv4 space only. */
	return (network & 0xFF000000U) == 0x0A000000U ||
	       (network & 0xFFF00000U) == 0xAC100000U ||
	       (network & 0xFFFF0000U) == 0xC0A80000U;
}

bool linkr_debugger_net_identity_valid(const struct linkr_debugger_net_identity *identity)
{
	if (identity == NULL) {
		return false;
	}

	if (identity->origin != LINKR_DEBUGGER_NET_ORIGIN_DERIVED &&
	    identity->origin != LINKR_DEBUGGER_NET_ORIGIN_MANUAL) {
		return false;
	}

	return linkr_debugger_net_mac_valid(identity->mac) &&
	       linkr_debugger_net_network_valid(identity->network);
}

bool linkr_debugger_net_chip_id_valid(const uint8_t *chip_id, size_t chip_id_len)
{
	bool all_zero = true;
	bool all_ones = true;

	if (chip_id == NULL || chip_id_len < LINKR_DEBUGGER_NET_CHIP_ID_LEN) {
		return false;
	}

	for (size_t i = 0; i < LINKR_DEBUGGER_NET_CHIP_ID_LEN; i++) {
		all_zero = all_zero && chip_id[i] == 0x00U;
		all_ones = all_ones && chip_id[i] == 0xFFU;
	}

	return !all_zero && !all_ones;
}

static int net_hex_nibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

int linkr_debugger_net_parse_mac(const char *text,
				 uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN])
{
	uint8_t parsed[LINKR_DEBUGGER_NET_MAC_LEN];

	if (text == NULL || mac == NULL ||
	    strlen(text) != (LINKR_DEBUGGER_NET_MAC_LEN * 3U - 1U)) {
		return -EINVAL;
	}

	for (size_t i = 0; i < LINKR_DEBUGGER_NET_MAC_LEN; i++) {
		int high = net_hex_nibble(text[i * 3U]);
		int low;
		char sep;

		if (high < 0) {
			return -EINVAL;
		}

		low = net_hex_nibble(text[i * 3U + 1U]);
		if (low < 0) {
			return -EINVAL;
		}

		/* Both hex digits are present, so the separator is in bounds. */
		sep = text[i * 3U + 2U];
		if (i + 1U < LINKR_DEBUGGER_NET_MAC_LEN) {
			if (sep != ':') {
				return -EINVAL;
			}
		} else if (sep != '\0') {
			return -EINVAL;
		}

		parsed[i] = (uint8_t)((high << 4) | low);
	}

	if (!linkr_debugger_net_mac_valid(parsed)) {
		return -EINVAL;
	}

	memcpy(mac, parsed, sizeof(parsed));
	return 0;
}

int linkr_debugger_net_format_mac(const uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN],
				  char *buf, size_t cap)
{
	int written;

	if (buf == NULL || cap == 0U || !linkr_debugger_net_mac_valid(mac)) {
		return -EINVAL;
	}

	written = snprintf(buf, cap, "%02x:%02x:%02x:%02x:%02x:%02x",
			   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	if (written < 0) {
		return -EINVAL;
	}
	if ((size_t)written >= cap) {
		return -ENOSPC;
	}

	return 0;
}

/* Strict unsigned decimal octet: digits only, value 0..255. */
static int net_parse_octet(const char **cursor, uint32_t *octet)
{
	const char *p = *cursor;
	uint32_t value = 0U;
	size_t digits = 0U;

	while (*p >= '0' && *p <= '9') {
		value = value * 10U + (uint32_t)(*p - '0');
		if (value > 255U) {
			return -EINVAL;
		}
		p++;
		digits++;
		if (digits > 3U) {
			return -EINVAL;
		}
	}

	if (digits == 0U) {
		return -EINVAL;
	}

	*cursor = p;
	*octet = value;
	return 0;
}

int linkr_debugger_net_parse_network(const char *text, uint32_t *network)
{
	const char *cursor;
	uint32_t octet;
	uint32_t value = 0U;

	if (text == NULL || network == NULL || text[0] == '\0') {
		return -EINVAL;
	}

	cursor = text;
	for (size_t i = 0; i < 4U; i++) {
		if (i > 0U && *cursor != '.') {
			return -EINVAL;
		}
		if (i > 0U) {
			cursor++;
		}
		if (net_parse_octet(&cursor, &octet) < 0) {
			return -EINVAL;
		}
		value = (value << 8) | octet;
	}

	/* An optional "/30" suffix is accepted; any other prefix is not. */
	if (cursor[0] == '/') {
		if (cursor[1] != '3' || cursor[2] != '0' || cursor[3] != '\0') {
			return -EINVAL;
		}
	} else if (cursor[0] != '\0') {
		return -EINVAL;
	}

	if (!linkr_debugger_net_network_valid(value)) {
		return -EINVAL;
	}

	*network = value;
	return 0;
}

int linkr_debugger_net_format_ipv4(uint32_t network, uint8_t host, char *buf,
				   size_t cap)
{
	uint32_t addr;
	int written;

	if (buf == NULL || cap == 0U || !linkr_debugger_net_network_valid(network)) {
		return -EINVAL;
	}

	addr = network + (uint32_t)host;
	written = snprintf(buf, cap, "%u.%u.%u.%u", (unsigned int)((addr >> 24) & 0xFFU),
			   (unsigned int)((addr >> 16) & 0xFFU),
			   (unsigned int)((addr >> 8) & 0xFFU),
			   (unsigned int)(addr & 0xFFU));
	if (written < 0) {
		return -EINVAL;
	}
	if ((size_t)written >= cap) {
		return -ENOSPC;
	}

	return 0;
}

int linkr_debugger_net_format_network(uint32_t network, char *buf, size_t cap)
{
	char ip[LINKR_DEBUGGER_NET_IPV4_STR_LEN];
	int written;
	int ret;

	ret = linkr_debugger_net_format_ipv4(network, 0U, ip, sizeof(ip));
	if (ret < 0) {
		return ret;
	}

	written = snprintf(buf, cap, "%s/30", ip);
	if (written < 0) {
		return -EINVAL;
	}
	if ((size_t)written >= cap) {
		return -ENOSPC;
	}

	return 0;
}

int linkr_debugger_net_format_url(uint32_t network, char *buf, size_t cap)
{
	char ip[LINKR_DEBUGGER_NET_IPV4_STR_LEN];
	int written;
	int ret;

	ret = linkr_debugger_net_format_ipv4(network, LINKR_DEBUGGER_NET_DEVICE_HOST,
					     ip, sizeof(ip));
	if (ret < 0) {
		return ret;
	}

	written = snprintf(buf, cap, "http://%s/", ip);
	if (written < 0) {
		return -EINVAL;
	}
	if ((size_t)written >= cap) {
		return -ENOSPC;
	}

	return 0;
}

int linkr_debugger_net_derive_identity(const uint8_t *chip_id,
				       size_t chip_id_len,
				       struct linkr_debugger_net_identity *identity)
{
	struct linkr_debugger_net_identity derived;
	uint64_t mac_hash;
	uint64_t net_hash;

	if (identity == NULL || !linkr_debugger_net_chip_id_valid(chip_id, chip_id_len)) {
		return -EINVAL;
	}

	mac_hash = net_fnv1a64(NET_DERIVE_TAG_MAC, chip_id);
	net_hash = net_fnv1a64(NET_DERIVE_TAG_NET, chip_id);

	for (size_t i = 0; i < LINKR_DEBUGGER_NET_MAC_LEN; i++) {
		derived.mac[i] = (uint8_t)((mac_hash >> (8U * i)) & 0xFFU);
	}
	derived.mac[0] = (uint8_t)((derived.mac[0] & 0xFCU) | 0x02U);

	derived.network = LINKR_DEBUGGER_NET_DERIVE_POOL_BASE +
			  ((uint32_t)(net_hash & NET_DERIVE_INDEX_MASK)
			   << 2U);
	derived.origin = LINKR_DEBUGGER_NET_ORIGIN_DERIVED;

	if (!linkr_debugger_net_identity_valid(&derived)) {
		return -EINVAL;
	}

	*identity = derived;
	return 0;
}

int linkr_debugger_net_identity_encode(const struct linkr_debugger_net_identity *identity,
				       uint8_t *buf, size_t cap, size_t *len)
{
	if (buf == NULL || len == NULL ||
	    cap < LINKR_DEBUGGER_NET_IDENTITY_BLOB_LEN ||
	    !linkr_debugger_net_identity_valid(identity)) {
		return -EINVAL;
	}

	buf[0] = (uint8_t)LINKR_DEBUGGER_NET_SETTINGS_VERSION;
	buf[1] = (uint8_t)identity->origin;
	memcpy(&buf[2], identity->mac, LINKR_DEBUGGER_NET_MAC_LEN);
	buf[8] = (uint8_t)((identity->network >> 24) & 0xFFU);
	buf[9] = (uint8_t)((identity->network >> 16) & 0xFFU);
	buf[10] = (uint8_t)((identity->network >> 8) & 0xFFU);
	buf[11] = (uint8_t)(identity->network & 0xFFU);

	*len = LINKR_DEBUGGER_NET_IDENTITY_BLOB_LEN;
	return 0;
}

int linkr_debugger_net_identity_decode(const uint8_t *buf, size_t len,
				       struct linkr_debugger_net_identity *identity)
{
	struct linkr_debugger_net_identity decoded;

	if (buf == NULL || identity == NULL ||
	    len != LINKR_DEBUGGER_NET_IDENTITY_BLOB_LEN) {
		return -EINVAL;
	}

	if (buf[0] != (uint8_t)LINKR_DEBUGGER_NET_SETTINGS_VERSION) {
		return -EINVAL;
	}

	decoded.origin = (enum linkr_debugger_net_origin)buf[1];
	memcpy(decoded.mac, &buf[2], LINKR_DEBUGGER_NET_MAC_LEN);
	decoded.network = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
			  ((uint32_t)buf[10] << 8) | (uint32_t)buf[11];

	if (!linkr_debugger_net_identity_valid(&decoded)) {
		return -EINVAL;
	}

	*identity = decoded;
	return 0;
}

#ifdef LINKR_DEBUGGER_NET_SETTINGS_HOST_TEST
/*
 * Host-model tests provide the storage backend so the load/save/resolve
 * policy runs against a controllable fake.
 */
int linkr_debugger_net_settings_storage_load(uint8_t *buf, size_t cap, size_t *len);
int linkr_debugger_net_settings_storage_save(const uint8_t *buf, size_t len);
#else
static int linkr_debugger_net_settings_storage_load(uint8_t *buf, size_t cap, size_t *len)
{
	ssize_t read;

	if (cap < LINKR_DEBUGGER_NET_IDENTITY_BLOB_LEN) {
		return -EINVAL;
	}

	read = settings_load_one(LINKR_DEBUGGER_NET_SETTINGS_KEY, buf,
				 LINKR_DEBUGGER_NET_IDENTITY_BLOB_LEN);
	if (read < 0) {
		return (int)read;
	}

	*len = (size_t)read;
	return 0;
}

static int linkr_debugger_net_settings_storage_save(const uint8_t *buf, size_t len)
{
	return (int)settings_save_one(LINKR_DEBUGGER_NET_SETTINGS_KEY, buf, len);
}
#endif

int linkr_debugger_net_identity_load(struct linkr_debugger_net_identity *identity)
{
	uint8_t blob[LINKR_DEBUGGER_NET_IDENTITY_BLOB_LEN];
	size_t len = 0U;
	int ret;

	if (identity == NULL) {
		return -EINVAL;
	}

	ret = linkr_debugger_net_settings_storage_load(blob, sizeof(blob), &len);
	if (ret < 0) {
		return ret;
	}

	return linkr_debugger_net_identity_decode(blob, len, identity);
}

int linkr_debugger_net_identity_save(const struct linkr_debugger_net_identity *identity)
{
	uint8_t blob[LINKR_DEBUGGER_NET_IDENTITY_BLOB_LEN];
	size_t len = 0U;
	int ret;

	ret = linkr_debugger_net_identity_encode(identity, blob, sizeof(blob), &len);
	if (ret < 0) {
		return ret;
	}

	return linkr_debugger_net_settings_storage_save(blob, len);
}

int linkr_debugger_net_identity_resolve(const uint8_t *chip_id,
					size_t chip_id_len,
					struct linkr_debugger_net_identity *identity)
{
	int ret;

	if (identity == NULL) {
		return -EINVAL;
	}

	ret = linkr_debugger_net_identity_load(identity);
	if (ret == 0) {
		/* Persisted identity is authoritative; never rederive over it. */
		return 0;
	}

	{
		int load_ret = ret;

		ret = linkr_debugger_net_derive_identity(chip_id, chip_id_len,
							 identity);
		if (ret < 0) {
			return ret;
		}

		/*
		 * Only a truly absent key is safe to replace. A transient read or
		 * decode failure must not overwrite a persisted manual identity; use
		 * the deterministic derivation in RAM and retry the read next boot.
		 */
		if (load_ret != -ENOENT) {
			/* Keep the stored blob; the next boot retries the read. */
			return 0;
		}

		/*
		 * Persist the first-boot derivation immediately. A failed write is
		 * not fatal here: derivation is deterministic, so a later boot
		 * rederives the same identity until the write succeeds. Manual
		 * values take a different path and must fail loudly when they
		 * cannot be persisted.
		 */
		(void)linkr_debugger_net_identity_save(identity);
	}
	return 0;
}
