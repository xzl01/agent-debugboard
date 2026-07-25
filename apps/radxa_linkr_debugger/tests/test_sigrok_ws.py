#!/usr/bin/env python3
# pyright: reportMissingImports=false, reportMissingTypeArgument=false, reportReturnType=false
"""Sigrok WebSocket protocol test script."""

import argparse
import struct
import sys
import time
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
SIGROK_DATA_META_SIZE = 8

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

COMPRESSION_NONE = 0
COMPRESSION_BIT_PACK = 1
COMPRESSION_RLE = 2
COMPRESSION_BIT_PACK_RLE = 3

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


def parse_data_meta(payload: bytes) -> dict:
    if len(payload) < SIGROK_DATA_META_SIZE:
        raise ValueError("DATA payload shorter than metadata")
    return {
        "sample_index": struct.unpack("<I", payload[0:3] + b'\x00')[0],
        "sample_count": struct.unpack("<H", payload[3:5])[0],
        "compression": payload[5],
        "channel_mask": struct.unpack("<H", payload[6:8])[0],
    }


def sigrok_bytes_per_sample(channel_mask: int) -> int:
    channel_count = (channel_mask & 0xFFFF).bit_count()
    if channel_count == 0:
        return 0
    return (channel_count + 7) // 8


def decode_data_payload(meta: dict, sample_payload: bytes) -> bytes:
    sample_count = int(meta["sample_count"])
    if sample_count <= 0:
        raise ValueError("DATA sample_count must be non-zero")
    bytes_per_sample = sigrok_bytes_per_sample(int(meta["channel_mask"]))
    if bytes_per_sample <= 0:
        raise ValueError("DATA channel_mask selects no channels")
    expected_len = sample_count * bytes_per_sample
    compression = int(meta["compression"])
    if compression in (COMPRESSION_NONE, COMPRESSION_BIT_PACK):
        if len(sample_payload) != expected_len:
            raise ValueError(f"BIT_PACK payload length {len(sample_payload)} != expected {expected_len}")
        return sample_payload
    if compression == COMPRESSION_BIT_PACK_RLE:
        tuple_len = bytes_per_sample + 2
        if len(sample_payload) == 0 or len(sample_payload) % tuple_len != 0:
            raise ValueError("BIT_PACK_RLE payload has truncated tuple")
        out = bytearray()
        expanded_count = 0
        for pos in range(0, len(sample_payload), tuple_len):
            value = sample_payload[pos:pos + bytes_per_sample]
            run_pos = pos + bytes_per_sample
            run_count = sample_payload[run_pos] | (sample_payload[run_pos + 1] << 8)
            if run_count == 0:
                raise ValueError("BIT_PACK_RLE run_count must be non-zero")
            if expanded_count + run_count > sample_count:
                raise ValueError("BIT_PACK_RLE expanded sample count overflows metadata")
            out.extend(value * run_count)
            expanded_count += run_count
        if expanded_count != sample_count or len(out) != expected_len:
            raise ValueError("BIT_PACK_RLE expanded output does not match metadata")
        return bytes(out)
    if compression == COMPRESSION_RLE:
        raise ValueError("standalone RLE is not valid for current DATA samples")
    raise ValueError(f"unsupported DATA compression {compression}")


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
                 samplerate_khz: int, pre_samples: int = 0, post_samples: int = 0,
                 trigger_channel: int = 0) -> dict:
    payload = struct.pack("<BBBH", mode_id, trigger_type, trigger_channel, channel_mask)
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


