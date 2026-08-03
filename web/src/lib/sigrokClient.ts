export const SIGROK_MAGIC = 0x72;
export const SIGROK_PROTOCOL_VERSION = 1;
export const SIGROK_HEADER_BYTES = 9;
export const SIGROK_SAMPLE_INDEX_BITS = 24;
export const SIGROK_SAMPLE_INDEX_MODULO = 1 << SIGROK_SAMPLE_INDEX_BITS;
const SIGROK_CONNECT_TIMEOUT_MS = 10_000;

export const SigrokFrameType = {
  HELLO_REQ: 0x01,
  HELLO_RESP: 0x02,
  CAPS_REQ: 0x03,
  CAPS_RESP: 0x04,
  CONFIG_REQ: 0x05,
  CONFIG_RESP: 0x06,
  START_REQ: 0x07,
  START_RESP: 0x08,
  STOP_REQ: 0x09,
  STOP_RESP: 0x0a,
  CONFIG_V2_REQ: 0x0b,
  EVENT: 0x10,
  DATA: 0x11,
  ERROR: 0x7f,
} as const;

export const SigrokServerFlag = {
  CONFIG_V2: 1 << 0,
  GENERIC_PACKED_BURST: 1 << 1,
} as const;

export type SigrokServerFlag = typeof SigrokServerFlag[keyof typeof SigrokServerFlag];

export type SigrokConfigEncoding = "auto" | "v1" | "v2";

export interface SigrokConfigureOptions {
  encoding?: SigrokConfigEncoding;
}

export type SigrokFrameType = typeof SigrokFrameType[keyof typeof SigrokFrameType];

export const SigrokSessionState = {
  IDLE: 0,
  CONFIGURED: 1,
  ARMED: 2,
  RUNNING: 3,
  STOPPED: 4,
} as const;

export type SigrokSessionState = typeof SigrokSessionState[keyof typeof SigrokSessionState];

export const SigrokEventCode = {
  ARMED: 1,
  TRIGGERED: 2,
  RUNNING: 3,
  STOPPED: 4,
  OVERRUN: 5,
  ERROR: 6,
} as const;

export type SigrokEventCode = typeof SigrokEventCode[keyof typeof SigrokEventCode];

export const SigrokErrorCode = {
  INVALID_TYPE: 1,
  INVALID_LENGTH: 2,
  UNSUPPORTED_VERSION: 3,
  OVERSIZE_PAYLOAD: 4,
  INTERNAL: 5,
  INVALID_STATE: 6,
  INVALID_CONFIG: 7,
  BUSY: 8,
} as const;

export type SigrokErrorCode = typeof SigrokErrorCode[keyof typeof SigrokErrorCode];

export const SigrokModeId = {
  FAST8: 1,
  WIDE11: 2,
} as const;

export type SigrokModeId = typeof SigrokModeId[keyof typeof SigrokModeId];

export const SigrokModeFlag = {
  CONTINUOUS: 1 << 0,
  TRIGGER_NONE: 1 << 1,
  TRIGGER_RISING: 1 << 2,
  TRIGGER_FALLING: 1 << 3,
  TRIGGER_EITHER: 1 << 4,
  PRE_TRIGGER: 1 << 5,
} as const;

export type SigrokModeFlag = typeof SigrokModeFlag[keyof typeof SigrokModeFlag];

export const SigrokTriggerType = {
  NONE: 0,
  RISING: 1,
  FALLING: 2,
  EITHER: 3,
} as const;

export type SigrokTriggerType = typeof SigrokTriggerType[keyof typeof SigrokTriggerType];

export const SigrokCompression = {
  RAW: 0,
  BIT_PACK: 1,
  RLE: 2,
  BIT_PACK_RLE: 3,
} as const;

export type SigrokCompression = typeof SigrokCompression[keyof typeof SigrokCompression];

export interface SigrokHeader {
  magic: number;
  version: number;
  type: number;
  id: number;
  payloadLen: number;
}

export interface SigrokHelloResp {
  protocolVersion: number;
  serverFlags: number;
  modeCount: number;
  maxPayloadLen: number;
}

