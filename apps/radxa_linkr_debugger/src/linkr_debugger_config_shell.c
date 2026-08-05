#include "linkr_debugger_config_shell.h"

#include "linkr_debugger_config_summary.h"

#include <errno.h>
#include <string.h>

#include <zephyr/shell/shell.h>

static const char *linkr_debugger_config_shell_item_id(
	const struct linkr_debugger_config_item_desc *item)
{
	return item != NULL && item->id != NULL ? item->id : "unknown";
}

static int linkr_debugger_config_shell_syntax_error(const struct shell *sh,
					     const char *verb, const char *code)
{
	shell_error(sh, "config %s error=%s", verb, code);
	return -EINVAL;
}

static void linkr_debugger_config_shell_confirmation_ids(
	const struct shell *sh, const char *verb,
	const struct linkr_debugger_config_operation_report *report)
{
	if (report == NULL) {
		return;
	}
	for (size_t index = 0U; index < report->confirmation_count; index++) {
		shell_error(sh, "config %s confirmation_id=%s", verb,
			    linkr_debugger_config_shell_item_id(
				    report->confirmation_items[index]));
	}
}

static int linkr_debugger_config_shell_replay_failed(
	const struct shell *sh, const char *verb,
	const struct linkr_debugger_config_operation_report *report)
{
	const struct linkr_debugger_config_item_desc *failed_item =
		report == NULL ? NULL : report->failed_item;
	const int failed_errno = report == NULL ? 0 : report->failed_errno;

	shell_error(sh, "config %s error=apply_failed failed_id=%s failed_errno=%d",
		    verb, linkr_debugger_config_shell_item_id(failed_item), failed_errno);
	if (report != NULL) {
		for (size_t index = 0U; index < report->applied_count; index++) {
			shell_error(sh, "config %s applied_id=%s", verb,
				    linkr_debugger_config_shell_item_id(
					    report->applied_items[index]));
		}
		for (size_t index = 0U; index < report->pending_count; index++) {
			shell_error(sh, "config %s pending_id=%s", verb,
				    linkr_debugger_config_shell_item_id(
					    report->pending_items[index]));
		}
	}
	return failed_errno < 0 ? failed_errno : -EIO;
}

static int linkr_debugger_config_shell_service_error(
	const struct shell *sh, const char *verb,
	enum linkr_debugger_config_service_result result,
	const struct linkr_debugger_config_operation_report *report)
{
	const char *code = "invalid_argument";
	int error = -EINVAL;

	switch (result) {
	case LINKR_DEBUGGER_CONFIG_SERVICE_OK:
		return 0;
	case LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT:
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION:
		code = "empty_selection";
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM:
		code = "unknown_item";
		error = -ENOENT;
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM:
		code = "duplicate_item";
		error = -EEXIST;
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_ITEM_UNAVAILABLE:
		code = "item_unavailable";
		error = -ENODEV;
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED:
		shell_error(sh, "config %s error=confirmation_required", verb);
		linkr_debugger_config_shell_confirmation_ids(sh, verb, report);
		return -EACCES;
	case LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE:
		code = "busy activity=capture";
		error = -EBUSY;
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_FLASH:
		code = "busy activity=ota";
		error = -EBUSY;
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE:
		code = "backend_unavailable";
		error = -ENODEV;
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_NO_SNAPSHOT:
		code = "no_snapshot";
		error = -ENOENT;
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT:
		code = "invalid_snapshot";
		error = -EBADMSG;
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_UNSUPPORTED_VERSION:
		code = "unsupported_version";
		error = -ENOTSUP;
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR:
		code = "storage_error";
		error = -EIO;
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_CONTROL_CAPTURE_FAILED:
		code = "control_capture_failed";
		error = -EIO;
		break;
	case LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED:
		return linkr_debugger_config_shell_replay_failed(sh, verb, report);
	default:
		break;
	}

	shell_error(sh, "config %s error=%s", verb, code);
	return error;
}

int linkr_debugger_config_shell_show(const struct shell *sh, size_t argc, char **argv)
{
	struct linkr_debugger_config_service_status status;
	struct linkr_debugger_config_summary summary;
	enum linkr_debugger_config_service_result result;

	(void)argv;
	if (argc != 1U) {
		return linkr_debugger_config_shell_syntax_error(sh, "show",
							       "invalid_arguments");
	}
	result = linkr_debugger_config_service_status_get(&status);
	if (result != LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		return linkr_debugger_config_shell_service_error(sh, "show", result, NULL);
	}
	if (!linkr_debugger_config_summary_from_status(&status, &summary)) {
		shell_error(sh, "config show error=invalid_status");
		return -EIO;
	}
	shell_print(sh, "config available=%s reason=%s saved_count=%zu pending_count=%zu",
		    summary.available ? "true" : "false", summary.reason,
		    summary.saved_count, summary.pending_count);
	return 0;
}

int linkr_debugger_config_shell_save(const struct shell *sh, size_t argc, char **argv)
{
	struct linkr_debugger_config_save_request request = {0};
	struct linkr_debugger_config_operation_report report = {0};
	enum linkr_debugger_config_service_result result;

	for (size_t index = 1U; index < argc; index++) {
		if (strcmp(argv[index], "--confirm") == 0) {
			if (request.confirmed) {
				return linkr_debugger_config_shell_syntax_error(
					sh, "save", "duplicate_confirm");
			}
			request.confirmed = true;
			continue;
		}
		if (argv[index][0] == '-') {
			return linkr_debugger_config_shell_syntax_error(
				sh, "save", "unknown_option");
		}
		if (request.item_count == LINKR_DEBUGGER_CONFIG_MAX_ENTRIES) {
			shell_error(sh, "config save error=too_many_items max=%u",
				    LINKR_DEBUGGER_CONFIG_MAX_ENTRIES);
			return -E2BIG;
		}
		request.item_ids[request.item_count++] = argv[index];
	}
	if (request.item_count == 0U) {
		return linkr_debugger_config_shell_syntax_error(sh, "save",
							       "empty_selection");
	}
	result = linkr_debugger_config_service_save(&request, &report);
	if (result != LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		return linkr_debugger_config_shell_service_error(sh, "save", result, &report);
	}
	shell_print(sh, "config save saved_count=%zu pending_count=%zu",
		    request.item_count, report.pending_count);
	return 0;
}

int linkr_debugger_config_shell_clear(const struct shell *sh, size_t argc, char **argv)
{
	enum linkr_debugger_config_service_result result;

	(void)argv;
	if (argc != 1U) {
		return linkr_debugger_config_shell_syntax_error(sh, "clear",
							       "invalid_arguments");
	}
	result = linkr_debugger_config_service_clear();
	if (result != LINKR_DEBUGGER_CONFIG_SERVICE_OK) {
		return linkr_debugger_config_shell_service_error(sh, "clear", result, NULL);
	}
	shell_print(sh, "config clear hardware_changed=false");
	return 0;
}
