import { createSerialUtf8Decoder } from "./serial-utf8.mjs";
import {
  SERIAL_BROKER_CHANNELS,
  serialBrokerFrame,
} from "./serial-broker-protocol.mjs";

const DEFAULT_IDLE_TIMEOUT_MS = 30_000;
const DEFAULT_VENDOR_ID = "1a86";
const MAX_REDACTION_BUFFER_BYTES = 65_536;
const REDACTION_SUFFIX_CHARS = 512;

function redactInternalShellLine(line, token) {
  const variable = `__linkr_rc_${token}`;
  if (line.includes(variable)) return "";
  const marker = `__LINKR_RC_${token}__`;
  const markerStart = line.indexOf(marker);
  if (markerStart < 0) return line;
  const prefix = line.slice(0, markerStart);
  if (!prefix) return "";
  const ending = line.endsWith("\r\n") ? "\r\n" : line.endsWith("\n") ? "\n" : "";
  return `${prefix}${ending}`;
}

function pushObserverRedaction(redaction, text) {
  redaction.pending += text;
  let visible = "";
  let newline;
  while ((newline = redaction.pending.indexOf("\n")) >= 0) {
    const line = redaction.pending.slice(0, newline + 1);
    redaction.pending = redaction.pending.slice(newline + 1);
    visible += redactInternalShellLine(line, redaction.token);
  }
  if (Buffer.byteLength(redaction.pending, "utf8") > MAX_REDACTION_BUFFER_BYTES) {
    const split = Math.max(0, redaction.pending.length - REDACTION_SUFFIX_CHARS);
    visible += redaction.pending.slice(0, split);
    redaction.pending = redaction.pending.slice(split);
  }
  return visible;
}

function serialPortIsOpen(port) {
  return Boolean(port?.isOpen);
}

function openPort(port) {
  return new Promise((resolve, reject) => {
    port.open((error) => error ? reject(error) : resolve());
  });
}

function closePort(port) {
  if (!serialPortIsOpen(port)) return Promise.resolve();
  return new Promise((resolve) => {
    port.close(() => resolve());
  });
}

function writePort(port, data) {
  return new Promise((resolve, reject) => {
    port.write(data, (writeError) => {
      if (writeError) {
        reject(writeError);
        return;
      }
      port.drain((drainError) => drainError ? reject(drainError) : resolve());
    });
  });
}

function decodeWriteData(data) {
  if (data.encoding === "utf8") return Buffer.from(data.value, "utf8");
  const compact = data.value.replaceAll(/\s/g, "");
  if (compact.length % 4 !== 0 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(compact)) {
    throw new Error("write data is not valid base64");
  }
  return Buffer.from(compact, "base64");
}

export function selectSerialBrokerPort(ports, channel, vendorId = DEFAULT_VENDOR_ID) {
  const candidates = ports
    .filter((port) => (port.vendorId || "").toLowerCase() === vendorId.toLowerCase())
    .sort((left, right) => left.path.localeCompare(right.path));
  const suffix = channel === "uart1" ? /D3$/i : /D1$/i;
  return candidates.find((port) => suffix.test(port.path)) ??
    candidates[channel === "uart1" ? 1 : 0];
}

export class SerialBroker {
  constructor({
    listPorts,
    createPort,
    send,
    vendorId = DEFAULT_VENDOR_ID,
    idleTimeoutMs = DEFAULT_IDLE_TIMEOUT_MS,
    nowMonoUs = () => Number(process.hrtime.bigint() / 1_000n),
    setTimer = setTimeout,
    clearTimer = clearTimeout,
  }) {
    this.listPorts = listPorts;
    this.createPort = createPort;
    this.send = send;
    this.vendorId = vendorId;
    this.idleTimeoutMs = idleTimeoutMs;
    this.nowMonoUs = nowMonoUs;
    this.setTimer = setTimer;
    this.clearTimer = clearTimer;
    this.peerSubscriptions = new Map();
    this.states = new Map(SERIAL_BROKER_CHANNELS.map((channel) => [channel, {
      channel,
      serial: null,
      decoder: null,
      pendingByteCount: 0,
      path: null,
      baud: null,
      opening: null,
      openingBaud: null,
      subscribers: new Set(),
      owner: null,
      observerRedaction: null,
      sequence: 0,
      writeQueue: Promise.resolve(),
      idleTimer: null,
    }]));
  }

