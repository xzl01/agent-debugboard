#!/usr/bin/env python3
"""Logic-analyzer HIL performance matrix runner.

This runner measures the host-observed behavior of the current firmware.  It
does not infer the hardware samplerate from transport throughput: receive rates
reported here are only effective host receive rates for the selected transport.
"""

from __future__ import annotations

import argparse
import importlib
import json
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
FRAME_EVENT = 0x10
FRAME_DATA = 0x11
FRAME_ERROR = 0x7F

TRIGGER_NONE = 0
TRIGGER_RISING = 1
TRIGGER_FALLING = 2
TRIGGER_EITHER = 3
MODE_FAST8 = 1
MODE_WIDE12 = 2
MASK_SINGLE = 0x0001
MASK_FAST8 = 0x00FF
MASK_WIDE12 = 0x0FFF
STREAM_CHUNK_SAMPLES = 1024
MAX_STREAM_DIAGNOSTIC_RECORDS = 16
EVENT_TRIGGERED = 2
EVENT_STOPPED = 4

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
    "WIDE12": ModeCase(mode_id=MODE_WIDE12, mask=MASK_WIDE12, pins=list(range(10, 21)) + [29], sample_bytes=2),
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


@dataclass
class StreamStats:
    data_frames: int = 0
    received_sample_count: int = 0
    payload_bytes: int = 0
    sample_index_gaps: int = 0
    disconnects: int = 0
    stopped_events: int = 0
    triggered_events: int = 0
    first_sample_index: int | None = None
    last_sample_index: int | None = None
    trigger_sample_index: int | None = None
    first_data_sample_indices: list[int] = field(default_factory=list)
    first_gap_records: list[dict[str, int]] = field(default_factory=list)
    invalid_sample_count_frames: int = 0
    payload_over_budget_frames: int = 0
    channel_mask_mismatch_frames: int = 0
    data_decode_error_frames: int = 0
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

    def observe_event(self, payload: bytes) -> None:
        event = parse_event(payload)
        if event.type_detail == EVENT_TRIGGERED:
            self.triggered_events += 1
            if event.sample_index is not None and self.trigger_sample_index is None:
                self.trigger_sample_index = event.sample_index
        elif event.type_detail == EVENT_STOPPED:
            self.stopped_events += 1

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


def recv_exact(sock: socket.socket, byte_count: int, deadline: float) -> bytes:
    chunks: list[bytes] = []
    remaining = byte_count
    while remaining > 0:
        if time.monotonic() >= deadline:
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
    payload = recv_exact(sock, header.payload_len, deadline) if header.payload_len else b""
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
        return {
            "ok": bool(payload.get("ok")),
            "http_status": payload.get("_http_status"),
            "elapsed_s": time.monotonic() - started,
            "error": payload.get("error"),
        }
    except Exception as exc:  # noqa: BLE001 - surfaced in machine-readable result
        return {"ok": False, "elapsed_s": time.monotonic() - started, "error": str(exc)}