export interface SigrokServerCapabilities {
  readonly hello: SigrokHelloResp | null;
  readonly caps: SigrokCapsResp | null;
  readonly serverFlags: number;
  readonly supportsConfigV2: boolean;
  readonly supportsGenericPackedBurst: boolean;
}

export interface SigrokModeCaps {
  readonly modeId: number;
  readonly modeFlags: number;
  readonly channelCount: number;
  readonly sampleBytes: number;
  readonly maxSamplerateKhz: number;
  readonly compression: number;
}

export interface SigrokCapsResp {
  readonly modeCount: number;
  readonly modes: readonly SigrokModeCaps[];
}

export interface SigrokConfigReq {
  modeId: number;
  triggerType: number;
  triggerChannel: number;
  channelMask: number;
  samplerateKhz: number;
  preSamples: number;
  postSamples: number;
}

export interface SigrokConfigFrameRequest {
  type: typeof SigrokFrameType.CONFIG_REQ | typeof SigrokFrameType.CONFIG_V2_REQ;
  payload: Uint8Array;
}

function assertUintRange(value: number, bits: number, fieldName: string): void {
  const max = bits === 32 ? 0xffffffff : (1 << bits) - 1;
  if (!Number.isInteger(value) || value < 0 || value > max) {
    throw new Error(`Sigrok ${fieldName} must fit uint${bits}`);
  }
}

function configNeedsV2(config: SigrokConfigReq): boolean {
  return config.preSamples > 0xffff || config.postSamples > 0xffff;
}

function buildSigrokConfigPayloadV1(config: SigrokConfigReq): Uint8Array {
  assertUintRange(config.modeId, 8, "modeId");
  assertUintRange(config.triggerType, 8, "triggerType");
  assertUintRange(config.triggerChannel, 8, "triggerChannel");
  assertUintRange(config.channelMask, 16, "channelMask");
  assertUintRange(config.samplerateKhz, 24, "samplerateKhz");
  assertUintRange(config.preSamples, 16, "preSamples");
  assertUintRange(config.postSamples, 16, "postSamples");

  const payload = new Uint8Array(12);
  payload[0] = config.modeId;
  payload[1] = config.triggerType;
  payload[2] = config.triggerChannel;
  payload[3] = config.channelMask & 0xff;
  payload[4] = (config.channelMask >> 8) & 0xff;
  payload[5] = config.samplerateKhz & 0xff;
  payload[6] = (config.samplerateKhz >> 8) & 0xff;
  payload[7] = (config.samplerateKhz >> 16) & 0xff;
  payload[8] = config.preSamples & 0xff;
  payload[9] = (config.preSamples >> 8) & 0xff;
  payload[10] = config.postSamples & 0xff;
  payload[11] = (config.postSamples >> 8) & 0xff;
  return payload;
}

function buildSigrokConfigPayloadV2(config: SigrokConfigReq): Uint8Array {
  assertUintRange(config.modeId, 8, "modeId");
  assertUintRange(config.triggerType, 8, "triggerType");
  assertUintRange(config.triggerChannel, 8, "triggerChannel");
  assertUintRange(config.channelMask, 16, "channelMask");
  assertUintRange(config.samplerateKhz, 24, "samplerateKhz");
  assertUintRange(config.preSamples, 32, "preSamples");
  assertUintRange(config.postSamples, 32, "postSamples");

  const payload = new Uint8Array(16);
  payload[0] = config.modeId;
  payload[1] = config.triggerType;
  payload[2] = config.triggerChannel;
  payload[3] = config.channelMask & 0xff;
  payload[4] = (config.channelMask >> 8) & 0xff;
  payload[5] = config.samplerateKhz & 0xff;
  payload[6] = (config.samplerateKhz >> 8) & 0xff;
  payload[7] = (config.samplerateKhz >> 16) & 0xff;
  payload[8] = config.preSamples & 0xff;
  payload[9] = (config.preSamples >> 8) & 0xff;
  payload[10] = (config.preSamples >> 16) & 0xff;
  payload[11] = (config.preSamples >> 24) & 0xff;
  payload[12] = config.postSamples & 0xff;
  payload[13] = (config.postSamples >> 8) & 0xff;
  payload[14] = (config.postSamples >> 16) & 0xff;
  payload[15] = (config.postSamples >> 24) & 0xff;
  return payload;
}

