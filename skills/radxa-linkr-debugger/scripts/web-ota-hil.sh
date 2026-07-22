#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later

set -eu

DEFAULT_BOARD_URL="http://172.29.203.1"
DEFAULT_IMAGE="build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin"
DEFAULT_UF2="radxa-linkr-debugger-rp2350.uf2"

BOARD_URL="$DEFAULT_BOARD_URL"
IMAGE="$DEFAULT_IMAGE"
UF2="$DEFAULT_UF2"
TTY=""
FLOW="preflight"
DRY_RUN=1
ALLOW_UPLOAD_TEST_REBOOT=0
ALLOW_BOOTSEL=0
ALLOW_FLASH=0
SHORT_TIMEOUT="5s"
UPLOAD_TIMEOUT="90s"
POLL_TIMEOUT=45

usage() {
  cat <<'USAGE'
Usage: web-ota-hil.sh [options]

Repository-local Web OTA HIL runner. Defaults to safe preflight/read-only or dry-run behavior.

Options:
  --flow NAME                  preflight, status, api-auto-confirm, api-manual-confirm,
                               negative-upload, watchdog-rollback, bootsel-http,
                               bootsel-cdc, flash-uf2, all (dry-run only)
  --board-url URL              Board base URL. Default: http://172.29.203.1
  --image PATH                 OTA .bin payload. Default: build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin
  --uf2 PATH                   Combined recovery UF2. Default: radxa-linkr-debugger-rp2350.uf2
  --tty PATH                   CDC ACM tty for --flow bootsel-cdc
  --execute                    Execute side-effectful upload/test/reboot/BOOTSEL actions
  --allow-upload-test-reboot   Required with --execute for OTA upload/test/confirm flows
  --allow-bootsel              Required with --execute for HTTP/CDC BOOTSEL entry
  --allow-flash                Required with --execute for UF2 copy; this is separate from BOOTSEL permission
  --dry-run                    Print command plan only for side-effectful flows (default)
  --help                       Show this help

Preflight and status perform only bounded GET requests. Upload, test boot, BOOTSEL,
and UF2 copy never run unless the matching explicit gates are present.
Watchdog rollback is reported as BLOCKED because no safe fault-injection path exists.
USAGE
}

fail() {
  echo "ERROR: $*" >&2
  exit 1
}

note() {
  echo "[web-ota-hil] $*"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --flow)
      [ "$#" -ge 2 ] || fail "--flow requires a value"
      FLOW="$2"
      shift 2
      ;;
    --board-url)
      [ "$#" -ge 2 ] || fail "--board-url requires a value"
      BOARD_URL="${2%/}"
      shift 2
      ;;
    --image)
      [ "$#" -ge 2 ] || fail "--image requires a value"
      IMAGE="$2"
      shift 2
      ;;
    --uf2)
      [ "$#" -ge 2 ] || fail "--uf2 requires a value"
      UF2="$2"
      shift 2
      ;;
    --tty)
      [ "$#" -ge 2 ] || fail "--tty requires a value"
      TTY="$2"
      shift 2
      ;;
    --execute)
      DRY_RUN=0
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --allow-upload-test-reboot)
      ALLOW_UPLOAD_TEST_REBOOT=1
      shift
      ;;
    --allow-bootsel)
      ALLOW_BOOTSEL=1
      shift
      ;;
    --allow-flash)
      ALLOW_FLASH=1
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      fail "unknown argument: $1"
      ;;
  esac
done

api_url() {
  printf '%s/api/v1%s' "$BOARD_URL" "$1"
}

run_timeout() {
  limit="$1"
  shift
  if command -v timeout >/dev/null 2>&1; then
    timeout "$limit" "$@"
  else
    "$@"
  fi
}

quote_arg() {
  printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\''/g")"
}

plan() {
  printf 'DRY-RUN: '
  for arg in "$@"; do
    quote_arg "$arg"
    printf ' '
  done
  printf '\n'
}

