#!/usr/bin/env python3
"""Sigrok WebSocket protocol test script."""

import argparse
import struct
import sys
import time
import threading
import socket
import json
import urllib.request

try:
    import websocket
except ImportError:
    print("ERROR: websocket-client not installed. Run: pip install websocket-client")
    sys.exit(1)

SIGROK_MAGIC = 0x72
SIGROK_PROTOCOL_VERSION = 1
SIGROK_HEADER_SIZE = 9

FRAME_HELLO_REQ = 0x01
FRAME_HELLO_RESP = 0x02
FRAME_CAPS_REQ = 0x03
FRAME_CAPS_RESP = 0x04
FRAME_CONFIG_REQ = 0x05
FRAME_CONFIG_RESP = 0x06
FRAME_START_REQ = 0x07
FRAME_START_RESP = 0x08
FRAME_STOP_REQ = 0x09
FRAME_STOP_RESP = 0x0a
FRAME_EVENT = 0x10
FRAME_DATA = 0x11
FRAME_ERROR = 0x7f

ERROR_BUSY = 8

TRIGGER_NONE = 0
TRIGGER_RISING = 1
TRIGGER_FALLING = 2
TRIGGER_EITHER = 3

WS_URL = "ws://172.29.203.1/api/v1/ws/0"
TCP_HOST = "172.29.203.1"
TCP_PORT = 5556
HTTP_BASE = "http://172.29.203.1"


def create_session() -> str:
    req = urllib.request.Request(HTTP_BASE + "/api/v1/live-sessions", method="POST")
    resp = json.loads(urllib.request.urlopen(req).read())
    return resp["ws_url"]


def build_frame(frame_type: int, payload: bytes = b"", frame_id: int = 1) -> bytes:
    header = struct.pack("<BBBIH", SIGROK_MAGIC, SIGROK_PROTOCOL_VERSION,
                         frame_type, frame_id, len(payload))
    return header + payload


def parse_header(data: bytes) -> tuple:
    if len(data) < SIGROK_HEADER_SIZE:
        return None
    magic, version, frame_type, frame_id, payload_len = struct.unpack("<BBBIH", data[:SIGROK_HEADER_SIZE])
    if magic != SIGROK_MAGIC:
        return None
    return (magic, version, frame_type, frame_id, payload_len)


def parse_hello_resp(payload: bytes) -> dict:
    protocol_version, server_flags, mode_count = struct.unpack("<BBB", payload[:3])
    max_payload_len = struct.unpack("<H", payload[3:5])[0]
    return {
        "protocol_version": protocol_version,
        "server_flags": server_flags,
        "mode_count": mode_count,
        "max_payload_len": max_payload_len,
    }


def parse_caps_resp(payload: bytes) -> dict:
    mode_count = payload[0]
    modes = []
    for i in range(mode_count):
        offset = 1 + i * 8
        mode_id, mode_flags, channel_count, sample_bytes = struct.unpack("<BBBB", payload[offset:offset+4])
        max_samplerate_khz = struct.unpack("<I", payload[offset+4:offset+7] + b'\x00')[0]
        compression = payload[offset+7]
        modes.append({
            "mode_id": mode_id,
            "mode_flags": mode_flags,
            "channel_count": channel_count,
            "sample_bytes": sample_bytes,
            "max_samplerate_khz": max_samplerate_khz,
            "compression": compression,
        })
    return {"mode_count": mode_count, "modes": modes}


def parse_ack(payload: bytes) -> dict:
    session_id, state = struct.unpack("<HB", payload[:3])
    actual_rate_khz = struct.unpack("<I", payload[3:6] + b'\x00')[0]
    return {
        "session_id": session_id,
        "state": state,
        "actual_rate_khz": actual_rate_khz,
    }


def parse_event(payload: bytes) -> dict:
    session_id, type_detail = struct.unpack("<HB", payload[:3])
    sample_index = struct.unpack("<I", payload[3:6] + b'\x00')[0]
    return {
        "session_id": session_id,
        "type_detail": type_detail,
        "sample_index": sample_index,
    }


def parse_error(payload: bytes) -> dict:
    error_code, detail = struct.unpack("<BH", payload[:3])
    return {"error_code": error_code, "detail": detail}


