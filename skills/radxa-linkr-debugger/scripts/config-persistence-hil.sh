#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later

set -eu

CANONICAL_UF2="build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2"
CANONICAL_OTA="build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin"
MODE="dry-run"
URL=""
URL_SET=0
FLOW=""
SERIAL=""
REBOOT_COMMAND=""
CAPTURE_START=""
CAPTURE_STOP=""
UF2="$CANONICAL_UF2"
OTA_IMAGE="$CANONICAL_OTA"
CONFIRM_SAVE=0
CONFIRM_APPLY=0
CLEANUP_REQUIRED=0
CAPTURE_ACTIVE=0
SAFE_RESTORE_REQUIRED=0
SAFE_SD_ID=""
SAFE_TF_WP_ID=""
REQUEST_NUMBER=0
OTA_UPLOAD_PID=""
OTA_UPLOAD_BODY=""
OTA_UPLOAD_STATUS=""
OTA_UPLOAD_ACTIVE=0
CURL_CONNECT_TIMEOUT=2
HTTP_MAX_TIME=5
OTA_STATUS_MAX_TIME=2
OTA_UPLOAD_MAX_TIME=120
CLEANUP_MAX_TIME=5
REBOOT_READINESS_ATTEMPTS=45
REBOOT_READINESS_MAX_TIME=2
BOOTSEL_PARTITIONS_BEFORE=""
BOOTSEL_MOUNTED_PARTITION=""
CURL_BIN="${CONFIG_PERSISTENCE_HIL_CURL_BIN:-curl}"
SLEEP_BIN="${CONFIG_PERSISTENCE_HIL_SLEEP_BIN:-sleep}"
SERIAL_BIN="${CONFIG_PERSISTENCE_HIL_SERIAL_BIN:-}"
LSBLK_BIN="${CONFIG_PERSISTENCE_HIL_LSBLK_BIN:-lsblk}"
MOUNT_BIN="${CONFIG_PERSISTENCE_HIL_MOUNT_BIN:-udisksctl}"
FLASH_BIN="${CONFIG_PERSISTENCE_HIL_FLASH_BIN:-cp}"
SHA256_BIN="${CONFIG_PERSISTENCE_HIL_SHA256_BIN:-sha256sum}"

usage() {
  cat <<'USAGE'
Usage: config-persistence-hil.sh [options] FLOW

Flows:
  safe-reboot       Save a firmware-enumerated safe value, reboot, verify, clear.
  dangerous-pending Prove USB route target needs separate save/apply confirmations.
  capture-busy      Hold a supplied capture and require config save to return busy.
  ota-preserve      Preserve a safe snapshot through a canonical MCUboot OTA test boot.
  bootsel-preserve  Preserve a safe snapshot through HTTP BOOTSEL combined-UF2 recovery.
  cdc-fallback      Exercise CDC config commands and CDC BOOTSEL combined-UF2 recovery.
  all               Run every flow; CDC and BOOTSEL prerequisites are mandatory.

Options:
  --dry-run                    Print evidence-backed plans only (default).
  --execute                    Permit real operations; requires --url and FLOW.
  --url URL                    Explicit board URL for --execute.
  --serial PATH                CDC device for cdc-fallback and all.
  --reboot-command PATH        Explicit host command for safe-reboot/dangerous-pending.
  --capture-start PATH         Explicit command that starts a capture for capture-busy.
  --capture-stop PATH          Explicit command that stops that capture.
  --combined-uf2 PATH          Must be the canonical combined recovery UF2 path.
  --ota-image PATH             Must be the canonical MCUboot OTA .bin path.
  --confirm-dangerous-save     Explicit confirmation for the dangerous save step.
  --confirm-dangerous-apply    Explicit confirmation for the dangerous apply step.
  --help                       Show this help.

The runner never owns a rail, GPIO, or route catalog. Execute mode discovers
configuration item IDs from GET /api/v1/config. Dry-run never invokes curl,
serial, sleep, BOOTSEL discovery, mounting, copying, flashing, or hardware.
Safe selection preserves the firmware response order and uses its first safe item.
USAGE
}

fail() {
  printf '%s\n' "ERROR: $*" >&2
  exit 1
}

require_value() {
  [ "$#" -ge 2 ] || fail "$1 requires a value"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dry-run) MODE="dry-run"; shift ;;
    --execute) MODE="execute"; shift ;;
    --url)
      require_value "$@"
      URL="${2%/}"
      URL_SET=1
      shift 2
      ;;
    --serial)
      require_value "$@"
      SERIAL="$2"
      shift 2
      ;;
    --reboot-command)
      require_value "$@"
      REBOOT_COMMAND="$2"
      shift 2
      ;;
    --capture-start)
      require_value "$@"
      CAPTURE_START="$2"
      shift 2
      ;;
    --capture-stop)
      require_value "$@"
      CAPTURE_STOP="$2"
      shift 2
      ;;
    --combined-uf2)
      require_value "$@"
      UF2="$2"
      shift 2
      ;;
    --ota-image)
      require_value "$@"
      OTA_IMAGE="$2"
      shift 2
      ;;
    --confirm-dangerous-save) CONFIRM_SAVE=1; shift ;;
    --confirm-dangerous-apply) CONFIRM_APPLY=1; shift ;;
    --help|-h) usage; exit 0 ;;
    --*) fail "unknown option: $1" ;;
    *)
      [ -z "$FLOW" ] || fail "only one flow may be specified"
      FLOW="$1"
      shift
      ;;
  esac
done

case "$FLOW" in
  safe-reboot|dangerous-pending|capture-busy|ota-preserve|bootsel-preserve|cdc-fallback|all) ;;
  "") usage >&2; exit 2 ;;
  *) fail "unknown flow: $FLOW" ;;
esac

if [ "$MODE" = "execute" ]; then
  [ "$URL_SET" -eq 1 ] || fail "--execute requires an explicit --url"
  case "$URL" in
    http://*|https://*) ;;
    *) fail "--url must start with http:// or https://" ;;
  esac
  case "$URL" in *[[:space:]]*) fail "--url must not contain whitespace" ;; esac
fi

TMPDIR_RUN=""

evidence() {
  timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  printf 'timestamp=%s\trequest=%s\thttp_status=%s\tcode=%s\tassertion=%s\n' \
    "$timestamp" "$1" "$2" "$3" "$4"
}

plan() {
  evidence "$1" "planned" "planned" "$2"
}

api_url() {
  if [ -n "$URL" ]; then
    printf '%s/api/v1%s' "$URL" "$1"
  else
    printf '<explicit-url>/api/v1%s' "$1"
  fi
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || fail "required command is unavailable: $1"
}

require_reboot_command() {
  [ -n "$REBOOT_COMMAND" ] || fail "--reboot-command is required for $FLOW"
  [ -x "$REBOOT_COMMAND" ] || fail "reboot command is not executable: $REBOOT_COMMAND"
}

require_capture_commands() {
  [ -n "$CAPTURE_START" ] || fail "--capture-start is required for capture-busy"
  [ -n "$CAPTURE_STOP" ] || fail "--capture-stop is required for capture-busy"
  [ -x "$CAPTURE_START" ] || fail "capture start command is not executable: $CAPTURE_START"
  [ -x "$CAPTURE_STOP" ] || fail "capture stop command is not executable: $CAPTURE_STOP"
}

require_serial() {
  [ -n "$SERIAL" ] || fail "--serial is required for $FLOW"
  [ -e "$SERIAL" ] || fail "CDC serial device is unavailable: $SERIAL"
  if [ -z "$SERIAL_BIN" ]; then
    require_command python3
    python3 -c 'import serial' >/dev/null 2>&1 || fail "default PySerial import failed"
  else
    [ -x "$SERIAL_BIN" ] || fail "serial helper is not executable: $SERIAL_BIN"
  fi
}

