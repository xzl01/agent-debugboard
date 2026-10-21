/** J4's text control frames share a socket with unwrapped binary UART data. */
export const J4_SERIAL_TOPIC = "serial.j4";
export type J4SerialChannel = "j4";
export const J4_SERIAL_FRAME_BYTES = 480;
export const J4_SERIAL_QUEUE_LIMIT = 8;
export const J4_SERIAL_BUFFER_LIMIT = J4_SERIAL_FRAME_BYTES * J4_SERIAL_QUEUE_LIMIT;
export const J4_SERIAL_BAUD_MIN = 4800;
export const J4_SERIAL_BAUD_MAX = 115200;
// Recommended presets, not an exhaustive list of legal firmware baud rates.
export const J4_SERIAL_BAUD_RATES = [9600, 19200, 38400, 57600, 115200];

export function isJ4SerialBaudRate(value: unknown): value is number {
  return typeof value === "number" && Number.isSafeInteger(value)
    && value >= J4_SERIAL_BAUD_MIN && value <= J4_SERIAL_BAUD_MAX;
}

export interface J4SerialConfig {
  baud: number;
  data_bits: 5 | 6 | 7 | 8;
  parity: "none" | "odd" | "even";
  stop_bits: 1 | 2;
  flow_control: "none";
}

// A descriptor identifies the selected responder, not this browser's identity.
// Only an explicit per-client `true` authorizes automatic terminal replies.
export type J4TerminalResponderDescriptor = { transport: "cdc" | "websocket" | "raw_tcp"; slot?: number };
export type J4TerminalResponder = boolean | J4TerminalResponderDescriptor;

export interface J4SerialState {
  generation: number;
  state: "ready" | "reconfig" | "paused" | "faulted";
  config: J4SerialConfig;
  dtr: boolean;
  read_only: boolean;
  terminal_responder: boolean;
  responder: J4TerminalResponderDescriptor | null;
  last_config_request_id?: number;
  last_config_done?: boolean;
  last_config_result?: number;
}

export function record(value: unknown): Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown> : {};
}

export function parseJ4TerminalResponder(value: unknown): J4TerminalResponder {
  if (value === undefined || value === null) return false;
  if (typeof value === "boolean") return value;
  const responder = record(value);
  if ((responder.transport !== "cdc" && responder.transport !== "websocket" && responder.transport !== "raw_tcp")
    || (responder.slot !== undefined && (!Number.isSafeInteger(responder.slot) || Number(responder.slot) < 0))) {
    throw new Error("Invalid J4 terminal responder");
  }
  return { transport: responder.transport as Exclude<J4TerminalResponder, boolean>["transport"],
    ...(responder.slot === undefined ? {} : { slot: Number(responder.slot) }) };
}

export function parseJ4SerialState(value: unknown): J4SerialState {
  const data = record(value);
  if (data.available === false || data.state === "unavailable") throw new J4SerialError("unavailable");
  const config = record(data.line_coding);
  const request = record(data.config_request);
  const responder = parseJ4TerminalResponder(data.terminal_responder);
  const descriptor = data.responder === undefined ? (typeof responder === "object" ? responder : null)
    : data.responder === null ? null : parseJ4TerminalResponder(data.responder);
  if (typeof descriptor === "boolean") throw new Error("Invalid J4 terminal responder descriptor");
  if (!Number.isSafeInteger(data.generation) || Number(data.generation) < 0
    || !["ready", "reconfig", "paused", "faulted"].includes(String(data.state))
    || !isJ4SerialBaudRate(config.baud)
    || typeof config.data_bits !== "number" || ![5, 6, 7, 8].includes(config.data_bits)
    || !["none", "odd", "even"].includes(String(config.parity))
    || (config.stop_bits !== 1 && config.stop_bits !== 2)
    || config.flow_control !== "none"
    || typeof data.dtr !== "boolean"
    || (data.read_only !== undefined && typeof data.read_only !== "boolean")) {
    throw new Error("Invalid J4 serial state");
  }
  return {
    generation: Number(data.generation), state: data.state as J4SerialState["state"],
    config: config as unknown as J4SerialConfig, dtr: data.dtr,
    read_only: data.read_only === true || record(data.queue).admission_frozen === true,
    terminal_responder: !data.dtr && responder === true,
    responder: descriptor,
    ...(typeof request.id === "number" ? { last_config_request_id: request.id } : {}),
    ...(typeof request.done === "boolean" ? { last_config_done: request.done } : {}),
    ...(typeof request.result === "number" ? { last_config_result: request.result } : {}),
  };
}

export class J4SerialError extends Error {
  constructor(public readonly code: string, message = code) {
    super(message);
    this.name = "J4SerialError";
  }
}
