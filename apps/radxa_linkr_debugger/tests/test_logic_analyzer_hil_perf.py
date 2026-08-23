#!/usr/bin/env python3
"""Offline unit tests for logic_analyzer_hil_perf.py."""

import argparse
import errno
import importlib.util
import pathlib
import sys
import types
import unittest
from dataclasses import asdict
from collections.abc import Mapping
from typing import cast
from unittest import mock

RUNNER_PATH = pathlib.Path(__file__).resolve().with_name("logic_analyzer_hil_perf.py")
RUNNER_SPEC = importlib.util.spec_from_file_location("logic_analyzer_hil_perf", RUNNER_PATH)
if RUNNER_SPEC is None or RUNNER_SPEC.loader is None:
    raise RuntimeError(f"failed to load {RUNNER_PATH}")
runner = importlib.util.module_from_spec(RUNNER_SPEC)
sys.modules[RUNNER_SPEC.name] = runner
RUNNER_SPEC.loader.exec_module(runner)

FakeWebSocketTimeoutException = type("WebSocketTimeoutException", (Exception,), {})


class FakeSocket:
    def __init__(self, frames: list[bytes]) -> None:
        self.buffer = b"".join(frames)
        self.sent: list[bytes] = []
        self.closed = False

    def __enter__(self) -> "FakeSocket":
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()

    def settimeout(self, timeout: float) -> None:
        pass

    def close(self) -> None:
        self.closed = True

    def sendall(self, data: bytes) -> None:
        self.sent.append(data)

    def recv(self, byte_count: int) -> bytes:
        if not self.buffer:
            return b""
        chunk = self.buffer[:byte_count]
        self.buffer = self.buffer[byte_count:]
        return chunk


class FakeWsConnection:
    def __init__(self, frames: list[bytes]) -> None:
        self.frames = frames
        self.sent: list[tuple[bytes, int]] = []
        self.timeout: float | None = None
        self.closed = False
        self.recv_calls = 0

    def settimeout(self, timeout: float) -> None:
        self.timeout = timeout

    def send(self, data: bytes, opcode: int) -> None:
        self.sent.append((data, opcode))

    def recv(self) -> bytes:
        self.recv_calls += 1
        if not self.frames:
            raise TimeoutError("no frames")
        return self.frames.pop(0)

    def close(self) -> None:
        self.closed = True


class FakeWebSocketModule:
    class ABNF:
        OPCODE_BINARY = 2


class FakeJsonWsConnection:
    def __init__(self, frames: list[object]) -> None:
        self.frames = frames
        self.sent: list[object] = []
        self.timeout: float | None = None
        self.closed = False

    def settimeout(self, timeout: float) -> None:
        self.timeout = timeout

    def send(self, data: object, opcode: int | None = None) -> None:
        self.sent.append((data, opcode))

    def recv(self) -> object:
        if not self.frames:
            raise TimeoutError("no json frames")
        frame = self.frames.pop(0)
        if isinstance(frame, BaseException):
            raise frame
        return frame

    def close(self) -> None:
        self.closed = True


