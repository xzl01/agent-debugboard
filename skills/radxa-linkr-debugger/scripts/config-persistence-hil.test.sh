#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/../../.." && pwd)
RUNNER="$ROOT/skills/radxa-linkr-debugger/scripts/config-persistence-hil.sh"
CANONICAL_UF2="build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2"
CANONICAL_OTA="build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/config-persistence-hil-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
STUBS="$WORK/stubs"
RESPONSES="$WORK/responses"
CALLS="$WORK/calls"
TIMEOUTS="$WORK/timeouts"
PARTITION_LOG="$WORK/partitions"
SERIAL="$WORK/ttyACM0"
MOUNT_POINT="$WORK/RPI-RP2"
PYTHON_STUB="$WORK/python"
REAL_SLEEP=$(command -v sleep)

fail() {
  printf '%s\n' "FAIL: $*" >&2
  exit 1
}

assert_empty() {
  [ ! -s "$1" ] || fail "expected no stub calls, found: $(cat "$1")"
}

assert_contains() {
  grep -F -- "$2" "$1" >/dev/null || fail "missing '$2' in $1"
}

assert_timeout() {
  method="$1"
  path="$2"
  payload="$3"
  max_time="$4"
  assert_contains "$TIMEOUTS" \
    "method=$method path=$path payload=$payload connect=2 max=$max_time"
}

assert_order() {
  output="$1"
  shift
  last=0
  for needle in "$@"; do
    line=$(awk -v start="$last" -v needle="$needle" 'NR > start && index($0, needle) { print NR; exit }' "$output")
    [ -n "$line" ] || fail "missing ordered operation '$needle' in $output"
    last="$line"
  done
}

assert_exact_requests() {
  output="$1"
  shift
  actual=$(awk -F '\t' '{ sub(/^request=/, "", $2); print $2 }' "$output")
  expected=$(printf '%s\n' "$@")
  [ "$actual" = "$expected" ] || fail "unexpected planned request sequence in $output"
}

assert_evidence_fields() {
  awk -F '\t' '
    NF != 5 || $1 !~ /^timestamp=/ || $2 !~ /^request=/ ||
    $3 !~ /^http_status=/ || $4 !~ /^code=/ || $5 !~ /^assertion=/ { exit 1 }
  ' "$1" || fail "evidence fields are not tab-delimited key/value records"
}

reset_fixture() {
  # POSIX shells may retain temporary assignments made before a shell function
  # call. Clear every fault-injection knob here so one negative fixture cannot
  # contaminate the fixtures that follow it.
  unset CONFIG_HIL_CLEANUP_ENUMERATION
  unset CONFIG_HIL_FLASH_FAIL
  unset CONFIG_HIL_LSBLK_LAYOUT
  unset CONFIG_HIL_OTA_ACTIVATE_AFTER
  unset CONFIG_HIL_OTA_APPLY_RESPONSE
  unset CONFIG_HIL_OTA_APPLY_STATUS
  unset CONFIG_HIL_OTA_BAD_STATE
  unset CONFIG_HIL_OTA_CONCURRENCY
  unset CONFIG_HIL_OTA_REQUIRE_LIMIT_RATE
  unset CONFIG_HIL_OTA_WRONG_ACTIVITY
  unset CONFIG_HIL_PYTHON_SERIAL_LATE_ERROR
  unset CONFIG_HIL_PYTHON_SERIAL_PROMPT_ONLY
  unset CONFIG_HIL_REBOOT_READINESS_INVALID
  unset CONFIG_HIL_REBOOT_TRANSPORT_FAILS
  unset CONFIG_HIL_SERIAL_BAD_BOOTSEL
  unset CONFIG_HIL_SERIAL_BAD_SHOW
  : > "$CALLS"
  : > "$TIMEOUTS"
  : > "$PARTITION_LOG"
  rm -rf "$RESPONSES" "$WORK/state"
  mkdir -p "$RESPONSES" "$WORK/state"
}

write_response() {
  number="$1"
  status="$2"
  body="$3"
  printf '%s\n' "$status" > "$RESPONSES/$number.status"
  printf '%s\n' "$body" > "$RESPONSES/$number.body"
}

run_fixture() {
  output="$1"
  shift
  (
    cd "$WORK"
    PATH="$STUBS:$PATH" \
      CONFIG_PERSISTENCE_HIL_CURL_BIN="$STUBS/curl" \
      CONFIG_PERSISTENCE_HIL_SLEEP_BIN="$STUBS/sleep" \
      CONFIG_PERSISTENCE_HIL_SERIAL_BIN="$STUBS/serial" \
      CONFIG_PERSISTENCE_HIL_LSBLK_BIN="$STUBS/lsblk" \
      CONFIG_PERSISTENCE_HIL_MOUNT_BIN="$STUBS/mount" \
      CONFIG_PERSISTENCE_HIL_FLASH_BIN="$STUBS/flash" \
      CONFIG_HIL_CALLS="$CALLS" \
      CONFIG_HIL_RESPONSES="$RESPONSES" \
      CONFIG_HIL_TIMEOUTS="$TIMEOUTS" \
      CONFIG_HIL_PARTITION_LOG="$PARTITION_LOG" \
      CONFIG_HIL_MOUNT_POINT="$MOUNT_POINT" \
      CONFIG_HIL_STATE_DIR="$WORK/state" \
      CONFIG_HIL_CLEANUP_ENUMERATION="${CONFIG_HIL_CLEANUP_ENUMERATION:-0}" \
      CONFIG_HIL_OTA_CONCURRENCY="${CONFIG_HIL_OTA_CONCURRENCY:-0}" \
      CONFIG_HIL_OTA_BAD_STATE="${CONFIG_HIL_OTA_BAD_STATE:-0}" \
      CONFIG_HIL_OTA_WRONG_ACTIVITY="${CONFIG_HIL_OTA_WRONG_ACTIVITY:-0}" \
      CONFIG_HIL_OTA_ACTIVATE_AFTER="${CONFIG_HIL_OTA_ACTIVATE_AFTER:-0}" \
      CONFIG_HIL_OTA_APPLY_RESPONSE="${CONFIG_HIL_OTA_APPLY_RESPONSE:-}" \
      CONFIG_HIL_OTA_APPLY_STATUS="${CONFIG_HIL_OTA_APPLY_STATUS:-200}" \
      CONFIG_HIL_OTA_REQUIRE_LIMIT_RATE="${CONFIG_HIL_OTA_REQUIRE_LIMIT_RATE:-0}" \
      CONFIG_HIL_REBOOT_TRANSPORT_FAILS="${CONFIG_HIL_REBOOT_TRANSPORT_FAILS:-0}" \
      CONFIG_HIL_LSBLK_LAYOUT="${CONFIG_HIL_LSBLK_LAYOUT:-default}" \
      CONFIG_HIL_FLASH_FAIL="${CONFIG_HIL_FLASH_FAIL:-0}" \
      CONFIG_HIL_REAL_SLEEP="$REAL_SLEEP" \
      sh "$RUNNER" "$@"
  ) > "$output" 2>&1
}

run_fixture_default_serial() {
  output="$1"
  shift
  (
    cd "$WORK"
    unset CONFIG_PERSISTENCE_HIL_SERIAL_BIN
    PATH="$STUBS:$PATH" \
      PYTHONPATH="$PYTHON_STUB${PYTHONPATH:+:$PYTHONPATH}" \
      CONFIG_PERSISTENCE_HIL_CURL_BIN="$STUBS/curl" \
      CONFIG_PERSISTENCE_HIL_SLEEP_BIN="$STUBS/sleep" \
      CONFIG_PERSISTENCE_HIL_LSBLK_BIN="$STUBS/lsblk" \
      CONFIG_PERSISTENCE_HIL_MOUNT_BIN="$STUBS/mount" \
      CONFIG_PERSISTENCE_HIL_FLASH_BIN="$STUBS/flash" \
      CONFIG_HIL_CALLS="$CALLS" \
      CONFIG_HIL_RESPONSES="$RESPONSES" \
      CONFIG_HIL_TIMEOUTS="$TIMEOUTS" \
      CONFIG_HIL_PARTITION_LOG="$PARTITION_LOG" \
      CONFIG_HIL_MOUNT_POINT="$MOUNT_POINT" \
      CONFIG_HIL_STATE_DIR="$WORK/state" \
      CONFIG_HIL_CLEANUP_ENUMERATION="${CONFIG_HIL_CLEANUP_ENUMERATION:-0}" \
      CONFIG_HIL_OTA_CONCURRENCY="${CONFIG_HIL_OTA_CONCURRENCY:-0}" \
      CONFIG_HIL_OTA_BAD_STATE="${CONFIG_HIL_OTA_BAD_STATE:-0}" \
      CONFIG_HIL_OTA_WRONG_ACTIVITY="${CONFIG_HIL_OTA_WRONG_ACTIVITY:-0}" \
      CONFIG_HIL_OTA_ACTIVATE_AFTER="${CONFIG_HIL_OTA_ACTIVATE_AFTER:-0}" \
      CONFIG_HIL_OTA_APPLY_RESPONSE="${CONFIG_HIL_OTA_APPLY_RESPONSE:-}" \
      CONFIG_HIL_OTA_APPLY_STATUS="${CONFIG_HIL_OTA_APPLY_STATUS:-200}" \
      CONFIG_HIL_OTA_REQUIRE_LIMIT_RATE="${CONFIG_HIL_OTA_REQUIRE_LIMIT_RATE:-0}" \
      CONFIG_HIL_REBOOT_TRANSPORT_FAILS="${CONFIG_HIL_REBOOT_TRANSPORT_FAILS:-0}" \
      CONFIG_HIL_LSBLK_LAYOUT="${CONFIG_HIL_LSBLK_LAYOUT:-default}" \
      CONFIG_HIL_FLASH_FAIL="${CONFIG_HIL_FLASH_FAIL:-0}" \
      CONFIG_HIL_REAL_SLEEP="$REAL_SLEEP" \
      sh "$RUNNER" "$@"
  ) > "$output" 2>&1
}

mkdir -p "$STUBS" "$RESPONSES" "$MOUNT_POINT" "$PYTHON_STUB" \
  "$WORK/build/radxa_linkr_debugger"
printf 'combined' > "$WORK/$CANONICAL_UF2"
printf 'ota' > "$WORK/$CANONICAL_OTA"
: > "$SERIAL"

cat > "$STUBS/curl" <<'STUB'
#!/bin/sh
set -eu
body=""
method=GET
url=""
payload=""
limit_rate=""
connect_timeout=""
max_time=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --output) body="$2"; shift 2 ;;
    --request) method="$2"; shift 2 ;;
    --header|--write-out) shift 2 ;;
    --connect-timeout) connect_timeout="$2"; shift 2 ;;
    --max-time) max_time="$2"; shift 2 ;;
    --limit-rate) limit_rate="$2"; shift 2 ;;
    --data|--data-binary) payload="$2"; shift 2 ;;
    --silent|--show-error) shift ;;
    *) url="$1"; shift ;;
  esac
done
case "$url" in
  http://fixture.invalid/api/v1/*) ;;
  *)
    printf '%s\n' "fixture curl rejected non-local URL: $url" >&2
    exit 1
    ;;