maybe_run() {
  if [ "$DRY_RUN" -eq 1 ]; then
    plan "$@"
    return 0
  fi
  "$@"
}

require_file() {
  path="$1"
  label="$2"
  [ -f "$path" ] || fail "$label not found: $path"
  [ -s "$path" ] || fail "$label is empty: $path"
}

require_ota_image() {
  require_file "$IMAGE" "OTA image"
  case "$IMAGE" in
    *.bin) ;;
    *) fail "OTA image must be a MCUboot .bin file, got: $IMAGE" ;;
  esac
}

require_uf2_image() {
  require_file "$UF2" "UF2 image"
  case "$UF2" in
    *.uf2) ;;
    *) fail "UF2 image must end in .uf2, got: $UF2" ;;
  esac
}

require_tty() {
  [ -n "$TTY" ] || fail "--tty is required for CDC BOOTSEL"
  [ -e "$TTY" ] || fail "CDC tty not found: $TTY"
}

require_upload_gate() {
  [ "$DRY_RUN" -eq 1 ] && return 0
  [ "$ALLOW_UPLOAD_TEST_REBOOT" -eq 1 ] || fail "--allow-upload-test-reboot is required with --execute for OTA upload/test/reboot flows"
}

require_bootsel_gate() {
  [ "$DRY_RUN" -eq 1 ] && return 0
  [ "$ALLOW_BOOTSEL" -eq 1 ] || fail "--allow-bootsel is required with --execute for BOOTSEL entry"
}

require_flash_gate() {
  [ "$DRY_RUN" -eq 1 ] && return 0
  [ "$ALLOW_FLASH" -eq 1 ] || fail "--allow-flash is required with --execute for UF2 copy"
}

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
    return
  fi
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
    return
  fi
  fail "sha256sum or shasum is required"
}

file_size() {
  if size=$(stat -c %s "$1" 2>/dev/null); then
    printf '%s\n' "$size"
    return
  fi
  wc -c < "$1" | tr -d ' '
}

curl_get() {
  run_timeout "$SHORT_TIMEOUT" curl -fsS "$(api_url "$1")"
}

preflight() {
  note "read-only preflight against $BOARD_URL"
  if [ "$DRY_RUN" -eq 1 ]; then
    plan run_timeout "$SHORT_TIMEOUT" curl -fsS "$(api_url /status)"
    plan run_timeout "$SHORT_TIMEOUT" curl -fsS "$(api_url /watchdog)"
    plan run_timeout "$SHORT_TIMEOUT" curl -fsS "$(api_url /ota)"
    return
  fi
  command -v curl >/dev/null 2>&1 || fail "curl is required"
  curl_get /status
  curl_get /watchdog
  curl_get /ota
}

status_only() {
  note "read-only OTA status"
  if [ "$DRY_RUN" -eq 1 ]; then
    plan run_timeout "$SHORT_TIMEOUT" curl -fsS "$(api_url /ota)"
    return
  fi
  command -v curl >/dev/null 2>&1 || fail "curl is required"
  curl_get /ota
}

upload_valid_image() {
  [ "$DRY_RUN" -eq 1 ] || require_ota_image
  require_upload_gate
  if [ "$DRY_RUN" -eq 1 ]; then
    sha="<sha256($IMAGE)>"
    size="<size($IMAGE)>"
  else
    sha=$(sha256_file "$IMAGE")
    size=$(file_size "$IMAGE")
  fi
  maybe_run run_timeout "$UPLOAD_TIMEOUT" curl -fsS -X POST \
    -H "Content-Type: application/octet-stream" \
    -H "X-Linkr-Ota-Size: $size" \
    -H "X-Linkr-Ota-Sha256: $sha" \
    --data-binary "@$IMAGE" \
    "$(api_url /ota/upload)"
}