function freezeSigrokCaps(caps: SigrokCapsResp): SigrokCapsResp {
  const modes = Object.freeze(caps.modes.map((mode) => Object.freeze({ ...mode })));
  return Object.freeze({ modeCount: caps.modeCount, modes });
}

export function buildSigrokConfigFrameRequest(
  config: SigrokConfigReq,
  capabilities: Pick<SigrokServerCapabilities, "supportsConfigV2"> = { supportsConfigV2: false },
  options: SigrokConfigureOptions = {}
): SigrokConfigFrameRequest {
  const encoding = options.encoding ?? "auto";
  if (encoding === "v1") {
    return { type: SigrokFrameType.CONFIG_REQ, payload: buildSigrokConfigPayloadV1(config) };
  }
  if (encoding === "v2") {
    if (!capabilities.supportsConfigV2) {
      throw new Error("CONFIG_V2 requested but server did not advertise support");
    }
    return { type: SigrokFrameType.CONFIG_V2_REQ, payload: buildSigrokConfigPayloadV2(config) };
  }
  if (!configNeedsV2(config)) {
    return { type: SigrokFrameType.CONFIG_REQ, payload: buildSigrokConfigPayloadV1(config) };
  }
  if (!capabilities.supportsConfigV2) {
    throw new Error("Requested sigrok pre/post samples require CONFIG_V2, but server did not advertise support");
  }
  return { type: SigrokFrameType.CONFIG_V2_REQ, payload: buildSigrokConfigPayloadV2(config) };
}

export interface SigrokAck {
  sessionId: number;
  state: number;
  actualRateKhz: number;
}

export interface SigrokEvent {
  sessionId: number;
  typeDetail: number;
  sampleIndex: number;
}

export interface SigrokDataMeta {
  sampleIndex: number;
  sampleCount: number;
  compression: number;
  channelMask: number;
}

export interface SigrokError {
  errorCode: number;
  detail: number;
}

function countChannelBits(channelMask: number): number {
  let count = 0;
  let remaining = channelMask >>> 0;
  while (remaining > 0) {
    count += remaining & 1;
    remaining >>>= 1;
  }
  return count;
}

function getPackedSampleWidth(channelMask: number): { activeChannelCount: number; bytesPerSample: number } {
  const activeChannelCount = countChannelBits(channelMask);
  if (activeChannelCount <= 0) {
    throw new Error("DATA frame channel mask selects no channels");
  }
  return {
    activeChannelCount,
    bytesPerSample: Math.ceil(activeChannelCount / 8),
  };
}

function decodeBitPackSamples(
  packed: Uint8Array,
  sampleCount: number,
  channelMask: number
): Uint8Array {
  const { bytesPerSample } = getPackedSampleWidth(channelMask);
  const expectedPackedBytes = sampleCount * bytesPerSample;
  if (packed.length !== expectedPackedBytes) {
    throw new Error("BIT_PACK DATA frame length does not match sampleCount and channelMask");
  }

  return packed;
}

function decodeRleSamples(
  packed: Uint8Array,
  sampleCount: number,
  channelMask: number,
  compressionName: "RLE" | "BIT_PACK_RLE"
): Uint8Array {
  const { bytesPerSample } = getPackedSampleWidth(channelMask);
  const tupleWidth = bytesPerSample + 2;
  const decoded = new Uint8Array(sampleCount * bytesPerSample);
  let packedOffset = 0;
  let expandedSamples = 0;
  let writeOffset = 0;

  while (packedOffset < packed.length) {
    if (packedOffset + tupleWidth > packed.length) {
      throw new Error(`${compressionName} DATA frame tuple is truncated`);
    }

    const runValue = packed.subarray(packedOffset, packedOffset + bytesPerSample);
    const runCount =
      packed[packedOffset + bytesPerSample] |
      (packed[packedOffset + bytesPerSample + 1] << 8);
    if (runCount === 0) {
      throw new Error(`${compressionName} DATA frame contains zero-length run`);
    }
    if (expandedSamples + runCount > sampleCount) {
      throw new Error(`${compressionName} DATA frame expands beyond advertised sampleCount`);
    }

    for (let repeatIndex = 0; repeatIndex < runCount; repeatIndex += 1) {
      decoded.set(runValue, writeOffset);
      writeOffset += bytesPerSample;
    }

    expandedSamples += runCount;
    packedOffset += tupleWidth;
  }

  if (expandedSamples !== sampleCount) {
    throw new Error(
      `${compressionName} DATA frame expanded sample count does not match advertised sampleCount`
    );
  }

  return decoded;
}

