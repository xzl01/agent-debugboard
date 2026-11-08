#!/usr/bin/env python3
"""Live WebUSB regression for an explicitly authorized Linkr board.

Requires a real board and PyUSB/libusb permissions. First complete fixture setup
under the full or an explicitly authorized usb-only profile. This requests no PD
voltage, switches no power, replays no mutations, and is not a full HIL pass.
"""
import argparse
import errno
import json
import struct
import sys

WEBUSB_UUID = bytes.fromhex("38b60834a909a0478bfda0768815b665")
def count(value):
    value = int(value)
    if not 32 <= value <= 256:
        raise argparse.ArgumentTypeError("must be between 32 and 256")
    return value
def arguments():
    parser = argparse.ArgumentParser(description="Run the live Linkr WebUSB HIL regression")
    for name in ("bus", "address"):
        parser.add_argument(f"--{name}", required=True, type=int)
    for name in ("serial", "expected-url"):
        parser.add_argument(f"--{name}", required=True)
    for name, default in (("vid", 0x2FE3), ("pid", 0xDB01)):
        parser.add_argument(f"--{name}", type=lambda x: int(x, 0), default=default)
    parser.add_argument("--zero-length-count", type=count, default=64)
    parser.add_argument("--output")
    return parser.parse_args()
def require(condition, message):
    if not condition:
        raise AssertionError(message)
def get_device_descriptor(device):
    data = bytes(device.ctrl_transfer(0x80, 6, 0x0100, 0, 18))
    require(len(data) == 18 and data[:2] == b"\x12\x01", f"device descriptor length/type: {len(data)}/{data[1] if len(data) > 1 else None}")
    version = struct.unpack_from("<H", data, 2)[0]
    require(version >= 0x0210, f"bcdUSB 0x{version:04x} is below 0x0210")
    return data, version
def get_bos(device):
    header = bytes(device.ctrl_transfer(0x80, 6, 0x0F00, 0, 5))
    require(len(header) == 5 and header[:2] == b"\x05\x0f", f"BOS header length/type: {len(header)}/{header[1] if len(header) > 1 else None}")
    total = struct.unpack_from("<H", header, 2)[0]
    data = bytes(device.ctrl_transfer(0x80, 6, 0x0F00, 0, total))
    require(len(data) == total and data[:5] == header, f"BOS expected {total} bytes, received {len(data)}")
    webusb, offset, cap_count = None, header[0], 0
    while offset < total:
        length = data[offset]
        require(length >= 3 and offset + length <= total, f"BOS capability at {offset} has invalid length {length}")
        cap = data[offset:offset + length]
        if length == 24 and cap[1:4] == b"\x10\x05\x00" and cap[4:20] == WEBUSB_UUID:
            webusb = (struct.unpack_from("<H", cap, 20)[0], cap[22], cap[23])
        offset, cap_count = offset + length, cap_count + 1
    require(cap_count == header[4], f"BOS capability count: expected {header[4]}, parsed {cap_count}")
    require(webusb is not None, "WebUSB platform capability not found")
    version, code, index = webusb
    require(version >= 0x0100 and code != 0 and index != 0, f"WebUSB version/code/index: 0x{version:04x}/{code}/{index}")
    return data, version, code, index
def get_url(device, code, index, length, expected):
    data = bytes(device.ctrl_transfer(0xC0, code, index, 2, length))
    require(len(data) >= 3 and data[0] == len(data) and data[1:3] == b"\x03\x01", f"URL descriptor length/header: {len(data)}/{list(data[:3])}")
    try:
        actual = "https://" + data[3:].decode("utf-8")
    except UnicodeDecodeError as error:
        raise AssertionError(f"URL descriptor invalid UTF-8 at byte {error.start + 3}") from error
    require(actual == expected, f"URL mismatch: expected length {len(expected)}, received length {len(actual)}")
    return data
