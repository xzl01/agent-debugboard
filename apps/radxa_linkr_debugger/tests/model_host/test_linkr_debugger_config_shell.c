#include "stubs/zephyr/shell/shell.h"

#include "../../src/linkr_debugger_config_service.h"
#include "../../src/linkr_debugger_config_shell.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE_LOCAL(array) (sizeof(array) / sizeof((array)[0]))
#define SHELL_OUTPUT_CAP 32U
#define SHELL_OUTPUT_LINE_CAP 160U
#define SAVED_ID_CAP 64U

_Static_assert(LINKR_DEBUGGER_CONFIG_MAX_ENTRIES == 23U,
	       "config shell selection limit changed");

enum fake_shell_line_kind {
	FAKE_SHELL_LINE_PRINT,
	FAKE_SHELL_LINE_ERROR,
};

struct fake_shell_output {
	enum fake_shell_line_kind kinds[SHELL_OUTPUT_CAP];
	char lines[SHELL_OUTPUT_CAP][SHELL_OUTPUT_LINE_CAP];
	size_t line_count;
	size_t print_calls;
	size_t error_calls;
};

struct service_call_counts {
	size_t status;
	size_t save;
	size_t apply;
	size_t clear;
};

struct fake_service {
	enum linkr_debugger_config_service_result status_result;
	enum linkr_debugger_config_service_result save_result;
	enum linkr_debugger_config_service_result apply_result;
	enum linkr_debugger_config_service_result clear_result;
	struct linkr_debugger_config_service_status status;
	struct linkr_debugger_config_operation_report save_report;
	struct linkr_debugger_config_operation_report apply_report;
	struct service_call_counts calls;
	size_t saved_item_count;
	char saved_ids[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES][SAVED_ID_CAP];
	bool last_save_confirmed;
	bool last_apply_confirmed;
};

static struct fake_shell_output shell_output;
static struct fake_service service;
static const struct shell test_shell;
static const struct linkr_debugger_config_item_desc item_power_12v = {
	.domain = LINKR_DEBUGGER_CONFIG_DOMAIN_POWER,
	.item_id = LINKR_DEBUGGER_CONFIG_POWER_12V_OUT_ID,
	.id = "power/12v_out",
};
static const struct linkr_debugger_config_item_desc item_switch_sd = {
	.domain = LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
	.item_id = LINKR_DEBUGGER_CONFIG_SWITCH_SD_ID,
	.id = "switch/sd",
};
static const struct linkr_debugger_config_item_desc item_switch_usb = {
	.domain = LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
	.item_id = LINKR_DEBUGGER_CONFIG_SWITCH_USB_ID,
	.id = "switch/usb",
};
static const struct linkr_debugger_config_item_desc item_switch_vin = {
	.domain = LINKR_DEBUGGER_CONFIG_DOMAIN_SWITCH,
	.item_id = LINKR_DEBUGGER_CONFIG_SWITCH_VIN_ID,
	.id = "switch/vin",
};
static const struct linkr_debugger_config_item_desc item_gpio_7 = {
	.domain = LINKR_DEBUGGER_CONFIG_DOMAIN_GPIO,
	.item_id = 7U,
	.id = "gpio/GP7",
};

static void fake_shell_write(enum fake_shell_line_kind kind, const char *format,
			     va_list arguments)
{
	int written;

	assert(shell_output.line_count < SHELL_OUTPUT_CAP);
	written = vsnprintf(shell_output.lines[shell_output.line_count],
			    SHELL_OUTPUT_LINE_CAP, format, arguments);
	assert(written >= 0);
	assert((size_t)written < SHELL_OUTPUT_LINE_CAP);
	shell_output.kinds[shell_output.line_count++] = kind;
	if (kind == FAKE_SHELL_LINE_PRINT) {
		shell_output.print_calls++;
	} else {
		shell_output.error_calls++;
	}
}

void shell_print(const struct shell *sh, const char *format, ...)
{
	va_list arguments;

	(void)sh;
	va_start(arguments, format);
	fake_shell_write(FAKE_SHELL_LINE_PRINT, format, arguments);
	va_end(arguments);
}

void shell_error(const struct shell *sh, const char *format, ...)
{
	va_list arguments;

	(void)sh;
	va_start(arguments, format);
	fake_shell_write(FAKE_SHELL_LINE_ERROR, format, arguments);
	va_end(arguments);
}