require_canonical_uf2() {
  [ "$UF2" = "$CANONICAL_UF2" ] || fail "ROM BOOTSEL accepts only $CANONICAL_UF2; app-only zephyr.uf2 is forbidden"
  if [ ! -f "$UF2" ] || [ ! -s "$UF2" ]; then
    fail "canonical combined UF2 is unavailable: $UF2"
  fi
}

require_canonical_ota() {
  case "$OTA_IMAGE" in
    *.uf2|*.elf) fail "OTA rejects unsafe artifact type: $OTA_IMAGE" ;;
  esac
  [ "$OTA_IMAGE" = "$CANONICAL_OTA" ] || fail "OTA accepts only $CANONICAL_OTA"
  if [ ! -f "$OTA_IMAGE" ] || [ ! -s "$OTA_IMAGE" ]; then
    fail "canonical MCUboot OTA image is unavailable: $OTA_IMAGE"
  fi
}

require_dangerous_confirmations() {
  [ "$CONFIRM_SAVE" -eq 1 ] || fail "dangerous-pending requires --confirm-dangerous-save"
  [ "$CONFIRM_APPLY" -eq 1 ] || fail "dangerous-pending requires --confirm-dangerous-apply"
}

prepare_execute() {
  [ "$MODE" = "execute" ] || return 0
  require_command "$CURL_BIN"
  require_command python3
  require_command date
  require_command mktemp
  require_command rm
  case "$FLOW" in
    safe-reboot|dangerous-pending|all) require_reboot_command ;;
  esac
  case "$FLOW" in
    dangerous-pending|all) require_dangerous_confirmations ;;
  esac
  case "$FLOW" in
    capture-busy|all) require_capture_commands ;;
  esac
  case "$FLOW" in
    ota-preserve|all)
      require_canonical_ota
      require_command "$SHA256_BIN"
      require_command wc
      require_command tr
      require_command awk
      ;;
  esac
  case "$FLOW" in
    bootsel-preserve|cdc-fallback|all)
      require_canonical_uf2
      require_command "$LSBLK_BIN"
      require_command "$MOUNT_BIN"
      require_command "$FLASH_BIN"
      require_command awk
      require_command tr
      ;;
  esac
  case "$FLOW" in
    cdc-fallback|all) require_serial ;;
  esac
  case "$FLOW" in
    safe-reboot|dangerous-pending|ota-preserve|bootsel-preserve|cdc-fallback|all)
      require_command "$SLEEP_BIN"
      ;;
  esac
}

prepare_dry_run() {
  [ "$MODE" = "dry-run" ] || return 0
  case "$FLOW" in
    ota-preserve|all) require_canonical_ota ;;
  esac
  case "$FLOW" in
    bootsel-preserve|cdc-fallback|all) require_canonical_uf2 ;;
  esac
  case "$FLOW" in
    cdc-fallback|all) require_serial ;;
  esac
}

response_code() {
  python3 - "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8" "$9" "${10}" <<'PY'
import json
import sys

(
    path,
    status_text,
    expected_command,
    expected_action,
    expected_activity,
    expected_dangerous_ids,
    method,
    request_path,
    payload,
    expected_state,
) = sys.argv[1:]
with open(path, encoding="utf-8") as source:
    value = json.load(source)
if not isinstance(value, dict):
    raise SystemExit("response is not an object")
if value.get("schema") != "radxa-linkr-debugger.v1":
    raise SystemExit("response schema is missing or invalid")
if not isinstance(value.get("ok"), bool):
    raise SystemExit("response ok is missing or invalid")
if value.get("command") != expected_command:
    raise SystemExit("response command is missing or invalid")
if expected_action != "-" and value.get("action") != expected_action:
    raise SystemExit("response action is missing or invalid")
try:
    status = int(status_text)
except ValueError as error:
    raise SystemExit("HTTP status is invalid") from error
if not 100 <= status <= 599:
    raise SystemExit("HTTP status is invalid")
if value["ok"] != (200 <= status < 300):
    raise SystemExit("response HTTP status disagrees with ok")

def string_list(field, *, nonempty=False):
    result = value.get(field)
    if not isinstance(result, list) or any(not isinstance(item, str) or not item for item in result):
        raise SystemExit(f"{field} is missing or invalid")
    if nonempty and not result:
        raise SystemExit(f"{field} is missing or invalid")
    return result

def snapshot():
    result = value.get("snapshot")
    if not isinstance(result, dict) or not isinstance(result.get("present"), bool):
        raise SystemExit("snapshot is missing or invalid")
    version = result.get("version")
    if version is not None and (type(version) is not int or version < 0):
        raise SystemExit("snapshot is missing or invalid")

def pending():
    result = value.get("pending")
    if type(result) is not int or result < 0:
        raise SystemExit("pending is missing or invalid")

def config_success_shape():
    if expected_action == "get":
        backend = value.get("backend")
        if not isinstance(backend, dict) or not isinstance(backend.get("available"), bool) or not isinstance(backend.get("reason"), str):
            raise SystemExit("backend is missing or invalid")
        snapshot()
        pending()
        if not isinstance(value.get("items"), list):
            raise SystemExit("items is missing or invalid")
    elif expected_action == "save":
        string_list("saved_items")
        string_list("confirmation_items")
        snapshot()
        pending()
    elif expected_action == "apply":
        if not isinstance(value.get("noop"), bool):
            raise SystemExit("noop is missing or invalid")
        string_list("applied_items")
        if "failed_item" not in value:
            raise SystemExit("failed_item is missing or invalid")
        failed_item = value["failed_item"]
        if failed_item is not None and (not isinstance(failed_item, str) or not failed_item):
            raise SystemExit("failed_item is missing or invalid")
        string_list("pending_items")
    elif expected_action == "clear":
        if not isinstance(value.get("noop"), bool):
            raise SystemExit("noop is missing or invalid")
        snapshot()
        pending()

def switch_route_success_shape():
    if method != "PUT" or not request_path.startswith("/switch/"):
        return
    name = request_path.rsplit("/", 1)[1]
    try:
        request = json.loads(payload)
    except json.JSONDecodeError as error:
        raise SystemExit("switch route request is invalid") from error
    route = request.get("route") if isinstance(request, dict) else None
    if not isinstance(route, str) or not route:
        raise SystemExit("switch route request is invalid")
    if value.get("name") != name or value.get("route") != route:
        raise SystemExit("switch route response is missing or invalid")

def switch_get_success_shape():
    if method != "GET" or not request_path.startswith("/switch/"):
        raise SystemExit("switch get request is invalid")
    name = request_path.rsplit("/", 1)[1]
    if value.get("name") != name or not isinstance(value.get("route"), str) or not value["route"]:
        raise SystemExit("switch get response is missing or invalid")

def power_output_shape(output, *, expected_name=None, expected_state=None):
    if not isinstance(output, dict) or not isinstance(output.get("name"), str) or not output["name"]:
        raise SystemExit("power output is missing or invalid")
    if not isinstance(output.get("controllable"), bool):
        raise SystemExit("power output controllable is missing or invalid")
    if expected_name is not None and output["name"] != expected_name:
        raise SystemExit("power output name does not match the request")
    if output["controllable"]:
        if output.get("state") not in {"on", "off"} or type(output.get("value")) is not int or output["value"] not in {0, 1}:
            raise SystemExit("controllable power output state is invalid")
        if output["value"] != (1 if output["state"] == "on" else 0):
            raise SystemExit("power output state and value disagree")
    elif output.get("state") != "locked" or output.get("value") is not None:
        raise SystemExit("locked power output state is invalid")
    if expected_state is not None and output.get("state") != expected_state:
        raise SystemExit("power output state does not match the request")

def power_success_shape():
    if expected_action == "list":
        outputs = value.get("power_outputs")
        if not isinstance(outputs, list):
            raise SystemExit("power_outputs is missing or invalid")
        for output in outputs:
            power_output_shape(output)
        names = [output["name"] for output in outputs]
        if len(names) != len(set(names)):
            raise SystemExit("power_outputs contains duplicate names")
    elif expected_action == "set":
        if method != "PUT" or not request_path.startswith("/power/"):
            raise SystemExit("power set request is invalid")
        name = request_path.rsplit("/", 1)[1]
        try:
            request = json.loads(payload)
        except json.JSONDecodeError as error:
            raise SystemExit("power set request is invalid") from error
        if not isinstance(request, dict) or request.get("state") != "off":
            raise SystemExit("power set request is invalid")
        power_output_shape(value.get("power_output"), expected_name=name, expected_state="off")

def gpio_row_shape(row, *, list_response):
    if not isinstance(row, dict) or not isinstance(row.get("name"), str) or not row["name"]:
        raise SystemExit("GPIO row is missing or invalid")
    if row.get("direction") not in {"input", "output"}:
        raise SystemExit("GPIO direction is missing or invalid")
    if list_response:
        if type(row.get("value")) is not int or row["value"] not in {0, 1}:
            raise SystemExit("GPIO value is missing or invalid")
    elif row.get("direction") != "input" or row.get("value") is not None:
        raise SystemExit("GPIO input response is invalid")

def gpio_success_shape():
    if expected_action == "list":
        rows = value.get("gpios")
        if not isinstance(rows, list):
            raise SystemExit("gpios is missing or invalid")
        for row in rows:
            gpio_row_shape(row, list_response=True)
        names = [row["name"] for row in rows]
        if len(names) != len(set(names)):
            raise SystemExit("gpios contains duplicate names")
    elif expected_action == "input":
        if method != "PUT" or not request_path.startswith("/gpio/"):
            raise SystemExit("GPIO input request is invalid")
        name = request_path.rsplit("/", 1)[1]
        try:
            request = json.loads(payload)
        except json.JSONDecodeError as error:
            raise SystemExit("GPIO input request is invalid") from error
        if not isinstance(request, dict) or request.get("direction") != "input":
            raise SystemExit("GPIO input request is invalid")
        row = value.get("gpio")
        gpio_row_shape(row, list_response=False)
        if row["name"] != name:
            raise SystemExit("GPIO name does not match the request")

def ota_success_shape():
    state = value.get("state")
    if not isinstance(state, str) or not state:
        raise SystemExit("OTA state is missing or invalid")
    if expected_state == "-" or state != expected_state:
        raise SystemExit("OTA state does not match the expected state")

if value["ok"]:
    if expected_command == "config":
        config_success_shape()
    elif expected_command == "switch" and expected_action == "route":
        switch_route_success_shape()
    elif expected_command == "switch" and expected_action == "get":
        switch_get_success_shape()
    elif expected_command == "power":
        power_success_shape()
    elif expected_command == "gpio":
        gpio_success_shape()
    elif expected_command == "ota":
        ota_success_shape()
    print("ok")
else:
    error = value.get("error")
    if not isinstance(error, dict) or not isinstance(error.get("code"), str) or not error["code"]:
        raise SystemExit("error.code is missing or invalid")
    if not isinstance(error.get("message"), str):
        raise SystemExit("error.message is missing or invalid")
    code = error["code"]
    if code == "confirmation_required":
        dangerous_items = string_list("dangerous_items", nonempty=True)
        expected_items = [] if expected_dangerous_ids == "-" else expected_dangerous_ids.split(",")
        if expected_items and dangerous_items != expected_items:
            raise SystemExit("dangerous_items do not match the requested IDs")
    elif code == "busy":
        activity = value.get("activity")
        if activity not in {"capture", "ota"}:
            raise SystemExit("activity is missing or invalid")
        if expected_activity != "-" and activity != expected_activity:
            raise SystemExit("activity does not match the expected operation")
    elif code == "apply_failed":
        string_list("applied_items")
        failed_item = value.get("failed_item")
        if not isinstance(failed_item, str) or not failed_item:
            raise SystemExit("failed_item is missing or invalid")
        string_list("pending_items")
    print(code)
PY
}

