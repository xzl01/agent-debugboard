#include "linkr_debugger_config_http_result.h"

static void clear_error(struct linkr_debugger_config_http_error *error)
{
	if (error == NULL) {
		return;
	}

	error->status = HTTP_200_OK;
	error->code = NULL;
	error->message = NULL;
	error->activity = NULL;
}

static bool map_error(struct linkr_debugger_config_http_error *error,
		      enum http_status status, const char *code,
		      const char *message, const char *activity)
{
	if (error != NULL) {
		error->status = status;
		error->code = code;
		error->message = message;
		error->activity = activity;
	}
	return true;
}

static bool map_internal_error(struct linkr_debugger_config_http_error *error)
{
	return map_error(error, HTTP_500_INTERNAL_SERVER_ERROR, "internal_error",
			 "internal config error", NULL);
}

static bool map_busy(struct linkr_debugger_config_http_error *error,
		     const char *message, const char *activity)
{
	return map_error(error, HTTP_409_CONFLICT, "busy", message, activity);
}

bool linkr_debugger_config_http_map_service_result(
	enum linkr_debugger_config_http_action action,
	enum linkr_debugger_config_service_result result,
	struct linkr_debugger_config_http_error *error)
{
	switch (action) {
	case LINKR_DEBUGGER_CONFIG_HTTP_ACTION_GET:
		switch (result) {
		case LINKR_DEBUGGER_CONFIG_SERVICE_OK:
			clear_error(error);
			return false;
		case LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT:
		case LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION:
		case LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM:
		case LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM:
		case LINKR_DEBUGGER_CONFIG_SERVICE_ITEM_UNAVAILABLE:
		case LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED:
		case LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE:
		case LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_FLASH:
		case LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE:
		case LINKR_DEBUGGER_CONFIG_SERVICE_NO_SNAPSHOT:
		case LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT:
		case LINKR_DEBUGGER_CONFIG_SERVICE_UNSUPPORTED_VERSION:
		case LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR:
		case LINKR_DEBUGGER_CONFIG_SERVICE_CONTROL_CAPTURE_FAILED:
		case LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED:
		default:
			return map_internal_error(error);
		}

	case LINKR_DEBUGGER_CONFIG_HTTP_ACTION_SAVE:
		switch (result) {
		case LINKR_DEBUGGER_CONFIG_SERVICE_OK:
			clear_error(error);
			return false;
		case LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT:
			return map_internal_error(error);
		case LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION:
			return map_error(error, HTTP_400_BAD_REQUEST, "empty_selection",
					 "at least one config item is required", NULL);
		case LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM:
			return map_error(error, HTTP_400_BAD_REQUEST, "unknown_item",
					 "unknown config item", NULL);
		case LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM:
			return map_error(error, HTTP_400_BAD_REQUEST, "duplicate_item",
					 "duplicate config item", NULL);
		case LINKR_DEBUGGER_CONFIG_SERVICE_ITEM_UNAVAILABLE:
			return map_error(error, HTTP_409_CONFLICT, "item_unavailable",
					 "config item is unavailable", NULL);
		case LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED:
			return map_error(error, HTTP_409_CONFLICT, "confirmation_required",
					 "confirmation is required", NULL);
		case LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE:
			return map_busy(error, "configuration is blocked by active capture",
					"capture");
		case LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_FLASH:
			return map_busy(error, "configuration is blocked by active OTA", "ota");
		case LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE:
			return map_error(error, HTTP_500_INTERNAL_SERVER_ERROR,
					 "backend_unavailable", "config storage backend is unavailable", NULL);
		case LINKR_DEBUGGER_CONFIG_SERVICE_NO_SNAPSHOT:
			return map_internal_error(error);
		case LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT:
			return map_error(error, HTTP_500_INTERNAL_SERVER_ERROR, "invalid_snapshot",
					 "saved config snapshot is invalid", NULL);
		case LINKR_DEBUGGER_CONFIG_SERVICE_UNSUPPORTED_VERSION:
			return map_error(error, HTTP_500_INTERNAL_SERVER_ERROR,
					 "unsupported_version", "saved config snapshot version is unsupported", NULL);
		case LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR:
			return map_error(error, HTTP_500_INTERNAL_SERVER_ERROR,
					 "storage_write_failed", "failed to update config storage", NULL);
		case LINKR_DEBUGGER_CONFIG_SERVICE_CONTROL_CAPTURE_FAILED:
			return map_error(error, HTTP_500_INTERNAL_SERVER_ERROR,
					 "control_capture_failed", "failed to capture current control state", NULL);
		case LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED:
			return map_error(error, HTTP_500_INTERNAL_SERVER_ERROR, "apply_failed",
					 "failed to apply saved config", NULL);
		default:
			return map_internal_error(error);
		}

	case LINKR_DEBUGGER_CONFIG_HTTP_ACTION_CLEAR:
		switch (result) {
		case LINKR_DEBUGGER_CONFIG_SERVICE_OK:
			clear_error(error);
			return false;
		case LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_ARGUMENT:
		case LINKR_DEBUGGER_CONFIG_SERVICE_EMPTY_SELECTION:
		case LINKR_DEBUGGER_CONFIG_SERVICE_UNKNOWN_ITEM:
		case LINKR_DEBUGGER_CONFIG_SERVICE_DUPLICATE_ITEM:
		case LINKR_DEBUGGER_CONFIG_SERVICE_ITEM_UNAVAILABLE:
		case LINKR_DEBUGGER_CONFIG_SERVICE_CONFIRMATION_REQUIRED:
			return map_internal_error(error);
		case LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE:
			return map_busy(error, "configuration is blocked by active capture",
					"capture");
		case LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_FLASH:
			return map_busy(error, "configuration is blocked by active OTA", "ota");
		case LINKR_DEBUGGER_CONFIG_SERVICE_BACKEND_UNAVAILABLE:
			return map_error(error, HTTP_500_INTERNAL_SERVER_ERROR,
					 "backend_unavailable", "config storage backend is unavailable", NULL);
		case LINKR_DEBUGGER_CONFIG_SERVICE_NO_SNAPSHOT:
		case LINKR_DEBUGGER_CONFIG_SERVICE_INVALID_SNAPSHOT:
		case LINKR_DEBUGGER_CONFIG_SERVICE_UNSUPPORTED_VERSION:
		case LINKR_DEBUGGER_CONFIG_SERVICE_CONTROL_CAPTURE_FAILED:
		case LINKR_DEBUGGER_CONFIG_SERVICE_APPLY_FAILED:
			return map_internal_error(error);
		case LINKR_DEBUGGER_CONFIG_SERVICE_STORAGE_ERROR:
			return map_error(error, HTTP_500_INTERNAL_SERVER_ERROR,
					 "storage_write_failed", "failed to update config storage", NULL);
		default:
			return map_internal_error(error);
		}

	default:
		return map_internal_error(error);
	}
}

const char *linkr_debugger_config_http_reason_name(
	enum linkr_debugger_config_service_reason reason)
{
	switch (reason) {
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNINITIALIZED:
		return "uninitialized";
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_READY:
		return "ready";
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_ABSENT:
		return "absent";
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_BACKEND_UNAVAILABLE:
		return "backend_unavailable";
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_STORAGE_ERROR:
		return "storage_error";
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_INVALID_SNAPSHOT:
		return "invalid_snapshot";
	case LINKR_DEBUGGER_CONFIG_SERVICE_REASON_UNSUPPORTED_VERSION:
		return "unsupported_version";
	default:
		return NULL;
	}
}

const char *linkr_debugger_config_http_apply_state_name(
	enum linkr_debugger_config_apply_state state)
{
	switch (state) {
	case LINKR_DEBUGGER_CONFIG_APPLY_NOT_SAVED:
		return "not_saved";
	case LINKR_DEBUGGER_CONFIG_APPLY_APPLIED:
		return "applied";
	case LINKR_DEBUGGER_CONFIG_APPLY_PENDING:
		return "pending";
	case LINKR_DEBUGGER_CONFIG_APPLY_FAILED:
		return "failed";
	default:
		return NULL;
	}
}
