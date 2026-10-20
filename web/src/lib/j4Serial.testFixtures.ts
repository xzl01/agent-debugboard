import type { J4SerialSessionSnapshot } from "./j4SerialSession";

/** Offline serial state only; this fixture never connects to a device. */
export function j4ReadySnapshot(changes: Partial<J4SerialSessionSnapshot> = {}): J4SerialSessionSnapshot {
  return {
    phase: "connected", serial: { generation: 1, state: "ready", dtr: false, read_only: false,
      config: { baud: 115200, data_bits: 8, parity: "none", stop_bits: 1, flow_control: "none" } },
    error: null, backpressure: false, pendingWrites: 0, droppedBytes: 0,
    rxBytes: 0, txBytes: 0, configuring: false, automationActive: false, ...changes,
  };
}