http_request() {
  method="$1"
  path="$2"
  payload="$3"
  expected_status="$4"
  expected_code="$5"
  expected_command="$6"
  expected_action="$7"
  assertion="$8"
  expected_activity="${9:--}"
  expected_dangerous_ids="${10:--}"
  expected_state="${11:--}"
  REQUEST_NUMBER=$((REQUEST_NUMBER + 1))
  body="$TMPDIR_RUN/response.$REQUEST_NUMBER.json"
  if [ -n "$payload" ]; then
    status=$("$CURL_BIN" --silent --show-error --connect-timeout "$CURL_CONNECT_TIMEOUT" \
      --max-time "$HTTP_MAX_TIME" --output "$body" --write-out '%{http_code}' \
      --request "$method" --header 'Content-Type: application/json' --data "$payload" \
      "$(api_url "$path")") || fail "transport failed for $method $path"
  else
    status=$("$CURL_BIN" --silent --show-error --connect-timeout "$CURL_CONNECT_TIMEOUT" \
      --max-time "$HTTP_MAX_TIME" --output "$body" --write-out '%{http_code}' \
      --request "$method" "$(api_url "$path")") || fail "transport failed for $method $path"
  fi
  code=$(response_code "$body" "$status" "$expected_command" "$expected_action" \
    "$expected_activity" "$expected_dangerous_ids" "$method" "$path" "$payload" \
    "$expected_state") || \
    fail "malformed response for $method $path"
  evidence "$method $path" "$status" "$code" "$assertion"
  [ "$status" = "$expected_status" ] || fail "$method $path expected HTTP $expected_status, got $status"
  [ "$code" = "$expected_code" ] || fail "$method $path expected code $expected_code, got $code"
  HTTP_BODY="$body"
}

config_get() {
  http_request GET /config "" 200 ok config get "read firmware-owned config catalog"
}

select_config_item() {
  mode="$1"
  python3 - "$HTTP_BODY" "$mode" <<'PY'
import json
import re
import sys

path, mode = sys.argv[1:]
with open(path, encoding="utf-8") as source:
    document = json.load(source)
backend = document.get("backend")
snapshot = document.get("snapshot")
items = document.get("items")
if not isinstance(backend, dict) or backend.get("available") is not True:
    raise SystemExit("config backend is unavailable")
if not isinstance(snapshot, dict) or snapshot.get("present") is not False:
    raise SystemExit("runner requires an absent snapshot to preserve prior state")
if not isinstance(items, list):
    raise SystemExit("config items is missing")

if mode == "safe":
    candidates = [
        item for item in items
        if isinstance(item, dict)
        and isinstance(item.get("id"), str)
        and item.get("current") is not None
        and item.get("requires_confirm") is False
    ]
elif mode == "dangerous":
    matching = [item for item in items if isinstance(item, dict) and item.get("id") == "switch/usb"]
    if len(matching) != 1:
        raise SystemExit("expected exactly one switch/usb item")
    item = matching[0]
    current = item.get("current")
    if (
        item.get("kind") != "switch"
        or item.get("requires_confirm") is not True
        or not isinstance(current, dict)
        or current.get("route") != "target"
    ):
        raise SystemExit("switch/usb target item is malformed")
    candidates = [item]
else:
    raise SystemExit("unknown selection mode")

if not candidates:
    raise SystemExit(f"expected a firmware-enumerated {mode} candidate, got none")
item_id = candidates[0]["id"]
if not re.fullmatch(r"[A-Za-z0-9_./-]+", item_id):
    raise SystemExit("firmware item id is unsafe for shell transport")
print(item_id)
PY
}

