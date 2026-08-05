#ifndef RADXA_LINKR_DEBUGGER_CONFIG_HTTP_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_HTTP_H_

#ifndef LINKR_DEBUGGER_CONFIG_HTTP_HOST_TEST
#include <zephyr/net/http/server.h>
#endif

#define LINKR_DEBUGGER_CONFIG_HTTP_RESPONSE_CAP 4160U

enum linkr_debugger_config_http_route {
	LINKR_DEBUGGER_CONFIG_HTTP_ROUTE_CONFIG = 3,
};

int linkr_debugger_config_http_handle(
	struct http_client_ctx *client,
	enum http_transaction_status status,
	const struct http_request_ctx *request_ctx,
	struct http_response_ctx *response_ctx,
	void *user_data);

#endif
