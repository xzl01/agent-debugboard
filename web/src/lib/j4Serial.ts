import * as api from "./api";
import {
  J4_SERIAL_BUFFER_LIMIT, J4_SERIAL_FRAME_BYTES, J4_SERIAL_QUEUE_LIMIT,
  J4_SERIAL_TOPIC, J4SerialError, parseJ4SerialState, record,
  type J4SerialConfig, type J4SerialState,
} from "./j4SerialProtocol";

export type J4ConnectionPhase = "disconnected" | "connecting" | "connected" | "unavailable";
export interface J4SerialSnapshot {
  phase: J4ConnectionPhase;
  serial: J4SerialState | null;
  error: { code: string; message: string } | null;
  backpressure: boolean;
  pendingWrites: number;
  droppedBytes: number;
  rxBytes: number;
  txBytes: number;
  configuring: boolean;
}

interface PendingWrite {
  eventId?: number;
  bytes: number;
  payload: Uint8Array;
  sent: boolean;
  resolve: () => void;
  reject: (reason: Error) => void;
  timer?: ReturnType<typeof setTimeout>;
}

export const emptyJ4Snapshot = (): J4SerialSnapshot => ({
  phase: "disconnected", serial: null, error: null, backpressure: false,
  pendingWrites: 0, droppedBytes: 0, rxBytes: 0, txBytes: 0, configuring: false,
});

/** One owner, one live session. Never uses board telemetry's WS or replays TX. */
export class J4SerialClient {
  private snapshot = emptyJ4Snapshot();
  private epoch = 0;
  private ws: WebSocket | null = null;
  private sessionId: number | null = null;
  private pending: PendingWrite[] = [];
  private abort: AbortController | null = null;
  private pressureTimer: ReturnType<typeof setInterval> | null = null;
  private configTimer: ReturnType<typeof setTimeout> | null = null;
  private connectTimer: ReturnType<typeof setTimeout> | null = null;
  private configRequestId: number | null = null;
  private reportedDrops: number | null = null;

  constructor(private readonly onChange: (snapshot: J4SerialSnapshot) => void,
    private readonly onBytes: (bytes: Uint8Array) => void) {}

  getSnapshot(): J4SerialSnapshot { return this.snapshot; }

  private update(patch: Partial<J4SerialSnapshot>) {
    this.snapshot = { ...this.snapshot, ...patch };
    this.onChange(this.snapshot);
  }

  private async request(init?: RequestInit): Promise<Record<string, unknown>> {
    const response = await fetch(`${api.apiEndpoint()}/serial/j4${init?.method === "PUT" ? "/config" : ""}`, {
      ...init, headers: { ...(init?.body ? { "Content-Type": "application/json" } : {}) },
    });
    let data: Record<string, unknown>;
    try { data = record(await response.json()); }
    catch { throw new J4SerialError(response.status === 404 ? "unavailable" : "invalid_response"); }
    if (!response.ok || data.ok === false) {
      const error = record(data.error);
      throw new J4SerialError(response.status === 404 || response.status === 501 ? "unavailable"
        : String(error.code ?? `http_${response.status}`), String(error.message ?? response.statusText));
    }
    return data;
  }

  async connect(): Promise<void> {
    const cleanup = this.disconnect();
    const epoch = this.epoch;
    await cleanup;
    if (epoch !== this.epoch) return;
    this.abort = new AbortController();
    this.reportedDrops = null;
    this.update({ phase: "connecting", serial: null, error: null });
    try {
      const serial = parseJ4SerialState(await this.request({ signal: this.abort.signal }));
      if (epoch !== this.epoch) return;
      this.update({ serial });
      const session = await api.createLiveSession();
      if (epoch !== this.epoch) {
        await api.deleteLiveSession(session.session_id).catch(() => {});
        return;
      }
      this.sessionId = session.session_id;
      const ws = new WebSocket(api.liveWebSocketUrl(session.ws_url));
      ws.binaryType = "arraybuffer";
      this.ws = ws;
      const current = () => this.epoch === epoch && this.ws === ws;
      this.connectTimer = setTimeout(() => {
        if (current() && this.snapshot.phase === "connecting") this.fail(new J4SerialError("connect_timeout"));
      }, 8_000);
      ws.onopen = () => {
        if (current()) ws.send(JSON.stringify({ type: "subscribe", topic: J4_SERIAL_TOPIC }));
      };
      ws.onmessage = (event) => {
        if (!current()) return;
        if (event.data instanceof ArrayBuffer) {
          const bytes = new Uint8Array(event.data);
          if (!bytes.length) return;
          this.update({ rxBytes: this.snapshot.rxBytes + bytes.length });
          this.onBytes(bytes);
        } else if (typeof event.data === "string") {
          this.receiveText(event.data);
        }
      };
      ws.onerror = () => { if (current()) this.fail(new J4SerialError("connection_error")); };
      ws.onclose = () => { if (current()) this.fail(new J4SerialError("connection_closed")); };
      // No automatic retries: missing APIs and a stopped board remain quiet.
    } catch (reason) {
      if (epoch === this.epoch) this.fail(reason);
    }
  }

