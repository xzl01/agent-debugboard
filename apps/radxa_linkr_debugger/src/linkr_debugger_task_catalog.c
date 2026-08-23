/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 */

#include "linkr_debugger_task_catalog.h"

#include "linkr_debugger_http_task_response.h"
#include "linkr_debugger_task_parse.h"

#define TASK_CATALOG_GPIO_PATH "/api/v1/gpio/CON_MAS"
#define TASK_CATALOG_GPIO_INPUT_BODY "{\"direction\":\"input\"}"
#define TASK_CATALOG_GPIO_LOW_BODY "{\"direction\":\"output\",\"value\":0}"
#define TASK_CATALOG_GPIO_HIGH_BODY "{\"direction\":\"output\",\"value\":1}"
#define TASK_CATALOG_POWER_5V_PATH "/api/v1/power/5v_out"
#define TASK_CATALOG_POWER_12V_PATH "/api/v1/power/12v_out"
#define TASK_CATALOG_POWER_20V_PATH "/api/v1/power/20v_out"
#define TASK_CATALOG_POWER_OFF_BODY "{\"state\":\"off\"}"
#define TASK_CATALOG_POWER_ON_BODY "{\"state\":\"on\"}"

#define TASK_CATALOG_INPUT_REQUEST \
	"{\"method\":\"PUT\",\"path\":\"" TASK_CATALOG_GPIO_PATH \
	"\",\"body\":" TASK_CATALOG_GPIO_INPUT_BODY ",\"wait_ms\":0}"
#define TASK_CATALOG_LOW_REQUEST \
	"{\"method\":\"PUT\",\"path\":\"" TASK_CATALOG_GPIO_PATH \
	"\",\"body\":" TASK_CATALOG_GPIO_LOW_BODY ",\"wait_ms\":20}"
#define TASK_CATALOG_HIGH_REQUEST \
	"{\"method\":\"PUT\",\"path\":\"" TASK_CATALOG_GPIO_PATH \
	"\",\"body\":" TASK_CATALOG_GPIO_HIGH_BODY ",\"wait_ms\":20}"
#define TASK_CATALOG_POWER_OFF_REQUEST(path_) \
	"{\"method\":\"PUT\",\"path\":\"" path_ \
	"\",\"body\":" TASK_CATALOG_POWER_OFF_BODY ",\"wait_ms\":1000}"
#define TASK_CATALOG_POWER_ON_REQUEST(path_) \
	"{\"method\":\"PUT\",\"path\":\"" path_ \
	"\",\"body\":" TASK_CATALOG_POWER_ON_BODY ",\"wait_ms\":500}"
#define TASK_CATALOG_TASK(id_, name_, rail_path_, boot_request_) \
	"{\"id\":\"" id_ "\",\"name\":\"" name_ "\",\"requests\":[" \
	TASK_CATALOG_INPUT_REQUEST "," \
	TASK_CATALOG_POWER_OFF_REQUEST(rail_path_) "," \
	boot_request_ "," \
	TASK_CATALOG_POWER_ON_REQUEST(rail_path_) "," \
	TASK_CATALOG_INPUT_REQUEST \
	"],\"cleanup\":" TASK_CATALOG_INPUT_REQUEST "}"

_Static_assert(sizeof(TASK_CATALOG_GPIO_PATH) - 1U <= LINKR_DEBUGGER_TASK_MAX_PATH_LEN,
	       "catalog GPIO path exceeds task path limit");
_Static_assert(sizeof(TASK_CATALOG_POWER_5V_PATH) - 1U <= LINKR_DEBUGGER_TASK_MAX_PATH_LEN,
	       "catalog 5V path exceeds task path limit");
_Static_assert(sizeof(TASK_CATALOG_POWER_12V_PATH) - 1U <= LINKR_DEBUGGER_TASK_MAX_PATH_LEN,
	       "catalog 12V path exceeds task path limit");
_Static_assert(sizeof(TASK_CATALOG_POWER_20V_PATH) - 1U <= LINKR_DEBUGGER_TASK_MAX_PATH_LEN,
	       "catalog 20V path exceeds task path limit");
_Static_assert(sizeof(TASK_CATALOG_GPIO_INPUT_BODY) - 1U <= LINKR_DEBUGGER_TASK_MAX_BODY_LEN,
	       "catalog input body exceeds task body limit");
_Static_assert(sizeof(TASK_CATALOG_GPIO_LOW_BODY) - 1U <= LINKR_DEBUGGER_TASK_MAX_BODY_LEN,
	       "catalog low body exceeds task body limit");
_Static_assert(sizeof(TASK_CATALOG_GPIO_HIGH_BODY) - 1U <= LINKR_DEBUGGER_TASK_MAX_BODY_LEN,
	       "catalog high body exceeds task body limit");
_Static_assert(sizeof(TASK_CATALOG_POWER_OFF_BODY) - 1U <= LINKR_DEBUGGER_TASK_MAX_BODY_LEN,
	       "catalog power-off body exceeds task body limit");
_Static_assert(sizeof(TASK_CATALOG_POWER_ON_BODY) - 1U <= LINKR_DEBUGGER_TASK_MAX_BODY_LEN,
	       "catalog power-on body exceeds task body limit");

static const char task_catalog_json[] =
	"{\"schema\":\"radxa-linkr-debugger.v1\",\"ok\":true,\"command\":\"task\","
	"\"action\":\"catalog\",\"version\":1,\"tasks\":["
	TASK_CATALOG_TASK("builtin/maskrom/5v_out", "Rockchip MASKROM via 5v_out",
			  TASK_CATALOG_POWER_5V_PATH, TASK_CATALOG_LOW_REQUEST) ","
	TASK_CATALOG_TASK("builtin/maskrom/12v_out", "Rockchip MASKROM via 12v_out",
			  TASK_CATALOG_POWER_12V_PATH, TASK_CATALOG_LOW_REQUEST) ","
	TASK_CATALOG_TASK("builtin/maskrom/20v_out", "Rockchip MASKROM via 20v_out",
			  TASK_CATALOG_POWER_20V_PATH, TASK_CATALOG_LOW_REQUEST) ","
	TASK_CATALOG_TASK("builtin/edl/5v_out", "Qualcomm EDL via 5v_out",
			  TASK_CATALOG_POWER_5V_PATH, TASK_CATALOG_HIGH_REQUEST) ","
	TASK_CATALOG_TASK("builtin/edl/12v_out", "Qualcomm EDL via 12v_out",
			  TASK_CATALOG_POWER_12V_PATH, TASK_CATALOG_HIGH_REQUEST) ","
	TASK_CATALOG_TASK("builtin/edl/20v_out", "Qualcomm EDL via 20v_out",
			  TASK_CATALOG_POWER_20V_PATH, TASK_CATALOG_HIGH_REQUEST) "]}\n";

_Static_assert(sizeof(task_catalog_json) - 1U <= LINKR_DEBUGGER_TASK_HTTP_RESPONSE_CAP,
	       "catalog response exceeds task HTTP response limit");

const uint8_t *linkr_debugger_task_catalog_json(size_t *body_len)
{
	if (body_len != NULL) {
		*body_len = sizeof(task_catalog_json) - 1U;
	}

	return (const uint8_t *)task_catalog_json;
}
