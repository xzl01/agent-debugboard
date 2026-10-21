import { describe, expect, it } from "vitest";
import { isJ4SerialBaudRate, J4_SERIAL_BAUD_MIN, J4_SERIAL_BAUD_MAX, J4_SERIAL_BAUD_RATES, parseJ4SerialState, parseJ4TerminalResponder } from "./j4SerialProtocol";

function report(baud: unknown) {
  return { generation: 7, state: "ready", dtr: false, read_only: false,
    line_coding: { baud, data_bits: 8, parity: "none", stop_bits: 1, flow_control: "none" } };
}

describe("J4 inclusive integer baud contract", () => {
  it.each([4800, 5200, 9600, 115200])("accepts authoritative non-preset/boundary baud %s", (baud) => {
    expect(isJ4SerialBaudRate(baud)).toBe(true);
    expect(parseJ4SerialState(report(baud)).config.baud).toBe(baud);
  });

  it.each([4799, 115201, 5200.5, -1, NaN, Infinity, "5200", undefined, null])("rejects invalid reported baud %s", (baud) => {
    expect(isJ4SerialBaudRate(baud)).toBe(false);
    expect(() => parseJ4SerialState(report(baud))).toThrow("Invalid J4 serial state");
  });

  it("retains five recommended presets without making them a whitelist", () => {
    expect([J4_SERIAL_BAUD_MIN, J4_SERIAL_BAUD_MAX]).toEqual([4800, 115200]);
    expect(J4_SERIAL_BAUD_RATES).toEqual([9600, 19200, 38400, 57600, 115200]);
    expect(J4_SERIAL_BAUD_RATES.every(isJ4SerialBaudRate)).toBe(true);
    expect(J4_SERIAL_BAUD_RATES).not.toContain(5200);
    expect(isJ4SerialBaudRate(5200)).toBe(true);
  });
});

describe("J4 automatic terminal responder, not an exclusive TX owner", () => {
  it.each([null, undefined, false])("keeps automatic replies off for unselected state %s, not manual input", (responder) => {
    expect(parseJ4TerminalResponder(responder)).toBe(false);
    const parsed = parseJ4SerialState({ ...report(5200), terminal_responder: responder });
    expect(parsed.terminal_responder).toBe(false);
    expect(parsed.read_only).toBe(false);
  });

  it.each([{ transport: "cdc" }, { transport: "websocket", slot: 0 }, { transport: "raw_tcp", slot: 3 }, { transport: "websocket" }])("retains the responder descriptor %j without guessing this page is selected", (responder) => {
    expect(parseJ4TerminalResponder(responder)).toEqual(responder);
    const parsed = parseJ4SerialState({ ...report(5200), terminal_responder: responder });
    expect(parsed.responder).toEqual(responder);
    expect(parsed.terminal_responder).toBe(false);
    expect(parsed.terminal_responder).not.toBe(true);
    expect(parsed.read_only).toBe(false);
  });

  it("parses the exact WS boolean plus responder descriptor without promoting other tabs", () => {
    const responder = { transport: "websocket", slot: 2 };
    const selected = parseJ4SerialState({ ...report(5200), terminal_responder: true, responder });
    const observer = parseJ4SerialState({ ...report(5200), terminal_responder: false, responder });
    expect(selected).toMatchObject({ terminal_responder: true, responder, read_only: false });
    expect(observer).toMatchObject({ terminal_responder: false, responder, read_only: false });
    expect(() => parseJ4SerialState({ ...report(5200), terminal_responder: false, responder: true })).toThrow();
  });

  it.each(["none", "cdc", [], {}, { transport: "none" }, { transport: "telnet" }, { transport: ["cdc"] },
    { transport: "websocket", slot: -1 }, { transport: "raw_tcp", slot: 0.5 }, { transport: "websocket", slot: "1" },
    { transport: "cdc", slot: Infinity }, { transport: "raw_tcp", slot: null }])("rejects malformed responder %j", (responder) => {
    expect(() => parseJ4SerialState({ ...report(5200), terminal_responder: responder })).toThrow("Invalid J4 terminal responder");
  });

  it("uses explicit true for automatic replies and DTR only disables the responder, never manual TX", () => {
    expect(parseJ4SerialState({ ...report(5200), terminal_responder: true }).terminal_responder).toBe(true);
    const cdc = parseJ4SerialState({ ...report(5200), terminal_responder: true, dtr: true });
    expect(cdc.terminal_responder).toBe(false);
    expect(cdc.read_only).toBe(false);
    for (const extra of [{ read_only: true }, { queue: { admission_frozen: true } }]) {
      expect(parseJ4SerialState({ ...report(5200), terminal_responder: false, ...extra }).read_only).toBe(true);
    }
    expect(() => parseJ4SerialState({ ...report(5200), read_only: "false" })).toThrow();
  });
});
