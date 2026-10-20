/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

#include "test_linkr_debugger_http_net_stubs.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the production fallback router plus the /api/v1/net and /api/v1/pd
 * callbacks directly. */
#include "../../src/linkr_debugger_http.c"

static unsigned int setter_calls;
static uint8_t setter_subnet;
static uint8_t current_subnet = 203U;
static unsigned int pd_fixed_calls;
static unsigned int pd_pps_calls;
static unsigned int pd_status_calls;
static uint16_t pd_fixed_mv;
static uint16_t pd_pps_mv;
static int pd_status_result;
static struct linkr_debugger_pd_status pd_status;

const char *linkr_debugger_json_schema(void)
{
	return "radxa-linkr-debugger.v1";
}

int linkr_debugger_json_append_string(void *context,
				      int (*write)(void *, const char *, size_t),
				      const char *value, size_t len)
{
	if (write(context, "\"", 1U) < 0 || write(context, value, len) < 0) {
		return -ENOMEM;
	}
	return write(context, "\"", 1U);
}

int64_t json_obj_parse(char *payload, size_t len,
		       const struct json_obj_descr *descr, size_t descr_len,
		       void *value)
{
	char text[2048];
	int consumed = 0;

	if (len >= sizeof(text)) {
		return -EINVAL;
	}
	memcpy(text, payload, len);
	text[len] = '\0';

	if (descr_len == 1U) {
		int subnet;

		if (sscanf(text, "{\"subnet\":%d}%n", &subnet, &consumed) != 1 ||
		    consumed != (int)len) {
			return -EINVAL;
		}
		*(int *)((char *)value + descr[0].offset) = subnet;
		return BIT64(0);
	}
	if (descr_len == 2U) {
		int mode;
		int voltage_mv;

		if (sscanf(text, "{\"mode\":%d,\"voltage_mv\":%d}%n", &mode,
			   &voltage_mv, &consumed) != 2 ||
		    consumed != (int)len) {
			return -EINVAL;
		}
		*(int *)((char *)value + descr[0].offset) = mode;
		*(int *)((char *)value + descr[1].offset) = voltage_mv;
		return BIT64(0) | BIT64(1);
	}

	return -EINVAL;
}

int linkr_debugger_net_format_ipv4(uint8_t subnet, uint8_t host,
				   char *buf, size_t cap)
{
	int len = snprintf(buf, cap, "172.29.%u.%u",
			   (unsigned int)subnet, (unsigned int)host);

	return len < 0 || (size_t)len >= cap ? -ENOSPC : 0;
}

uint8_t linkr_debugger_network_subnet(void)
{
	return current_subnet;
}

int linkr_debugger_network_set_subnet_with_result(uint8_t subnet,
						  bool *reconnect_required)
{
	setter_calls++;
	setter_subnet = subnet;
	*reconnect_required = current_subnet != subnet;
	current_subnet = subnet;
	return 0;
}

int linkr_debugger_pd_get_status_short(struct linkr_debugger_pd_status *status)
{
	*status = pd_status;
	return pd_status_result;
}

int linkr_debugger_pd_get_status(struct linkr_debugger_pd_status *status)
{
	pd_status_calls++;
	*status = pd_status;
	return pd_status_result;
}

int linkr_debugger_pd_set_pps(uint16_t voltage_mv)
{
	pd_pps_calls++;
	pd_pps_mv = voltage_mv;
	return 0;
}

int linkr_debugger_pd_set_fixed(uint16_t voltage_mv)
{
	pd_fixed_calls++;
	pd_fixed_mv = voltage_mv;
	return 0;
}

static void reset_test(void)
{
	setter_calls = 0U;
	setter_subnet = 0U;
	current_subnet = 203U;
	pd_fixed_calls = 0U;
	pd_pps_calls = 0U;
	pd_status_calls = 0U;
	pd_fixed_mv = 0U;
	pd_pps_mv = 0U;
	pd_status_result = 0;
	memset(&pd_status, 0, sizeof(pd_status));
	pd_status.available = true;
	pd_status.mode = LINKR_DEBUGGER_PD_MODE_FIXED;
	pd_status.pd_active = true;
	pd_status.voltage_mv = 5000U;
	pd_status.max_current_ma = 3000U;
	linkr_debugger_http_body_reset_all();
}

static void route(struct http_client_ctx *client,
		  enum http_transaction_status status,
		  const char *fragment, struct http_response_ctx *response)
{
	struct http_request_ctx request = {
		.data = (char *)fragment,
		.data_len = fragment != NULL ? strlen(fragment) : 0U,
	};

	assert(linkr_debugger_http_route_request(client, status, &request,
						 response, NULL) == 0);
}

static struct http_client_ctx net_put_client(void)
{
	struct http_client_ctx client = { .method = HTTP_PUT };

	strcpy(client.url_buffer, "/api/v1/net");
	return client;
}

static struct http_client_ctx pd_put_client(void)
{
	struct http_client_ctx client = { .method = HTTP_PUT };

