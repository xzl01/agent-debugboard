/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "linkr_debugger_dns.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(LINKR_DEBUGGER_DNS_HOST_TEST)
#include "linkr_debugger_network.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>

LOG_MODULE_REGISTER(linkr_debugger_dns, CONFIG_LINKR_DEBUGGER_LOG_LEVEL);

#define LINKR_DEBUGGER_DNS_THREAD_STACK_SIZE 2048U
#define LINKR_DEBUGGER_DNS_THREAD_PRIORITY 7
#endif

#define DNS_HEADER_LEN 12U
#define DNS_QR_MASK 0x8000U
#define DNS_OPCODE_MASK 0x7800U
#define DNS_TC_MASK 0x0200U
#define DNS_RD_MASK 0x0100U
#define DNS_AA_MASK 0x0400U
#define DNS_TYPE_A 1U
#define DNS_TYPE_AAAA 28U
#define DNS_CLASS_IN 1U
#define DNS_RETRY_DELAY_MS 1000U
#define DNS_RECV_ERROR_DELAY_MS 250U

static uint16_t dns_read_u16(const uint8_t *buf)
{
	return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}

static void dns_write_u16(uint8_t *buf, uint16_t value)
{
	buf[0] = (uint8_t)(value >> 8);
	buf[1] = (uint8_t)value;
}

static void dns_write_u32(uint8_t *buf, uint32_t value)
{
	buf[0] = (uint8_t)(value >> 24);
	buf[1] = (uint8_t)(value >> 16);
	buf[2] = (uint8_t)(value >> 8);
	buf[3] = (uint8_t)value;
}

static int dns_parse_name(const uint8_t *packet, size_t packet_len, size_t offset,
				 size_t *wire_end, bool allow_compression)
{
	size_t pos = offset;
	size_t end = offset;
	size_t total_len = 0U;
	size_t jumps = 0U;
	bool jumped = false;

	if (packet == NULL || wire_end == NULL || offset >= packet_len) {
		return -EINVAL;
	}

	while (true) {
		uint8_t len;

		if (pos >= packet_len) {
			return -EINVAL;
		}

		len = packet[pos];
		if ((len & 0xc0U) == 0xc0U) {
			uint16_t ptr;

			if (!allow_compression) {
				return -EINVAL;
			}
			if (pos + 1U >= packet_len) {
				return -EINVAL;
			}
			ptr = (uint16_t)(((uint16_t)(len & 0x3fU) << 8) | packet[pos + 1U]);
			if (ptr >= packet_len || ptr == pos) {
				return -EINVAL;
			}
			if (!jumped) {
				end = pos + 2U;
			}
			jumped = true;
			pos = ptr;
			jumps++;
			if (jumps > packet_len) {
				return -EINVAL;
			}
			continue;
		}

		if ((len & 0xc0U) != 0U || len > 63U) {
			return -EINVAL;
		}

		pos++;
		if (len == 0U) {
			if (total_len + 1U > 255U) {
				return -EINVAL;
			}
			if (!jumped) {
				end = pos;
			}
			break;
		}

		if (pos + len > packet_len) {
			return -EINVAL;
		}
		total_len += (size_t)len + 1U;
		if (total_len > 255U) {
			return -EINVAL;
		}
		pos += len;
		if (!jumped) {
			end = pos;
		}
	}

	*wire_end = end;
	return 0;
}

static int dns_validate_rrs(const uint8_t *packet, size_t packet_len, size_t offset,
				    uint16_t count, size_t *end_out)
{
	size_t pos = offset;

	for (uint16_t i = 0U; i < count; i++) {
		size_t name_end;
		uint16_t rdlen;

		if (dns_parse_name(packet, packet_len, pos, &name_end, true) < 0) {
			return -EINVAL;
		}
		if (name_end + 10U > packet_len) {
			return -EINVAL;
		}
		rdlen = dns_read_u16(&packet[name_end + 8U]);
		if (name_end + 10U + rdlen > packet_len) {
			return -EINVAL;
		}
		pos = name_end + 10U + rdlen;
	}

	*end_out = pos;
	return 0;
}

