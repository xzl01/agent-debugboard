/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_power_json.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define POWER_JSON_SCRATCH 160U

static int power_json_safe_string(const char *text)
{
	if (text == NULL) {
		return 0;
	}
	for (const char *p = text; *p != '\0'; p++) {
		const char c = *p;

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
			continue;
		}
		return 0;
	}
	return 1;
}

static int power_json_emit(struct linkr_debugger_power_json_writer writer,
			   const char *fmt, ...)
{
	char scratch[POWER_JSON_SCRATCH];
	va_list args;
	int written;

	va_start(args, fmt);
	written = vsnprintf(scratch, sizeof(scratch), fmt, args);
	va_end(args);
	if (written < 0 || (size_t)written >= sizeof(scratch)) {
		return -1;
	}
	return writer.append(writer.context, scratch);
}

static int power_json_write_one(struct linkr_debugger_power_output_view view,
				enum linkr_debugger_power_json_detail detail,
				struct linkr_debugger_power_json_writer writer)
{
	const char *state;
	bool value_known = false;
	int value = 0;

	if (view.name == NULL || !power_json_safe_string(view.name) ||
	    !power_json_safe_string(view.signal) ||
	    !power_json_safe_string(view.desired) ||
	    !power_json_safe_string(view.requested) ||
	    !power_json_safe_string(view.effective) ||
	    !power_json_safe_string(view.backfeed)) {
		return -EINVAL;
	}

	if (view.desired != NULL) {
		/* Port VBUS: state mirrors effective verbatim and value stays
		 * null so a released port is never shown as a measured 0 V.
		 */
		state = view.effective != NULL ? view.effective : "off";
	} else if (view.locked) {
		state = "locked";
	} else {
		state = view.on ? "on" : "off";
		value_known = true;
		value = view.on ? 1 : 0;
	}

	if (detail == LINKR_DEBUGGER_POWER_JSON_BRIEF) {
		if (power_json_emit(writer, "{\"name\":\"%s\",\"state\":\"%s\",\"value\":",
				    view.name, state) < 0) {
			return -ENOMEM;
		}
	} else {
		if (power_json_emit(writer,
				    "{\"name\":\"%s\",\"signal\":\"%s\",\"gp\":%u,"
				    "\"controllable\":%s,\"state\":\"%s\",\"value\":",
				    view.name, view.signal != NULL ? view.signal : "",
				    view.gp, view.controllable ? "true" : "false",
				    state) < 0) {
			return -ENOMEM;
		}
	}

	if (value_known) {
		if (power_json_emit(writer, "%d", value) < 0) {
			return -ENOMEM;
		}
	} else if (writer.append(writer.context, "null") < 0) {
		return -ENOMEM;
	}

	if (view.desired != NULL) {
		if (detail == LINKR_DEBUGGER_POWER_JSON_FULL) {
			if (power_json_emit(writer,
					    ",\"role\":\"port_vbus\",\"measured\":false"
					    ",\"desired\":\"%s\",\"requested\":\"%s\""
					    ",\"effective\":\"%s\",\"backfeed\":\"%s\"",
					    view.desired,
					    view.requested != NULL ? view.requested : "off",
					    view.effective != NULL ? view.effective : "off",
					    view.backfeed != NULL ? view.backfeed :
					    "unknown") < 0) {
				return -ENOMEM;
			}
		} else if (power_json_emit(writer,
					   ",\"desired\":\"%s\",\"requested\":\"%s\""
					   ",\"effective\":\"%s\",\"backfeed\":\"%s\"",
					   view.desired,
					   view.requested != NULL ? view.requested : "off",
					   view.effective != NULL ? view.effective : "off",
					   view.backfeed != NULL ? view.backfeed :
					   "unknown") < 0) {
			return -ENOMEM;
		}
	}

	return writer.append(writer.context, "}") < 0 ? -12 : 0;
}

int linkr_debugger_power_outputs_json_write(
	const struct linkr_debugger_power_output_view *views, size_t count,
	enum linkr_debugger_power_json_detail detail,
	struct linkr_debugger_power_json_writer writer)
{
	if (writer.append == NULL || (views == NULL && count > 0U)) {
		return -EINVAL;
	}

	if (writer.append(writer.context, "[") < 0) {
		return -ENOMEM;
	}
	for (size_t i = 0U; i < count; i++) {
		int ret;

		if (i > 0U && writer.append(writer.context, ",") < 0) {
			return -ENOMEM;
		}
		ret = power_json_write_one(views[i], detail, writer);
		if (ret < 0) {
			return ret;
		}
	}
	return writer.append(writer.context, "]") < 0 ? -12 : 0;
}

static int power_json_count_append(void *context, const char *text)
{
	*(size_t *)context += strlen(text);
	return 0;
}

size_t linkr_debugger_power_outputs_json_worst_case_size(
	size_t count, enum linkr_debugger_power_json_detail detail)
{
	char name[LINKR_DEBUGGER_POWER_JSON_NAME_MAX + 1U];
	char signal[LINKR_DEBUGGER_POWER_JSON_SIGNAL_MAX + 1U];
	struct linkr_debugger_power_output_view worst = {
		.name = name,
		.signal = signal,
		.gp = 99U,
		.controllable = true,
		.desired = "off",
		.requested = "off",
		.effective = "blocked",
		.backfeed = "detected",
	};
	struct linkr_debugger_power_json_writer counter = {
		.append = power_json_count_append,
	};
	size_t entry = 0U;

	memset(name, 'n', LINKR_DEBUGGER_POWER_JSON_NAME_MAX);
	name[LINKR_DEBUGGER_POWER_JSON_NAME_MAX] = '\0';
	memset(signal, 's', LINKR_DEBUGGER_POWER_JSON_SIGNAL_MAX);
	signal[LINKR_DEBUGGER_POWER_JSON_SIGNAL_MAX] = '\0';
	counter.context = &entry;

	if (power_json_write_one(worst, detail, counter) < 0) {
		return (size_t)-1;
	}
	/* Array brackets plus one separator between entries. */
	return 2U + (count * entry) + (count > 0U ? count - 1U : 0U);
}