	strcpy(client.url_buffer, "/api/v1/pd");
	return client;
}

static void test_fragmented_put_executes_once_with_complete_json(void)
{
	struct http_client_ctx client = net_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"subnet\":", &response);
	assert(setter_calls == 0U);
	assert(!response.final_chunk);

	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "204}", &response);
	assert(setter_calls == 1U);
	assert(setter_subnet == 204U);
	assert(response.status == HTTP_200_OK);
	assert(response.final_chunk);
}

static void test_aborted_and_complete_clear_without_executing(void)
{
	struct http_client_ctx client = net_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"subnet\":", &response);
	route(&client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, &response);
	assert(setter_calls == 0U);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "204}", &response);
	assert(setter_calls == 0U);
	assert(response.status == HTTP_400_BAD_REQUEST);

	memset(&response, 0, sizeof(response));
	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"subnet\":", &response);
	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	assert(setter_calls == 0U);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "204}", &response);
	assert(setter_calls == 0U);
	assert(response.status == HTTP_400_BAD_REQUEST);
}

static void test_invalid_prefix_cannot_be_replaced_by_valid_final_fragment(void)
{
	struct http_client_ctx client = net_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "garbage", &response);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "{\"subnet\":204}", &response);
	assert(setter_calls == 0U);
	assert(response.status == HTTP_400_BAD_REQUEST);
}

static void test_pd_fragmented_put_executes_once_with_complete_json(void)
{
	struct http_client_ctx client = pd_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"mode\":0,\"voltage_mv\":",
	      &response);
	assert(pd_fixed_calls == 0U);
	assert(pd_pps_calls == 0U);
	assert(!response.final_chunk);

	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "9000}", &response);
	assert(pd_fixed_calls == 1U);
	assert(pd_fixed_mv == 9000U);
	assert(pd_pps_calls == 0U);
	assert(pd_status_calls == 1U);
	assert(response.status == HTTP_200_OK);
	assert(response.final_chunk);
	assert(strstr((const char *)response.body, "\"action\":\"set\"") != NULL);
}

static void test_pd_aborted_request_clears_partial_body(void)
{
	struct http_client_ctx client = pd_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"mode\":0,", &response);
	route(&client, HTTP_SERVER_TRANSACTION_ABORTED, NULL, &response);
	assert(pd_fixed_calls == 0U);
	assert(pd_pps_calls == 0U);

	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
	      "\"voltage_mv\":9000}", &response);
	assert(pd_fixed_calls == 0U);
	assert(pd_pps_calls == 0U);
	assert(response.status == HTTP_400_BAD_REQUEST);

	memset(&response, 0, sizeof(response));
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"mode\":0,\"voltage_mv\":",
	      &response);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "9000}", &response);
	assert(pd_fixed_calls == 1U);
	assert(pd_fixed_mv == 9000U);
	assert(response.status == HTTP_200_OK);
}

static void test_pd_complete_request_clears_partial_body(void)
{
	struct http_client_ctx client = pd_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"mode\":0,", &response);
	route(&client, HTTP_SERVER_TRANSACTION_COMPLETE, NULL, &response);
	assert(pd_fixed_calls == 0U);
	assert(pd_pps_calls == 0U);

	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
	      "\"voltage_mv\":12000}", &response);
	assert(pd_fixed_calls == 0U);
	assert(pd_pps_calls == 0U);
	assert(response.status == HTTP_400_BAD_REQUEST);

	memset(&response, 0, sizeof(response));
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "{\"mode\":0,\"voltage_mv\":",
	      &response);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL, "12000}", &response);
	assert(pd_fixed_calls == 1U);
	assert(pd_fixed_mv == 12000U);
	assert(response.status == HTTP_200_OK);
}

static void test_pd_invalid_prefix_cannot_be_replaced_by_valid_final_fragment(void)
{
	struct http_client_ctx client = pd_put_client();
	struct http_response_ctx response = { 0 };

	reset_test();
	route(&client, HTTP_SERVER_REQUEST_DATA_MORE, "garbage", &response);
	route(&client, HTTP_SERVER_REQUEST_DATA_FINAL,
	      "{\"mode\":0,\"voltage_mv\":9000}", &response);
	assert(pd_fixed_calls == 0U);
	assert(pd_pps_calls == 0U);
	assert(response.status == HTTP_400_BAD_REQUEST);
	assert(strstr((const char *)response.body, "\"code\":\"invalid_body\"") != NULL);
}

int main(void)
{
	test_fragmented_put_executes_once_with_complete_json();
	test_aborted_and_complete_clear_without_executing();
	test_invalid_prefix_cannot_be_replaced_by_valid_final_fragment();
	test_pd_fragmented_put_executes_once_with_complete_json();
	test_pd_aborted_request_clears_partial_body();
	test_pd_complete_request_clears_partial_body();
	test_pd_invalid_prefix_cannot_be_replaced_by_valid_final_fragment();
	return 0;
}