int linkr_debugger_dns_build_response(const uint8_t *query, size_t query_len,
				      uint8_t *response, size_t response_cap,
				      size_t *response_len)
{
	size_t question_end;
	size_t packet_end;
	uint16_t flags;
	uint16_t qdcount;
	uint16_t ancount;
	uint16_t nscount;
	uint16_t arcount;
	uint16_t qtype;
	uint16_t qclass;
	uint16_t answer_count = 0U;
	size_t question_len;
	size_t needed;

	if (response_len == NULL) {
		return -EINVAL;
	}
	*response_len = 0U;

	if (query == NULL || response == NULL || query_len < DNS_HEADER_LEN) {
		return -EINVAL;
	}

	flags = dns_read_u16(&query[2]);
	qdcount = dns_read_u16(&query[4]);
	ancount = dns_read_u16(&query[6]);
	nscount = dns_read_u16(&query[8]);
	arcount = dns_read_u16(&query[10]);

	if ((flags & DNS_QR_MASK) != 0U || (flags & DNS_OPCODE_MASK) != 0U ||
	    (flags & DNS_TC_MASK) != 0U || qdcount != 1U || ancount != 0U || nscount != 0U) {
		return -EINVAL;
	}

	if (dns_parse_name(query, query_len, DNS_HEADER_LEN, &question_end, false) < 0) {
		return -EINVAL;
	}
	if (question_end + 4U > query_len) {
		return -EINVAL;
	}

	qtype = dns_read_u16(&query[question_end]);
	qclass = dns_read_u16(&query[question_end + 2U]);
	if (qclass != DNS_CLASS_IN) {
		return -EINVAL;
	}

	packet_end = question_end + 4U;
	if (dns_validate_rrs(query, query_len, packet_end, arcount, &packet_end) < 0 ||
	    packet_end != query_len) {
		return -EINVAL;
	}

	if (qtype == DNS_TYPE_A) {
		answer_count = 1U;
	} else if (qtype == DNS_TYPE_AAAA) {
		answer_count = 0U;
	} else {
		answer_count = 0U;
	}

	question_len = question_end + 4U - DNS_HEADER_LEN;
	needed = DNS_HEADER_LEN + question_len + (answer_count != 0U ? 16U : 0U);
	if (needed > response_cap) {
		return -ENOSPC;
	}

	memcpy(response, query, DNS_HEADER_LEN);
	dns_write_u16(&response[2], (uint16_t)(DNS_QR_MASK | DNS_AA_MASK | (flags & DNS_RD_MASK)));
	dns_write_u16(&response[4], 1U);
	dns_write_u16(&response[6], answer_count);
	dns_write_u16(&response[8], 0U);
	dns_write_u16(&response[10], 0U);
	memcpy(&response[DNS_HEADER_LEN], &query[DNS_HEADER_LEN], question_len);

	if (answer_count != 0U) {
		size_t pos = DNS_HEADER_LEN + question_len;

		response[pos++] = 0xc0U;
		response[pos++] = DNS_HEADER_LEN;
		dns_write_u16(&response[pos], DNS_TYPE_A);
		pos += 2U;
		dns_write_u16(&response[pos], DNS_CLASS_IN);
		pos += 2U;
		dns_write_u32(&response[pos], LINKR_DEBUGGER_DNS_TTL_SECONDS);
		pos += 4U;
		dns_write_u16(&response[pos], 4U);
		pos += 2U;
		response[pos++] = 172U;
		response[pos++] = 29U;
		response[pos++] = 203U;
		response[pos++] = 1U;
	}

	*response_len = needed;
	return 0;
}

#if !defined(LINKR_DEBUGGER_DNS_HOST_TEST)
static K_THREAD_STACK_DEFINE(linkr_debugger_dns_stack, LINKR_DEBUGGER_DNS_THREAD_STACK_SIZE);
static struct k_thread linkr_debugger_dns_thread_data;
static bool linkr_debugger_dns_thread_started;
static uint8_t dns_query[LINKR_DEBUGGER_DNS_RESPONSE_MAX_SIZE];
static uint8_t dns_response[LINKR_DEBUGGER_DNS_RESPONSE_MAX_SIZE];