post_ota_test() {
  require_upload_gate
  maybe_run run_timeout "$SHORT_TIMEOUT" curl -fsS -X POST "$(api_url /ota/test)"
}

post_ota_confirm() {
  require_upload_gate
  maybe_run run_timeout "$SHORT_TIMEOUT" curl -fsS -X POST "$(api_url /ota/confirm)"
}

wait_for_ota() {
  expected="$1"
  deadline=$((POLL_TIMEOUT * 2))
  count=0
  note "polling OTA state for $expected"
  while [ "$count" -lt "$deadline" ]; do
    body=$(run_timeout "$SHORT_TIMEOUT" curl -fsS "$(api_url /ota)" 2>/dev/null || true)
    printf '%s\n' "$body"
    printf '%s\n' "$body" | grep -q "$expected" && return 0
    sleep 0.5
    count=$((count + 1))
  done
  fail "timed out waiting for OTA state containing: $expected"
}

api_auto_confirm() {
  upload_valid_image
  post_ota_test
  if [ "$DRY_RUN" -eq 1 ]; then
    plan run_timeout "$SHORT_TIMEOUT" curl -fsS "$(api_url /ota)" "# repeat until pending_test, then idle/current_image_confirmed=true"
    return
  fi
  wait_for_ota 'pending_test'
  wait_for_ota 'current_image_confirmed.*true\|"current_image_confirmed":true'
}

api_manual_confirm() {
  upload_valid_image
  post_ota_test
  if [ "$DRY_RUN" -eq 1 ]; then
    plan run_timeout "$SHORT_TIMEOUT" curl -fsS "$(api_url /ota)" "# repeat until pending_test"
  else
    wait_for_ota 'pending_test'
  fi
  post_ota_confirm
  [ "$DRY_RUN" -eq 1 ] || wait_for_ota 'current_image_confirmed.*true\|"current_image_confirmed":true'
}