  clientId(peer) {
    return String(peer.__brokerClientId || peer.id || "unknown");
  }

  state(channel) {
    const state = this.states.get(channel);
    if (!state) throw new Error(`unknown serial broker channel: ${channel}`);
    return state;
  }

  sendFrame(peer, type, fields = {}) {
    this.send(peer, serialBrokerFrame(type, fields));
  }

  sendError(peer, code, message, requestId, channel, retryable = false) {
    this.sendFrame(peer, "error", {
      request_id: requestId,
      channel,
      code,
      message,
      retryable,
    });
  }

  statusFrame(state, requestId) {
    return serialBrokerFrame("status", {
      request_id: requestId,
      channel: state.channel,
      connected: serialPortIsOpen(state.serial),
      opening: Boolean(state.opening),
      path: state.path,
      baud: state.baud ?? state.openingBaud,
      subscribers: state.subscribers.size,
      owner: state.owner ? {
        client_id: this.clientId(state.owner.peer),
        label: state.owner.label,
      } : null,
    });
  }

  sendStatus(peer, channel, requestId) {
    this.send(peer, this.statusFrame(this.state(channel), requestId));
  }

  publishStatus(state) {
    const frame = this.statusFrame(state);
    for (const peer of state.subscribers) this.send(peer, frame);
  }

  addSubscription(peer, state) {
    state.subscribers.add(peer);
    let channels = this.peerSubscriptions.get(peer);
    if (!channels) {
      channels = new Set();
      this.peerSubscriptions.set(peer, channels);
    }
    channels.add(state.channel);
  }

  removeSubscription(peer, state) {
    if (state.owner?.peer === peer) this.finishObserverRedaction(state);
    state.subscribers.delete(peer);
    const channels = this.peerSubscriptions.get(peer);
    channels?.delete(state.channel);
    if (channels?.size === 0) this.peerSubscriptions.delete(peer);
    if (state.owner?.peer === peer) state.owner = null;
  }

  cancelIdleClose(state) {
    if (state.idleTimer == null) return;
    this.clearTimer(state.idleTimer);
    state.idleTimer = null;
  }

  scheduleIdleClose(state) {
    this.cancelIdleClose(state);
    if (state.subscribers.size > 0 || !state.serial) return;
    state.idleTimer = this.setTimer(() => {
      state.idleTimer = null;
      if (state.subscribers.size === 0) void this.closeSerial(state);
    }, this.idleTimeoutMs);
    state.idleTimer?.unref?.();
  }

  dataFrame(state, text, byteCount) {
    return serialBrokerFrame("data", {
      channel: state.channel,
      sequence: state.sequence,
      host_t_mono_us: this.nowMonoUs(),
      text,
      byte_count: byteCount,
    });
  }

  broadcastData(state, text, byteCount) {
    if (!text) return;
    state.sequence += 1;
    const redaction = state.observerRedaction;
    if (!redaction) {
      const frame = this.dataFrame(state, text, byteCount);
      for (const peer of state.subscribers) this.send(peer, frame);
      return;
    }

    if (state.subscribers.has(redaction.owner)) {
      this.send(redaction.owner, this.dataFrame(state, text, byteCount));
    }
    const visible = pushObserverRedaction(redaction, text);
    if (!visible) return;
    const frame = this.dataFrame(state, visible, Buffer.byteLength(visible, "utf8"));
    for (const peer of state.subscribers) {
      if (peer !== redaction.owner) this.send(peer, frame);
    }
  }

  finishObserverRedaction(state) {
    const redaction = state.observerRedaction;
    if (!redaction) return;
    state.observerRedaction = null;
    const visible = redactInternalShellLine(redaction.pending, redaction.token);
    if (!visible) return;
    state.sequence += 1;
    const frame = this.dataFrame(state, visible, Buffer.byteLength(visible, "utf8"));
    for (const peer of state.subscribers) {
      if (peer !== redaction.owner) this.send(peer, frame);
    }
  }