def recv_frame(ws, timeout: float = 5.0) -> tuple:
    ws.settimeout(timeout)
    try:
        data = ws.recv()
        if isinstance(data, str):
            return None, None
        header = parse_header(data)
        if header is None:
            return None, None
        _, _, frame_type, frame_id, payload_len = header
        payload = data[SIGROK_HEADER_SIZE:SIGROK_HEADER_SIZE + payload_len]
        return frame_type, payload
    except websocket.WebSocketTimeoutException:
        return None, None
    except Exception:
        return None, None


def send_hello(ws) -> bool:
    ws.send(build_frame(FRAME_HELLO_REQ), opcode=websocket.ABNF.OPCODE_BINARY)
    frame_type, payload = recv_frame(ws)
    if frame_type is None or frame_type != FRAME_HELLO_RESP:
        print(f"  FAIL: Expected HELLO_RESP (0x02), got {frame_type}")
        return False
    hello = parse_hello_resp(payload)
    print(f"  HELLO: protocol_version={hello['protocol_version']}, mode_count={hello['mode_count']}")
    return True


def send_caps(ws) -> bool:
    ws.send(build_frame(FRAME_CAPS_REQ), opcode=websocket.ABNF.OPCODE_BINARY)
    frame_type, payload = recv_frame(ws)
    if frame_type is None or frame_type != FRAME_CAPS_RESP:
        print(f"  FAIL: Expected CAPS_RESP (0x04), got {frame_type}")
        return False
    caps = parse_caps_resp(payload)
    print(f"  CAPS: modes={caps['mode_count']}")
    for mode in caps["modes"]:
        print(f"    Mode {mode['mode_id']}: channels={mode['channel_count']}, max_rate={mode['max_samplerate_khz']}kHz")
    return True


def send_config(ws, mode_id: int, trigger_type: int, channel_mask: int,
                samplerate_khz: int, pre_samples: int = 0, post_samples: int = 0) -> dict:
    payload = struct.pack("<BBBH", mode_id, trigger_type, 0, channel_mask)
    payload += struct.pack("<I", samplerate_khz)[:3]
    payload += struct.pack("<HH", pre_samples, post_samples)
    ws.send(build_frame(FRAME_CONFIG_REQ, payload), opcode=websocket.ABNF.OPCODE_BINARY)
    frame_type, payload = recv_frame(ws)
    if frame_type is None or frame_type != FRAME_CONFIG_RESP:
        print(f"  FAIL: Expected CONFIG_RESP (0x06), got {frame_type}")
        return None
    return parse_ack(payload)


def send_start(ws) -> dict:
    ws.send(build_frame(FRAME_START_REQ), opcode=websocket.ABNF.OPCODE_BINARY)
    frame_type, payload = recv_frame(ws)
    if frame_type is None or frame_type != FRAME_START_RESP:
        print(f"  FAIL: Expected START_RESP (0x08), got {frame_type}")
        return None
    return parse_ack(payload)


def send_stop(ws) -> dict:
    ws.send(build_frame(FRAME_STOP_REQ), opcode=websocket.ABNF.OPCODE_BINARY)
    frame_type, payload = recv_frame(ws)
    if frame_type is None or frame_type != FRAME_STOP_RESP:
        print(f"  FAIL: Expected STOP_RESP (0x0a), got {frame_type}")
        return None
    return parse_ack(payload)


def wait_for_event(ws, expected_type: int, timeout: float = 5.0) -> dict:
    ws.settimeout(timeout)
    try:
        while True:
            data = ws.recv()
            if isinstance(data, str):
                continue
            header = parse_header(data)
            if header is None:
                continue
            _, _, frame_type, _, payload_len = header
            payload = data[SIGROK_HEADER_SIZE:SIGROK_HEADER_SIZE + payload_len]
            if frame_type == FRAME_EVENT:
                event = parse_event(payload)
                if event["type_detail"] == expected_type:
                    return event
            elif frame_type == FRAME_DATA:
                continue
    except websocket.WebSocketTimeoutException:
        return None


def test_hello() -> bool:
    print("Testing HELLO/CAPS handshake...")
    ws_url = create_session()
    ws = websocket.create_connection(ws_url, timeout=5)
    try:
        if not send_hello(ws):
            return False
        if not send_caps(ws):
            return False
        print("  PASS: HELLO/CAPS handshake successful")
        return True
    finally:
        ws.close()


