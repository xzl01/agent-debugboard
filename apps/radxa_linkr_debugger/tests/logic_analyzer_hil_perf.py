#!/usr/bin/env python3
"""Logic-analyzer HIL performance matrix runner.

This runner measures the host-observed behavior of the current firmware.  It
does not infer the hardware samplerate from transport throughput: receive rates
reported here are only effective host receive rates for the selected transport.
"""

from __future__ import annotations

import argparse
import errno
import importlib
import json
import threading
import socket
import struct
import sys
import time
import urllib.error
import urllib.request
from collections.abc import Iterable, Mapping
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from typing import Any, cast


SIGROK_MAGIC = 0x72
SIGROK_PROTOCOL_VERSION = 1
SIGROK_HEADER_BYTES = 9
SIGROK_DATA_META_BYTES = 8
SIGROK_SAMPLE_INDEX_MODULO = 1 << 24
SIGROK_SAMPLE_INDEX_MASK = SIGROK_SAMPLE_INDEX_MODULO - 1
COMPRESSION_NONE = 0
COMPRESSION_BIT_PACK = 1
COMPRESSION_RLE = 2
COMPRESSION_BIT_PACK_RLE = 3

FRAME_HELLO_REQ = 0x01
FRAME_HELLO_RESP = 0x02
FRAME_CAPS_REQ = 0x03
FRAME_CAPS_RESP = 0x04
FRAME_CONFIG_REQ = 0x05
FRAME_CONFIG_RESP = 0x06
FRAME_START_REQ = 0x07
FRAME_START_RESP = 0x08
FRAME_STOP_REQ = 0x09
FRAME_STOP_RESP = 0x0A
FRAME_CONFIG_V2_REQ = 0x0B
FRAME_EVENT = 0x10
FRAME_DATA = 0x11
FRAME_ERROR = 0x7F
SERVER_FLAG_CONFIG_V2 = 1 << 0
SERVER_FLAG_GENERIC_PACKED_BURST = 1 << 1
SERVER_EXPECTED_HIGH_RATE_FLAGS = SERVER_FLAG_CONFIG_V2 | SERVER_FLAG_GENERIC_PACKED_BURST

TRIGGER_NONE = 0
TRIGGER_RISING = 1
TRIGGER_FALLING = 2
TRIGGER_EITHER = 3
MODE_FAST8 = 1
MODE_WIDE11 = 2
MASK_SINGLE = 0x0001
MASK_FAST8 = 0x00FF
MASK_FAST8_SPARSE_GP10_GP13_GP17 = 0x0089
MASK_WIDE11 = 0x07FF
HIGH_RATE_PACKED_BURST_RATES_KHZ = (100000, 125000)
HIGH_RATE_PACKED_BURST_POST_SAMPLES = (513, 65535, 65536, 100000)
HIGH_RATE_PACKED_BURST_TRIGGER_TYPES = (TRIGGER_NONE, TRIGGER_RISING, TRIGGER_EITHER)
HIGH_RATE_PACKED_BURST_CONTINUOUS_RATE_KHZ = 100000
HIGH_RATE_PACKED_BURST_CONTINUOUS_TARGET_SAMPLES = 100000
HIGH_RATE_PACKED_BURST_MANUAL_STOP_RATE_KHZ = 1000
HIGH_RATE_PACKED_BURST_MANUAL_STOP_DURATION_S = 0.25
HIGH_RATE_PACKED_BURST_HEALTH_RECOVERY_TIMEOUT_S = 5.0
WIDE11_MAPPING_RATE_KHZ = 100000
WIDE11_MAPPING_PRE_SAMPLES = 0
WIDE11_MAPPING_POST_SAMPLES = 100000
WIDE11_MAPPING_TRIGGER_CHANNEL = 0
WIDE11_MAPPING_TRIGGER_TYPE = TRIGGER_RISING
WIDE11_MAPPING_DEFAULT_HOLD_SAMPLES = 64
WIDE11_MAPPING_DEFAULT_CHECK_SAMPLES = 8192
WIDE11_MAPPING_OBSERVED_BITS = (0, 1, 8, 10)
WIDE11_MAPPING_NIBBLE_TO_SAMPLE_BITS = (0, 1, 8, 10)
WIDE11_MAPPING_LOW_OTHERS_ZERO_MASK = 0x07FE
WIDE11_MAPPING_GP10_UART_DEFAULT_STIMULUS = "UUUUUUUU"
WIDE11_MAPPING_GP10_UART_DEFAULT_DEVICE = "/dev/ttyACM1"
WIDE11_MAPPING_GP10_UART_DEFAULT_BAUD = 115200
WIDE11_MAPPING_DEFAULT_PATTERN_NIBBLES = (0x1, 0x3, 0x2, 0x6, 0x4, 0xC, 0x8, 0x9, 0xB, 0xF, 0x5, 0xD, 0x0, 0xA, 0x2, 0x1)
UART_STIMULUS_BUSY_RETRY_WINDOW_S = 5.0
UART_STIMULUS_BUSY_RETRY_SLEEP_S = 0.05
WIDE11_MAPPING_PHYSICAL_PREREQUISITE = (
    "External 3.3V-compatible pattern generator required: connect generator D0->GP10 "
    "(captured bit0 and trigger channel 0), D1->GP11 (bit1), D2->GP18 (bit8), "
    "D3->GP20 (bit10), and generator ground to Linkr Debugger ground. "
    "Hold the initial idle state with GP10 low before arming, then emit the declared "
    "repeating nibble pattern with each nibble held for --wide11-map-hold-samples "
    "100MHz sample ticks. Do not use HTTP safe-GPIO outputs as stimulus: WIDE11 PIO "
    "preparation configures GP10-GP20 as inputs for capture."
)
WIDE11_MAPPING_GP10_UART_PREREQUISITE = (
    "Reduced single-wire setup: connect /dev/ttyACM1 TX to GP10 and share ground; "
    "leave GP11-GP20 externally low. This validates only GP10 DATA bit0 activity "
    "and absence of high/crosstalk on bits1..10. It does not validate independent "
    "high-state mapping for GP11-GP20."
)
WIDE11_BURST_EXPECTED_DATA_FRAMES = 98
TELEMETRY_ISOLATION_DEFAULT_RATE_HZ = 100
TELEMETRY_ISOLATION_DEFAULT_BASELINE_SAMPLES = 2
TELEMETRY_ISOLATION_DEFAULT_POST_RELEASE_SAMPLES = 2
TELEMETRY_ISOLATION_GRACE_S = 0.05
STREAM_CHUNK_SAMPLES = 1024
MAX_STREAM_DIAGNOSTIC_RECORDS = 16
EVENT_TRIGGERED = 2
EVENT_STOPPED = 4
EVENT_OVERRUN = 5
EVENT_ERROR = 6
TERMINAL_REASON_SERVER_STOPPED = "server_stopped"
TERMINAL_REASON_SERVER_OVERRUN = "server_overrun"
TERMINAL_REASON_SERVER_ERROR = "server_error"

TRIGGER_NAMES = {
    TRIGGER_NONE: "none",
    TRIGGER_RISING: "rising",
    TRIGGER_FALLING: "falling",
    TRIGGER_EITHER: "either",
}
TRIGGER_TYPES_BY_NAME = {name: trigger_type for trigger_type, name in TRIGGER_NAMES.items()}

@dataclass(frozen=True)
class ModeCase:
    mode_id: int
    mask: int
    pins: list[int]
    sample_bytes: int


MODE_CASES = {
    "SINGLE": ModeCase(mode_id=MODE_FAST8, mask=MASK_SINGLE, pins=[10], sample_bytes=1),
    "FAST8": ModeCase(mode_id=MODE_FAST8, mask=MASK_FAST8, pins=list(range(10, 18)), sample_bytes=1),
    "FAST8_SPARSE_GP10_GP13_GP17": ModeCase(mode_id=MODE_FAST8, mask=MASK_FAST8_SPARSE_GP10_GP13_GP17, pins=[10, 13, 17], sample_bytes=1),
    "WIDE11": ModeCase(mode_id=MODE_WIDE11, mask=MASK_WIDE11, pins=list(range(10, 21)), sample_bytes=2),
}


@dataclass(frozen=True)
class SigrokHeader:
    frame_type: int
    frame_id: int
    payload_len: int


@dataclass(frozen=True)
class SigrokDataMeta:
    sample_index: int
    sample_count: int
    compression: int
    channel_mask: int


@dataclass(frozen=True)
class SigrokEvent:
    session_id: int
    type_detail: int
    sample_index: int | None


@dataclass(frozen=True, slots=True)
class SigrokEventRecord:
    type_detail: int
    sample_index: int | None


@dataclass
class StreamStats:
    data_frames: int = 0
    received_sample_count: int = 0
    payload_bytes: int = 0
    sample_index_gaps: int = 0
    disconnects: int = 0
    stopped_events: int = 0
    triggered_events: int = 0
    overrun_events: int = 0
    error_events: int = 0
    first_sample_index: int | None = None
    last_sample_index: int | None = None
    trigger_sample_index: int | None = None
    first_data_sample_indices: list[int] = field(default_factory=list)
    first_gap_records: list[dict[str, int]] = field(default_factory=list)
    invalid_sample_count_frames: int = 0
    payload_over_budget_frames: int = 0
    channel_mask_mismatch_frames: int = 0
    data_decode_error_frames: int = 0
    event_records: list[SigrokEventRecord] = field(default_factory=list)
    first_payload_budget_records: list[dict[str, int]] = field(default_factory=list)
    first_decode_error_records: list[dict[str, str]] = field(default_factory=list)

    def observe_data(
        self,
        meta: SigrokDataMeta,
        sample_payload_len: int,
        *,
        expected_channel_mask: int | None = None,
        sample_bytes: int | None = None,
    ) -> None:
        if meta.sample_count <= 0:
            self.invalid_sample_count_frames += 1
        if expected_channel_mask is not None and meta.channel_mask != expected_channel_mask:
            self.channel_mask_mismatch_frames += 1
        if sample_bytes is not None:
            payload_budget = meta.sample_count * sample_bytes
            if sample_payload_len > payload_budget:
                self.payload_over_budget_frames += 1
                if len(self.first_payload_budget_records) < MAX_STREAM_DIAGNOSTIC_RECORDS:
                    self.first_payload_budget_records.append({"sample_count": meta.sample_count, "payload_len": sample_payload_len, "budget": payload_budget})
        expected = None
        if self.last_sample_index is not None:
            expected = (self.last_sample_index + 1) & SIGROK_SAMPLE_INDEX_MASK
        if expected is not None and meta.sample_index != expected:
            self.sample_index_gaps += 1
            if len(self.first_gap_records) < MAX_STREAM_DIAGNOSTIC_RECORDS:
                self.first_gap_records.append({"expected": expected, "actual": meta.sample_index})
        if len(self.first_data_sample_indices) < MAX_STREAM_DIAGNOSTIC_RECORDS:
            self.first_data_sample_indices.append(meta.sample_index)
        if self.first_sample_index is None:
            self.first_sample_index = meta.sample_index
        self.last_sample_index = (meta.sample_index + meta.sample_count - 1) & SIGROK_SAMPLE_INDEX_MASK
        self.data_frames += 1
        self.received_sample_count += meta.sample_count
        self.payload_bytes += sample_payload_len

    def observe_decode_error(self, meta: SigrokDataMeta, error: Exception) -> None:
        self.data_decode_error_frames += 1
        if len(self.first_decode_error_records) < MAX_STREAM_DIAGNOSTIC_RECORDS:
            self.first_decode_error_records.append({
                "sample_count": str(meta.sample_count),
                "compression": str(meta.compression),
                "error": str(error),
            })

    def rates(self, elapsed_s: float) -> dict[str, float]:
        if elapsed_s <= 0:
            return {"effective_receive_samples_per_s": 0.0, "effective_receive_bytes_per_s": 0.0}
        return {
            "effective_receive_samples_per_s": self.received_sample_count / elapsed_s,
            "effective_receive_bytes_per_s": self.payload_bytes / elapsed_s,
        }

    def observe_event(self, payload: bytes) -> SigrokEvent:
        event = parse_event(payload)
        if len(self.event_records) < MAX_STREAM_DIAGNOSTIC_RECORDS:
            self.event_records.append(SigrokEventRecord(type_detail=event.type_detail, sample_index=event.sample_index))
        if event.type_detail == EVENT_TRIGGERED:
            self.triggered_events += 1
            if event.sample_index is not None and self.trigger_sample_index is None:
                self.trigger_sample_index = event.sample_index
        elif event.type_detail == EVENT_STOPPED:
            self.stopped_events += 1
        elif event.type_detail == EVENT_OVERRUN:
            self.overrun_events += 1
        elif event.type_detail == EVENT_ERROR:
            self.error_events += 1
        return event

    @staticmethod
    def is_terminal_event(event: SigrokEvent) -> bool:
        return event.type_detail in (EVENT_STOPPED, EVENT_OVERRUN, EVENT_ERROR)

    @staticmethod
    def terminal_reason_for_event(event: SigrokEvent) -> str | None:
        if event.type_detail == EVENT_STOPPED:
            return TERMINAL_REASON_SERVER_STOPPED
        if event.type_detail == EVENT_OVERRUN:
            return TERMINAL_REASON_SERVER_OVERRUN
        if event.type_detail == EVENT_ERROR:
            return TERMINAL_REASON_SERVER_ERROR
        return None

    def trigger_sample_offset(self) -> int | None:
        if self.trigger_sample_index is None or self.first_sample_index is None:
            return None
        return (self.trigger_sample_index - self.first_sample_index) & SIGROK_SAMPLE_INDEX_MASK

    def trigger_sample_offset_valid(self) -> bool:
        offset = self.trigger_sample_offset()
        return offset is not None and offset < self.received_sample_count


def parse_csv_ints(value: str) -> list[int]:
    values: list[int] = []
    for item in value.split(","):
        item = item.strip()
        if item:
            values.append(int(item, 0))
    return values


def parse_csv_nibbles(value: str) -> list[int]:
    nibbles = parse_csv_ints(value)
    if not nibbles:
        raise ValueError("at least one WIDE11 mapping pattern nibble is required")
    invalid = [nibble for nibble in nibbles if not 0 <= nibble <= 0xF]
    if invalid:
        raise ValueError(f"WIDE11 mapping pattern nibbles must be 0..0xf: {invalid}")
    return nibbles


def parse_csv_trigger_types(value: str) -> list[int]:
    trigger_types: list[int] = []
    for item in value.split(","):
        item = item.strip().lower()
        if not item:
            continue
        if item not in TRIGGER_TYPES_BY_NAME:
            raise ValueError(f"unknown trigger type {item}")
        trigger_types.append(TRIGGER_TYPES_BY_NAME[item])
    return trigger_types


def wide11_mapping_nibble_to_sample(nibble: int) -> int:
    if not 0 <= nibble <= 0xF:
        raise ValueError("WIDE11 mapping nibble must be 0..0xf")
    sample = 0
    for nibble_bit, sample_bit in enumerate(WIDE11_MAPPING_NIBBLE_TO_SAMPLE_BITS):
        if (nibble & (1 << nibble_bit)) != 0:
            sample |= 1 << sample_bit
    return sample


