import { J4SerialClient, emptyJ4Snapshot, type J4SerialSnapshot } from "./j4Serial";
import { J4SerialError, type J4SerialChannel, type J4SerialConfig } from "./j4SerialProtocol";

export type J4SerialConnectionState = "disconnected" | "connecting" | "unavailable" | "ready" | "paused" | "faulted" | "reconfig" | "read_only";
export interface J4SerialSessionSnapshot extends J4SerialSnapshot { automationActive: boolean }

export function j4ConnectionState(snapshot: J4SerialSnapshot): J4SerialConnectionState {
  if (snapshot.phase !== "connected") return snapshot.phase;
  if (snapshot.configuring) return "reconfig";
  if (snapshot.serial?.state !== "ready") return snapshot.serial?.state ?? "read_only";
  return snapshot.serial.read_only ? "read_only" : "ready";
}

export interface SerialAutomationHandle {
  getState: () => J4SerialSessionSnapshot;
  isConnected: (channel?: J4SerialChannel) => boolean;
  connectedChannels: () => J4SerialChannel[];
  clear: (channel?: J4SerialChannel) => void;
  write: (text: string, channel?: J4SerialChannel) => Promise<void>;
  setAutomationActive: (active: boolean, channel?: J4SerialChannel) => void;
  subscribe: (listener: (text: string, receivedAtMs: number) => void, channel?: J4SerialChannel) => () => void;
}

/** App-owned backend: terminal and automation share exactly one MCU session.
 * Independent future pipelines can instantiate their own owner; there is no
 * process-global socket, DOM dependency, channel alias, or reconnect replay.
 */
export class J4SerialSession implements SerialAutomationHandle {
  private snapshot: J4SerialSessionSnapshot = { ...emptyJ4Snapshot(), automationActive: false };
  private readonly client: J4SerialClient;
  private decoder = new TextDecoder();
  private readonly stateListeners = new Set<() => void>();
  private readonly byteListeners = new Set<(bytes: Uint8Array) => void>();
  private readonly textListeners = new Set<(text: string, receivedAtMs: number) => void>();
  private readonly clearListeners = new Set<() => void>();

  constructor() {
    this.client = new J4SerialClient((next) => {
      if (next.phase === "connecting" || next.phase === "disconnected"
        || next.serial?.generation !== this.snapshot.serial?.generation) this.decoder = new TextDecoder();
      this.snapshot = { ...next, automationActive: this.snapshot.automationActive };
      this.publish();
    }, (bytes) => {
      this.byteListeners.forEach((listener) => listener(bytes));
      const text = this.decoder.decode(bytes, { stream: true });
      if (text) {
        const now = performance.now();
        this.textListeners.forEach((listener) => listener(text, now));
      }
    });
  }

  private publish() { this.stateListeners.forEach((listener) => listener()); }
  getState = (): J4SerialSessionSnapshot => this.snapshot;
  subscribeState = (listener: () => void) => { this.stateListeners.add(listener); return () => { this.stateListeners.delete(listener); }; };
  isConnected = (_channel?: J4SerialChannel) => j4ConnectionState(this.snapshot) === "ready";
  connectedChannels = (): J4SerialChannel[] => this.isConnected() ? ["j4"] : [];
  connect = () => this.client.connect();
  disconnect = () => this.client.disconnect();
  configure = (config: J4SerialConfig) => this.client.configure(config);
  write = (text: string, _channel?: J4SerialChannel) => this.client.writeText(text);
  writeManual = (text: string) => this.snapshot.automationActive
    ? Promise.reject(new J4SerialError("automation_active")) : this.client.writeText(text);
  writeBinaryManual = (bytes: Uint8Array) => this.snapshot.automationActive
    ? Promise.reject(new J4SerialError("automation_active")) : this.client.writeBytes(bytes);
  setAutomationActive = (active: boolean, _channel?: J4SerialChannel) => {
    if (active && this.snapshot.automationActive) throw new J4SerialError("automation_active");
    this.snapshot = { ...this.snapshot, automationActive: active }; this.publish();
  };
  subscribe = (listener: (text: string, receivedAtMs: number) => void, _channel?: J4SerialChannel) => {
    this.textListeners.add(listener); return () => { this.textListeners.delete(listener); };
  };
  subscribeBytes = (listener: (bytes: Uint8Array) => void) => {
    this.byteListeners.add(listener); return () => { this.byteListeners.delete(listener); };
  };
  subscribeClear = (listener: () => void) => {
    this.clearListeners.add(listener); return () => { this.clearListeners.delete(listener); };
  };
  clear = (_channel?: J4SerialChannel) => {
    this.decoder = new TextDecoder(); this.clearListeners.forEach((listener) => listener());
  };
}