negative_upload() {
  [ "$DRY_RUN" -eq 1 ] || require_ota_image
  require_upload_gate
  if [ "$DRY_RUN" -eq 1 ]; then
    sha="<sha256($IMAGE)>"
    size="<size($IMAGE)>"
  else
    sha=$(sha256_file "$IMAGE")
    size=$(file_size "$IMAGE")
  fi
  bad_sha="0000000000000000000000000000000000000000000000000000000000000000"
  note "negative upload: SHA256 mismatch should return firmware JSON error"
  if [ "$DRY_RUN" -eq 1 ]; then
    plan run_timeout "$SHORT_TIMEOUT" curl -sS -o /tmp/linkr-ota-bad-sha.json -w '%{http_code}' -X POST \
      -H "Content-Type: application/octet-stream" \
      -H "X-Linkr-Ota-Size: $size" \
      -H "X-Linkr-Ota-Sha256: $bad_sha" \
      --data-binary "@$IMAGE" \
      "$(api_url /ota/upload)" "# expect HTTP 400 and error.code sha256_mismatch"
  else
    bad_sha_body=$(mktemp "${TMPDIR:-/tmp}/linkr-ota-bad-sha.XXXXXX") || fail "mktemp failed"
    bad_type_body=$(mktemp "${TMPDIR:-/tmp}/linkr-ota-bad-type.XXXXXX") || fail "mktemp failed"
    trap 'rm -f "$bad_sha_body" "$bad_type_body"' EXIT HUP INT TERM
    bad_sha_http=$(run_timeout "$SHORT_TIMEOUT" curl -sS -o "$bad_sha_body" -w '%{http_code}' -X POST \
      -H "Content-Type: application/octet-stream" \
      -H "X-Linkr-Ota-Size: $size" \
      -H "X-Linkr-Ota-Sha256: $bad_sha" \
      --data-binary "@$IMAGE" \
      "$(api_url /ota/upload)") || {
        echo "bad SHA upload transport failed; response body follows:" >&2
        cat "$bad_sha_body" >&2 || true
        fail "bad SHA upload transport failed"
      }
    assert_http_error "$bad_sha_http" "$bad_sha_body" 400 sha256_mismatch "bad SHA upload"
  fi
  note "negative upload: unsupported content type should preserve first error"
  if [ "$DRY_RUN" -eq 1 ]; then
    plan run_timeout "30s" curl -sS -o /tmp/linkr-ota-bad-type.json -w '%{http_code}' -X POST \
      -H "Content-Type: text/plain" \
      -H "X-Linkr-Ota-Size: $size" \
      -H "X-Linkr-Ota-Sha256: $sha" \
      --data-binary "@$IMAGE" \
      "$(api_url /ota/upload)" "# expect HTTP 415 and error.code unsupported_content_type"
    plan run_timeout "$SHORT_TIMEOUT" curl -fsS "$(api_url /ota)" "# verify last_error.code remains unsupported_content_type"
  else
    bad_type_http=$(run_timeout "30s" curl -sS -o "$bad_type_body" -w '%{http_code}' -X POST \
      -H "Content-Type: text/plain" \
      -H "X-Linkr-Ota-Size: $size" \
      -H "X-Linkr-Ota-Sha256: $sha" \
      --data-binary "@$IMAGE" \
      "$(api_url /ota/upload)") || {
        echo "bad Content-Type upload transport failed; response body follows:" >&2
        cat "$bad_type_body" >&2 || true
        fail "bad Content-Type upload transport failed"
      }
    assert_http_error "$bad_type_http" "$bad_type_body" 415 unsupported_content_type "bad Content-Type upload"
    status_body=$(run_timeout "$SHORT_TIMEOUT" curl -fsS "$(api_url /ota)") || fail "OTA status after negative upload failed"
    printf '%s\n' "$status_body" | grep -q '"unsupported_content_type"' || {
      echo "OTA status did not preserve unsupported_content_type; response follows:" >&2
      printf '%s\n' "$status_body" >&2
      fail "OTA status missing unsupported_content_type last_error"
    }
    rm -f "$bad_sha_body" "$bad_type_body"
    trap - EXIT HUP INT TERM
  fi
  note "negative upload: non-.bin artifacts are rejected by this runner before upload"
}

assert_http_error() {
  actual_http="$1"
  body_path="$2"
  expected_http="$3"
  expected_code="$4"
  label="$5"

  if [ "$actual_http" != "$expected_http" ]; then
    echo "$label expected HTTP $expected_http, got $actual_http; response body follows:" >&2
    cat "$body_path" >&2 || true
    fail "$label returned unexpected HTTP status"
  fi
  if ! grep -q "\"code\"[[:space:]]*:[[:space:]]*\"$expected_code\"" "$body_path"; then
    echo "$label expected error.code=$expected_code; response body follows:" >&2
    cat "$body_path" >&2 || true
    fail "$label returned unexpected error code"
  fi
}

watchdog_rollback() {
  note "watchdog rollback: BLOCKED"
  cat <<'EOF'
BLOCKED: watchdog rollback HIL requires a safe firmware fault-injection path.
No HTTP/WS/cmdline liveness fault is injected by this runner. Do not claim rollback passed.
EOF
}

find_rpi_partition() {
  lsblk -P -o NAME,VENDOR,TYPE,PKNAME 2>/dev/null | awk '
    function field(line, key, pattern, value) {
      pattern = key "=\"[^\"]*\""
      if (match(line, pattern)) {
        value = substr(line, RSTART + length(key) + 2, RLENGTH - length(key) - 3)
        return value
      }
      return ""
    }
    {
      name = field($0, "NAME")
      vendor = field($0, "VENDOR")
      type = field($0, "TYPE")
      pkname = field($0, "PKNAME")
      if (type == "disk" && vendor == "RPI") {
        rpi_disk[name] = 1
      }
      if (type == "part" && pkname != "") {
        count++
        part_name[count] = name
        part_parent[count] = pkname
      }
    }
    END {
      for (i = 1; i <= count; i++) {
        if (rpi_disk[part_parent[i]]) {
          print "/dev/" part_name[i]
          exit
        }
      }
    }
  '
}