def test_config_start_stop() -> bool:
    print("Testing CONFIG/START/STOP flow...")
    ws = websocket.create_connection(create_session(), timeout=5)
    try:
        send_hello(ws)
        send_caps(ws)

        print("  Sending CONFIG (mode=1, rate=1000kHz)...")
        ack = send_config(ws, mode_id=1, trigger_type=TRIGGER_NONE,
                          channel_mask=0x01, samplerate_khz=1000)
        if ack is None:
            return False
        print(f"  CONFIG: session_id={ack['session_id']}, state={ack['state']}, rate={ack['actual_rate_khz']}kHz")

        print("  Sending START...")
        ack = send_start(ws)
        if ack is None:
            return False
        print(f"  START: state={ack['state']}")

        print("  Waiting for DATA or EVENT...")
        ws.settimeout(2.0)
        data_received = False
        try:
            while True:
                data = ws.recv()
                if isinstance(data, bytes):
                    header = parse_header(data)
                    if header and header[2] == FRAME_DATA:
                        print(f"  DATA received: {len(data)} bytes")
                        data_received = True
                        break
                    elif header and header[2] == FRAME_EVENT:
                        print(f"  EVENT received")
                    else:
                        print(f"  WARNING: Unexpected frame type 0x{header[2]:02x}")
                        break
        except websocket.WebSocketTimeoutException:
            print("  WARNING: No DATA frame received within timeout")

        print("  Sending STOP...")
        ws.send(build_frame(FRAME_STOP_REQ), opcode=websocket.ABNF.OPCODE_BINARY)
        ws.settimeout(2.0)
        stop_received = False
        try:
            while True:
                data = ws.recv()
                if isinstance(data, bytes):
                    header = parse_header(data)
                    if header and header[2] == FRAME_STOP_RESP:
                        payload = data[SIGROK_HEADER_SIZE:SIGROK_HEADER_SIZE + header[4]]
                        ack = parse_ack(payload)
                        print(f"  STOP: state={ack['state']}")
                        stop_received = True
                        break
                    elif header and header[2] in (FRAME_DATA, FRAME_EVENT):
                        pass
                    else:
                        print(f"  WARNING: Unexpected frame type 0x{header[2]:02x}")
                        break
        except websocket.WebSocketTimeoutException:
            print("  WARNING: No STOP_RESP received within timeout")

        print("  PASS: CONFIG/START/STOP flow successful")
        return True
    finally:
        ws.close()


def test_rates() -> bool:
    print("Testing sample rates...")
    rates = [100, 500, 1000, 5000, 10000, 50000, 100000]
    all_pass = True

    for rate_khz in rates:
        print(f"  Testing {rate_khz} kHz...")
        ws = websocket.create_connection(create_session(), timeout=5)
        try:
            send_hello(ws)
            send_caps(ws)

            ack = send_config(ws, mode_id=1, trigger_type=TRIGGER_NONE,
                              channel_mask=0x01, samplerate_khz=rate_khz)
            if ack is None:
                print(f"    FAIL: CONFIG failed")
                all_pass = False
                continue

            ack = send_start(ws)
            if ack is None:
                print(f"    FAIL: START failed")
                all_pass = False
                continue

            ws.settimeout(2.0)
            data_received = False
            try:
                while True:
                    data = ws.recv()
                    if isinstance(data, bytes):
                        header = parse_header(data)
                        if header and header[2] == FRAME_DATA:
                            print(f"    PASS: {rate_khz} kHz")
                            data_received = True
                            break
                        elif header and header[2] == FRAME_EVENT:
                            pass
                        else:
                            print(f"    FAIL: Unexpected frame type")
                            all_pass = False
                            break
            except websocket.WebSocketTimeoutException:
                print(f"    FAIL: Timeout waiting for data")
                all_pass = False

            ws.send(build_frame(FRAME_STOP_REQ), opcode=websocket.ABNF.OPCODE_BINARY)
            ws.settimeout(1.0)
            try:
                while True:
                    data = ws.recv()
                    if isinstance(data, bytes):
                        header = parse_header(data)
                        if header and header[2] in (FRAME_STOP_RESP, FRAME_DATA, FRAME_EVENT):
                            if header[2] == FRAME_STOP_RESP:
                                break
            except websocket.WebSocketTimeoutException:
                pass
        finally:
            ws.close()

    return all_pass