discover_safe_reboot_items() {
  safe_items=$(python3 - "$HTTP_BODY" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    document = json.load(source)
backend = document.get("backend")
snapshot = document.get("snapshot")
items = document.get("items")
if not isinstance(backend, dict) or backend.get("available") is not True:
    raise SystemExit("config backend is unavailable")
if not isinstance(snapshot, dict) or snapshot.get("present") is not False:
    raise SystemExit("runner requires an absent snapshot to preserve prior state")
if not isinstance(items, list):
    raise SystemExit("config items is missing")

selected = {}
for item in items:
    if not isinstance(item, dict) or item.get("id") not in {"switch/sd", "switch/tf_wp"}:
        continue
    current = item.get("current")
    if (
        item.get("kind") != "switch"
        or item.get("requires_confirm") is not False
        or not isinstance(current, dict)
        or not isinstance(current.get("route"), str)
        or not current["route"]
    ):
        raise SystemExit(f"required safe item is malformed: {item.get('id')}")
    selected[item["id"]] = item

if set(selected) != {"switch/sd", "switch/tf_wp"}:
    raise SystemExit("required SD and TF-WP config items are unavailable")
for item_id in ("switch/sd", "switch/tf_wp"):
    if not re.fullmatch(r"[A-Za-z0-9_./-]+", item_id):
        raise SystemExit("firmware item id is unsafe for shell transport")
print("switch/sd switch/tf_wp")
PY
  ) || fail "missing or malformed safe-reboot config items"
  [ "$safe_items" = "switch/sd switch/tf_wp" ] || \
    fail "safe-reboot item discovery returned an invalid result"
  SAFE_SD_ID=switch/sd
  SAFE_TF_WP_ID=switch/tf_wp
}

assert_safe_reboot_state() {
  snapshot_present="$1"
  sd_route="$2"
  tf_wp_route="$3"
  saved_present="$4"
  apply_state="$5"
  python3 - "$HTTP_BODY" "$snapshot_present" "$sd_route" "$tf_wp_route" "$saved_present" "$apply_state" <<'PY'
import json
import sys

path, snapshot_present, sd_route, tf_wp_route, saved_present, apply_state = sys.argv[1:]
with open(path, encoding="utf-8") as source:
    document = json.load(source)
snapshot = document.get("snapshot")
if not isinstance(snapshot, dict) or snapshot.get("present") is not (snapshot_present == "true"):
    raise SystemExit("snapshot state does not match the expected safe-reboot phase")
items = document.get("items")
if not isinstance(items, list):
    raise SystemExit("config items is missing")
expected = {"switch/sd": sd_route, "switch/tf_wp": tf_wp_route}
for item_id, route in expected.items():
    matching = [item for item in items if isinstance(item, dict) and item.get("id") == item_id]
    if len(matching) != 1:
        raise SystemExit(f"required safe item is missing: {item_id}")
    item = matching[0]
    current = item.get("current")
    if not isinstance(current, dict) or current.get("route") != route:
        raise SystemExit(f"live route does not match for {item_id}")
    if saved_present == "true":
        saved = item.get("saved")
        if not isinstance(saved, dict) or saved.get("route") != route or item.get("selected") is not True:
            raise SystemExit(f"saved route does not match for {item_id}")
    elif item.get("saved") is not None or item.get("selected") is not False:
        raise SystemExit(f"cleared route was retained for {item_id}")
    if item.get("apply_state") != apply_state:
        raise SystemExit(f"apply state does not match for {item_id}")
PY
}

assert_snapshot_absent() {
  python3 - "$HTTP_BODY" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    document = json.load(source)
snapshot = document.get("snapshot")
if not isinstance(snapshot, dict) or snapshot.get("present") is not False:
    raise SystemExit("snapshot was not cleared")
if document.get("pending") != 0:
    raise SystemExit("cleared snapshot still reports pending items")
PY
}

assert_config_item() {
  item_id="$1"
  apply_state="$2"
  python3 - "$HTTP_BODY" "$item_id" "$apply_state" <<'PY'
import json
import sys

path, item_id, apply_state = sys.argv[1:]
with open(path, encoding="utf-8") as source:
    document = json.load(source)
snapshot = document.get("snapshot")
if not isinstance(snapshot, dict) or snapshot.get("present") is not True:
    raise SystemExit("saved snapshot is missing")
items = document.get("items")
if not isinstance(items, list):
    raise SystemExit("config items is missing")
matching = [item for item in items if isinstance(item, dict) and item.get("id") == item_id]
if len(matching) != 1:
    raise SystemExit("saved item is missing")
item = matching[0]
if item.get("selected") is not True or item.get("saved") is None:
    raise SystemExit("saved item was not retained")
if item.get("apply_state") != apply_state:
    raise SystemExit(f"expected apply_state={apply_state}, got {item.get('apply_state')}")
PY
}

switch_route() {
  switch_name="$1"
  route="$2"
  payload=$(printf '{"route":"%s"}' "$route")
  http_request PUT "/switch/$switch_name" "$payload" 200 ok switch route \
    "set switch route $switch_name=$route"
}

switch_get() {
  switch_name="$1"
  expected_route="$2"
  http_request GET "/switch/$switch_name" "" 200 ok switch get "read switch route $switch_name"
  python3 - "$HTTP_BODY" "$switch_name" "$expected_route" <<'PY'
import json
import sys

path, name, route = sys.argv[1:]
with open(path, encoding="utf-8") as source:
    value = json.load(source)
if value.get("name") != name or value.get("route") != route:
    raise SystemExit("switch route does not match the expected safe state")
PY
}

save_items() {
  confirmed="$1"
  shift
  [ "$#" -gt 0 ] || fail "config save requires at least one firmware item"
  payload='{"items":['
  separator=""
  for item_id in "$@"; do
    payload="${payload}${separator}\"${item_id}\""
    separator=,
  done
  payload="${payload}],\"confirm\":${confirmed}}"
  CLEANUP_REQUIRED=1
  http_request PUT /config "$payload" 200 ok config save "save selected firmware item"
}

save_item() {
  item_id="$1"
  confirmed="$2"
  save_items "$confirmed" "$item_id"
}

clear_snapshot() {
  http_request DELETE /config "" 200 ok config clear "clear test snapshot without changing hardware"
  CLEANUP_REQUIRED=0
}

apply_snapshot() {
  confirmed="$1"
  payload=$(printf '{"confirm":%s}' "$confirmed")
  http_request POST /config/apply "$payload" 200 ok config apply "apply saved configuration"
}

sleep_for_reboot() {
  if [ "$MODE" = "dry-run" ]; then
    plan "sleep 5" "wait for planned reboot"
    plan "GET /config readiness" "wait for bounded post-reboot transport recovery"
    return
  fi
  "$SLEEP_BIN" 5
  evidence "sleep 5" "not_applicable" "not_applicable" "wait for reboot"
  wait_for_reboot_transport
}

sleep_for_bootsel() {
  if [ "$MODE" = "dry-run" ]; then
    plan "sleep 5" "wait for planned BOOTSEL enumeration"
    return
  fi
  "$SLEEP_BIN" 5
  evidence "sleep 5" "not_applicable" "not_applicable" "wait for BOOTSEL enumeration"
}