  flushDecoder(state) {
    if (!state.decoder) return;
    const text = state.decoder.flush();
    if (text) this.broadcastData(state, text, state.pendingByteCount);
    state.decoder = null;
    state.pendingByteCount = 0;
  }

  handleSerialClose(state, serial) {
    if (state.serial !== serial) return;
    this.flushDecoder(state);
    this.finishObserverRedaction(state);
    state.serial = null;
    state.path = null;
    state.baud = null;
    state.owner = null;
    const frame = serialBrokerFrame("closed", {
      channel: state.channel,
      reason: "serial port closed",
    });
    for (const peer of state.subscribers) this.send(peer, frame);
    this.publishStatus(state);
  }

  attachSerial(state, serial) {
    const decoder = createSerialUtf8Decoder();
    state.decoder = decoder;
    state.pendingByteCount = 0;
    serial.on("data", (chunk) => {
      if (state.serial !== serial) return;
      state.pendingByteCount += chunk.byteLength;
      const text = decoder.decode(chunk);
      if (!text) return;
      const byteCount = Math.min(state.pendingByteCount, Buffer.byteLength(text, "utf8"));
      state.pendingByteCount -= byteCount;
      this.broadcastData(state, text, byteCount);
    });
    serial.on("error", (error) => {
      if (state.serial !== serial) return;
      for (const peer of state.subscribers) {
        this.sendError(peer, "serial_io_error", error.message, undefined, state.channel, true);
      }
    });
    serial.on("close", () => this.handleSerialClose(state, serial));
  }

  async openSerial(state, baud) {
    if (serialPortIsOpen(state.serial) && state.baud === baud) return;
    if (state.opening) {
      if (state.openingBaud !== baud) throw new Error(`channel is opening at ${state.openingBaud} baud`);
      await state.opening;
      return;
    }
    if (state.serial) await this.closeSerial(state);

    state.openingBaud = baud;
    state.opening = (async () => {
      const target = selectSerialBrokerPort(await this.listPorts(), state.channel, this.vendorId);
      if (!target) throw new Error(`No CH347F ${state.channel.toUpperCase()} serial port found`);
      const serial = this.createPort({ path: target.path, baudRate: baud, autoOpen: false });
      state.serial = serial;
      state.path = target.path;
      state.baud = baud;
      this.attachSerial(state, serial);
      try {
        await openPort(serial);
      } catch (error) {
        if (state.serial === serial) {
          this.flushDecoder(state);
          state.serial = null;
          state.path = null;
          state.baud = null;
        }
        throw error;
      }
    })();
    try {
      await state.opening;
    } finally {
      state.opening = null;
      state.openingBaud = null;
    }
  }

  async closeSerial(state) {
    this.cancelIdleClose(state);
    const serial = state.serial;
    if (!serial) return;
    this.flushDecoder(state);
    await closePort(serial);
    if (state.serial === serial) {
      state.serial = null;
      state.path = null;
      state.baud = null;
      state.owner = null;
    }
  }

  async subscribe(peer, { channel, baud, request_id: requestId }) {
    const state = this.state(channel);
    this.cancelIdleClose(state);
    if (state.subscribers.size > 0 && state.baud != null && state.baud !== baud) {
      this.sendError(peer, "baud_conflict", `${channel} is already open at ${state.baud} baud`, requestId, channel);
      return false;
    }
    if (state.opening && state.openingBaud !== baud) {
      this.sendError(peer, "baud_conflict", `${channel} is opening at ${state.openingBaud} baud`, requestId, channel);
      return false;
    }

    this.addSubscription(peer, state);
    try {
      await this.openSerial(state, baud);
    } catch (error) {
      this.removeSubscription(peer, state);
      this.sendError(
        peer,
        /No CH347F/.test(String(error)) ? "serial_not_found" : "serial_open_failed",
        error instanceof Error ? error.message : String(error),
        requestId,
        channel,
        true,
      );
      this.publishStatus(state);
      return false;
    }

    if (!state.subscribers.has(peer)) {
      this.scheduleIdleClose(state);
      return false;
    }
    this.sendFrame(peer, "opened", {
      request_id: requestId,
      channel,
      path: state.path,
      baud: state.baud,
      shared: true,
    });
    this.publishStatus(state);
    return true;
  }

