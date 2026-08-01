#include "../../src/linkr_debugger_config_summary.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define ARRAY_SIZE_LOCAL(array) (sizeof(array) / sizeof((array)[0]))
#define HTTP_TAIL_RESERVE 2U
#define WS_TAIL_RESERVE 1U

static const char maximum_fragment[] =
	",\"config\":{\"available\":false,\"reason\":\"unsupported_version\","
	"\"saved_count\":23,\"pending_count\":23}";

_Static_assert(LINKR_DEBUGGER_CONFIG_MAX_ENTRIES == 23U,
	       "config item capacity changed");
_Static_assert(sizeof(maximum_fragment) - 1U ==
	       LINKR_DEBUGGER_CONFIG_SUMMARY_FRAGMENT_MAX,
	       "config summary maximum fragment changed");

static void test_summary_maps_all_internal_reasons(void)
{
	static const struct {
		enum linkr_debugger_config_service_reason internal_reason;
		const char *public_reason;
	} cases[] = {
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNINITIALIZED, "storage_error" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY, "ready" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT, "absent" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE,
		  "storage_error" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_STORAGE_ERROR, "storage_error" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_INVALID_SNAPSHOT,
		  "invalid_snapshot" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNSUPPORTED_VERSION,
		  "unsupported_version" },
	};

	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(cases); i++) {
		struct linkr_debugger_config_service_status status = {
			.available = false,
			.reason = cases[i].internal_reason,
			.saved_count = 5U,
			.pending_count = 3U,
		};
		struct linkr_debugger_config_summary summary = {0};

		/* Given */
		assert(status.reason == cases[i].internal_reason);

		/* When */
		assert(linkr_debugger_config_summary_from_status(&status, &summary));

		/* Then */
		assert(!summary.available);
		assert(strcmp(summary.reason, cases[i].public_reason) == 0);
		assert(summary.saved_count == status.saved_count);
		assert(summary.pending_count == status.pending_count);
	}
}

static void test_summary_copies_false_available_from_ready_status(void)
{
	struct linkr_debugger_config_service_status status = {
		.available = false,
		.reason = LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY,
		.saved_count = 1U,
		.pending_count = 0U,
	};
	struct linkr_debugger_config_summary summary = {0};

	/* Given */
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY);
	assert(!status.available);

	/* When */
	assert(linkr_debugger_config_summary_from_status(&status, &summary));

	/* Then */
	assert(!summary.available);
	assert(strcmp(summary.reason, "ready") == 0);
}

static void test_summary_copies_true_available_from_storage_error_status(void)
{
	struct linkr_debugger_config_service_status status = {
		.available = true,
		.reason = LINKR_DEBUGGER_CONFIG_SERVICE_REASON_STORAGE_ERROR,
		.saved_count = 1U,
		.pending_count = 0U,
	};
	struct linkr_debugger_config_summary summary = {0};

	/* Given */
	assert(status.reason == LINKR_DEBUGGER_CONFIG_SERVICE_REASON_STORAGE_ERROR);
	assert(status.available);

	/* When */
	assert(linkr_debugger_config_summary_from_status(&status, &summary));

	/* Then */
	assert(summary.available);
	assert(strcmp(summary.reason, "storage_error") == 0);
}

static void test_append_serializes_identical_http_and_ws_fragments(void)
{
	static const char expected[] =
		",\"config\":{\"available\":false,\"reason\":\"storage_error\","
		"\"saved_count\":0,\"pending_count\":0}";
	char http[sizeof(expected) + HTTP_TAIL_RESERVE];
	char ws[sizeof(expected) + WS_TAIL_RESERVE];
	struct linkr_debugger_config_service_status status = {
		.available = false,
		.reason = LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE,
		.saved_count = 0U,
		.pending_count = 0U,
	};
	struct linkr_debugger_config_summary_buffer http_buffer = {
		.data = http,
		.capacity = sizeof(http),
		.length = 0U,
		.tail_reserve = HTTP_TAIL_RESERVE,
	};
	struct linkr_debugger_config_summary_buffer ws_buffer = {
		.data = ws,
		.capacity = sizeof(ws),
		.length = 0U,
		.tail_reserve = WS_TAIL_RESERVE,
	};

	/* Given */
	memset(http, 0xa5, sizeof(http));
	memset(ws, 0xa5, sizeof(ws));

	/* When */
	assert(linkr_debugger_config_summary_append(&http_buffer, &status) ==
	       LINKR_DEBUGGER_CONFIG_SUMMARY_APPENDED);
	assert(linkr_debugger_config_summary_append(&ws_buffer, &status) ==
	       LINKR_DEBUGGER_CONFIG_SUMMARY_APPENDED);

	/* Then */
	assert(http_buffer.length == sizeof(expected) - 1U);
	assert(ws_buffer.length == http_buffer.length);
	assert(strcmp(http, expected) == 0);
	assert(strcmp(ws, expected) == 0);
	assert(memcmp(http, ws, http_buffer.length) == 0);
}