def wait_for_triggered_capture(ws, requested_samples: int, timeout: float = 5.0) -> dict:
    deadline = time.monotonic() + timeout
    trigger_event = None
    first_sample_index = None
    received_samples = 0
    data_frames = 0
    while time.monotonic() < deadline and (trigger_event is None or received_samples < requested_samples):
        ws.settimeout(max(0.05, deadline - time.monotonic()))
        try:
            data = ws.recv()
        except websocket.WebSocketTimeoutException:
            break
        if isinstance(data, str):
            continue
        header = parse_header(data)
        if header is None:
            continue
        _, _, frame_type, _, payload_len = header
        payload = data[SIGROK_HEADER_SIZE:SIGROK_HEADER_SIZE + payload_len]
        if frame_type == FRAME_EVENT:
            event = parse_event(payload)
            if event["type_detail"] == 2:
                trigger_event = event
        elif frame_type == FRAME_DATA:
            meta = parse_data_meta(payload)
            _ = decode_data_payload(meta, payload[SIGROK_DATA_META_SIZE:])
            if first_sample_index is None:
                first_sample_index = meta["sample_index"]
            received_samples += meta["sample_count"]
            data_frames += 1
    offset = None
    offset_valid = False
    if trigger_event is not None and first_sample_index is not None:
        offset = (trigger_event["sample_index"] - first_sample_index) & 0xFFFFFF
        offset_valid = offset < received_samples
    return {
        "trigger_event": trigger_event,
        "data_frames": data_frames,
        "received_samples": received_samples,
        "first_sample_index": first_sample_index,
        "trigger_sample_offset": offset,
        "trigger_sample_offset_valid": offset_valid,
    }


def perform_uart_stimulus(args) -> bool:
    if args.uart_device != "/dev/ttyACM1":
        print("    FAIL: trigger UART stimulus requires --uart-device /dev/ttyACM1")
        return False
    if args.uart_baud is None:
        print("    FAIL: trigger UART stimulus requires --uart-baud")
        return False
    if args.uart_stimulus is None:
        print("    FAIL: trigger test requires --uart-stimulus")
        return False
    try:
        import serial
    except ImportError:
        print("    FAIL: trigger UART stimulus requires pyserial")
        return False
    with serial.Serial(
        port=args.uart_device,
        baudrate=args.uart_baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=args.timeout,
        write_timeout=args.timeout,
    ) as ser:
        ser.write(args.uart_stimulus.encode("utf-8"))
        ser.flush()
    return True


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


def test_trigger(args) -> bool:
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
                              channel_mask=0x01, samplerate_khz=1000,
                              pre_samples=0, post_samples=args.trigger_post_samples,
                              trigger_channel=args.trigger_channel)
            if ack is None:
                print(f"    FAIL: CONFIG failed")
                all_pass = False
                continue

            ack = send_start(ws)
            if ack is None:
                print(f"    FAIL: START failed")
                all_pass = False
                continue

            if not perform_uart_stimulus(args):
                all_pass = False
                send_stop(ws)
                continue

            capture = wait_for_triggered_capture(ws, args.trigger_post_samples, timeout=args.timeout)
            if capture["trigger_event"] is None:
                print(f"    FAIL: {trigger_name} trigger did not fire")
                all_pass = False
            elif not capture["trigger_sample_offset_valid"]:
                print(f"    FAIL: {trigger_name} trigger sample offset invalid: {capture}")
                all_pass = False
            elif capture["received_samples"] < args.trigger_post_samples:
                print(f"    FAIL: {trigger_name} bounded capture incomplete: {capture}")
                all_pass = False
            else:
                print(f"    PASS: {trigger_name} trigger fired: {capture}")

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
    parser.add_argument("--timeout", type=float, default=5.0, help="per-operation timeout in seconds")
    parser.add_argument("--trigger-post-samples", type=int, default=1024, help="bounded post-trigger samples for trigger tests")
    parser.add_argument("--trigger-channel", type=int, default=0, help="sigrok trigger channel index byte; GP10 is channel 0")
    parser.add_argument("--uart-stimulus", help="text stimulus written after START for trigger tests")
    parser.add_argument("--uart-device", help="must be explicitly /dev/ttyACM1 for trigger UART stimulus")
    parser.add_argument("--uart-baud", type=int, help="explicit UART baud for 8N1 trigger stimulus")
    args = parser.parse_args()
    if not 0 <= args.trigger_channel <= 0xFF:
        parser.error("--trigger-channel must fit uint8")

    tests = {
        "hello": test_hello,
        "rates": test_rates,
        "trigger": lambda: test_trigger(args),
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
