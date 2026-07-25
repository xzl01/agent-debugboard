#!/usr/bin/env python3
"""Offline unit tests for logic_analyzer_hil_perf.py."""

import argparse
import importlib.util
import pathlib
import sys
import unittest
from unittest import mock

RUNNER_PATH = pathlib.Path(__file__).resolve().with_name("logic_analyzer_hil_perf.py")
RUNNER_SPEC = importlib.util.spec_from_file_location("logic_analyzer_hil_perf", RUNNER_PATH)
if RUNNER_SPEC is None or RUNNER_SPEC.loader is None:
    raise RuntimeError(f"failed to load {RUNNER_PATH}")
runner = importlib.util.module_from_spec(RUNNER_SPEC)
sys.modules[RUNNER_SPEC.name] = runner
RUNNER_SPEC.loader.exec_module(runner)


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

        wide_meta = runner.SigrokDataMeta(0, 5, runner.COMPRESSION_BIT_PACK_RLE, runner.MASK_WIDE12)
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
        self.assertIn("WIDE12", plan["modes"])
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


if __name__ == "__main__":
    unittest.main()