static void test_append_honors_exact_maximum_tail_reserves(void)
{
	char http[LINKR_DEBUGGER_CONFIG_SUMMARY_FRAGMENT_MAX + HTTP_TAIL_RESERVE + 1U];
	char ws[LINKR_DEBUGGER_CONFIG_SUMMARY_FRAGMENT_MAX + WS_TAIL_RESERVE + 1U];
	struct linkr_debugger_config_service_status status = {
		.available = false,
		.reason = LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNSUPPORTED_VERSION,
		.saved_count = LINKR_DEBUGGER_CONFIG_MAX_ENTRIES,
		.pending_count = LINKR_DEBUGGER_CONFIG_MAX_ENTRIES,
	};
	struct linkr_debugger_config_summary_buffer http_buffer = {
		.data = http,
		.capacity = sizeof(http),
		.length = 0U,
		.tail_reserve = HTTP_TAIL_RESERVE,
	};
	struct linkr_debugger_config_summary_buffer ws_buffer = {
		.data = ws,
		.capacity = sizeof(ws),
		.length = 0U,
		.tail_reserve = WS_TAIL_RESERVE,
	};

	/* Given */
	memset(http, 0xa5, sizeof(http));
	memset(ws, 0xa5, sizeof(ws));
	assert(sizeof(http) == 99U);
	assert(sizeof(ws) == 98U);

	/* When */
	assert(linkr_debugger_config_summary_append(&http_buffer, &status) ==
	       LINKR_DEBUGGER_CONFIG_SUMMARY_APPENDED);
	assert(linkr_debugger_config_summary_append(&ws_buffer, &status) ==
	       LINKR_DEBUGGER_CONFIG_SUMMARY_APPENDED);

	/* Then */
	assert(http_buffer.length == LINKR_DEBUGGER_CONFIG_SUMMARY_FRAGMENT_MAX);
	assert(ws_buffer.length == LINKR_DEBUGGER_CONFIG_SUMMARY_FRAGMENT_MAX);
	assert(strcmp(http, maximum_fragment) == 0);
	assert(strcmp(ws, maximum_fragment) == 0);
	assert(http[http_buffer.length] == '\0');
	assert(ws[ws_buffer.length] == '\0');
	assert((unsigned char)http[http_buffer.length + 1U] == 0xa5U);
	assert((unsigned char)http[http_buffer.length + 2U] == 0xa5U);
	assert((unsigned char)ws[ws_buffer.length + 1U] == 0xa5U);

	http[http_buffer.length] = '}';
	http[http_buffer.length + 1U] = '\n';
	http[http_buffer.length + HTTP_TAIL_RESERVE] = '\0';
	ws[ws_buffer.length] = '}';
	ws[ws_buffer.length + WS_TAIL_RESERVE] = '\0';
	assert(strcmp(&http[http_buffer.length], "}\n") == 0);
	assert(strcmp(&ws[ws_buffer.length], "}") == 0);
}

