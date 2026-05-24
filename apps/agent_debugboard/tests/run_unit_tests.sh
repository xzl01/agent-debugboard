#!/bin/sh
# SPDX-License-Identifier: Apache-2.0

set -eu

SCRIPT_DIR="$(dirname -- "$0")"
ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT="${ROOT}/build/agent_debugboard_unit"

mkdir -p "${OUT}"

cc -std=c11 -Wall -Wextra -Werror \
	-I"${ROOT}/apps/agent_debugboard/src" \
	"${ROOT}/apps/agent_debugboard/src/debugboard_model.c" \
	"${ROOT}/apps/agent_debugboard/tests/model_host/test_debugboard_model.c" \
	-o "${OUT}/debugboard_model_test"

"${OUT}/debugboard_model_test"
# Keep Go test package discovery inside real source directories only. Running
# `go test ./...` from the repo root can recurse into Zephyr/CMake build output
# trees under build/, which are not Go packages and can break discovery.
(cd "${ROOT}" && go test ./cmd/... ./internal/...)