function decodeSigrokDataSamples(meta: SigrokDataMeta, packed: Uint8Array): Uint8Array {
  switch (meta.compression) {
    case SigrokCompression.RAW:
      return packed;
    case SigrokCompression.BIT_PACK:
      return decodeBitPackSamples(packed, meta.sampleCount, meta.channelMask);
    case SigrokCompression.RLE:
      return decodeRleSamples(packed, meta.sampleCount, meta.channelMask, "RLE");
    case SigrokCompression.BIT_PACK_RLE:
      return decodeRleSamples(packed, meta.sampleCount, meta.channelMask, "BIT_PACK_RLE");
    default:
      throw new Error(`Unsupported DATA compression ${meta.compression}`);
  }
}

export function sigrokEventCodeName(code: number): string {
  switch (code) {
    case SigrokEventCode.ARMED:
      return "ARMED";
    case SigrokEventCode.TRIGGERED:
      return "TRIGGERED";
    case SigrokEventCode.RUNNING:
      return "RUNNING";
    case SigrokEventCode.STOPPED:
      return "STOPPED";
    case SigrokEventCode.OVERRUN:
      return "OVERRUN";
    case SigrokEventCode.ERROR:
      return "ERROR";
    default:
      return `UNKNOWN(${code})`;
  }
}

export function formatSigrokErrorMessage(error: SigrokError): string {
  const codeName = Object.entries(SigrokErrorCode).find(([, value]) => value === error.errorCode)?.[0];
  return codeName == null
    ? `Sigrok error ${error.errorCode}: ${error.detail}`
    : `Sigrok error ${codeName}: ${error.detail}`;
}

export type SigrokFrame =
  | { type: typeof SigrokFrameType.HELLO_RESP; id: number; payload: SigrokHelloResp }
  | { type: typeof SigrokFrameType.CAPS_RESP; id: number; payload: SigrokCapsResp }
  | { type: typeof SigrokFrameType.CONFIG_RESP; id: number; payload: SigrokAck }
  | { type: typeof SigrokFrameType.START_RESP; id: number; payload: SigrokAck }
  | { type: typeof SigrokFrameType.STOP_RESP; id: number; payload: SigrokAck }
  | { type: typeof SigrokFrameType.EVENT; id: number; payload: SigrokEvent }
  | { type: typeof SigrokFrameType.DATA; id: number; meta: SigrokDataMeta; samples: Uint8Array }
  | { type: typeof SigrokFrameType.ERROR; id: number; payload: SigrokError };

export type SigrokEventFrame = Extract<SigrokFrame, { type: typeof SigrokFrameType.EVENT }>;
export type SigrokDataFrame = Extract<SigrokFrame, { type: typeof SigrokFrameType.DATA }>;
export type SigrokErrorFrame = Extract<SigrokFrame, { type: typeof SigrokFrameType.ERROR }>;

export function isSigrokEventFrame(frame: SigrokFrame): frame is SigrokEventFrame {
  return frame.type === SigrokFrameType.EVENT;
}

export function isSigrokDataFrame(frame: SigrokFrame): frame is SigrokDataFrame {
  return frame.type === SigrokFrameType.DATA;
}

export function isSigrokErrorFrame(frame: SigrokFrame): frame is SigrokErrorFrame {
  return frame.type === SigrokFrameType.ERROR;
}

export type SigrokClientState = "disconnected" | "connecting" | "ready" | "configured" | "armed" | "running";

