#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
# Copyright (c) xzl <xiangzelong@radxa.com>
# Copyright (c) Jiali Chen <chenjiali@radxa.com>

set -eu

SCRIPT_DIR="$(dirname -- "$0")"
ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT="${ROOT}/build/radxa_linkr_debugger_unit"

mkdir -p "${OUT}"

cc -std=c11 -Wall -Wextra -Werror \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_model.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_model.c" \
	-o "${OUT}/linkr_debugger_model_rp2040_test"

"${OUT}/linkr_debugger_model_rp2040_test"

cc -std=c11 -Wall -Wextra -Werror \
	-DCONFIG_SOC_SERIES_RP2350 \
	-I"${ROOT}/apps/radxa_linkr_debugger/src" \
	"${ROOT}/apps/radxa_linkr_debugger/src/linkr_debugger_model.c" \
	"${ROOT}/apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_model.c" \
	-o "${OUT}/linkr_debugger_model_rp2350_test"

"${OUT}/linkr_debugger_model_rp2350_test"
