#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
# Copyright (c) xzl <xiangzelong@radxa.com>
# Copyright (c) Jiali Chen <chenjiali@radxa.com>

set -eu

CC="${CC:-cc}"

SCRIPT_DIR="$(dirname -- "$0")"
ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT="${ROOT}/build/radxa_linkr_debugger_unit"

mkdir -p "${OUT}"

HOST_STUB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/radxa-linkr-debugger-unit.XXXXXX")"
trap 'rm -rf "${HOST_STUB_DIR}"' EXIT
mkdir -p "${HOST_STUB_DIR}/zephyr/drivers"
cat > "${HOST_STUB_DIR}/zephyr/drivers/sensor.h" <<'EOF'
#ifndef LINKR_DEBUGGER_UNIT_SENSOR_H_
#define LINKR_DEBUGGER_UNIT_SENSOR_H_

#include <stdint.h>

struct sensor_value {
	int32_t val1;
	int32_t val2;
};

#endif
EOF

${CC} -std=c11 -Wall -Wextra -Werror \
	-DCONFIG_SOC_SERIES_RP2350 \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_model.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_model.c" \
	-o "${OUT}/linkr_debugger_model_rp2350_test"

"${OUT}/linkr_debugger_model_rp2350_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_CONFIG_CODEC_HOST_TEST \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_codec.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_config_codec.c" \
	-o "${OUT}/linkr_debugger_config_codec_test"

"${OUT}/linkr_debugger_config_codec_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_CONFIG_CODEC_HOST_TEST \
	-DLINKR_DEBUGGER_CONFIG_STORE_HOST_TEST \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_codec.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_store.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_config_store.c" \
	-o "${OUT}/linkr_debugger_config_store_test"

"${OUT}/linkr_debugger_config_store_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_CONFIG_CODEC_HOST_TEST \
	-DLINKR_DEBUGGER_CONFIG_SERVICE_HOST_TEST \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"${HOST_STUB_DIR}" \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_codec.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_policy.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_config_policy.c" \
	-o "${OUT}/linkr_debugger_config_policy_test"

"${OUT}/linkr_debugger_config_policy_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_CONFIG_CODEC_HOST_TEST \
	-DLINKR_DEBUGGER_CONFIG_SERVICE_HOST_TEST \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"${HOST_STUB_DIR}" \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_codec.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_replay.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_config_replay.c" \
	-o "${OUT}/linkr_debugger_config_replay_test"

"${OUT}/linkr_debugger_config_replay_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_CONFIG_CODEC_HOST_TEST \
	-DLINKR_DEBUGGER_CONFIG_SERVICE_HOST_TEST \
	-pthread \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"${HOST_STUB_DIR}" \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_codec.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_policy.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_replay.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_service_state.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_service.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_config_service.c" \
	-o "${OUT}/linkr_debugger_config_service_test"