def wide11_mapping_expected_sample(pattern_nibbles: list[int], hold_samples: int, sample_index: int, phase_samples: int = 0) -> int:
    if hold_samples <= 0:
        raise ValueError("WIDE11 mapping hold_samples must be positive")
    if not pattern_nibbles:
        raise ValueError("WIDE11 mapping pattern must not be empty")
    pattern_index = ((sample_index + phase_samples) // hold_samples) % len(pattern_nibbles)
    return wide11_mapping_nibble_to_sample(pattern_nibbles[pattern_index])


def decode_wide11_sample_words(sample_payload: bytes) -> list[int]:
    if len(sample_payload) % 2 != 0:
        raise ValueError("WIDE11 DATA payload length must be even")
    return [sample_payload[offset] | (sample_payload[offset + 1] << 8) for offset in range(0, len(sample_payload), 2)]


def validate_wide11_mapping_payload(
    sample_payload: bytes,
    *,
    pattern_nibbles: list[int],
    hold_samples: int,
    check_samples: int,
) -> dict[str, Any]:
    samples = decode_wide11_sample_words(sample_payload)
    if check_samples <= 0:
        raise ValueError("WIDE11 mapping check_samples must be positive")
    if len(samples) < check_samples:
        return {"pass": False, "reason": "not enough decoded samples for WIDE11 mapping check", "decoded_samples": len(samples), "required_samples": check_samples}
    pattern_period_samples = hold_samples * len(pattern_nibbles)
    if pattern_period_samples <= 1:
        raise ValueError("WIDE11 mapping pattern period must be at least two samples")

    best: dict[str, Any] | None = None
    for phase in range(pattern_period_samples):
        mismatches: list[dict[str, int]] = []
        mismatch_count = 0
        bit_values = {bit: set() for bit in WIDE11_MAPPING_OBSERVED_BITS}
        bit_transitions = {bit: 0 for bit in WIDE11_MAPPING_OBSERVED_BITS}
        previous_expected = wide11_mapping_expected_sample(pattern_nibbles, hold_samples, 0, phase)
        for index in range(check_samples):
            observed = samples[index] & MASK_WIDE11
            expected = wide11_mapping_expected_sample(pattern_nibbles, hold_samples, index, phase)
            for bit in WIDE11_MAPPING_OBSERVED_BITS:
                bit_values[bit].add((observed >> bit) & 1)
                if index > 0 and ((previous_expected >> bit) & 1) != ((expected >> bit) & 1):
                    bit_transitions[bit] += 1
            if observed != expected:
                mismatch_count += 1
                if len(mismatches) < MAX_STREAM_DIAGNOSTIC_RECORDS:
                    mismatches.append({"sample_index": index, "expected": expected, "observed": observed, "xor": observed ^ expected})
            previous_expected = expected
        candidate = {
            "phase_samples": phase,
            "mismatch_count": mismatch_count,
            "first_mismatches": mismatches,
            "bit_values": {str(bit): sorted(values) for bit, values in bit_values.items()},
            "bit_transitions": {str(bit): count for bit, count in bit_transitions.items()},
        }
        if best is None or mismatch_count < int(best["mismatch_count"]):
            best = candidate
        if mismatch_count == 0:
            independent_bits = all(bit_values[bit] == {0, 1} and bit_transitions[bit] > 0 for bit in WIDE11_MAPPING_OBSERVED_BITS)
            if independent_bits:
                return {"pass": True, "reason": "ok", "checked_samples": check_samples, **candidate}

    assert best is not None
    inactive_bits = [
        bit for bit, values in cast(dict[str, list[int]], best["bit_values"]).items()
        if values != [0, 1] or int(cast(dict[str, int], best["bit_transitions"])[bit]) <= 0
    ]
    if inactive_bits:
        return {"pass": False, "reason": "WIDE11 mapping pattern did not independently exercise every required bit", "inactive_bits": inactive_bits, "checked_samples": check_samples, **best}
    return {"pass": False, "reason": "WIDE11 mapping payload mismatches expected pattern", "checked_samples": check_samples, **best}


def validate_wide11_gp10_uart_low_others_payload(sample_payload: bytes, *, check_samples: int) -> dict[str, Any]:
    samples = decode_wide11_sample_words(sample_payload)
    if check_samples <= 0:
        raise ValueError("WIDE11 GP10 UART check_samples must be positive")
    if len(samples) < check_samples:
        return {"pass": False, "reason": "not enough decoded samples for WIDE11 GP10 UART check", "decoded_samples": len(samples), "required_samples": check_samples}

    gp10_values: set[int] = set()
    gp10_runs: list[dict[str, int]] = []
    current_value: int | None = None
    current_run_start = 0
    zero_mask_violations: list[dict[str, int]] = []
    zero_mask_violation_count = 0

    for index, sample in enumerate(samples[:check_samples]):
        observed = sample & MASK_WIDE11
        gp10 = observed & 0x1
        gp10_values.add(gp10)
        if current_value is None:
            current_value = gp10
            current_run_start = index
        elif gp10 != current_value:
            if len(gp10_runs) < MAX_STREAM_DIAGNOSTIC_RECORDS:
                gp10_runs.append({"value": current_value, "start": current_run_start, "length": index - current_run_start})
            current_value = gp10
            current_run_start = index

        unexpected = observed & WIDE11_MAPPING_LOW_OTHERS_ZERO_MASK
        if unexpected != 0:
            zero_mask_violation_count += 1
            if len(zero_mask_violations) < MAX_STREAM_DIAGNOSTIC_RECORDS:
                zero_mask_violations.append({"sample_index": index, "observed": observed, "unexpected_mask": unexpected})

    if current_value is not None and len(gp10_runs) < MAX_STREAM_DIAGNOSTIC_RECORDS:
        gp10_runs.append({"value": current_value, "start": current_run_start, "length": check_samples - current_run_start})

    gp10_low_high_seen = gp10_values == {0, 1}
    gp10_transition_count = sum(1 for run in gp10_runs[1:] if run["length"] > 0)
    result = {
        "pass": gp10_low_high_seen and zero_mask_violation_count == 0,
        "reason": "ok",
        "checked_samples": check_samples,
        "profile": "gp10_uart_low_others",
        "gp10_bit": 0,
        "zero_mask": WIDE11_MAPPING_LOW_OTHERS_ZERO_MASK,
        "zero_mask_bits": {f"GP{pin}": pin - 10 for pin in range(11, 21)},
        "gp10_values": sorted(gp10_values),
        "gp10_low_high_seen": gp10_low_high_seen,
        "gp10_transition_count": gp10_transition_count,
        "first_gp10_runs": gp10_runs,
        "zero_mask_violation_count": zero_mask_violation_count,
        "first_zero_mask_violations": zero_mask_violations,
        "limitations": [
            "Independent high-state mapping for GP11-GP20 is not validated by this reduced single-wire test",
            "Selected external mapping coverage for GP11/GP18/GP20 is not exercised when only GP10 is driven",
        ],
    }
    if zero_mask_violation_count != 0:
        result["reason"] = "WIDE11 GP10 UART low-others check saw unexpected high/crosstalk on bits1..10"
    elif not gp10_low_high_seen:
        result["reason"] = "WIDE11 GP10 UART low-others check did not observe both low and high runs on bit0"
    return result


def build_frame(frame_type: int, payload: bytes = b"", frame_id: int = 1) -> bytes:
    return struct.pack("<BBBIH", SIGROK_MAGIC, SIGROK_PROTOCOL_VERSION, frame_type, frame_id, len(payload)) + payload


def parse_header(data: bytes) -> SigrokHeader:
    if len(data) != SIGROK_HEADER_BYTES:
        raise ValueError(f"header must be {SIGROK_HEADER_BYTES} bytes")
    magic, version, frame_type, frame_id, payload_len = struct.unpack("<BBBIH", data)
    if magic != SIGROK_MAGIC:
        raise ValueError(f"bad magic 0x{magic:02x}")
    if version != SIGROK_PROTOCOL_VERSION:
        raise ValueError(f"unsupported protocol version {version}")
    return SigrokHeader(frame_type=frame_type, frame_id=frame_id, payload_len=payload_len)


def parse_data_meta(payload: bytes) -> SigrokDataMeta:
    if len(payload) < SIGROK_DATA_META_BYTES:
        raise ValueError("DATA payload shorter than 8-byte metadata")
    return SigrokDataMeta(
        sample_index=payload[0] | (payload[1] << 8) | (payload[2] << 16),
        sample_count=payload[3] | (payload[4] << 8),
        compression=payload[5],
        channel_mask=payload[6] | (payload[7] << 8),
    )


def sigrok_bytes_per_sample(channel_mask: int) -> int:
    channel_count = int(channel_mask & 0xFFFF).bit_count()
    if channel_count == 0:
        return 0
    return (channel_count + 7) // 8


def decode_sigrok_data_payload(meta: SigrokDataMeta, sample_payload: bytes) -> bytes:
    if meta.sample_count <= 0:
        raise ValueError("DATA sample_count must be non-zero")
    bytes_per_sample = sigrok_bytes_per_sample(meta.channel_mask)
    if bytes_per_sample <= 0:
        raise ValueError("DATA channel_mask selects no channels")
    expected_len = meta.sample_count * bytes_per_sample
    if meta.compression in (COMPRESSION_NONE, COMPRESSION_BIT_PACK):
        if len(sample_payload) != expected_len:
            raise ValueError(f"BIT_PACK payload length {len(sample_payload)} != expected {expected_len}")
        return sample_payload
    if meta.compression == COMPRESSION_BIT_PACK_RLE:
        tuple_len = bytes_per_sample + 2
        if len(sample_payload) == 0 or len(sample_payload) % tuple_len != 0:
            raise ValueError("BIT_PACK_RLE payload has truncated tuple")
        out = bytearray()
        pos = 0
        expanded_count = 0
        while pos < len(sample_payload):
            value = sample_payload[pos:pos + bytes_per_sample]
            run_pos = pos + bytes_per_sample
            if run_pos + 2 > len(sample_payload):
                raise ValueError("BIT_PACK_RLE tuple missing run_count")
            run_count = sample_payload[run_pos] | (sample_payload[run_pos + 1] << 8)
            if run_count == 0:
                raise ValueError("BIT_PACK_RLE run_count must be non-zero")
            if expanded_count + run_count > meta.sample_count:
                raise ValueError("BIT_PACK_RLE expanded sample count overflows metadata")
            out.extend(value * run_count)
            expanded_count += run_count
            pos += tuple_len
        if expanded_count != meta.sample_count:
            raise ValueError("BIT_PACK_RLE expanded sample count does not match metadata")
        if len(out) != expected_len:
            raise ValueError("BIT_PACK_RLE expanded byte length does not match metadata")
        return bytes(out)
    if meta.compression == COMPRESSION_RLE:
        raise ValueError("standalone RLE is not valid for the current DATA sample decoder")
    raise ValueError(f"unsupported DATA compression {meta.compression}")


def observe_sigrok_data_payload(
    stats: StreamStats,
    meta: SigrokDataMeta,
    frame_payload: bytes,
    *,
    expected_channel_mask: int | None = None,
    sample_bytes: int | None = None,
) -> None:
    sample_payload = frame_payload[SIGROK_DATA_META_BYTES:]
    try:
        _ = decode_sigrok_data_payload(meta, sample_payload)
    except ValueError as exc:
        stats.observe_decode_error(meta, exc)
    stats.observe_data(
        meta,
        len(sample_payload),
        expected_channel_mask=expected_channel_mask,
        sample_bytes=sample_bytes,
    )


def parse_event(payload: bytes) -> SigrokEvent:
    if len(payload) < 3:
        return SigrokEvent(session_id=0, type_detail=0, sample_index=None)
    sample_index = None
    if len(payload) >= 6:
        sample_index = payload[3] | (payload[4] << 8) | (payload[5] << 16)
    return SigrokEvent(
        session_id=payload[0] | (payload[1] << 8),
        type_detail=payload[2],
        sample_index=sample_index,
    )


def parse_ack(payload: bytes) -> dict[str, int]:
    if len(payload) < 6:
        raise ValueError("ACK payload shorter than 6 bytes")
    return {
        "session_id": payload[0] | (payload[1] << 8),
        "state": payload[2],
        "actual_rate_khz": payload[3] | (payload[4] << 8) | (payload[5] << 16),
    }


def parse_error(payload: bytes) -> dict[str, int]:
    if len(payload) < 3:
        raise ValueError("ERROR payload shorter than 3 bytes")
    return {"error_code": payload[0], "detail": payload[1] | (payload[2] << 8)}


def parse_hello_resp(payload: bytes) -> dict[str, int | bool]:
    if len(payload) < 5:
        raise ValueError("HELLO payload shorter than 5 bytes")
    server_flags = payload[1]
    return {
        "protocol_version": payload[0],
        "server_flags": server_flags,
        "supports_config_v2": (server_flags & SERVER_FLAG_CONFIG_V2) != 0,
        "supports_generic_packed_burst": (server_flags & SERVER_FLAG_GENERIC_PACKED_BURST) != 0,
        "expected_high_rate_flags": SERVER_EXPECTED_HIGH_RATE_FLAGS,
        "expected_high_rate_flags_present": (server_flags & SERVER_EXPECTED_HIGH_RATE_FLAGS) == SERVER_EXPECTED_HIGH_RATE_FLAGS,
        "mode_count": payload[2],
        "max_payload_len": payload[3] | (payload[4] << 8),
    }


def handshake_supports_config_v2(handshake: Mapping[str, object]) -> bool:
    hello = handshake.get("hello")
    return isinstance(hello, Mapping) and bool(hello.get("supports_config_v2"))


def handshake_supports_generic_packed_burst(handshake: Mapping[str, object]) -> bool:
    hello = handshake.get("hello")
    return isinstance(hello, Mapping) and bool(hello.get("supports_generic_packed_burst"))


def handshake_has_expected_high_rate_flags(handshake: Mapping[str, object]) -> bool:
    hello = handshake.get("hello")
    return isinstance(hello, Mapping) and bool(hello.get("expected_high_rate_flags_present"))


def recv_exact(sock: socket.socket, byte_count: int, deadline: float | None) -> bytes:
    chunks: list[bytes] = []
    remaining = byte_count
    while remaining > 0:
        if not chunks and deadline is not None and time.monotonic() >= deadline:
            raise TimeoutError(f"timeout while receiving {byte_count} bytes")
        chunk = sock.recv(remaining)
        if not chunk:
            raise ConnectionError("peer disconnected")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def recv_frame(sock: socket.socket, timeout_s: float) -> tuple[SigrokHeader, bytes]:
    deadline = time.monotonic() + timeout_s
    header = parse_header(recv_exact(sock, SIGROK_HEADER_BYTES, deadline))
    payload = recv_exact(sock, header.payload_len, None) if header.payload_len else b""
    return header, payload


def send_request(sock: socket.socket, frame_type: int, payload: bytes, frame_id: int, timeout_s: float) -> tuple[SigrokHeader, bytes]:
    sock.sendall(build_frame(frame_type, payload, frame_id))
    return recv_frame(sock, timeout_s)


def sigrok_send_request_wait_response(
    sock: socket.socket,
    request_type: int,
    payload: bytes,
    frame_id: int,
    expected_response_type: int,
    timeout_s: float,
) -> tuple[SigrokHeader, bytes, dict[str, int]]:
    sock.sendall(build_frame(request_type, payload, frame_id))
    deadline = time.monotonic() + timeout_s
    diagnostics = {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}
    while time.monotonic() < deadline:
        header, response_payload = recv_frame(sock, max(0.05, deadline - time.monotonic()))
        if header.frame_type == expected_response_type and header.frame_id == frame_id:
            return header, response_payload, diagnostics
        if header.frame_type == FRAME_ERROR:
            return header, response_payload, diagnostics
        if header.frame_type == FRAME_DATA:
            diagnostics["skipped_data_frames"] += 1
        elif header.frame_type == FRAME_EVENT:
            diagnostics["skipped_events"] += 1
        else:
            diagnostics["skipped_other_frames"] += 1
    raise TimeoutError(f"timeout waiting for response 0x{expected_response_type:02x} id {frame_id}")


def strict_start_wait_failure_reason(start_wait: Mapping[str, object]) -> str | None:
    skipped_data_frames = cast(int, start_wait.get("skipped_data_frames", 0) or 0)
    skipped_events = cast(int, start_wait.get("skipped_events", 0) or 0)
    if skipped_data_frames == 0 and skipped_events == 0:
        return None
    skipped_other_frames = cast(int, start_wait.get("skipped_other_frames", 0) or 0)
    return (
        "DATA/EVENT arrived before START_RESP: "
        f"skipped_data_frames={skipped_data_frames}, "
        f"skipped_events={skipped_events}, "
        f"skipped_other_frames={skipped_other_frames}, "
        f"start_wait={dict(start_wait)}"
    )


def exception_detail(exc: Exception) -> dict[str, str]:
    return {"type": type(exc).__name__, "message": str(exc)}


def build_config_payload(
    mode_id: int,
    channel_mask: int,
    samplerate_khz: int,
    pre_samples: int,
    post_samples: int,
    trigger_type: int = TRIGGER_NONE,
    trigger_channel: int = 0,
) -> bytes:
    if not 0 <= pre_samples <= 0xFFFF or not 0 <= post_samples <= 0xFFFF:
        raise ValueError("sigrok pre/post samples are uint16")
    if trigger_type not in TRIGGER_NAMES:
        raise ValueError(f"unknown trigger type {trigger_type}")
    if not 0 <= trigger_channel <= 0xFF:
        raise ValueError("sigrok trigger channel is uint8")
    payload = struct.pack("<BBBH", mode_id, trigger_type, trigger_channel, channel_mask)
    payload += struct.pack("<I", samplerate_khz)[:3]
    payload += struct.pack("<HH", pre_samples, post_samples)
    return payload


def build_config_v2_payload(
    mode_id: int,
    channel_mask: int,
    samplerate_khz: int,
    pre_samples: int,
    post_samples: int,
    trigger_type: int = TRIGGER_NONE,
    trigger_channel: int = 0,
) -> bytes:
    if not 0 <= pre_samples <= 0xFFFFFFFF or not 0 <= post_samples <= 0xFFFFFFFF:
        raise ValueError("sigrok pre/post samples are uint32")
    if trigger_type not in TRIGGER_NAMES:
        raise ValueError(f"unknown trigger type {trigger_type}")
    if not 0 <= trigger_channel <= 0xFF:
        raise ValueError("sigrok trigger channel is uint8")
    payload = struct.pack("<BBBH", mode_id, trigger_type, trigger_channel, channel_mask)
    payload += struct.pack("<I", samplerate_khz)[:3]
    payload += struct.pack("<II", pre_samples, post_samples)
    return payload


def build_config_request(
    mode_id: int,
    channel_mask: int,
    samplerate_khz: int,
    pre_samples: int,
    post_samples: int,
    trigger_type: int = TRIGGER_NONE,
    trigger_channel: int = 0,
    supports_config_v2: bool = False,
) -> tuple[int, bytes, str]:
    needs_v2 = pre_samples > 0xFFFF or post_samples > 0xFFFF
    if not needs_v2:
        return (
            FRAME_CONFIG_REQ,
            build_config_payload(mode_id, channel_mask, samplerate_khz, pre_samples, post_samples, trigger_type, trigger_channel),
            "v1",
        )
    if not supports_config_v2:
        raise ValueError("sigrok pre/post samples require CONFIG_V2, but HELLO server_flags bit0 is not set")
    return (
        FRAME_CONFIG_V2_REQ,
        build_config_v2_payload(mode_id, channel_mask, samplerate_khz, pre_samples, post_samples, trigger_type, trigger_channel),
        "v2",
    )


def http_json_request(method: str, url: str, timeout_s: float, body: Mapping[str, object] | None = None) -> dict[str, object]:
    data = None
    headers = {"Accept": "application/json"}
    if body is not None:
        data = json.dumps(body, separators=(",", ":")).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            payload = response.read()
            decoded = json.loads(payload.decode("utf-8")) if payload else {}
            parsed: dict[str, object] = decoded if isinstance(decoded, dict) else {"value": decoded}
            parsed["_http_status"] = response.status
            parsed["_payload_bytes"] = len(payload)
            return parsed
    except urllib.error.HTTPError as exc:
        payload = exc.read()
        try:
            decoded = json.loads(payload.decode("utf-8")) if payload else {}
            parsed = decoded if isinstance(decoded, dict) else {"value": decoded}
        except json.JSONDecodeError:
            parsed = {"raw": payload.decode("utf-8", errors="replace")}
        parsed["_http_status"] = exc.code
        parsed["_payload_bytes"] = len(payload)
        return parsed


def board_health(http_base: str, timeout_s: float) -> dict[str, Any]:
    started = time.monotonic()
    try:
        payload = http_json_request("GET", f"{http_base}/api/v1/status", timeout_s)
        board_monitoring = payload.get("board_monitoring")
        memory = board_monitoring.get("memory") if isinstance(board_monitoring, Mapping) else None
        return {
            "ok": bool(payload.get("ok")),
            "http_status": payload.get("_http_status"),
            "elapsed_s": time.monotonic() - started,
            "error": payload.get("error"),
            "board_monitoring_available": isinstance(board_monitoring, Mapping),
            "board_monitoring": board_monitoring if isinstance(board_monitoring, Mapping) else None,
            "arena_telemetry": {
                "memory_available": isinstance(memory, Mapping),
                "current_pressure": memory.get("current_pressure") if isinstance(memory, Mapping) else None,
                "peak_pressure": memory.get("peak_pressure") if isinstance(memory, Mapping) else None,
            },
        }
    except Exception as exc:  # noqa: BLE001 - surfaced in machine-readable result
        return {"ok": False, "elapsed_s": time.monotonic() - started, "error": str(exc)}


def adc_http_health(http_base: str, timeout_s: float) -> dict[str, Any]:
    started = time.monotonic()
    try:
        payload = http_json_request("GET", f"{http_base}/api/v1/adc/read", timeout_s)
        readings = payload.get("readings")
        return {
            "ok": bool(payload.get("ok")) and isinstance(readings, list) and len(readings) > 0,
            "http_status": payload.get("_http_status"),
            "elapsed_s": time.monotonic() - started,
            "reading_count": len(readings) if isinstance(readings, list) else 0,
            "error": payload.get("error"),
        }
    except Exception as exc:  # noqa: BLE001
        return {"ok": False, "elapsed_s": time.monotonic() - started, "error": str(exc)}


def sigrok_handshake(sock: socket.socket, timeout_s: float) -> dict[str, Any]:
    hello_header, hello_payload, hello_wait = sigrok_send_request_wait_response(sock, FRAME_HELLO_REQ, b"", 1, FRAME_HELLO_RESP, timeout_s)
    if hello_header.frame_type != FRAME_HELLO_RESP:
        raise RuntimeError(f"expected HELLO_RESP, got 0x{hello_header.frame_type:02x}")
    hello = parse_hello_resp(hello_payload)
    caps_header, caps_payload, caps_wait = sigrok_send_request_wait_response(sock, FRAME_CAPS_REQ, b"", 2, FRAME_CAPS_RESP, timeout_s)
    if caps_header.frame_type != FRAME_CAPS_RESP:
        raise RuntimeError(f"expected CAPS_RESP, got 0x{caps_header.frame_type:02x}")
    return {"hello": hello, "hello_payload_bytes": len(hello_payload), "caps_payload_bytes": len(caps_payload), "hello_wait": hello_wait, "caps_wait": caps_wait}


def sigrok_stop(sock: socket.socket, timeout_s: float, frame_id: int) -> dict[str, Any]:
    try:
        sock.sendall(build_frame(FRAME_STOP_REQ, b"", frame_id))
        deadline = time.monotonic() + timeout_s
        skipped_data_frames = 0
        skipped_events = 0
        while time.monotonic() < deadline:
            header, payload = recv_frame(sock, max(0.05, deadline - time.monotonic()))
            if header.frame_type == FRAME_STOP_RESP:
                return {
                    "received": True,
                    "sent": True,
                    "frame_type": header.frame_type,
                    "ack": parse_ack(payload),
                    "skipped_data_frames": skipped_data_frames,
                    "skipped_events": skipped_events,
                }
            if header.frame_type == FRAME_ERROR:
                return {
                    "received": True,
                    "sent": True,
                    "frame_type": header.frame_type,
                    "error": parse_error(payload),
                    "skipped_data_frames": skipped_data_frames,
                    "skipped_events": skipped_events,
                }
            if header.frame_type == FRAME_DATA:
                skipped_data_frames += 1
            elif header.frame_type == FRAME_EVENT:
                skipped_events += 1
        return {
            "received": False,
            "sent": True,
            "reason": "timeout waiting for STOP_RESP",
            "skipped_data_frames": skipped_data_frames,
            "skipped_events": skipped_events,
        }
    except Exception as exc:  # noqa: BLE001
        return {"received": False, "sent": False, "reason": str(exc)}


def sigrok_immediate_restart(sock: socket.socket, mode_id: int, channel_mask: int, samplerate_khz: int, timeout_s: float) -> dict[str, Any]:
    try:
        config = build_config_payload(mode_id, channel_mask, samplerate_khz, 0, 1)
        header, payload, config_wait = sigrok_send_request_wait_response(sock, FRAME_CONFIG_REQ, config, 200, FRAME_CONFIG_RESP, timeout_s)
        if header.frame_type != FRAME_CONFIG_RESP:
            error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
            return {"ok": False, "reason": f"restart CONFIG got 0x{header.frame_type:02x}", "error": error, "config_wait": config_wait}
        config_ack = parse_ack(payload)
        header, payload, start_wait = sigrok_send_request_wait_response(sock, FRAME_START_REQ, b"", 201, FRAME_START_RESP, timeout_s)
        if header.frame_type != FRAME_START_RESP:
            error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
            return {"ok": False, "reason": f"restart START got 0x{header.frame_type:02x}", "config_ack": config_ack, "error": error, "config_wait": config_wait, "start_wait": start_wait}
        start_ack = parse_ack(payload)
        start_wait_reason = strict_start_wait_failure_reason(start_wait)
        if start_wait_reason is not None:
            stop = sigrok_stop(sock, timeout_s, 202)
            return {"ok": False, "reason": start_wait_reason, "config_ack": config_ack, "start_ack": start_ack, "config_wait": config_wait, "start_wait": start_wait, "stop_response": stop}
        stop = sigrok_stop(sock, timeout_s, 202)
        return {"ok": bool(stop.get("received")), "config_ack": config_ack, "start_ack": start_ack, "stop_response": stop, "config_wait": config_wait, "start_wait": start_wait}
    except Exception as exc:  # noqa: BLE001
        return {"ok": False, "reason": str(exc)}


def sigrok_fresh_restart_probe(host: str, port: int, mode_id: int, channel_mask: int, samplerate_khz: int, timeout_s: float) -> dict[str, Any]:
    stats = StreamStats()
    try:
        with socket.create_connection((host, port), timeout=timeout_s) as sock:
            sock.settimeout(timeout_s)
            handshake = sigrok_handshake(sock, timeout_s)
            config = build_config_payload(mode_id, channel_mask, samplerate_khz, 0, 1)
            header, payload, config_wait = sigrok_send_request_wait_response(sock, FRAME_CONFIG_REQ, config, 200, FRAME_CONFIG_RESP, timeout_s)
            if header.frame_type != FRAME_CONFIG_RESP:
                error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
                return {"ok": False, "reason": f"fresh CONFIG got 0x{header.frame_type:02x}", "error": error, "handshake": handshake, "config_wait": config_wait}
            config_ack = parse_ack(payload)
            header, payload, start_wait = sigrok_send_request_wait_response(sock, FRAME_START_REQ, b"", 201, FRAME_START_RESP, timeout_s)
            if header.frame_type != FRAME_START_RESP:
                error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
                return {"ok": False, "reason": f"fresh START got 0x{header.frame_type:02x}", "error": error, "handshake": handshake, "config_ack": config_ack, "config_wait": config_wait, "start_wait": start_wait}
            start_ack = parse_ack(payload)
            start_wait_reason = strict_start_wait_failure_reason(start_wait)
            if start_wait_reason is not None:
                stop = sigrok_stop(sock, timeout_s, 202)
                return {"ok": False, "reason": start_wait_reason, "error": None, "handshake": handshake, "config_ack": config_ack, "start_ack": start_ack, "config_wait": config_wait, "start_wait": start_wait, "stop_response": stop, "stats": asdict(stats)}
            deadline = time.monotonic() + timeout_s
            while stats.data_frames == 0 and time.monotonic() < deadline:
                try:
                    data_header, data_payload = recv_frame(sock, max(0.05, deadline - time.monotonic()))
                except TimeoutError:
                    break
                if data_header.frame_type == FRAME_DATA:
                    meta = parse_data_meta(data_payload)
                    observe_sigrok_data_payload(stats, meta, data_payload)
                elif data_header.frame_type == FRAME_ERROR:
                    return {"ok": False, "reason": "fresh DATA wait returned ERROR", "error": parse_error(data_payload), "handshake": handshake, "config_ack": config_ack, "start_ack": start_ack, "config_wait": config_wait, "start_wait": start_wait, "stats": asdict(stats)}
                elif data_header.frame_type == FRAME_EVENT:
                    event = stats.observe_event(data_payload)
                    _ = event.type_detail
            stop = sigrok_stop(sock, timeout_s, 202)
            ok = stats.data_frames > 0 and bool(stop.get("received")) and stop.get("frame_type") == FRAME_STOP_RESP
            reason = "ok" if ok else "fresh restart did not receive DATA before STOP" if stats.data_frames == 0 else "fresh restart STOP_RESP not received"
            return {"ok": ok, "reason": reason, "transport": "tcp", "fresh_connection": True, "handshake": handshake, "config_ack": config_ack, "start_ack": start_ack, "stop_response": stop, "config_wait": config_wait, "start_wait": start_wait, "stats": asdict(stats)}
    except Exception as exc:  # noqa: BLE001
        return {"ok": False, "reason": str(exc), "transport": "tcp", "fresh_connection": True}


def sigrok_tcp_wide11_deep_burst_raw_capture(
    host: str,
    port: int,
    timeout_s: float,
    *,
    on_start_resp: Any | None = None,
) -> dict[str, Any]:
    mode = MODE_CASES["WIDE11"]
    stats = StreamStats()
    result: dict[str, Any] = {
        "transport": "tcp",
        "mode": "WIDE11",
        "requested_samplerate_khz": WIDE11_MAPPING_RATE_KHZ,
        "sigrok_pre_samples": WIDE11_MAPPING_PRE_SAMPLES,
        "sigrok_post_samples": WIDE11_MAPPING_POST_SAMPLES,
        "expected_data_frames": WIDE11_BURST_EXPECTED_DATA_FRAMES,
    }
    started = time.monotonic()
    stream_started = 0.0
    stream_ended = 0.0
    stop_response: dict[str, Any] = {"received": False, "reason": "not sent"}
    try:
        with socket.create_connection((host, port), timeout=timeout_s) as sock:
            sock.settimeout(timeout_s)
            result["handshake"] = sigrok_handshake(sock, timeout_s)
            config_frame_type, config_payload, config_encoding = build_config_request(
                mode.mode_id,
                mode.mask,
                WIDE11_MAPPING_RATE_KHZ,
                WIDE11_MAPPING_PRE_SAMPLES,
                WIDE11_MAPPING_POST_SAMPLES,
                TRIGGER_NONE,
                0,
                handshake_supports_config_v2(cast(Mapping[str, object], result["handshake"])),
            )
            result["config_encoding"] = config_encoding
            result["config_frame_type"] = config_frame_type
            if config_frame_type != FRAME_CONFIG_V2_REQ or config_encoding != "v2":
                result.update({"ok": False, "reason": "WIDE11 deep burst did not use CONFIG_V2"})
                return result
            header, payload, config_wait = sigrok_send_request_wait_response(sock, config_frame_type, config_payload, 3, FRAME_CONFIG_RESP, timeout_s)
            result["config_wait"] = config_wait
            if header.frame_type != FRAME_CONFIG_RESP:
                error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
                result.update({"ok": False, "reason": f"CONFIG returned 0x{header.frame_type:02x}", "error": error})
                return result
            result["config_ack"] = parse_ack(payload)
            header, payload, start_wait = sigrok_send_request_wait_response(sock, FRAME_START_REQ, b"", 4, FRAME_START_RESP, timeout_s)
            result["start_wait"] = start_wait
            if header.frame_type != FRAME_START_RESP:
                error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
                result.update({"ok": False, "reason": f"START returned 0x{header.frame_type:02x}", "error": error})
                return result
            result["start_ack"] = parse_ack(payload)
            result["start_resp_monotonic_s"] = time.monotonic()
            start_wait_reason = strict_start_wait_failure_reason(start_wait)
            if start_wait_reason is not None:
                stop_response = sigrok_stop(sock, timeout_s, 5)
                result.update({"ok": False, "reason": start_wait_reason, "stop_response": stop_response})
                return result
            if on_start_resp is not None:
                on_start_resp(result)

            target_samples = WIDE11_MAPPING_PRE_SAMPLES + WIDE11_MAPPING_POST_SAMPLES
            stream_started = time.monotonic()
            read_deadline = stream_started + timeout_s
            while stats.received_sample_count < target_samples and time.monotonic() < read_deadline:
                try:
                    frame_header, frame_payload = recv_frame(sock, max(0.05, min(timeout_s, read_deadline - time.monotonic())))
                except TimeoutError:
                    break
                except ConnectionError as exc:
                    stats.disconnects += 1
                    result.setdefault("stream_disconnect_error", exception_detail(exc))
                    break
                if frame_header.frame_type == FRAME_DATA:
                    meta = parse_data_meta(frame_payload)
                    observe_sigrok_data_payload(stats, meta, frame_payload, expected_channel_mask=mode.mask, sample_bytes=mode.sample_bytes)
                elif frame_header.frame_type == FRAME_ERROR:
                    result["stream_error"] = parse_error(frame_payload)
                    break
                elif frame_header.frame_type == FRAME_EVENT:
                    event = stats.observe_event(frame_payload)
                    _ = event.type_detail
                    result.setdefault("events", 0)
                    result["events"] += 1
            stream_ended = time.monotonic()
            stop_response = sigrok_stop(sock, timeout_s, 5)
    except Exception as exc:  # noqa: BLE001
        stats.disconnects += 1
        result.update({"ok": False, "reason": str(exc)})
        result.setdefault("stream_disconnect_error", exception_detail(exc))

    checks = [
        (stats.received_sample_count >= WIDE11_MAPPING_POST_SAMPLES, "exact WIDE11 deep-burst sample count not received"),
        (stats.data_frames == WIDE11_BURST_EXPECTED_DATA_FRAMES, "WIDE11 deep burst DATA frame count mismatch"),
        (stats.sample_index_gaps == 0, "sample_index gaps detected"),
        (stats.invalid_sample_count_frames == 0, "invalid DATA sample_count detected"),
        (stats.payload_over_budget_frames == 0, "DATA payload exceeds sample-count budget"),
        (stats.channel_mask_mismatch_frames == 0, "DATA channel mask mismatch detected"),
        (stats.data_decode_error_frames == 0, "DATA payload decode errors detected"),
        (stats.disconnects == 0, "disconnects detected"),
        (bool(stop_response.get("received")) and stop_response.get("frame_type") == FRAME_STOP_RESP, "STOP_RESP not received"),
    ]
    ok = True
    reason = "ok"
    for passed, check_reason in checks:
        if not passed:
            ok = False
            reason = check_reason
            break
    result.update({
        "ok": ok,
        "reason": reason,
        "elapsed_s": time.monotonic() - started,
        "stream_elapsed_s": max(0.0, stream_ended - stream_started),
        "stats": asdict(stats),
        "stop_response": stop_response,
    })
    return result


def load_websocket_module() -> object:
    try:
        return importlib.import_module("websocket")
    except ImportError as exc:
        raise RuntimeError("websocket-client is required for sigrok WebSocket matrices") from exc


def create_live_session_ws_url(http_base: str, timeout_s: float) -> str:
    payload = http_json_request("POST", f"{http_base}/api/v1/live-sessions", timeout_s)
    if not bool(payload.get("ok")):
        raise RuntimeError(f"live-session create failed: {payload.get('error')}")
    ws_url = payload.get("ws_url")
    if not isinstance(ws_url, str) or not ws_url:
        raise RuntimeError("live-session response did not include ws_url")
    return ws_url


def telemetry_record_from_message(message: Mapping[str, Any], received_monotonic_s: float) -> dict[str, Any] | None:
    message_type = message.get("type")
    if message_type == "telemetry" and message.get("topic") == "adc":
        return {
            "sequence": message.get("sequence"),
            "sample_sequence": message.get("sample_sequence"),
            "uptime_us": message.get("uptime_us"),
            "device_t_mono_us": message.get("device_t_mono_us"),
            "received_monotonic_s": received_monotonic_s,
            "message_type": message_type,
        }
    if message_type == "telemetry-batch" and message.get("topic") == "adc":
        samples = message.get("samples")
        if not isinstance(samples, list) or not samples:
            return None
        last = samples[-1]
        if not isinstance(last, Mapping):
            return None
        return {
            "sequence": last.get("sequence"),
            "sample_sequence": last.get("sample_sequence"),
            "uptime_us": last.get("uptime_us"),
            "device_t_mono_us": last.get("device_t_mono_us"),
            "received_monotonic_s": received_monotonic_s,
            "message_type": message_type,
            "batch_sample_count": len(samples),
            "dropped_samples": message.get("dropped_samples"),
        }
    return None


def telemetry_sequence_value(record: Mapping[str, Any]) -> int | None:
    value = record.get("sample_sequence", record.get("sequence"))
    return value if isinstance(value, int) else None


def telemetry_device_time_value(record: Mapping[str, Any]) -> int | None:
    value = record.get("device_t_mono_us", record.get("uptime_us"))
    return value if isinstance(value, int) else None


def is_websocket_recv_timeout(exc: BaseException) -> bool:
    if isinstance(exc, TimeoutError):
        return True
    return type(exc).__name__ == "WebSocketTimeoutException"


def telemetry_is_sequence_reset(
    record: Mapping[str, Any],
    *,
    baseline_sequence: int | None,
    old_epoch_last_sequence: int | None,
    old_epoch_last_device_time_us: int | None,
) -> bool:
    sequence = telemetry_sequence_value(record)
    device_time_us = telemetry_device_time_value(record)
    if not isinstance(sequence, int) or not isinstance(device_time_us, int):
        return False
    if not isinstance(old_epoch_last_device_time_us, int) or device_time_us <= old_epoch_last_device_time_us:
        return False
    if isinstance(old_epoch_last_sequence, int) and sequence < old_epoch_last_sequence:
        return True
    return isinstance(baseline_sequence, int) and sequence <= baseline_sequence


def telemetry_read_until(
    ws_conn: object,
    *,
    timeout_s: float,
    min_records: int = 1,
    phase: str,
) -> dict[str, Any]:
    records: list[dict[str, Any]] = []
    malformed_json = 0
    binary_frames = 0
    other_json = 0
    recv_timeouts = 0
    errors: list[str] = []
    deadline = time.monotonic() + timeout_s
    try:
        getattr(ws_conn, "settimeout")(max(0.05, timeout_s))
        while len(records) < min_records and time.monotonic() < deadline:
            try:
                raw = getattr(ws_conn, "recv")()
            except Exception as exc:  # noqa: BLE001
                if is_websocket_recv_timeout(exc):
                    recv_timeouts += 1
                    break
                errors.append(f"recv:{type(exc).__name__}:{exc}")
                break
            received = time.monotonic()
            if isinstance(raw, bytes | bytearray):
                binary_frames += 1
                continue
            if not isinstance(raw, str):
                other_json += 1
                continue
            try:
                parsed = json.loads(raw)
            except json.JSONDecodeError as exc:
                malformed_json += 1
                errors.append(f"json:{exc}")
                continue
            if not isinstance(parsed, Mapping):
                other_json += 1
                continue
            record = telemetry_record_from_message(parsed, received)
            if record is None:
                other_json += 1
                continue
            records.append(record)
    except Exception as exc:  # noqa: BLE001
        errors.append(f"outer:{type(exc).__name__}:{exc}")
    return {
        "phase": phase,
        "records": records,
        "record_count": len(records),
        "connected": not errors,
        "malformed_json": malformed_json,
        "binary_frames": binary_frames,
        "other_json": other_json,
        "recv_timeouts": recv_timeouts,
        "errors": errors,
        "elapsed_s": max(0.0, timeout_s - max(0.0, deadline - time.monotonic())),
    }


def telemetry_subscribe(ws_conn: object, rate_hz: int, batch_size: int | None, timeout_s: float) -> dict[str, Any]:
    request: dict[str, Any] = {"type": "subscribe", "topic": "live", "rate_hz": rate_hz, "id": "wide11-telemetry-isolation"}
    if batch_size is not None:
        request["batch_size"] = batch_size
    getattr(ws_conn, "send")(json.dumps(request, separators=(",", ":")))
    return telemetry_read_until(ws_conn, timeout_s=timeout_s, min_records=0, phase="subscribe_ack")


def telemetry_read_during_overlap(
    ws_conn: object,
    *,
    start_monotonic_s: float,
    worker: threading.Thread,
    timeout_s: float,
    baseline_sequence: int | None = None,
    baseline_device_time_us: int | None = None,
) -> dict[str, Any]:
    grace_records: list[dict[str, Any]] = []
    old_epoch_pause_records: list[dict[str, Any]] = []
    reset_epoch_records: list[dict[str, Any]] = []
    malformed_json = 0
    binary_frames = 0
    other_json = 0
    recv_timeouts = 0
    errors: list[str] = []
    reset_observed = False
    old_epoch_last_sequence = baseline_sequence
    old_epoch_last_device_time_us = baseline_device_time_us
    inferred_release_monotonic_s: float | None = None
    deadline = time.monotonic() + timeout_s
    getattr(ws_conn, "settimeout")(0.05)
    while worker.is_alive() and time.monotonic() < deadline:
        try:
            raw = getattr(ws_conn, "recv")()
        except Exception as exc:  # noqa: BLE001
            if is_websocket_recv_timeout(exc):
                recv_timeouts += 1
                continue
            errors.append(f"recv:{type(exc).__name__}:{exc}")
            break
        received = time.monotonic()
        if isinstance(raw, bytes | bytearray):
            binary_frames += 1
            continue
        if not isinstance(raw, str):
            other_json += 1
            continue
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError as exc:
            malformed_json += 1
            errors.append(f"json:{exc}")
            continue
        if not isinstance(parsed, Mapping):
            other_json += 1
            continue
        record = telemetry_record_from_message(parsed, received)
        if record is None:
            other_json += 1
            continue
        if reset_observed:
            reset_epoch_records.append(record)
            continue
        if telemetry_is_sequence_reset(
            record,
            baseline_sequence=baseline_sequence,
            old_epoch_last_sequence=old_epoch_last_sequence,
            old_epoch_last_device_time_us=old_epoch_last_device_time_us,
        ):
            reset_observed = True
            inferred_release_monotonic_s = received
            reset_epoch_records.append(record)
            continue
        sequence = telemetry_sequence_value(record)
        device_time_us = telemetry_device_time_value(record)
        if isinstance(sequence, int):
            old_epoch_last_sequence = sequence
        if isinstance(device_time_us, int):
            old_epoch_last_device_time_us = device_time_us
        if received - start_monotonic_s <= TELEMETRY_ISOLATION_GRACE_S:
            grace_records.append(record)
        else:
            old_epoch_pause_records.append(record)
    return {
        "phase": "overlap_pause",
        "connected": not errors,
        "grace_s": TELEMETRY_ISOLATION_GRACE_S,
        "pre_pause_delivery_grace_records": grace_records,
        "pre_pause_delivery_grace_count": len(grace_records),
        "old_epoch_pause_records": old_epoch_pause_records,
        "old_epoch_pause_record_count": len(old_epoch_pause_records),
        "pause_records": old_epoch_pause_records,
        "pause_record_count": len(old_epoch_pause_records),
        "reset_epoch_records": reset_epoch_records,
        "reset_epoch_record_count": len(reset_epoch_records),
        "sequence_reset_observed": reset_observed,
        "inferred_release_monotonic_s": inferred_release_monotonic_s,
        "expected_pause_observed": len(old_epoch_pause_records) == 0,
        "malformed_json": malformed_json,
        "binary_frames": binary_frames,
        "other_json": other_json,
        "recv_timeouts": recv_timeouts,
        "errors": errors,
        "elapsed_s": max(0.0, time.monotonic() - start_monotonic_s),
    }


def sigrok_tcp_wide11_telemetry_isolation_case(args: argparse.Namespace) -> dict[str, Any]:
    result: dict[str, Any] = {
        "case": "wide11_telemetry_isolation",
        "matrix": "wide11-telemetry-isolation",
        "telemetry_transport": "json_websocket",
        "capture_transport": "raw_tcp_sigrok",
        "capture_contract": {"mode": "WIDE11", "samplerate_khz": WIDE11_MAPPING_RATE_KHZ, "pre_samples": 0, "post_samples": WIDE11_MAPPING_POST_SAMPLES},
        "telemetry_rate_hz": args.telemetry_isolation_rate_hz,
        "baseline_required_samples": args.telemetry_isolation_baseline_samples,
        "post_release_required_samples": args.telemetry_isolation_post_release_samples,
        "pause_grace_s": TELEMETRY_ISOLATION_GRACE_S,
    }
    ws_conn: object | None = None
    burst_result_holder: dict[str, Any] = {}
    start_event = threading.Event()
    started = time.monotonic()
    try:
        websocket_module = load_websocket_module()
        ws_url = create_live_session_ws_url(args.http_base, args.timeout)
        result["telemetry_ws_url"] = ws_url
        ws_conn = getattr(websocket_module, "create_connection")(ws_url, timeout=args.timeout)
        result["subscribe"] = telemetry_subscribe(ws_conn, args.telemetry_isolation_rate_hz, None, args.timeout)
        baseline = telemetry_read_until(
            ws_conn,
            timeout_s=args.timeout,
            min_records=args.telemetry_isolation_baseline_samples,
            phase="baseline",
        )
        result["baseline"] = baseline
        baseline_records = cast(list[dict[str, Any]], baseline.get("records", []))
        baseline_last_sequence = telemetry_sequence_value(baseline_records[-1]) if baseline_records else None
        baseline_last_device_time_us = telemetry_device_time_value(baseline_records[-1]) if baseline_records else None
        result["baseline_last_sequence"] = baseline_last_sequence
        result["baseline_last_device_time_us"] = baseline_last_device_time_us

        def on_start_resp(raw_result: dict[str, Any]) -> None:
            result["arena_pause_start_monotonic_s"] = raw_result.get("start_resp_monotonic_s")
            start_event.set()

        def burst_worker() -> None:
            burst_result_holder["result"] = sigrok_tcp_wide11_deep_burst_raw_capture(
                args.tcp_host,
                args.tcp_port,
                args.timeout,
                on_start_resp=on_start_resp,
            )

        worker = threading.Thread(target=burst_worker, name="wide11-burst-isolation", daemon=True)
        worker.start()
        if not start_event.wait(args.timeout):
            result["overlap"] = {"connected": True, "expected_pause_observed": False, "reason": "raw TCP START_RESP not reached"}
        else:
            pause_start = result.get("arena_pause_start_monotonic_s")
            result["overlap"] = telemetry_read_during_overlap(
                ws_conn,
                start_monotonic_s=float(pause_start) if isinstance(pause_start, float) else time.monotonic(),
                worker=worker,
                timeout_s=args.timeout,
                baseline_sequence=baseline_last_sequence,
                baseline_device_time_us=baseline_last_device_time_us,
            )
        worker.join(args.timeout)
        if worker.is_alive():
            result["burst_join_timeout"] = True
        raw_burst = cast(dict[str, Any], burst_result_holder.get("result", {"ok": False, "reason": "raw burst worker did not finish"}))
        result["raw_tcp_wide11_burst"] = raw_burst
        result["raw_tcp_helper_return_monotonic_s"] = time.monotonic()
        result["arena_release_monotonic_s_upper_bound"] = result["raw_tcp_helper_return_monotonic_s"]
        overlap_result = cast(Mapping[str, Any], result.get("overlap", {}))
        if isinstance(overlap_result.get("inferred_release_monotonic_s"), float):
            result["inferred_release_monotonic_s"] = overlap_result.get("inferred_release_monotonic_s")
        post_release_after_helper = telemetry_read_until(
            ws_conn,
            timeout_s=args.timeout,
            min_records=args.telemetry_isolation_post_release_samples,
            phase="post_release",
        )
        result["post_release_after_helper_return"] = post_release_after_helper
        overlap_reset_records = cast(list[dict[str, Any]], overlap_result.get("reset_epoch_records", []))
        post_after_helper_records = cast(list[dict[str, Any]], post_release_after_helper.get("records", []))
        post_records = overlap_reset_records + post_after_helper_records
        result["post_release"] = {
            "phase": "post_release_evidence",
            "records": post_records,
            "record_count": len(post_records),
            "overlap_reset_epoch_record_count": len(overlap_reset_records),
            "after_helper_return_record_count": len(post_after_helper_records),
            "connected": bool(post_release_after_helper.get("connected", False)),
            "malformed_json": post_release_after_helper.get("malformed_json", 0),
            "binary_frames": post_release_after_helper.get("binary_frames", 0),
            "other_json": post_release_after_helper.get("other_json", 0),
            "recv_timeouts": post_release_after_helper.get("recv_timeouts", 0),
            "errors": post_release_after_helper.get("errors", []),
        }
        post_last_sequence = telemetry_sequence_value(post_records[-1]) if post_records else None
        post_last_device_time_us = telemetry_device_time_value(post_records[-1]) if post_records else None
        result["post_release_last_sequence"] = post_last_sequence
        result["post_release_last_device_time_us"] = post_last_device_time_us
        result["post_release_sequence_advanced"] = isinstance(baseline_last_sequence, int) and isinstance(post_last_sequence, int) and post_last_sequence > baseline_last_sequence
        result["post_release_sequence_reset_observed"] = isinstance(baseline_last_sequence, int) and isinstance(post_last_sequence, int) and post_last_sequence <= baseline_last_sequence
        result["post_release_device_time_advanced"] = isinstance(baseline_last_device_time_us, int) and isinstance(post_last_device_time_us, int) and post_last_device_time_us > baseline_last_device_time_us
        result["post_release_sequence_epoch"] = "continued" if bool(result["post_release_sequence_advanced"]) else "reset_or_reconstructed" if bool(result["post_release_sequence_reset_observed"]) else "unknown"
        result["post_release_valid_epoch"] = isinstance(post_last_sequence, int) and bool(result["post_release_device_time_advanced"])
        result["adc_http_health_after"] = adc_http_health(args.http_base, args.timeout)
    except Exception as exc:  # noqa: BLE001
        result.update({"pass": False, "reason": str(exc), "error": exception_detail(exc)})
    finally:
        if ws_conn is not None:
            try:
                getattr(ws_conn, "close")()
            except Exception:
                pass

    baseline_result = cast(Mapping[str, Any], result.get("baseline", {}))
    overlap_result = cast(Mapping[str, Any], result.get("overlap", {}))
    post_release_result = cast(Mapping[str, Any], result.get("post_release", {}))
    raw_result = cast(Mapping[str, Any], result.get("raw_tcp_wide11_burst", {}))
    adc_after = cast(Mapping[str, Any], result.get("adc_http_health_after", {}))
    checks = [
        (int(baseline_result.get("record_count", 0) or 0) >= args.telemetry_isolation_baseline_samples, "baseline telemetry not established"),
        (bool(overlap_result.get("connected", False)), "telemetry WS disconnected or errored during overlap"),
        (int(overlap_result.get("malformed_json", 0) or 0) == 0, "malformed JSON received on telemetry WS"),
        (int(overlap_result.get("binary_frames", 0) or 0) == 0, "binary contamination received on telemetry WS"),
        (bool(overlap_result.get("expected_pause_observed", False)), "telemetry was emitted during arena pause/lease interval"),
        (bool(raw_result.get("ok", False)), str(raw_result.get("reason", "raw TCP WIDE11 burst failed"))),
        (int(cast(Mapping[str, Any], raw_result.get("stats", {})).get("received_sample_count", 0) or 0) >= WIDE11_MAPPING_POST_SAMPLES, "raw TCP WIDE11 did not receive 100000 samples"),
        (int(cast(Mapping[str, Any], raw_result.get("stats", {})).get("data_frames", 0) or 0) == WIDE11_BURST_EXPECTED_DATA_FRAMES, "raw TCP WIDE11 did not receive 98 DATA frames"),
        (bool(post_release_result.get("connected", False)), "telemetry WS disconnected or errored during post-release collection"),
        (int(post_release_result.get("malformed_json", 0) or 0) == 0, "malformed JSON received on telemetry WS after release"),
        (int(post_release_result.get("binary_frames", 0) or 0) == 0, "binary contamination received on telemetry WS after release"),
        (int(post_release_result.get("record_count", 0) or 0) >= args.telemetry_isolation_post_release_samples, "fresh telemetry did not resume after release"),
        (bool(result.get("post_release_valid_epoch", False)), "post-release telemetry did not provide a valid resumed epoch with advancing device time"),
        (bool(adc_after.get("ok", False)), "ADC HTTP health failed after release"),
    ]
    passed = True
    reason = "ok"
    for check_passed, check_reason in checks:
        if not check_passed:
            passed = False
            reason = check_reason
            break
    result.update({"pass": passed, "reason": reason, "elapsed_s": time.monotonic() - started})
    return result


def ws_send_frame(ws_conn: object, websocket_module: object, frame_type: int, payload: bytes = b"", frame_id: int = 1) -> None:
    abnf = getattr(websocket_module, "ABNF")
    opcode_binary = getattr(abnf, "OPCODE_BINARY")
    getattr(ws_conn, "send")(build_frame(frame_type, payload, frame_id), opcode=opcode_binary)


def ws_take_buffered_frame(ws_conn: object) -> tuple[SigrokHeader, bytes] | None:
    data = getattr(ws_conn, "_linkr_sigrok_pending", b"")
    if not isinstance(data, bytes) or len(data) < SIGROK_HEADER_BYTES:
        return None

    header = parse_header(data[:SIGROK_HEADER_BYTES])
    frame_len = SIGROK_HEADER_BYTES + header.payload_len
    if len(data) < frame_len:
        return None

    payload = data[SIGROK_HEADER_BYTES:frame_len]
    setattr(ws_conn, "_linkr_sigrok_pending", data[frame_len:])
    return header, payload


def ws_recv_frame(ws_conn: object, timeout_s: float) -> tuple[SigrokHeader, bytes]:
    getattr(ws_conn, "settimeout")(timeout_s)
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        buffered = ws_take_buffered_frame(ws_conn)
        if buffered is not None:
            return buffered

        data = getattr(ws_conn, "recv")()
        if isinstance(data, str):
            continue
        if isinstance(data, bytearray):
            data = bytes(data)
        if not isinstance(data, bytes):
            continue
        buffered_data = getattr(ws_conn, "_linkr_sigrok_pending", b"")
        if not isinstance(buffered_data, bytes):
            buffered_data = b""
        setattr(ws_conn, "_linkr_sigrok_pending", buffered_data + data)
    raise TimeoutError("timeout waiting for WebSocket sigrok frame")


def ws_send_request(ws_conn: object, websocket_module: object, frame_type: int, payload: bytes, frame_id: int, timeout_s: float) -> tuple[SigrokHeader, bytes]:
    ws_send_frame(ws_conn, websocket_module, frame_type, payload, frame_id)
    return ws_recv_frame(ws_conn, timeout_s)


def ws_send_request_wait_response(
    ws_conn: object,
    websocket_module: object,
    request_type: int,
    payload: bytes,
    frame_id: int,
    expected_response_type: int,
    timeout_s: float,
) -> tuple[SigrokHeader, bytes, dict[str, int]]:
    ws_send_frame(ws_conn, websocket_module, request_type, payload, frame_id)
    deadline = time.monotonic() + timeout_s
    diagnostics = {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}
    while time.monotonic() < deadline:
        header, response_payload = ws_recv_frame(ws_conn, max(0.05, deadline - time.monotonic()))
        if header.frame_type == expected_response_type and header.frame_id == frame_id:
            return header, response_payload, diagnostics
        if header.frame_type == FRAME_ERROR:
            return header, response_payload, diagnostics
        if header.frame_type == FRAME_DATA:
            diagnostics["skipped_data_frames"] += 1
        elif header.frame_type == FRAME_EVENT:
            diagnostics["skipped_events"] += 1
        else:
            diagnostics["skipped_other_frames"] += 1
    raise TimeoutError(f"timeout waiting for WebSocket response 0x{expected_response_type:02x} id {frame_id}")


def ws_sigrok_stop(ws_conn: object, websocket_module: object, timeout_s: float, frame_id: int) -> dict[str, Any]:
    try:
        ws_send_frame(ws_conn, websocket_module, FRAME_STOP_REQ, b"", frame_id)
        deadline = time.monotonic() + timeout_s
        skipped_data_frames = 0
        skipped_events = 0
        while time.monotonic() < deadline:
            header, payload = ws_recv_frame(ws_conn, max(0.05, deadline - time.monotonic()))
            if header.frame_type == FRAME_STOP_RESP:
                return {"received": True, "sent": True, "frame_type": header.frame_type, "ack": parse_ack(payload), "skipped_data_frames": skipped_data_frames, "skipped_events": skipped_events}
            if header.frame_type == FRAME_ERROR:
                return {"received": True, "sent": True, "frame_type": header.frame_type, "error": parse_error(payload), "skipped_data_frames": skipped_data_frames, "skipped_events": skipped_events}
            if header.frame_type == FRAME_DATA:
                skipped_data_frames += 1
            elif header.frame_type == FRAME_EVENT:
                skipped_events += 1
        return {"received": False, "sent": True, "reason": "timeout waiting for STOP_RESP", "skipped_data_frames": skipped_data_frames, "skipped_events": skipped_events}
    except Exception as exc:  # noqa: BLE001
        return {"received": False, "sent": False, "reason": str(exc)}


def ws_sigrok_immediate_restart(ws_conn: object, websocket_module: object, mode_id: int, channel_mask: int, samplerate_khz: int, timeout_s: float) -> dict[str, Any]:
    try:
        config = build_config_payload(mode_id, channel_mask, samplerate_khz, 0, 1)
        header, payload, config_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_CONFIG_REQ, config, 200, FRAME_CONFIG_RESP, timeout_s)
        if header.frame_type != FRAME_CONFIG_RESP:
            error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
            return {"ok": False, "reason": f"restart CONFIG got 0x{header.frame_type:02x}", "error": error, "config_wait": config_wait}
        config_ack = parse_ack(payload)
        header, payload, start_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_START_REQ, b"", 201, FRAME_START_RESP, timeout_s)
        if header.frame_type != FRAME_START_RESP:
            error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
            return {"ok": False, "reason": f"restart START got 0x{header.frame_type:02x}", "config_ack": config_ack, "error": error, "config_wait": config_wait, "start_wait": start_wait}
        start_ack = parse_ack(payload)
        start_wait_reason = strict_start_wait_failure_reason(start_wait)
        if start_wait_reason is not None:
            stop = ws_sigrok_stop(ws_conn, websocket_module, timeout_s, 202)
            return {"ok": False, "reason": start_wait_reason, "config_ack": config_ack, "start_ack": start_ack, "config_wait": config_wait, "start_wait": start_wait, "stop_response": stop}
        stop = ws_sigrok_stop(ws_conn, websocket_module, timeout_s, 202)
        return {"ok": bool(stop.get("received")), "config_ack": config_ack, "start_ack": start_ack, "stop_response": stop, "config_wait": config_wait, "start_wait": start_wait}
    except Exception as exc:  # noqa: BLE001
        return {"ok": False, "reason": str(exc)}