  private receiveText(text: string) {
    let frame: Record<string, unknown>;
    try { frame = record(JSON.parse(text)); } catch { return; }
    if (frame.topic !== undefined && frame.topic !== J4_SERIAL_TOPIC) return;
    if (frame.type === "serial_state") {
      try { this.applyState(parseJ4SerialState(frame)); }
      catch (reason) { this.fail(reason); }
    } else if (frame.type === "serial_overrun") {
      if (!Number.isSafeInteger(frame.dropped_bytes) || !Number.isSafeInteger(frame.total_dropped_bytes)) return;
      const delta = Number(frame.dropped_bytes);
      const total = Number(frame.total_dropped_bytes);
      if (delta < 0 || total < delta || (this.reportedDrops !== null && total <= this.reportedDrops)) return;
      // The wire delta anchors a new subscription: a reused firmware WS slot
      // may already have a historical cumulative total. Subsequent monotonic
      // totals account for new losses without counting duplicates or old frames.
      // UART config generations do not reset the endpoint's cumulative counter.
      const added = this.reportedDrops === null ? delta : total - this.reportedDrops;
      this.reportedDrops = total;
      this.update({ droppedBytes: this.snapshot.droppedBytes + added });
    } else if (frame.type === "serial_write_accepted") {
      const pending = this.pending.find((item) => item.sent && item.eventId === undefined);
      if (pending && Number.isSafeInteger(frame.event_id) && Number(frame.event_id) > 0 && frame.bytes === pending.bytes) pending.eventId = Number(frame.event_id);
    } else if (frame.type === "serial_write_complete") {
      if (!Number.isSafeInteger(frame.event_id) || Number(frame.event_id) <= 0
        || (frame.disposition !== "completed" && frame.disposition !== "canceled")) return;
      const pending = this.pending.find((item) => item.eventId === frame.event_id);
      if (!pending) return;
      this.pending = this.pending.filter((item) => item !== pending);
      clearTimeout(pending.timer);
      if (frame.disposition === "completed") {
        this.update({ txBytes: this.snapshot.txBytes + pending.bytes });
        pending.resolve();
      } else {
        this.update({ error: { code: "tx_canceled", message: "J4 UART write was canceled" } });
        pending.reject(new J4SerialError("tx_canceled"));
        this.rejectWrites(new J4SerialError("tx_canceled"));
      }
      this.refreshPressure();
    } else if (frame.type === "error") {
      const error = record(frame.error);
      // Subscription failures cannot leave an apparently usable terminal.
      this.fail(new J4SerialError(String(frame.code ?? error.code ?? "serial_error"),
        String(frame.message ?? error.message ?? "J4 serial error")));
    }
  }