esac
respond() {
  printf '%s\n' "$2" > "$body"
  printf '%s' "$1"
  exit 0
}

schema='radxa-linkr-debugger.v1'
path=${url#http://fixture.invalid/api/v1}
if [ "$method $path" = 'POST /bootloader' ]; then
  : > "$CONFIG_HIL_STATE_DIR/bootsel-entered"
  : > "$CONFIG_HIL_STATE_DIR/reboot-readiness-pending"
fi
if [ "$method $path" = 'GET /config' ] && [ -f "$CONFIG_HIL_STATE_DIR/ota-test-requested" ]; then
  attempts_file="$CONFIG_HIL_STATE_DIR/reboot-transport-attempts"
  attempts=0
  [ ! -f "$attempts_file" ] || attempts=$(cat "$attempts_file")
  if [ "$attempts" -lt "${CONFIG_HIL_REBOOT_TRANSPORT_FAILS:-0}" ]; then
    attempts=$((attempts + 1))
    printf '%s\n' "$attempts" > "$attempts_file"
    printf '%s\n' 'fixture reboot transport unavailable' >&2
    exit 7
  fi
fi
if [ "$method $path" = 'GET /config' ] && [ -f "$CONFIG_HIL_STATE_DIR/reboot-readiness-pending" ]; then
  responses_file="$CONFIG_HIL_STATE_DIR/reboot-readiness-responses"
  responses=0
  [ ! -f "$responses_file" ] || responses=$(cat "$responses_file")
  printf '%s\n' "$((responses + 1))" > "$responses_file"
  rm -f "$CONFIG_HIL_STATE_DIR/reboot-readiness-pending"
  if [ "${CONFIG_HIL_REBOOT_READINESS_INVALID:-0}" = 1 ]; then
    respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"config\",\"action\":\"invalid\"}"
  fi
  respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"config\",\"action\":\"get\",\"backend\":{\"available\":true,\"reason\":\"ready\"},\"snapshot\":{\"present\":false,\"version\":null},\"pending\":0,\"items\":[{\"id\":\"switch/sd\",\"kind\":\"switch\",\"current\":{\"route\":\"target\"},\"saved\":null,\"selected\":false,\"requires_confirm\":false,\"apply_state\":\"not_saved\"}]}"
fi
number=$(awk '/^curl / { count++ } END { print count + 1 }' "$CONFIG_HIL_CALLS")
printf 'curl %s %s %s\n' "$method" "$url" "$payload" >> "$CONFIG_HIL_CALLS"
printf 'method=%s path=%s payload=%s connect=%s max=%s\n' \
  "$method" "$path" "$payload" "$connect_timeout" "$max_time" >> "$CONFIG_HIL_TIMEOUTS"
if [ "${CONFIG_HIL_OTA_CONCURRENCY:-0}" = 1 ] && \
  [ "$number" -gt "${CONFIG_HIL_OTA_ACTIVATE_AFTER:-0}" ] && \
  [ ! -f "$CONFIG_HIL_STATE_DIR/ota-flow.complete" ]; then
  case "$method $path" in
    'POST /ota/upload')
      printf '%s\n' "$$" > "$CONFIG_HIL_STATE_DIR/ota-upload.pid"
      : > "$CONFIG_HIL_STATE_DIR/ota-upload.started"
      if [ "${CONFIG_HIL_OTA_REQUIRE_LIMIT_RATE:-0}" = 1 ] && [ "$limit_rate" != 64K ]; then
        printf '%s\n' 'fixture OTA upload requires --limit-rate 64K' >&2
        exit 1
      fi
      attempts=0
      while [ "$attempts" -lt 100 ]; do
        if awk '
          /curl POST http:\/\/fixture.invalid\/api\/v1\/ota\/upload / {
            active = 1
            next
          }
          active && /curl GET http:\/\/fixture.invalid\/api\/v1\/ota / { status = 1 }
          active && /curl PUT http:\/\/fixture.invalid\/api\/v1\/config / { save = 1 }
          active && /curl DELETE http:\/\/fixture.invalid\/api\/v1\/config / { clear = 1 }
          active && clear && /curl GET http:\/\/fixture.invalid\/api\/v1\/config / { retained = 1 }
          END { exit !(status && save && clear && retained) }
        ' "$CONFIG_HIL_CALLS"; then
          : > "$CONFIG_HIL_STATE_DIR/ota-upload.complete"
          respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"ota\",\"state\":\"verified\"}"
        fi
        attempts=$((attempts + 1))
        "$CONFIG_HIL_REAL_SLEEP" 0.01
      done
      printf '%s\n' 'fixture OTA upload timed out before required interleaved calls' >&2
      exit 1
      ;;
    'GET /ota')
      state=uploading
      [ "${CONFIG_HIL_OTA_BAD_STATE:-0}" = 0 ] || state=verified
      respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"ota\",\"state\":\"$state\"}"
      ;;
    'PUT /config')
      put_count=$(grep -F -c 'curl PUT http://fixture.invalid/api/v1/config ' "$CONFIG_HIL_CALLS")
      if [ "$put_count" -eq 1 ]; then
        respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"config\",\"action\":\"save\",\"saved_items\":[\"switch/sd\"],\"confirmation_items\":[],\"applied_items\":[\"switch/sd\"],\"snapshot\":{\"present\":true,\"version\":1},\"pending\":0}"
      fi
      activity=ota
      [ "${CONFIG_HIL_OTA_WRONG_ACTIVITY:-0}" = 0 ] || activity=capture
      respond 409 "{\"schema\":\"$schema\",\"ok\":false,\"command\":\"config\",\"action\":\"save\",\"error\":{\"code\":\"busy\",\"message\":\"OTA active\"},\"activity\":\"$activity\"}"
      ;;
    'DELETE /config')
      if [ -f "$CONFIG_HIL_STATE_DIR/ota-upload.started" ] && [ ! -f "$CONFIG_HIL_STATE_DIR/ota-upload.complete" ]; then
        activity=ota
        [ "${CONFIG_HIL_OTA_WRONG_ACTIVITY:-0}" = 0 ] || activity=capture
        respond 409 "{\"schema\":\"$schema\",\"ok\":false,\"command\":\"config\",\"action\":\"clear\",\"error\":{\"code\":\"busy\",\"message\":\"OTA active\"},\"activity\":\"$activity\"}"
      fi
      : > "$CONFIG_HIL_STATE_DIR/ota-flow.complete"
      respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"config\",\"action\":\"clear\",\"noop\":false,\"snapshot\":{\"present\":false,\"version\":null},\"pending\":0}"
      ;;
    'POST /ota/test')
      : > "$CONFIG_HIL_STATE_DIR/ota-test-requested"
      : > "$CONFIG_HIL_STATE_DIR/reboot-readiness-pending"
      respond 202 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"ota\",\"state\":\"rebooting\"}"
      ;;
    'POST /ota/confirm')
      respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"ota\",\"state\":\"idle\"}"
      ;;
    'GET /config')
      if [ "$(grep -F -c 'curl GET http://fixture.invalid/api/v1/config ' "$CONFIG_HIL_CALLS")" -eq 1 ]; then
        respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"config\",\"action\":\"get\",\"backend\":{\"available\":true,\"reason\":\"absent\"},\"snapshot\":{\"present\":false,\"version\":null},\"pending\":0,\"items\":[{\"id\":\"switch/sd\",\"kind\":\"switch\",\"current\":{\"route\":\"target\"},\"saved\":null,\"selected\":false,\"requires_confirm\":false,\"apply_state\":\"not_saved\"}]}"
      fi
      respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"config\",\"action\":\"get\",\"backend\":{\"available\":true,\"reason\":\"ready\"},\"snapshot\":{\"present\":true,\"version\":1},\"pending\":0,\"items\":[{\"id\":\"switch/sd\",\"kind\":\"switch\",\"current\":{\"route\":\"target\"},\"saved\":{\"route\":\"target\"},\"selected\":true,\"requires_confirm\":false,\"apply_state\":\"applied\"}]}"
      ;;
  esac
fi

if [ "${CONFIG_HIL_CLEANUP_ENUMERATION:-0}" = 1 ] && [ -f "$CONFIG_HIL_STATE_DIR/final-cleanup.ready" ]; then
  case "$method $path" in
    'DELETE /config') respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"config\",\"action\":\"clear\",\"noop\":false,\"snapshot\":{\"present\":false,\"version\":null},\"pending\":0}" ;;
    'PUT /switch/usb') respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"switch\",\"action\":\"route\",\"name\":\"usb\",\"route\":\"target\"}" ;;
    'PUT /switch/sd') respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"switch\",\"action\":\"route\",\"name\":\"sd\",\"route\":\"target\"}" ;;
    'PUT /switch/tf_wp') respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"switch\",\"action\":\"route\",\"name\":\"tf_wp\",\"route\":\"writable\"}" ;;
    'GET /switch/vin') respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"switch\",\"action\":\"get\",\"name\":\"vin\",\"route\":\"3.3v\"}" ;;
    'GET /power')
      power_count=$(grep -F -c 'curl GET http://fixture.invalid/api/v1/power ' "$CONFIG_HIL_CALLS")
      state=on
      [ "$power_count" -eq 1 ] || state=off
      value=1
      [ "$state" = on ] || value=0
      respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"power\",\"action\":\"list\",\"power_outputs\":[{\"name\":\"fixture_rail\",\"controllable\":true,\"state\":\"$state\",\"value\":$value},{\"name\":\"fixture_input\",\"controllable\":false,\"state\":\"locked\",\"value\":null}]}"
      ;;
    'PUT /power/fixture_rail') respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"power\",\"action\":\"set\",\"power_output\":{\"name\":\"fixture_rail\",\"controllable\":true,\"state\":\"off\",\"value\":0}}" ;;
    'GET /gpio')
      gpio_count=$(grep -F -c 'curl GET http://fixture.invalid/api/v1/gpio ' "$CONFIG_HIL_CALLS")
      direction=output
      [ "$gpio_count" -eq 1 ] || direction=input
      value=1
      [ "$direction" = output ] || value=0
      respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"gpio\",\"action\":\"list\",\"gpios\":[{\"name\":\"FIXTURE_GPIO\",\"direction\":\"$direction\",\"value\":$value},{\"name\":\"FIXTURE_INPUT\",\"direction\":\"input\",\"value\":0}]}"
      ;;
    'PUT /gpio/FIXTURE_GPIO') respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"gpio\",\"action\":\"input\",\"gpio\":{\"name\":\"FIXTURE_GPIO\",\"direction\":\"input\",\"value\":null}}" ;;
    'GET /config') respond 200 "{\"schema\":\"$schema\",\"ok\":true,\"command\":\"config\",\"action\":\"get\",\"backend\":{\"available\":true,\"reason\":\"absent\"},\"snapshot\":{\"present\":false,\"version\":null},\"pending\":0,\"items\":[{\"id\":\"switch/usb\",\"kind\":\"switch\",\"current\":{\"route\":\"target\"},\"saved\":null,\"selected\":false,\"requires_confirm\":true,\"apply_state\":\"not_saved\"},{\"id\":\"switch/sd\",\"kind\":\"switch\",\"current\":{\"route\":\"target\"},\"saved\":null,\"selected\":false,\"requires_confirm\":false,\"apply_state\":\"not_saved\"},{\"id\":\"switch/tf_wp\",\"kind\":\"switch\",\"current\":{\"route\":\"writable\"},\"saved\":null,\"selected\":false,\"requires_confirm\":false,\"apply_state\":\"not_saved\"}]}" ;;
  esac