def ws_sigrok_fresh_restart_probe(http_base: str, websocket_module: object, mode_id: int, channel_mask: int, samplerate_khz: int, timeout_s: float) -> dict[str, Any]:
    stats = StreamStats()
    ws_conn: object | None = None
    try:
        ws_url = create_live_session_ws_url(http_base, timeout_s)
        ws_conn = getattr(websocket_module, "create_connection")(ws_url, timeout=timeout_s)
        header, payload, hello_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_HELLO_REQ, b"", 1, FRAME_HELLO_RESP, timeout_s)
        if header.frame_type != FRAME_HELLO_RESP:
            error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
            return {"ok": False, "reason": f"fresh HELLO got 0x{header.frame_type:02x}", "error": error, "transport": "websocket", "fresh_session": True, "ws_url": ws_url, "hello_wait": hello_wait}
        hello_payload_bytes = len(payload)
        header, payload, caps_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_CAPS_REQ, b"", 2, FRAME_CAPS_RESP, timeout_s)
        if header.frame_type != FRAME_CAPS_RESP:
            error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
            return {"ok": False, "reason": f"fresh CAPS got 0x{header.frame_type:02x}", "error": error, "transport": "websocket", "fresh_session": True, "ws_url": ws_url, "hello_wait": hello_wait, "caps_wait": caps_wait}
        handshake = {"hello_payload_bytes": hello_payload_bytes, "caps_payload_bytes": len(payload), "hello_wait": hello_wait, "caps_wait": caps_wait}
        config = build_config_payload(mode_id, channel_mask, samplerate_khz, 0, 1)
        header, payload, config_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_CONFIG_REQ, config, 200, FRAME_CONFIG_RESP, timeout_s)
        if header.frame_type != FRAME_CONFIG_RESP:
            error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
            return {"ok": False, "reason": f"fresh CONFIG got 0x{header.frame_type:02x}", "error": error, "transport": "websocket", "fresh_session": True, "ws_url": ws_url, "handshake": handshake, "config_wait": config_wait}
        config_ack = parse_ack(payload)
        header, payload, start_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_START_REQ, b"", 201, FRAME_START_RESP, timeout_s)
        if header.frame_type != FRAME_START_RESP:
            error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
            return {"ok": False, "reason": f"fresh START got 0x{header.frame_type:02x}", "error": error, "transport": "websocket", "fresh_session": True, "ws_url": ws_url, "handshake": handshake, "config_ack": config_ack, "config_wait": config_wait, "start_wait": start_wait}
        start_ack = parse_ack(payload)
        start_wait_reason = strict_start_wait_failure_reason(start_wait)
        if start_wait_reason is not None:
            stop = ws_sigrok_stop(ws_conn, websocket_module, timeout_s, 202)
            return {"ok": False, "reason": start_wait_reason, "error": None, "transport": "websocket", "fresh_session": True, "ws_url": ws_url, "handshake": handshake, "config_ack": config_ack, "start_ack": start_ack, "config_wait": config_wait, "start_wait": start_wait, "stop_response": stop, "stats": asdict(stats)}
        deadline = time.monotonic() + timeout_s
        while stats.data_frames == 0 and time.monotonic() < deadline:
            try:
                data_header, data_payload = ws_recv_frame(ws_conn, max(0.05, deadline - time.monotonic()))
            except TimeoutError:
                break
            if data_header.frame_type == FRAME_DATA:
                meta = parse_data_meta(data_payload)
                observe_sigrok_data_payload(stats, meta, data_payload)
            elif data_header.frame_type == FRAME_ERROR:
                return {"ok": False, "reason": "fresh DATA wait returned ERROR", "error": parse_error(data_payload), "transport": "websocket", "fresh_session": True, "ws_url": ws_url, "handshake": handshake, "config_ack": config_ack, "start_ack": start_ack, "config_wait": config_wait, "start_wait": start_wait, "stats": asdict(stats)}
            elif data_header.frame_type == FRAME_EVENT:
                _ = stats.observe_event(data_payload)
        stop = ws_sigrok_stop(ws_conn, websocket_module, timeout_s, 202)
        ok = stats.data_frames > 0 and bool(stop.get("received")) and stop.get("frame_type") == FRAME_STOP_RESP
        reason = "ok" if ok else "fresh restart did not receive DATA before STOP" if stats.data_frames == 0 else "fresh restart STOP_RESP not received"
        return {"ok": ok, "reason": reason, "transport": "websocket", "fresh_session": True, "ws_url": ws_url, "handshake": handshake, "config_ack": config_ack, "start_ack": start_ack, "stop_response": stop, "config_wait": config_wait, "start_wait": start_wait, "stats": asdict(stats)}
    except Exception as exc:  # noqa: BLE001
        return {"ok": False, "reason": str(exc), "transport": "websocket", "fresh_session": True}
    finally:
        if ws_conn is not None:
            try:
                getattr(ws_conn, "close")()
            except Exception:
                pass