static void test_one_byte_short_buffers_omit_atomically(void)
{
	char http[LINKR_DEBUGGER_CONFIG_SUMMARY_FRAGMENT_MAX + HTTP_TAIL_RESERVE];
	char ws[LINKR_DEBUGGER_CONFIG_SUMMARY_FRAGMENT_MAX + WS_TAIL_RESERVE];
	char http_before[sizeof(http)];
	char ws_before[sizeof(ws)];
	struct linkr_debugger_config_service_status status = {
		.available = false,
		.reason = LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNSUPPORTED_VERSION,
		.saved_count = LINKR_DEBUGGER_CONFIG_MAX_ENTRIES,
		.pending_count = LINKR_DEBUGGER_CONFIG_MAX_ENTRIES,
	};
	struct linkr_debugger_config_summary_buffer http_buffer = {
		.data = http,
		.capacity = sizeof(http),
		.length = 0U,
		.tail_reserve = HTTP_TAIL_RESERVE,
	};
	struct linkr_debugger_config_summary_buffer ws_buffer = {
		.data = ws,
		.capacity = sizeof(ws),
		.length = 0U,
		.tail_reserve = WS_TAIL_RESERVE,
	};

	/* Given */
	memset(http, 0xa5, sizeof(http));
	memset(ws, 0xa5, sizeof(ws));
	memcpy(http_before, http, sizeof(http));
	memcpy(ws_before, ws, sizeof(ws));
	assert(sizeof(http) == 98U);
	assert(sizeof(ws) == 97U);

	/* When */
	assert(linkr_debugger_config_summary_append(&http_buffer, &status) ==
	       LINKR_DEBUGGER_CONFIG_SUMMARY_OMITTED);
	assert(linkr_debugger_config_summary_append(&ws_buffer, &status) ==
	       LINKR_DEBUGGER_CONFIG_SUMMARY_OMITTED);

	/* Then */
	assert(http_buffer.length == 0U);
	assert(ws_buffer.length == 0U);
	assert(memcmp(http, http_before, sizeof(http)) == 0);
	assert(memcmp(ws, ws_before, sizeof(ws)) == 0);
}

static void test_invalid_statuses_omit_atomically(void)
{
	static const struct {
		enum linkr_debugger_config_service_reason reason;
		size_t saved_count;
		size_t pending_count;
	} cases[] = {
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY,
		  LINKR_DEBUGGER_CONFIG_MAX_ENTRIES + 1U, 0U },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY, 1U, 2U },
		{ (enum linkr_debugger_config_service_reason)99, 0U, 0U },
	};
	static const char legacy_prefix[] = "{\"legacy\":true";

	for (size_t i = 0U; i < ARRAY_SIZE_LOCAL(cases); i++) {
		char data[128];
		char before[sizeof(data)];
		struct linkr_debugger_config_service_status status = {
			.available = true,
			.reason = cases[i].reason,
			.saved_count = cases[i].saved_count,
			.pending_count = cases[i].pending_count,
		};
		struct linkr_debugger_config_summary summary = {0};
		struct linkr_debugger_config_summary_buffer buffer = {
			.data = data,
			.capacity = sizeof(data),
			.length = sizeof(legacy_prefix) - 1U,
			.tail_reserve = HTTP_TAIL_RESERVE,
		};

		/* Given */
		memset(data, 0xa5, sizeof(data));
		memcpy(data, legacy_prefix, sizeof(legacy_prefix) - 1U);
		memcpy(before, data, sizeof(data));

		/* When */
		assert(!linkr_debugger_config_summary_from_status(&status, &summary));
		assert(linkr_debugger_config_summary_append(&buffer, &status) ==
		       LINKR_DEBUGGER_CONFIG_SUMMARY_OMITTED);

		/* Then */
		assert(buffer.length == sizeof(legacy_prefix) - 1U);
		assert(memcmp(data, before, sizeof(data)) == 0);
	}
}

static void test_null_status_omits_atomically(void)
{
	static const char legacy_prefix[] = "{\"legacy\":true";
	char data[64];
	char before[sizeof(data)];
	struct linkr_debugger_config_summary_buffer buffer = {
		.data = data,
		.capacity = sizeof(data),
		.length = sizeof(legacy_prefix) - 1U,
		.tail_reserve = WS_TAIL_RESERVE,
	};

	/* Given */
	memset(data, 0xa5, sizeof(data));
	memcpy(data, legacy_prefix, sizeof(legacy_prefix) - 1U);
	memcpy(before, data, sizeof(data));

	/* When */
	assert(linkr_debugger_config_summary_append(&buffer, NULL) ==
	       LINKR_DEBUGGER_CONFIG_SUMMARY_OMITTED);

	/* Then */
	assert(buffer.length == sizeof(legacy_prefix) - 1U);
	assert(memcmp(data, before, sizeof(data)) == 0);
}

int main(void)
{
	test_summary_maps_all_internal_reasons();
	test_summary_copies_false_available_from_ready_status();
	test_summary_copies_true_available_from_storage_error_status();
	test_append_serializes_identical_http_and_ws_fragments();
	test_append_honors_exact_maximum_tail_reserves();
	test_one_byte_short_buffers_omit_atomically();
	test_invalid_statuses_omit_atomically();
	test_null_status_omits_atomically();
	return 0;
}
