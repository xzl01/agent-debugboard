import WebSocket from "ws";

import {
  createSerialBrokerRequestId,
  normalizeSerialBrokerBaud,
  normalizeSerialBrokerChannel,
  parseSerialBrokerServerFrame,
  serialBrokerClaimRequest,
  serialBrokerCloseRequest,
  serialBrokerFrame,
  serialBrokerOpenRequest,
  serialBrokerReleaseRequest,
  serialBrokerWriteRequest,
} from "./serial-broker-protocol.mjs";

const DEFAULT_REQUEST_TIMEOUT_MS = 10_000;
const DEFAULT_HISTORY_CHARS = 1_048_576;

export class SerialBrokerClientError extends Error {
  constructor(code, message, details = {}) {
    super(message);
    this.name = "SerialBrokerClientError";
    this.code = code;
    this.details = details;
  }
}

function lineEndingText(value) {
  switch (value) {
    case "cr": return "\r";
    case "lf": return "\n";
    case "crlf": return "\r\n";
    case "none": return "";
    default: throw new SerialBrokerClientError("invalid_line_ending", `unsupported line ending: ${value}`);
  }
}

function asError(error, fallbackCode = "serial_broker_error") {
  if (error instanceof SerialBrokerClientError) return error;
  return new SerialBrokerClientError(
    fallbackCode,
    error instanceof Error ? error.message : String(error),
  );
}

function escapeRegExp(value) {
  return String(value).replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

function redactInternalShellText(text, token) {
  const variable = `__linkr_rc_${token}`;
  const marker = `__LINKR_RC_${token}__`;
  return String(text).split(/(?<=\n)/).map((line) => {
    if (line.includes(variable)) return "";
    const markerStart = line.indexOf(marker);
    if (markerStart < 0) return line;
    const prefix = line.slice(0, markerStart);
    if (!prefix) return "";
    const ending = line.endsWith("\r\n") ? "\r\n" : line.endsWith("\n") ? "\n" : "";
    return `${prefix}${ending}`;
  }).join("");
}

function matchesPattern(text, pattern, regex) {
  try {
    return regex ? new RegExp(pattern, "m").test(text) : text.includes(pattern);
  } catch (error) {
    throw new SerialBrokerClientError("invalid_regex", error.message);
  }
}

function matchesTailPrompt(text, pattern, regex) {
  try {
    return regex ? new RegExp(pattern).test(text) : text.endsWith(pattern);
  } catch (error) {
    throw new SerialBrokerClientError("invalid_regex", error.message);
  }
}

function redactSecret(value, secret) {
  if (!secret) return value;
  if (typeof value === "string") return value.split(secret).join("[redacted]");
  if (Array.isArray(value)) return value.map((item) => redactSecret(item, secret));
  if (value && typeof value === "object") {
    return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, redactSecret(item, secret)]));
  }
  return value;
}

export class SerialBrokerClient {
  constructor({
    url = "ws://127.0.0.1:8787/serial",
    WebSocketImpl = WebSocket,
    requestTimeoutMs = DEFAULT_REQUEST_TIMEOUT_MS,
    maxHistoryChars = DEFAULT_HISTORY_CHARS,
  } = {}) {
    this.url = url;
    this.WebSocketImpl = WebSocketImpl;
    this.requestTimeoutMs = requestTimeoutMs;
    this.maxHistoryChars = maxHistoryChars;
    this.ws = null;
    this.clientId = null;
    this.connecting = null;
    this.pending = new Map();
    this.waiters = new Map();
    this.channels = new Map(["uart0", "uart1"].map((channel) => [channel, {
      channel,
      open: false,
      claimed: false,
      status: null,
      earliestCursor: 0,
      latestCursor: 0,
      chunks: [],
    }]));
  }

  channelState(channel) {
    return this.channels.get(normalizeSerialBrokerChannel(channel));
  }