static void reset_fixture(void)
{
	memset(&shell_output, 0, sizeof(shell_output));
	memset(&service, 0, sizeof(service));
	service.status_result = LINKR_DEBUGGER_CONFIG_SERVICE_OK;
	service.save_result = LINKR_DEBUGGER_CONFIG_SERVICE_OK;
	service.apply_result = LINKR_DEBUGGER_CONFIG_SERVICE_OK;
	service.clear_result = LINKR_DEBUGGER_CONFIG_SERVICE_OK;
	service.status.available = true;
	service.status.reason = LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY;
	service.status.saved_count = 2U;
	service.status.pending_count = 1U;
}

enum linkr_debugger_config_service_result linkr_debugger_config_service_status_get(
	struct linkr_debugger_config_service_status *status)
{
	assert(status != NULL);
	service.calls.status++;
	*status = service.status;
	return service.status_result;
}

enum linkr_debugger_config_service_result linkr_debugger_config_service_save(
	const struct linkr_debugger_config_save_request *request,
	struct linkr_debugger_config_operation_report *report)
{
	assert(request != NULL);
	assert(report != NULL);
	assert(request->item_count <= LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	service.calls.save++;
	service.saved_item_count = request->item_count;
	service.last_save_confirmed = request->confirmed;
	for (size_t index = 0U; index < request->item_count; index++) {
		int written;

		assert(request->item_ids[index] != NULL);
		written = snprintf(service.saved_ids[index], SAVED_ID_CAP, "%s",
				   request->item_ids[index]);
		assert(written >= 0);
		assert((size_t)written < SAVED_ID_CAP);
	}
	*report = service.save_report;
	report->result = service.save_result;
	return service.save_result;
}

enum linkr_debugger_config_service_result linkr_debugger_config_service_apply(
	bool confirmed, struct linkr_debugger_config_operation_report *report)
{
	assert(report != NULL);
	service.calls.apply++;
	service.last_apply_confirmed = confirmed;
	*report = service.apply_report;
	report->result = service.apply_result;
	return service.apply_result;
}

enum linkr_debugger_config_service_result linkr_debugger_config_service_clear(void)
{
	service.calls.clear++;
	return service.clear_result;
}

static void assert_service_calls(const struct service_call_counts expected)
{
	assert(service.calls.status == expected.status);
	assert(service.calls.save == expected.save);
	assert(service.calls.apply == expected.apply);
	assert(service.calls.clear == expected.clear);
}

static void assert_no_service_calls(void)
{
	assert_service_calls((struct service_call_counts){0});
}

static void assert_shell_output(enum fake_shell_line_kind expected_kind,
				const char *const expected_lines[], size_t expected_count)
{
	assert(shell_output.line_count == expected_count);
	assert(shell_output.print_calls ==
	       (expected_kind == FAKE_SHELL_LINE_PRINT ? expected_count : 0U));
	assert(shell_output.error_calls ==
	       (expected_kind == FAKE_SHELL_LINE_ERROR ? expected_count : 0U));
	for (size_t index = 0U; index < expected_count; index++) {
		assert(shell_output.kinds[index] == expected_kind);
		assert(strcmp(shell_output.lines[index], expected_lines[index]) == 0);
	}
}

static void assert_print_line(const char *expected_line)
{
	const char *const expected_lines[] = {expected_line};

	assert_shell_output(FAKE_SHELL_LINE_PRINT, expected_lines,
			    ARRAY_SIZE_LOCAL(expected_lines));
}

static void assert_error_line(const char *expected_line)
{
	const char *const expected_lines[] = {expected_line};

	assert_shell_output(FAKE_SHELL_LINE_ERROR, expected_lines,
			    ARRAY_SIZE_LOCAL(expected_lines));
}

static void assert_saved_request(const char *const expected_ids[], size_t expected_count,
				 bool expected_confirmed)
{
	assert(service.saved_item_count == expected_count);
	assert(service.last_save_confirmed == expected_confirmed);
	for (size_t index = 0U; index < expected_count; index++) {
		assert(strcmp(service.saved_ids[index], expected_ids[index]) == 0);
	}
}

static void test_show_prints_public_status_when_service_succeeds(void)
{
	char *argv[] = {"show"};

	/* Given */
	reset_fixture();
	service.status.available = false;
	service.status.reason = LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE;
	service.status.saved_count = 3U;
	service.status.pending_count = 2U;

	/* When */
	const int result = linkr_debugger_config_shell_show(&test_shell,
						     ARRAY_SIZE_LOCAL(argv), argv);

	/* Then */
	assert(result == 0);
	assert_print_line("config available=false reason=storage_error saved_count=3 pending_count=2");
	assert_service_calls((struct service_call_counts){.status = 1U});
}

static void test_show_rejects_invalid_grammar_before_service_io(void)
{
	static struct {
		size_t argc;
		char *argv[2];
	} cases[] = {
		{ 0U, { "show", NULL } },
		{ 2U, { "show", "extra" } },
	};

	for (size_t index = 0U; index < ARRAY_SIZE_LOCAL(cases); index++) {
		/* Given */
		reset_fixture();

		/* When */
		const int result = linkr_debugger_config_shell_show(
			&test_shell, cases[index].argc, cases[index].argv);

		/* Then */
		assert(result == -EINVAL);
		assert_error_line("config show error=invalid_arguments");
		assert_no_service_calls();
	}
}

static void test_show_maps_service_failure(void)
{
	char *argv[] = {"show"};

	/* Given */
	reset_fixture();
	service.status_result = LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR;

	/* When */
	const int result = linkr_debugger_config_shell_show(&test_shell,
						     ARRAY_SIZE_LOCAL(argv), argv);

	/* Then */
	assert(result == -EIO);
	assert_error_line("config show error=storage_error");
	assert_service_calls((struct service_call_counts){.status = 1U});
}

static void test_save_preserves_ids_without_confirmation(void)
{
	char *argv[] = {"save", "power/12v_out", "switch/sd"};
	const char *const expected_ids[] = {"power/12v_out", "switch/sd"};

	/* Given */
	reset_fixture();

	/* When */
	const int result = linkr_debugger_config_shell_save(&test_shell,
						     ARRAY_SIZE_LOCAL(argv), argv);

	/* Then */
	assert(result == 0);
	assert_print_line("config save saved_count=2 pending_count=0");
	assert_saved_request(expected_ids, ARRAY_SIZE_LOCAL(expected_ids), false);
	assert_service_calls((struct service_call_counts){.save = 1U});
}

static void test_save_accepts_confirmation_before_ids(void)
{
	char *argv[] = {"save", "--confirm", "power/12v_out", "switch/sd"};
	const char *const expected_ids[] = {"power/12v_out", "switch/sd"};

	/* Given */
	reset_fixture();

	/* When */
	const int result = linkr_debugger_config_shell_save(&test_shell,
						     ARRAY_SIZE_LOCAL(argv), argv);

	/* Then */
	assert(result == 0);
	assert_print_line("config save saved_count=2 pending_count=0");
	assert_saved_request(expected_ids, ARRAY_SIZE_LOCAL(expected_ids), true);
	assert_service_calls((struct service_call_counts){.save = 1U});
}

static void test_save_accepts_confirmation_after_maximum_selection(void)
{
	char *argv[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES + 2U];

	/* Given */
	reset_fixture();
	argv[0] = "save";
	for (size_t index = 0U; index < LINKR_DEBUGGER_CONFIG_MAX_ENTRIES; index++) {
		argv[index + 1U] = "power/12v_out";
	}
	argv[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES + 1U] = "--confirm";

	/* When */
	const int result = linkr_debugger_config_shell_save(&test_shell,
						     ARRAY_SIZE_LOCAL(argv), argv);

	/* Then */
	assert(result == 0);
	assert_print_line("config save saved_count=23 pending_count=0");
	assert(service.saved_item_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
	assert(service.last_save_confirmed);
	assert(strcmp(service.saved_ids[0], "power/12v_out") == 0);
	assert(strcmp(service.saved_ids[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES - 1U],
		      "power/12v_out") == 0);
	assert_service_calls((struct service_call_counts){.save = 1U});
}

static void test_save_rejects_syntax_before_service_io(void)
{
	static struct {
		size_t argc;
		char *argv[4];
		const char *error_line;
	} cases[] = {
		{ 1U, { "save", NULL, NULL, NULL },
		  "config save error=empty_selection" },
		{ 2U, { "save", "--confirm", NULL, NULL },
		  "config save error=empty_selection" },
		{ 4U, { "save", "--confirm", "power/12v_out", "--confirm" },
		  "config save error=duplicate_confirm" },
		{ 3U, { "save", "power/12v_out", "--unknown", NULL },
		  "config save error=unknown_option" },
	};

	for (size_t index = 0U; index < ARRAY_SIZE_LOCAL(cases); index++) {
		/* Given */
		reset_fixture();

		/* When */
		const int result = linkr_debugger_config_shell_save(
			&test_shell, cases[index].argc, cases[index].argv);

		/* Then */
		assert(result == -EINVAL);
		assert_error_line(cases[index].error_line);
		assert_no_service_calls();
	}
}

static void test_save_rejects_too_many_ids_before_service_io(void)
{
	char *argv[LINKR_DEBUGGER_CONFIG_MAX_ENTRIES + 2U];

	/* Given */
	reset_fixture();
	argv[0] = "save";
	for (size_t index = 1U; index < ARRAY_SIZE_LOCAL(argv); index++) {
		argv[index] = "power/12v_out";
	}

	/* When */
	const int result = linkr_debugger_config_shell_save(&test_shell,
						     ARRAY_SIZE_LOCAL(argv), argv);

	/* Then */
	assert(result == -E2BIG);
	assert_error_line("config save error=too_many_items max=23");
	assert_no_service_calls();
}

struct service_error_case {
	enum linkr_debugger_config_service_result result;
	int expected_return;
	const char *error_line;
};

static void test_save_maps_every_non_apply_service_result(void)
{
	static const struct service_error_case cases[] = {
		{ LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT, -EINVAL,
		  "config save error=invalid_argument" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION, -EINVAL,
		  "config save error=empty_selection" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM, -ENOENT,
		  "config save error=unknown_item" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM, -EEXIST,
		  "config save error=duplicate_item" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_ITEM_UNAVAILABLE, -ENODEV,
		  "config save error=item_unavailable" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED, -EACCES,
		  "config save error=confirmation_required" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE, -EBUSY,
		  "config save error=busy activity=capture" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_FLASH, -EBUSY,
		  "config save error=busy activity=ota" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE, -ENODEV,
		  "config save error=backend_unavailable" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_NO_SNAPSHOT, -ENOENT,
		  "config save error=no_snapshot" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT, -EBADMSG,
		  "config save error=invalid_snapshot" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_UNSUPPORTED_VERSION, -ENOTSUP,
		  "config save error=unsupported_version" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR, -EIO,
		  "config save error=storage_error" },
		{ LINKR_DEBUGGER_CONFIG_SERVICE_CONTROL_CAPTURE_FAILED, -EIO,
		  "config save error=control_capture_failed" },
	};
	char *argv[] = {"save", "power/12v_out"};

	for (size_t index = 0U; index < ARRAY_SIZE_LOCAL(cases); index++) {
		/* Given */
		reset_fixture();
		service.save_result = cases[index].result;
		if (cases[index].result ==
		    LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED) {
			service.save_report.confirmation_count = 2U;
			service.save_report.confirmation_items[0] = &item_switch_usb;
			service.save_report.confirmation_items[1] = &item_switch_vin;
		}

		/* When */
		const int result = linkr_debugger_config_shell_save(
			&test_shell, ARRAY_SIZE_LOCAL(argv), argv);

		/* Then */
		assert(result == cases[index].expected_return);
		if (cases[index].result ==
		    LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED) {
			const char *const expected_lines[] = {
				"config save error=confirmation_required",
				"config save confirmation_id=switch/usb",
				"config save confirmation_id=switch/vin",
			};

			assert_shell_output(FAKE_SHELL_LINE_ERROR, expected_lines,
					    ARRAY_SIZE_LOCAL(expected_lines));
		} else {
			assert_error_line(cases[index].error_line);
		}
		assert_service_calls((struct service_call_counts){.save = 1U});
	}
}

static void test_apply_prints_report_counts_when_service_succeeds(void)
{
	char *argv[] = {"apply", "--confirm"};

	/* Given */
	reset_fixture();
	service.apply_report.applied_count = 2U;
	service.apply_report.pending_count = 1U;

	/* When */
	const int result = linkr_debugger_config_shell_apply(&test_shell,
						      ARRAY_SIZE_LOCAL(argv), argv);

	/* Then */
	assert(result == 0);
	assert_print_line("config apply applied_count=2 pending_count=1");
	assert(service.last_apply_confirmed);
	assert_service_calls((struct service_call_counts){.apply = 1U});
}

static void test_apply_rejects_invalid_grammar_before_service_io(void)
{
	static struct {
		size_t argc;
		char *argv[3];
	} cases[] = {
		{ 1U, { "apply", NULL, NULL } },
		{ 2U, { "apply", "confirm", NULL } },
		{ 3U, { "apply", "--confirm", "extra" } },
		{ 3U, { "apply", "--confirm", "--confirm" } },
	};

	for (size_t index = 0U; index < ARRAY_SIZE_LOCAL(cases); index++) {
		/* Given */
		reset_fixture();

		/* When */
		const int result = linkr_debugger_config_shell_apply(
			&test_shell, cases[index].argc, cases[index].argv);

		/* Then */
		assert(result == -EINVAL);
		assert_error_line("config apply error=invalid_arguments");
		assert_no_service_calls();
	}
}

static void test_apply_reports_partial_failure_in_report_order(void)
{
	char *argv[] = {"apply", "--confirm"};
	const char *const expected_lines[] = {
		"config apply error=apply_failed failed_id=switch/usb failed_errno=-17",
		"config apply applied_id=power/12v_out",
		"config apply applied_id=switch/sd",
		"config apply pending_id=switch/usb",
		"config apply pending_id=gpio/GP7",
	};

	/* Given */
	reset_fixture();
	service.apply_result = LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED;
	service.apply_report.failed_item = &item_switch_usb;
	service.apply_report.failed_errno = -17;
	service.apply_report.applied_count = 2U;
	service.apply_report.applied_items[0] = &item_power_12v;
	service.apply_report.applied_items[1] = &item_switch_sd;
	service.apply_report.pending_count = 2U;
	service.apply_report.pending_items[0] = &item_switch_usb;
	service.apply_report.pending_items[1] = &item_gpio_7;

	/* When */
	const int result = linkr_debugger_config_shell_apply(&test_shell,
						      ARRAY_SIZE_LOCAL(argv), argv);

	/* Then */
	assert(result == -17);
	assert_shell_output(FAKE_SHELL_LINE_ERROR, expected_lines,
			    ARRAY_SIZE_LOCAL(expected_lines));
	assert(service.last_apply_confirmed);
	assert_service_calls((struct service_call_counts){.apply = 1U});
}

static void test_apply_uses_io_error_for_nonnegative_failed_errno(void)
{
	char *argv[] = {"apply", "--confirm"};

	/* Given */
	reset_fixture();
	service.apply_result = LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED;
	service.apply_report.failed_errno = 0;

	/* When */
	const int result = linkr_debugger_config_shell_apply(&test_shell,
						      ARRAY_SIZE_LOCAL(argv), argv);

	/* Then */
	assert(result == -EIO);
	assert_error_line("config apply error=apply_failed failed_id=unknown failed_errno=0");
	assert_service_calls((struct service_call_counts){.apply = 1U});
}

static void test_clear_only_calls_clear_and_reports_hardware_unchanged(void)
{
	char *argv[] = {"clear"};

	/* Given */
	reset_fixture();

	/* When */
	const int result = linkr_debugger_config_shell_clear(&test_shell,
						      ARRAY_SIZE_LOCAL(argv), argv);

	/* Then */
	assert(result == 0);
	assert_print_line("config clear hardware_changed=false");
	assert_service_calls((struct service_call_counts){.clear = 1U});
}

static void test_clear_rejects_invalid_grammar_before_service_io(void)
{
	static struct {
		size_t argc;
		char *argv[2];
	} cases[] = {
		{ 0U, { "clear", NULL } },
		{ 2U, { "clear", "extra" } },
	};

	for (size_t index = 0U; index < ARRAY_SIZE_LOCAL(cases); index++) {
		/* Given */
		reset_fixture();

		/* When */
		const int result = linkr_debugger_config_shell_clear(
			&test_shell, cases[index].argc, cases[index].argv);

		/* Then */
		assert(result == -EINVAL);
		assert_error_line("config clear error=invalid_arguments");
		assert_no_service_calls();
	}
}

static void test_clear_maps_service_failure(void)
{
	char *argv[] = {"clear"};

	/* Given */
	reset_fixture();
	service.clear_result = LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE;

	/* When */
	const int result = linkr_debugger_config_shell_clear(&test_shell,
						      ARRAY_SIZE_LOCAL(argv), argv);

	/* Then */
	assert(result == -ENODEV);
	assert_error_line("config clear error=backend_unavailable");
	assert_service_calls((struct service_call_counts){.clear = 1U});
}

int main(void)
{
	test_show_prints_public_status_when_service_succeeds();
	test_show_rejects_invalid_grammar_before_service_io();
	test_show_maps_service_failure();
	test_save_preserves_ids_without_confirmation();
	test_save_accepts_confirmation_before_ids();
	test_save_accepts_confirmation_after_maximum_selection();
	test_save_rejects_syntax_before_service_io();
	test_save_rejects_too_many_ids_before_service_io();
	test_save_maps_every_non_apply_service_result();
	test_apply_prints_report_counts_when_service_succeeds();
	test_apply_rejects_invalid_grammar_before_service_io();
	test_apply_reports_partial_failure_in_report_order();
	test_apply_uses_io_error_for_nonnegative_failed_errno();
	test_clear_only_calls_clear_and_reports_hardware_unchanged();
	test_clear_rejects_invalid_grammar_before_service_io();
	test_clear_maps_service_failure();
	return 0;
}