def evaluate_sigrok_pass(
    *,
    bounded: bool,
    trigger_required: bool,
    stats: StreamStats,
    requested_samples: int | None,
    requested_duration_s: float | None,
    stream_elapsed_s: float,
    stop_response: Mapping[str, object],
    immediate_restart: Mapping[str, object],
    board_health_after: Mapping[str, object],
    terminal_reason: str | None = None,
) -> tuple[bool, str]:
    requested_met = True if requested_samples is None else stats.received_sample_count >= requested_samples
    duration_met = True if bounded else requested_duration_s is not None and stream_elapsed_s >= requested_duration_s * 0.95
    terminal_accepts_completion = terminal_reason in (TERMINAL_REASON_SERVER_STOPPED, TERMINAL_REASON_SERVER_OVERRUN)
    data_observed_or_capacity_stop = stats.data_frames > 0 or (
        not bounded and terminal_reason == TERMINAL_REASON_SERVER_OVERRUN
    )
    if terminal_reason == TERMINAL_REASON_SERVER_ERROR:
        return False, "server terminal error received"
    if bounded and terminal_accepts_completion and not requested_met:
        return False, "requested pre+post sample count not met"
    if not bounded and terminal_accepts_completion:
        duration_met = True
    checks = [
        (not trigger_required or stats.triggered_events > 0, "trigger event not received"),
        (not trigger_required or stats.trigger_sample_offset_valid(), "trigger sample offset invalid"),
        (data_observed_or_capacity_stop, "no DATA frames received"),
        (stats.invalid_sample_count_frames == 0, "invalid DATA sample_count detected"),
        (stats.payload_over_budget_frames == 0, "DATA payload exceeds sample-count budget"),
        (stats.channel_mask_mismatch_frames == 0, "DATA channel mask mismatch detected"),
        (stats.data_decode_error_frames == 0, "DATA payload decode errors detected"),
        (stats.sample_index_gaps == 0, "sample_index gaps detected"),
        (stats.disconnects == 0, "disconnects detected"),
        (terminal_accepts_completion or (bool(stop_response.get("received")) and stop_response.get("frame_type") == FRAME_STOP_RESP), "STOP_RESP not received"),
        (bool(immediate_restart.get("ok")), "immediate restart failed"),
        (bool(board_health_after.get("ok")), "board HTTP health is not ok"),
        (requested_met, "requested pre+post sample count not met"),
        (duration_met, "continuous data window duration not met"),
    ]
    for passed, reason in checks:
        if not passed:
            return False, reason
    return True, "ok"


