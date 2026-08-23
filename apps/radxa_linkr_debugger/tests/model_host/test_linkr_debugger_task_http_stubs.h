#ifndef LINKR_DEBUGGER_TASK_HTTP_TEST_STUBS_H_
#define LINKR_DEBUGGER_TASK_HTTP_TEST_STUBS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINKR_DEBUGGER_TASK_HTTP_HOST_TEST 1

enum http_status {
	HTTP_200_OK = 200,
	HTTP_400_BAD_REQUEST = 400,
	HTTP_404_NOT_FOUND = 404,
	HTTP_405_METHOD_NOT_ALLOWED = 405,
	HTTP_409_CONFLICT = 409,
	HTTP_413_PAYLOAD_TOO_LARGE = 413,
	HTTP_500_INTERNAL_SERVER_ERROR = 500,
};

enum http_transaction_status {
	HTTP_SERVER_TRANSACTION_ABORTED = -1,
	HTTP_SERVER_REQUEST_DATA_MORE = 0,
	HTTP_SERVER_REQUEST_DATA_FINAL = 1,
	HTTP_SERVER_TRANSACTION_COMPLETE = 2,
};

enum http_header_status {
	HTTP_HEADER_STATUS_OK,
	HTTP_HEADER_STATUS_DROPPED,
	HTTP_HEADER_STATUS_NONE,
};

enum http_method {
	HTTP_DELETE = 0,
	HTTP_GET = 1,
	HTTP_POST = 3,
	HTTP_PUT = 4,
};

struct http_header {
	const char *name;
	const char *value;
};

struct http_request_ctx {
	uint8_t *data;
	size_t data_len;
	struct http_header *headers;
	size_t header_count;
	enum http_header_status headers_status;
};

struct http_response_ctx {
	enum http_status status;
	const struct http_header *headers;
	size_t header_count;
	const uint8_t *body;
	size_t body_len;
	bool final_chunk;
};

struct http_client_ctx {
	enum http_method method;
};

#endif