  private applyState(serial: J4SerialState) {
    const previous = this.snapshot.serial;
    if (previous && serial.generation < previous.generation) return;
    if (previous && serial.generation !== previous.generation) {
      // Acceptance frames carry event IDs, not generations. If an old binary
      // write has not yet been admitted, a fresh socket is the only unambiguous
      // boundary; never attach a late acceptance to new input.
      if (this.pending.some((item) => item.sent && item.eventId === undefined)) {
        this.fail(new J4SerialError("generation_changed"));
        return;
      }
      if (this.pending.length > 0) this.update({ error: { code: "generation_changed", message: "UART settings changed while input was pending" } });
      this.rejectWrites(new J4SerialError("generation_changed"));
    }
    if (serial.state !== "ready" || serial.read_only) {
      if (this.pending.some((item) => item.sent && item.eventId === undefined)) {
        this.fail(new J4SerialError("serial_not_ready", "J4 became read-only before write admission was confirmed"));
        return;
      }
      this.rejectWrites(new J4SerialError("read_only"));
    }
    if (this.connectTimer) clearTimeout(this.connectTimer);
    this.connectTimer = null;
    const done = this.configRequestId !== null && serial.last_config_done === true
      && serial.last_config_request_id === this.configRequestId;
    if (done) this.configRequestId = null;
    const configuring = serial.state === "reconfig" || this.configRequestId !== null;
    this.update({ serial, phase: "connected", configuring,
      ...(done && serial.last_config_result !== undefined && serial.last_config_result < 0
        ? { error: { code: serial.last_config_result === -16 || serial.last_config_result === -11 ? "config_conflict" : "config_failed",
          message: `J4 configuration failed (${serial.last_config_result})` } } : {}),
    });
    if (!configuring && this.configTimer) { clearTimeout(this.configTimer); this.configTimer = null; }
    this.refreshPressure();
  }

  async configure(config: J4SerialConfig): Promise<void> {
    const serial = this.snapshot.serial;
    if (!serial || this.snapshot.configuring || this.snapshot.phase !== "connected") throw new J4SerialError("disconnected");
    if (this.pending.length > 0) throw new J4SerialError("backpressure");
    const epoch = this.epoch;
    this.configRequestId = -1;
    this.update({ configuring: true, error: null });
    try {
      const result = await this.request({ method: "PUT", signal: this.abort?.signal,
        body: JSON.stringify({ ...config, expected_generation: serial.generation }) });
      if (epoch !== this.epoch) return;
      if (result.accepted !== true || !Number.isSafeInteger(result.request_id)) throw new J4SerialError("invalid_response");
      this.configRequestId = Number(result.request_id);
      // 202 is admission, not completion. Poll only this pending operation, bounded.
      if (this.snapshot.configuring) this.pollConfiguration(epoch, 0);
    } catch (reason) {
      if (epoch !== this.epoch) return;
      this.configRequestId = null;
      const error = this.errorInfo(reason);
      this.update({ configuring: false, error });
      // A CDC line-coding/CAS conflict is not retried; refresh the actual tuple.
      try {
        const current = parseJ4SerialState(await this.request({ signal: this.abort?.signal }));
        if (epoch === this.epoch) this.applyState(current);
      } catch { /* Preserve the useful conflict instead of replacing it. */ }
      throw reason;
    }
  }

  private pollConfiguration(epoch: number, attempt: number) {
    if (epoch !== this.epoch || !this.snapshot.configuring) return;
    if (attempt >= 20) {
      this.fail(new J4SerialError("config_timeout", "J4 configuration completion was not reported"));
      return;
    }
    this.configTimer = setTimeout(() => {
      this.configTimer = null;
      void this.request({ signal: this.abort?.signal }).then((data) => {
        if (epoch !== this.epoch) return;
        this.applyState(parseJ4SerialState(data));
        this.pollConfiguration(epoch, attempt + 1);
      }).catch((reason) => {
        if (epoch === this.epoch) this.fail(reason);
      });
    }, 250);
  }

  writeText(text: string): Promise<void> {
    // Reject oversized pastes before allocating their UTF-8 representation.
    if (text.length > J4_SERIAL_BUFFER_LIMIT) return this.rejectPressure();
    return this.writeBytes(new TextEncoder().encode(text));
  }

  writeBytes(bytes: Uint8Array): Promise<void> {
    if (bytes.length === 0) return Promise.resolve();
    const ws = this.ws;
    const serial = this.snapshot.serial;
    if (this.snapshot.phase !== "connected" || !ws || ws.readyState !== WebSocket.OPEN
      || serial?.state !== "ready" || serial.read_only || this.snapshot.configuring) {
      return Promise.reject(new J4SerialError("read_only"));
    }
    const chunks = Math.ceil(bytes.length / J4_SERIAL_FRAME_BYTES);
    if (chunks + this.pending.length > J4_SERIAL_QUEUE_LIMIT
      || ws.bufferedAmount + bytes.length > J4_SERIAL_BUFFER_LIMIT) {
      return this.rejectPressure();
    }
    if (this.snapshot.error?.code === "backpressure") this.update({ error: null });
    const operations: Promise<void>[] = [];
    for (let offset = 0; offset < bytes.length; offset += J4_SERIAL_FRAME_BYTES) {
      const payload = bytes.slice(offset, offset + J4_SERIAL_FRAME_BYTES);
      operations.push(new Promise<void>((resolve, reject) => {
        const pending: PendingWrite = { bytes: payload.length, payload, sent: false, resolve, reject };
        this.pending.push(pending);
      }));
    }
    this.refreshPressure();
    return Promise.all(operations).then(() => {});
  }