fi
cat "$CONFIG_HIL_RESPONSES/$number.body" > "$body"
cat "$CONFIG_HIL_RESPONSES/$number.status"
STUB
cat > "$STUBS/serial" <<'STUB'
#!/bin/sh
printf 'serial %s %s\n' "$1" "$2" >> "$CONFIG_HIL_CALLS"
if [ "${CONFIG_HIL_SERIAL_BAD_BOOTSEL:-0}" = 1 ] && [ "$2" = bootloader ]; then
  printf '%s\n' 'linkr-debugger:~$ bootloader'
  exit 0
fi
if [ "${CONFIG_HIL_SERIAL_BAD_SHOW:-0}" = 1 ] && [ "$2" = 'config show' ]; then
  printf '%s\n' 'linkr-debugger:~$ config show'
  exit 0
fi
case "$2" in
  'config show') printf '%s\n' 'config available=true reason=ready saved_count=0 pending_count=0' ;;
  'config save '*) printf '%s\n' 'config save saved_count=1 pending_count=0' ;;
  'config clear') printf '%s\n' 'config clear hardware_changed=false' ;;
  bootloader)
    [ "${CONFIG_HIL_CLEANUP_ENUMERATION:-0}" = 0 ] || : > "$CONFIG_HIL_STATE_DIR/final-cleanup.ready"
    : > "$CONFIG_HIL_STATE_DIR/bootsel-entered"
    : > "$CONFIG_HIL_STATE_DIR/reboot-readiness-pending"
    printf '%s\n' 'Entering RP2350 BOOTSEL in 250 ms...'
    ;;
  *) printf '%s\n' 'unexpected command' ;;
esac
STUB
cat > "$STUBS/sleep" <<'STUB'
#!/bin/sh
printf 'sleep %s\n' "$1" >> "$CONFIG_HIL_CALLS"
STUB
cat > "$STUBS/lsblk" <<'STUB'
#!/bin/sh
set -eu

layout=${CONFIG_HIL_LSBLK_LAYOUT:-default}
entered=0
[ ! -f "$CONFIG_HIL_STATE_DIR/bootsel-entered" ] || entered=1
printf 'layout=%s entered=%s\n' "$layout" "$entered" >> "$CONFIG_HIL_PARTITION_LOG"

emit_rpi_partition() {
  disk="$1"
  printf '%s\n' "NAME=\"$disk\" VENDOR=\"RPI\" TYPE=\"disk\" PKNAME=\"\""
  printf '%s\n' "NAME=\"${disk}1\" VENDOR=\"\" TYPE=\"part\" PKNAME=\"$disk\""
}

case "$layout:$entered" in
  default:1) emit_rpi_partition sdb ;;
  existing-and-new:0) emit_rpi_partition sda ;;
  existing-and-new:1)
    emit_rpi_partition sda
    emit_rpi_partition sdb
    ;;
  multiple-new:1)
    emit_rpi_partition sdb
    emit_rpi_partition sdc
    ;;
esac
STUB
cat > "$STUBS/mount" <<'STUB'
#!/bin/sh
set -eu
case "$1" in
  mount)
    printf 'mount %s %s %s\n' "$1" "$2" "$3" >> "$CONFIG_HIL_CALLS"
    printf 'Mounted %s at %s\n' "$3" "$CONFIG_HIL_MOUNT_POINT"
    ;;
  unmount)
    printf 'unmount %s %s %s\n' "$1" "$2" "$3" >> "$CONFIG_HIL_CALLS"
    ;;
  *)
    printf '%s\n' "unexpected mount operation: $1" >&2
    exit 1
    ;;
esac
STUB
cat > "$STUBS/flash" <<'STUB'
#!/bin/sh
set -eu
printf 'flash %s %s\n' "$1" "$2" >> "$CONFIG_HIL_CALLS"
[ "${CONFIG_HIL_FLASH_FAIL:-0}" = 0 ] || exit 1
rm -f "$CONFIG_HIL_STATE_DIR/bootsel-entered"
: > "$CONFIG_HIL_STATE_DIR/reboot-readiness-pending"
STUB
cat > "$STUBS/reboot" <<'STUB'
#!/bin/sh
printf '%s\n' reboot >> "$CONFIG_HIL_CALLS"
: > "$CONFIG_HIL_STATE_DIR/reboot-readiness-pending"
STUB
cat > "$STUBS/capture-start" <<'STUB'
#!/bin/sh
printf '%s\n' capture-start >> "$CONFIG_HIL_CALLS"
STUB
cat > "$STUBS/capture-stop" <<'STUB'
#!/bin/sh
printf '%s\n' capture-stop >> "$CONFIG_HIL_CALLS"
STUB
cat > "$PYTHON_STUB/serial.py" <<'STUB'
import os


class Serial:
    def __init__(self, device, baudrate, timeout):
        self.command = ""
        self.responses = []

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        return False

    def reset_input_buffer(self):
        return None

    def write(self, payload):
        self.command = payload.decode("utf-8").strip()
        with open(os.environ["CONFIG_HIL_CALLS"], "a", encoding="utf-8") as output:
            output.write(f"python-serial {self.command}\n")
        if self.command == "config show" and os.environ.get("CONFIG_HIL_PYTHON_SERIAL_PROMPT_ONLY") == "1":
            self.responses = [b"linkr-debugger:~$ config show\n"]
        elif self.command == "config show" and os.environ.get("CONFIG_HIL_PYTHON_SERIAL_LATE_ERROR") == "1":
            self.responses = [
                b"config available=true reason=ready saved_count=0 pending_count=0\n",
                b"config item switch/sd current=target\n",
                b"ERROR: config backend unavailable\n",
            ]
        elif self.command == "bootloader":
            with open(os.path.join(os.environ["CONFIG_HIL_STATE_DIR"], "bootsel-entered"), "w", encoding="utf-8"):
                pass
            with open(os.path.join(os.environ["CONFIG_HIL_STATE_DIR"], "reboot-readiness-pending"), "w", encoding="utf-8"):
                pass
            self.responses = [b"Entering RP2350 BOOTSEL in 250 ms...\n"]
        elif self.command == "config show":
            self.responses = [b"config available=true reason=ready saved_count=0 pending_count=0\n"]
        elif self.command.startswith("config save "):
            self.responses = [b"config save saved_count=1 pending_count=0\n"]
        else:
            self.responses = [b"config clear hardware_changed=false\n"]

    def flush(self):
        return None

    def read(self, size=1):
        if self.responses:
            return self.responses.pop(0)
        return b"linkr-debugger:~$ "

    def readline(self):
        if self.responses:
            return self.responses.pop(0)
        return b""