  async write(peer, { channel, data, observer_redaction: observerRedaction, request_id: requestId }) {
    const state = this.state(channel);
    if (!state.subscribers.has(peer) || !serialPortIsOpen(state.serial)) {
      this.sendError(peer, "serial_not_open", `${channel} is not open for this client`, requestId, channel, true);
      return false;
    }
    if (state.owner && state.owner.peer !== peer) {
      this.sendError(
        peer,
        "serial_busy",
        `${channel} write access is owned by ${state.owner.label}`,
        requestId,
        channel,
        true,
      );
      return false;
    }
    if (observerRedaction) {
      if (state.owner?.peer !== peer) {
        this.sendError(
          peer,
          "serial_claim_required",
          "observer redaction requires exclusive write ownership",
          requestId,
          channel,
        );
        return false;
      }
      if (state.observerRedaction) {
        this.sendError(
          peer,
          "serial_redaction_active",
          "an observer redaction operation is already active",
          requestId,
          channel,
          true,
        );
        return false;
      }
    }

    let bytes;
    try {
      bytes = decodeWriteData(data);
    } catch (error) {
      this.sendError(peer, "invalid_write", error.message, requestId, channel);
      return false;
    }
    const serial = state.serial;
    const operation = state.writeQueue.then(async () => {
      if (state.serial !== serial || !serialPortIsOpen(serial)) {
        throw new Error(`${channel} closed before the write completed`);
      }
      if (observerRedaction) {
        state.observerRedaction = {
          owner: peer,
          token: observerRedaction.line_token,
          pending: "",
        };
      }
      await writePort(serial, bytes);
      this.sendFrame(peer, "write_ack", {
        request_id: requestId,
        channel,
        bytes: bytes.byteLength,
      });
    });
    state.writeQueue = operation.catch(() => {});
    try {
      await operation;
      return true;
    } catch (error) {
      this.sendError(peer, "serial_write_failed", error.message, requestId, channel, true);
      return false;
    }
  }

  claim(peer, { channel, owner, request_id: requestId }) {
    const state = this.state(channel);
    if (!state.subscribers.has(peer) || !serialPortIsOpen(state.serial)) {
      this.sendError(peer, "serial_not_open", `${channel} is not open for this client`, requestId, channel, true);
      return false;
    }
    if (state.owner && state.owner.peer !== peer) {
      this.sendError(peer, "serial_busy", `${channel} is already owned by ${state.owner.label}`, requestId, channel, true);
      return false;
    }
    state.owner = { peer, label: owner };
    this.sendFrame(peer, "claimed", { request_id: requestId, channel, owner });
    this.publishStatus(state);
    return true;
  }

  release(peer, { channel, request_id: requestId }) {
    const state = this.state(channel);
    if (state.owner?.peer === peer) {
      this.finishObserverRedaction(state);
      state.owner = null;
    }
    this.sendFrame(peer, "released", { request_id: requestId, channel });
    this.publishStatus(state);
  }

  async unsubscribe(peer, channel, { requestId, immediate = false } = {}) {
    const state = this.state(channel);
    this.removeSubscription(peer, state);
    this.sendFrame(peer, "closed", {
      request_id: requestId,
      channel,
      reason: "client unsubscribed",
    });
    this.publishStatus(state);
    if (state.subscribers.size > 0) return;
    if (immediate) await this.closeSerial(state);
    else this.scheduleIdleClose(state);
  }

  async disconnectPeer(peer) {
    const channels = [...(this.peerSubscriptions.get(peer) || [])];
    for (const channel of channels) {
      const state = this.state(channel);
      this.removeSubscription(peer, state);
      this.publishStatus(state);
      this.scheduleIdleClose(state);
    }
  }

  async shutdown() {
    for (const state of this.states.values()) {
      this.cancelIdleClose(state);
      this.finishObserverRedaction(state);
      state.subscribers.clear();
      state.owner = null;
      await this.closeSerial(state);
    }
    this.peerSubscriptions.clear();
  }
}