static int dns_bind_to_ncm_device(int fd)
{
	struct net_if *iface;
	struct net_ifreq ifreq;
	int ret;

	ret = linkr_debugger_network_get_ncm_iface(&iface);
	if (ret < 0) {
		return ret;
	}

	memset(&ifreq, 0, sizeof(ifreq));
	ret = net_if_get_name(iface, ifreq.ifr_name, sizeof(ifreq.ifr_name));
	if (ret < 0) {
		return ret;
	}

	ret = zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_BINDTODEVICE, &ifreq,
			       sizeof(ifreq));
	return ret < 0 ? -errno : ret;
}

static int dns_open_socket(void)
{
	struct sockaddr_in bind_addr;
	int fd;
	int ret;

	fd = zsock_socket(NET_AF_INET, NET_SOCK_DGRAM, NET_IPPROTO_UDP);
	if (fd < 0) {
		ret = -errno;
		LOG_ERR("DNS socket create failed: %d", ret);
		return ret;
	}

	ret = dns_bind_to_ncm_device(fd);
	if (ret < 0) {
		LOG_WRN("DNS socket device bind failed: %d", ret);
		(void)zsock_close(fd);
		return ret;
	}

	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.sin_family = NET_AF_INET;
	bind_addr.sin_port = htons(LINKR_DEBUGGER_DNS_PORT);
	ret = zsock_inet_pton(NET_AF_INET, "172.29.203.1", &bind_addr.sin_addr);
	if (ret != 1) {
		LOG_ERR("DNS bind address parse failed");
		(void)zsock_close(fd);
		return -EINVAL;
	}

	ret = zsock_bind(fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr));
	if (ret < 0) {
		ret = -errno;
		LOG_ERR("DNS bind failed: %d", ret);
		(void)zsock_close(fd);
		return ret;
	}

	LOG_INF("DNS captive responder listening on 172.29.203.1:%u", LINKR_DEBUGGER_DNS_PORT);
	return fd;
}

static int dns_serve_socket(int fd)
{
	while (true) {
		struct sockaddr_in peer;
		socklen_t peer_len = sizeof(peer);
		ssize_t got;
		size_t response_len;
		int ret;

		got = zsock_recvfrom(fd, dns_query, sizeof(dns_query), 0,
				     (struct sockaddr *)&peer, &peer_len);
		if (got < 0) {
			int ret = -errno;

			LOG_WRN("DNS recvfrom failed: %d", ret);
			k_sleep(K_MSEC(DNS_RECV_ERROR_DELAY_MS));
			return ret;
		}

		ret = linkr_debugger_dns_build_response(dns_query, (size_t)got, dns_response,
						       sizeof(dns_response), &response_len);
		if (ret < 0) {
			continue;
		}

		got = zsock_sendto(fd, dns_response, response_len, 0,
				    (const struct sockaddr *)&peer, peer_len);
		if (got < 0) {
			LOG_WRN("DNS sendto failed: %d", -errno);
		}
	}

	return 0;
}

static void linkr_debugger_dns_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		int fd;

		fd = dns_open_socket();
		if (fd >= 0) {
			(void)dns_serve_socket(fd);
			(void)zsock_close(fd);
		}

		k_sleep(K_MSEC(DNS_RETRY_DELAY_MS));
	}
}

int linkr_debugger_dns_start(void)
{
	if (linkr_debugger_dns_thread_started) {
		return 0;
	}

	if (!linkr_debugger_network_has_preferred_ipv4()) {
		return -EAGAIN;
	}

	(void)k_thread_create(&linkr_debugger_dns_thread_data, linkr_debugger_dns_stack,
			       K_THREAD_STACK_SIZEOF(linkr_debugger_dns_stack),
			       linkr_debugger_dns_thread, NULL, NULL, NULL,
			       LINKR_DEBUGGER_DNS_THREAD_PRIORITY, 0, K_NO_WAIT);
	if (IS_ENABLED(CONFIG_THREAD_NAME)) {
		k_thread_name_set(&linkr_debugger_dns_thread_data, "linkr_dns");
	}
	linkr_debugger_dns_thread_started = true;
	return 0;
}
#endif