STUB
chmod 755 "$STUBS"/*

reset_fixture
if run_fixture "$WORK/default.out"; then
  fail "default invocation unexpectedly succeeded without a flow"
fi
assert_empty "$CALLS"

reset_fixture
run_fixture "$WORK/all.out" --dry-run --serial "$SERIAL" --combined-uf2 "$CANONICAL_UF2" \
  --ota-image "$CANONICAL_OTA" all
assert_empty "$CALLS"
assert_evidence_fields "$WORK/all.out"

for flow in safe-reboot dangerous-auto-restore capture-busy ota-preserve bootsel-preserve cdc-fallback; do
  reset_fixture
  run_fixture "$WORK/$flow.out" --dry-run --serial "$SERIAL" --combined-uf2 "$CANONICAL_UF2" \
    --ota-image "$CANONICAL_OTA" "$flow"
  assert_empty "$CALLS"
  assert_contains "$WORK/$flow.out" 'http_status=planned'
  assert_evidence_fields "$WORK/$flow.out"
done

assert_exact_requests "$WORK/safe-reboot.out" \
  'GET /config' 'PUT /switch/sd' 'PUT /switch/tf_wp' 'GET /config' 'PUT /config' \
  'external reboot command' 'sleep 5' 'GET /config' 'external reboot command' 'sleep 5' \
  'GET /config' 'DELETE /config' 'GET /config' \
  'PUT /switch/sd' 'PUT /switch/tf_wp' 'GET /config'
assert_exact_requests "$WORK/dangerous-auto-restore.out" \
  'GET /config' 'PUT /switch/usb' 'PUT /config' 'PUT /config' 'external reboot command' 'sleep 5' 'GET /config' \
  'external reboot command' 'sleep 5' 'GET /config' 'DELETE /config'
assert_exact_requests "$WORK/capture-busy.out" \
  'GET /config' 'PUT /config' 'GET /config' 'external capture start command' 'PUT /config' \
  'DELETE /config' 'GET /config' 'external capture stop command' 'DELETE /config' 'GET /config'
assert_exact_requests "$WORK/ota-preserve.out" \
  'GET /config' 'PUT /config' 'GET /config' 'background POST /ota/upload' \
  'bounded GET /ota' 'PUT /config' 'DELETE /config' 'GET /config' \
  'await POST /ota/upload' 'POST /ota/test' 'sleep 5' 'GET /config' 'POST /ota/confirm' \
  'DELETE /config'
assert_exact_requests "$WORK/bootsel-preserve.out" \
  'GET /config' 'PUT /config' 'POST /bootloader' 'sleep 5' 'lsblk RPI-RP2 discovery' \
  'mount BOOTSEL partition' 'flash canonical combined UF2' 'sleep 5' 'GET /config' 'DELETE /config'
assert_exact_requests "$WORK/cdc-fallback.out" \
  'GET /config' 'CDC config show' 'CDC config save <firmware-item-id>' \
  'CDC config clear' 'CDC bootloader' 'sleep 5' 'lsblk RPI-RP2 discovery' 'mount BOOTSEL partition' \
  'flash canonical combined UF2' 'sleep 5'
all_requests=$(awk -F '\t' '{ sub(/^request=/, "", $2); print $2 }' "$WORK/all.out")
flow_requests=$(for flow in safe-reboot dangerous-auto-restore capture-busy ota-preserve bootsel-preserve cdc-fallback; do
  awk -F '\t' '{ sub(/^request=/, "", $2); print $2 }' "$WORK/$flow.out"
done)
final_cleanup_requests=$(printf '%s\n' \
  'DELETE /config' 'PUT /switch/usb' 'PUT /switch/sd' 'PUT /switch/tf_wp' \
  'GET /switch/vin' 'GET /power' 'PUT /power/<firmware-name>' 'GET /power' \
  'GET /gpio' 'PUT /gpio/<firmware-name>' 'GET /gpio' 'GET /config' 'CDC config show')
expected_all_requests=$(printf '%s\n%s\n' "$flow_requests" "$final_cleanup_requests")
[ "$all_requests" = "$expected_all_requests" ] || fail "all dry-run request plan differs from its complete flow sequence"

valid_config='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"absent"},"snapshot":{"present":false,"version":null},"pending":0,"items":[{"id":"switch/sd","kind":"switch","current":{"route":"target"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"},{"id":"switch/usb","kind":"switch","current":{"route":"target"},"saved":null,"selected":false,"requires_confirm":true,"apply_state":"not_saved"}]}'
multiple_safe_config='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"absent"},"snapshot":{"present":false,"version":null},"pending":0,"items":[{"id":"switch/sd","kind":"switch","current":{"route":"target"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"},{"id":"switch/tf_wp","kind":"switch","current":{"route":"writable"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"},{"id":"switch/usb","kind":"switch","current":{"route":"target"},"saved":null,"selected":false,"requires_confirm":true,"apply_state":"not_saved"}]}'
saved_first_safe_config='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"switch/sd","kind":"switch","current":{"route":"target"},"saved":{"route":"target"},"selected":true,"requires_confirm":false,"apply_state":"applied"},{"id":"switch/tf_wp","kind":"switch","current":{"route":"writable"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"},{"id":"switch/usb","kind":"switch","current":{"route":"target"},"saved":null,"selected":false,"requires_confirm":true,"apply_state":"not_saved"}]}'
save_ok='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save","saved_items":["switch/sd"],"confirmation_items":[],"applied_items":["switch/sd"],"snapshot":{"present":true,"version":1},"pending":0}'
clear_ok='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"clear","noop":false,"snapshot":{"present":false,"version":null},"pending":0}'
switch_usb_pc_ok='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"switch","action":"route","name":"usb","route":"pc"}'
busy_config='{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"busy","message":"capture active"},"activity":"capture"}'

reset_fixture
write_response 1 200 "$valid_config"
if ! run_fixture_default_serial "$WORK/default-serial.out" --execute --url http://fixture.invalid \
  --serial "$SERIAL" --combined-uf2 "$CANONICAL_UF2" cdc-fallback; then
  cat "$WORK/default-serial.out" >&2
  fail "default Python CDC helper failed"
fi
assert_order "$CALLS" \
  'python-serial config show' \
  'python-serial config save switch/sd' \
  'python-serial config clear' \
  'python-serial bootloader' \
  'mount mount -b /dev/sdb1' \
  "flash $CANONICAL_UF2 $MOUNT_POINT/"

reset_fixture
if run_fixture "$WORK/missing-save-confirm.out" --execute --url http://fixture.invalid \
  --reboot-command "$STUBS/reboot" dangerous-auto-restore; then
  fail "dangerous flow unexpectedly accepted a missing save confirmation"
fi
assert_contains "$WORK/missing-save-confirm.out" '--confirm-dangerous-save'
assert_empty "$CALLS"

reset_fixture
assert_empty "$CALLS"

reset_fixture
CONFIG_HIL_SERIAL_BAD_BOOTSEL=1 run_fixture "$WORK/bad-bootsel.out" --execute --url http://fixture.invalid \
  --serial "$SERIAL" --combined-uf2 "$CANONICAL_UF2" cdc-fallback && \
  fail "CDC prompt echo unexpectedly entered BOOTSEL recovery"
! grep -E '^(mount|flash) ' "$CALLS" >/dev/null || fail "invalid CDC BOOTSEL response mounted or flashed"

reset_fixture
write_response 1 200 "$valid_config"
CONFIG_HIL_SERIAL_BAD_SHOW=1 run_fixture "$WORK/bad-show.out" --execute --url http://fixture.invalid \
  --serial "$SERIAL" --combined-uf2 "$CANONICAL_UF2" cdc-fallback && \
  fail "CDC prompt echo unexpectedly passed config show"
! grep -F 'config save ' "$CALLS" >/dev/null || fail "invalid CDC config show advanced to save"
! grep -E '^(mount|flash) ' "$CALLS" >/dev/null || fail "invalid CDC config show mounted or flashed"

reset_fixture
write_response 1 200 "$valid_config"
write_response 2 200 "$save_ok"
write_response 3 200 '{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"ota"}'
write_response 4 200 "$clear_ok"
if run_fixture "$WORK/wrong-bootloader-command.out" --execute --url http://fixture.invalid \
  --combined-uf2 "$CANONICAL_UF2" bootsel-preserve; then
  fail "wrong bootloader command unexpectedly succeeded"
fi
! grep -E '^(sleep|mount|flash) ' "$CALLS" >/dev/null || fail "wrong bootloader command advanced recovery"

reset_fixture
wrong_get_action='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save"}'
write_response 1 200 "$wrong_get_action"
if run_fixture "$WORK/wrong-action.out" --execute --url http://fixture.invalid \
  --reboot-command "$STUBS/reboot" safe-reboot; then
  fail "wrong config GET action unexpectedly succeeded"
fi
[ "$(wc -l < "$CALLS" | tr -d ' ')" = 1 ] || fail "wrong config action made an additional request"

reset_fixture
if (
  CONFIG_PERSISTENCE_HIL_SHA256_BIN="$STUBS/missing-sha" run_fixture "$WORK/all-preflight.out" \
    --execute --url http://fixture.invalid --serial "$SERIAL" --reboot-command "$STUBS/reboot" \
    --capture-start "$STUBS/capture-start" --capture-stop "$STUBS/capture-stop" \
    --confirm-dangerous-save --combined-uf2 "$CANONICAL_UF2" \
    --ota-image "$CANONICAL_OTA" all
); then
  fail "all unexpectedly ignored missing sha256 dependency"
fi
assert_empty "$CALLS"

reset_fixture
printf '%s\n' 'raise ImportError("fixture serial unavailable")' > "$PYTHON_STUB/serial.py"
if run_fixture_default_serial "$WORK/serial-import.out" --execute --url http://fixture.invalid \
  --serial "$SERIAL" --combined-uf2 "$CANONICAL_UF2" cdc-fallback; then
  fail "missing default PySerial dependency unexpectedly succeeded"
fi
assert_empty "$CALLS"
cat > "$PYTHON_STUB/serial.py" <<'STUB'
import os


class Serial:
    def __init__(self, device, baudrate, timeout):
        self.command = ""
        self.responses = []

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        return False

    def reset_input_buffer(self):
        return None

    def write(self, payload):
        self.command = payload.decode("utf-8").strip()
        with open(os.environ["CONFIG_HIL_CALLS"], "a", encoding="utf-8") as output:
            output.write(f"python-serial {self.command}\n")
        if self.command == "config show" and os.environ.get("CONFIG_HIL_PYTHON_SERIAL_PROMPT_ONLY") == "1":
            self.responses = [b"linkr-debugger:~$ config show\n"]
        elif self.command == "config show" and os.environ.get("CONFIG_HIL_PYTHON_SERIAL_LATE_ERROR") == "1":
            self.responses = [
                b"config available=true reason=ready saved_count=0 pending_count=0\n",
                b"config item switch/sd current=target\n",
                b"ERROR: config backend unavailable\n",
            ]
        elif self.command == "bootloader":
            with open(os.path.join(os.environ["CONFIG_HIL_STATE_DIR"], "bootsel-entered"), "w", encoding="utf-8"):
                pass
            with open(os.path.join(os.environ["CONFIG_HIL_STATE_DIR"], "reboot-readiness-pending"), "w", encoding="utf-8"):
                pass
            self.responses = [b"Entering RP2350 BOOTSEL in 250 ms...\n"]
        elif self.command == "config show":
            self.responses = [b"config available=true reason=ready saved_count=0 pending_count=0\n"]
        elif self.command.startswith("config save "):
            self.responses = [b"config save saved_count=1 pending_count=0\n"]
        else:
            self.responses = [b"config clear hardware_changed=false\n"]

    def flush(self):
        return None

    def read(self, size=1):
        if self.responses:
            return self.responses.pop(0)
        return b"linkr-debugger:~$ "

    def readline(self):
        if self.responses:
            return self.responses.pop(0)
        return b""
STUB

reset_fixture
write_response 1 200 '{}'
if run_fixture "$WORK/malformed.out" --execute --url http://fixture.invalid \
  --reboot-command "$STUBS/reboot" safe-reboot; then
  fail "malformed config response unexpectedly succeeded"
fi
assert_contains "$WORK/malformed.out" 'malformed response for GET /config'
[ "$(wc -l < "$CALLS" | tr -d ' ')" = 1 ] || fail "malformed response did not stop after one request"

reset_fixture
write_response 1 200 "$valid_config"
write_response 2 200 "$switch_usb_pc_ok"
write_response 3 409 '{}'
write_response 4 200 "$clear_ok"
if run_fixture "$WORK/dangerous-probe-ambiguous.out" --execute --url http://fixture.invalid \
  --reboot-command "$STUBS/reboot" --confirm-dangerous-save dangerous-auto-restore; then
  fail "ambiguous dangerous probe response unexpectedly succeeded"
fi
assert_order "$CALLS" \
  'curl GET http://fixture.invalid/api/v1/config' \
  'curl PUT http://fixture.invalid/api/v1/switch/usb {"route":"pc"}' \
  'curl PUT http://fixture.invalid/api/v1/config {"items":["switch/usb"],"confirm":false}' \
  'curl DELETE http://fixture.invalid/api/v1/config'
! grep -F reboot "$CALLS" >/dev/null || fail "ambiguous dangerous probe advanced before cleanup"

reset_fixture
write_response 1 200 "$multiple_safe_config"
write_response 2 200 "$save_ok"
write_response 3 200 "$saved_first_safe_config"
write_response 4 409 '{}'
write_response 5 200 "$clear_ok"
if run_fixture "$WORK/capture-save-ambiguous.out" --execute --url http://fixture.invalid \
  --capture-start "$STUBS/capture-start" --capture-stop "$STUBS/capture-stop" capture-busy; then
  fail "ambiguous capture save response unexpectedly succeeded"
fi
assert_order "$CALLS" \
  'curl GET http://fixture.invalid/api/v1/config' \
  'curl PUT http://fixture.invalid/api/v1/config {"items":["switch/sd"],"confirm":false}' \
  'curl GET http://fixture.invalid/api/v1/config' \
  'capture-start' \
  'curl PUT http://fixture.invalid/api/v1/config {"items":["switch/sd"],"confirm":false}' \
  'capture-stop' \
  'curl DELETE http://fixture.invalid/api/v1/config'

reset_fixture
if run_fixture "$WORK/missing-serial.out" --dry-run --serial "$WORK/missing-tty" cdc-fallback; then
  fail "missing CDC serial prerequisite unexpectedly succeeded"
fi
assert_contains "$WORK/missing-serial.out" 'CDC serial device is unavailable'
assert_empty "$CALLS"

reset_fixture
printf 'app-only' > "$WORK/zephyr.uf2"
if run_fixture "$WORK/unsafe-uf2.out" --dry-run --combined-uf2 zephyr.uf2 bootsel-preserve; then
  fail "app-only UF2 unexpectedly accepted"
fi
assert_contains "$WORK/unsafe-uf2.out" 'app-only zephyr.uf2 is forbidden'
assert_empty "$CALLS"

reset_fixture
if run_fixture "$WORK/unsafe-ota.out" --dry-run --ota-image zephyr.uf2 ota-preserve; then
  fail "UF2 OTA artifact unexpectedly accepted"
fi
assert_contains "$WORK/unsafe-ota.out" 'OTA rejects unsafe artifact type'
assert_empty "$CALLS"

assert_exact_calls() {
  output="$1"
  shift
  actual=$(awk '{$1 = $1; print}' "$output")
  expected=$(printf '%s\n' "$@")
  [ "$actual" = "$expected" ] || fail "unexpected stub call sequence in $output"
}

assert_request_suffix() {
  output="$1"
  shift
  actual=$(awk -F '\t' '{ sub(/^request=/, "", $2); print $2 }' "$output")
  expected=$(printf '%s\n' "$@")
  case "$actual" in
    *"$expected") ;;
    *) fail "missing exact final request sequence in $output" ;;
  esac
}

omit_response_field() {
  field="$1"
  document="$2"
  python3 - "$field" "$document" <<'PY'
import json
import sys

field, document = sys.argv[1:]
value = json.loads(document)
del value[field]
print(json.dumps(value, separators=(",", ":")))
PY
}

snapshot_with_version() {
  version="$1"
  document="$2"
  python3 - "$version" "$document" <<'PY'
import json
import sys

version, document = sys.argv[1:]
value = json.loads(document)
value["snapshot"]["version"] = int(version)
print(json.dumps(value, separators=(",", ":")))
PY
}

assert_rejected_envelope() {
  label="$1"
  expected_request="$2"
  shift 2
  output="$WORK/$label.runner.out"
  if run_fixture "$output" "$@"; then
    fail "$label unexpectedly completed with a malformed envelope"
  fi
  if ! grep -F -- "malformed response for $expected_request" "$output" >/dev/null; then
    printf '%s\n' "runner output for $label:" >&2
    cat "$output" >&2
    fail "$label did not reject the malformed $expected_request envelope"
  fi
}

RED_GAPS=0
red_case() {
  label="$1"
  shift
  output="$WORK/red-$label.assert.out"
  if ( "$@" ) > "$output" 2>&1; then
    printf '%s\n' "GREEN[$label]"
  else
    RED_GAPS=$((RED_GAPS + 1))
    printf '%s\n' "RED[$label]" >&2
    cat "$output" >&2
  fi
}

safe_config_prepared='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":false,"version":null},"pending":0,"items":[{"id":"switch/sd","kind":"switch","current":{"route":"usb-reader"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"},{"id":"switch/tf_wp","kind":"switch","current":{"route":"protected"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"},{"id":"switch/usb","kind":"switch","current":{"route":"target"},"saved":null,"selected":false,"requires_confirm":true,"apply_state":"not_saved"}]}'
safe_config_saved='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"switch/sd","kind":"switch","current":{"route":"usb-reader"},"saved":{"route":"usb-reader"},"selected":true,"requires_confirm":false,"apply_state":"applied"},{"id":"switch/tf_wp","kind":"switch","current":{"route":"protected"},"saved":{"route":"protected"},"selected":true,"requires_confirm":false,"apply_state":"applied"},{"id":"switch/usb","kind":"switch","current":{"route":"target"},"saved":null,"selected":false,"requires_confirm":true,"apply_state":"not_saved"}]}'
safe_config_cleared='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":false,"version":null},"pending":0,"items":[{"id":"switch/sd","kind":"switch","current":{"route":"usb-reader"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"},{"id":"switch/tf_wp","kind":"switch","current":{"route":"protected"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"},{"id":"switch/usb","kind":"switch","current":{"route":"target"},"saved":null,"selected":false,"requires_confirm":true,"apply_state":"not_saved"}]}'
safe_config_final='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":false,"version":null},"pending":0,"items":[{"id":"switch/sd","kind":"switch","current":{"route":"target"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"},{"id":"switch/tf_wp","kind":"switch","current":{"route":"writable"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"},{"id":"switch/usb","kind":"switch","current":{"route":"target"},"saved":null,"selected":false,"requires_confirm":true,"apply_state":"not_saved"}]}'
safe_save_both_ok='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save","saved_items":["switch/sd","switch/tf_wp"],"confirmation_items":[],"applied_items":["switch/sd","switch/tf_wp"],"snapshot":{"present":true,"version":1},"pending":0}'
switch_sd_usb_reader_ok='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"switch","action":"route","name":"sd","route":"usb-reader"}'
switch_tf_wp_protected_ok='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"switch","action":"route","name":"tf_wp","route":"protected"}'
switch_sd_target_ok='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"switch","action":"route","name":"sd","route":"target"}'
switch_tf_wp_writable_ok='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"switch","action":"route","name":"tf_wp","route":"writable"}'
busy_clear_capture='{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"clear","error":{"code":"busy","message":"capture active"},"activity":"capture"}'
busy_missing_message='{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"busy"},"activity":"capture"}'
busy_missing_activity='{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"busy","message":"capture active"}}'
busy_ota_activity='{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"busy","message":"OTA active"},"activity":"ota"}'
confirmation_required_missing_items='{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"confirmation_required","message":"confirm USB route"}}'
confirmation_required_wrong_items='{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"confirmation_required","message":"confirm USB route"},"dangerous_items":["switch/sd"]}'
confirmation_required_save_ok='{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"confirmation_required","message":"confirm USB route"},"dangerous_items":["switch/usb"]}'
save_apply_failed='{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"config","action":"save","error":{"code":"apply_failed","message":"hardware replay failed"},"applied_items":["switch/usb"],"failed_item":"switch/tf_wp","pending_items":["switch/tf_wp"]}'
dangerous_save_ok='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"save","saved_items":["switch/usb"],"confirmation_items":["switch/usb"],"applied_items":["switch/usb"],"snapshot":{"present":true,"version":1},"pending":0}'
dangerous_auto_restore_first_config='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"switch/sd","kind":"switch","current":{"route":"target"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"},{"id":"switch/tf_wp","kind":"switch","current":{"route":"writable"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"},{"id":"switch/usb","kind":"switch","current":{"route":"pc"},"saved":{"route":"pc"},"selected":true,"requires_confirm":true,"apply_state":"applied"}]}'
dangerous_auto_restore_second_config='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"switch/sd","kind":"switch","current":{"route":"target"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"},{"id":"switch/tf_wp","kind":"switch","current":{"route":"writable"},"saved":null,"selected":false,"requires_confirm":false,"apply_state":"not_saved"},{"id":"switch/usb","kind":"switch","current":{"route":"pc"},"saved":{"route":"pc"},"selected":true,"requires_confirm":true,"apply_state":"applied"}]}'

red_safe_reboot_dry_run() {
  reset_fixture
  run_fixture "$WORK/red-safe-reboot-dry-run.out" --dry-run safe-reboot
  assert_exact_requests "$WORK/red-safe-reboot-dry-run.out" \
    'GET /config' 'PUT /switch/sd' 'PUT /switch/tf_wp' 'GET /config' 'PUT /config' \
    'external reboot command' 'sleep 5' 'GET /config' 'external reboot command' 'sleep 5' \
    'GET /config' 'DELETE /config' 'GET /config' \
    'PUT /switch/sd' 'PUT /switch/tf_wp' 'GET /config'
}

red_capture_busy_dry_run() {
  reset_fixture
  run_fixture "$WORK/red-capture-busy-dry-run.out" --dry-run capture-busy
  assert_exact_requests "$WORK/red-capture-busy-dry-run.out" \
    'GET /config' 'PUT /config' 'GET /config' 'external capture start command' 'PUT /config' \
    'DELETE /config' 'GET /config' 'external capture stop command' 'DELETE /config' 'GET /config'
}

red_safe_reboot_execute() {
  reset_fixture
  write_response 1 200 "$multiple_safe_config"
  write_response 2 200 "$switch_sd_usb_reader_ok"
  write_response 3 200 "$switch_tf_wp_protected_ok"
  write_response 4 200 "$safe_config_prepared"
  write_response 5 200 "$safe_save_both_ok"
  write_response 6 200 "$safe_config_saved"
  write_response 7 200 "$safe_config_saved"
  write_response 8 200 "$clear_ok"
  write_response 9 200 "$safe_config_cleared"
  write_response 10 200 "$switch_sd_target_ok"
  write_response 11 200 "$switch_tf_wp_writable_ok"
  write_response 12 200 "$safe_config_final"
  if ! run_fixture "$WORK/red-safe-reboot-execute.out" --execute --url http://fixture.invalid \
    --reboot-command "$STUBS/reboot" safe-reboot; then
    cat "$WORK/red-safe-reboot-execute.out" >&2
    fail "safe-reboot did not complete the exact two-item Todo 16 fixture"
  fi
  assert_exact_calls "$CALLS" \
    'curl GET http://fixture.invalid/api/v1/config' \
    'curl PUT http://fixture.invalid/api/v1/switch/sd {"route":"usb-reader"}' \
    'curl PUT http://fixture.invalid/api/v1/switch/tf_wp {"route":"protected"}' \
    'curl GET http://fixture.invalid/api/v1/config' \
    'curl PUT http://fixture.invalid/api/v1/config {"items":["switch/sd","switch/tf_wp"],"confirm":false}' \
    'reboot' \
    'sleep 5' \
    'curl GET http://fixture.invalid/api/v1/config' \
    'reboot' \
    'sleep 5' \
    'curl GET http://fixture.invalid/api/v1/config' \
    'curl DELETE http://fixture.invalid/api/v1/config' \
    'curl GET http://fixture.invalid/api/v1/config' \
    'curl PUT http://fixture.invalid/api/v1/switch/sd {"route":"target"}' \
    'curl PUT http://fixture.invalid/api/v1/switch/tf_wp {"route":"writable"}' \
    'curl GET http://fixture.invalid/api/v1/config'
  [ "$(cat "$WORK/state/reboot-readiness-responses")" = 2 ] || \
    fail "safe-reboot did not probe readiness after both reboots"
}

red_capture_busy_execute() {
  reset_fixture
  write_response 1 200 "$multiple_safe_config"
  write_response 2 200 "$save_ok"
  write_response 3 200 "$saved_first_safe_config"
  write_response 4 409 "$busy_config"
  write_response 5 409 "$busy_clear_capture"
  write_response 6 200 "$saved_first_safe_config"
  write_response 7 200 "$clear_ok"
  write_response 8 200 "$multiple_safe_config"
  if ! run_fixture "$WORK/red-capture-busy-execute.out" --execute --url http://fixture.invalid \
    --capture-start "$STUBS/capture-start" --capture-stop "$STUBS/capture-stop" capture-busy; then
    cat "$WORK/red-capture-busy-execute.out" >&2
    fail "capture-busy did not prepare a snapshot and prove save plus clear busy"
  fi
  assert_exact_calls "$CALLS" \
    'curl GET http://fixture.invalid/api/v1/config' \
    'curl PUT http://fixture.invalid/api/v1/config {"items":["switch/sd"],"confirm":false}' \
    'curl GET http://fixture.invalid/api/v1/config' \
    'capture-start' \
    'curl PUT http://fixture.invalid/api/v1/config {"items":["switch/sd"],"confirm":false}' \
    'curl DELETE http://fixture.invalid/api/v1/config' \
    'curl GET http://fixture.invalid/api/v1/config' \
    'capture-stop' \
    'curl DELETE http://fixture.invalid/api/v1/config' \
    'curl GET http://fixture.invalid/api/v1/config'
}

red_final_cleanup_plan() {
  reset_fixture
  run_fixture "$WORK/red-final-cleanup-plan.out" --dry-run --serial "$SERIAL" \
    --combined-uf2 "$CANONICAL_UF2" --ota-image "$CANONICAL_OTA" all
  assert_request_suffix "$WORK/red-final-cleanup-plan.out" \
    'DELETE /config' 'PUT /switch/usb' 'PUT /switch/sd' 'PUT /switch/tf_wp' \
    'GET /switch/vin' 'GET /power' 'PUT /power/<firmware-name>' 'GET /power' \
    'GET /gpio' 'PUT /gpio/<firmware-name>' 'GET /gpio' 'GET /config' 'CDC config show'
}

red_get_missing_field() {
  field="$1"
  reset_fixture
  write_response 1 200 "$(omit_response_field "$field" "$multiple_safe_config")"
  assert_rejected_envelope "get-missing-$field" 'GET /config' --execute --url http://fixture.invalid \
    --reboot-command "$STUBS/reboot" safe-reboot
}

red_save_missing_field() {
  field="$1"
  reset_fixture
  write_response 1 200 "$multiple_safe_config"
  write_response 2 200 "$switch_sd_usb_reader_ok"
  write_response 3 200 "$switch_tf_wp_protected_ok"
  write_response 4 200 "$safe_config_prepared"
  write_response 5 200 "$(omit_response_field "$field" "$safe_save_both_ok")"
  write_response 6 200 "$switch_sd_target_ok"
  write_response 7 200 "$switch_tf_wp_writable_ok"
  write_response 8 200 "$clear_ok"
  assert_rejected_envelope "save-missing-$field" 'PUT /config' --execute --url http://fixture.invalid \
    --reboot-command "$STUBS/reboot" safe-reboot
}

red_save_apply_failed_missing_field() {
  field="$1"
  reset_fixture
  write_response 1 200 "$multiple_safe_config"
  write_response 2 200 "$switch_sd_usb_reader_ok"
  write_response 3 200 "$switch_tf_wp_protected_ok"
  write_response 4 200 "$safe_config_prepared"
  write_response 5 200 "$(omit_response_field "$field" "$save_apply_failed")"
  write_response 6 200 "$switch_sd_target_ok"
  write_response 7 200 "$switch_tf_wp_writable_ok"
  write_response 8 200 "$clear_ok"
  assert_rejected_envelope "save-apply-failed-missing-$field" 'PUT /config' --execute --url http://fixture.invalid \
    --reboot-command "$STUBS/reboot" safe-reboot
}

red_clear_missing_field() {
  field="$1"
  reset_fixture
  write_response 1 200 "$multiple_safe_config"
  write_response 2 200 "$switch_sd_usb_reader_ok"
  write_response 3 200 "$switch_tf_wp_protected_ok"
  write_response 4 200 "$safe_config_prepared"
  write_response 5 200 "$safe_save_both_ok"
  write_response 6 200 "$safe_config_saved"
  write_response 7 200 "$safe_config_saved"
  write_response 8 200 "$(omit_response_field "$field" "$clear_ok")"
  write_response 9 200 "$switch_sd_target_ok"
  write_response 10 200 "$switch_tf_wp_writable_ok"
  write_response 11 200 "$clear_ok"
  assert_rejected_envelope "clear-missing-$field" 'DELETE /config' --execute --url http://fixture.invalid \
    --reboot-command "$STUBS/reboot" safe-reboot
}

red_confirmation_required_case() {
  label="$1"
  response="$2"
  reset_fixture
  write_response 1 200 "$valid_config"
  write_response 2 200 "$switch_usb_pc_ok"
  write_response 3 409 "$response"
  write_response 4 200 "$clear_ok"
  assert_rejected_envelope "$label" 'PUT /config' --execute --url http://fixture.invalid \
    --reboot-command "$STUBS/reboot" --confirm-dangerous-save dangerous-auto-restore
}

red_busy_case() {
  label="$1"
  expected_status="$2"
  response="$3"
  reset_fixture
  write_response 1 200 "$multiple_safe_config"
  write_response 2 200 "$save_ok"
  write_response 3 200 "$saved_first_safe_config"
  write_response 4 "$expected_status" "$response"
  write_response 5 200 "$clear_ok"
  assert_rejected_envelope "$label" 'PUT /config' --execute --url http://fixture.invalid \
    --capture-start "$STUBS/capture-start" --capture-stop "$STUBS/capture-stop" capture-busy
}

red_cdc_late_primary_error() {
  reset_fixture
  write_response 1 200 "$valid_config"
  if CONFIG_HIL_PYTHON_SERIAL_LATE_ERROR=1 run_fixture_default_serial "$WORK/red-cdc-late-primary-error.out" \
    --execute --url http://fixture.invalid --serial "$SERIAL" --combined-uf2 "$CANONICAL_UF2" cdc-fallback; then
    fail "CDC accepted a success/detail line before a primary error line"
  fi
  ! grep -F 'python-serial config save ' "$CALLS" >/dev/null || fail "CDC primary error advanced to config save"
  ! grep -E '^(mount|flash) ' "$CALLS" >/dev/null || fail "CDC primary error advanced to BOOTSEL recovery"
}

red_dangerous_auto_restore_execute() {
  catalog='{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"absent"},"snapshot":{"present":false,"version":null},"pending":0,"items":[{"id":"switch/fixture_target","kind":"switch","current":{"route":"target"},"saved":null,"selected":false,"requires_confirm":true,"apply_state":"not_saved"},{"id":"switch/usb","kind":"switch","current":{"route":"target"},"saved":null,"selected":false,"requires_confirm":true,"apply_state":"not_saved"}]}'
  reset_fixture
  write_response 1 200 "$catalog"
  write_response 2 200 "$switch_usb_pc_ok"
  write_response 3 409 "$confirmation_required_save_ok"
  write_response 4 200 "$dangerous_save_ok"
  write_response 5 200 "$dangerous_auto_restore_first_config"
  write_response 6 200 "$dangerous_auto_restore_second_config"
  write_response 7 200 "$clear_ok"
  if ! run_fixture "$WORK/red-dangerous-auto-restore.out" --execute --url http://fixture.invalid \
    --reboot-command "$STUBS/reboot" --confirm-dangerous-save dangerous-auto-restore; then
    cat "$WORK/red-dangerous-auto-restore.out" >&2
    fail "dangerous auto-restore did not pin confirmation and both boots to switch/usb"
  fi
  assert_exact_calls "$CALLS" \
    'curl GET http://fixture.invalid/api/v1/config' \
    'curl PUT http://fixture.invalid/api/v1/switch/usb {"route":"pc"}' \
    'curl PUT http://fixture.invalid/api/v1/config {"items":["switch/usb"],"confirm":false}' \
    'curl PUT http://fixture.invalid/api/v1/config {"items":["switch/usb"],"confirm":true}' \
    'reboot' \
    'sleep 5' \
    'curl GET http://fixture.invalid/api/v1/config' \
    'reboot' \
    'sleep 5' \
    'curl GET http://fixture.invalid/api/v1/config' \
    'curl DELETE http://fixture.invalid/api/v1/config'
  [ "$(cat "$WORK/state/reboot-readiness-responses")" = 2 ] || \
    fail "dangerous auto-restore did not probe readiness after both reboots"
}

red_dangerous_auto_restore_dry_run() {
  reset_fixture
  run_fixture "$WORK/red-dangerous-auto-restore-dry-run.out" --dry-run dangerous-auto-restore
  assert_exact_requests "$WORK/red-dangerous-auto-restore-dry-run.out" \
    'GET /config' 'PUT /switch/usb' 'PUT /config' 'PUT /config' 'external reboot command' 'sleep 5' 'GET /config' \
    'external reboot command' 'sleep 5' 'GET /config' 'DELETE /config'
}

red_dangerous_auto_restore_rejects_v2_save() {
  reset_fixture
  write_response 1 200 "$valid_config"
  write_response 2 200 "$switch_usb_pc_ok"
  write_response 3 409 "$confirmation_required_save_ok"
  write_response 4 200 "$(snapshot_with_version 2 "$dangerous_save_ok")"
  write_response 5 200 "$clear_ok"
  if run_fixture "$WORK/red-dangerous-auto-restore-v2-save.out" --execute --url http://fixture.invalid \
    --reboot-command "$STUBS/reboot" --confirm-dangerous-save dangerous-auto-restore; then
    fail "dangerous auto-restore accepted a v2 confirmed save"
  fi
  assert_contains "$WORK/red-dangerous-auto-restore-v2-save.out" 'saved snapshot version is not v1'
  ! grep -F reboot "$CALLS" >/dev/null || fail "v2 dangerous save advanced to reboot"
}

red_dangerous_auto_restore_rejects_v2_reboot() {
  reset_fixture
  write_response 1 200 "$valid_config"
  write_response 2 200 "$switch_usb_pc_ok"
  write_response 3 409 "$confirmation_required_save_ok"
  write_response 4 200 "$dangerous_save_ok"
  write_response 5 200 "$(snapshot_with_version 2 "$dangerous_auto_restore_first_config")"
  write_response 6 200 "$clear_ok"
  if run_fixture "$WORK/red-dangerous-auto-restore-v2-reboot.out" --execute --url http://fixture.invalid \
    --reboot-command "$STUBS/reboot" --confirm-dangerous-save dangerous-auto-restore; then
    fail "dangerous auto-restore accepted a v2 reboot snapshot"
  fi
  assert_contains "$WORK/red-dangerous-auto-restore-v2-reboot.out" 'dangerous snapshot is missing or not v1'
  [ "$(grep -F -c reboot "$CALLS")" -eq 1 ] || \
    fail "v2 dangerous reboot advanced beyond the first reboot"
}

red_ota_preserve_dry_run_round_2() {
  reset_fixture
  run_fixture "$WORK/red-ota-preserve-dry-run-2.out" --dry-run --ota-image "$CANONICAL_OTA" ota-preserve
  assert_exact_requests "$WORK/red-ota-preserve-dry-run-2.out" \
    'GET /config' 'PUT /config' 'GET /config' 'background POST /ota/upload' \
    'bounded GET /ota' 'PUT /config' 'DELETE /config' 'GET /config' \
    'await POST /ota/upload' 'POST /ota/test' 'sleep 5' \
    'GET /config' 'POST /ota/confirm' 'DELETE /config'
}

assert_ota_stub_stopped() {
  pid_file="$WORK/state/ota-upload.pid"
  [ -f "$pid_file" ] || return 0
  pid=$(cat "$pid_file")
  if kill -0 "$pid" 2>/dev/null; then
    fail "OTA upload stub process remained after fixture completion: $pid"
  fi
}

red_ota_preserve_concurrent_execute() {
  reset_fixture
  if CONFIG_HIL_OTA_CONCURRENCY=1 run_fixture "$WORK/red-ota-preserve-concurrent.out" \
    --execute --url http://fixture.invalid --ota-image "$CANONICAL_OTA" ota-preserve; then
    result=0
  else
    result=$?
  fi
  assert_ota_stub_stopped
  [ "$result" -eq 0 ] || {
    cat "$WORK/red-ota-preserve-concurrent.out" >&2
    fail "ota-preserve did not exercise config while upload owned flash"
  }
  [ -f "$WORK/state/ota-upload.complete" ] || fail "OTA upload completed before all interleaved calls"
  assert_order "$CALLS" \
    'curl GET http://fixture.invalid/api/v1/config' \
    'curl PUT http://fixture.invalid/api/v1/config {"items":["switch/sd"],"confirm":false}' \
    'curl GET http://fixture.invalid/api/v1/config' \
    'curl POST http://fixture.invalid/api/v1/ota/upload' \
    'curl GET http://fixture.invalid/api/v1/ota' \
    'curl PUT http://fixture.invalid/api/v1/config {"items":["switch/sd"],"confirm":false}' \
    'curl DELETE http://fixture.invalid/api/v1/config' \
    'curl GET http://fixture.invalid/api/v1/config' \
    'curl POST http://fixture.invalid/api/v1/ota/test' \
    'curl GET http://fixture.invalid/api/v1/config' \
    'curl POST http://fixture.invalid/api/v1/ota/confirm' \
    'curl DELETE http://fixture.invalid/api/v1/config'
}

reboot_transport_recovers_after_failed_probes() {
  reset_fixture
  if CONFIG_HIL_OTA_CONCURRENCY=1 CONFIG_HIL_REBOOT_TRANSPORT_FAILS=3 \
    run_fixture "$WORK/reboot-transport-recovery.out" --execute --url http://fixture.invalid \
      --ota-image "$CANONICAL_OTA" ota-preserve; then
    result=0
  else
    result=$?
  fi
  assert_ota_stub_stopped
  [ "$result" -eq 0 ] || fail "ota-preserve did not recover after bounded transport failures"
  [ "$(cat "$WORK/state/reboot-transport-attempts")" = 3 ] || \
    fail "reboot transport fixture did not consume three failed probes"
}

reboot_readiness_rejects_reached_invalid_response() {
  reset_fixture
  if CONFIG_HIL_OTA_CONCURRENCY=1 CONFIG_HIL_REBOOT_READINESS_INVALID=1 \
    run_fixture "$WORK/reboot-readiness-invalid.out" --execute --url http://fixture.invalid \
      --ota-image "$CANONICAL_OTA" ota-preserve; then
    fail "reboot readiness accepted an invalid reached response"
  fi
  assert_ota_stub_stopped
  assert_contains "$WORK/reboot-readiness-invalid.out" \
    'malformed response for post-reboot GET /config readiness'
  [ "$(cat "$WORK/state/reboot-readiness-responses")" = 1 ] || \
    fail "reboot readiness retried an invalid reached response"
}

red_ota_upload_bounded_rate() {
  reset_fixture
  if CONFIG_HIL_OTA_CONCURRENCY=1 CONFIG_HIL_OTA_REQUIRE_LIMIT_RATE=1 \
    run_fixture "$WORK/red-ota-upload-bounded-rate.out" --execute --url http://fixture.invalid \
      --ota-image "$CANONICAL_OTA" ota-preserve; then
    result=0
  else
    result=$?
  fi
  assert_ota_stub_stopped
  [ "$result" -eq 0 ] || {
    assert_contains "$WORK/red-ota-upload-bounded-rate.out" \
      'fixture OTA upload requires --limit-rate 64K'
    fail "background OTA upload did not use --limit-rate 64K"
  }
}

red_ota_malformed_active_state() {
  reset_fixture
  if CONFIG_HIL_OTA_CONCURRENCY=1 CONFIG_HIL_OTA_BAD_STATE=1 \
    run_fixture "$WORK/red-ota-malformed-state.out" --execute --url http://fixture.invalid \
      --ota-image "$CANONICAL_OTA" ota-preserve; then
    fail "ota-preserve accepted a non-uploading state during active upload"
  fi
  assert_ota_stub_stopped
  grep -F 'curl GET http://fixture.invalid/api/v1/ota ' "$CALLS" >/dev/null || \
    fail "ota-preserve did not inspect OTA state"
  [ "$(grep -F -c 'curl PUT http://fixture.invalid/api/v1/config ' "$CALLS")" -eq 1 ] || \
    fail "malformed OTA state advanced to the busy config save"
}

red_ota_wrong_busy_activity() {
  reset_fixture
  if CONFIG_HIL_OTA_CONCURRENCY=1 CONFIG_HIL_OTA_WRONG_ACTIVITY=1 \
    run_fixture "$WORK/red-ota-wrong-activity.out" --execute --url http://fixture.invalid \
      --ota-image "$CANONICAL_OTA" ota-preserve; then
    fail "ota-preserve accepted activity=capture while OTA owned flash"
  fi
  assert_ota_stub_stopped
  grep -F 'curl GET http://fixture.invalid/api/v1/ota ' "$CALLS" >/dev/null || \
    fail "wrong-activity fixture did not observe uploading state"
  [ "$(grep -F -c 'curl PUT http://fixture.invalid/api/v1/config ' "$CALLS")" -eq 2 ] || \
    fail "wrong-activity fixture did not reach the active-upload config save"
}

write_all_responses_before_final_cleanup() {
  write_response 1 200 "$multiple_safe_config"
  write_response 2 200 "$switch_sd_usb_reader_ok"
  write_response 3 200 "$switch_tf_wp_protected_ok"
  write_response 4 200 "$safe_config_prepared"
  write_response 5 200 "$safe_save_both_ok"
  write_response 6 200 "$safe_config_saved"
  write_response 7 200 "$safe_config_saved"
  write_response 8 200 "$clear_ok"
  write_response 9 200 "$safe_config_cleared"
  write_response 10 200 "$switch_sd_target_ok"
  write_response 11 200 "$switch_tf_wp_writable_ok"
  write_response 12 200 "$safe_config_final"
  write_response 13 200 "$valid_config"
  write_response 14 200 "$switch_usb_pc_ok"
  write_response 15 409 "$confirmation_required_save_ok"
  write_response 16 200 "$dangerous_save_ok"
  write_response 17 200 "$dangerous_auto_restore_first_config"
  write_response 18 200 "$dangerous_auto_restore_second_config"
  write_response 19 200 "$clear_ok"
  write_response 20 200 "$multiple_safe_config"
  write_response 21 200 "$save_ok"
  write_response 22 200 "$saved_first_safe_config"
  write_response 23 409 "$busy_config"
  write_response 24 409 "$busy_clear_capture"
  write_response 25 200 "$saved_first_safe_config"
  write_response 26 200 "$clear_ok"
  write_response 27 200 "$multiple_safe_config"
  write_response 28 200 "$multiple_safe_config"
  write_response 29 200 "$save_ok"
  write_response 30 200 "$saved_first_safe_config"
  write_response 40 200 "$multiple_safe_config"
  write_response 41 200 "$save_ok"
  write_response 42 200 '{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"bootloader"}'
  write_response 43 200 "$saved_first_safe_config"
  write_response 44 200 "$clear_ok"
  write_response 45 200 "$valid_config"
}

red_final_cleanup_enumerated_execute() {
  reset_fixture
  write_all_responses_before_final_cleanup
  if ! CONFIG_HIL_CLEANUP_ENUMERATION=1 CONFIG_HIL_OTA_CONCURRENCY=1 \
    CONFIG_HIL_OTA_ACTIVATE_AFTER=30 run_fixture "$WORK/red-final-cleanup-enumerated.out" \
    --execute --url http://fixture.invalid --serial "$SERIAL" --reboot-command "$STUBS/reboot" \
    --capture-start "$STUBS/capture-start" --capture-stop "$STUBS/capture-stop" \
    --confirm-dangerous-save --combined-uf2 "$CANONICAL_UF2" \
    --ota-image "$CANONICAL_OTA" all; then
    cat "$WORK/red-final-cleanup-enumerated.out" >&2
    fail "all flow did not reach final cleanup"
  fi
  assert_order "$CALLS" \
    'serial ' \
    'bootloader' \
    'curl PUT http://fixture.invalid/api/v1/switch/usb {"route":"target"}' \
    'curl PUT http://fixture.invalid/api/v1/switch/sd {"route":"target"}' \
    'curl PUT http://fixture.invalid/api/v1/switch/tf_wp {"route":"writable"}' \
    'curl GET http://fixture.invalid/api/v1/switch/vin' \
    'curl GET http://fixture.invalid/api/v1/power' \
    'curl PUT http://fixture.invalid/api/v1/power/fixture_rail {"state":"off"}' \
    'curl GET http://fixture.invalid/api/v1/power' \
    'curl GET http://fixture.invalid/api/v1/gpio' \
    'curl PUT http://fixture.invalid/api/v1/gpio/FIXTURE_GPIO {"direction":"input"}' \
    'curl GET http://fixture.invalid/api/v1/gpio' \
    'curl GET http://fixture.invalid/api/v1/config' \
    'serial '
}

red_final_cleanup_enumerated_plan() {
  reset_fixture
  run_fixture "$WORK/red-final-cleanup-enumerated-plan.out" --dry-run --serial "$SERIAL" \
    --combined-uf2 "$CANONICAL_UF2" --ota-image "$CANONICAL_OTA" all
  assert_request_suffix "$WORK/red-final-cleanup-enumerated-plan.out" \
    'DELETE /config' 'PUT /switch/usb' 'PUT /switch/sd' 'PUT /switch/tf_wp' \
    'GET /switch/vin' 'GET /power' 'PUT /power/<firmware-name>' 'GET /power' \
    'GET /gpio' 'PUT /gpio/<firmware-name>' 'GET /gpio' 'GET /config' 'CDC config show'
}

existing_uncertain_safe_save() {
  reset_fixture
  write_response 1 200 "$multiple_safe_config"
  write_response 2 200 "$switch_sd_usb_reader_ok"
  write_response 3 200 "$switch_tf_wp_protected_ok"
  write_response 4 200 "$safe_config_prepared"
  write_response 5 200 '{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"clear"}'
  write_response 6 200 "$switch_sd_target_ok"
  write_response 7 200 "$switch_tf_wp_writable_ok"
  write_response 8 200 "$clear_ok"
  if run_fixture "$WORK/uncertain-save.out" --execute --url http://fixture.invalid \
    --reboot-command "$STUBS/reboot" safe-reboot; then
    fail "wrong save action unexpectedly succeeded"
  fi
  assert_order "$CALLS" \
    'curl GET http://fixture.invalid/api/v1/config' \
    'curl PUT http://fixture.invalid/api/v1/switch/sd {"route":"usb-reader"}' \
    'curl PUT http://fixture.invalid/api/v1/switch/tf_wp {"route":"protected"}' \
    'curl GET http://fixture.invalid/api/v1/config' \
    'curl PUT http://fixture.invalid/api/v1/config {"items":["switch/sd","switch/tf_wp"],"confirm":false}' \
    'curl PUT http://fixture.invalid/api/v1/switch/sd {"route":"target"}' \
    'curl PUT http://fixture.invalid/api/v1/switch/tf_wp {"route":"writable"}' \
    'curl DELETE http://fixture.invalid/api/v1/config'
  ! grep -F reboot "$CALLS" >/dev/null || fail "uncertain save advanced to reboot before cleanup"
}

red_bootsel_selects_only_new_partition() {
  reset_fixture
  write_response 1 200 "$valid_config"
  write_response 2 200 "$save_ok"
  write_response 3 200 '{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"bootloader"}'
  write_response 4 200 "$saved_first_safe_config"
  write_response 5 200 "$clear_ok"
  if ! CONFIG_HIL_LSBLK_LAYOUT=existing-and-new run_fixture "$WORK/red-bootsel-new-partition.out" \
    --execute --url http://fixture.invalid --combined-uf2 "$CANONICAL_UF2" bootsel-preserve; then
    cat "$WORK/red-bootsel-new-partition.out" >&2
    fail "BOOTSEL flow did not select the sole new RPI partition"
  fi
  assert_order "$PARTITION_LOG" \
    'layout=existing-and-new entered=0' \
    'layout=existing-and-new entered=1'
  assert_order "$CALLS" \
    'mount mount -b /dev/sdb1' \
    "flash $CANONICAL_UF2 $MOUNT_POINT/"
  ! grep -F 'mount mount -b /dev/sda1' "$CALLS" >/dev/null || \
    fail "BOOTSEL flow mounted a pre-existing RPI partition"
}

red_bootsel_rejects_multiple_new_partitions() {
  reset_fixture
  write_response 1 200 "$valid_config"
  write_response 2 200 "$save_ok"
  write_response 3 200 '{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"bootloader"}'
  write_response 4 200 "$clear_ok"
  if CONFIG_HIL_LSBLK_LAYOUT=multiple-new run_fixture "$WORK/red-bootsel-multiple-new.out" \
    --execute --url http://fixture.invalid --combined-uf2 "$CANONICAL_UF2" bootsel-preserve; then
    fail "BOOTSEL flow accepted multiple new RPI partitions"
  fi
  assert_order "$PARTITION_LOG" \
    'layout=multiple-new entered=0' \
    'layout=multiple-new entered=1'
  ! grep -E '^(mount|flash) ' "$CALLS" >/dev/null || \
    fail "multiple new RPI partitions advanced to mount or copy"
}

red_copy_failure_unmounts_only_owned_partition() {
  reset_fixture
  write_response 1 200 "$valid_config"
  write_response 2 200 "$save_ok"
  write_response 3 200 '{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"bootloader"}'
  write_response 4 200 "$clear_ok"
  if CONFIG_HIL_LSBLK_LAYOUT=existing-and-new CONFIG_HIL_FLASH_FAIL=1 \
    run_fixture "$WORK/red-bootsel-copy-failure.out" --execute --url http://fixture.invalid \
    --combined-uf2 "$CANONICAL_UF2" bootsel-preserve; then
    fail "BOOTSEL copy failure unexpectedly succeeded"
  fi
  assert_order "$CALLS" \
    'mount mount -b /dev/sdb1' \
    "flash $CANONICAL_UF2 $MOUNT_POINT/" \
    'unmount unmount -b /dev/sdb1'
  ! grep -F 'unmount unmount -b /dev/sda1' "$CALLS" >/dev/null || \
    fail "cleanup unmounted a pre-existing RPI partition"
}

red_cleanup_continues_after_failure() {
  switch_failure='{"schema":"radxa-linkr-debugger.v1","ok":false,"command":"switch","action":"route","error":{"code":"fixture_failure","message":"fixture cleanup failure"}}'
  reset_fixture
  write_response 1 200 "$multiple_safe_config"
  write_response 2 200 "$switch_sd_usb_reader_ok"
  write_response 3 200 "$switch_tf_wp_protected_ok"
  write_response 4 200 "$safe_config_prepared"
  write_response 5 200 '{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"clear"}'
  write_response 6 500 "$switch_failure"
  write_response 7 200 "$switch_tf_wp_writable_ok"
  write_response 8 200 "$clear_ok"
  if run_fixture "$WORK/red-cleanup-continuation.out" --execute --url http://fixture.invalid \
    --reboot-command "$STUBS/reboot" safe-reboot; then
    fail "malformed save unexpectedly completed"
  fi
  assert_order "$CALLS" \
    'curl PUT http://fixture.invalid/api/v1/switch/sd {"route":"target"}' \
    'curl PUT http://fixture.invalid/api/v1/switch/tf_wp {"route":"writable"}' \
    'curl DELETE http://fixture.invalid/api/v1/config '
  assert_timeout PUT /switch/sd '{"route":"target"}' 5
  assert_timeout PUT /switch/tf_wp '{"route":"writable"}' 5
  assert_timeout DELETE /config '' 5
}

red_ota_upload_has_hard_deadlines() {
  red_ota_preserve_concurrent_execute
  assert_timeout POST /ota/upload "@$CANONICAL_OTA" 120
  assert_timeout GET /ota '' 2
}

red_dangerous_auto_restore_v1_contract() {
  python3 - "$dangerous_save_ok" "$dangerous_auto_restore_first_config" "$dangerous_auto_restore_second_config" <<'PY'
import json
import sys

save_response, first_reboot_response, second_reboot_response = map(json.loads, sys.argv[1:])
if save_response.get("snapshot", {}).get("version") != 1:
    raise SystemExit("dangerous save fixture must report snapshot.version=1")
if save_response.get("pending") != 0:
    raise SystemExit("dangerous save fixture must report pending=0 before reboot")
if save_response.get("saved_items") != ["switch/usb"] or save_response.get("confirmation_items") != ["switch/usb"]:
    raise SystemExit("dangerous save fixture must record confirmed switch/usb")
for label, response in (("first", first_reboot_response), ("second", second_reboot_response)):
    if response.get("snapshot", {}).get("version") != 1:
        raise SystemExit(f"{label} reboot fixture must report snapshot.version=1")
    if response.get("pending") != 0:
        raise SystemExit(f"{label} reboot fixture must report pending=0")
    items = response.get("items")
    matching = [item for item in items if item.get("id") == "switch/usb"] if isinstance(items, list) else []
    if len(matching) != 1:
        raise SystemExit(f"{label} reboot fixture must contain switch/usb")
    item = matching[0]
    if (
        item.get("selected") is not True
        or item.get("requires_confirm") is not True
        or item.get("apply_state") != "applied"
        or item.get("current") != {"route": "pc"}
        or item.get("saved") != {"route": "pc"}
    ):
        raise SystemExit(f"{label} reboot fixture must fully restore confirmed switch/usb")
PY
}

reset_fixture
write_response 1 200 "$valid_config"
if CONFIG_HIL_PYTHON_SERIAL_PROMPT_ONLY=1 run_fixture_default_serial "$WORK/python-prompt-only.out" \
  --execute --url http://fixture.invalid --serial "$SERIAL" --combined-uf2 "$CANONICAL_UF2" cdc-fallback; then
  fail "default CDC reader accepted a prompt/echo without a primary response"
fi
! grep -F 'python-serial config save ' "$CALLS" >/dev/null || fail "prompt/echo advanced default CDC reader to save"
! grep -E '^(mount|flash) ' "$CALLS" >/dev/null || fail "prompt/echo advanced default CDC reader to BOOTSEL recovery"

existing_uncertain_safe_save
red_safe_reboot_execute
red_capture_busy_execute

red_case safe-reboot-dry-run red_safe_reboot_dry_run
red_case capture-busy-dry-run red_capture_busy_dry_run
red_case safe-reboot-execute red_safe_reboot_execute
red_case capture-busy-execute red_capture_busy_execute
red_case final-cleanup-plan red_final_cleanup_plan

for field in backend snapshot pending items; do
  red_case "get-missing-$field" red_get_missing_field "$field"
done
for field in saved_items confirmation_items applied_items snapshot pending; do
  red_case "save-missing-$field" red_save_missing_field "$field"
done
for field in noop snapshot pending; do
  red_case "clear-missing-$field" red_clear_missing_field "$field"
done

red_case error-missing-message red_busy_case error-missing-message 409 "$busy_missing_message"
red_case confirmation-required-missing-dangerous-items red_confirmation_required_case \
  confirmation-required-missing-dangerous-items "$confirmation_required_missing_items"
red_case confirmation-required-wrong-dangerous-items red_confirmation_required_case \
  confirmation-required-wrong-dangerous-items "$confirmation_required_wrong_items"
red_case busy-missing-activity red_busy_case busy-missing-activity 409 "$busy_missing_activity"
red_case busy-wrong-activity-ota-for-capture red_busy_case busy-wrong-activity-ota-for-capture 409 "$busy_ota_activity"
for field in applied_items failed_item pending_items; do
  red_case "save-apply-failed-missing-$field" red_save_apply_failed_missing_field "$field"
done
red_case status-200-ok-false red_busy_case status-200-ok-false 200 "$busy_config"
red_case status-409-ok-true red_busy_case status-409-ok-true 409 "$save_ok"
red_case cdc-late-primary-error red_cdc_late_primary_error

printf '%s\n' 'config-persistence-hil first-round fixture coverage: PASS'
red_case dangerous-auto-restore-dry-run red_dangerous_auto_restore_dry_run
red_case dangerous-auto-restore-execute red_dangerous_auto_restore_execute
red_case dangerous-auto-restore-rejects-v2-save red_dangerous_auto_restore_rejects_v2_save
red_case dangerous-auto-restore-rejects-v2-reboot red_dangerous_auto_restore_rejects_v2_reboot
red_case final-cleanup-enumerated-plan red_final_cleanup_enumerated_plan
red_case final-cleanup-enumerated-execute red_final_cleanup_enumerated_execute
red_case ota-preserve-concurrent-dry-run red_ota_preserve_dry_run_round_2
red_case ota-preserve-concurrent-execute red_ota_preserve_concurrent_execute
reboot_transport_recovers_after_failed_probes
reboot_readiness_rejects_reached_invalid_response
red_case ota-preserve-malformed-active-state red_ota_malformed_active_state
red_case ota-preserve-wrong-busy-activity red_ota_wrong_busy_activity

printf '%s\n' 'config-persistence-hil existing fixture coverage: PASS'
red_case ota-upload-bounded-rate red_ota_upload_bounded_rate
red_case bootsel-selects-only-new-partition red_bootsel_selects_only_new_partition
red_case bootsel-rejects-multiple-new-partitions red_bootsel_rejects_multiple_new_partitions
red_case bootsel-copy-failure-unmounts-owned-partition red_copy_failure_unmounts_only_owned_partition
red_case cleanup-continues-after-failure red_cleanup_continues_after_failure
red_case ota-upload-hard-deadlines red_ota_upload_has_hard_deadlines
red_case dangerous-auto-restore-v1-contract red_dangerous_auto_restore_v1_contract
if [ "$RED_GAPS" -gt 0 ]; then
  printf '%s\n' "config-persistence-hil fixture tests: EXPECTED RED ($RED_GAPS runner contract gaps)" >&2
  exit 1
fi

printf '%s\n' 'config-persistence-hil fixture tests: PASS'