export type SigrokClientEvent =
  | { type: "state"; state: SigrokClientState }
  | { type: "frame"; frame: SigrokFrame }
  | { type: "error"; message: string }
  | { type: "data"; meta: SigrokDataMeta; samples: Uint8Array };

export type SigrokClientEventListener = (event: SigrokClientEvent) => void;

export function buildSigrokFrame(
  id: number,
  type: SigrokFrameType,
  payload: Uint8Array = new Uint8Array(0)
): Uint8Array {
  const frame = new Uint8Array(SIGROK_HEADER_BYTES + payload.length);
  frame[0] = SIGROK_MAGIC;
  frame[1] = SIGROK_PROTOCOL_VERSION;
  frame[2] = type;
  frame[3] = id & 0xff;
  frame[4] = (id >> 8) & 0xff;
  frame[5] = (id >> 16) & 0xff;
  frame[6] = (id >> 24) & 0xff;
  frame[7] = payload.length & 0xff;
  frame[8] = (payload.length >> 8) & 0xff;
  if (payload.length > 0) {
    frame.set(payload, SIGROK_HEADER_BYTES);
  }
  return frame;
}

export function parseSigrokHeader(data: Uint8Array): SigrokHeader | null {
  if (data.length < SIGROK_HEADER_BYTES) {
    return null;
  }

  const magic = data[0];
  if (magic !== SIGROK_MAGIC) {
    return null;
  }

  return {
    magic,
    version: data[1],
    type: data[2],
    id: data[3] | (data[4] << 8) | (data[5] << 16) | (data[6] << 24),
    payloadLen: data[7] | (data[8] << 8),
  };
}

export function parseSigrokFrame(header: SigrokHeader, data: Uint8Array): SigrokFrame | null {
  const payload = data.slice(SIGROK_HEADER_BYTES);

  switch (header.type) {
    case SigrokFrameType.HELLO_RESP:
      return {
        type: SigrokFrameType.HELLO_RESP,
        id: header.id,
        payload: {
          protocolVersion: payload[0],
          serverFlags: payload[1],
          modeCount: payload[2],
          maxPayloadLen: payload[3] | (payload[4] << 8),
        },
      };

    case SigrokFrameType.CAPS_RESP: {
      const modeCount = payload[0];
      const modes: SigrokModeCaps[] = [];
      for (let i = 0; i < modeCount; i++) {
        const offset = 1 + i * 8;
        modes.push({
          modeId: payload[offset],
          modeFlags: payload[offset + 1],
          channelCount: payload[offset + 2],
          sampleBytes: payload[offset + 3],
          maxSamplerateKhz:
            payload[offset + 4] |
            (payload[offset + 5] << 8) |
            (payload[offset + 6] << 16),
          compression: payload[offset + 7],
        });
      }
      return { type: SigrokFrameType.CAPS_RESP, id: header.id, payload: { modeCount, modes } };
    }

    case SigrokFrameType.CONFIG_RESP:
    case SigrokFrameType.START_RESP:
    case SigrokFrameType.STOP_RESP:
      return {
        type:
          header.type as
            | typeof SigrokFrameType.CONFIG_RESP
            | typeof SigrokFrameType.START_RESP
            | typeof SigrokFrameType.STOP_RESP,
        id: header.id,
        payload: {
          sessionId: payload[0] | (payload[1] << 8),
          state: payload[2],
          actualRateKhz: payload[3] | (payload[4] << 8) | (payload[5] << 16),
        },
      };

    case SigrokFrameType.EVENT:
      return {
        type: SigrokFrameType.EVENT,
        id: header.id,
        payload: {
          sessionId: payload[0] | (payload[1] << 8),
          typeDetail: payload[2],
          sampleIndex: payload[3] | (payload[4] << 8) | (payload[5] << 16),
        },
      };

    case SigrokFrameType.DATA: {
      if (payload.length < 8) {
        throw new Error("DATA payload shorter than 8-byte metadata");
      }
      const meta: SigrokDataMeta = {
        sampleIndex: payload[0] | (payload[1] << 8) | (payload[2] << 16),
        sampleCount: payload[3] | (payload[4] << 8),
        compression: payload[5],
        channelMask: payload[6] | (payload[7] << 8),
      };
      const samples = decodeSigrokDataSamples(meta, payload.slice(8));
      return { type: SigrokFrameType.DATA, id: header.id, meta, samples };
    }

    case SigrokFrameType.ERROR:
      return {
        type: SigrokFrameType.ERROR,
        id: header.id,
        payload: {
          errorCode: payload[0],
          detail: payload[1] | (payload[2] << 8),
        },
      };

    default:
      return null;
  }
}