wait_for_reboot_transport() {
  attempts=0
  while [ "$attempts" -lt "$REBOOT_READINESS_ATTEMPTS" ]; do
    REQUEST_NUMBER=$((REQUEST_NUMBER + 1))
    body="$TMPDIR_RUN/reboot-readiness.$REQUEST_NUMBER.json"
    if status=$("$CURL_BIN" --silent --show-error --connect-timeout "$CURL_CONNECT_TIMEOUT" \
      --max-time "$REBOOT_READINESS_MAX_TIME" --output "$body" --write-out '%{http_code}' \
      --request GET "$(api_url /config)"); then
      code=$(response_code "$body" "$status" config get - - GET /config "" -) || \
        fail "malformed response for post-reboot GET /config readiness"
      if [ "$status" != 200 ] || [ "$code" != ok ]; then
        fail "post-reboot GET /config readiness expected HTTP 200 ok, got HTTP $status $code"
      fi
      HTTP_BODY="$body"
      evidence "GET /config readiness" "$status" "$code" "confirm post-reboot config transport readiness"
      return
    fi
    attempts=$((attempts + 1))
    "$SLEEP_BIN" 1
  done
  fail "post-reboot config transport did not recover within ${REBOOT_READINESS_ATTEMPTS}s"
}

reboot_board() {
  if [ "$MODE" = "dry-run" ]; then
    plan "external reboot command" "reboot uses an explicit host command"
    sleep_for_reboot
    return
  fi
  "$REBOOT_COMMAND"
  evidence "external reboot command" "not_applicable" "not_applicable" "reboot command completed"
  sleep_for_reboot
}

ota_upload_is_alive() {
  [ "$OTA_UPLOAD_ACTIVE" -eq 1 ] && [ -n "$OTA_UPLOAD_PID" ] && \
    kill -0 "$OTA_UPLOAD_PID" 2>/dev/null
}

stop_ota_upload() {
  [ "$OTA_UPLOAD_ACTIVE" -eq 1 ] || return 0
  if ota_upload_is_alive; then
    kill "$OTA_UPLOAD_PID" 2>/dev/null || :
  fi
  wait "$OTA_UPLOAD_PID" >/dev/null 2>&1 || :
  OTA_UPLOAD_ACTIVE=0
  OTA_UPLOAD_PID=""
}

start_ota_upload() {
  REQUEST_NUMBER=$((REQUEST_NUMBER + 1))
  OTA_UPLOAD_BODY="$TMPDIR_RUN/ota-upload.$REQUEST_NUMBER.json"
  OTA_UPLOAD_STATUS="$TMPDIR_RUN/ota-upload.$REQUEST_NUMBER.status"
  size=$(wc -c < "$OTA_IMAGE" | tr -d ' ')
  sha=$("$SHA256_BIN" "$OTA_IMAGE" | awk '{print $1}')
  "$CURL_BIN" --silent --show-error --connect-timeout "$CURL_CONNECT_TIMEOUT" \
    --max-time "$OTA_UPLOAD_MAX_TIME" --output "$OTA_UPLOAD_BODY" --write-out '%{http_code}' \
    --request POST --header 'Content-Type: application/octet-stream' \
    --header "X-Linkr-Ota-Size: $size" --header "X-Linkr-Ota-Sha256: $sha" \
    --limit-rate 64K \
    --data-binary "@$OTA_IMAGE" "$(api_url /ota/upload)" > "$OTA_UPLOAD_STATUS" &
  OTA_UPLOAD_PID=$!
  OTA_UPLOAD_ACTIVE=1
  evidence "background POST /ota/upload" "in_progress" "in_progress" \
    "start canonical MCUboot OTA upload"
}

wait_for_ota_uploading() {
  attempts=0
  while [ "$attempts" -lt 20 ]; do
    ota_upload_is_alive || fail "OTA upload ended before state=uploading was observed"
    REQUEST_NUMBER=$((REQUEST_NUMBER + 1))
    body="$TMPDIR_RUN/response.$REQUEST_NUMBER.json"
    status=$("$CURL_BIN" --silent --show-error --connect-timeout "$CURL_CONNECT_TIMEOUT" \
      --max-time "$OTA_STATUS_MAX_TIME" --output "$body" \
      --write-out '%{http_code}' --request GET "$(api_url /ota)") || \
      fail "transport failed for GET /ota"
    if code=$(response_code "$body" "$status" ota - - - GET /ota "" uploading); then
      if [ "$status" != 200 ] || [ "$code" != ok ]; then
        fail "GET /ota did not report uploading"
      fi
      HTTP_BODY="$body"
      evidence "GET /ota" "$status" "$code" "observe bounded active OTA upload"
      return 0
    fi
    attempts=$((attempts + 1))
    "$SLEEP_BIN" 0.1
  done
  fail "OTA state did not reach uploading within the bounded polling window"
}

require_ota_upload_active() {
  ota_upload_is_alive || fail "OTA upload ended before active-window config checks completed"
}

await_ota_upload() {
  if [ "$OTA_UPLOAD_ACTIVE" -ne 1 ] || [ -z "$OTA_UPLOAD_PID" ]; then
    fail "OTA upload is not owned by the runner"
  fi
  if wait "$OTA_UPLOAD_PID"; then
    upload_result=0
  else
    upload_result=$?
  fi
  OTA_UPLOAD_ACTIVE=0
  OTA_UPLOAD_PID=""
  [ "$upload_result" -eq 0 ] || fail "transport failed for POST /ota/upload"
  [ -s "$OTA_UPLOAD_STATUS" ] || fail "OTA upload HTTP status is missing"
  status=$(tr -d '\r\n' < "$OTA_UPLOAD_STATUS")
  code=$(response_code "$OTA_UPLOAD_BODY" "$status" ota - - - POST /ota/upload \
    "@$OTA_IMAGE" verified) || fail "malformed response for POST /ota/upload"
  evidence "await POST /ota/upload" "$status" "$code" \
    "await and validate canonical MCUboot OTA upload"
  if [ "$status" != 200 ] || [ "$code" != ok ]; then
    fail "canonical OTA upload failed"
  fi
}

serial_request() {
  command_text="$1"
  expected_success="$2"
  if [ -n "$SERIAL_BIN" ]; then
    output=$("$SERIAL_BIN" "$SERIAL" "$command_text") || fail "CDC command failed: $command_text"
  else
    output=$(python3 - "$SERIAL" "$command_text" <<'PY'
import sys
import time

import serial

device, command = sys.argv[1:]
with serial.Serial(device, 115200, timeout=1) as port:
    port.reset_input_buffer()
    port.write((command + "\n").encode("utf-8"))
    port.flush()
    deadline = time.monotonic() + 3
    lines = []
    while time.monotonic() < deadline:
        line = port.readline().decode("utf-8", "replace").strip()
        if not line:
            continue
        if line.startswith("linkr-debugger:~$"):
            break
        lines.append(line)
if not lines:
    raise SystemExit("CDC command produced no response")
print("\n".join(lines))
PY
    ) || fail "CDC command failed: $command_text"
  fi
  validate_serial_output "$expected_success" "$output" || fail "CDC $command_text returned unexpected output"
  evidence "CDC $command_text" "not_applicable" "ok" "CDC shell command completed"
}