def sigrok_capture_case(
    host: str,
    port: int,
    http_base: str,
    mode_name: str,
    samplerate_khz: int,
    pre_samples: int,
    post_samples: int,
    duration_s: float | None,
    timeout_s: float,
    trigger_type: int = TRIGGER_NONE,
    trigger_channel: int = 0,
    uart_args: argparse.Namespace | None = None,
) -> dict[str, Any]:
    mode = MODE_CASES[mode_name]
    bounded = post_samples != 0
    trigger_required = bounded and trigger_type != TRIGGER_NONE
    stats = StreamStats()
    result: dict[str, Any] = {
        "case": "sigrok_tcp_triggered_bounded_samples" if trigger_required else "sigrok_tcp_bounded_samples" if bounded else "sigrok_tcp_continuous_duration",
        "capture_semantics": "triggered_bounded_capture" if trigger_required else "bounded_capture" if bounded else "continuous_rate_diagnostics",
        "mode": mode_name,
        "mode_id": mode.mode_id,
        "channel_mask": mode.mask,
        "sample_bytes": mode.sample_bytes,
        "requested_samplerate_khz": samplerate_khz,
        "sigrok_pre_samples": pre_samples,
        "sigrok_post_samples": post_samples,
        "sigrok_post_zero_unlimited": post_samples == 0,
        "trigger_type": trigger_type,
        "trigger_name": TRIGGER_NAMES[trigger_type],
        "trigger_channel": trigger_channel,
        "trigger_required": trigger_required,
        "active_stream_chunk_samples": STREAM_CHUNK_SAMPLES,
        "duration_s": duration_s,
        "transport_note": "Effective receive rate is host transport throughput, not hardware samplerate.",
    }
    started = time.monotonic()
    stop_response: dict[str, Any] = {"received": False, "reason": "not sent"}
    immediate_restart: dict[str, Any] = {"ok": False, "reason": "not attempted"}
    stream_started = 0.0
    stream_ended = 0.0
    terminal_reason: str | None = None
    try:
        with socket.create_connection((host, port), timeout=timeout_s) as sock:
            sock.settimeout(timeout_s)
            result["handshake"] = sigrok_handshake(sock, timeout_s)
            config_frame_type, config_payload, config_encoding = build_config_request(
                mode.mode_id,
                mode.mask,
                samplerate_khz,
                pre_samples,
                post_samples,
                trigger_type,
                trigger_channel,
                handshake_supports_config_v2(cast(Mapping[str, object], result["handshake"])),
            )
            result["config_encoding"] = config_encoding
            result["config_frame_type"] = config_frame_type
            header, payload, config_wait = sigrok_send_request_wait_response(sock, config_frame_type, config_payload, 3, FRAME_CONFIG_RESP, timeout_s)
            result["config_wait"] = config_wait
            if header.frame_type == FRAME_ERROR:
                result.update({"pass": False, "reason": "CONFIG returned ERROR", "error": parse_error(payload)})
                return result
            if header.frame_type != FRAME_CONFIG_RESP:
                result.update({"pass": False, "reason": f"CONFIG returned 0x{header.frame_type:02x}"})
                return result
            result["config_ack"] = parse_ack(payload)
            header, payload, start_wait = sigrok_send_request_wait_response(sock, FRAME_START_REQ, b"", 4, FRAME_START_RESP, timeout_s)
            result["start_wait"] = start_wait
            if header.frame_type != FRAME_START_RESP:
                error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
                result.update({"pass": False, "reason": f"START returned 0x{header.frame_type:02x}", "error": error})
                return result
            result["start_ack"] = parse_ack(payload)
            start_wait_reason = strict_start_wait_failure_reason(start_wait)
            if start_wait_reason is not None:
                stop_response = sigrok_stop(sock, timeout_s, 5)
                result.update({"pass": False, "reason": start_wait_reason, "stop_response": stop_response, "client_stop_sent": bool(stop_response.get("sent")), "client_stopped": bool(stop_response.get("received")) and stop_response.get("frame_type") == FRAME_STOP_RESP})
                return result
            if trigger_required and uart_args is not None and uart_args.uart_stimulus is not None:
                result["uart_stimulus"] = perform_uart_stimulus(uart_args)

            target_samples = pre_samples + post_samples if bounded else None
            stream_started = time.monotonic()
            read_deadline = stream_started + (duration_s if duration_s is not None else timeout_s)
            while True:
                if bounded and target_samples is not None and stats.received_sample_count >= target_samples and (not trigger_required or stats.triggered_events > 0):
                    break
                if time.monotonic() >= read_deadline:
                    break
                per_frame_timeout = max(0.05, min(timeout_s, read_deadline - time.monotonic()))
                try:
                    frame_header, frame_payload = recv_frame(sock, per_frame_timeout)
                except TimeoutError:
                    break
                except ConnectionError as exc:
                    stats.disconnects += 1
                    result.setdefault("stream_disconnect_error", exception_detail(exc))
                    break
                if frame_header.frame_type == FRAME_DATA:
                    meta = parse_data_meta(frame_payload)
                    observe_sigrok_data_payload(
                        stats,
                        meta,
                        frame_payload,
                        expected_channel_mask=mode.mask,
                        sample_bytes=mode.sample_bytes,
                    )
                elif frame_header.frame_type == FRAME_ERROR:
                    result["stream_error"] = parse_error(frame_payload)
                    terminal_reason = TERMINAL_REASON_SERVER_ERROR
                    break
                elif frame_header.frame_type == FRAME_EVENT:
                    event = stats.observe_event(frame_payload)
                    result.setdefault("events", 0)
                    result["events"] += 1
                    terminal_reason = stats.terminal_reason_for_event(event)
                    if terminal_reason is not None:
                        result["terminal_reason"] = terminal_reason
                        result["terminal_event"] = asdict(event)
                        break

            stream_ended = time.monotonic()
            if terminal_reason is None:
                stop_response = sigrok_stop(sock, timeout_s, 5)
        immediate_restart = sigrok_fresh_restart_probe(host, port, mode.mode_id, mode.mask, samplerate_khz, timeout_s)
    except Exception as exc:  # noqa: BLE001
        stats.disconnects += 1
        result.update({"pass": False, "reason": str(exc)})
        result.setdefault("stream_disconnect_error", exception_detail(exc))

    elapsed = time.monotonic() - started
    stream_elapsed = max(0.0, stream_ended - stream_started)
    requested_samples = pre_samples + post_samples if bounded else None
    requested_pre_post_met = True if requested_samples is None else stats.received_sample_count >= requested_samples
    board_health_after = board_health(http_base, timeout_s)
    passed, reason = evaluate_sigrok_pass(
        bounded=bounded,
        trigger_required=trigger_required,
        stats=stats,
        requested_samples=requested_samples,
        requested_duration_s=duration_s,
        stream_elapsed_s=stream_elapsed,
        stop_response=stop_response,
        immediate_restart=immediate_restart,
        board_health_after=board_health_after,
        terminal_reason=terminal_reason,
    )
    result.update({
        "pass": passed,
        "reason": reason,
        "elapsed_s": elapsed,
        "stream_elapsed_s": stream_elapsed,
        "stats": {**asdict(stats), **stats.rates(stream_elapsed)},
        "trigger_event_received": stats.triggered_events > 0,
        "trigger_sample_index": stats.trigger_sample_index,
        "trigger_sample_offset_samples": stats.trigger_sample_offset(),
        "trigger_sample_offset_valid": stats.trigger_sample_offset_valid(),
        "requested_pre_post_samples": requested_samples,
        "requested_pre_post_met": requested_pre_post_met,
        "bounded_target_met_before_stop": bounded and requested_pre_post_met,
        "client_stop_sent": bool(stop_response.get("sent")),
        "client_stopped": bool(stop_response.get("received")) and stop_response.get("frame_type") == FRAME_STOP_RESP,
        "server_auto_stopped": terminal_reason in (TERMINAL_REASON_SERVER_STOPPED, TERMINAL_REASON_SERVER_OVERRUN) and not bool(stop_response.get("sent")),
        "terminal_reason": terminal_reason,
        "continuous_data_observed": not bounded and stats.data_frames > 0,
        "capacity_stop_before_data": not bounded and terminal_reason == TERMINAL_REASON_SERVER_OVERRUN and stats.data_frames == 0,
        "stop_response": stop_response,
        "immediate_restart": immediate_restart,
        "board_http_health_after": board_health_after,
    })
    return result


