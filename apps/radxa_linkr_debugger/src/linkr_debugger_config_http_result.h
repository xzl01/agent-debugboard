#ifndef RADXA_LINKR_DEBUGGER_CONFIG_HTTP_RESULT_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_HTTP_RESULT_H_

#include "linkr_debugger_config_http_encode.h"

#ifndef LINKR_DEBUGGER_CONFIG_HTTP_HOST_TEST
#include <zephyr/net/http/server.h>
#endif

struct linkr_debugger_config_http_error {
	enum http_status status;
	const char *code;
	const char *message;
	const char *activity;
};

bool linkr_debugger_config_http_map_service_result(
	enum linkr_debugger_config_http_action action,
	enum linkr_debugger_config_service_result result,
	struct linkr_debugger_config_http_error *error);

const char *linkr_debugger_config_http_reason_name(
	enum linkr_debugger_config_service_reason reason);

const char *linkr_debugger_config_http_apply_state_name(
	enum linkr_debugger_config_apply_state state);

#endif