class LogicAnalyzerHilPerfTests(unittest.TestCase):
    @staticmethod
    def ack_payload() -> bytes:
        return bytes([1, 0, 2, 0xE8, 0x03, 0])

    @staticmethod
    def data_payload(
        sample_index: int = 0,
        sample_count: int = 1,
        channel_mask: int = 1,
        compression: int = runner.COMPRESSION_NONE,
        sample_payload: bytes = b"\x01",
    ) -> bytes:
        return bytes([
            sample_index & 0xFF,
            (sample_index >> 8) & 0xFF,
            (sample_index >> 16) & 0xFF,
            sample_count & 0xFF,
            (sample_count >> 8) & 0xFF,
            compression,
            channel_mask & 0xFF,
            (channel_mask >> 8) & 0xFF,
        ]) + sample_payload

    @classmethod
    def restart_probe_frames(cls) -> list[bytes]:
        ack = cls.ack_payload()
        return [
            runner.build_frame(runner.FRAME_HELLO_RESP, b"hello", 1),
            runner.build_frame(runner.FRAME_CAPS_RESP, b"caps", 2),
            runner.build_frame(runner.FRAME_CONFIG_RESP, ack, 200),
            runner.build_frame(runner.FRAME_START_RESP, ack, 201),
            runner.build_frame(runner.FRAME_DATA, cls.data_payload(), 9),
            runner.build_frame(runner.FRAME_STOP_RESP, ack, 202),
        ]

    def test_parse_data_meta_uses_exact_8_byte_layout(self) -> None:
        payload = bytes([0x56, 0x34, 0x12, 0x00, 0x04, 0x01, 0xFF, 0x00]) + b"payload"
        meta = runner.parse_data_meta(payload)
        self.assertEqual(meta.sample_index, 0x123456)
        self.assertEqual(meta.sample_count, 1024)
        self.assertEqual(meta.compression, 1)
        self.assertEqual(meta.channel_mask, runner.MASK_FAST8)

    def test_build_config_payload_carries_trigger_type_and_channel(self) -> None:
        payload = runner.build_config_payload(runner.MODE_FAST8, runner.MASK_SINGLE, 1000, 0, 1024, runner.TRIGGER_RISING, 0)
        self.assertEqual(payload[:5], bytes([runner.MODE_FAST8, runner.TRIGGER_RISING, 0, 1, 0]))
        self.assertEqual(len(payload), 12)

    def test_parse_hello_resp_exposes_config_v2_flag(self) -> None:
        hello = runner.parse_hello_resp(bytes([1, runner.SERVER_FLAG_CONFIG_V2, 2, 0x00, 0x40]))
        self.assertEqual(hello["protocol_version"], 1)
        self.assertEqual(hello["server_flags"], runner.SERVER_FLAG_CONFIG_V2)
        self.assertTrue(hello["supports_config_v2"])
        self.assertFalse(hello["supports_generic_packed_burst"])
        self.assertFalse(hello["expected_high_rate_flags_present"])
        self.assertEqual(hello["max_payload_len"], 0x4000)

    def test_parse_hello_resp_exposes_generic_packed_burst_flag(self) -> None:
        flags = runner.SERVER_FLAG_CONFIG_V2 | runner.SERVER_FLAG_GENERIC_PACKED_BURST
        hello = runner.parse_hello_resp(bytes([1, flags, 2, 0x00, 0x40]))
        self.assertEqual(hello["server_flags"], flags)
        self.assertTrue(hello["supports_config_v2"])
        self.assertTrue(hello["supports_generic_packed_burst"])
        self.assertTrue(hello["expected_high_rate_flags_present"])
        self.assertEqual(hello["expected_high_rate_flags"], flags)

    def test_build_config_request_keeps_uint16_on_v1_even_with_v2_support(self) -> None:
        frame_type, payload, encoding = runner.build_config_request(
            runner.MODE_FAST8,
            runner.MASK_SINGLE,
            100000,
            0,
            65535,
            runner.TRIGGER_RISING,
            0,
            True,
        )
        self.assertEqual(frame_type, runner.FRAME_CONFIG_REQ)
        self.assertEqual(encoding, "v1")
        self.assertEqual(payload, bytes([0x01, 0x01, 0x00, 0x01, 0x00, 0xA0, 0x86, 0x01, 0x00, 0x00, 0xFF, 0xFF]))

    def test_build_config_request_preserves_post_zero_sentinel_as_v1(self) -> None:
        frame_type, payload, encoding = runner.build_config_request(
            runner.MODE_WIDE11,
            runner.MASK_WIDE11,
            25000,
            0,
            0,
        )
        self.assertEqual(frame_type, runner.FRAME_CONFIG_REQ)
        self.assertEqual(encoding, "v1")
        self.assertEqual(payload, bytes([0x02, 0x00, 0x00, 0xFF, 0x07, 0xA8, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00]))

    def test_build_config_request_uses_config_v2_for_100000_when_advertised(self) -> None:
        frame_type, payload, encoding = runner.build_config_request(
            runner.MODE_WIDE11,
            runner.MASK_WIDE11,
            100000,
            0,
            100000,
            runner.TRIGGER_EITHER,
            11,
            True,
        )
        self.assertEqual(frame_type, runner.FRAME_CONFIG_V2_REQ)
        self.assertEqual(encoding, "v2")
        self.assertEqual(payload, bytes([0x02, 0x03, 0x0B, 0xFF, 0x07, 0xA0, 0x86, 0x01, 0x00, 0x00, 0x00, 0x00, 0xA0, 0x86, 0x01, 0x00]))

    def test_build_config_request_rejects_large_request_without_v2_flag(self) -> None:
        with self.assertRaisesRegex(ValueError, "require CONFIG_V2"):
            runner.build_config_request(runner.MODE_WIDE11, runner.MASK_WIDE11, 100000, 0, 100000)

    def test_stream_stats_counts_sample_index_gaps(self) -> None:
        stats = runner.StreamStats()
        stats.observe_data(runner.SigrokDataMeta(0, 1024, 1, runner.MASK_FAST8), 1024)
        stats.observe_data(runner.SigrokDataMeta(2048, 1024, 1, runner.MASK_FAST8), 1024)
        self.assertEqual(stats.data_frames, 2)
        self.assertEqual(stats.received_sample_count, 2048)
        self.assertEqual(stats.sample_index_gaps, 1)
        self.assertEqual(stats.first_data_sample_indices, [0, 2048])
        self.assertEqual(stats.first_gap_records, [{"expected": 1024, "actual": 2048}])
        self.assertEqual(stats.payload_bytes, 2048)

    def test_stream_stats_bounds_gap_diagnostics(self) -> None:
        stats = runner.StreamStats()
        for index in range(20):
            stats.observe_data(runner.SigrokDataMeta(index * 2, 1, 1, runner.MASK_FAST8), 1)
        self.assertEqual(stats.sample_index_gaps, 19)
        self.assertEqual(len(stats.first_data_sample_indices), 16)
        self.assertEqual(len(stats.first_gap_records), 16)

    def test_stream_stats_handles_24_bit_sample_index_wrap(self) -> None:
        stats = runner.StreamStats()
        stats.observe_data(runner.SigrokDataMeta(0xFFFF00, 512, 1, runner.MASK_FAST8), 512)
        stats.observe_data(runner.SigrokDataMeta(0x000100, 1024, 1, runner.MASK_FAST8), 1024)
        self.assertEqual(stats.sample_index_gaps, 0)
        self.assertEqual(stats.last_sample_index, 0x0004FF)
        stats.observe_data(runner.SigrokDataMeta(0x000700, 1024, 1, runner.MASK_FAST8), 1024)
        self.assertEqual(stats.sample_index_gaps, 1)

    def test_stream_stats_records_raw_event_details_and_counts_overrun(self) -> None:
        stats = runner.StreamStats()
        stats.observe_event(bytes([1, 0, runner.EVENT_TRIGGERED, 0x10, 0x00, 0x00]))
        stats.observe_event(bytes([1, 0, runner.EVENT_OVERRUN, 0x20, 0x00, 0x00]))
        stats.observe_event(bytes([1, 0, runner.EVENT_ERROR, 0x30, 0x00, 0x00]))

        snapshot = asdict(stats)

        self.assertEqual(stats.triggered_events, 1)
        self.assertEqual(stats.stopped_events, 0)
        self.assertEqual(stats.overrun_events, 1)
        self.assertEqual(stats.error_events, 1)
        self.assertEqual(snapshot["overrun_events"], 1)
        self.assertEqual(snapshot["error_events"], 1)
        self.assertEqual(
            snapshot["event_records"],
            [
                {"type_detail": runner.EVENT_TRIGGERED, "sample_index": 16},
                {"type_detail": runner.EVENT_OVERRUN, "sample_index": 32},
                {"type_detail": runner.EVENT_ERROR, "sample_index": 48},
            ],
        )

    def test_stream_stats_caps_raw_event_records(self) -> None:
        stats = runner.StreamStats()
        for index in range(runner.MAX_STREAM_DIAGNOSTIC_RECORDS + 4):
            stats.observe_event(bytes([1, 0, runner.EVENT_OVERRUN, index, 0x00, 0x00]))

        snapshot = asdict(stats)

        self.assertEqual(stats.overrun_events, runner.MAX_STREAM_DIAGNOSTIC_RECORDS + 4)
        self.assertEqual(len(stats.event_records), runner.MAX_STREAM_DIAGNOSTIC_RECORDS)
        self.assertEqual(len(snapshot["event_records"]), runner.MAX_STREAM_DIAGNOSTIC_RECORDS)
        self.assertEqual(snapshot["event_records"][0], {"type_detail": runner.EVENT_OVERRUN, "sample_index": 0})
        self.assertEqual(snapshot["event_records"][-1], {"type_detail": runner.EVENT_OVERRUN, "sample_index": runner.MAX_STREAM_DIAGNOSTIC_RECORDS - 1})

    def test_stream_stats_records_trigger_event_offset(self) -> None:
        stats = runner.StreamStats()
        stats.observe_event(bytes([1, 0, runner.EVENT_TRIGGERED, 0x10, 0x00, 0x00]))
        stats.observe_data(runner.SigrokDataMeta(0x08, 16, 0, runner.MASK_SINGLE), 16, expected_channel_mask=runner.MASK_SINGLE, sample_bytes=1)
        self.assertEqual(stats.triggered_events, 1)
        self.assertEqual(stats.trigger_sample_index, 0x10)
        self.assertEqual(stats.trigger_sample_offset(), 8)
        self.assertTrue(stats.trigger_sample_offset_valid())

    def test_stream_stats_flags_frame_payload_budget_and_mask_errors(self) -> None:
        stats = runner.StreamStats()
        stats.observe_data(runner.SigrokDataMeta(0, 4, 0, runner.MASK_FAST8), 5, expected_channel_mask=runner.MASK_SINGLE, sample_bytes=1)
        self.assertEqual(stats.payload_over_budget_frames, 1)
        self.assertEqual(stats.channel_mask_mismatch_frames, 1)
        self.assertEqual(stats.first_payload_budget_records, [{"sample_count": 4, "payload_len": 5, "budget": 4}])

    def test_decode_bit_pack_rle_roundtrip_1_and_2_byte_units(self) -> None:
        fast_meta = runner.SigrokDataMeta(0, 8, runner.COMPRESSION_BIT_PACK_RLE, runner.MASK_FAST8)
        fast_payload = bytes([0xFF, 3, 0, 0x00, 2, 0, 0x81, 3, 0])
        self.assertEqual(runner.decode_sigrok_data_payload(fast_meta, fast_payload), b"\xff\xff\xff\x00\x00\x81\x81\x81")

        wide_meta = runner.SigrokDataMeta(0, 5, runner.COMPRESSION_BIT_PACK_RLE, runner.MASK_WIDE11)
        wide_payload = bytes([0x01, 0x08, 3, 0, 0x01, 0x00, 2, 0])
        self.assertEqual(
            runner.decode_sigrok_data_payload(wide_meta, wide_payload),
            bytes([0x01, 0x08, 0x01, 0x08, 0x01, 0x08, 0x01, 0x00, 0x01, 0x00]),
        )

    def test_decode_bit_pack_rle_rejects_malformed_inputs(self) -> None:
        meta = runner.SigrokDataMeta(0, 4, runner.COMPRESSION_BIT_PACK_RLE, runner.MASK_FAST8)
        malformed_payloads = [
            b"\x00\x01",
            b"\x00\x00\x00",
            b"\x00\x05\x00",
            b"\x00\x02\x00",
        ]
        for payload in malformed_payloads:
            with self.assertRaises(ValueError):
                runner.decode_sigrok_data_payload(meta, payload)

    def test_observe_sigrok_data_payload_records_decode_error(self) -> None:
        stats = runner.StreamStats()
        payload = self.data_payload(sample_count=4, compression=runner.COMPRESSION_BIT_PACK_RLE, sample_payload=b"\x00\x00\x00")
        meta = runner.parse_data_meta(payload)
        runner.observe_sigrok_data_payload(stats, meta, payload, expected_channel_mask=runner.MASK_SINGLE, sample_bytes=1)
        self.assertEqual(stats.data_decode_error_frames, 1)
        self.assertEqual(stats.data_frames, 1)

    def test_dry_run_plan_has_all_mandatory_matrices(self) -> None:
        args = runner.parse_args(["--dry-run"])
        plan = runner.build_plan(args)
        names = {matrix["name"] for matrix in plan["matrices"]}
        self.assertIn("sigrok_tcp_bounded_samples", names)
        self.assertIn("sigrok_tcp_continuous_duration", names)
        self.assertIn("sigrok_ws_bounded_samples", names)
        self.assertIn("sigrok_ws_continuous_duration", names)
        self.assertNotIn("http_finite_burst_boundary", names)
        self.assertIn("SINGLE", plan["modes"])
        self.assertIn("FAST8", plan["modes"])
        self.assertIn("WIDE11", plan["modes"])
        self.assertEqual(plan["bounded_triggers"], [{"type": runner.TRIGGER_NONE, "name": "none", "channel": 0}])

    def test_runner_contains_no_http_logic_analyzer_protocol(self) -> None:
        source = RUNNER_PATH.read_text(encoding="utf-8")
        forbidden = [
            "/api/v1/logic-analyzer",
            "HTTP_MAX_TIMEPOINTS",
            "probe_http_logic_analyzer_endpoint",
            "http_capture_case",
            "poll_http_capture",
            "http_finite_burst",
        ]
        for token in forbidden:
            self.assertNotIn(token, source)

    def test_all_matrix_default_is_authoritative_ws_tcp_only(self) -> None:
        args = runner.parse_args([])
        self.assertEqual(args.matrix, ["tcp-bounded", "tcp-continuous", "ws-bounded", "ws-continuous"])

    def test_http_logic_analyzer_cli_options_are_rejected(self) -> None:
        with self.assertRaises(SystemExit):
            runner.parse_args(["--matrix", "http"])
        with self.assertRaises(SystemExit):
            runner.parse_args(["--http-rates-hz", "1000000"])
        with self.assertRaises(SystemExit):
            runner.parse_args(["--http-samples", "512"])

    def test_single_case_uses_fast8_protocol_mode_with_single_bit_mask(self) -> None:
        single = runner.MODE_CASES["SINGLE"]
        self.assertEqual(single.mode_id, runner.MODE_FAST8)
        self.assertEqual(single.mode_id, 1)
        self.assertEqual(single.mask, 0x0001)
        self.assertEqual(single.pins, [10])

    def test_sparse_fast8_case_uses_exact_selected_mask_and_sample_bytes(self) -> None:
        sparse = runner.MODE_CASES["FAST8_SPARSE_GP10_GP13_GP17"]
        self.assertEqual(sparse.mode_id, runner.MODE_FAST8)
        self.assertEqual(sparse.mask, 0x0089)
        self.assertEqual(sparse.pins, [10, 13, 17])
        self.assertEqual(sparse.sample_bytes, 1)
        self.assertEqual(runner.sigrok_expected_sample_bytes(sparse.mask), 1)

    def test_tcp_request_wait_skips_event_before_config_response(self) -> None:
        ack = bytes([1, 0, 2, 0xE8, 0x03, 0])
        sock = FakeSocket([
            runner.build_frame(runner.FRAME_EVENT, bytes([0, 0, 4]), 99),
            runner.build_frame(runner.FRAME_CONFIG_RESP, ack, 200),
        ])
        header, payload, diagnostics = runner.sigrok_send_request_wait_response(sock, runner.FRAME_CONFIG_REQ, b"cfg", 200, runner.FRAME_CONFIG_RESP, 1.0)
        self.assertEqual(header.frame_type, runner.FRAME_CONFIG_RESP)
        self.assertEqual(header.frame_id, 200)
        self.assertEqual(payload, ack)
        self.assertEqual(diagnostics["skipped_events"], 1)
        self.assertEqual(diagnostics["skipped_data_frames"], 0)

    def test_tcp_partial_data_frame_after_deadline_keeps_stop_response_aligned(self) -> None:
        # Given
        data_payload = b"\xff" + b"\x00" * 9
        data_frame = runner.build_frame(runner.FRAME_DATA, data_payload, 41)
        stop_frame = runner.build_frame(runner.FRAME_STOP_RESP, self.ack_payload(), 42)
        chunks = iter([
            data_frame[:runner.SIGROK_HEADER_BYTES],
            data_frame[runner.SIGROK_HEADER_BYTES:runner.SIGROK_HEADER_BYTES + 1],
            data_frame[runner.SIGROK_HEADER_BYTES + 1:],
            stop_frame[:runner.SIGROK_HEADER_BYTES],
            stop_frame[runner.SIGROK_HEADER_BYTES:],
        ])
        sock = FakeSocket([])
        current_time = 0.0

        def monotonic() -> float:
            return current_time

        def recv(byte_count: int) -> bytes:
            nonlocal current_time
            chunk = next(chunks)
            self.assertLessEqual(len(chunk), byte_count)
            if len(chunk) == 1:
                current_time = 1.0
            return chunk

        # When
        with mock.patch.object(runner.time, "monotonic", side_effect=monotonic), mock.patch.object(sock, "recv", side_effect=recv):
            try:
                data_frame = runner.recv_frame(sock, 0.25)
            except TimeoutError:
                data_frame = None
            stop = runner.sigrok_stop(sock, 1.0, 42)

        # Then
        self.assertTrue(stop["received"], stop)
        if data_frame is None:
            self.fail("DATA frame was discarded after its read started")
        header, payload = data_frame
        self.assertEqual(header.frame_type, runner.FRAME_DATA)
        self.assertEqual(payload, data_payload)
        self.assertTrue(stop["sent"], stop)
        self.assertEqual(stop["frame_type"], runner.FRAME_STOP_RESP)

    def test_ws_request_wait_skips_event_before_config_response(self) -> None:
        ack = bytes([1, 0, 2, 0xE8, 0x03, 0])
        ws_conn = FakeWsConnection([
            runner.build_frame(runner.FRAME_EVENT, bytes([0, 0, 4]), 99),
            runner.build_frame(runner.FRAME_CONFIG_RESP, ack, 200),
        ])
        header, payload, diagnostics = runner.ws_send_request_wait_response(ws_conn, FakeWebSocketModule, runner.FRAME_CONFIG_REQ, b"cfg", 200, runner.FRAME_CONFIG_RESP, 1.0)
        self.assertEqual(header.frame_type, runner.FRAME_CONFIG_RESP)
        self.assertEqual(header.frame_id, 200)
        self.assertEqual(payload, ack)
        self.assertEqual(diagnostics["skipped_events"], 1)
        self.assertEqual(diagnostics["skipped_data_frames"], 0)

    def test_strict_start_wait_helper_allows_zero_skips_and_other_frames(self) -> None:
        self.assertIsNone(runner.strict_start_wait_failure_reason({"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 2}))

    def test_strict_start_wait_helper_rejects_data_or_event_before_start(self) -> None:
        data_reason = runner.strict_start_wait_failure_reason({"skipped_data_frames": 1, "skipped_events": 0, "skipped_other_frames": 0})
        self.assertIsNotNone(data_reason)
        self.assertIn("DATA/EVENT arrived before START_RESP", data_reason)
        self.assertIn("skipped_data_frames=1", data_reason)
        self.assertIn("skipped_events=0", data_reason)

        event_reason = runner.strict_start_wait_failure_reason({"skipped_data_frames": 0, "skipped_events": 1, "skipped_other_frames": 3})
        self.assertIsNotNone(event_reason)
        self.assertIn("DATA/EVENT arrived before START_RESP", event_reason)
        self.assertIn("skipped_events=1", event_reason)
        self.assertIn("skipped_other_frames=3", event_reason)

    def test_tcp_capture_case_fails_when_start_wait_skips_data(self) -> None:
        fake_sock = FakeSocket([])
        ack = self.ack_payload()
        start_wait = {"skipped_data_frames": 1, "skipped_events": 0, "skipped_other_frames": 0}
        with mock.patch.object(runner.socket, "create_connection", return_value=fake_sock), \
            mock.patch.object(runner, "sigrok_handshake", return_value={"hello_payload_bytes": 5, "caps_payload_bytes": 4, "hello_wait": {}, "caps_wait": {}}), \
            mock.patch.object(runner, "sigrok_send_request_wait_response", side_effect=[
                (runner.SigrokHeader(runner.FRAME_CONFIG_RESP, 3, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_START_RESP, 4, len(ack)), ack, start_wait),
            ]):
            result = runner.sigrok_capture_case("host", 5556, "http://board", "SINGLE", 1000, 0, 1024, None, 1.0)
        self.assertFalse(result["pass"])
        self.assertIn("DATA/EVENT arrived before START_RESP", result["reason"])
        self.assertEqual(result["start_wait"], start_wait)

    def test_ws_capture_case_fails_when_start_wait_skips_event(self) -> None:
        fake_ws = FakeWsConnection([])

        class WsModule:
            ABNF = FakeWebSocketModule.ABNF

            @staticmethod
            def create_connection(url: str, timeout: float) -> FakeWsConnection:
                return fake_ws

        ack = self.ack_payload()
        start_wait = {"skipped_data_frames": 0, "skipped_events": 1, "skipped_other_frames": 2}
        with mock.patch.object(runner, "load_websocket_module", return_value=WsModule()), \
            mock.patch.object(runner, "create_live_session_ws_url", return_value="ws://example"), \
            mock.patch.object(runner, "ws_send_request_wait_response", side_effect=[
                (runner.SigrokHeader(runner.FRAME_HELLO_RESP, 1, 5), b"hello", {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_CAPS_RESP, 2, 4), b"caps", {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_CONFIG_RESP, 3, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_START_RESP, 4, len(ack)), ack, start_wait),
            ]):
            result = runner.sigrok_ws_capture_case("http://board", "SINGLE", 1000, 0, 1024, None, 1.0)
        self.assertFalse(result["pass"])
        self.assertIn("DATA/EVENT arrived before START_RESP", result["reason"])
        self.assertEqual(result["start_wait"], start_wait)

    def test_tcp_capture_case_records_stream_disconnect_error_details(self) -> None:
        fake_sock = FakeSocket([])
        ack = self.ack_payload()
        with (
            mock.patch.object(runner.socket, "create_connection", return_value=fake_sock),
            mock.patch.object(runner, "sigrok_handshake", return_value={"hello_payload_bytes": 5, "caps_payload_bytes": 4, "hello_wait": {}, "caps_wait": {}}),
            mock.patch.object(runner, "sigrok_send_request_wait_response", side_effect=[
                (runner.SigrokHeader(runner.FRAME_CONFIG_RESP, 3, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_START_RESP, 4, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
            ]),
            mock.patch.object(runner, "recv_frame", side_effect=ConnectionError("stream_disconnect_error")),
            mock.patch.object(runner, "sigrok_stop", return_value={"received": True, "sent": True, "frame_type": runner.FRAME_STOP_RESP}),
            mock.patch.object(runner, "sigrok_fresh_restart_probe", return_value={"ok": True}),
            mock.patch.object(runner, "board_health", return_value={"ok": True}),
        ):
            result = runner.sigrok_capture_case("host", 5556, "http://board", "SINGLE", 1000, 0, 1024, None, 1.0)
        self.assertFalse(result["pass"])
        self.assertEqual(result["reason"], "no DATA frames received")
        self.assertEqual(result["stream_disconnect_error"], {"type": "ConnectionError", "message": "stream_disconnect_error"})
        self.assertEqual(result["stats"]["disconnects"], 1)

    def test_tcp_capture_case_stops_on_server_overrun_without_stop_req_or_disconnect_wait(self) -> None:
        fake_sock = FakeSocket([])
        ack = self.ack_payload()
        with (
            mock.patch.object(runner.socket, "create_connection", return_value=fake_sock),
            mock.patch.object(runner, "sigrok_handshake", return_value={"hello_payload_bytes": 5, "caps_payload_bytes": 4, "hello_wait": {}, "caps_wait": {}}),
            mock.patch.object(runner, "sigrok_send_request_wait_response", side_effect=[
                (runner.SigrokHeader(runner.FRAME_CONFIG_RESP, 3, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_START_RESP, 4, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
            ]),
            mock.patch.object(runner, "recv_frame", side_effect=[
                (runner.SigrokHeader(runner.FRAME_EVENT, 10, 6), bytes([1, 0, runner.EVENT_OVERRUN, 0x00, 0x80, 0x01])),
                AssertionError("should stop after server terminal"),
            ]),
            mock.patch.object(runner, "sigrok_stop", return_value={"received": True, "sent": True, "frame_type": runner.FRAME_STOP_RESP}) as stop_mock,
            mock.patch.object(runner, "sigrok_fresh_restart_probe", return_value={"ok": True}),
            mock.patch.object(runner, "board_health", return_value={"ok": True}),
        ):
            result = runner.sigrok_capture_case("host", 5556, "http://board", "SINGLE", 1000, 0, 0, 1.0, 1.0)
        self.assertTrue(result["pass"], result["reason"])
        self.assertEqual(result["terminal_reason"], "server_overrun")
        self.assertTrue(result["server_auto_stopped"])
        self.assertFalse(result["client_stop_sent"])
        self.assertFalse(result["client_stopped"])
        self.assertEqual(result["stats"]["overrun_events"], 1)
        self.assertTrue(result["capacity_stop_before_data"])
        self.assertFalse(result["continuous_data_observed"])
        self.assertNotIn("stream_disconnect_error", result)
        stop_mock.assert_not_called()

    def test_ws_capture_case_records_stream_disconnect_error_details(self) -> None:
        fake_ws = FakeWsConnection([])

        class WsModule:
            ABNF = FakeWebSocketModule.ABNF

            @staticmethod
            def create_connection(url: str, timeout: float) -> FakeWsConnection:
                return fake_ws

        ack = self.ack_payload()
        with (
            mock.patch.object(runner, "load_websocket_module", return_value=WsModule()),
            mock.patch.object(runner, "create_live_session_ws_url", return_value="ws://example"),
            mock.patch.object(runner, "ws_send_request_wait_response", side_effect=[
                (runner.SigrokHeader(runner.FRAME_HELLO_RESP, 1, 5), b"hello", {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_CAPS_RESP, 2, 4), b"caps", {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_CONFIG_RESP, 3, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_START_RESP, 4, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
            ]),
            mock.patch.object(runner, "ws_recv_frame", side_effect=RuntimeError("stream_disconnect_error")),
            mock.patch.object(runner, "ws_sigrok_stop", return_value={"received": True, "sent": True, "frame_type": runner.FRAME_STOP_RESP}),
            mock.patch.object(runner, "ws_sigrok_fresh_restart_probe", return_value={"ok": True}),
            mock.patch.object(runner, "board_health", return_value={"ok": True}),
        ):
            result = runner.sigrok_ws_capture_case("http://board", "SINGLE", 1000, 0, 1024, None, 1.0)
        self.assertFalse(result["pass"])
        self.assertEqual(result["reason"], "no DATA frames received")
        self.assertEqual(result["stream_disconnect_error"], {"type": "RuntimeError", "message": "stream_disconnect_error"})
        self.assertEqual(result["stats"]["disconnects"], 1)

    def test_ws_capture_case_stops_on_server_overrun_without_stop_req_or_disconnect_wait(self) -> None:
        fake_ws = FakeWsConnection([])

        class WsModule:
            ABNF = FakeWebSocketModule.ABNF

            @staticmethod
            def create_connection(url: str, timeout: float) -> FakeWsConnection:
                return fake_ws

        ack = self.ack_payload()
        data_payload = self.data_payload(sample_index=0, sample_count=1, sample_payload=b"\x01")
        with (
            mock.patch.object(runner, "load_websocket_module", return_value=WsModule()),
            mock.patch.object(runner, "create_live_session_ws_url", return_value="ws://example"),
            mock.patch.object(runner, "ws_send_request_wait_response", side_effect=[
                (runner.SigrokHeader(runner.FRAME_HELLO_RESP, 1, 5), b"hello", {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_CAPS_RESP, 2, 4), b"caps", {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_CONFIG_RESP, 3, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_START_RESP, 4, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
            ]),
            mock.patch.object(runner, "ws_recv_frame", side_effect=[
                (runner.SigrokHeader(runner.FRAME_DATA, 9, len(data_payload)), data_payload),
                (runner.SigrokHeader(runner.FRAME_EVENT, 10, 6), bytes([1, 0, runner.EVENT_OVERRUN, 0x00, 0x80, 0x01])),
                AssertionError("should stop after server terminal"),
            ]),
            mock.patch.object(runner, "ws_sigrok_stop", return_value={"received": True, "sent": True, "frame_type": runner.FRAME_STOP_RESP}) as stop_mock,
            mock.patch.object(runner, "ws_sigrok_fresh_restart_probe", return_value={"ok": True}),
            mock.patch.object(runner, "board_health", return_value={"ok": True}),
        ):
            result = runner.sigrok_ws_capture_case("http://board", "SINGLE", 1000, 0, 0, 1.0, 1.0)
        self.assertTrue(result["pass"], result["reason"])
        self.assertEqual(result["terminal_reason"], "server_overrun")
        self.assertTrue(result["server_auto_stopped"])
        self.assertFalse(result["client_stop_sent"])
        self.assertFalse(result["client_stopped"])
        self.assertEqual(result["stats"]["overrun_events"], 1)
        self.assertFalse(result["capacity_stop_before_data"])
        self.assertTrue(result["continuous_data_observed"])
        self.assertNotIn("stream_disconnect_error", result)
        stop_mock.assert_not_called()

    def test_tcp_fresh_restart_probe_fails_when_start_wait_skips_data(self) -> None:
        ack = self.ack_payload()
        frames = [
            runner.build_frame(runner.FRAME_HELLO_RESP, b"hello", 1),
            runner.build_frame(runner.FRAME_CAPS_RESP, b"caps", 2),
            runner.build_frame(runner.FRAME_CONFIG_RESP, ack, 200),
            runner.build_frame(runner.FRAME_DATA, self.data_payload(), 9),
            runner.build_frame(runner.FRAME_START_RESP, ack, 201),
        ]
        fake_sock = FakeSocket(frames)
        with mock.patch.object(runner.socket, "create_connection", return_value=fake_sock):
            result = runner.sigrok_fresh_restart_probe("host", 5556, runner.MODE_FAST8, runner.MASK_SINGLE, 1000, 1.0)
        self.assertFalse(result["ok"])
        self.assertIn("DATA/EVENT arrived before START_RESP", result["reason"])
        self.assertEqual(result["start_wait"]["skipped_data_frames"], 1)

    def test_ws_fresh_restart_probe_fails_when_start_wait_skips_event(self) -> None:
        ack = self.ack_payload()
        fake_ws = FakeWsConnection([
            runner.build_frame(runner.FRAME_HELLO_RESP, b"hello", 1),
            runner.build_frame(runner.FRAME_CAPS_RESP, b"caps", 2),
            runner.build_frame(runner.FRAME_CONFIG_RESP, ack, 200),
            runner.build_frame(runner.FRAME_EVENT, bytes([0, 0, 4]), 99),
            runner.build_frame(runner.FRAME_START_RESP, ack, 201),
        ])

        class WsModule:
            ABNF = FakeWebSocketModule.ABNF

            @staticmethod
            def create_connection(url: str, timeout: float) -> FakeWsConnection:
                return fake_ws

        with mock.patch.object(runner, "create_live_session_ws_url", return_value="ws://fresh-session"), \
            mock.patch.object(runner, "load_websocket_module", return_value=WsModule()):
            result = runner.ws_sigrok_fresh_restart_probe("http://board", WsModule(), runner.MODE_FAST8, runner.MASK_SINGLE, 1000, 1.0)
        self.assertFalse(result["ok"])
        self.assertIn("DATA/EVENT arrived before START_RESP", result["reason"])
        self.assertEqual(result["start_wait"]["skipped_events"], 1)

    def test_ws_recv_frame_returns_coalesced_inner_frames_successively(self) -> None:
        first = runner.build_frame(runner.FRAME_DATA, self.data_payload(sample_index=0), 10)
        second = runner.build_frame(runner.FRAME_DATA, self.data_payload(sample_index=1), 11)
        ws_conn = FakeWsConnection([first + second])

        first_header, first_payload = runner.ws_recv_frame(ws_conn, 1.0)
        second_header, second_payload = runner.ws_recv_frame(ws_conn, 1.0)

        self.assertEqual(first_header.frame_type, runner.FRAME_DATA)
        self.assertEqual(first_header.frame_id, 10)
        self.assertEqual(first_payload, self.data_payload(sample_index=0))
        self.assertEqual(second_header.frame_type, runner.FRAME_DATA)
        self.assertEqual(second_header.frame_id, 11)
        self.assertEqual(second_payload, self.data_payload(sample_index=1))
        self.assertEqual(ws_conn.recv_calls, 1)

    def test_ws_recv_frame_returns_coalesced_compressed_data_frames(self) -> None:
        compressed_first = self.data_payload(
            sample_index=0,
            sample_count=8,
            channel_mask=runner.MASK_FAST8,
            compression=runner.COMPRESSION_BIT_PACK_RLE,
            sample_payload=bytes([0x00, 8, 0]),
        )
        compressed_second = self.data_payload(
            sample_index=8,
            sample_count=8,
            channel_mask=runner.MASK_FAST8,
            compression=runner.COMPRESSION_BIT_PACK_RLE,
            sample_payload=bytes([0xFF, 8, 0]),
        )
        first = runner.build_frame(runner.FRAME_DATA, compressed_first, 10)
        second = runner.build_frame(runner.FRAME_DATA, compressed_second, 11)
        ws_conn = FakeWsConnection([first + second])

        stats = runner.StreamStats()
        first_header, first_payload = runner.ws_recv_frame(ws_conn, 1.0)
        first_meta = runner.parse_data_meta(first_payload)
        runner.observe_sigrok_data_payload(stats, first_meta, first_payload, expected_channel_mask=runner.MASK_FAST8, sample_bytes=1)
        second_header, second_payload = runner.ws_recv_frame(ws_conn, 1.0)
        second_meta = runner.parse_data_meta(second_payload)
        runner.observe_sigrok_data_payload(stats, second_meta, second_payload, expected_channel_mask=runner.MASK_FAST8, sample_bytes=1)

        self.assertEqual(first_header.frame_id, 10)
        self.assertEqual(second_header.frame_id, 11)
        self.assertEqual(stats.data_decode_error_frames, 0)
        self.assertEqual(stats.received_sample_count, 16)
        self.assertEqual(stats.sample_index_gaps, 0)
        self.assertEqual(ws_conn.recv_calls, 1)

    def test_tcp_fresh_restart_probe_opens_new_socket(self) -> None:
        fake_sock = FakeSocket(self.restart_probe_frames())
        with mock.patch.object(runner.socket, "create_connection", return_value=fake_sock) as create_connection:
            result = runner.sigrok_fresh_restart_probe("host", 5556, runner.MODE_FAST8, runner.MASK_SINGLE, 1000, 1.0)
        self.assertTrue(result["ok"], result.get("reason"))
        self.assertTrue(result["fresh_connection"])
        self.assertTrue(fake_sock.closed)
        create_connection.assert_called_once_with(("host", 5556), timeout=1.0)

    def test_ws_fresh_restart_probe_creates_new_live_session(self) -> None:
        fake_ws = FakeWsConnection(self.restart_probe_frames())
        created_urls: list[str] = []

        class WsModule:
            ABNF = FakeWebSocketModule.ABNF

            @staticmethod
            def create_connection(url: str, timeout: float) -> FakeWsConnection:
                created_urls.append(url)
                return fake_ws

        with mock.patch.object(runner, "create_live_session_ws_url", return_value="ws://fresh-session") as create_session:
            result = runner.ws_sigrok_fresh_restart_probe("http://board", WsModule, runner.MODE_FAST8, runner.MASK_SINGLE, 1000, 1.0)
        self.assertTrue(result["ok"], result.get("reason"))
        self.assertTrue(result["fresh_session"])
        self.assertEqual(created_urls, ["ws://fresh-session"])
        self.assertTrue(fake_ws.closed)
        create_session.assert_called_once_with("http://board", 1.0)

    def test_ws_matrix_plan_cases_can_be_selected(self) -> None:
        args = runner.parse_args(["--dry-run", "--matrix", "ws-bounded", "--matrix", "ws-continuous", "--modes", "FAST8", "--tcp-rates-khz", "1000", "--tcp-post-samples", "1024"])
        plan = runner.build_plan(args)
        names = [matrix["name"] for matrix in plan["matrices"]]
        self.assertEqual(names, ["sigrok_ws_bounded_samples", "sigrok_ws_continuous_duration"])

    def test_high_rate_packed_burst_plan_includes_required_ws_tcp_cases(self) -> None:
        args = runner.parse_args(["--dry-run", "--matrix", "high-rate-packed-burst"])
        plan = runner.build_plan(args)
        self.assertEqual(args.matrix, ["high-rate-packed-burst"])
        self.assertEqual(plan["high_rate_capability_flags"]["required_mask"], runner.SERVER_EXPECTED_HIGH_RATE_FLAGS)
        matrix = plan["matrices"][0]
        self.assertEqual(matrix["name"], "sigrok_high_rate_packed_burst")
        cases = matrix["cases"]
        transports = {case["transport"] for case in cases}
        self.assertEqual(transports, {"tcp", "websocket"})
        self.assertEqual(sum(1 for case in cases if case.get("case_kind") == "hello_flags"), 2)

        single_bounded = [case for case in cases if case.get("mode") == "SINGLE" and case.get("sigrok_post_samples") in (513, 65535, 65536, 100000)]
        self.assertTrue(any(case["transport"] == "tcp" and case["requested_samplerate_khz"] == 100000 and case["sigrok_post_samples"] == 513 and case["trigger"]["type"] == runner.TRIGGER_NONE for case in single_bounded))
        self.assertTrue(any(case["transport"] == "websocket" and case["requested_samplerate_khz"] == 125000 and case["sigrok_post_samples"] == 100000 and case["trigger"]["type"] == runner.TRIGGER_EITHER for case in single_bounded))

        sparse = next(case for case in cases if case.get("mode") == "FAST8_SPARSE_GP10_GP13_GP17")
        self.assertEqual(sparse["channel_mask"], 0x0089)
        self.assertEqual(sparse["mapping_bits"], {"GP10": 0, "GP13": 3, "GP17": 7})
        self.assertIn("expected-low", sparse["physical_stimulus_scope"])

        wide_reject = next(case for case in cases if case.get("mode") == "WIDE11" and case.get("requested_samplerate_khz") == 125000)
        self.assertTrue(wide_reject["expect_rejection"])
        capacity = next(case for case in cases if case.get("capture_semantics") == "continuous_until_capacity_exact_data_then_normal_stopped")
        self.assertEqual(capacity["sigrok_post_samples"], 0)
        self.assertEqual(capacity["target_data_samples"], 100000)
        manual = next(case for case in cases if case.get("capture_semantics") == "manual_stop_lower_rate_distinguished_from_capacity_stop")
        self.assertEqual(manual["manual_stop_after_s"], 0.25)

    def test_high_rate_packed_burst_does_not_change_all_default(self) -> None:
        args = runner.parse_args([])
        self.assertNotIn("high-rate-packed-burst", args.matrix)

    def test_bounded_trigger_plan_adds_trigger_dimension_without_continuous_trigger(self) -> None:
        args = runner.parse_args([
            "--dry-run",
            "--matrix",
            "ws-bounded",
            "--matrix",
            "ws-continuous",
            "--modes",
            "SINGLE",
            "--tcp-rates-khz",
            "1000",
            "--tcp-post-samples",
            "1024",
            "--continuous-durations-s",
            "5",
            "--trigger-types",
            "rising,falling",
            "--trigger-channel",
            "0",
        ])
        plan = runner.build_plan(args)
        bounded = next(matrix for matrix in plan["matrices"] if matrix["name"] == "sigrok_ws_bounded_samples")
        continuous = next(matrix for matrix in plan["matrices"] if matrix["name"] == "sigrok_ws_continuous_duration")
        self.assertEqual(len(bounded["cases"]), 2)
        self.assertEqual({case[4] for case in bounded["cases"]}, {runner.TRIGGER_RISING, runner.TRIGGER_FALLING})
        self.assertEqual(continuous["cases"], [("SINGLE", 1000, 5.0)])
        self.assertIn("TRIGGER_NONE", plan["continuous_trigger_note"])

    def test_uart_stimulus_requires_exact_device(self) -> None:
        args = argparse.Namespace(uart_stimulus="x", uart_device="/dev/ttyACM0", uart_baud=115200, timeout=5)
        with self.assertRaises(ValueError):
            runner.perform_uart_stimulus(args)

    def test_uart_stimulus_retries_transient_ebusy_then_succeeds(self) -> None:
        args = argparse.Namespace(uart_stimulus="UU", uart_device="/dev/ttyACM1", uart_baud=115200, timeout=5)
        created: list[object] = []

        class FakeSerial:
            calls = 0

            def __init__(self, **kwargs: object) -> None:
                type(self).calls += 1
                self.kwargs = kwargs
                self.closed = False
                self.payload = b""
                self.flushed = False
                created.append(self)
                if type(self).calls <= 2:
                    raise OSError(errno.EBUSY, "Device or resource busy")

            def __enter__(self) -> "FakeSerial":
                return self

            def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
                self.closed = True

            def write(self, payload: bytes) -> int:
                self.payload = payload
                return len(payload)

            def flush(self) -> None:
                self.flushed = True

        fake_serial_module = types.SimpleNamespace(Serial=FakeSerial, EIGHTBITS=8, PARITY_NONE="N", STOPBITS_ONE=1)
        with (
            mock.patch.object(runner.importlib, "import_module", return_value=fake_serial_module),
            mock.patch.object(runner.time, "sleep") as sleep,
        ):
            result = runner.perform_uart_stimulus(args)

        self.assertEqual(FakeSerial.calls, 3)
        self.assertEqual(result["attempts"], 3)
        self.assertEqual(result["bytes_written"], 2)
        successful_serial = cast(FakeSerial, created[-1])
        self.assertEqual(successful_serial.kwargs["port"], "/dev/ttyACM1")
        self.assertEqual(successful_serial.payload, b"UU")
        self.assertTrue(successful_serial.flushed)
        self.assertTrue(successful_serial.closed)
        self.assertEqual(sleep.call_count, 2)

    def test_uart_stimulus_non_ebusy_serial_error_fails_immediately(self) -> None:
        args = argparse.Namespace(uart_stimulus="UU", uart_device="/dev/ttyACM1", uart_baud=115200, timeout=5)

        class FakeSerial:
            calls = 0

            def __init__(self, **kwargs: object) -> None:
                type(self).calls += 1
                raise OSError(errno.EACCES, "Permission denied")

        fake_serial_module = types.SimpleNamespace(Serial=FakeSerial, EIGHTBITS=8, PARITY_NONE="N", STOPBITS_ONE=1)
        with (
            mock.patch.object(runner.importlib, "import_module", return_value=fake_serial_module),
            mock.patch.object(runner.time, "sleep") as sleep,
            self.assertRaises(OSError) as raised,
        ):
            runner.perform_uart_stimulus(args)

        self.assertEqual(raised.exception.errno, errno.EACCES)
        self.assertEqual(FakeSerial.calls, 1)
        sleep.assert_not_called()

    def test_uart_ebusy_detection_accepts_errno_and_message_without_pyserial_internals(self) -> None:
        self.assertTrue(runner.exception_matches_ebusy(OSError(errno.EBUSY, "Device or resource busy")))
        self.assertTrue(runner.exception_matches_ebusy(Exception("could not open port: [Errno 16] Device or resource busy")))
        self.assertFalse(runner.exception_matches_ebusy(OSError(errno.EACCES, "Permission denied")))

    def test_sigrok_pass_criteria_require_no_gaps_stop_restart_and_health(self) -> None:
        stats = runner.StreamStats(data_frames=2, received_sample_count=2048, payload_bytes=2048)
        stop = {"received": True, "frame_type": runner.FRAME_STOP_RESP}
        restart = {"ok": True}
        health = {"ok": True}
        passed, reason = runner.evaluate_sigrok_pass(
            bounded=True,
            trigger_required=False,
            stats=stats,
            requested_samples=2048,
            requested_duration_s=None,
            stream_elapsed_s=1.0,
            stop_response=stop,
            immediate_restart=restart,
            board_health_after=health,
        )
        self.assertTrue(passed, reason)
        stats.sample_index_gaps = 1
        passed, reason = runner.evaluate_sigrok_pass(
            bounded=True,
            trigger_required=False,
            stats=stats,
            requested_samples=2048,
            requested_duration_s=None,
            stream_elapsed_s=1.0,
            stop_response=stop,
            immediate_restart=restart,
            board_health_after=health,
        )
        self.assertFalse(passed)
        self.assertEqual(reason, "sample_index gaps detected")

    def test_sigrok_pass_accepts_continuous_overrun_terminal_when_clean_and_restart_ok(self) -> None:
        stats = runner.StreamStats(data_frames=2, received_sample_count=413696, payload_bytes=413696, overrun_events=1)
        stop = {"received": False, "sent": False, "reason": "server terminal"}
        restart = {"ok": True}
        health = {"ok": True}
        passed, reason = runner.evaluate_sigrok_pass(
            bounded=False,
            trigger_required=False,
            stats=stats,
            requested_samples=None,
            requested_duration_s=1.0,
            stream_elapsed_s=1.016,
            stop_response=stop,
            immediate_restart=restart,
            board_health_after=health,
            terminal_reason="server_overrun",
        )
        self.assertTrue(passed, reason)

    def test_sigrok_pass_accepts_continuous_overrun_before_first_data_frame(self) -> None:
        stats = runner.StreamStats(overrun_events=1)
        passed, reason = runner.evaluate_sigrok_pass(
            bounded=False,
            trigger_required=False,
            stats=stats,
            requested_samples=None,
            requested_duration_s=5.0,
            stream_elapsed_s=0.001,
            stop_response={"received": False, "sent": False, "reason": "server terminal"},
            immediate_restart={"ok": True},
            board_health_after={"ok": True},
            terminal_reason="server_overrun",
        )
        self.assertTrue(passed, reason)

    def test_sigrok_pass_rejects_bounded_overrun_before_requested_samples(self) -> None:
        stats = runner.StreamStats(data_frames=2, received_sample_count=95072, payload_bytes=95072, overrun_events=1)
        stop = {"received": False, "sent": False, "reason": "server terminal"}
        restart = {"ok": True}
        health = {"ok": True}
        passed, reason = runner.evaluate_sigrok_pass(
            bounded=True,
            trigger_required=False,
            stats=stats,
            requested_samples=100000,
            requested_duration_s=None,
            stream_elapsed_s=1.0,
            stop_response=stop,
            immediate_restart=restart,
            board_health_after=health,
            terminal_reason="server_overrun",
        )
        self.assertFalse(passed)
        self.assertEqual(reason, "requested pre+post sample count not met")

    def test_sigrok_pass_rejects_server_error_terminal(self) -> None:
        stats = runner.StreamStats(data_frames=1, received_sample_count=1024, payload_bytes=1024, error_events=1)
        stop = {"received": False, "sent": False, "reason": "server terminal"}
        restart = {"ok": True}
        health = {"ok": True}
        passed, reason = runner.evaluate_sigrok_pass(
            bounded=False,
            trigger_required=False,
            stats=stats,
            requested_samples=None,
            requested_duration_s=1.0,
            stream_elapsed_s=0.1,
            stop_response=stop,
            immediate_restart=restart,
            board_health_after=health,
            terminal_reason="server_error",
        )
        self.assertFalse(passed)
        self.assertEqual(reason, "server terminal error received")

    def test_high_rate_pass_criteria_require_exact_samples_stopped_restart_health_and_flags(self) -> None:
        descriptor = runner.high_rate_case_descriptor(
            transport="tcp",
            mode_name="SINGLE",
            samplerate_khz=100000,
            pre_samples=0,
            post_samples=100000,
            trigger_type=runner.TRIGGER_NONE,
            trigger_channel=0,
            semantics="high_rate_bounded_capture",
        )
        descriptor["handshake"] = {"hello": {"expected_high_rate_flags_present": True}}
        stats = runner.StreamStats(data_frames=98, received_sample_count=100000, payload_bytes=100000, stopped_events=1)
        stop = {"received": False, "sent": False, "reason": "not sent"}
        restart = {"ok": True}
        health = {"ok": True, "arena_telemetry": {"memory_available": True}}
        passed, reason = runner.high_rate_evaluate_result(
            descriptor=descriptor,
            stats=stats,
            stop_response=stop,
            immediate_restart=restart,
            board_health_after=health,
            rejected=False,
            rejection_error=None,
        )
        self.assertTrue(passed, reason)

        stats.received_sample_count = 99999
        passed, reason = runner.high_rate_evaluate_result(
            descriptor=descriptor,
            stats=stats,
            stop_response=stop,
            immediate_restart=restart,
            board_health_after=health,
            rejected=False,
            rejection_error=None,
        )
        self.assertFalse(passed)
        self.assertEqual(reason, "exact DATA sample count mismatch")

    def test_high_rate_pass_criteria_distinguish_manual_stop_from_auto_stop(self) -> None:
        descriptor = runner.high_rate_case_descriptor(
            transport="websocket",
            mode_name="SINGLE",
            samplerate_khz=1000,
            pre_samples=0,
            post_samples=0,
            trigger_type=runner.TRIGGER_NONE,
            trigger_channel=0,
            semantics="manual_stop_lower_rate_distinguished_from_capacity_stop",
            manual_stop_after_s=0.25,
        )
        descriptor["handshake"] = {"hello": {"expected_high_rate_flags_present": True}}
        stats = runner.StreamStats(data_frames=1, received_sample_count=1024, payload_bytes=1024, stopped_events=0)
        passed, reason = runner.high_rate_evaluate_result(
            descriptor=descriptor,
            stats=stats,
            stop_response={"received": True, "sent": True, "frame_type": runner.FRAME_STOP_RESP},
            immediate_restart={"ok": True},
            board_health_after={"ok": True},
            rejected=False,
            rejection_error=None,
        )
        self.assertTrue(passed, reason)

        passed, reason = runner.high_rate_evaluate_result(
            descriptor=descriptor,
            stats=stats,
            stop_response={"received": False, "sent": False},
            immediate_restart={"ok": True},
            board_health_after={"ok": True},
            rejected=False,
            rejection_error=None,
        )
        self.assertFalse(passed)
        self.assertEqual(reason, "manual STOP_REQ was not sent")

    def test_high_rate_rejection_case_passes_only_on_rejection(self) -> None:
        descriptor = runner.high_rate_case_descriptor(
            transport="tcp",
            mode_name="WIDE11",
            samplerate_khz=125000,
            pre_samples=0,
            post_samples=100000,
            trigger_type=runner.TRIGGER_NONE,
            trigger_channel=0,
            semantics="wide11_125mhz_rejection",
            expect_rejection=True,
        )
        passed, reason = runner.high_rate_evaluate_result(
            descriptor=descriptor,
            stats=runner.StreamStats(),
            stop_response={},
            immediate_restart={},
            board_health_after={"ok": False},
            rejected=True,
            rejection_error={"error_code": 1, "detail": 2},
        )
        self.assertTrue(passed, reason)
        passed, reason = runner.high_rate_evaluate_result(
            descriptor=descriptor,
            stats=runner.StreamStats(),
            stop_response={},
            immediate_restart={},
            board_health_after={"ok": True},
            rejected=False,
            rejection_error=None,
        )
        self.assertFalse(passed)
        self.assertEqual(reason, "expected rejection did not occur")

    def test_high_rate_evaluate_accepts_manual_stop_server_overrun_as_capacity_stop(self) -> None:
        descriptor = runner.high_rate_case_descriptor(
            transport="tcp",
            mode_name="SINGLE",
            samplerate_khz=1000,
            pre_samples=0,
            post_samples=0,
            trigger_type=runner.TRIGGER_NONE,
            trigger_channel=0,
            semantics="manual_stop_lower_rate_distinguished_from_capacity_stop",
            manual_stop_after_s=0.25,
        )
        descriptor["handshake"] = {"hello": {"expected_high_rate_flags_present": True}}
        stats = runner.StreamStats(data_frames=90, received_sample_count=91104, payload_bytes=91104, overrun_events=1)
        passed, reason = runner.high_rate_evaluate_result(
            descriptor=descriptor,
            stats=stats,
            stop_response={"received": False, "sent": False, "reason": "server terminal before manual STOP_REQ"},
            immediate_restart={"ok": True},
            board_health_after={"ok": True},
            rejected=False,
            rejection_error=None,
            terminal_reason="server_overrun",
        )
        self.assertTrue(passed, reason)

        stats.sample_index_gaps = 1
        passed, reason = runner.high_rate_evaluate_result(
            descriptor=descriptor,
            stats=stats,
            stop_response={"received": False, "sent": False, "reason": "server terminal before manual STOP_REQ"},
            immediate_restart={"ok": True},
            board_health_after={"ok": True},
            rejected=False,
            rejection_error=None,
            terminal_reason="server_overrun",
        )
        self.assertFalse(passed)
        self.assertEqual(reason, "sample_index gaps detected")

    def test_high_rate_evaluate_rejects_bounded_overrun_before_exact_samples(self) -> None:
        descriptor = runner.high_rate_case_descriptor(
            transport="tcp",
            mode_name="SINGLE",
            samplerate_khz=100000,
            pre_samples=0,
            post_samples=100000,
            trigger_type=runner.TRIGGER_NONE,
            trigger_channel=0,
            semantics="high_rate_bounded_capture",
        )
        descriptor["handshake"] = {"hello": {"expected_high_rate_flags_present": True}}
        stats = runner.StreamStats(data_frames=90, received_sample_count=91104, payload_bytes=91104, overrun_events=1)
        passed, reason = runner.high_rate_evaluate_result(
            descriptor=descriptor,
            stats=stats,
            stop_response={"received": False, "sent": False, "reason": "not sent"},
            immediate_restart={"ok": True},
            board_health_after={"ok": True},
            rejected=False,
            rejection_error=None,
            terminal_reason="server_overrun",
        )
        self.assertFalse(passed)
        self.assertEqual(reason, "server overrun before bounded capture completion")

    def test_high_rate_evaluate_rejects_server_error_terminal(self) -> None:
        descriptor = runner.high_rate_case_descriptor(
            transport="websocket",
            mode_name="SINGLE",
            samplerate_khz=1000,
            pre_samples=0,
            post_samples=0,
            trigger_type=runner.TRIGGER_NONE,
            trigger_channel=0,
            semantics="manual_stop_lower_rate_distinguished_from_capacity_stop",
            manual_stop_after_s=0.25,
        )
        descriptor["handshake"] = {"hello": {"expected_high_rate_flags_present": True}}
        stats = runner.StreamStats(data_frames=10, received_sample_count=10240, payload_bytes=10240, error_events=1)
        passed, reason = runner.high_rate_evaluate_result(
            descriptor=descriptor,
            stats=stats,
            stop_response={"received": False, "sent": False, "reason": "server terminal before manual STOP_REQ"},
            immediate_restart={"ok": True},
            board_health_after={"ok": True},
            rejected=False,
            rejection_error=None,
            terminal_reason="server_error",
        )
        self.assertFalse(passed)
        self.assertEqual(reason, "server terminal error received")

    def test_high_rate_tcp_manual_stop_case_stops_on_server_overrun_without_disconnect(self) -> None:
        fake_sock = FakeSocket([])
        ack = self.ack_payload()
        data_payload = self.data_payload(sample_index=0, sample_count=1, sample_payload=b"\x01")
        args = argparse.Namespace(tcp_host="host", tcp_port=5556, http_base="http://board", timeout=1.0, uart_stimulus=None)
        descriptor = runner.high_rate_case_descriptor(
            transport="tcp",
            mode_name="SINGLE",
            samplerate_khz=1000,
            pre_samples=0,
            post_samples=0,
            trigger_type=runner.TRIGGER_NONE,
            trigger_channel=0,
            semantics="manual_stop_lower_rate_distinguished_from_capacity_stop",
            manual_stop_after_s=30.0,
        )
        with (
            mock.patch.object(runner.socket, "create_connection", return_value=fake_sock),
            mock.patch.object(runner, "sigrok_handshake", return_value={"hello": {"expected_high_rate_flags_present": True, "supports_config_v2": True}, "hello_payload_bytes": 5, "caps_payload_bytes": 4, "hello_wait": {}, "caps_wait": {}}),
            mock.patch.object(runner, "sigrok_send_request_wait_response", side_effect=[
                (runner.SigrokHeader(runner.FRAME_CONFIG_RESP, 3, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_START_RESP, 4, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
            ]),
            mock.patch.object(runner, "recv_frame", side_effect=[
                (runner.SigrokHeader(runner.FRAME_DATA, 9, len(data_payload)), data_payload),
                (runner.SigrokHeader(runner.FRAME_EVENT, 10, 6), bytes([1, 0, runner.EVENT_OVERRUN, 0x00, 0x80, 0x01])),
                ConnectionError("peer idle close after overrun"),
            ]),
            mock.patch.object(runner, "sigrok_stop", return_value={"received": True, "sent": True, "frame_type": runner.FRAME_STOP_RESP}) as stop_mock,
            mock.patch.object(runner, "sigrok_fresh_restart_probe", return_value={"ok": True}),
            mock.patch.object(runner, "board_health", return_value={"ok": True}),
        ):
            result = runner.sigrok_high_rate_packed_burst_capture_case(args, descriptor)
        self.assertTrue(result["pass"], result["reason"])
        self.assertEqual(result["terminal_reason"], "server_overrun")
        self.assertTrue(result["server_auto_stopped"])
        self.assertFalse(result["client_stop_sent"])
        self.assertFalse(result["client_stopped"])
        self.assertEqual(result["stats"]["overrun_events"], 1)
        self.assertEqual(result["stats"]["disconnects"], 0)
        self.assertNotIn("stream_disconnect_error", result)
        stop_mock.assert_not_called()

    def test_high_rate_ws_manual_stop_case_stops_on_server_overrun_without_disconnect(self) -> None:
        fake_ws = FakeWsConnection([])

        class WsModule:
            ABNF = FakeWebSocketModule.ABNF

            @staticmethod
            def create_connection(url: str, timeout: float) -> FakeWsConnection:
                return fake_ws

        ack = self.ack_payload()
        hello_payload = bytes([1, runner.SERVER_EXPECTED_HIGH_RATE_FLAGS, 2, 0x00, 0x40])
        caps_payload = b"caps"
        data_payload = self.data_payload(sample_index=0, sample_count=1, sample_payload=b"\x01")
        args = argparse.Namespace(tcp_host="host", tcp_port=5556, http_base="http://board", timeout=1.0, uart_stimulus=None)
        descriptor = runner.high_rate_case_descriptor(
            transport="websocket",
            mode_name="SINGLE",
            samplerate_khz=1000,
            pre_samples=0,
            post_samples=0,
            trigger_type=runner.TRIGGER_NONE,
            trigger_channel=0,
            semantics="manual_stop_lower_rate_distinguished_from_capacity_stop",
            manual_stop_after_s=30.0,
        )
        with (
            mock.patch.object(runner, "load_websocket_module", return_value=WsModule()),
            mock.patch.object(runner, "create_live_session_ws_url", return_value="ws://example"),
            mock.patch.object(runner, "ws_send_request_wait_response", side_effect=[
                (runner.SigrokHeader(runner.FRAME_HELLO_RESP, 1, len(hello_payload)), hello_payload, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_CAPS_RESP, 2, len(caps_payload)), caps_payload, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_CONFIG_RESP, 3, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
                (runner.SigrokHeader(runner.FRAME_START_RESP, 4, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}),
            ]),
            mock.patch.object(runner, "ws_recv_frame", side_effect=[
                (runner.SigrokHeader(runner.FRAME_DATA, 9, len(data_payload)), data_payload),
                (runner.SigrokHeader(runner.FRAME_EVENT, 10, 6), bytes([1, 0, runner.EVENT_OVERRUN, 0x00, 0x80, 0x01])),
                ConnectionError("peer idle close after overrun"),
            ]),
            mock.patch.object(runner, "ws_sigrok_stop", return_value={"received": True, "sent": True, "frame_type": runner.FRAME_STOP_RESP}) as stop_mock,
            mock.patch.object(runner, "ws_sigrok_fresh_restart_probe", return_value={"ok": True}),
            mock.patch.object(runner, "board_health", return_value={"ok": True}),
        ):
            result = runner.sigrok_high_rate_packed_burst_capture_case(args, descriptor)
        self.assertTrue(result["pass"], result["reason"])
        self.assertEqual(result["terminal_reason"], "server_overrun")
        self.assertTrue(result["server_auto_stopped"])
        self.assertFalse(result["client_stop_sent"])
        self.assertFalse(result["client_stopped"])
        self.assertEqual(result["stats"]["overrun_events"], 1)
        self.assertEqual(result["stats"]["disconnects"], 0)
        self.assertNotIn("stream_disconnect_error", result)
        stop_mock.assert_not_called()

    def test_positive_trigger_pass_criteria_require_trigger_event_and_valid_offset(self) -> None:
        stats = runner.StreamStats(data_frames=1, received_sample_count=16, payload_bytes=16, first_sample_index=8, trigger_sample_index=16, triggered_events=1)
        stop = {"received": True, "frame_type": runner.FRAME_STOP_RESP}
        restart = {"ok": True}
        health = {"ok": True}
        passed, reason = runner.evaluate_sigrok_pass(
            bounded=True,
            trigger_required=True,
            stats=stats,
            requested_samples=16,
            requested_duration_s=None,
            stream_elapsed_s=1.0,
            stop_response=stop,
            immediate_restart=restart,
            board_health_after=health,
        )
        self.assertTrue(passed, reason)
        stats.triggered_events = 0
        stats.trigger_sample_index = None
        passed, reason = runner.evaluate_sigrok_pass(
            bounded=True,
            trigger_required=True,
            stats=stats,
            requested_samples=16,
            requested_duration_s=None,
            stream_elapsed_s=1.0,
            stop_response=stop,
            immediate_restart=restart,
            board_health_after=health,
        )
        self.assertFalse(passed)
        self.assertEqual(reason, "trigger event not received")

    def test_sigrok_result_field_names_do_not_claim_auto_stop_for_client_stop(self) -> None:
        stop = {"received": True, "sent": True, "frame_type": runner.FRAME_STOP_RESP}
        stats = runner.StreamStats(data_frames=1, received_sample_count=1024)
        requested_met = stats.received_sample_count >= 1024
        result = {
            "bounded_target_met_before_stop": requested_met,
            "client_stop_sent": bool(stop.get("sent")),
            "client_stopped": bool(stop.get("received")) and stop.get("frame_type") == runner.FRAME_STOP_RESP,
            "server_auto_stopped": stats.stopped_events > 0 and not bool(stop.get("sent")),
        }
        self.assertTrue(result["bounded_target_met_before_stop"])
        self.assertTrue(result["client_stop_sent"])
        self.assertTrue(result["client_stopped"])
        self.assertFalse(result["server_auto_stopped"])

    def test_ws_dependency_error_is_clear(self) -> None:
        with mock.patch.object(runner.importlib, "import_module", side_effect=ImportError("missing")):
            result = runner.sigrok_ws_capture_case("http://board", "FAST8", 1000, 0, 1024, None, 5.0)
        self.assertFalse(result["pass"])
        self.assertTrue(result["dependency_error"])
        self.assertIn("websocket-client", result["reason"])

    def test_stream_rates_use_stream_elapsed_window(self) -> None:
        stats = runner.StreamStats(data_frames=1, received_sample_count=1000, payload_bytes=2000)
        rates = stats.rates(2.0)
        self.assertEqual(rates["effective_receive_samples_per_s"], 500.0)
        self.assertEqual(rates["effective_receive_bytes_per_s"], 1000.0)

    def test_wide11_mapping_validator_accepts_independent_gp10_gp11_gp18_gp20_pattern(self) -> None:
        pattern = list(runner.WIDE11_MAPPING_DEFAULT_PATTERN_NIBBLES)
        hold = 4
        sample_count = len(pattern) * hold * 3
        samples = [runner.wide11_mapping_expected_sample(pattern, hold, index) for index in range(sample_count)]
        payload = b"".join(sample.to_bytes(2, "little") for sample in samples)

        result = runner.validate_wide11_mapping_payload(
            payload,
            pattern_nibbles=pattern,
            hold_samples=hold,
            check_samples=sample_count,
        )

        self.assertTrue(result["pass"], result)
        for bit in ("0", "1", "8", "10"):
            self.assertEqual(result["bit_values"][bit], [0, 1])
            self.assertGreater(result["bit_transitions"][bit], 0)

    def test_wide11_mapping_validator_rejects_malformed_payload(self) -> None:
        with self.assertRaisesRegex(ValueError, "even"):
            runner.validate_wide11_mapping_payload(
                b"\x01",
                pattern_nibbles=list(runner.WIDE11_MAPPING_DEFAULT_PATTERN_NIBBLES),
                hold_samples=4,
                check_samples=1,
            )

    def test_wide11_mapping_validator_rejects_gp20_one_sample_skew(self) -> None:
        pattern = list(runner.WIDE11_MAPPING_DEFAULT_PATTERN_NIBBLES)
        hold = 4
        sample_count = len(pattern) * hold * 3
        expected = [runner.wide11_mapping_expected_sample(pattern, hold, index) for index in range(sample_count)]
        skewed = []
        for index, sample in enumerate(expected):
            previous_gp20 = expected[index - 1] & (1 << 10) if index > 0 else sample & (1 << 10)
            skewed.append((sample & ~(1 << 10)) | previous_gp20)
        payload = b"".join(sample.to_bytes(2, "little") for sample in skewed)

        result = runner.validate_wide11_mapping_payload(
            payload,
            pattern_nibbles=pattern,
            hold_samples=hold,
            check_samples=sample_count,
        )

        self.assertFalse(result["pass"])
        self.assertEqual(result["reason"], "WIDE11 mapping payload mismatches expected pattern")
        self.assertGreater(result["mismatch_count"], 0)

    def test_wide11_mapping_validator_rejects_gp20_mapped_to_bit9(self) -> None:
        pattern = list(runner.WIDE11_MAPPING_DEFAULT_PATTERN_NIBBLES)
        hold = 4
        sample_count = len(pattern) * hold * 2
        wrong = []
        for index in range(sample_count):
            sample = runner.wide11_mapping_expected_sample(pattern, hold, index)
            gp20 = (sample >> 10) & 1
            sample = (sample & ~(1 << 10)) | (gp20 << 9)
            wrong.append(sample)
        payload = b"".join(sample.to_bytes(2, "little") for sample in wrong)

        result = runner.validate_wide11_mapping_payload(
            payload,
            pattern_nibbles=pattern,
            hold_samples=hold,
            check_samples=sample_count,
        )

        self.assertFalse(result["pass"])
        self.assertIn(result["reason"], [
            "WIDE11 mapping payload mismatches expected pattern",
            "WIDE11 mapping pattern did not independently exercise every required bit",
        ])

    def test_wide11_gp10_uart_low_others_validator_accepts_gp10_activity_and_zero_others(self) -> None:
        samples = [0, 0, 1, 1, 0, 0, 1, 1] * 4
        payload = b"".join(sample.to_bytes(2, "little") for sample in samples)

        result = runner.validate_wide11_gp10_uart_low_others_payload(payload, check_samples=len(samples))

        self.assertTrue(result["pass"], result)
        self.assertEqual(result["profile"], "gp10_uart_low_others")
        self.assertEqual(result["zero_mask"], 0x07FE)
        self.assertEqual(result["gp10_values"], [0, 1])
        self.assertTrue(result["gp10_low_high_seen"])
        self.assertEqual(result["zero_mask_violation_count"], 0)
        self.assertIn("GP11/GP18/GP20", result["limitations"][1])

    def test_wide11_gp10_uart_low_others_validator_rejects_crosstalk_on_low_bits(self) -> None:
        samples = [0, 1, 0, 1, 0x0002, 1, 0x0400, 0x0201]
        payload = b"".join(sample.to_bytes(2, "little") for sample in samples)

        result = runner.validate_wide11_gp10_uart_low_others_payload(payload, check_samples=len(samples))

        self.assertFalse(result["pass"])
        self.assertEqual(result["reason"], "WIDE11 GP10 UART low-others check saw unexpected high/crosstalk on bits1..10")
        self.assertEqual(result["zero_mask_violation_count"], 3)
        self.assertEqual(result["first_zero_mask_violations"][0]["unexpected_mask"], 0x0002)

    def test_wide11_gp10_uart_low_others_validator_rejects_missing_gp10_high(self) -> None:
        payload = b"".join((0).to_bytes(2, "little") for _ in range(16))

        result = runner.validate_wide11_gp10_uart_low_others_payload(payload, check_samples=16)

        self.assertFalse(result["pass"])
        self.assertEqual(result["reason"], "WIDE11 GP10 UART low-others check did not observe both low and high runs on bit0")
        self.assertEqual(result["gp10_values"], [0])

    def test_wide11_mapping_cli_parses_external_generator_flags(self) -> None:
        args = runner.parse_args([
            "--dry-run",
            "--matrix",
            "wide11-mapping",
            "--wide11-map-external-generator",
            "--wide11-map-pattern",
            "0x1,0x2,0x4,0x8",
            "--wide11-map-hold-samples",
            "8",
            "--wide11-map-check-samples",
            "128",
        ])
        self.assertEqual(args.matrix, ["wide11-mapping"])
        self.assertTrue(args.wide11_map_external_generator)
        self.assertEqual(args.wide11_map_pattern, [1, 2, 4, 8])
        self.assertEqual(args.wide11_map_hold_samples, 8)
        self.assertEqual(args.wide11_map_check_samples, 128)
        plan = runner.build_plan(args)
        self.assertEqual(plan["matrices"][0]["name"], "sigrok_tcp_wide11_deep_burst_mapping")
        self.assertEqual(plan["matrices"][0]["cases"][0]["mapping_profile"], "external_4bit")
        self.assertEqual(plan["matrices"][0]["cases"][0]["validation_scope"], "multi_channel_external_4bit")
        self.assertTrue(plan["matrices"][0]["cases"][0]["external_generator_acknowledged"])

    def test_wide11_mapping_cli_parses_gp10_uart_low_others_profile(self) -> None:
        args = runner.parse_args([
            "--dry-run",
            "--matrix",
            "wide11-mapping",
            "--wide11-map-gp10-uart-low-others",
            "--wide11-map-check-samples",
            "256",
        ])

        self.assertEqual(args.matrix, ["wide11-mapping"])
        self.assertTrue(args.wide11_map_gp10_uart_low_others)
        self.assertEqual(args.uart_stimulus, runner.WIDE11_MAPPING_GP10_UART_DEFAULT_STIMULUS)
        self.assertEqual(args.uart_device, "/dev/ttyACM1")
        self.assertEqual(args.uart_baud, 115200)
        plan = runner.build_plan(args)
        case = plan["matrices"][0]["cases"][0]
        self.assertEqual(plan["matrices"][0]["name"], "sigrok_tcp_wide11_gp10_uart_low_others")
        self.assertEqual(case["stimulus_profile"], "gp10_uart_low_others")
        self.assertEqual(case["mapping_profile"], "gp10_uart_low_others")
        self.assertEqual(case["validation_scope"], "single_channel_reduced")
        self.assertFalse(case["requires_external_generator"])
        self.assertEqual(case["zero_mask"], 0x07FE)
        self.assertEqual(case["uart_stimulus"]["sent_after"], "START_RESP trigger-safe barrier")

    def test_wide11_mapping_cli_rejects_invalid_pattern_and_check_window(self) -> None:
        with self.assertRaises(SystemExit):
            runner.parse_args(["--matrix", "wide11-mapping", "--wide11-map-pattern", "0x10"])
        with self.assertRaises(SystemExit):
            runner.parse_args(["--matrix", "wide11-mapping", "--wide11-map-check-samples", "100001"])
        with self.assertRaises(SystemExit):
            runner.parse_args(["--matrix", "wide11-mapping", "--wide11-map-external-generator", "--wide11-map-gp10-uart-low-others"])

    def test_wide11_mapping_case_blocks_without_external_generator_ack(self) -> None:
        args = runner.parse_args(["--matrix", "wide11-mapping"])
        result = runner.sigrok_tcp_wide11_mapping_case(args)
        self.assertFalse(result["pass"])
        self.assertTrue(result["blocked"])
        self.assertIn("external-generator", result["reason"])

    def test_wide11_mapping_case_defers_gp10_uart_until_after_start_response(self) -> None:
        args = runner.parse_args(["--matrix", "wide11-mapping", "--wide11-map-gp10-uart-low-others", "--wide11-map-check-samples", "4"])
        ack = self.ack_payload()
        meta_payload = self.data_payload(
            sample_count=4,
            channel_mask=runner.MASK_WIDE11,
            compression=runner.COMPRESSION_NONE,
            sample_payload=b"\x00\x00\x01\x00\x00\x00\x01\x00",
        )
        fake_sock = FakeSocket([])
        calls: list[str] = []

        def fake_send_request(*call_args: object):
            frame_type = call_args[1]
            if frame_type == runner.FRAME_CONFIG_V2_REQ:
                calls.append("config")
                return runner.SigrokHeader(runner.FRAME_CONFIG_RESP, 3, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}
            if frame_type == runner.FRAME_START_REQ:
                calls.append("start")
                return runner.SigrokHeader(runner.FRAME_START_RESP, 4, len(ack)), ack, {"skipped_data_frames": 0, "skipped_events": 0, "skipped_other_frames": 0}
            raise AssertionError(f"unexpected frame_type {frame_type}")

        def fake_uart_stimulus(_args: argparse.Namespace) -> dict[str, object]:
            calls.append("uart")
            return {"bytes_written": 8}

        with (
            mock.patch.object(runner.socket, "create_connection", return_value=fake_sock),
            mock.patch.object(runner, "sigrok_handshake", return_value={"hello": {"supports_config_v2": True}, "hello_payload_bytes": 5, "caps_payload_bytes": 4, "hello_wait": {}, "caps_wait": {}}),
            mock.patch.object(runner, "sigrok_send_request_wait_response", side_effect=fake_send_request),
            mock.patch.object(runner, "recv_frame", side_effect=[
                (runner.SigrokHeader(runner.FRAME_EVENT, 9, 6), bytes([1, 0, runner.EVENT_TRIGGERED, 0, 0, 0])),
                (runner.SigrokHeader(runner.FRAME_DATA, 10, len(meta_payload)), meta_payload),
                TimeoutError("done"),
            ]),
            mock.patch.object(runner, "perform_uart_stimulus", side_effect=fake_uart_stimulus),
            mock.patch.object(runner, "sigrok_stop", return_value={"received": True, "sent": True, "frame_type": runner.FRAME_STOP_RESP}),
            mock.patch.object(runner, "board_health", return_value={"ok": True}),
        ):
            result = runner.sigrok_tcp_wide11_mapping_case(args)

        self.assertEqual(calls, ["config", "start", "uart"])
        self.assertFalse(result["pass"])
        self.assertEqual(result["reason"], "exact WIDE11 deep-burst sample count not received")
        self.assertEqual(result["stimulus_profile"], "gp10_uart_low_others")
        self.assertEqual(result["validation_scope"], "single_channel_reduced")
        self.assertFalse(result["selected_external_4bit_mapping_validated"])
        self.assertTrue(result["mapping_validation"]["pass"])
        self.assertEqual(result["mapping_validation"]["zero_mask"], 0x07FE)
        self.assertTrue(result["gp10_bit0_activity_validated"])
        self.assertTrue(result["low_others_zero_mask_validated"])
        self.assertFalse(result["selected_gp11_gp18_gp20_high_mapping_validated"])
        self.assertIn("not validated", result["validation_limitations"][0])

    def test_telemetry_record_from_message_accepts_single_and_batch_adc(self) -> None:
        single = runner.telemetry_record_from_message({"type": "telemetry", "topic": "adc", "sequence": 7, "sample_sequence": 17, "uptime_us": 100, "device_t_mono_us": 101}, 1.25)
        self.assertEqual(single["sample_sequence"], 17)
        self.assertEqual(single["received_monotonic_s"], 1.25)

        batch = runner.telemetry_record_from_message({
            "type": "telemetry-batch",
            "topic": "adc",
            "dropped_samples": 2,
            "samples": [
                {"sequence": 1, "sample_sequence": 11, "uptime_us": 10, "device_t_mono_us": 11},
                {"sequence": 2, "sample_sequence": 12, "uptime_us": 20, "device_t_mono_us": 21},
            ],
        }, 2.0)
        self.assertEqual(batch["sample_sequence"], 12)
        self.assertEqual(batch["batch_sample_count"], 2)
        self.assertEqual(batch["dropped_samples"], 2)
        self.assertIsNone(runner.telemetry_record_from_message({"type": "snapshot", "topic": "status"}, 0.0))

    def test_telemetry_read_until_flags_binary_and_malformed_without_disconnect(self) -> None:
        ws = FakeJsonWsConnection([
            b"binary",
            "not json",
            '{"type":"telemetry","topic":"adc","sequence":3,"sample_sequence":30,"uptime_us":1,"device_t_mono_us":2}',
        ])
        result = runner.telemetry_read_until(ws, timeout_s=1.0, min_records=1, phase="baseline")
        self.assertEqual(result["record_count"], 1)
        self.assertEqual(result["binary_frames"], 1)
        self.assertEqual(result["malformed_json"], 1)
        self.assertFalse(result["connected"])

    def test_telemetry_overlap_distinguishes_grace_from_pause_pollution(self) -> None:
        ws = FakeJsonWsConnection([
            '{"type":"telemetry","topic":"adc","sequence":1,"sample_sequence":1}',
            TimeoutError("pause"),
            '{"type":"telemetry","topic":"adc","sequence":2,"sample_sequence":2}',
        ])
        worker = mock.Mock()
        states = [True, True, True, False]
        worker.is_alive.side_effect = lambda: states.pop(0) if states else False
        with mock.patch.object(runner.time, "monotonic", side_effect=[0.0, 0.01, 0.02, 0.10, 0.11, 0.12, 0.13, 0.14]):
            result = runner.telemetry_read_during_overlap(ws, start_monotonic_s=0.0, worker=worker, timeout_s=1.0)
        self.assertEqual(result["pre_pause_delivery_grace_count"], 1)
        self.assertEqual(result["pause_record_count"], 1)
        self.assertFalse(result["expected_pause_observed"])

    def test_telemetry_overlap_treats_websocket_timeout_as_connected_no_data(self) -> None:
        ws = FakeJsonWsConnection([
            FakeWebSocketTimeoutException("expected pause"),
            FakeWebSocketTimeoutException("expected pause"),
        ])
        worker = mock.Mock()
        states = [True, True, False]
        worker.is_alive.side_effect = lambda: states.pop(0) if states else False
        result = runner.telemetry_read_during_overlap(ws, start_monotonic_s=0.0, worker=worker, timeout_s=1.0)
        self.assertTrue(result["connected"])
        self.assertTrue(result["expected_pause_observed"])
        self.assertEqual(result["pause_record_count"], 0)
        self.assertEqual(result["binary_frames"], 0)
        self.assertEqual(result["malformed_json"], 0)
        self.assertEqual(result["recv_timeouts"], 2)
        self.assertEqual(result["errors"], [])

    def test_telemetry_overlap_uses_sequence_reset_as_inferred_release(self) -> None:
        grace_frames: list[object] = [
            f'{{"type":"telemetry","topic":"adc","sequence":{seq},"sample_sequence":{seq},"uptime_us":{1000 + seq},"device_t_mono_us":{1000 + seq}}}'
            for seq in range(13, 26)
        ]
        reset_frames: list[object] = [
            f'{{"type":"telemetry","topic":"adc","sequence":{seq},"sample_sequence":{seq},"uptime_us":{2000 + seq},"device_t_mono_us":{2000 + seq}}}'
            for seq in range(1, 10)
        ]
        ws = FakeJsonWsConnection(grace_frames + reset_frames)
        worker = mock.Mock()
        states = [True] * 22 + [False]
        worker.is_alive.side_effect = lambda: states.pop(0) if states else False
        receive_times = [0.01 + index * 0.003 for index in range(13)] + [0.06 + index * 0.003 for index in range(9)]
        monotonic_values = [0.0]
        for receive_time in receive_times:
            monotonic_values.extend([max(0.0, receive_time - 0.001), receive_time])
        monotonic_values.append(0.09)
        with mock.patch.object(runner.time, "monotonic", side_effect=monotonic_values):
            result = runner.telemetry_read_during_overlap(
                ws,
                start_monotonic_s=0.0,
                worker=worker,
                timeout_s=1.0,
                baseline_sequence=12,
                baseline_device_time_us=1000,
            )
        self.assertTrue(result["connected"])
        self.assertTrue(result["expected_pause_observed"])
        self.assertEqual(result["pre_pause_delivery_grace_count"], 13)
        self.assertEqual(result["old_epoch_pause_record_count"], 0)
        self.assertEqual(result["reset_epoch_record_count"], 9)
        self.assertTrue(result["sequence_reset_observed"])
        self.assertEqual(result["reset_epoch_records"][0]["sample_sequence"], 1)
        self.assertIsInstance(result["inferred_release_monotonic_s"], float)

    def test_telemetry_overlap_fails_same_epoch_post_grace_before_reset(self) -> None:
        ws = FakeJsonWsConnection([
            '{"type":"telemetry","topic":"adc","sequence":13,"sample_sequence":13,"uptime_us":1013,"device_t_mono_us":1013}',
            '{"type":"telemetry","topic":"adc","sequence":14,"sample_sequence":14,"uptime_us":1014,"device_t_mono_us":1014}',
        ])
        worker = mock.Mock()
        states = [True, True, False]
        worker.is_alive.side_effect = lambda: states.pop(0) if states else False
        with mock.patch.object(runner.time, "monotonic", side_effect=[0.0, 0.01, 0.02, 0.10, 0.11, 0.12]):
            result = runner.telemetry_read_during_overlap(
                ws,
                start_monotonic_s=0.0,
                worker=worker,
                timeout_s=1.0,
                baseline_sequence=12,
                baseline_device_time_us=1000,
            )
        self.assertEqual(result["pre_pause_delivery_grace_count"], 1)
        self.assertEqual(result["old_epoch_pause_record_count"], 1)
        self.assertEqual(result["reset_epoch_record_count"], 0)
        self.assertFalse(result["expected_pause_observed"])

    def test_wide11_telemetry_isolation_cli_plan(self) -> None:
        args = runner.parse_args([
            "--dry-run",
            "--matrix",
            "wide11-telemetry-isolation",
            "--telemetry-isolation-rate-hz",
            "250",
            "--telemetry-isolation-baseline-samples",
            "3",
            "--telemetry-isolation-post-release-samples",
            "4",
        ])
        plan = runner.build_plan(args)
        self.assertEqual(args.matrix, ["wide11-telemetry-isolation"])
        case = plan["matrices"][0]["cases"][0]
        self.assertEqual(plan["matrices"][0]["name"], "wide11_telemetry_isolation")
        self.assertEqual(case["telemetry_client"]["rate_hz"], 250)
        self.assertEqual(case["capture_client"]["post_samples"], 100000)
        self.assertEqual(case["capture_client"]["expected_data_frames"], 98)

    def test_wide11_telemetry_isolation_case_success_with_pause_and_resume(self) -> None:
        args = runner.parse_args(["--matrix", "wide11-telemetry-isolation", "--telemetry-isolation-baseline-samples", "1", "--telemetry-isolation-post-release-samples", "1"])
        fake_ws = FakeJsonWsConnection([])

        class WsModule:
            @staticmethod
            def create_connection(url: str, timeout: float) -> FakeJsonWsConnection:
                return fake_ws

        baseline = {"phase": "baseline", "records": [{"sample_sequence": 10, "device_t_mono_us": 868824800, "uptime_us": 868824800, "received_monotonic_s": 1.0}], "record_count": 1, "connected": True, "malformed_json": 0, "binary_frames": 0, "errors": []}
        overlap = {"phase": "overlap_pause", "connected": True, "expected_pause_observed": True, "pause_record_count": 0, "malformed_json": 0, "binary_frames": 0, "recv_timeouts": 1, "errors": [], "pre_pause_delivery_grace_records": []}
        post = {"phase": "post_release", "records": [{"sample_sequence": 1, "device_t_mono_us": 869354700, "uptime_us": 869354700, "received_monotonic_s": 2.0}, {"sample_sequence": 2, "device_t_mono_us": 869354800, "uptime_us": 869354800, "received_monotonic_s": 2.1}], "record_count": 2, "connected": True, "malformed_json": 0, "binary_frames": 0, "errors": []}
        raw = {"ok": True, "reason": "ok", "stats": {"received_sample_count": 100000, "data_frames": 98, "sample_index_gaps": 0}}

        def fake_burst(host: str, port: int, timeout_s: float, *, on_start_resp=None):
            if callable(on_start_resp):
                on_start_resp({"start_resp_monotonic_s": 1.5})
            return raw

        with (
            mock.patch.object(runner, "load_websocket_module", return_value=WsModule),
            mock.patch.object(runner, "create_live_session_ws_url", return_value="ws://telemetry"),
            mock.patch.object(runner, "telemetry_subscribe", return_value={"connected": True}),
            mock.patch.object(runner, "telemetry_read_until", side_effect=[baseline, post]),
            mock.patch.object(runner, "telemetry_read_during_overlap", return_value=overlap),
            mock.patch.object(runner, "sigrok_tcp_wide11_deep_burst_raw_capture", side_effect=fake_burst),
            mock.patch.object(runner, "adc_http_health", return_value={"ok": True}),
        ):
            result = runner.sigrok_tcp_wide11_telemetry_isolation_case(args)
        self.assertTrue(result["pass"], result)
        self.assertFalse(result["post_release_sequence_advanced"])
        self.assertTrue(result["post_release_sequence_reset_observed"])
        self.assertEqual(result["post_release_sequence_epoch"], "reset_or_reconstructed")
        self.assertTrue(result["post_release_device_time_advanced"])
        self.assertTrue(result["post_release_valid_epoch"])
        self.assertEqual(result["raw_tcp_wide11_burst"]["stats"]["data_frames"], 98)
        self.assertTrue(fake_ws.closed)

    def test_wide11_telemetry_isolation_case_accepts_reset_epoch_before_helper_return(self) -> None:
        args = runner.parse_args(["--matrix", "wide11-telemetry-isolation", "--telemetry-isolation-baseline-samples", "1", "--telemetry-isolation-post-release-samples", "2"])
        fake_ws = FakeJsonWsConnection([])

        class WsModule:
            @staticmethod
            def create_connection(url: str, timeout: float) -> FakeJsonWsConnection:
                return fake_ws

        baseline = {"phase": "baseline", "records": [{"sample_sequence": 12, "device_t_mono_us": 5000, "uptime_us": 5000, "received_monotonic_s": 1.0}], "record_count": 1, "connected": True, "malformed_json": 0, "binary_frames": 0, "errors": []}
        reset_epoch_records = [{"sample_sequence": seq, "device_t_mono_us": 6000 + seq, "uptime_us": 6000 + seq, "received_monotonic_s": 2.0 + seq / 100.0} for seq in range(1, 10)]
        overlap = {"phase": "overlap_pause", "connected": True, "expected_pause_observed": True, "pre_pause_delivery_grace_count": 13, "old_epoch_pause_record_count": 0, "pause_record_count": 0, "reset_epoch_records": reset_epoch_records, "reset_epoch_record_count": len(reset_epoch_records), "sequence_reset_observed": True, "inferred_release_monotonic_s": 2.01, "malformed_json": 0, "binary_frames": 0, "recv_timeouts": 0, "errors": []}
        post_after_helper = {"phase": "post_release", "records": [{"sample_sequence": 10, "device_t_mono_us": 6010, "uptime_us": 6010, "received_monotonic_s": 3.0}, {"sample_sequence": 11, "device_t_mono_us": 6011, "uptime_us": 6011, "received_monotonic_s": 3.1}], "record_count": 2, "connected": True, "malformed_json": 0, "binary_frames": 0, "errors": []}
        raw = {"ok": True, "reason": "ok", "stats": {"received_sample_count": 100000, "data_frames": 98, "sample_index_gaps": 0}}

        def fake_burst(host: str, port: int, timeout_s: float, *, on_start_resp=None):
            if callable(on_start_resp):
                on_start_resp({"start_resp_monotonic_s": 1.5})
            return raw

        with (
            mock.patch.object(runner, "load_websocket_module", return_value=WsModule),
            mock.patch.object(runner, "create_live_session_ws_url", return_value="ws://telemetry"),
            mock.patch.object(runner, "telemetry_subscribe", return_value={"connected": True}),
            mock.patch.object(runner, "telemetry_read_until", side_effect=[baseline, post_after_helper]),
            mock.patch.object(runner, "telemetry_read_during_overlap", return_value=overlap),
            mock.patch.object(runner, "sigrok_tcp_wide11_deep_burst_raw_capture", side_effect=fake_burst),
            mock.patch.object(runner, "adc_http_health", return_value={"ok": True}),
        ):
            result = runner.sigrok_tcp_wide11_telemetry_isolation_case(args)
        self.assertTrue(result["pass"], result)
        self.assertEqual(result["inferred_release_monotonic_s"], 2.01)
        self.assertEqual(result["post_release"]["overlap_reset_epoch_record_count"], 9)
        self.assertEqual(result["post_release"]["after_helper_return_record_count"], 2)
        self.assertEqual(result["post_release"]["record_count"], 11)
        self.assertEqual(result["post_release"]["records"][0]["sample_sequence"], 1)
        self.assertEqual(result["post_release_last_sequence"], 11)

    def test_high_rate_matrix_run_dispatches_hello_and_capture_without_global_uart(self) -> None:
        args = runner.parse_args(["--matrix", "high-rate-packed-burst"])
        hello_calls: list[str] = []
        capture_calls: list[str] = []

        def fake_hello(_args: argparse.Namespace, transport: str) -> dict[str, object]:
            hello_calls.append(transport)
            return {"case": "sigrok_high_rate_hello_flags", "transport": transport, "pass": True}

        def fake_capture(_args: argparse.Namespace, descriptor: Mapping[str, object]) -> dict[str, object]:
            case = dict(descriptor)
            capture_calls.append(str(case["transport"]))
            return {"case": "sigrok_high_rate_packed_burst", "transport": case["transport"], "pass": True}

        with (
            mock.patch.object(runner, "high_rate_hello_flags_case", side_effect=fake_hello),
            mock.patch.object(runner, "sigrok_high_rate_packed_burst_capture_case", side_effect=fake_capture),
            mock.patch.object(runner, "perform_uart_stimulus", side_effect=AssertionError("global UART stimulus should be deferred")),
        ):
            result = runner.run(args)

        self.assertTrue(result["overall_pass"])
        self.assertEqual(hello_calls, ["tcp", "websocket"])
        self.assertGreater(len(capture_calls), 0)
        self.assertIsNone(result["uart_stimulus"])


if __name__ == "__main__":
    unittest.main()
