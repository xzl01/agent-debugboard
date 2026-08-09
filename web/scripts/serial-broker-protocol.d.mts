export const SERIAL_BROKER_PROTOCOL: "linkr-serial-broker.v1";
export const SERIAL_BROKER_CHANNELS: readonly ["uart0", "uart1"];
export const SERIAL_BROKER_CAPABILITIES: Readonly<{
  channels: readonly ["uart0", "uart1"];
  encodings: readonly ["utf8", "base64"];
  shared_read: true;
  ordered_write: true;
  exclusive_write: true;
  observer_redaction: "line_token";
}>;

export type SerialBrokerChannel = "uart0" | "uart1";

export interface SerialBrokerFrame {
  protocol: typeof SERIAL_BROKER_PROTOCOL;
  type?: string;
  request_id?: string;
  client_id?: string;
  channel?: SerialBrokerChannel;
  text?: string;
  message?: string;
  code?: string;
  path?: string;
  baud?: number;
  bytes?: number;
  byte_count?: number;
  connected?: boolean;
  opening?: boolean;
  subscribers?: number;
  owner?: { client_id?: string; label?: string } | null;
  [key: string]: unknown;
}

export function createSerialBrokerRequestId(prefix?: string): string;
export function serialBrokerFrame(type: string, fields?: Record<string, unknown>): Record<string, unknown>;
export function serialBrokerOpenRequest(
  channel: SerialBrokerChannel,
  baud: number,
  requestId?: string,
): Record<string, unknown>;
export function serialBrokerWriteRequest(
  channel: SerialBrokerChannel,
  text: string,
  requestId?: string,
  options?: { observerRedactionToken?: string },
): Record<string, unknown>;
export function serialBrokerCloseRequest(
  channel: SerialBrokerChannel,
  requestId?: string,
): Record<string, unknown>;
export function serialBrokerClaimRequest(
  channel: SerialBrokerChannel,
  owner: string,
  requestId?: string,
): Record<string, unknown>;
export function serialBrokerReleaseRequest(
  channel: SerialBrokerChannel,
  requestId?: string,
): Record<string, unknown>;
export function normalizeSerialBrokerChannel(value: unknown): SerialBrokerChannel;
export function normalizeSerialBrokerBaud(value: unknown): number;
export function parseSerialBrokerClientFrame(
  raw: unknown,
):
  | { ok: true; message: Record<string, unknown> }
  | { ok: false; error: { code: string; message: string; request_id?: string } };
export function parseSerialBrokerServerFrame(raw: unknown): SerialBrokerFrame | null;