def reject(device, usb_core, code, value, index, length, case):
    try:
        data = device.ctrl_transfer(0xC0, code, value, index, length)
    except usb_core.USBError as error:
        if error.errno == errno.EPIPE or getattr(error, "backend_error_code", None) == -9:
            case["result"] = "pipe"
            return
        case.update(result="error", errno=error.errno, backend_error=getattr(error, "backend_error_code", None))
        raise AssertionError(f"invalid URL request returned errno {error.errno}, backend {case['backend_error']}") from error
    case.update(result="accepted", received=len(data))
    raise AssertionError(f"invalid URL request accepted {len(data)} bytes")
def emit(report, path):
    text = json.dumps(report, indent=2, sort_keys=True)
    if path:
        with open(path, "w", encoding="utf-8") as output:
            output.write(text + "\n")
        print(f"WebUSB HIL: {'PASS' if report['pass'] else 'FAIL'}; report={path}")
    else:
        print(text)
def main():
    args = arguments()  # --help works without importing PyUSB.
    report = {"schema": "linkr.webusb-hil.v1", "bus": args.bus, "address": args.address,
              "serial": args.serial, "url": args.expected_url, "zero_length_count_requested": args.zero_length_count,
              "zero_length_count_completed": 0, "truncation_cases": [],
              "invalid_cases": [], "stage": "import", "pass": False}
    device = usb_util = None
    try:
        import usb.core
        import usb.util
        usb_util, report["stage"] = usb.util, "find"
        device = usb.core.find(bus=args.bus, address=args.address)
        require(device is not None, f"no USB device at bus {args.bus} address {args.address}")
        require((device.idVendor, device.idProduct) == (args.vid, args.pid), f"VID:PID mismatch: {device.idVendor:04x}:{device.idProduct:04x}")
        report["stage"] = "device"
        first_device, bcd_usb = get_device_descriptor(device)
        serial = usb.util.get_string(device, first_device[16]) if first_device[16] else None
        require(serial == args.serial, f"serial mismatch: expected {args.serial!r}, received {serial!r}")
        report["bcdUSB"], report["stage"] = f"0x{bcd_usb:04x}", "bos"
        first_bos, version, code, index = get_bos(device)
        report["webusb"] = {"version": f"0x{version:04x}", "vendor_code": code, "landing_index": index}
        report["stage"] = "url"
        first_url = get_url(device, code, index, 255, args.expected_url)
        for length in (1, 3, len(first_url) - 1):
            case = {"wLength": length, "result": "running"}
            report["truncation_cases"].append(case)
            got = bytes(device.ctrl_transfer(0xC0, code, index, 2, length))
            require(got == first_url[:length], f"URL truncation wLength={length} received {len(got)} bytes")
            case.update(result="pass", received=len(got))
        report["stage"] = "invalid"
        for value, request_index in ((0, 2), (index + 1, 2), (0x100 | index, 2), (index, 1), (index, 0x102)):
            case = {"wValue": value, "wIndex": request_index, "result": "running"}
            report["invalid_cases"].append(case)
            reject(device, usb.core, code, value, request_index, len(first_url), case)
        get_url(device, code, index, 255, args.expected_url)
        report["stage"] = "zero-length"
        report["zero_length_expected"] = "pipe: no data-stage request has no OUT handler"
        for sequence in range(1, args.zero_length_count + 1):
            case = {"sequence": sequence}
            reject(device, usb.core, code, index, 2, 0, case)
            report["zero_length_count_completed"] = sequence
            if sequence % 8 == 0:
                get_url(device, code, index, 255, args.expected_url)
        report["stage"] = "final"
        final_device, _ = get_device_descriptor(device)
        final_bos, final_version, final_code, final_index = get_bos(device)
        final_url = get_url(device, code, index, 255, args.expected_url)
        require((final_device, final_bos, final_version, final_code, final_index, final_url) == (first_device, first_bos, version, code, index, first_url), "final descriptors changed")
        report.update({"stage": "complete", "pass": True})
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        if device is not None and usb_util is not None:
            try:
                usb_util.dispose_resources(device)
            except Exception as error:
                report.update({"pass": False, "stage": "dispose", "error": f"{type(error).__name__}: {error}"})
        emit(report, args.output)
    return 0 if report["pass"] else 1
if __name__ == "__main__":
    sys.exit(main())