def sigrok_handshake(sock: socket.socket, timeout_s: float) -> dict[str, Any]:
    hello_header, hello_payload, hello_wait = sigrok_send_request_wait_response(sock, FRAME_HELLO_REQ, b"", 1, FRAME_HELLO_RESP, timeout_s)
    if hello_header.frame_type != FRAME_HELLO_RESP:
        raise RuntimeError(f"expected HELLO_RESP, got 0x{hello_header.frame_type:02x}")
    caps_header, caps_payload, caps_wait = sigrok_send_request_wait_response(sock, FRAME_CAPS_REQ, b"", 2, FRAME_CAPS_RESP, timeout_s)
    if caps_header.frame_type != FRAME_CAPS_RESP:
        raise RuntimeError(f"expected CAPS_RESP, got 0x{caps_header.frame_type:02x}")
    return {"hello_payload_bytes": len(hello_payload), "caps_payload_bytes": len(caps_payload), "hello_wait": hello_wait, "caps_wait": caps_wait}


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
                    stats.observe_event(data_payload)
            stop = sigrok_stop(sock, timeout_s, 202)
            ok = stats.data_frames > 0 and bool(stop.get("received")) and stop.get("frame_type") == FRAME_STOP_RESP
            reason = "ok" if ok else "fresh restart did not receive DATA before STOP" if stats.data_frames == 0 else "fresh restart STOP_RESP not received"
            return {"ok": ok, "reason": reason, "transport": "tcp", "fresh_connection": True, "handshake": handshake, "config_ack": config_ack, "start_ack": start_ack, "stop_response": stop, "config_wait": config_wait, "start_wait": start_wait, "stats": asdict(stats)}
    except Exception as exc:  # noqa: BLE001
        return {"ok": False, "reason": str(exc), "transport": "tcp", "fresh_connection": True}


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
                stats.observe_event(data_payload)
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
) -> tuple[bool, str]:
    requested_met = True if requested_samples is None else stats.received_sample_count >= requested_samples
    duration_met = True if bounded else requested_duration_s is not None and stream_elapsed_s >= requested_duration_s * 0.95
    checks = [
        (not trigger_required or stats.triggered_events > 0, "trigger event not received"),
        (not trigger_required or stats.trigger_sample_offset_valid(), "trigger sample offset invalid"),
        (stats.data_frames > 0, "no DATA frames received"),
        (stats.invalid_sample_count_frames == 0, "invalid DATA sample_count detected"),
        (stats.payload_over_budget_frames == 0, "DATA payload exceeds sample-count budget"),
        (stats.channel_mask_mismatch_frames == 0, "DATA channel mask mismatch detected"),
        (stats.data_decode_error_frames == 0, "DATA payload decode errors detected"),
        (stats.sample_index_gaps == 0, "sample_index gaps detected"),
        (stats.disconnects == 0, "disconnects detected"),
        (bool(stop_response.get("received")) and stop_response.get("frame_type") == FRAME_STOP_RESP, "STOP_RESP not received"),
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
        "sigrok_pre_samples_uint16": pre_samples,
        "sigrok_post_samples_uint16": post_samples,
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
    try:
        with socket.create_connection((host, port), timeout=timeout_s) as sock:
            sock.settimeout(timeout_s)
            result["handshake"] = sigrok_handshake(sock, timeout_s)
            config_payload = build_config_payload(mode.mode_id, mode.mask, samplerate_khz, pre_samples, post_samples, trigger_type, trigger_channel)
            header, payload, config_wait = sigrok_send_request_wait_response(sock, FRAME_CONFIG_REQ, config_payload, 3, FRAME_CONFIG_RESP, timeout_s)
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
                except ConnectionError:
                    stats.disconnects += 1
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
                    break
                elif frame_header.frame_type == FRAME_EVENT:
                    stats.observe_event(frame_payload)
                    result.setdefault("events", 0)
                    result["events"] += 1

            stream_ended = time.monotonic()
            stop_response = sigrok_stop(sock, timeout_s, 5)
        immediate_restart = sigrok_fresh_restart_probe(host, port, mode.mode_id, mode.mask, samplerate_khz, timeout_s)
    except Exception as exc:  # noqa: BLE001
        stats.disconnects += 1
        result.update({"pass": False, "reason": str(exc)})

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
        "server_auto_stopped": stats.stopped_events > 0 and not bool(stop_response.get("sent")),
        "stop_response": stop_response,
        "immediate_restart": immediate_restart,
        "board_http_health_after": board_health_after,
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
        "sigrok_pre_samples_uint16": pre_samples,
        "sigrok_post_samples_uint16": post_samples,
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
        "sigrok_pre_samples_uint16": pre_samples,
        "sigrok_post_samples_uint16": post_samples,
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
            result["handshake"] = {"hello_payload_bytes": len(payload), "hello_wait": hello_wait}
            header, payload, caps_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_CAPS_REQ, b"", 2, FRAME_CAPS_RESP, timeout_s)
            if header.frame_type != FRAME_CAPS_RESP:
                error = parse_error(payload) if header.frame_type == FRAME_ERROR else None
                result.update({"pass": False, "reason": f"CAPS returned 0x{header.frame_type:02x}", "error": error})
                return result
            result["handshake"]["caps_payload_bytes"] = len(payload)
            result["handshake"]["caps_wait"] = caps_wait
            config_payload = build_config_payload(mode.mode_id, mode.mask, samplerate_khz, pre_samples, post_samples, trigger_type, trigger_channel)
            header, payload, config_wait = ws_send_request_wait_response(ws_conn, websocket_module, FRAME_CONFIG_REQ, config_payload, 3, FRAME_CONFIG_RESP, timeout_s)
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
                except Exception:
                    stats.disconnects += 1
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
                    break
                elif frame_header.frame_type == FRAME_EVENT:
                    stats.observe_event(frame_payload)
                    result.setdefault("events", 0)
                    result["events"] += 1
            stream_ended = time.monotonic()
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
        "server_auto_stopped": stats.stopped_events > 0 and not bool(stop_response.get("sent")),
        "stop_response": stop_response,
        "immediate_restart": immediate_restart,
        "board_http_health_after": board_health_after,
    })
    return result


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
    return {"device": args.uart_device, "baud": args.uart_baud, "format": "8N1", "bytes_written": written, "elapsed_s": time.monotonic() - started}


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
        "bounded_triggers": [{"type": trigger_type, "name": TRIGGER_NAMES[trigger_type], "channel": args.trigger_channel} for trigger_type in args.trigger_types],
        "continuous_trigger_note": "continuous matrices always use TRIGGER_NONE and report effective receive-rate diagnostics only",
        "matrices": matrices,
        "uart_stimulus": None if args.uart_stimulus is None else {"device": args.uart_device, "baud": args.uart_baud, "format": "8N1"},
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run logic-analyzer HIL performance matrices")
    parser.add_argument("--dry-run", action="store_true", help="print machine-readable plan without board actions")
    parser.add_argument("--http-base", default="http://172.29.203.1")
    parser.add_argument("--tcp-host", default="172.29.203.1")
    parser.add_argument("--tcp-port", type=int, default=5556)
    parser.add_argument("--timeout", type=float, default=5.0, help="bounded per-operation timeout in seconds")
    parser.add_argument("--matrix", action="append", choices=["tcp-bounded", "tcp-continuous", "ws-bounded", "ws-continuous", "all"])
    parser.add_argument("--modes", default="SINGLE,FAST8,WIDE12", help="comma-separated case names: SINGLE,FAST8,WIDE12; SINGLE uses protocol FAST8 mode with mask 0x0001")
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
    output["uart_stimulus"] = None if positive_trigger_bounded else perform_uart_stimulus(args)
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