def sigrok_tcp_wide11_mapping_case(args: argparse.Namespace) -> dict[str, Any]:
    mode = MODE_CASES["WIDE11"]
    stats = StreamStats()
    decoded_payload = bytearray()
    stimulus_profile = "gp10_uart_low_others" if args.wide11_map_gp10_uart_low_others else "external_4bit"
    result: dict[str, Any] = {
        "case": "sigrok_tcp_wide11_gp10_uart_low_others" if stimulus_profile == "gp10_uart_low_others" else "sigrok_tcp_wide11_deep_burst_mapping",
        "transport": "tcp",
        "mode": "WIDE11",
        "mode_id": mode.mode_id,
        "channel_mask": mode.mask,
        "sample_bytes": mode.sample_bytes,
        "requested_samplerate_khz": WIDE11_MAPPING_RATE_KHZ,
        "sigrok_pre_samples": WIDE11_MAPPING_PRE_SAMPLES,
        "sigrok_post_samples": WIDE11_MAPPING_POST_SAMPLES,
        "trigger_type": WIDE11_MAPPING_TRIGGER_TYPE,
        "trigger_name": TRIGGER_NAMES[WIDE11_MAPPING_TRIGGER_TYPE],
        "trigger_channel": WIDE11_MAPPING_TRIGGER_CHANNEL,
        "config_must_be_v2": True,
        "observed_mapping": {"GP10": 0, "GP11": 1, "GP18": 8, "GP20": 10},
        "stimulus_profile": stimulus_profile,
        "mapping_profile": stimulus_profile,
        "validation_scope": "single_channel_reduced" if stimulus_profile == "gp10_uart_low_others" else "multi_channel_external_4bit",
        "selected_external_4bit_mapping_validated": False,
        "stimulus_method": "uart_tx_to_gp10" if stimulus_profile == "gp10_uart_low_others" else "external_pattern_generator",
        "physical_prerequisite": WIDE11_MAPPING_GP10_UART_PREREQUISITE if stimulus_profile == "gp10_uart_low_others" else WIDE11_MAPPING_PHYSICAL_PREREQUISITE,
        "external_generator_acknowledged": bool(args.wide11_map_external_generator),
        "gp10_uart_low_others_acknowledged": bool(args.wide11_map_gp10_uart_low_others),
        "expected_pattern_nibbles": args.wide11_map_pattern,
        "expected_hold_samples": args.wide11_map_hold_samples,
        "check_samples": args.wide11_map_check_samples,
        "zero_mask": WIDE11_MAPPING_LOW_OTHERS_ZERO_MASK if stimulus_profile == "gp10_uart_low_others" else None,
        "validation_limitations": [] if stimulus_profile == "external_4bit" else [
            "Independent high-state mapping for GP11-GP20 is not validated by this reduced single-wire test",
            "Selected external mapping coverage for GP11/GP18/GP20 is not exercised when only GP10 is driven",
        ],
    }
    if stimulus_profile == "external_4bit" and not args.wide11_map_external_generator:
        result.update({"pass": False, "blocked": True, "reason": "WIDE11 mapping HIL requires --wide11-map-external-generator and the documented physical wiring"})
        return result

    started = time.monotonic()
    stop_response: dict[str, Any] = {"received": False, "reason": "not sent"}
    try:
        with socket.create_connection((args.tcp_host, args.tcp_port), timeout=args.timeout) as sock:
            sock.settimeout(args.timeout)
            result["handshake"] = sigrok_handshake(sock, args.timeout)
            config_frame_type, config_payload, config_encoding = build_config_request(
                mode.mode_id,
                mode.mask,
                WIDE11_MAPPING_RATE_KHZ,
                WIDE11_MAPPING_PRE_SAMPLES,
                WIDE11_MAPPING_POST_SAMPLES,
                WIDE11_MAPPING_TRIGGER_TYPE,
                WIDE11_MAPPING_TRIGGER_CHANNEL,
                handshake_supports_config_v2(cast(Mapping[str, object], result["handshake"])),
            )
            result["config_encoding"] = config_encoding
            result["config_frame_type"] = config_frame_type
            if config_frame_type != FRAME_CONFIG_V2_REQ or config_encoding != "v2":
                result.update({"pass": False, "reason": "WIDE11 mapping case did not use CONFIG_V2"})
                return result
            header, payload, config_wait = sigrok_send_request_wait_response(sock, config_frame_type, config_payload, 3, FRAME_CONFIG_RESP, args.timeout)
            result["config_wait"] = config_wait
            if header.frame_type == FRAME_ERROR:
                result.update({"pass": False, "reason": "CONFIG returned ERROR", "error": parse_error(payload)})
                return result
            if header.frame_type != FRAME_CONFIG_RESP:
                result.update({"pass": False, "reason": f"CONFIG returned 0x{header.frame_type:02x}"})
                return result
            result["config_ack"] = parse_ack(payload)
            header, payload, start_wait = sigrok_send_request_wait_response(sock, FRAME_START_REQ, b"", 4, FRAME_START_RESP, args.timeout)
            result["start_wait"] = start_wait
            if header.frame_type != FRAME_START_RESP:
                error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
                result.update({"pass": False, "reason": f"START returned 0x{header.frame_type:02x}", "error": error})
                return result
            result["start_ack"] = parse_ack(payload)
            start_wait_reason = strict_start_wait_failure_reason(start_wait)
            if start_wait_reason is not None:
                stop_response = sigrok_stop(sock, args.timeout, 5)
                result.update({"pass": False, "reason": start_wait_reason, "stop_response": stop_response})
                return result
            if stimulus_profile == "gp10_uart_low_others":
                result["uart_stimulus"] = perform_uart_stimulus(args)

            target_samples = WIDE11_MAPPING_PRE_SAMPLES + WIDE11_MAPPING_POST_SAMPLES
            read_deadline = time.monotonic() + args.timeout
            while stats.received_sample_count < target_samples and time.monotonic() < read_deadline:
                try:
                    frame_header, frame_payload = recv_frame(sock, max(0.05, min(args.timeout, read_deadline - time.monotonic())))
                except TimeoutError:
                    break
                except ConnectionError as exc:
                    stats.disconnects += 1
                    result.setdefault("stream_disconnect_error", exception_detail(exc))
                    break
                if frame_header.frame_type == FRAME_DATA:
                    meta = parse_data_meta(frame_payload)
                    sample_payload = frame_payload[SIGROK_DATA_META_BYTES:]
                    try:
                        decoded_payload.extend(decode_sigrok_data_payload(meta, sample_payload))
                    except ValueError as exc:
                        stats.observe_decode_error(meta, exc)
                    stats.observe_data(meta, len(sample_payload), expected_channel_mask=mode.mask, sample_bytes=mode.sample_bytes)
                elif frame_header.frame_type == FRAME_ERROR:
                    result["stream_error"] = parse_error(frame_payload)
                    break
                elif frame_header.frame_type == FRAME_EVENT:
                    _ = stats.observe_event(frame_payload)
                    result.setdefault("events", 0)
                    result["events"] += 1
            stop_response = sigrok_stop(sock, args.timeout, 5)
    except Exception as exc:  # noqa: BLE001
        stats.disconnects += 1
        result.update({"pass": False, "reason": str(exc)})
        result.setdefault("stream_disconnect_error", exception_detail(exc))

    if decoded_payload and stimulus_profile == "gp10_uart_low_others":
        mapping_validation = validate_wide11_gp10_uart_low_others_payload(
            bytes(decoded_payload),
            check_samples=args.wide11_map_check_samples,
        )
    elif decoded_payload:
        mapping_validation = validate_wide11_mapping_payload(
            bytes(decoded_payload),
            pattern_nibbles=args.wide11_map_pattern,
            hold_samples=args.wide11_map_hold_samples,
            check_samples=args.wide11_map_check_samples,
        )
    else:
        mapping_validation = {"pass": False, "reason": "no decoded WIDE11 DATA payload"}
    board_health_after = board_health(args.http_base, args.timeout)
    checks = [
        (stats.triggered_events > 0, "trigger event not received"),
        (stats.invalid_sample_count_frames == 0, "invalid DATA sample_count detected"),
        (stats.payload_over_budget_frames == 0, "DATA payload exceeds sample-count budget"),
        (stats.channel_mask_mismatch_frames == 0, "DATA channel mask mismatch detected"),
        (stats.data_decode_error_frames == 0, "DATA payload decode errors detected"),
        (stats.sample_index_gaps == 0, "sample_index gaps detected"),
        (stats.disconnects == 0, "disconnects detected"),
        (stats.received_sample_count >= WIDE11_MAPPING_POST_SAMPLES, "exact WIDE11 deep-burst sample count not received"),
        (bool(stop_response.get("received")) and stop_response.get("frame_type") == FRAME_STOP_RESP, "STOP_RESP not received; pin cleanup/release not confirmed"),
        (bool(board_health_after.get("ok")), "board HTTP health is not ok after cleanup"),
        (bool(mapping_validation.get("pass")), str(mapping_validation.get("reason"))),
    ]
    passed = True
    reason = "ok"
    for check_passed, check_reason in checks:
        if not check_passed:
            passed = False
            reason = check_reason
            break
    result.update({
        "pass": passed,
        "reason": reason,
        "elapsed_s": time.monotonic() - started,
        "stats": asdict(stats),
        "decoded_payload_bytes": len(decoded_payload),
        "mapping_validation": mapping_validation,
        "selected_external_4bit_mapping_validated": stimulus_profile == "external_4bit" and bool(mapping_validation.get("pass")),
        "gp10_bit0_activity_validated": bool(mapping_validation.get("pass")) if stimulus_profile == "gp10_uart_low_others" else None,
        "low_others_zero_mask_validated": bool(mapping_validation.get("pass")) if stimulus_profile == "gp10_uart_low_others" else None,
        "selected_gp11_gp18_gp20_high_mapping_validated": stimulus_profile == "external_4bit" and bool(mapping_validation.get("pass")),
        "trigger_event_received": stats.triggered_events > 0,
        "stop_response": stop_response,
        "board_http_health_after": board_health_after,
        "pin_state_restore": "STOP_REQ sent and STOP_RESP required; firmware cleanup restores GP10-GP20 to SIO GPIO_INPUT",
    })
    return result


def sigrok_ws_dependency_error(
    bounded: bool,
    mode_name: str,
    samplerate_khz: int,
    pre_samples: int,
    post_samples: int,
    duration_s: float | None,
    reason: str,
    trigger_type: int = TRIGGER_NONE,
    trigger_channel: int = 0,
) -> dict[str, Any]:
    trigger_required = bounded and trigger_type != TRIGGER_NONE
    return {
        "case": "sigrok_ws_triggered_bounded_samples" if trigger_required else "sigrok_ws_bounded_samples" if bounded else "sigrok_ws_continuous_duration",
        "transport": "websocket",
        "mode": mode_name,
        "requested_samplerate_khz": samplerate_khz,
        "sigrok_pre_samples": pre_samples,
        "sigrok_post_samples": post_samples,
        "trigger_type": trigger_type,
        "trigger_name": TRIGGER_NAMES[trigger_type],
        "trigger_channel": trigger_channel,
        "trigger_required": trigger_required,
        "duration_s": duration_s,
        "pass": False,
        "dependency_error": True,
        "reason": reason,
    }


def sigrok_ws_capture_case(
    http_base: str,
    mode_name: str,
    samplerate_khz: int,
    pre_samples: int,
    post_samples: int,
    duration_s: float | None,
    timeout_s: float,
    trigger_type: int = TRIGGER_NONE,
    trigger_channel: int = 0,
    uart_args: argparse.Namespace | None = None,
) -> dict[str, Any]:
    mode = MODE_CASES[mode_name]
    bounded = post_samples != 0
    trigger_required = bounded and trigger_type != TRIGGER_NONE
    stats = StreamStats()
    result: dict[str, Any] = {
        "case": "sigrok_ws_triggered_bounded_samples" if trigger_required else "sigrok_ws_bounded_samples" if bounded else "sigrok_ws_continuous_duration",
        "capture_semantics": "triggered_bounded_capture" if trigger_required else "bounded_capture" if bounded else "continuous_rate_diagnostics",
        "transport": "websocket",
        "mode": mode_name,
        "mode_id": mode.mode_id,
        "channel_mask": mode.mask,
        "sample_bytes": mode.sample_bytes,
        "requested_samplerate_khz": samplerate_khz,
        "sigrok_pre_samples": pre_samples,
        "sigrok_post_samples": post_samples,
        "sigrok_post_zero_unlimited": post_samples == 0,
        "trigger_type": trigger_type,
        "trigger_name": TRIGGER_NAMES[trigger_type],
        "trigger_channel": trigger_channel,
        "trigger_required": trigger_required,
        "active_stream_chunk_samples": STREAM_CHUNK_SAMPLES,
        "duration_s": duration_s,
        "transport_note": "Effective receive rate is host transport throughput, not hardware samplerate.",
    }
    try:
        websocket_module = load_websocket_module()
    except RuntimeError as exc:
        return sigrok_ws_dependency_error(bounded, mode_name, samplerate_khz, pre_samples, post_samples, duration_s, str(exc), trigger_type, trigger_channel)
    started = time.monotonic()
    stop_response: dict[str, Any] = {"received": False, "sent": False, "reason": "not sent"}
    immediate_restart: dict[str, Any] = {"ok": False, "reason": "not attempted"}
    stream_started = 0.0
    stream_ended = 0.0
    terminal_reason: str | None = None
    try:
        ws_url = create_live_session_ws_url(http_base, timeout_s)
        result["ws_url"] = ws_url
        ws_conn = getattr(websocket_module, "create_connection")(ws_url, timeout=timeout_s)
        try:
            header, payload, hello_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_HELLO_REQ, b"", 1, FRAME_HELLO_RESP, timeout_s)
            if header.frame_type != FRAME_HELLO_RESP:
                error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
                result.update({"pass": False, "reason": f"HELLO returned 0x{header.frame_type:02x}", "error": error})
                return result
            hello = parse_hello_resp(payload)
            result["handshake"] = {"hello": hello, "hello_payload_bytes": len(payload), "hello_wait": hello_wait}
            header, payload, caps_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_CAPS_REQ, b"", 2, FRAME_CAPS_RESP, timeout_s)
            if header.frame_type != FRAME_CAPS_RESP:
                error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
                result.update({"pass": False, "reason": f"CAPS returned 0x{header.frame_type:02x}", "error": error})
                return result
            result["handshake"]["caps_payload_bytes"] = len(payload)
            result["handshake"]["caps_wait"] = caps_wait
            config_frame_type, config_payload, config_encoding = build_config_request(
                mode.mode_id,
                mode.mask,
                samplerate_khz,
                pre_samples,
                post_samples,
                trigger_type,
                trigger_channel,
                handshake_supports_config_v2(cast(Mapping[str, object], result["handshake"])),
            )
            result["config_encoding"] = config_encoding
            result["config_frame_type"] = config_frame_type
            header, payload, config_wait = ws_send_request_wait_response(ws_conn, websocket_module, config_frame_type, config_payload, 3, FRAME_CONFIG_RESP, timeout_s)
            result["config_wait"] = config_wait
            if header.frame_type == FRAME_ERROR:
                result.update({"pass": False, "reason": "CONFIG returned ERROR", "error": parse_error(payload)})
                return result
            if header.frame_type != FRAME_CONFIG_RESP:
                result.update({"pass": False, "reason": f"CONFIG returned 0x{header.frame_type:02x}"})
                return result
            result["config_ack"] = parse_ack(payload)
            header, payload, start_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_START_REQ, b"", 4, FRAME_START_RESP, timeout_s)
            result["start_wait"] = start_wait
            if header.frame_type != FRAME_START_RESP:
                error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
                result.update({"pass": False, "reason": f"START returned 0x{header.frame_type:02x}", "error": error})
                return result
            result["start_ack"] = parse_ack(payload)
            start_wait_reason = strict_start_wait_failure_reason(start_wait)
            if start_wait_reason is not None:
                stop_response = ws_sigrok_stop(ws_conn, websocket_module, timeout_s, 5)
                result.update({"pass": False, "reason": start_wait_reason, "stop_response": stop_response, "client_stop_sent": bool(stop_response.get("sent")), "client_stopped": bool(stop_response.get("received")) and stop_response.get("frame_type") == FRAME_STOP_RESP})
                return result
            if trigger_required and uart_args is not None and uart_args.uart_stimulus is not None:
                result["uart_stimulus"] = perform_uart_stimulus(uart_args)
            target_samples = pre_samples + post_samples if bounded else None
            stream_started = time.monotonic()
            read_deadline = stream_started + (duration_s if duration_s is not None else timeout_s)
            while True:
                if bounded and target_samples is not None and stats.received_sample_count >= target_samples and (not trigger_required or stats.triggered_events > 0):
                    break
                if time.monotonic() >= read_deadline:
                    break
                try:
                    frame_header, frame_payload = ws_recv_frame(ws_conn, max(0.05, min(timeout_s, read_deadline - time.monotonic())))
                except TimeoutError:
                    break
                except Exception as exc:
                    stats.disconnects += 1
                    result.setdefault("stream_disconnect_error", exception_detail(exc))
                    break
                if frame_header.frame_type == FRAME_DATA:
                    meta = parse_data_meta(frame_payload)
                    observe_sigrok_data_payload(
                        stats,
                        meta,
                        frame_payload,
                        expected_channel_mask=mode.mask,
                        sample_bytes=mode.sample_bytes,
                    )
                elif frame_header.frame_type == FRAME_ERROR:
                    result["stream_error"] = parse_error(frame_payload)
                    terminal_reason = TERMINAL_REASON_SERVER_ERROR
                    break
                elif frame_header.frame_type == FRAME_EVENT:
                    event = stats.observe_event(frame_payload)
                    result.setdefault("events", 0)
                    result["events"] += 1
                    terminal_reason = stats.terminal_reason_for_event(event)
                    if terminal_reason is not None:
                        result["terminal_reason"] = terminal_reason
                        result["terminal_event"] = asdict(event)
                        break
            stream_ended = time.monotonic()
            if terminal_reason is None:
                stop_response = ws_sigrok_stop(ws_conn, websocket_module, timeout_s, 5)
        finally:
            try:
                getattr(ws_conn, "close")()
            except Exception:
                pass
        immediate_restart = ws_sigrok_fresh_restart_probe(http_base, websocket_module, mode.mode_id, mode.mask, samplerate_khz, timeout_s)
    except Exception as exc:  # noqa: BLE001
        stats.disconnects += 1
        result.update({"pass": False, "reason": str(exc)})
        result.setdefault("stream_disconnect_error", exception_detail(exc))
    elapsed = time.monotonic() - started
    stream_elapsed = max(0.0, stream_ended - stream_started)
    requested_samples = pre_samples + post_samples if bounded else None
    requested_pre_post_met = True if requested_samples is None else stats.received_sample_count >= requested_samples
    board_health_after = board_health(http_base, timeout_s)
    passed, reason = evaluate_sigrok_pass(
        bounded=bounded,
        trigger_required=trigger_required,
        stats=stats,
        requested_samples=requested_samples,
        requested_duration_s=duration_s,
        stream_elapsed_s=stream_elapsed,
        stop_response=stop_response,
        immediate_restart=immediate_restart,
        board_health_after=board_health_after,
        terminal_reason=terminal_reason,
    )
    result.update({
        "pass": passed,
        "reason": reason,
        "elapsed_s": elapsed,
        "stream_elapsed_s": stream_elapsed,
        "stats": {**asdict(stats), **stats.rates(stream_elapsed)},
        "trigger_event_received": stats.triggered_events > 0,
        "trigger_sample_index": stats.trigger_sample_index,
        "trigger_sample_offset_samples": stats.trigger_sample_offset(),
        "trigger_sample_offset_valid": stats.trigger_sample_offset_valid(),
        "requested_pre_post_samples": requested_samples,
        "requested_pre_post_met": requested_pre_post_met,
        "bounded_target_met_before_stop": bounded and requested_pre_post_met,
        "client_stop_sent": bool(stop_response.get("sent")),
        "client_stopped": bool(stop_response.get("received")) and stop_response.get("frame_type") == FRAME_STOP_RESP,
        "server_auto_stopped": terminal_reason in (TERMINAL_REASON_SERVER_STOPPED, TERMINAL_REASON_SERVER_OVERRUN) and not bool(stop_response.get("sent")),
        "terminal_reason": terminal_reason,
        "continuous_data_observed": not bounded and stats.data_frames > 0,
        "capacity_stop_before_data": not bounded and terminal_reason == TERMINAL_REASON_SERVER_OVERRUN and stats.data_frames == 0,
        "stop_response": stop_response,
        "immediate_restart": immediate_restart,
        "board_http_health_after": board_health_after,
    })
    return result


def sigrok_expected_sample_bytes(channel_mask: int) -> int:
    return sigrok_bytes_per_sample(channel_mask)


def high_rate_case_descriptor(
    *,
    transport: str,
    mode_name: str,
    samplerate_khz: int,
    pre_samples: int,
    post_samples: int,
    trigger_type: int,
    trigger_channel: int,
    semantics: str,
    expect_rejection: bool = False,
    target_data_samples: int | None = None,
    manual_stop_after_s: float | None = None,
) -> dict[str, Any]:
    mode = MODE_CASES[mode_name]
    return {
        "transport": transport,
        "mode": mode_name,
        "mode_id": mode.mode_id,
        "channel_mask": mode.mask,
        "pins": mode.pins,
        "sample_bytes": mode.sample_bytes,
        "computed_bytes_per_sample": sigrok_expected_sample_bytes(mode.mask),
        "requested_samplerate_khz": samplerate_khz,
        "sigrok_pre_samples": pre_samples,
        "sigrok_post_samples": post_samples,
        "trigger": {"type": trigger_type, "name": TRIGGER_NAMES[trigger_type], "channel": trigger_channel},
        "capture_semantics": semantics,
        "config_requires_v2": pre_samples > 0xFFFF or post_samples > 0xFFFF,
        "requires_hello_flags": {"config_v2": True, "generic_packed_burst": True, "mask": SERVER_EXPECTED_HIGH_RATE_FLAGS},
        "expect_rejection": expect_rejection,
        "target_data_samples": target_data_samples,
        "manual_stop_after_s": manual_stop_after_s,
        "mapping_bits": {f"GP{pin}": bit for bit, pin in enumerate(MODE_CASES[mode_name].pins)} if mode_name != "FAST8_SPARSE_GP10_GP13_GP17" else {"GP10": 0, "GP13": 3, "GP17": 7},
        "physical_stimulus_scope": "GP10 may be driven by /dev/ttyACM1 TX; unconnected selected pins are expected-low only",
    }


