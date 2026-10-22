/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#ifndef RADXA_LINKR_DEBUGGER_NET_SETTINGS_H_
#define RADXA_LINKR_DEBUGGER_NET_SETTINGS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Deterministic persistent network identity.
 *
 * One identity = a 46-bit locally administered unicast MAC address plus one
 * private IPv4 /30 network (board at network+1, host DHCP base at network+2).
 * The identity is derived once from the 8-byte RP2350 OTP chip ID and
 * persisted as one atomic settings blob. Later boots load the blob and never
 * rederive. Manually set values are authoritative and persistent: they are
 * stored in the same blob and always win over derivation.
 */

#define LINKR_DEBUGGER_NET_SETTINGS_KEY "linkr/net/identity"
#define LINKR_DEBUGGER_NET_SETTINGS_VERSION 1U

#define LINKR_DEBUGGER_NET_MAC_LEN 6U
#define LINKR_DEBUGGER_NET_CHIP_ID_LEN 8U
#define LINKR_DEBUGGER_NET_PREFIX 30U

/* Host offsets inside the persisted /30. */
#define LINKR_DEBUGGER_NET_DEVICE_HOST 1U
#define LINKR_DEBUGGER_NET_DHCP_BASE_HOST 2U

/* "255.255.255.255" + NUL */
#define LINKR_DEBUGGER_NET_IPV4_STR_LEN 16U
/* "aa:bb:cc:dd:ee:ff" + NUL */
#define LINKR_DEBUGGER_NET_MAC_STR_LEN 18U
/* "255.255.255.255/30" + NUL */
#define LINKR_DEBUGGER_NET_NETWORK_STR_LEN 20U
/* "http://255.255.255.255/" + NUL */
#define LINKR_DEBUGGER_NET_URL_STR_LEN 24U

/*
 * Derived /30 pool: 10.0.0.0/8 split into 2^22 /30 networks. The 22-bit index
 * comes from the derivation hash, so the candidate network is
 * 10.0.0.0 + index * 4. Manual identities may use any RFC1918 /30.
 */
#define LINKR_DEBUGGER_NET_DERIVE_POOL_BASE 0x0A000000u
#define LINKR_DEBUGGER_NET_DERIVE_INDEX_BITS 22U

/* Fixed blob layout: version, origin, mac[6], network[4] big-endian. */
#define LINKR_DEBUGGER_NET_IDENTITY_BLOB_LEN 12U

enum linkr_debugger_net_origin {
	LINKR_DEBUGGER_NET_ORIGIN_DERIVED = 0,
	LINKR_DEBUGGER_NET_ORIGIN_MANUAL = 1,
};

struct linkr_debugger_net_identity {
	uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN];
	/* /30 network base address as (a << 24) | (b << 16) | (c << 8) | d. */
	uint32_t network;
	enum linkr_debugger_net_origin origin;
};

/* Validation. */
bool linkr_debugger_net_mac_valid(const uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN]);
bool linkr_debugger_net_network_valid(uint32_t network);
bool linkr_debugger_net_identity_valid(const struct linkr_debugger_net_identity *identity);
bool linkr_debugger_net_chip_id_valid(const uint8_t *chip_id, size_t chip_id_len);

/* Text parsing and formatting. */
int linkr_debugger_net_parse_mac(const char *text,
				 uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN]);
int linkr_debugger_net_format_mac(const uint8_t mac[LINKR_DEBUGGER_NET_MAC_LEN],
				  char *buf, size_t cap);
int linkr_debugger_net_parse_network(const char *text, uint32_t *network);
int linkr_debugger_net_format_network(uint32_t network, char *buf, size_t cap);
int linkr_debugger_net_format_ipv4(uint32_t network, uint8_t host, char *buf,
				   size_t cap);
int linkr_debugger_net_format_url(uint32_t network, char *buf, size_t cap);

/*
 * Deterministic derivation from the chip ID (public, not a security
 * identity). Fills mac/network with origin DERIVED.
 */
int linkr_debugger_net_derive_identity(const uint8_t *chip_id,
				       size_t chip_id_len,
				       struct linkr_debugger_net_identity *identity);

/* Atomic single-blob persistence. */
int linkr_debugger_net_identity_encode(const struct linkr_debugger_net_identity *identity,
				       uint8_t *buf, size_t cap, size_t *len);
int linkr_debugger_net_identity_decode(const uint8_t *buf, size_t len,
				       struct linkr_debugger_net_identity *identity);
int linkr_debugger_net_identity_load(struct linkr_debugger_net_identity *identity);
int linkr_debugger_net_identity_save(const struct linkr_debugger_net_identity *identity);

/*
 * Boot-time policy: load the persisted identity if one is valid and never
 * derive over it. Otherwise derive from the chip ID and persist immediately.
 * Returns 0 with a usable identity, or a negative errno when no persisted
 * identity exists and the chip ID cannot produce one.
 */
int linkr_debugger_net_identity_resolve(const uint8_t *chip_id,
					size_t chip_id_len,
					struct linkr_debugger_net_identity *identity);

#endif /* RADXA_LINKR_DEBUGGER_NET_SETTINGS_H_ */
