#ifndef RADXA_LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_H_

#include "linkr_debugger_config_service.h"

enum linkr_debugger_config_http_action {
	LINKR_DEBUGGER_CONFIG_HTTP_ACTION_GET,
	LINKR_DEBUGGER_CONFIG_HTTP_ACTION_SAVE,
	LINKR_DEBUGGER_CONFIG_HTTP_ACTION_APPLY,
	LINKR_DEBUGGER_CONFIG_HTTP_ACTION_CLEAR,
};

enum linkr_debugger_config_http_encode_result {
	LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_OK = 0,
	LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_NO_SPACE,
	LINKR_DEBUGGER_CONFIG_HTTP_ENCODE_INVALID_STATE,
};

struct linkr_debugger_config_http_error;

enum linkr_debugger_config_http_encode_result
linkr_debugger_config_http_encode_get(
	const struct linkr_debugger_config_service_status *status,
	char *buffer, size_t capacity, size_t *encoded_size);

enum linkr_debugger_config_http_encode_result
linkr_debugger_config_http_encode_save(
	const struct linkr_debugger_config_save_request *request,
	const struct linkr_debugger_config_operation_report *report,
	char *buffer, size_t capacity, size_t *encoded_size);

enum linkr_debugger_config_http_encode_result
linkr_debugger_config_http_encode_apply(
	const struct linkr_debugger_config_operation_report *report,
	bool noop, char *buffer, size_t capacity, size_t *encoded_size);

enum linkr_debugger_config_http_encode_result
linkr_debugger_config_http_encode_clear(
	bool noop, char *buffer, size_t capacity, size_t *encoded_size);

enum linkr_debugger_config_http_encode_result
linkr_debugger_config_http_encode_error(
	enum linkr_debugger_config_http_action action,
	const struct linkr_debugger_config_http_error *error,
	const struct linkr_debugger_config_operation_report *report,
	char *buffer, size_t capacity, size_t *encoded_size);

#endif