export class SigrokClient {
  private ws: WebSocket | null = null;
  private connectionGeneration = 0;
  private connectCancellation: ((error: Error) => void) | null = null;
  private connectTimeout: ReturnType<typeof setTimeout> | null = null;
  private state: SigrokClientState = "disconnected";
  private nextId = 1;
  private listeners: SigrokClientEventListener[] = [];
  private pendingRequests = new Map<number, { resolve: (frame: SigrokFrame) => void; reject: (err: Error) => void }>();
  private buffer = new Uint8Array(0);
  private disconnecting = false;
  private hello: SigrokHelloResp | null = null;
  private caps: SigrokCapsResp | null = null;

  addEventListener(listener: SigrokClientEventListener): void {
    this.listeners.push(listener);
  }

  removeEventListener(listener: SigrokClientEventListener): void {
    this.listeners = this.listeners.filter(l => l !== listener);
  }

  private emit(event: SigrokClientEvent): void {
    for (const listener of this.listeners) {
      listener(event);
    }
  }

  getState(): SigrokClientState {
    return this.state;
  }

  getServerCapabilities(): SigrokServerCapabilities {
    const serverFlags = this.hello?.serverFlags ?? 0;
    return {
      hello: this.hello,
      caps: this.caps,
      serverFlags,
      supportsConfigV2: (serverFlags & SigrokServerFlag.CONFIG_V2) !== 0,
      supportsGenericPackedBurst:
        (serverFlags & SigrokServerFlag.GENERIC_PACKED_BURST) !== 0,
    };
  }

  private setState(state: SigrokClientState): void {
    this.state = state;
    this.emit({ type: "state", state });
  }

  private clearCapabilities(): void {
    this.hello = null;
    this.caps = null;
  }

  async connect(url: string): Promise<void> {
    if (this.ws || this.connectCancellation) {
      this.disconnect();
    }

    const generation = ++this.connectionGeneration;
    this.disconnecting = false;
    this.buffer = new Uint8Array(0);
    this.clearCapabilities();
    this.setState("connecting");

    return new Promise((resolve, reject) => {
      let ws: WebSocket;
      try {
        ws = new WebSocket(url);
      } catch (error) {
        this.setState("disconnected");
        reject(error instanceof Error ? error : new Error(String(error)));
        return;
      }

      ws.binaryType = "arraybuffer";
      this.ws = ws;

      let settled = false;
      let attemptTimeout: ReturnType<typeof setTimeout> | null = null;
      const isCurrent = () => this.ws === ws && this.connectionGeneration === generation;
      const clearAttempt = () => {
        if (attemptTimeout != null) {
          clearTimeout(attemptTimeout);
          if (this.connectTimeout === attemptTimeout) {
            this.connectTimeout = null;
          }
          attemptTimeout = null;
        } else if (this.connectTimeout != null && this.connectCancellation === rejectConnect) {
          clearTimeout(this.connectTimeout);
          this.connectTimeout = null;
        }
        if (this.connectCancellation === rejectConnect) {
          this.connectCancellation = null;
        }
      };
      const rejectConnect = (error: Error) => {
        if (settled) return;
        settled = true;
        clearAttempt();
        reject(error);
      };
      const failCurrentConnection = (error: Error, closeSocket = true) => {
        if (!isCurrent()) {
          rejectConnect(error);
          return;
        }
        this.connectionGeneration += 1;
        this.ws = null;
        this.buffer = new Uint8Array(0);
        if (
          closeSocket &&
          (ws.readyState === WebSocket.CONNECTING || ws.readyState === WebSocket.OPEN)
        ) {
          ws.close();
        }
        this.clearCapabilities();
        this.setState("disconnected");
        this.rejectAllPending(error);
        rejectConnect(error);
      };

      this.connectCancellation = rejectConnect;
      attemptTimeout = setTimeout(() => {
        failCurrentConnection(new Error("WebSocket connection timeout"));
      }, SIGROK_CONNECT_TIMEOUT_MS);
      this.connectTimeout = attemptTimeout;

      ws.onopen = () => {
        if (!isCurrent()) {
          if (ws.readyState === WebSocket.OPEN) ws.close();
          rejectConnect(new Error("WebSocket connection cancelled"));
          return;
        }
        this.sendHello().then(() => {
          return this.getCaps();
        }).then(() => {
          if (!isCurrent()) {
            if (ws.readyState === WebSocket.OPEN) ws.close();
            rejectConnect(new Error("WebSocket connection cancelled"));
            return;
          }
          settled = true;
          clearAttempt();
          this.setState("ready");
          resolve();
        }).catch((error) => {
          failCurrentConnection(
            error instanceof Error ? error : new Error(String(error))
          );
        });
      };

      ws.onmessage = (event) => {
        if (isCurrent() && event.data instanceof ArrayBuffer) {
          this.feedBinaryData(new Uint8Array(event.data));
        }
      };

      ws.onclose = () => {
        if (!isCurrent()) return;
        this.connectionGeneration += 1;
        this.ws = null;
        this.buffer = new Uint8Array(0);
        this.clearCapabilities();
        this.setState("disconnected");
        const error = new Error("WebSocket closed");
        this.rejectAllPending(error);
        rejectConnect(error);
      };

      ws.onerror = () => {
        failCurrentConnection(new Error("WebSocket connection failed"));
      };
    });
  }