validate_serial_output() {
  expected_success="$1"
  output="$2"
  python3 - "$expected_success" "$output" <<'PY'
import sys

expected_success, output = sys.argv[1:]
lines = [line.strip() for line in output.splitlines() if line.strip()]
if not lines:
    raise SystemExit("CDC command produced no primary response")

prefixes = {
    "show": "config available=",
    "save": "config save saved_count=",
    "apply": "config apply applied_count=",
    "clear": "config clear hardware_changed=false",
    "bootloader": "Entering ",
}
prefix = prefixes.get(expected_success)
if prefix is None:
    raise SystemExit("unknown CDC success contract")
error_indexes = [
    index
    for index, line in enumerate(lines)
    if line.lower().startswith(("error:", "error "))
]
if error_indexes:
    if error_indexes[0] != 0:
        raise SystemExit("CDC primary error must precede detail lines")
    raise SystemExit("CDC command reported a primary error")
if not lines[0].startswith(prefix):
    raise SystemExit("CDC primary response is missing or invalid")
if expected_success == "bootloader" and " BOOTSEL in 250 ms..." not in lines[0]:
    raise SystemExit("CDC bootloader primary response is invalid")
PY
}

list_rpi_partitions() {
  lsblk_listing=$("$LSBLK_BIN" -P -o NAME,VENDOR,TYPE,PKNAME 2>/dev/null) || return 1
  printf '%s\n' "$lsblk_listing" | awk '
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
      parent = field($0, "PKNAME")
      if (type == "disk" && vendor == "RPI") disks[name] = 1
      if (type == "part" && parent != "") {
        part_names[++part_count] = name
        part_parents[name] = parent
      }
    }
    END {
      for (item_index = 1; item_index <= part_count; item_index++) {
        name = part_names[item_index]
        if (disks[part_parents[name]]) print "/dev/" name
      }
    }
  '
}

remember_rpi_partitions() {
  BOOTSEL_PARTITIONS_BEFORE=$(list_rpi_partitions) || fail "failed to enumerate RPI partitions before BOOTSEL entry"
}