  async connect() {
    if (this.ws?.readyState === this.WebSocketImpl.OPEN && this.clientId) return this.clientId;
    if (this.connecting) return this.connecting;

    this.connecting = new Promise((resolve, reject) => {
      const ws = new this.WebSocketImpl(this.url);
      this.ws = ws;
      let settled = false;
      const timer = setTimeout(() => {
        if (settled) return;
        settled = true;
        ws.terminate?.();
        reject(new SerialBrokerClientError("broker_connect_timeout", "serial broker hello timed out"));
      }, this.requestTimeoutMs);
      timer.unref?.();

      const fail = (error) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        reject(asError(error, "broker_connect_failed"));
      };

      ws.on("message", (data) => {
        const frame = parseSerialBrokerServerFrame(data.toString());
        if (!frame) return;
        this.handleFrame(frame);
        if (!settled && frame.type === "hello") {
          settled = true;
          clearTimeout(timer);
          this.clientId = String(frame.client_id || "unknown");
          resolve(this.clientId);
        }
      });
      ws.once("error", fail);
      ws.once("close", () => {
        if (!settled) fail(new SerialBrokerClientError("broker_closed", "serial broker closed before hello"));
        this.handleSocketClose();
      });
    }).finally(() => {
      this.connecting = null;
    });
    return this.connecting;
  }

  handleSocketClose() {
    this.ws = null;
    this.clientId = null;
    for (const state of this.channels.values()) {
      state.open = false;
      state.claimed = false;
      state.status = null;
      this.notifyDataWaiters(state.channel);
    }
    const error = new SerialBrokerClientError("broker_closed", "serial broker connection closed");
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pending.clear();
  }

  handleFrame(frame) {
    const channel = frame.channel === "uart1" ? "uart1" : frame.channel === "uart0" ? "uart0" : null;
    if (channel) {
      const state = this.channelState(channel);
      if (frame.type === "data") this.appendData(state, frame);
      if (frame.type === "status") state.status = frame;
      if (frame.type === "opened") state.open = true;
      if (frame.type === "closed") {
        state.open = false;
        state.claimed = false;
      }
      if (frame.type === "claimed") state.claimed = true;
      if (frame.type === "released") state.claimed = false;
    }

    const requestId = typeof frame.request_id === "string" ? frame.request_id : null;
    if (!requestId) return;
    const pending = this.pending.get(requestId);
    if (!pending) return;
    if (frame.type === "error") {
      this.pending.delete(requestId);
      clearTimeout(pending.timer);
      pending.reject(new SerialBrokerClientError(frame.code || "broker_error", frame.message || "serial broker request failed", frame));
      return;
    }
    if (!pending.expected.has(frame.type)) return;
    this.pending.delete(requestId);
    clearTimeout(pending.timer);
    pending.resolve(frame);
  }

  appendData(state, frame) {
    const text = typeof frame.text === "string" ? frame.text : "";
    if (!text) return;
    const start = state.latestCursor;
    const end = start + text.length;
    state.chunks.push({
      start,
      end,
      text,
      sequence: frame.sequence,
      host_t_mono_us: frame.host_t_mono_us,
      byte_count: frame.byte_count,
    });
    state.latestCursor = end;

    const retainFrom = Math.max(0, state.latestCursor - this.maxHistoryChars);
    while (state.chunks.length > 0 && state.chunks[0].end <= retainFrom) state.chunks.shift();
    if (state.chunks.length > 0 && state.chunks[0].start < retainFrom) {
      const first = state.chunks[0];
      const offset = retainFrom - first.start;
      state.chunks[0] = { ...first, start: retainFrom, text: first.text.slice(offset) };
    }
    state.earliestCursor = state.chunks[0]?.start ?? state.latestCursor;
    this.notifyDataWaiters(state.channel);
  }

  notifyDataWaiters(channel) {
    const waiters = this.waiters.get(channel);
    if (!waiters) return;
    this.waiters.delete(channel);
    for (const resolve of waiters) resolve();
  }

  waitForData(channel, timeoutMs) {
    if (timeoutMs <= 0) return Promise.resolve();
    return new Promise((resolve) => {
      let waiters = this.waiters.get(channel);
      if (!waiters) {
        waiters = new Set();
        this.waiters.set(channel, waiters);
      }
      const done = () => {
        clearTimeout(timer);
        waiters.delete(done);
        if (waiters.size === 0) this.waiters.delete(channel);
        resolve();
      };
      waiters.add(done);
      const timer = setTimeout(done, timeoutMs);
      timer.unref?.();
    });
  }

  async request(frame, expected, timeoutMs = this.requestTimeoutMs) {
    await this.connect();
    if (!this.ws || this.ws.readyState !== this.WebSocketImpl.OPEN) {
      throw new SerialBrokerClientError("broker_closed", "serial broker is not connected");
    }
    const requestId = frame.request_id || createSerialBrokerRequestId("mcp");
    frame.request_id = requestId;
    const response = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(requestId);
        reject(new SerialBrokerClientError("broker_request_timeout", `${frame.type} request timed out`, { request_id: requestId }));
      }, timeoutMs);
      timer.unref?.();
      this.pending.set(requestId, { expected: new Set(expected), resolve, reject, timer });
    });
    try {
      this.ws.send(JSON.stringify(frame));
    } catch (error) {
      const pending = this.pending.get(requestId);
      if (pending) {
        this.pending.delete(requestId);
        clearTimeout(pending.timer);
        pending.reject(asError(error, "broker_send_failed"));
      }
    }
    return response;
  }

  async open(channel, baud = 115200) {
    channel = normalizeSerialBrokerChannel(channel);
    baud = normalizeSerialBrokerBaud(baud);
    const frame = await this.request(serialBrokerOpenRequest(channel, baud, createSerialBrokerRequestId("mcp")), ["opened"]);
    return { ...frame, cursor: this.channelState(channel).latestCursor };
  }

  async status(channel) {
    channel = normalizeSerialBrokerChannel(channel);
    return this.request(serialBrokerFrame("status", {
      request_id: createSerialBrokerRequestId("mcp"),
      channel,
    }), ["status"]);
  }

  async claim(channel, owner = "mcp") {
    channel = normalizeSerialBrokerChannel(channel);
    return this.request(serialBrokerClaimRequest(channel, owner, createSerialBrokerRequestId("mcp")), ["claimed"]);
  }

  async release(channel) {
    channel = normalizeSerialBrokerChannel(channel);
    return this.request(serialBrokerReleaseRequest(channel, createSerialBrokerRequestId("mcp")), ["released"]);
  }

  async write(channel, text, { lineEnding = "none", observerRedactionToken } = {}) {
    channel = normalizeSerialBrokerChannel(channel);
    const payload = String(text) + lineEndingText(lineEnding);
    const frame = await this.request(
      serialBrokerWriteRequest(channel, payload, createSerialBrokerRequestId("mcp"), { observerRedactionToken }),
      ["write_ack"],
    );
    return { ...frame, cursor: this.channelState(channel).latestCursor };
  }

  async closeChannel(channel) {
    channel = normalizeSerialBrokerChannel(channel);
    if (!this.channelState(channel).open) return { channel, closed: true };
    return this.request(serialBrokerCloseRequest(channel, createSerialBrokerRequestId("mcp")), ["closed"]);
  }

  currentCursor(channel) {
    return this.channelState(channel).latestCursor;
  }

  tail(channel, maxChars = 4_096) {
    const state = this.channelState(channel);
    const cursor = Math.max(state.earliestCursor, state.latestCursor - maxChars);
    return this.collect(channel, cursor, maxChars);
  }

  collect(channel, cursor, maxChars) {
    const state = this.channelState(channel);
    if (cursor < state.earliestCursor) {
      throw new SerialBrokerClientError(
        "serial_cursor_expired",
        `cursor ${cursor} is older than retained cursor ${state.earliestCursor}`,
        { earliest_cursor: state.earliestCursor, latest_cursor: state.latestCursor },
      );
    }
    if (cursor > state.latestCursor) {
      throw new SerialBrokerClientError(
        "serial_cursor_ahead",
        `cursor ${cursor} is newer than current cursor ${state.latestCursor}; reconnect or restart from the reported cursor`,
        { earliest_cursor: state.earliestCursor, latest_cursor: state.latestCursor },
      );
    }
    const requestedCursor = cursor;
    let text = "";
    for (const chunk of state.chunks) {
      if (chunk.end <= requestedCursor) continue;
      const offset = Math.max(0, requestedCursor - chunk.start);
      text += chunk.text.slice(offset);
      if (text.length >= maxChars) break;
    }
    text = text.slice(0, maxChars);
    const truncated = requestedCursor + text.length < state.latestCursor;
    return {
      channel,
      cursor: requestedCursor,
      next_cursor: requestedCursor + text.length,
      earliest_cursor: state.earliestCursor,
      latest_cursor: state.latestCursor,
      truncated,
      text,
    };
  }

  async read(channel, { cursor, maxChars = 32_768, waitMs = 0 } = {}) {
    channel = normalizeSerialBrokerChannel(channel);
    const state = this.channelState(channel);
    const start = cursor == null ? state.earliestCursor : Number(cursor);
    let result = this.collect(channel, start, maxChars);
    if (!result.text && waitMs > 0 && state.open) {
      await this.waitForData(channel, waitMs);
      result = this.collect(channel, start, maxChars);
    }
    return result;
  }

  async expect(channel, {
    pattern,
    regex = false,
    caseSensitive = true,
    cursor,
    timeoutMs = 30_000,
    maxChars = 131_072,
  }) {
    channel = normalizeSerialBrokerChannel(channel);
    const state = this.channelState(channel);
    const start = cursor == null ? state.latestCursor : Number(cursor);
    let searchCursor = start;
    let matcher;
    try {
      matcher = regex
        ? new RegExp(pattern, caseSensitive ? "m" : "im")
        : null;
    } catch (error) {
      throw new SerialBrokerClientError("invalid_regex", error.message);
    }
    const needle = caseSensitive ? String(pattern) : String(pattern).toLowerCase();
    const deadline = Date.now() + timeoutMs;

    while (true) {
      const result = this.collect(channel, searchCursor, maxChars);
      const haystack = caseSensitive ? result.text : result.text.toLowerCase();
      const match = matcher ? matcher.exec(result.text) : null;
      const index = matcher ? match?.index ?? -1 : haystack.indexOf(needle);
      if (index >= 0) {
        const matched = matcher ? match[0] : result.text.slice(index, index + String(pattern).length);
        return {
          ...result,
          matched,
          match_index: index,
          started_cursor: start,
          match_cursor: searchCursor + index,
          next_cursor: searchCursor + index + matched.length,
        };
      }
      if (result.truncated) {
        const overlap = Math.min(Math.max(String(pattern).length, 256), 4_096);
        searchCursor = Math.max(searchCursor + 1, result.next_cursor - overlap);
        continue;
      }
      const remaining = deadline - Date.now();
      if (remaining <= 0) {
        throw new SerialBrokerClientError("serial_expect_timeout", `timed out waiting for ${JSON.stringify(pattern)}`, {
          channel,
          cursor: start,
          captured: result.text,
        });
      }
      await this.waitForData(channel, Math.min(remaining, 250));
    }
  }

  async command(channel, {
    command,
    prompt,
    promptRegex = false,
    timeoutMs = 30_000,
    lineEnding = "cr",
    owner = "mcp-command",
  }) {
    channel = normalizeSerialBrokerChannel(channel);
    const cursor = this.currentCursor(channel);
    await this.claim(channel, owner);
    let operationError = null;
    try {
      const write = await this.write(channel, command, { lineEnding });
      const output = await this.expect(channel, {
        pattern: prompt,
        regex: promptRegex,
        cursor,
        timeoutMs,
      });
      return { channel, write, output };
    } catch (error) {
      operationError = error;
      throw error;
    } finally {
      try {
        await this.release(channel);
      } catch (releaseError) {
        // Preserve the original operation failure, but never report a command
        // as successful if its Broker ownership could not be released.
        if (!operationError) throw releaseError;
      }
    }
  }

  async shellCommand(channel, {
    command,
    prompt,
    promptRegex = true,
    requireZero = true,
    timeoutMs = 30_000,
    lineEnding = "cr",
    owner = "mcp-shell-command",
  }) {
    channel = normalizeSerialBrokerChannel(channel);
    const token = createSerialBrokerRequestId("shell").replace(/[^A-Za-z0-9]/g, "");
    const marker = `__LINKR_RC_${token}__`;
    const variable = `__linkr_rc_${token}`;
    const wrapped = `${command}\n${variable}=$?\nprintf '${marker}%d\\n' "$${variable}"`;
    const cursor = this.currentCursor(channel);
    await this.claim(channel, owner);
    let operationError = null;
    try {
      await this.write(channel, wrapped, { lineEnding, observerRedactionToken: token });
      const exit = await this.expect(channel, {
        pattern: `${escapeRegExp(marker)}([0-9]+)`,
        regex: true,
        cursor,
        timeoutMs,
      });
      const match = new RegExp(`${escapeRegExp(marker)}([0-9]+)`).exec(exit.matched);
      const exitCode = Number(match?.[1]);
      if (!Number.isInteger(exitCode)) {
        throw new SerialBrokerClientError("serial_exit_code_missing", "target shell did not return a valid exit code");
      }
      const shell = await this.expect(channel, {
        pattern: prompt,
        regex: promptRegex,
        cursor: exit.next_cursor,
        timeoutMs,
      });
      const result = {
        channel,
        exit_code: exitCode,
        output: redactInternalShellText(exit.text.slice(0, exit.match_index), token),
        next_cursor: shell.next_cursor,
      };
      if (requireZero && exitCode !== 0) {
        throw new SerialBrokerClientError(
          "serial_command_failed",
          `target command exited with status ${exitCode}`,
          result,
        );
      }
      return result;
    } catch (error) {
      operationError = error;
      throw error;
    } finally {
      try {
        await this.release(channel);
      } catch (releaseError) {
        if (!operationError) throw releaseError;
      }
    }
  }

  async login(channel, {
    username,
    password,
    loginPrompt = "login:",
    passwordPrompt = "Password:",
    shellPrompt = "[#$>]\\s*$",
    shellPromptRegex = true,
    timeoutMs = 30_000,
    lineEnding = "cr",
    owner = "mcp-login",
  }) {
    channel = normalizeSerialBrokerChannel(channel);
    const deadline = Date.now() + timeoutMs;
    const remaining = () => Math.max(1, deadline - Date.now());
    const combinedPrompt = `${escapeRegExp(loginPrompt)}|${shellPromptRegex ? shellPrompt : escapeRegExp(shellPrompt)}`;
    await this.claim(channel, owner);
    let passwordSent = false;
    let operationError = null;
    try {
      const snapshot = this.tail(channel);
      if (matchesTailPrompt(snapshot.text, shellPrompt, shellPromptRegex)) {
        return { channel, authenticated: true, already_authenticated: true, next_cursor: snapshot.next_cursor };
      }

      if (!snapshot.text.includes(loginPrompt)) {
        const cursor = this.currentCursor(channel);
        await this.write(channel, "", { lineEnding });
        const prompt = await this.expect(channel, {
          pattern: combinedPrompt,
          regex: true,
          cursor,
          timeoutMs: remaining(),
        });
        if (matchesPattern(prompt.matched, shellPrompt, shellPromptRegex)) {
          return { channel, authenticated: true, already_authenticated: true, next_cursor: prompt.next_cursor };
        }
      }

      let cursor = this.currentCursor(channel);
      await this.write(channel, username, { lineEnding });
      const passwordOrShell = await this.expect(channel, {
        pattern: `${escapeRegExp(passwordPrompt)}|${shellPromptRegex ? shellPrompt : escapeRegExp(shellPrompt)}`,
        regex: true,
        cursor,
        timeoutMs: remaining(),
      });
      if (matchesPattern(passwordOrShell.matched, shellPrompt, shellPromptRegex)) {
        return { channel, authenticated: true, password_required: false, next_cursor: passwordOrShell.next_cursor };
      }

      cursor = this.currentCursor(channel);
      await this.write(channel, password, { lineEnding });
      passwordSent = true;
      const shell = await this.expect(channel, {
        pattern: shellPrompt,
        regex: shellPromptRegex,
        cursor,
        timeoutMs: remaining(),
      });
      return { channel, authenticated: true, password_required: true, next_cursor: shell.next_cursor };
    } catch (error) {
      operationError = error;
      if (!passwordSent || !(error instanceof SerialBrokerClientError)) throw error;
      const redacted = new SerialBrokerClientError(
        error.code,
        redactSecret(error.message, password),
        redactSecret(error.details, password),
      );
      operationError = redacted;
      throw redacted;
    } finally {
      try {
        await this.release(channel);
      } catch (releaseError) {
        if (!operationError) throw releaseError;
      }
    }
  }

  async close() {
    for (const channel of ["uart0", "uart1"]) {
      if (!this.channelState(channel).open) continue;
      try {
        if (this.channelState(channel).claimed) await this.release(channel);
        await this.closeChannel(channel);
      } catch {
        // Socket shutdown below releases server-side ownership as a final fallback.
      }
    }
    const ws = this.ws;
    this.ws = null;
    if (ws && (ws.readyState === this.WebSocketImpl.CONNECTING || ws.readyState === this.WebSocketImpl.OPEN)) {
      ws.close();
    }
  }
}