  disconnect(): void {
    this.disconnecting = true;
    this.connectionGeneration += 1;
    const ws = this.ws;
    this.ws = null;
    const cancelConnect = this.connectCancellation;
    this.connectCancellation = null;
    if (this.connectTimeout != null) {
      clearTimeout(this.connectTimeout);
      this.connectTimeout = null;
    }
    if (
      ws &&
      (ws.readyState === WebSocket.CONNECTING || ws.readyState === WebSocket.OPEN)
    ) {
      ws.close();
    }
    this.buffer = new Uint8Array(0);
    this.clearCapabilities();
    this.setState("disconnected");
    const error = new Error("Client disconnected");
    this.rejectAllPending(error);
    cancelConnect?.(error);
  }

  async disconnectGracefully(): Promise<void> {
    this.disconnecting = true;
    if (this.state === "connecting") {
      this.disconnect();
      return;
    }
    const ws = this.ws;
    const generation = this.connectionGeneration;
    if (ws) await new Promise(resolve => setTimeout(resolve, 100));
    if (this.ws !== ws || this.connectionGeneration !== generation) return;
    this.disconnect();
  }

  private rejectAllPending(error: Error): void {
    for (const pending of this.pendingRequests.values()) {
      pending.reject(error);
    }
    this.pendingRequests.clear();
  }

  feedBinaryData(data: Uint8Array): void {
    if (this.disconnecting) return;
    const merged = new Uint8Array(this.buffer.length + data.length);
    merged.set(this.buffer);
    merged.set(data, this.buffer.length);
    this.buffer = merged;

    while (this.buffer.length >= SIGROK_HEADER_BYTES) {
      const header = parseSigrokHeader(this.buffer);
      if (!header) {
        this.buffer = this.buffer.slice(1);
        continue;
      }

      const totalLen = SIGROK_HEADER_BYTES + header.payloadLen;
      if (this.buffer.length < totalLen) {
        break;
      }

      const frameData = this.buffer.slice(0, totalLen);
      this.buffer = this.buffer.slice(totalLen);

      const frame = parseSigrokFrame(header, frameData);
      if (frame) {
        this.handleFrame(frame);
      }
    }
  }