find_new_rpi_partition() {
  rpi_partitions_after=$(list_rpi_partitions) || return 1
  new_partition=$(printf '%s\n' "$rpi_partitions_after" | awk -v before="$BOOTSEL_PARTITIONS_BEFORE" '
    BEGIN {
      count = split(before, previous, "\n")
      for (item_index = 1; item_index <= count; item_index++) {
        if (previous[item_index] != "") known[previous[item_index]] = 1
      }
    }
    NF {
      if (seen[$0]++) duplicate = 1
      if (!known[$0]) candidates[++candidate_count] = $0
    }
    END {
      if (duplicate || candidate_count != 1) exit 1
      print candidates[1]
    }
  ') || return 1
  [ -n "$new_partition" ] || return 1
  printf '%s\n' "$new_partition"
}

unmount_bootsel_partition() {
  [ -n "$BOOTSEL_MOUNTED_PARTITION" ] || return 0
  "$MOUNT_BIN" unmount -b "$BOOTSEL_MOUNTED_PARTITION" >/dev/null 2>&1 || return 1
  BOOTSEL_MOUNTED_PARTITION=""
}

flash_combined_uf2() {
  partition=$(find_new_rpi_partition) || \
    fail "expected exactly one new RPI-RP2 partition after BOOTSEL entry"
  evidence "lsblk RPI-RP2 discovery" "not_applicable" "ok" "discover BOOTSEL partition"
  mount_output=$("$MOUNT_BIN" mount -b "$partition") || fail "failed to mount $partition"
  BOOTSEL_MOUNTED_PARTITION="$partition"
  mount_point=$(printf '%s\n' "$mount_output" | awk -F' at ' '{print $2}' | tr -d '\n')
  if [ -z "$mount_point" ] || [ ! -d "$mount_point" ]; then
    fail "could not determine BOOTSEL mount point"
  fi
  evidence "mount -b $partition" "not_applicable" "ok" "mount BOOTSEL partition"
  "$FLASH_BIN" "$UF2" "$mount_point/" || fail "failed to copy canonical combined UF2"
  evidence "flash canonical combined UF2" "not_applicable" "ok" "flash canonical combined UF2 only"
  unmount_bootsel_partition || fail "failed to unmount BOOTSEL partition $partition"
}

bootloader_recovery() {
  remember_rpi_partitions
  http_request POST /bootloader "" 200 ok bootloader - "enter ROM BOOTSEL through HTTP"
  sleep_for_bootsel
  flash_combined_uf2
  sleep_for_reboot
}

cdc_bootloader_recovery() {
  remember_rpi_partitions
  serial_request "bootloader" bootloader
  sleep_for_bootsel
  flash_combined_uf2
  sleep_for_reboot
}

cleanup() {
  result=$?
  trap - EXIT HUP INT TERM
  if [ "$OTA_UPLOAD_ACTIVE" -eq 1 ]; then
    stop_ota_upload
  fi
  if [ "$CAPTURE_ACTIVE" -eq 1 ] && [ -n "$CAPTURE_STOP" ]; then
    "$CAPTURE_STOP" >/dev/null 2>&1 || result=1
    CAPTURE_ACTIVE=0
  fi
  if ! unmount_bootsel_partition; then
    result=1
  fi
  if [ "$SAFE_RESTORE_REQUIRED" -eq 1 ] && [ "$MODE" = "execute" ] && [ -n "$TMPDIR_RUN" ] && \
    [ -n "$SAFE_SD_ID" ] && [ -n "$SAFE_TF_WP_ID" ]; then
    safe_restore_ok=1
    if ! cleanup_switch_route "${SAFE_SD_ID#switch/}" target; then
      safe_restore_ok=0
      result=1
    fi
    if ! cleanup_switch_route "${SAFE_TF_WP_ID#switch/}" writable; then
      safe_restore_ok=0
      result=1
    fi
    if [ "$safe_restore_ok" -eq 1 ]; then
      SAFE_RESTORE_REQUIRED=0
    fi
  fi
  if [ "$CLEANUP_REQUIRED" -eq 1 ] && [ "$MODE" = "execute" ] && [ -n "$TMPDIR_RUN" ]; then
    if cleanup_clear_snapshot; then
      evidence "DELETE /config" "$status" "$code" "cleanup restored an absent test snapshot"
    else
      evidence "DELETE /config" "cleanup_failed" "cleanup_failed" "cleanup failure requires operator action"
      result=1
    fi
  fi
  [ -z "$TMPDIR_RUN" ] || rm -rf "$TMPDIR_RUN"
  exit "$result"
}

cleanup_clear_snapshot() {
  REQUEST_NUMBER=$((REQUEST_NUMBER + 1))
  body="$TMPDIR_RUN/cleanup.$REQUEST_NUMBER.json"
  status=$("$CURL_BIN" --silent --show-error --connect-timeout "$CURL_CONNECT_TIMEOUT" \
    --max-time "$CLEANUP_MAX_TIME" --output "$body" --write-out '%{http_code}' \
    --request DELETE "$(api_url /config)") || return 1
  code=$(response_code "$body" "$status" config clear - - DELETE /config "" -) || return 1
  [ "$status" = 200 ] && [ "$code" = ok ]
}

cleanup_switch_route() {
  switch_name="$1"
  route="$2"
  payload=$(printf '{"route":"%s"}' "$route")
  REQUEST_NUMBER=$((REQUEST_NUMBER + 1))
  body="$TMPDIR_RUN/cleanup.$REQUEST_NUMBER.json"
  status=$("$CURL_BIN" --silent --show-error --connect-timeout "$CURL_CONNECT_TIMEOUT" \
    --max-time "$CLEANUP_MAX_TIME" --output "$body" --write-out '%{http_code}' \
    --request PUT --header 'Content-Type: application/json' --data "$payload" \
    "$(api_url "/switch/$switch_name")") || return 1
  code=$(response_code "$body" "$status" switch route - - PUT "/switch/$switch_name" "$payload" -) || return 1
  [ "$status" = 200 ] && [ "$code" = ok ]
}

trap cleanup EXIT HUP INT TERM

safe_reboot() {
  if [ "$MODE" = "dry-run" ]; then
    plan "GET /config" "discover SD and TF-WP firmware items with no snapshot"
    plan "PUT /switch/sd" "route SD to usb-reader"
    plan "PUT /switch/tf_wp" "route TF-WP to protected"
    plan "GET /config" "assert both live safe routes before saving"
    plan "PUT /config" "save both discovered safe items"
    plan "external reboot command" "reboot without a client-owned hardware catalog"
    plan "sleep 5" "wait for planned reboot"
    plan "GET /config" "assert both saved safe routes are applied after reboot"
    plan "DELETE /config" "clear test snapshot without changing hardware"
    plan "GET /config" "assert clear retained both live safe routes"
    plan "PUT /switch/sd" "restore SD target route"
    plan "PUT /switch/tf_wp" "restore TF-WP writable route"
    plan "GET /config" "assert absent snapshot and final safe defaults"
    return
  fi
  config_get
  discover_safe_reboot_items
  CLEANUP_REQUIRED=1
  SAFE_RESTORE_REQUIRED=1
  switch_route "${SAFE_SD_ID#switch/}" usb-reader
  switch_route "${SAFE_TF_WP_ID#switch/}" protected
  config_get
  assert_safe_reboot_state false usb-reader protected false not_saved || \
    fail "safe routes were not prepared before save"
  save_items false "$SAFE_SD_ID" "$SAFE_TF_WP_ID"
  reboot_board
  config_get
  assert_safe_reboot_state true usb-reader protected true applied || \
    fail "safe routes were not restored after reboot"
  clear_snapshot
  config_get
  assert_safe_reboot_state false usb-reader protected false not_saved || \
    fail "clear altered live safe routes"
  switch_route "${SAFE_SD_ID#switch/}" target
  switch_route "${SAFE_TF_WP_ID#switch/}" writable
  config_get
  assert_safe_reboot_state false target writable false not_saved || \
    fail "safe-reboot did not restore final defaults"
  SAFE_RESTORE_REQUIRED=0
}

dangerous_pending() {
  if [ "$MODE" = "dry-run" ]; then
    plan "GET /config" "discover the confirmation-required switch route target"
    plan "PUT /config" "assert confirmation_required before dangerous save"
    plan "PUT /config" "requires --confirm-dangerous-save"
    plan "external reboot command" "reboot without applying dangerous pending state"
    plan "sleep 5" "wait for planned reboot"
    plan "GET /config" "assert dangerous item remains pending"
    plan "POST /config/apply" "assert confirmation_required before apply"
    plan "POST /config/apply" "requires --confirm-dangerous-apply"
    plan "GET /config" "assert dangerous item is applied after confirmation"
    plan "DELETE /config" "clear test snapshot without changing hardware"
    return
  fi
  config_get
  dangerous_id=$(select_config_item dangerous) || fail "missing or malformed dangerous USB target item"
  payload=$(printf '{"items":["%s"],"confirm":false}' "$dangerous_id")
  CLEANUP_REQUIRED=1
  http_request PUT /config "$payload" 409 confirmation_required config save \
    "dangerous save is rejected without confirmation" - "$dangerous_id"
  CLEANUP_REQUIRED=0
  save_item "$dangerous_id" true
  reboot_board
  config_get
  assert_config_item "$dangerous_id" pending || fail "dangerous item was not pending after reboot"
  http_request POST /config/apply '{"confirm":false}' 409 confirmation_required config apply \
    "dangerous apply is rejected without confirmation" - "$dangerous_id"
  apply_snapshot true
  config_get
  assert_config_item "$dangerous_id" applied || fail "dangerous item was not applied after confirmation"
  clear_snapshot
}

capture_busy() {
  if [ "$MODE" = "dry-run" ]; then
    plan "GET /config" "discover one safe firmware item with no snapshot"
    plan "PUT /config" "save a safe snapshot before capture"
    plan "GET /config" "assert the prepared snapshot is present"
    plan "external capture start command" "hold capture ownership"
    plan "PUT /config" "assert HTTP 409 busy activity=capture for save"
    plan "DELETE /config" "assert HTTP 409 busy activity=capture for present snapshot clear"
    plan "GET /config" "assert bounded GET remains 200 with the snapshot present"
    plan "external capture stop command" "release capture ownership"
    plan "DELETE /config" "clear the prepared snapshot after capture release"
    plan "GET /config" "assert cleanup left no snapshot"
    return
  fi
  config_get
  safe_id=$(select_config_item safe) || fail "missing or malformed safe config item"
  save_item "$safe_id" false
  config_get
  assert_config_item "$safe_id" applied || fail "capture-busy did not prepare a safe snapshot"
  "$CAPTURE_START"
  CAPTURE_ACTIVE=1
  evidence "external capture start command" "not_applicable" "ok" "capture ownership acquired"
  payload=$(printf '{"items":["%s"],"confirm":false}' "$safe_id")
  http_request PUT /config "$payload" 409 busy config save \
    "busy blocks config save while capture owns the arbiter" capture -
  http_request DELETE /config "" 409 busy config clear \
    "busy blocks present snapshot clear while capture owns the arbiter" capture -
  config_get
  assert_config_item "$safe_id" applied || fail "capture-busy GET did not retain the prepared snapshot"
  "$CAPTURE_STOP"
  CAPTURE_ACTIVE=0
  evidence "external capture stop command" "not_applicable" "ok" "capture ownership released"
  clear_snapshot
  config_get
  assert_snapshot_absent || fail "capture-busy cleanup did not clear the snapshot"
}

ota_preserve() {
  if [ "$MODE" = "dry-run" ]; then
    plan "GET /config" "discover one safe firmware item"
    plan "PUT /config" "save the discovered safe item"
    plan "GET /config" "assert the safe snapshot is prepared"
    plan "background POST /ota/upload" "start only the canonical MCUboot bin upload"
    plan "bounded GET /ota" "observe state=uploading while the upload request is active"
    plan "PUT /config" "assert HTTP 409 busy activity=ota for existing-snapshot save"
    plan "DELETE /config" "assert HTTP 409 busy activity=ota for present snapshot clear"
    plan "GET /config" "assert bounded read remains available with the snapshot present"
    plan "POST /config/apply" "assert confirm=false is a bounded noop while upload is active"
    plan "await POST /ota/upload" "validate HTTP 200 state=verified before continuing"
    plan "POST /ota/test" "request OTA test boot"
    plan "sleep 5" "wait for planned reboot"
    plan "GET /config" "assert saved safe item survives OTA boot"
    plan "POST /ota/confirm" "confirm the tested OTA image"
    plan "DELETE /config" "clear test snapshot without changing hardware"
    return
  fi
  config_get
  safe_id=$(select_config_item safe) || fail "missing or malformed safe config item"
  save_item "$safe_id" false
  config_get
  assert_config_item "$safe_id" applied || fail "ota-preserve did not prepare a safe snapshot"
  start_ota_upload
  wait_for_ota_uploading
  payload=$(printf '{"items":["%s"],"confirm":false}' "$safe_id")
  require_ota_upload_active
  http_request PUT /config "$payload" 409 busy config save \
    "busy blocks config save while OTA owns flash" ota -
  require_ota_upload_active
  http_request DELETE /config "" 409 busy config clear \
    "busy blocks present snapshot clear while OTA owns flash" ota -
  require_ota_upload_active
  config_get
  assert_config_item "$safe_id" applied || fail "OTA-active GET did not retain the prepared snapshot"
  require_ota_upload_active
  http_request POST /config/apply '{"confirm":false}' 200 ok config apply \
    "safe no-op apply remains bounded while OTA owns flash"
  python3 - "$HTTP_BODY" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    document = json.load(source)
if document.get("noop") is not True:
    raise SystemExit("OTA-active config apply was not a no-op")
PY
  await_ota_upload
  http_request POST /ota/test "" 202 ok ota - "request OTA test boot" - - rebooting
  sleep_for_reboot
  config_get
  assert_config_item "$safe_id" applied || fail "safe item did not survive OTA test boot"
  http_request POST /ota/confirm "" 200 ok ota - "confirm tested OTA image" - - idle
  clear_snapshot
}

bootsel_preserve() {
  if [ "$MODE" = "dry-run" ]; then
    plan "GET /config" "discover one safe firmware item"
    plan "PUT /config" "save the discovered safe item"
    plan "POST /bootloader" "enter ROM BOOTSEL"
    plan "sleep 5" "wait for reboot"
    plan "lsblk RPI-RP2 discovery" "discover BOOTSEL partition"
    plan "mount BOOTSEL partition" "mount BOOTSEL partition"
    plan "flash canonical combined UF2" "recover only with combined UF2"
    plan "sleep 5" "wait for reboot"
    plan "GET /config" "assert saved safe item survives recovery"
    plan "DELETE /config" "clear test snapshot without changing hardware"
    return
  fi
  config_get
  safe_id=$(select_config_item safe) || fail "missing or malformed safe config item"
  save_item "$safe_id" false
  bootloader_recovery
  config_get
  assert_config_item "$safe_id" applied || fail "safe item did not survive combined-UF2 recovery"
  clear_snapshot
}

cdc_fallback() {
  if [ "$MODE" = "dry-run" ]; then
    plan "GET /config" "discover one safe firmware item"
    plan "CDC config show" "verify CDC fallback is available"
    plan "CDC config save <firmware-item-id>" "save a firmware-enumerated safe item"
    plan "CDC config apply --confirm" "exercise confirmed apply grammar"
    plan "CDC config clear" "clear without changing hardware"
    plan "CDC bootloader" "enter ROM BOOTSEL through CDC fallback"
    plan "sleep 5" "wait for reboot"
    plan "lsblk RPI-RP2 discovery" "discover BOOTSEL partition"
    plan "mount BOOTSEL partition" "mount BOOTSEL partition"
    plan "flash canonical combined UF2" "recover only with combined UF2"
    plan "sleep 5" "wait for reboot"
    return
  fi
  config_get
  safe_id=$(select_config_item safe) || fail "missing or malformed safe config item"
  serial_request "config show" show
  CLEANUP_REQUIRED=1
  serial_request "config save $safe_id" save
  serial_request "config apply --confirm" apply
  serial_request "config clear" clear
  CLEANUP_REQUIRED=0
  cdc_bootloader_recovery
}

assert_final_cleanup_config() {
  python3 - "$HTTP_BODY" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    document = json.load(source)
snapshot = document.get("snapshot")
if not isinstance(snapshot, dict) or snapshot.get("present") is not False:
    raise SystemExit("final cleanup left a saved snapshot")
if document.get("pending") != 0:
    raise SystemExit("final cleanup left pending configuration")
items = document.get("items")
if not isinstance(items, list):
    raise SystemExit("config items is missing")
expected_routes = {
    "switch/usb": "target",
    "switch/sd": "target",
    "switch/tf_wp": "writable",
}
for item_id, route in expected_routes.items():
    matching = [item for item in items if isinstance(item, dict) and item.get("id") == item_id]
    if len(matching) != 1:
        raise SystemExit(f"final cleanup item is missing: {item_id}")
    current = matching[0].get("current")
    if not isinstance(current, dict) or current.get("route") != route:
        raise SystemExit(f"final cleanup route does not match for {item_id}")
PY
}

enumerated_power_cleanup() {
  http_request GET /power "" 200 ok power list \
    "enumerate firmware power outputs before final cleanup"
  unsafe_outputs=$(python3 - "$HTTP_BODY" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    document = json.load(source)
for output in document["power_outputs"]:
    if output["controllable"] and output["state"] != "off":
        name = output["name"]
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
            raise SystemExit("firmware power output name is unsafe for URL transport")
        print(name)
PY
  ) || fail "power output enumeration is malformed"
  for output_name in $unsafe_outputs; do
    http_request PUT "/power/$output_name" '{"state":"off"}' 200 ok power set \
      "restore firmware-enumerated power output to off"
  done
  http_request GET /power "" 200 ok power list \
    "reread firmware power outputs after final cleanup"
  python3 - "$HTTP_BODY" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    document = json.load(source)
if any(output["controllable"] and output["state"] != "off" for output in document["power_outputs"]):
    raise SystemExit("final cleanup left a controllable power output on")
PY
}

enumerated_gpio_cleanup() {
  http_request GET /gpio "" 200 ok gpio list \
    "enumerate firmware GPIO directions before final cleanup"
  output_gpios=$(python3 - "$HTTP_BODY" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    document = json.load(source)
for gpio in document["gpios"]:
    if gpio["direction"] == "output":
        name = gpio["name"]
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
            raise SystemExit("firmware GPIO name is unsafe for URL transport")
        print(name)
PY
  ) || fail "GPIO enumeration is malformed"
  for gpio_name in $output_gpios; do
    http_request PUT "/gpio/$gpio_name" '{"direction":"input"}' 200 ok gpio input \
      "restore firmware-enumerated GPIO to input"
  done
  http_request GET /gpio "" 200 ok gpio list \
    "reread firmware GPIO directions after final cleanup"
  python3 - "$HTTP_BODY" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    document = json.load(source)
if any(gpio["direction"] != "input" for gpio in document["gpios"]):
    raise SystemExit("final cleanup left a GPIO in output direction")
PY
}

final_cleanup() {
  if [ "$MODE" = "dry-run" ]; then
    plan "DELETE /config" "clear any remaining saved snapshot"
    plan "PUT /switch/usb" "restore USB target route"
    plan "PUT /switch/sd" "restore SD target route"
    plan "PUT /switch/tf_wp" "restore TF-WP writable route"
    plan "GET /switch/vin" "check final VIN safe state"
    plan "GET /power" "enumerate firmware power outputs"
    plan "PUT /power/<firmware-name>" "set each controllable on output to off"
    plan "GET /power" "prove every controllable output is off"
    plan "GET /gpio" "enumerate firmware GPIO directions"
    plan "PUT /gpio/<firmware-name>" "set each output-direction GPIO to input"
    plan "GET /gpio" "prove every GPIO direction is input"
    plan "GET /config" "assert absent snapshot and final safe switch routes"
    plan "CDC config show" "confirm CDC sees no saved snapshot"
    return
  fi
  CLEANUP_REQUIRED=1
  clear_snapshot
  switch_route usb target
  switch_route sd target
  switch_route tf_wp writable
  switch_get vin 3.3v
  enumerated_power_cleanup
  enumerated_gpio_cleanup
  config_get
  assert_final_cleanup_config || fail "final cleanup did not restore safe switch defaults"
  serial_request "config show" show
}

run_flow() {
  case "$1" in
    safe-reboot) safe_reboot ;;
    dangerous-pending) dangerous_pending ;;
    capture-busy) capture_busy ;;
    ota-preserve) ota_preserve ;;
    bootsel-preserve) bootsel_preserve ;;
    cdc-fallback) cdc_fallback ;;
    all)
      safe_reboot
      dangerous_pending
      capture_busy
      ota_preserve
      bootsel_preserve
      cdc_fallback
      final_cleanup
      ;;
  esac
}

prepare_execute
prepare_dry_run
if [ "$MODE" = "execute" ]; then
  TMPDIR_RUN=$(mktemp -d "${TMPDIR:-/tmp}/config-persistence-hil.XXXXXX") || fail "mktemp failed"
fi
run_flow "$FLOW"