  private watchPressure() {
    if (this.pressureTimer) return;
    this.pressureTimer = setInterval(() => this.refreshPressure(), 100);
  }

  private rejectPressure(): Promise<void> {
    this.update({ backpressure: true, error: { code: "backpressure", message: "J4 input buffer is full; input was not sent" } });
    this.watchPressure();
    return Promise.reject(new J4SerialError("backpressure"));
  }

  private refreshPressure() {
    this.pump();
    const next = this.pending.find((item) => !item.sent);
    const backpressure = this.pending.length >= J4_SERIAL_QUEUE_LIMIT
      || (this.ws?.bufferedAmount ?? 0) + (next?.bytes ?? 0) >= J4_SERIAL_BUFFER_LIMIT;
    this.update({ pendingWrites: this.pending.length, backpressure });
    if (!backpressure && this.pressureTimer) { clearInterval(this.pressureTimer); this.pressureTimer = null; }
    if (backpressure) this.watchPressure();
  }

  private pump() {
    // Firmware permits one in-flight write per WS. The other bounded slots
    // stay client-local until its physical-complete event releases admission.
    const ws = this.ws;
    if (!ws || ws.readyState !== WebSocket.OPEN || this.pending.some((item) => item.sent)) return;
    const pending = this.pending[0];
    if (!pending) return;
    if (ws.bufferedAmount + pending.bytes > J4_SERIAL_BUFFER_LIMIT) { this.watchPressure(); return; }
    pending.sent = true;
    pending.timer = setTimeout(() => this.fail(new J4SerialError("tx_timeout")), 5_000);
    try { ws.send(pending.payload); } catch (reason) { this.fail(reason); }
  }

  private rejectWrites(reason: Error) {
    for (const pending of this.pending) { clearTimeout(pending.timer); pending.reject(reason); }
    this.pending = [];
  }

  private errorInfo(reason: unknown) {
    return { code: reason instanceof J4SerialError ? reason.code : "connection_error",
      message: reason instanceof Error ? reason.message : String(reason) };
  }

  private fail(reason: unknown) {
    const error = this.errorInfo(reason);
    void this.disconnect();
    this.update({ error, phase: error.code === "unavailable" || error.code === "not_found" || error.code === "unsupported_topic" || error.code === "serial_unavailable"
      ? "unavailable" : "disconnected" });
  }

  async disconnect(): Promise<void> {
    this.epoch += 1;
    this.abort?.abort(); this.abort = null;
    this.configRequestId = null;
    this.rejectWrites(new J4SerialError("disconnected"));
    if (this.pressureTimer) clearInterval(this.pressureTimer);
    if (this.configTimer) clearTimeout(this.configTimer);
    if (this.connectTimer) clearTimeout(this.connectTimer);
    this.pressureTimer = this.configTimer = this.connectTimer = null;
    const ws = this.ws; this.ws = null;
    const sessionId = this.sessionId; this.sessionId = null;
    this.update({ phase: "disconnected", pendingWrites: 0, backpressure: false, configuring: false });
    if (ws) {
      ws.onopen = ws.onmessage = ws.onclose = ws.onerror = null;
      if (ws.readyState !== 3) {
        // DELETE refuses connected firmware sessions. Give the close handshake
        // a bounded chance to detach first; never delete another client's slot.
        await new Promise<void>((resolve) => {
          const timer = setTimeout(() => { ws.onclose = null; resolve(); }, 750);
          ws.onclose = () => { clearTimeout(timer); ws.onclose = null; resolve(); };
          try { ws.close(); } catch { clearTimeout(timer); ws.onclose = null; resolve(); }
        });
      }
    }
    if (sessionId !== null) await api.deleteLiveSession(sessionId).catch(() => {});
  }
}