  private handleFrame(frame: SigrokFrame): void {
    if (frame.type === SigrokFrameType.HELLO_RESP) {
      this.hello = frame.payload;
    }
    if (frame.type === SigrokFrameType.CAPS_RESP) {
      this.caps = freezeSigrokCaps(frame.payload);
    }

    this.emit({ type: "frame", frame });

    if (frame.type === SigrokFrameType.DATA) {
      this.emit({ type: "data", meta: frame.meta, samples: frame.samples });
    }

    const pending = this.pendingRequests.get(frame.id);
    if (pending) {
      this.pendingRequests.delete(frame.id);
      pending.resolve(frame);
    }

    if (frame.type === SigrokFrameType.EVENT) {
      this.handleEvent(frame.payload);
    }

    if (frame.type === SigrokFrameType.ERROR) {
      this.emit({ type: "error", message: formatSigrokErrorMessage(frame.payload) });
    }
  }

  private handleEvent(event: SigrokEvent): void {
    switch (event.typeDetail) {
      case SigrokEventCode.ARMED:
        this.setState("armed");
        break;
      case SigrokEventCode.TRIGGERED:
      case SigrokEventCode.RUNNING:
        this.setState("running");
        break;
      case SigrokEventCode.STOPPED:
        this.setState("configured");
        break;
    }
  }

  private buildFrame(type: typeof SigrokFrameType[keyof typeof SigrokFrameType], payload: Uint8Array = new Uint8Array(0)): Uint8Array {
    return buildSigrokFrame(this.nextId++, type, payload);
  }

  private sendFrame(frame: Uint8Array): void {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      throw new Error("WebSocket not connected");
    }
    this.ws.send(frame);
  }

  private sendRequest(type: typeof SigrokFrameType[keyof typeof SigrokFrameType], payload: Uint8Array = new Uint8Array(0)): Promise<SigrokFrame> {
    const frame = this.buildFrame(type, payload);
    const id = this.nextId - 1;

    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.pendingRequests.delete(id);
        reject(new Error("Request timeout"));
      }, 5000);

      this.pendingRequests.set(id, {
        resolve: (frame) => {
          clearTimeout(timeout);
          resolve(frame);
        },
        reject: (err) => {
          clearTimeout(timeout);
          reject(err);
        },
      });

      try {
        this.sendFrame(frame);
      } catch (error) {
        clearTimeout(timeout);
        this.pendingRequests.delete(id);
        reject(error instanceof Error ? error : new Error(String(error)));
      }
    });
  }

  private async sendHello(): Promise<void> {
    const resp = await this.sendRequest(SigrokFrameType.HELLO_REQ);
    if (resp.type !== SigrokFrameType.HELLO_RESP) {
      throw new Error("Unexpected response to HELLO");
    }
    this.hello = resp.payload;
  }

  async getCaps(): Promise<SigrokCapsResp> {
    const resp = await this.sendRequest(SigrokFrameType.CAPS_REQ);
    if (resp.type !== SigrokFrameType.CAPS_RESP) {
      throw new Error("Unexpected response to CAPS");
    }
    const caps = freezeSigrokCaps(resp.payload);
    this.caps = caps;
    return caps;
  }

  async configure(config: SigrokConfigReq, options: SigrokConfigureOptions = {}): Promise<SigrokAck> {
    const request = buildSigrokConfigFrameRequest(config, this.getServerCapabilities(), options);
    const resp = await this.sendRequest(request.type, request.payload);
    if (resp.type !== SigrokFrameType.CONFIG_RESP) {
      throw new Error("Unexpected response to CONFIG");
    }
    this.setState("configured");
    return resp.payload;
  }

  async start(): Promise<SigrokAck> {
    const resp = await this.sendRequest(SigrokFrameType.START_REQ);
    if (resp.type !== SigrokFrameType.START_RESP) {
      throw new Error("Unexpected response to START");
    }
    return resp.payload;
  }

  async stop(): Promise<SigrokAck> {
    const resp = await this.sendRequest(SigrokFrameType.STOP_REQ);
    if (resp.type !== SigrokFrameType.STOP_RESP) {
      throw new Error("Unexpected response to STOP");
    }
    this.setState("configured");
    await new Promise(resolve => setTimeout(resolve, 100));
    return resp.payload;
  }
}