def iter_high_rate_packed_burst_cases(args: argparse.Namespace) -> Iterable[dict[str, Any]]:
    for transport in ("tcp", "websocket"):
        yield {"case_kind": "hello_flags", "transport": transport, "requires_hello_flags": {"config_v2": True, "generic_packed_burst": True, "mask": SERVER_EXPECTED_HIGH_RATE_FLAGS}}
        for rate in HIGH_RATE_PACKED_BURST_RATES_KHZ:
            for post in HIGH_RATE_PACKED_BURST_POST_SAMPLES:
                for trigger_type in HIGH_RATE_PACKED_BURST_TRIGGER_TYPES:
                    yield high_rate_case_descriptor(
                        transport=transport,
                        mode_name="SINGLE",
                        samplerate_khz=rate,
                        pre_samples=0,
                        post_samples=post,
                        trigger_type=trigger_type,
                        trigger_channel=args.trigger_channel,
                        semantics="high_rate_triggered_bounded_capture" if trigger_type != TRIGGER_NONE else "high_rate_bounded_capture",
                    )
            yield high_rate_case_descriptor(
                transport=transport,
                mode_name="FAST8_SPARSE_GP10_GP13_GP17",
                samplerate_khz=rate,
                pre_samples=0,
                post_samples=100000,
                trigger_type=TRIGGER_NONE,
                trigger_channel=args.trigger_channel,
                semantics="sparse_fast8_high_rate_bounded_capture_expected_low_on_unconnected_selected_pins",
            )
        yield high_rate_case_descriptor(
            transport=transport,
            mode_name="WIDE11",
            samplerate_khz=100000,
            pre_samples=0,
            post_samples=100000,
            trigger_type=TRIGGER_NONE,
            trigger_channel=args.trigger_channel,
            semantics="wide11_existing_100mhz_mapping_compatible_bounded_capture",
        )
        yield high_rate_case_descriptor(
            transport=transport,
            mode_name="WIDE11",
            samplerate_khz=125000,
            pre_samples=0,
            post_samples=100000,
            trigger_type=TRIGGER_NONE,
            trigger_channel=args.trigger_channel,
            semantics="wide11_125mhz_rejection",
            expect_rejection=True,
        )
        yield high_rate_case_descriptor(
            transport=transport,
            mode_name="SINGLE",
            samplerate_khz=HIGH_RATE_PACKED_BURST_CONTINUOUS_RATE_KHZ,
            pre_samples=0,
            post_samples=0,
            trigger_type=TRIGGER_NONE,
            trigger_channel=args.trigger_channel,
            semantics="continuous_until_capacity_exact_data_then_normal_stopped",
            target_data_samples=HIGH_RATE_PACKED_BURST_CONTINUOUS_TARGET_SAMPLES,
        )
        yield high_rate_case_descriptor(
            transport=transport,
            mode_name="SINGLE",
            samplerate_khz=HIGH_RATE_PACKED_BURST_MANUAL_STOP_RATE_KHZ,
            pre_samples=0,
            post_samples=0,
            trigger_type=TRIGGER_NONE,
            trigger_channel=args.trigger_channel,
            semantics="manual_stop_lower_rate_distinguished_from_capacity_stop",
            manual_stop_after_s=HIGH_RATE_PACKED_BURST_MANUAL_STOP_DURATION_S,
        )


def high_rate_hello_flags_case(args: argparse.Namespace, transport: str) -> dict[str, Any]:
    result: dict[str, Any] = {
        "case": "sigrok_high_rate_hello_flags",
        "transport": transport,
        "required_server_flags": SERVER_EXPECTED_HIGH_RATE_FLAGS,
        "required_capabilities": ["config_v2", "generic_packed_burst"],
    }
    ws_conn: object | None = None
    started = time.monotonic()
    try:
        if transport == "tcp":
            with socket.create_connection((args.tcp_host, args.tcp_port), timeout=args.timeout) as sock:
                sock.settimeout(args.timeout)
                handshake = sigrok_handshake(sock, args.timeout)
        else:
            websocket_module = load_websocket_module()
            ws_url = create_live_session_ws_url(args.http_base, args.timeout)
            ws_conn = getattr(websocket_module, "create_connection")(ws_url, timeout=args.timeout)
            header, payload, hello_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_HELLO_REQ, b"", 1, FRAME_HELLO_RESP, args.timeout)
            if header.frame_type != FRAME_HELLO_RESP:
                error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
                result.update({"pass": False, "reason": f"HELLO returned 0x{header.frame_type:02x}", "error": error, "elapsed_s": time.monotonic() - started})
                return result
            header, caps_payload, caps_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_CAPS_REQ, b"", 2, FRAME_CAPS_RESP, args.timeout)
            if header.frame_type != FRAME_CAPS_RESP:
                error = parse_error(caps_payload) if header.frame_type == FRAME_ERROR else None
                result.update({"pass": False, "reason": f"CAPS returned 0x{header.frame_type:02x}", "error": error, "elapsed_s": time.monotonic() - started})
                return result
            handshake = {"hello": parse_hello_resp(payload), "hello_payload_bytes": len(payload), "caps_payload_bytes": len(caps_payload), "hello_wait": hello_wait, "caps_wait": caps_wait}
        hello = cast(Mapping[str, Any], handshake.get("hello", {}))
        passed = handshake_has_expected_high_rate_flags(handshake)
        result.update({"pass": passed, "reason": "ok" if passed else "HELLO server_flags missing config_v2 or generic_packed_burst", "handshake": handshake, "server_flags": hello.get("server_flags"), "capabilities": {"config_v2": hello.get("supports_config_v2"), "generic_packed_burst": hello.get("supports_generic_packed_burst")}, "elapsed_s": time.monotonic() - started})
    except Exception as exc:  # noqa: BLE001
        result.update({"pass": False, "reason": str(exc), "error": exception_detail(exc), "elapsed_s": time.monotonic() - started})
    finally:
        if ws_conn is not None:
            try:
                getattr(ws_conn, "close")()
            except Exception:
                pass
    return result


def high_rate_evaluate_result(
    *,
    descriptor: Mapping[str, Any],
    stats: StreamStats,
    stop_response: Mapping[str, Any],
    immediate_restart: Mapping[str, Any],
    board_health_after: Mapping[str, Any],
    rejected: bool,
    rejection_error: Mapping[str, Any] | None,
    terminal_reason: str | None = None,
) -> tuple[bool, str]:
    if bool(descriptor.get("expect_rejection")):
        return (True, "ok") if rejected else (False, "expected rejection did not occur")
    if rejected:
        return False, f"unexpected rejection: {rejection_error}"
    target_samples = descriptor.get("target_data_samples") or descriptor.get("sigrok_post_samples")
    exact_samples = int(target_samples) if isinstance(target_samples, int) and target_samples > 0 else None
    manual_stop = descriptor.get("manual_stop_after_s") is not None
    if terminal_reason == TERMINAL_REASON_SERVER_ERROR:
        return False, "server terminal error received"
    server_overrun = terminal_reason == TERMINAL_REASON_SERVER_OVERRUN
    if server_overrun and not manual_stop:
        return False, "server overrun before bounded capture completion"
    checks = [
        (handshake_has_expected_high_rate_flags(cast(Mapping[str, object], descriptor.get("handshake", {}))), "HELLO server_flags missing config_v2 or generic_packed_burst"),
        (int(descriptor.get("sample_bytes", 0) or 0) == int(descriptor.get("computed_bytes_per_sample", -1) or -1), "bytes/sample does not match selected channel mask"),
        (stats.data_frames > 0, "no DATA frames received"),
        (stats.invalid_sample_count_frames == 0, "invalid DATA sample_count detected"),
        (stats.payload_over_budget_frames == 0, "DATA payload exceeds sample-count budget"),
        (stats.channel_mask_mismatch_frames == 0, "DATA channel mask mismatch detected"),
        (stats.data_decode_error_frames == 0, "DATA payload decode errors detected"),
        (stats.sample_index_gaps == 0, "sample_index gaps detected"),
        (stats.disconnects == 0, "disconnects detected"),
        (exact_samples is None or stats.received_sample_count == exact_samples, "exact DATA sample count mismatch"),
        (manual_stop or stats.stopped_events > 0, "normal STOPPED terminal event not received"),
        (not manual_stop or server_overrun or bool(stop_response.get("sent")), "manual STOP_REQ was not sent"),
        (manual_stop or not bool(stop_response.get("sent")), "client STOP_REQ sent for auto-stopped capture"),
        (manual_stop or stats.stopped_events > 0, "server/capacity STOPPED event not distinguished"),
        (bool(immediate_restart.get("ok")), "immediate restart failed"),
        (bool(board_health_after.get("ok")), "board HTTP health is not ok"),
    ]
    for passed, reason in checks:
        if not passed:
            return False, reason
    return True, "ok"


def sigrok_high_rate_packed_burst_capture_case(args: argparse.Namespace, descriptor: Mapping[str, Any]) -> dict[str, Any]:
    transport = str(descriptor["transport"])
    mode_name = str(descriptor["mode"])
    mode = MODE_CASES[mode_name]
    samplerate_khz = int(descriptor["requested_samplerate_khz"])
    pre_samples = int(descriptor["sigrok_pre_samples"])
    post_samples = int(descriptor["sigrok_post_samples"])
    trigger = cast(Mapping[str, Any], descriptor["trigger"])
    trigger_type = int(trigger["type"])
    trigger_channel = int(trigger["channel"])
    target_data_samples = descriptor.get("target_data_samples")
    manual_stop_after_s = descriptor.get("manual_stop_after_s")
    expect_rejection = bool(descriptor.get("expect_rejection"))
    stats = StreamStats()
    result: dict[str, Any] = {
        "case": "sigrok_high_rate_packed_burst",
        **dict(descriptor),
        "timeout_s": args.timeout,
    }
    ws_conn: object | None = None
    websocket_module: object | None = None
    started = time.monotonic()
    stream_started = 0.0
    stream_ended = 0.0
    rejected = False
    rejection_error: dict[str, Any] | None = None
    terminal_reason: str | None = None
    stop_response: dict[str, Any] = {"received": False, "sent": False, "reason": "not sent"}
    immediate_restart: dict[str, Any] = {"ok": False, "reason": "not attempted"}
    conn_cm: Any | None = None
    try:
        if transport == "tcp":
            conn_cm = socket.create_connection((args.tcp_host, args.tcp_port), timeout=args.timeout)
            sock_or_ws: object = conn_cm.__enter__()
            cast(socket.socket, sock_or_ws).settimeout(args.timeout)
            handshake = sigrok_handshake(cast(socket.socket, sock_or_ws), args.timeout)
        else:
            websocket_module = load_websocket_module()
            ws_url = create_live_session_ws_url(args.http_base, args.timeout)
            result["ws_url"] = ws_url
            ws_conn = getattr(websocket_module, "create_connection")(ws_url, timeout=args.timeout)
            sock_or_ws = ws_conn
            header, payload, hello_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_HELLO_REQ, b"", 1, FRAME_HELLO_RESP, args.timeout)
            if header.frame_type != FRAME_HELLO_RESP:
                rejection_error = parse_error(payload) if header.frame_type == FRAME_ERROR else {"frame_type": header.frame_type}
                result.update({"pass": False, "reason": f"HELLO returned 0x{header.frame_type:02x}", "error": rejection_error})
                return result
            header, caps_payload, caps_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_CAPS_REQ, b"", 2, FRAME_CAPS_RESP, args.timeout)
            if header.frame_type != FRAME_CAPS_RESP:
                rejection_error = parse_error(caps_payload) if header.frame_type == FRAME_ERROR else {"frame_type": header.frame_type}
                result.update({"pass": False, "reason": f"CAPS returned 0x{header.frame_type:02x}", "error": rejection_error})
                return result
            handshake = {"hello": parse_hello_resp(payload), "hello_payload_bytes": len(payload), "caps_payload_bytes": len(caps_payload), "hello_wait": hello_wait, "caps_wait": caps_wait}
        result["handshake"] = handshake
        config_frame_type, config_payload, config_encoding = build_config_request(
            mode.mode_id,
            mode.mask,
            samplerate_khz,
            pre_samples,
            post_samples,
            trigger_type,
            trigger_channel,
            handshake_supports_config_v2(handshake),
        )
        result["config_encoding"] = config_encoding
        result["config_frame_type"] = config_frame_type
        if transport == "tcp":
            header, payload, config_wait = sigrok_send_request_wait_response(cast(socket.socket, sock_or_ws), config_frame_type, config_payload, 3, FRAME_CONFIG_RESP, args.timeout)
        else:
            assert websocket_module is not None and ws_conn is not None
            header, payload, config_wait = ws_send_request_wait_response(ws_conn, websocket_module, config_frame_type, config_payload, 3, FRAME_CONFIG_RESP, args.timeout)
        result["config_wait"] = config_wait
        if header.frame_type == FRAME_ERROR:
            rejected = True
            rejection_error = parse_error(payload)
        elif header.frame_type != FRAME_CONFIG_RESP:
            result.update({"pass": False, "reason": f"CONFIG returned 0x{header.frame_type:02x}"})
            return result
        else:
            result["config_ack"] = parse_ack(payload)
            if transport == "tcp":
                header, payload, start_wait = sigrok_send_request_wait_response(cast(socket.socket, sock_or_ws), FRAME_START_REQ, b"", 4, FRAME_START_RESP, args.timeout)
            else:
                assert websocket_module is not None and ws_conn is not None
                header, payload, start_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_START_REQ, b"", 4, FRAME_START_RESP, args.timeout)
            result["start_wait"] = start_wait
            if header.frame_type == FRAME_ERROR:
                rejected = True
                rejection_error = parse_error(payload)
            elif header.frame_type != FRAME_START_RESP:
                result.update({"pass": False, "reason": f"START returned 0x{header.frame_type:02x}"})
                return result
            else:
                result["start_ack"] = parse_ack(payload)
                start_wait_reason = strict_start_wait_failure_reason(start_wait)
                if start_wait_reason is not None:
                    if transport == "tcp":
                        stop_response = sigrok_stop(cast(socket.socket, sock_or_ws), args.timeout, 5)
                    else:
                        assert websocket_module is not None and ws_conn is not None
                        stop_response = ws_sigrok_stop(ws_conn, websocket_module, args.timeout, 5)
                    result.update({"pass": False, "reason": start_wait_reason, "stop_response": stop_response})
                    return result
                if trigger_type != TRIGGER_NONE and args.uart_stimulus is not None:
                    result["uart_stimulus"] = perform_uart_stimulus(args)
                target_samples = int(target_data_samples) if isinstance(target_data_samples, int) else (pre_samples + post_samples if post_samples > 0 else None)
                stream_started = time.monotonic()
                read_deadline = stream_started + (float(manual_stop_after_s) if isinstance(manual_stop_after_s, float) else args.timeout)
                while time.monotonic() < read_deadline:
                    if target_samples is not None and stats.received_sample_count >= target_samples and stats.stopped_events > 0:
                        break
                    try:
                        per_frame_timeout = max(0.05, min(args.timeout, read_deadline - time.monotonic()))
                        if transport == "tcp":
                            frame_header, frame_payload = recv_frame(cast(socket.socket, sock_or_ws), per_frame_timeout)
                        else:
                            assert ws_conn is not None
                            frame_header, frame_payload = ws_recv_frame(ws_conn, per_frame_timeout)
                    except TimeoutError:
                        break
                    except ConnectionError as exc:
                        stats.disconnects += 1
                        result.setdefault("stream_disconnect_error", exception_detail(exc))
                        break
                    except Exception as exc:  # noqa: BLE001
                        stats.disconnects += 1
                        result.setdefault("stream_disconnect_error", exception_detail(exc))
                        break
                    if frame_header.frame_type == FRAME_DATA:
                        meta = parse_data_meta(frame_payload)
                        observe_sigrok_data_payload(stats, meta, frame_payload, expected_channel_mask=mode.mask, sample_bytes=mode.sample_bytes)
                    elif frame_header.frame_type == FRAME_EVENT:
                        event = stats.observe_event(frame_payload)
                        result.setdefault("terminal_events", [])
                        cast(list[Any], result["terminal_events"]).append(asdict(event))
                        if stats.is_terminal_event(event):
                            terminal_reason = stats.terminal_reason_for_event(event)
                            break
                    elif frame_header.frame_type == FRAME_ERROR:
                        result["stream_error"] = parse_error(frame_payload)
                        terminal_reason = TERMINAL_REASON_SERVER_ERROR
                        break
                stream_ended = time.monotonic()
                if manual_stop_after_s is not None and terminal_reason is None:
                    if transport == "tcp":
                        stop_response = sigrok_stop(cast(socket.socket, sock_or_ws), args.timeout, 5)
                    else:
                        assert websocket_module is not None and ws_conn is not None
                        stop_response = ws_sigrok_stop(ws_conn, websocket_module, args.timeout, 5)
        if transport == "tcp":
            assert conn_cm is not None
            cast(Any, conn_cm).__exit__(None, None, None)
            conn_cm = None
        elif ws_conn is not None:
            getattr(ws_conn, "close")()
            ws_conn = None
        if not expect_rejection and not rejected:
            if transport == "tcp":
                immediate_restart = sigrok_fresh_restart_probe(args.tcp_host, args.tcp_port, mode.mode_id, mode.mask, samplerate_khz, args.timeout)
            else:
                assert websocket_module is not None
                immediate_restart = ws_sigrok_fresh_restart_probe(args.http_base, websocket_module, mode.mode_id, mode.mask, samplerate_khz, args.timeout)
    except Exception as exc:  # noqa: BLE001
        stats.disconnects += 1
        result.update({"pass": False, "reason": str(exc), "error": exception_detail(exc)})
    finally:
        if conn_cm is not None:
            try:
                cast(Any, conn_cm).__exit__(None, None, None)
            except Exception:
                pass
        if ws_conn is not None:
            try:
                getattr(ws_conn, "close")()
            except Exception:
                pass
    board_health_after = board_health(args.http_base, min(args.timeout, HIGH_RATE_PACKED_BURST_HEALTH_RECOVERY_TIMEOUT_S))
    descriptor_for_eval = {**dict(descriptor), "handshake": result.get("handshake", {})}
    passed, reason = high_rate_evaluate_result(
        descriptor=descriptor_for_eval,
        stats=stats,
        stop_response=stop_response,
        immediate_restart=immediate_restart,
        board_health_after=board_health_after,
        rejected=rejected,
        rejection_error=rejection_error,
        terminal_reason=terminal_reason,
    )
    result.update({
        "pass": passed,
        "reason": reason,
        "rejected": rejected,
        "rejection_error": rejection_error,
        "elapsed_s": time.monotonic() - started,
        "stream_elapsed_s": max(0.0, stream_ended - stream_started),
        "stats": {**asdict(stats), **stats.rates(max(0.0, stream_ended - stream_started))},
        "exact_sample_count_met": (target_data_samples if isinstance(target_data_samples, int) else post_samples if post_samples > 0 else None) == stats.received_sample_count,
        "frame_continuity": {"sample_index_modulo_bits": 24, "sample_index_gaps": stats.sample_index_gaps, "first_gap_records": stats.first_gap_records},
        "terminal_reason": terminal_reason if terminal_reason is not None else "manual_stop" if manual_stop_after_s is not None else "rejected" if rejected else "normal_stopped_event" if stats.stopped_events > 0 else "missing_terminal_event",
        "client_stop_sent": bool(stop_response.get("sent")),
        "client_stopped": bool(stop_response.get("received")) and stop_response.get("frame_type") == FRAME_STOP_RESP,
        "server_auto_stopped": terminal_reason in (TERMINAL_REASON_SERVER_STOPPED, TERMINAL_REASON_SERVER_OVERRUN) and not bool(stop_response.get("sent")),
        "stop_response": stop_response,
        "immediate_restart": immediate_restart,
        "board_http_health_after": board_health_after,
    })
    return result


def exception_matches_ebusy(exc: BaseException) -> bool:
    visited: set[int] = set()

    def visit(candidate: object) -> bool:
        if not isinstance(candidate, BaseException):
            return False
        candidate_id = id(candidate)
        if candidate_id in visited:
            return False
        visited.add(candidate_id)

        err_no = getattr(candidate, "errno", None)
        if err_no == errno.EBUSY:
            return True

        for arg in getattr(candidate, "args", ()):  # pyserial SerialException often stores an OSError or text in args.
            if isinstance(arg, BaseException) and visit(arg):
                return True

        message = str(candidate).lower()
        if f"errno {errno.EBUSY}" in message or "device or resource busy" in message or "resource busy" in message:
            return True

        return visit(getattr(candidate, "__cause__", None)) or visit(getattr(candidate, "__context__", None))

    return visit(exc)


