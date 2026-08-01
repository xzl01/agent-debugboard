#ifndef RADXA_LINKR_DEBUGGER_CONFIG_HTTP_PARSE_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_HTTP_PARSE_H_

#include "linkr_debugger_config_service.h"
#include "linkr_debugger_http_body.h"

enum linkr_debugger_config_http_parse_result {
	LINKR_DEBUGGER_CONFIG_HTTP_PARSE_OK = 0,
	LINKR_DEBUGGER_CONFIG_HTTP_PARSE_INVALID_JSON,
};

struct linkr_debugger_config_http_save_payload {
	char storage[LINKR_DEBUGGER_HTTP_BODY_CAP + 1U];
	struct linkr_debugger_config_save_request request;
};

enum linkr_debugger_config_http_parse_result
linkr_debugger_config_http_parse_save(
	const uint8_t *data, size_t len,
	struct linkr_debugger_config_http_save_payload *payload);

enum linkr_debugger_config_http_parse_result
linkr_debugger_config_http_parse_apply(const uint8_t *data, size_t len,
				       bool *confirmed);

#endif