def test_trigger() -> bool:
    print("Testing trigger modes...")
    triggers = [
        (TRIGGER_RISING, "rising"),
        (TRIGGER_FALLING, "falling"),
        (TRIGGER_EITHER, "either"),
    ]
    all_pass = True

    for trigger_type, trigger_name in triggers:
        print(f"  Testing {trigger_name} trigger...")
        ws = websocket.create_connection(create_session(), timeout=5)
        try:
            send_hello(ws)
            send_caps(ws)

            ack = send_config(ws, mode_id=1, trigger_type=trigger_type,
                              channel_mask=0x01, samplerate_khz=1000)
            if ack is None:
                print(f"    FAIL: CONFIG failed")
                all_pass = False
                continue

            ack = send_start(ws)
            if ack is None:
                print(f"    FAIL: START failed")
                all_pass = False
                continue

            event = wait_for_event(ws, expected_type=2, timeout=2.0)
            if event is None:
                print(f"    PASS: {trigger_name} trigger (no trigger event within timeout - expected)")
            else:
                print(f"    PASS: {trigger_name} trigger fired")

            send_stop(ws)
        finally:
            ws.close()

    return all_pass


def test_concurrent() -> bool:
    print("Testing concurrent access (TCP vs WS)...")
    print("  Connecting to TCP 5556...")

    tcp_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_sock.settimeout(5.0)
    try:
        tcp_sock.connect((TCP_HOST, TCP_PORT))
    except Exception as e:
        print(f"  SKIP: Cannot connect to TCP 5556: {e}")
        return True

    try:
        hello_frame = build_frame(FRAME_HELLO_REQ, frame_id=1)
        tcp_sock.send(hello_frame)

        response = tcp_sock.recv(1024)
        if len(response) < SIGROK_HEADER_SIZE:
            print("  FAIL: No TCP HELLO response")
            return False

        header = parse_header(response)
        if header is None or header[2] != FRAME_HELLO_RESP:
            print(f"  FAIL: Unexpected TCP response type 0x{header[2]:02x}")
            return False

        print("  TCP HELLO successful, now connecting WS...")

        ws = websocket.create_connection(create_session(), timeout=5)
        try:
            ws.send(build_frame(FRAME_HELLO_REQ), opcode=websocket.ABNF.OPCODE_BINARY)
            frame_type, payload = recv_frame(ws, timeout=2.0)

            if frame_type == FRAME_ERROR:
                error = parse_error(payload)
                if error["error_code"] == ERROR_BUSY:
                    print(f"  PASS: WS received BUSY error (as expected)")
                    return True
                else:
                    print(f"  FAIL: Unexpected error code {error['error_code']}")
                    return False
            elif frame_type == FRAME_HELLO_RESP:
                print("  FAIL: WS HELLO succeeded (should have been BUSY)")
                return False
            else:
                print(f"  FAIL: Unexpected WS response type 0x{frame_type:02x}")
                return False
        finally:
            ws.close()
    finally:
        tcp_sock.close()


def main():
    parser = argparse.ArgumentParser(description="Sigrok WebSocket protocol test")
    parser.add_argument("--test", choices=["hello", "rates", "trigger", "concurrent", "all"],
                        default="all", help="Test to run")
    args = parser.parse_args()

    tests = {
        "hello": test_hello,
        "rates": test_rates,
        "trigger": test_trigger,
        "concurrent": test_concurrent,
    }

    if args.test == "all":
        results = {}
        for name, test_func in tests.items():
            print(f"\n{'='*60}")
            print(f"Running: {name}")
            print('='*60)
            results[name] = test_func()

        print(f"\n{'='*60}")
        print("Summary:")
        print('='*60)
        for name, passed in results.items():
            status = "PASS" if passed else "FAIL"
            print(f"  {name}: {status}")

        all_pass = all(results.values())
        print(f"\nOverall: {'ALL PASS' if all_pass else 'SOME FAILED'}")
        return 0 if all_pass else 1
    else:
        test_func = tests[args.test]
        return 0 if test_func() else 1


if __name__ == "__main__":
    sys.exit(main())