def perform_uart_stimulus(args: argparse.Namespace) -> dict[str, Any] | None:
    if args.uart_stimulus is None:
        return None
    if args.uart_device != "/dev/ttyACM1":
        raise ValueError("UART stimulus requires explicit --uart-device /dev/ttyACM1")
    if args.uart_baud is None:
        raise ValueError("UART stimulus requires explicit --uart-baud")
    try:
        serial_module = importlib.import_module("serial")
    except ImportError as exc:
        raise RuntimeError("UART stimulus requires pyserial") from exc
    serial_class = getattr(serial_module, "Serial")
    started = time.monotonic()
    retry_window_s = min(max(float(args.timeout), 0.0), UART_STIMULUS_BUSY_RETRY_WINDOW_S)
    deadline = started + retry_window_s
    attempts = 0
    while True:
        attempts += 1
        try:
            with serial_class(
                port=args.uart_device,
                baudrate=args.uart_baud,
                bytesize=getattr(serial_module, "EIGHTBITS"),
                parity=getattr(serial_module, "PARITY_NONE"),
                stopbits=getattr(serial_module, "STOPBITS_ONE"),
                timeout=args.timeout,
                write_timeout=args.timeout,
            ) as ser:
                payload = args.uart_stimulus.encode("utf-8")
                written = ser.write(payload)
                ser.flush()
            return {"device": args.uart_device, "baud": args.uart_baud, "format": "8N1", "bytes_written": written, "attempts": attempts, "elapsed_s": time.monotonic() - started}
        except Exception as exc:
            if not exception_matches_ebusy(exc):
                raise
            now = time.monotonic()
            if now >= deadline:
                raise
            time.sleep(min(UART_STIMULUS_BUSY_RETRY_SLEEP_S, max(0.0, deadline - now)))


def iter_tcp_bounded_cases(args: argparse.Namespace) -> Iterable[tuple[str, int, int, int, int]]:
    for mode_name in args.modes:
        for rate in args.tcp_rates_khz:
            for pre in args.tcp_pre_samples:
                for post in args.tcp_post_samples:
                    for trigger_type in args.trigger_types:
                        if post != 0:
                            yield mode_name, rate, pre, post, trigger_type


def iter_tcp_continuous_cases(args: argparse.Namespace) -> Iterable[tuple[str, int, float]]:
    for mode_name in args.modes:
        for rate in args.tcp_rates_khz:
            for duration in args.continuous_durations_s:
                yield mode_name, rate, duration


def iter_ws_bounded_cases(args: argparse.Namespace) -> Iterable[tuple[str, int, int, int, int]]:
    yield from iter_tcp_bounded_cases(args)


def iter_ws_continuous_cases(args: argparse.Namespace) -> Iterable[tuple[str, int, float]]:
    yield from iter_tcp_continuous_cases(args)


def build_plan(args: argparse.Namespace) -> dict[str, Any]:
    matrices = []
    if "high-rate-packed-burst" in args.matrix:
        matrices.append({
            "name": "sigrok_high_rate_packed_burst",
            "cases": list(iter_high_rate_packed_burst_cases(args)),
            "timeout_policy": f"per-operation timeout is bounded by --timeout (default {args.timeout:g}s); health recovery probes cap at {HIGH_RATE_PACKED_BURST_HEALTH_RECOVERY_TIMEOUT_S:g}s",
            "wiring_scope": "Available HIL wiring is /dev/ttyACM1 TX -> GP10. Unconnected selected pins are expected-low only; this matrix does not invent high-state evidence for those pins.",
        })
    if "wide11-telemetry-isolation" in args.matrix:
        matrices.append({
            "name": "wide11_telemetry_isolation",
            "cases": [{
                "telemetry_client": {"transport": "json_websocket", "rate_hz": args.telemetry_isolation_rate_hz, "baseline_samples": args.telemetry_isolation_baseline_samples, "post_release_samples": args.telemetry_isolation_post_release_samples},
                "capture_client": {"transport": "raw_tcp_sigrok", "mode": "WIDE11", "samplerate_khz": WIDE11_MAPPING_RATE_KHZ, "pre_samples": 0, "post_samples": WIDE11_MAPPING_POST_SAMPLES, "expected_data_frames": WIDE11_BURST_EXPECTED_DATA_FRAMES},
                "expected_overlap_behavior": "baseline-epoch telemetry queued before quiesce is grace-only; same-epoch post-grace telemetry before reset is failure; recv timeout means no data while connected",
                "post_release_epoch_behavior": "sequence reset/regression with advancing device_t_mono_us/uptime_us infers firmware release/resume and begins post-release evidence even before raw helper return",
                "pause_grace_s": TELEMETRY_ISOLATION_GRACE_S,
            }],
        })
    if "wide11-mapping" in args.matrix:
        stimulus_profile = "gp10_uart_low_others" if args.wide11_map_gp10_uart_low_others else "external_4bit"
        matrices.append({
            "name": "sigrok_tcp_wide11_gp10_uart_low_others" if stimulus_profile == "gp10_uart_low_others" else "sigrok_tcp_wide11_deep_burst_mapping",
            "cases": [{
                "mode": "WIDE11",
                "samplerate_khz": WIDE11_MAPPING_RATE_KHZ,
                "pre_samples": WIDE11_MAPPING_PRE_SAMPLES,
                "post_samples": WIDE11_MAPPING_POST_SAMPLES,
                "trigger": {"type": WIDE11_MAPPING_TRIGGER_TYPE, "name": TRIGGER_NAMES[WIDE11_MAPPING_TRIGGER_TYPE], "channel": WIDE11_MAPPING_TRIGGER_CHANNEL},
                "stimulus_profile": stimulus_profile,
                "mapping_profile": stimulus_profile,
                "validation_scope": "single_channel_reduced" if stimulus_profile == "gp10_uart_low_others" else "multi_channel_external_4bit",
                "selected_external_4bit_mapping_validated": False,
                "requires_external_generator": stimulus_profile == "external_4bit",
                "external_generator_acknowledged": bool(args.wide11_map_external_generator),
                "gp10_uart_low_others_acknowledged": bool(args.wide11_map_gp10_uart_low_others),
                "physical_prerequisite": WIDE11_MAPPING_GP10_UART_PREREQUISITE if stimulus_profile == "gp10_uart_low_others" else WIDE11_MAPPING_PHYSICAL_PREREQUISITE,
                "pattern_nibbles": args.wide11_map_pattern,
                "hold_samples": args.wide11_map_hold_samples,
                "check_samples": args.wide11_map_check_samples,
                "zero_mask": WIDE11_MAPPING_LOW_OTHERS_ZERO_MASK if stimulus_profile == "gp10_uart_low_others" else None,
                "uart_stimulus": None if stimulus_profile != "gp10_uart_low_others" else {"device": args.uart_device, "baud": args.uart_baud, "format": "8N1", "sent_after": "START_RESP trigger-safe barrier"},
            }],
        })
    if "tcp-bounded" in args.matrix:
        matrices.append({"name": "sigrok_tcp_bounded_samples", "cases": list(iter_tcp_bounded_cases(args))})
    if "tcp-continuous" in args.matrix:
        matrices.append({"name": "sigrok_tcp_continuous_duration", "cases": list(iter_tcp_continuous_cases(args))})
    if "ws-bounded" in args.matrix:
        matrices.append({"name": "sigrok_ws_bounded_samples", "cases": list(iter_ws_bounded_cases(args))})
    if "ws-continuous" in args.matrix:
        matrices.append({"name": "sigrok_ws_continuous_duration", "cases": list(iter_ws_continuous_cases(args))})
    return {
        "dry_run": args.dry_run,
        "http_base": args.http_base,
        "tcp": {"host": args.tcp_host, "port": args.tcp_port},
        "timeout_s": args.timeout,
        "modes": {name: {"channel_mask": MODE_CASES[name].mask, "pins": MODE_CASES[name].pins} for name in args.modes},
        "high_rate_capability_flags": {"config_v2": SERVER_FLAG_CONFIG_V2, "generic_packed_burst": SERVER_FLAG_GENERIC_PACKED_BURST, "required_mask": SERVER_EXPECTED_HIGH_RATE_FLAGS},
        "bounded_triggers": [{"type": trigger_type, "name": TRIGGER_NAMES[trigger_type], "channel": args.trigger_channel} for trigger_type in args.trigger_types],
        "continuous_trigger_note": "continuous matrices always use TRIGGER_NONE and report effective receive-rate diagnostics only",
        "matrices": matrices,
        "uart_stimulus": None if args.uart_stimulus is None else {
            "device": args.uart_device,
            "baud": args.uart_baud,
            "format": "8N1",
                "sent_after": "START_RESP trigger-safe barrier" if args.wide11_map_gp10_uart_low_others or "high-rate-packed-burst" in args.matrix else "before matrix run unless bounded positive trigger defers it",
        },
        "wide11_mapping_physical_prerequisite": WIDE11_MAPPING_PHYSICAL_PREREQUISITE,
        "wide11_gp10_uart_low_others_physical_prerequisite": WIDE11_MAPPING_GP10_UART_PREREQUISITE,
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run logic-analyzer HIL performance matrices")
    parser.add_argument("--dry-run", action="store_true", help="print machine-readable plan without board actions")
    parser.add_argument("--http-base", default="http://172.29.203.1")
    parser.add_argument("--tcp-host", default="172.29.203.1")
    parser.add_argument("--tcp-port", type=int, default=5556)
    parser.add_argument("--timeout", type=float, default=5.0, help="bounded per-operation timeout in seconds")
    parser.add_argument("--matrix", action="append", choices=["tcp-bounded", "tcp-continuous", "ws-bounded", "ws-continuous", "wide11-mapping", "wide11-telemetry-isolation", "high-rate-packed-burst", "all"])
    parser.add_argument("--modes", default="SINGLE,FAST8,WIDE11", help="comma-separated case names: SINGLE,FAST8,WIDE11; SINGLE uses protocol FAST8 mode with mask 0x0001")
    parser.add_argument("--tcp-rates-khz", default="1000,5000,25000")
    parser.add_argument("--tcp-pre-samples", default="0")
    parser.add_argument("--tcp-post-samples", default="1024,4096")
    parser.add_argument("--continuous-durations-s", default="5")
    parser.add_argument("--trigger-types", default="none", help="comma-separated bounded trigger types: none,rising,falling,either; continuous matrices remain no-trigger diagnostics")
    parser.add_argument("--trigger-channel", type=int, default=0, help="sigrok trigger channel index byte; GP10 is channel 0 in SINGLE/FAST8")
    parser.add_argument("--json-output", help="write JSON result to this file instead of stdout")
    parser.add_argument("--uart-stimulus", help="optional text stimulus written through pyserial")
    parser.add_argument("--uart-device", help="must be explicitly /dev/ttyACM1 when UART stimulus is used")
    parser.add_argument("--uart-baud", type=int, help="explicit UART baud for 8N1 pyserial stimulus")
    parser.add_argument("--wide11-map-external-generator", action="store_true", help="acknowledge the documented external GP10/GP11/GP18/GP20 pattern-generator wiring and enable the WIDE11 mapping HIL")
    parser.add_argument("--wide11-map-gp10-uart-low-others", action="store_true", help="run the reduced single-wire WIDE11 mapping HIL: /dev/ttyACM1 TX drives GP10 while GP11-GP20 remain externally low")
    parser.add_argument("--wide11-map-pattern", default=",".join(f"0x{value:x}" for value in WIDE11_MAPPING_DEFAULT_PATTERN_NIBBLES), help="comma-separated 4-bit external-generator pattern nibbles; nibble bit0->GP10, bit1->GP11, bit2->GP18, bit3->GP20")
    parser.add_argument("--wide11-map-hold-samples", type=int, default=WIDE11_MAPPING_DEFAULT_HOLD_SAMPLES, help="number of 100MHz sample ticks each WIDE11 mapping pattern nibble is held")
    parser.add_argument("--wide11-map-check-samples", type=int, default=WIDE11_MAPPING_DEFAULT_CHECK_SAMPLES, help="decoded samples checked against the WIDE11 mapping pattern")
    parser.add_argument("--telemetry-isolation-rate-hz", type=int, default=TELEMETRY_ISOLATION_DEFAULT_RATE_HZ, help="JSON WS ADC telemetry subscription rate for wide11-telemetry-isolation")
    parser.add_argument("--telemetry-isolation-baseline-samples", type=int, default=TELEMETRY_ISOLATION_DEFAULT_BASELINE_SAMPLES, help="baseline ADC telemetry records required before raw TCP deep burst")
    parser.add_argument("--telemetry-isolation-post-release-samples", type=int, default=TELEMETRY_ISOLATION_DEFAULT_POST_RELEASE_SAMPLES, help="fresh ADC telemetry records required after raw TCP deep-burst drain/release")
    args = parser.parse_args(argv)
    matrices = set(args.matrix or ["all"])
    args.matrix = ["tcp-bounded", "tcp-continuous", "ws-bounded", "ws-continuous"] if "all" in matrices else sorted(matrices)
    args.modes = [mode.strip() for mode in args.modes.split(",") if mode.strip()]
    unknown_modes = [mode for mode in args.modes if mode not in MODE_CASES]
    if unknown_modes:
        parser.error(f"unknown modes: {', '.join(unknown_modes)}")
    args.tcp_rates_khz = parse_csv_ints(args.tcp_rates_khz)
    args.tcp_pre_samples = parse_csv_ints(args.tcp_pre_samples)
    args.tcp_post_samples = parse_csv_ints(args.tcp_post_samples)
    args.continuous_durations_s = [float(value) for value in args.continuous_durations_s.split(",") if value.strip()]
    try:
        args.trigger_types = parse_csv_trigger_types(args.trigger_types)
    except ValueError as exc:
        parser.error(str(exc))
    if not args.trigger_types:
        parser.error("at least one trigger type is required")
    if not 0 <= args.trigger_channel <= 0xFF:
        parser.error("--trigger-channel must fit uint8")
    try:
        args.wide11_map_pattern = parse_csv_nibbles(args.wide11_map_pattern)
    except ValueError as exc:
        parser.error(str(exc))
    if args.wide11_map_hold_samples <= 0:
        parser.error("--wide11-map-hold-samples must be positive")
    if args.wide11_map_check_samples <= 0 or args.wide11_map_check_samples > WIDE11_MAPPING_POST_SAMPLES:
        parser.error(f"--wide11-map-check-samples must be 1..{WIDE11_MAPPING_POST_SAMPLES}")
    if args.wide11_map_external_generator and args.wide11_map_gp10_uart_low_others:
        parser.error("choose only one WIDE11 mapping stimulus profile: --wide11-map-external-generator or --wide11-map-gp10-uart-low-others")
    if args.wide11_map_gp10_uart_low_others:
        if args.uart_stimulus is None:
            args.uart_stimulus = WIDE11_MAPPING_GP10_UART_DEFAULT_STIMULUS
        if args.uart_device is None:
            args.uart_device = WIDE11_MAPPING_GP10_UART_DEFAULT_DEVICE
        if args.uart_baud is None:
            args.uart_baud = WIDE11_MAPPING_GP10_UART_DEFAULT_BAUD
    if "high-rate-packed-burst" in args.matrix:
        if args.uart_stimulus is None:
            args.uart_stimulus = WIDE11_MAPPING_GP10_UART_DEFAULT_STIMULUS
        if args.uart_device is None:
            args.uart_device = WIDE11_MAPPING_GP10_UART_DEFAULT_DEVICE
        if args.uart_baud is None:
            args.uart_baud = WIDE11_MAPPING_GP10_UART_DEFAULT_BAUD
    if not 1 <= args.telemetry_isolation_rate_hz <= 1000:
        parser.error("--telemetry-isolation-rate-hz must be 1..1000")
    if args.telemetry_isolation_baseline_samples <= 0:
        parser.error("--telemetry-isolation-baseline-samples must be positive")
    if args.telemetry_isolation_post_release_samples <= 0:
        parser.error("--telemetry-isolation-post-release-samples must be positive")
    return args


def run(args: argparse.Namespace) -> dict[str, Any]:
    output: dict[str, Any] = {
        "schema": "radxa-linkr-debugger.logic-analyzer-hil-perf.v1",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "dry_run": args.dry_run,
        "plan": build_plan(args),
        "results": [],
    }
    if args.dry_run:
        output["overall_pass"] = True
        return output
    positive_trigger_bounded = any(trigger_type != TRIGGER_NONE for trigger_type in args.trigger_types) and any(matrix in args.matrix for matrix in ("tcp-bounded", "ws-bounded"))
    deferred_mapping_uart = "wide11-mapping" in args.matrix and args.wide11_map_gp10_uart_low_others
    deferred_high_rate_uart = "high-rate-packed-burst" in args.matrix
    output["uart_stimulus"] = None if positive_trigger_bounded or deferred_mapping_uart or deferred_high_rate_uart else perform_uart_stimulus(args)
    if "high-rate-packed-burst" in args.matrix:
        for descriptor in iter_high_rate_packed_burst_cases(args):
            if descriptor.get("case_kind") == "hello_flags":
                output["results"].append(high_rate_hello_flags_case(args, str(descriptor["transport"])))
            else:
                output["results"].append(sigrok_high_rate_packed_burst_capture_case(args, descriptor))
    if "wide11-mapping" in args.matrix:
        output["results"].append(sigrok_tcp_wide11_mapping_case(args))
    if "wide11-telemetry-isolation" in args.matrix:
        output["results"].append(sigrok_tcp_wide11_telemetry_isolation_case(args))
    if "tcp-bounded" in args.matrix:
        for mode_name, rate, pre, post, trigger_type in iter_tcp_bounded_cases(args):
            output["results"].append(sigrok_capture_case(args.tcp_host, args.tcp_port, args.http_base, mode_name, rate, pre, post, None, args.timeout, trigger_type, args.trigger_channel, args))
    if "tcp-continuous" in args.matrix:
        for mode_name, rate, duration in iter_tcp_continuous_cases(args):
            output["results"].append(sigrok_capture_case(args.tcp_host, args.tcp_port, args.http_base, mode_name, rate, 0, 0, duration, args.timeout))
    if "ws-bounded" in args.matrix:
        for mode_name, rate, pre, post, trigger_type in iter_ws_bounded_cases(args):
            output["results"].append(sigrok_ws_capture_case(args.http_base, mode_name, rate, pre, post, None, args.timeout, trigger_type, args.trigger_channel, args))
    if "ws-continuous" in args.matrix:
        for mode_name, rate, duration in iter_ws_continuous_cases(args):
            output["results"].append(sigrok_ws_capture_case(args.http_base, mode_name, rate, 0, 0, duration, args.timeout))
    output["overall_pass"] = all(bool(result.get("pass")) for result in output["results"])
    return output


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    output = run(args)
    encoded = json.dumps(output, indent=2, sort_keys=True) + "\n"
    if args.json_output:
        with open(args.json_output, "w", encoding="utf-8") as handle:
            handle.write(encoded)
    else:
        sys.stdout.write(encoded)
    return 0 if output.get("overall_pass") else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