wait_for_rpi_partition() {
  limit=20
  count=0
  while [ "$count" -lt "$limit" ]; do
    part=$(find_rpi_partition)
    if [ -n "$part" ]; then
      printf '%s\n' "$part"
      return 0
    fi
    sleep 0.5
    count=$((count + 1))
  done
  return 1
}

bootsel_http() {
  require_bootsel_gate
  maybe_run run_timeout "$SHORT_TIMEOUT" curl -fsS -X POST "$(api_url /bootloader)"
  if [ "$DRY_RUN" -eq 1 ]; then
    plan lsblk -P -o NAME,VENDOR,TYPE,PKNAME "# find actual partition whose PKNAME is a VENDOR=RPI disk"
    return
  fi
  part=$(wait_for_rpi_partition) || fail "BOOTSEL RPI partition not found after 10s"
  note "BOOTSEL partition: $part"
}

bootsel_cdc() {
  require_tty
  require_bootsel_gate
  if [ "$DRY_RUN" -eq 1 ]; then
    plan sh -c "printf '%s\\n' bootloader > '$TTY'"
    plan lsblk -P -o NAME,VENDOR,TYPE,PKNAME "# find actual partition whose PKNAME is a VENDOR=RPI disk"
    return
  fi
  printf '%s\n' bootloader > "$TTY"
  part=$(wait_for_rpi_partition) || fail "BOOTSEL RPI partition not found after 10s"
  note "BOOTSEL partition: $part"
}

flash_uf2() {
  [ "$DRY_RUN" -eq 1 ] || require_uf2_image
  require_flash_gate
  if [ "$DRY_RUN" -eq 1 ]; then
    plan lsblk -P -o NAME,VENDOR,TYPE,PKNAME "# find actual partition whose PKNAME is a VENDOR=RPI disk"
    plan udisksctl mount -b "<actual RPI partition from lsblk PKNAME>"
    plan cp "$UF2" /media/RPI-RP2/
    return
  fi
  part=$(find_rpi_partition)
  [ -n "$part" ] || fail "BOOTSEL RPI partition not found; enter BOOTSEL before flashing"
  mount_output=$(udisksctl mount -b "$part")
  mount_point=$(printf '%s\n' "$mount_output" | awk -F' at ' '{print $2}' | xargs printf '%s')
  [ -n "$mount_point" ] || fail "unable to parse udisksctl mount point"
  [ -d "$mount_point" ] || fail "udisksctl mount point is not a directory: $mount_point"
  cp "$UF2" "$mount_point/"
  note "copied UF2 to $mount_point"
}

run_flow() {
  case "$1" in
    preflight) preflight ;;
    status) status_only ;;
    api-auto-confirm) api_auto_confirm ;;
    api-manual-confirm) api_manual_confirm ;;
    negative-upload) negative_upload ;;
    watchdog-rollback) watchdog_rollback ;;
    bootsel-http) bootsel_http ;;
    bootsel-cdc) bootsel_cdc ;;
    flash-uf2) flash_uf2 ;;
    all)
      [ "$DRY_RUN" -eq 1 ] || fail "--flow all is dry-run-only; choose one executable flow at a time"
      status_only
      api_auto_confirm
      api_manual_confirm
      negative_upload
      watchdog_rollback
      bootsel_http
      if [ -n "$TTY" ]; then
        bootsel_cdc
      else
        note "CDC BOOTSEL dry-run skipped because --tty was not provided"
      fi
      flash_uf2
      ;;
    *) fail "unknown flow: $1" ;;
  esac
}

run_flow "$FLOW"