"${OUT}/linkr_debugger_config_service_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_CONFIG_HTTP_HOST_TEST=1 \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-include "${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_config_http_stubs.h" \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_config_http_codec.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_codec.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_http_parse.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_http_json.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_http_encode.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_http_result.c" \
	-o "${OUT}/linkr_debugger_config_http_codec_test"

"${OUT}/linkr_debugger_config_http_codec_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_CONFIG_HTTP_HOST_TEST=1 \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-include "${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_config_http_stubs.h" \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_config_http.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_codec.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_http_body.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_http_parse.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_http_json.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_http_encode.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_http_result.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_http.c" \
	-o "${OUT}/linkr_debugger_config_http_test"

"${OUT}/linkr_debugger_config_http_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_summary.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_config_summary.c" \
	-o "${OUT}/linkr_debugger_config_summary_test"

"${OUT}/linkr_debugger_config_summary_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all \
	-I"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/stubs" \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_summary.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_config_shell.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_config_shell.c" \
	-o "${OUT}/linkr_debugger_config_shell_test"

"${OUT}/linkr_debugger_config_shell_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_OTA_HOST_TEST \
	-DLINKR_DEBUGGER_OTA_FULL_HOST_TEST \
	-DLINKR_DEBUGGER_FLASH_ARBITER_HOST_TEST \
	-pthread \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-include "${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_ota_stubs.h" \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_ota.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_flash_arbiter.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_ota.c" \
	-o "${OUT}/linkr_debugger_ota_parser_test"

"${OUT}/linkr_debugger_ota_parser_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_CAPTIVE_PORTAL_HOST_TEST \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_captive_portal.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_portal.c" \
	-o "${OUT}/linkr_debugger_portal_test"

"${OUT}/linkr_debugger_portal_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_http_body.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_http_body.c" \
	-o "${OUT}/linkr_debugger_http_body_test"

"${OUT}/linkr_debugger_http_body_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_gpio_error.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_gpio_error.c" \
	-o "${OUT}/linkr_debugger_gpio_error_test"

"${OUT}/linkr_debugger_gpio_error_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_CAPTURE_ARENA_HOST_TEST \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_capture_arena.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_capture_arena.c" \
	-o "${OUT}/linkr_debugger_capture_arena_test"

"${OUT}/linkr_debugger_capture_arena_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_capture_engine.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_capture_engine.c" \
	-o "${OUT}/linkr_debugger_capture_engine_test"

"${OUT}/linkr_debugger_capture_engine_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_capture_gpio_guard.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_capture_gpio_guard.c" \
	-o "${OUT}/linkr_debugger_capture_gpio_guard_test"

"${OUT}/linkr_debugger_capture_gpio_guard_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_ws_sampler_sync.c" \
	-o "${OUT}/linkr_debugger_ws_sampler_sync_test"

"${OUT}/linkr_debugger_ws_sampler_sync_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_TASK_HOST_TEST \
	-pthread \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"${HOST_STUB_DIR}" \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_json_cursor.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_json_value.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_task_parse.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_task.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_task.c" \
	-o "${OUT}/linkr_debugger_task_test"

"${OUT}/linkr_debugger_task_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-Wframe-larger-than=256 \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	-fsyntax-only \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_json_value.c"

${CC} -std=c11 -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_task_catalog.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_json_cursor.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_json_value.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_task_catalog.c" \
	-o "${OUT}/linkr_debugger_task_catalog_test"

"${OUT}/linkr_debugger_task_catalog_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_http_task_response.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_http_task_response.c" \
	-o "${OUT}/linkr_debugger_http_task_response_test"

"${OUT}/linkr_debugger_http_task_response_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-Wframe-larger-than=1024 \
	-DLINKR_DEBUGGER_TASK_HOST_TEST \
	-DLINKR_DEBUGGER_TASK_HTTP_HOST_TEST=1 \
	-include "${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_task_http_stubs.h" \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	-fsyntax-only \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_task_catalog.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_task_http.c"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_TASK_HOST_TEST \
	-DLINKR_DEBUGGER_TASK_HTTP_HOST_TEST=1 \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-include "${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_task_http_stubs.h" \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_task_http.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_task_catalog.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_task_http.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_json_cursor.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_json_value.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_task_parse.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_task.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_task_mutation.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_http_task_response.c" \
	-o "${OUT}/linkr_debugger_task_http_test"

"${OUT}/linkr_debugger_task_http_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_TASK_HOST_TEST \
	-DLINKR_DEBUGGER_CAPTURE_ARBITER_HOST_TEST \
	-DLINKR_DEBUGGER_FLASH_ARBITER_HOST_TEST \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"${ROOT}/apps/radxa_linkr_debugger/tests/model_host" \
	-I"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/stubs" \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_task_shell.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_task_shell.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_json_cursor.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_json_value.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_task_parse.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_task.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_task_mutation.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_capture_arbiter.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_flash_arbiter.c" \
	-o "${OUT}/linkr_debugger_task_shell_test"

"${OUT}/linkr_debugger_task_shell_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_CAPTURE_ARBITER_HOST_TEST \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_capture_arbiter.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_capture_arbiter.c" \
	-o "${OUT}/linkr_debugger_capture_arbiter_test"

"${OUT}/linkr_debugger_capture_arbiter_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_FLASH_ARBITER_HOST_TEST \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_flash_arbiter.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_flash_arbiter.c" \
	-o "${OUT}/linkr_debugger_flash_arbiter_test"

"${OUT}/linkr_debugger_flash_arbiter_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_LA_HOST_TEST \
	-DCONFIG_SOC_SERIES_RP2350 \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_logic_analyzer.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_logic_analyzer.c" \
	-o "${OUT}/linkr_debugger_logic_analyzer_test"

"${OUT}/linkr_debugger_logic_analyzer_test"

${CC} -std=c11 -Wall -Wextra -Werror \
	-DLINKR_DEBUGGER_CAPTURE_ARBITER_HOST_TEST \
	-DLINKR_DEBUGGER_CAPTURE_ARENA_HOST_TEST \
	-DLINKR_DEBUGGER_LA_HOST_TEST \
	-DLINKR_DEBUGGER_SIGROK_LINKR_HOST_TEST \
	-DCONFIG_SOC_SERIES_RP2350 \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_capture_arbiter.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_capture_arena.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_logic_analyzer.c" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_sigrok_linkr.c" \
	-o "${OUT}/linkr_debugger_sigrok_linkr_test"

"${OUT}/linkr_debugger_sigrok_linkr_test"

python3 "${ROOT}/apps/radxa_linkr_debugger/tests/test_logic_analyzer_hil_perf.py"
