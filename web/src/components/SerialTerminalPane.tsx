import {
  forwardRef,
  useEffect,
  useImperativeHandle,
  useRef,
  useState,
} from "react";
import type {
  ClipboardEvent as ReactClipboardEvent,
  KeyboardEvent as ReactKeyboardEvent,
  MouseEvent as ReactMouseEvent,
} from "react";
import {
  Copy,
  Download,
  Maximize2,
  Minus,
  Plug,
  Plus,
  Trash2,
  Usb,
} from "lucide-react";
import type { Terminal as XTerm } from "@xterm/xterm";
import type { FitAddon } from "@xterm/addon-fit";
import "@xterm/xterm/css/xterm.css";
import { Button } from "./ui";
import { useI18n } from "@/lib/i18n";
import {
  appendSerialLogChunk,
  clearSerialLog,
  readSerialLog,
  SERIAL_LOG_RESTORE_BYTES,
  serialLogFilename,
} from "@/lib/serialLogCache";
import {
  isLinuxSerialHost,
  LINUX_SERIAL_PERMISSION_COMMAND,
  shouldShowLinuxSerialPermissionHelp,
} from "@/lib/serialPermissions";
import { downloadBlob, formatBytes } from "@/lib/utils";

const BAUD_RATES = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1500000];
const BRIDGE_PORT = 8787;
const MIN_FONT_SIZE = 10;
const MAX_FONT_SIZE = 20;
const LOG_FLUSH_INTERVAL_MS = 100;
const LOG_FLUSH_CHARS = 64 * 1024;
const LOG_CACHE_RETRY_BASE_MS = 1000;
const LOG_CACHE_RETRY_MAX_MS = 60_000;

export type SerialChannelId = "uart0" | "uart1";
type Source = "webserial" | "bridge" | null;
type LineEnding = "crlf" | "cr" | "lf";

export interface SerialChannelStatus {
  connected: boolean;
  connecting: boolean;
  automationActive: boolean;
  source: Source;
  portInfo: string;
  rxBytes: number;
  txBytes: number;
}

export interface SerialChannelHandle {
  isConnected: () => boolean;
  disconnect: () => Promise<void>;
  clear: () => void;
  write: (data: string) => Promise<void>;
  setAutomationActive: (active: boolean) => void;
  subscribe: (listener: (text: string, receivedAtMs: number) => void) => () => void;
}

const TERMINAL_KEYS: Record<string, string> = {
  Backspace: "\u007f",
  Tab: "\t",
  Escape: "\u001b",
  ArrowUp: "\u001b[A",
  ArrowDown: "\u001b[B",
  ArrowRight: "\u001b[C",
  ArrowLeft: "\u001b[D",
  Home: "\u001b[H",
  End: "\u001b[F",
  Insert: "\u001b[2~",
  Delete: "\u001b[3~",
  PageUp: "\u001b[5~",
  PageDown: "\u001b[6~",
};

const LINE_ENDINGS: Record<LineEnding, string> = {
  crlf: "\r\n",
  cr: "\r",
  lf: "\n",
};

function terminalKeyData(event: ReactKeyboardEvent<HTMLDivElement>, lineEnding: LineEnding) {
  if (event.metaKey) return null;
  if (event.key === "Enter") return LINE_ENDINGS[lineEnding];
  if (event.ctrlKey && event.key.length === 1) {
    const code = event.key.toUpperCase().charCodeAt(0);
    if (code >= 64 && code <= 95) return String.fromCharCode(code - 64);
  }
  const mapped = TERMINAL_KEYS[event.key];
  if (mapped) return mapped;
  if (!event.ctrlKey && event.key.length === 1) {
    return event.altKey ? `\u001b${event.key}` : event.key;
  }
  return null;
}

function terminalTheme() {
  if (document.documentElement.dataset.theme === "light") {
    return {
      background: "#ffffff",
      foreground: "#0f172a",
      cursor: "#356df3",
      selectionBackground: "#356df333",
      black: "#172033",
      brightBlack: "#64748b",
      white: "#e2e8f0",
      brightWhite: "#0f172a",
    };
  }
  return {
    background: "#07090c",
    foreground: "#d7e1ef",
    cursor: "#72a2ff",
    selectionBackground: "#72a2ff40",
    black: "#07090c",
    brightBlack: "#64748b",
    white: "#d7e1ef",
    brightWhite: "#ffffff",
  };
}

export const SerialTerminalPane = forwardRef<SerialChannelHandle, {
  channel: SerialChannelId;
  visible: boolean;
  compact: boolean;
  webSerialSupported: boolean;
  requestPort: (channel: SerialChannelId) => Promise<SerialPort>;
  releasePort: (channel: SerialChannelId, port: SerialPort, physicalDisconnect: boolean) => void;
  onStatus: (channel: SerialChannelId, status: SerialChannelStatus) => void;
  onOpenWebSerialSetup: (event: ReactMouseEvent<HTMLButtonElement>) => void;
}>(function SerialTerminalPane({
  channel,
  visible,
  compact,
  webSerialSupported,
  requestPort,
  releasePort,
  onStatus,
  onOpenWebSerialSetup,
}, ref) {
  const { t } = useI18n();
  const [source, setSource] = useState<Source>(null);
  const [connecting, setConnecting] = useState(false);
  const [baud, setBaud] = useState(115200);
  const [fontSize, setFontSize] = useState(13);
  const [lineEnding, setLineEnding] = useState<LineEnding>("crlf");
  const [portInfo, setPortInfo] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [rxBytes, setRxBytes] = useState(0);
  const [txBytes, setTxBytes] = useState(0);
  const [automationActive, setAutomationActive] = useState(false);
  const [cachedLogBytes, setCachedLogBytes] = useState(0);
  const [logCacheError, setLogCacheError] = useState<string | null>(null);
  const [showLinuxSerialHelp, setShowLinuxSerialHelp] = useState(false);
  const [serialFixCopyState, setSerialFixCopyState] = useState<"idle" | "copied" | "failed">("idle");

  const sourceRef = useRef<Source>(null);
  const portRef = useRef<SerialPort | null>(null);
  const readerRef = useRef<ReadableStreamDefaultReader<Uint8Array> | null>(null);
  const readTaskRef = useRef<Promise<void> | null>(null);
  const wsRef = useRef<WebSocket | null>(null);
  const terminalPanelRef = useRef<HTMLDivElement | null>(null);
  const terminalHostRef = useRef<HTMLDivElement | null>(null);
  const termRef = useRef<XTerm | null>(null);
  const fitAddonRef = useRef<FitAddon | null>(null);
  const pendingOutputRef = useRef("");
  const receiveListenersRef = useRef(new Set<(text: string, receivedAtMs: number) => void>());
  const writeQueueRef = useRef<Promise<void>>(Promise.resolve());
  const writeGenerationRef = useRef(0);
  const automationActiveRef = useRef(false);
  const pendingLogRef = useRef("");
  const logFlushTimerRef = useRef<number | null>(null);
  const logWriteQueueRef = useRef<Promise<void>>(Promise.resolve());
  const logCacheGenerationRef = useRef(0);
  const logCacheFailureCountRef = useRef(0);
  const logCacheRetryAfterRef = useRef(0);

  function reportLogCacheError(reason: unknown) {
    setLogCacheError(reason instanceof Error ? reason.message : String(reason));
  }

  function recordLogCacheFailure(reason: unknown, updateState: boolean) {
    const failureCount = Math.min(logCacheFailureCountRef.current + 1, 16);
    logCacheFailureCountRef.current = failureCount;
    logCacheRetryAfterRef.current = Date.now() + Math.min(
      LOG_CACHE_RETRY_BASE_MS * (2 ** (failureCount - 1)),
      LOG_CACHE_RETRY_MAX_MS
    );
    pendingLogRef.current = "";
    if (updateState) reportLogCacheError(reason);
  }

  function resetLogCacheFailure(updateState: boolean) {
    logCacheFailureCountRef.current = 0;
    logCacheRetryAfterRef.current = 0;
    if (updateState) setLogCacheError(null);
  }

  function flushLogCache(updateState = true): Promise<void> {
    if (logFlushTimerRef.current != null) {
      window.clearTimeout(logFlushTimerRef.current);
      logFlushTimerRef.current = null;
    }
    const text = pendingLogRef.current;
    pendingLogRef.current = "";
    if (!text) return logWriteQueueRef.current;

    const generation = logCacheGenerationRef.current;
    const operation = logWriteQueueRef.current
      .then(() => {
        if (Date.now() < logCacheRetryAfterRef.current) return undefined;
        return appendSerialLogChunk(channel, text);
      })
      .then((totalBytes) => {
        if (totalBytes == null) return;
        resetLogCacheFailure(updateState && generation === logCacheGenerationRef.current);
        if (updateState && generation === logCacheGenerationRef.current) {
          setCachedLogBytes(totalBytes);
        }
      });
    logWriteQueueRef.current = operation.catch(() => {});
    operation.catch((reason) => recordLogCacheFailure(reason, updateState));
    return operation;
  }

  function scheduleLogCache(text: string) {
    if (!text || Date.now() < logCacheRetryAfterRef.current) return;
    pendingLogRef.current += text;
    if (pendingLogRef.current.length >= LOG_FLUSH_CHARS) {
      void flushLogCache();
      return;
    }
    if (logFlushTimerRef.current == null) {
      logFlushTimerRef.current = window.setTimeout(() => {
        void flushLogCache();
      }, LOG_FLUSH_INTERVAL_MS);
    }
  }

  const append = (text: string) => {
    if (termRef.current) termRef.current.write(text);
    else pendingOutputRef.current += text;
  };

  const appendReceived = (text: string) => {
    append(text);
    scheduleLogCache(text);
    const receivedAtMs = performance.now();
    receiveListenersRef.current.forEach((listener) => listener(text, receivedAtMs));
  };

  const clear = () => {
    const generation = logCacheGenerationRef.current + 1;
    logCacheGenerationRef.current = generation;
    setRxBytes(0);
    setCachedLogBytes(0);
    pendingLogRef.current = "";
    if (logFlushTimerRef.current != null) {
      window.clearTimeout(logFlushTimerRef.current);
      logFlushTimerRef.current = null;
    }
    const operation = logWriteQueueRef.current
      .then(() => clearSerialLog(channel))
      .then(() => {
        resetLogCacheFailure(generation === logCacheGenerationRef.current);
        if (generation === logCacheGenerationRef.current) setCachedLogBytes(0);
      });
    logWriteQueueRef.current = operation.catch(() => {});
    operation.catch((reason) => recordLogCacheFailure(reason, true));
    termRef.current?.clear();
    termRef.current?.write("\u001b[2J\u001b[H");
  };

  const updateAutomationActive = (active: boolean) => {
    automationActiveRef.current = active;
    setAutomationActive(active);
  };

  async function disconnect(physicalDisconnect = false) {
    const reader = readerRef.current;
    const readTask = readTaskRef.current;
    const port = portRef.current;
    const ws = wsRef.current;

    readerRef.current = null;
    readTaskRef.current = null;
    portRef.current = null;
    wsRef.current = null;
    sourceRef.current = null;
    writeGenerationRef.current += 1;
    setConnecting(false);

    try { await reader?.cancel(); } catch { /* The OS may already have removed the device. */ }
    try { await readTask; } catch { /* Read errors are reported by the read task. */ }
    try { await port?.close(); } catch { /* A physically disconnected port is already closed. */ }
    try { ws?.close(); } catch { /* Ignore an already-closed bridge socket. */ }
    if (port) releasePort(channel, port, physicalDisconnect);
    setSource(null);
    setPortInfo("");
  }

  useEffect(() => {
    sourceRef.current = source;
    if (termRef.current) {
      termRef.current.options.cursorBlink = !!source;
      if (source && visible) termRef.current.focus();
    }
  }, [source, visible]);

  useEffect(() => {
    automationActiveRef.current = automationActive;
  }, [automationActive]);

  useEffect(() => {
    onStatus(channel, {
      connected: !!source,
      connecting,
      automationActive,
      source,
      portInfo,
      rxBytes,
      txBytes,
    });
  }, [automationActive, channel, connecting, onStatus, portInfo, rxBytes, source, txBytes]);

  useEffect(() => {
    const host = terminalHostRef.current;
    if (!host) return;
    let disposed = false;
    let term: XTerm | null = null;
    let fitAddon: FitAddon | null = null;
    let resizeObserver: ResizeObserver | null = null;
    let themeObserver: MutationObserver | null = null;
    const fit = () => {
      try { fitAddon?.fit(); } catch { /* Ignore transient layout changes. */ }
    };
    document.addEventListener("fullscreenchange", fit);

    void (async () => {
      const [{ Terminal }, { FitAddon }, cachedLog] = await Promise.all([
        import("@xterm/xterm"),
        import("@xterm/addon-fit"),
        readSerialLog(channel, SERIAL_LOG_RESTORE_BYTES).then(
          (snapshot) => ({ snapshot, error: null as unknown }),
          (error: unknown) => ({
            snapshot: { text: "", totalBytes: 0 },
            error,
          })
        ),
      ]);
      if (disposed) return;
      if (cachedLog.error) reportLogCacheError(cachedLog.error);
      else setCachedLogBytes(cachedLog.snapshot.totalBytes);
      term = new Terminal({
        cursorBlink: !!sourceRef.current,
        fontFamily: "ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace",
        fontSize,
        lineHeight: 1.15,
        scrollback: 10000,
        theme: terminalTheme(),
      });
      fitAddon = new FitAddon();
      term.loadAddon(fitAddon);
      term.open(host);
      termRef.current = term;
      fitAddonRef.current = fitAddon;
      if (cachedLog.snapshot.text) term.write(cachedLog.snapshot.text);
      if (pendingOutputRef.current) {
        term.write(pendingOutputRef.current);
        pendingOutputRef.current = "";
      }
      resizeObserver = new ResizeObserver(fit);
      resizeObserver.observe(host);
      themeObserver = new MutationObserver(() => {
        if (term) term.options.theme = terminalTheme();
      });
      themeObserver.observe(document.documentElement, {
        attributes: true,
        attributeFilter: ["data-theme"],
      });
      requestAnimationFrame(fit);
    })();

    return () => {
      disposed = true;
      void flushLogCache(false);
      void disconnect();
      resizeObserver?.disconnect();
      themeObserver?.disconnect();
      document.removeEventListener("fullscreenchange", fit);
      term?.dispose();
      termRef.current = null;
      fitAddonRef.current = null;
    };
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    const flushBeforeBackground = () => {
      if (document.visibilityState === "hidden") void flushLogCache();
    };
    const flushBeforeUnload = () => void flushLogCache(false);
    document.addEventListener("visibilitychange", flushBeforeBackground);
    window.addEventListener("pagehide", flushBeforeUnload);
    return () => {
      document.removeEventListener("visibilitychange", flushBeforeBackground);
      window.removeEventListener("pagehide", flushBeforeUnload);
    };
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (!visible) return;
    requestAnimationFrame(() => {
      try { fitAddonRef.current?.fit(); } catch { /* Ignore a transient layout change. */ }
    });
  }, [compact, visible]);

  useEffect(() => {
    if (!termRef.current) return;
    termRef.current.options.fontSize = fontSize;
    requestAnimationFrame(() => {
      try { fitAddonRef.current?.fit(); } catch { /* Ignore transient layout changes. */ }
    });
  }, [fontSize]);

  async function connectWebSerial() {
    setError(null);
    setShowLinuxSerialHelp(false);
    setSerialFixCopyState("idle");
    setConnecting(true);
    let failureStage: "request" | "open" = "request";
    try {
      const port = await requestPort(channel);
      portRef.current = port;
      failureStage = "open";
      await port.open({ baudRate: baud });
      const decoder = new TextDecoder();
      const reader = port.readable!.getReader();
      readerRef.current = reader;
      const readTask = (async () => {
        try {
          while (true) {
            const { value, done } = await reader.read();
            if (done) break;
            if (value) {
              setRxBytes((count) => count + value.byteLength);
              appendReceived(decoder.decode(value, { stream: true }));
            }
          }
        } catch (reason) {
          if (readerRef.current === reader) {
            append(`\r\n[read error: ${reason instanceof Error ? reason.message : reason}]\r\n`);
          }
        } finally {
          const tail = decoder.decode();
          if (tail) appendReceived(tail);
          try { reader.releaseLock(); } catch { /* The lock may already be released. */ }
          if (readerRef.current === reader) readerRef.current = null;
        }
      })();
      readTaskRef.current = readTask;
      sourceRef.current = "webserial";
      setSource("webserial");
      setShowLinuxSerialHelp(false);
      setPortInfo(`CH347F · Web Serial`);
      setConnecting(false);
      port.addEventListener("disconnect", () => {
        if (portRef.current !== port) return;
        append("\r\n[device disconnected]\r\n");
        void disconnect(true);
      }, { once: true });
    } catch (reason) {
      await disconnect();
      setError(reason instanceof Error ? reason.message : String(reason));
      setShowLinuxSerialHelp(shouldShowLinuxSerialPermissionHelp({
        isLinux: isLinuxSerialHost(navigator),
        stage: failureStage,
        error: reason,
      }));
    }
  }

  function connectBridge() {
    setError(null);
    setShowLinuxSerialHelp(false);
    setSerialFixCopyState("idle");
    if (wsRef.current) {
      try { wsRef.current.close(); } catch { /* Ignore a stale bridge socket. */ }
      wsRef.current = null;
    }
    setConnecting(true);
    const ws = new WebSocket(`ws://127.0.0.1:${BRIDGE_PORT}/serial`);
    wsRef.current = ws;
    ws.onopen = () => ws.send(JSON.stringify({ type: "open", channel, baud }));
    ws.onmessage = (event) => {
      try {
        const msg = JSON.parse(event.data);
        if (msg.type === "data") {
          const text = String(msg.text ?? "");
          setRxBytes((count) => count + new TextEncoder().encode(text).byteLength);
          appendReceived(text);
        } else if (msg.type === "opened") {
          if (wsRef.current !== ws) return;
          sourceRef.current = "bridge";
          setConnecting(false);
          setSource("bridge");
          setShowLinuxSerialHelp(false);
          setPortInfo(`${msg.path} · bridge`);
        } else if (msg.type === "error") {
          setConnecting(false);
          const message = String(msg.message ?? "");
          setError(message);
          setShowLinuxSerialHelp(shouldShowLinuxSerialPermissionHelp({
            isLinux: isLinuxSerialHost(navigator),
            stage: "bridge",
            error: message,
          }));
        } else if (msg.type === "closed") {
          append("\r\n[bridge closed]\r\n");
          void disconnect();
        }
      } catch {
        /* Ignore malformed bridge frames. */
      }
    };
    ws.onerror = () => {
      if (wsRef.current !== ws) return;
      setConnecting(false);
      setError(t("serial.bridgeError"));
      setShowLinuxSerialHelp(false);
    };
    ws.onclose = () => {
      if (wsRef.current !== ws) return;
      wsRef.current = null;
      sourceRef.current = null;
      setConnecting(false);
      setSource(null);
      setPortInfo("");
    };
  }

  async function writeSerial(data: string) {
    const activeSource = sourceRef.current;
    if (!activeSource) throw new Error(t("serial.disconnectedWrite"));
    if (!data) return;
    const bytes = new TextEncoder().encode(data);
    try {
      if (activeSource === "webserial" && portRef.current?.writable) {
        const writer = portRef.current.writable.getWriter();
        try { await writer.write(bytes); } finally { writer.releaseLock(); }
      } else if (activeSource === "bridge" && wsRef.current?.readyState === WebSocket.OPEN) {
        wsRef.current.send(data);
      } else {
        throw new Error(t("serial.disconnectedWrite"));
      }
      setTxBytes((count) => count + bytes.byteLength);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
      throw reason;
    }
  }

  function enqueueSerial(data: string): Promise<void> {
    if (!sourceRef.current) return Promise.reject(new Error(t("serial.disconnectedWrite")));
    if (!data) return Promise.resolve();
    const generation = writeGenerationRef.current;
    const operation = writeQueueRef.current.then(() => {
      if (generation !== writeGenerationRef.current) return;
      return writeSerial(data);
    });
    writeQueueRef.current = operation.catch(() => {});
    return operation;
  }

  useImperativeHandle(ref, () => ({
    isConnected: () => !!sourceRef.current,
    disconnect: () => disconnect(),
    clear,
    write: enqueueSerial,
    setAutomationActive: updateAutomationActive,
    subscribe: (listener) => {
      receiveListenersRef.current.add(listener);
      return () => receiveListenersRef.current.delete(listener);
    },
  }), []); // eslint-disable-line react-hooks/exhaustive-deps

  async function copySelection() {
    const selection = termRef.current?.getSelection();
    if (!selection) return;
    try { await navigator.clipboard.writeText(selection); } catch { /* Clipboard permission may be denied. */ }
  }

  async function downloadCachedLog() {
    try {
      await flushLogCache();
      const snapshot = await readSerialLog(channel);
      setCachedLogBytes(snapshot.totalBytes);
      setLogCacheError(null);
      if (!snapshot.text) return;
      downloadBlob(
        serialLogFilename(channel),
        snapshot.text,
        "text/plain;charset=utf-8"
      );
    } catch (reason) {
      reportLogCacheError(reason);
    }
  }

  async function copyLinuxSerialFix() {
    try {
      await navigator.clipboard.writeText(LINUX_SERIAL_PERMISSION_COMMAND);
      setSerialFixCopyState("copied");
    } catch {
      setSerialFixCopyState("failed");
    }
  }

  async function toggleFullscreen() {
    const panel = terminalPanelRef.current;
    if (!panel) return;
    if (document.fullscreenElement === panel) await document.exitFullscreen();
    else await panel.requestFullscreen();
  }

  function handleTerminalKeyDown(event: ReactKeyboardEvent<HTMLDivElement>) {
    if (automationActiveRef.current) {
      event.preventDefault();
      event.stopPropagation();
      return;
    }
    const data = terminalKeyData(event, lineEnding);
    if (!data || !sourceRef.current) return;
    event.preventDefault();
    event.stopPropagation();
    enqueueSerial(data);
  }

  function handleTerminalPaste(event: ReactClipboardEvent<HTMLDivElement>) {
    if (automationActiveRef.current) {
      event.preventDefault();
      event.stopPropagation();
      return;
    }
    const data = event.clipboardData.getData("text");
    if (!data || !sourceRef.current) return;
    event.preventDefault();
    event.stopPropagation();
    const normalized = data.replaceAll("\r\n", "\n").replaceAll("\r", "\n");
    enqueueSerial(normalized.replaceAll("\n", LINE_ENDINGS[lineEnding]));
  }

  function handleWebSerialClick(event: ReactMouseEvent<HTMLButtonElement>) {
    if (webSerialSupported) {
      void connectWebSerial();
      return;
    }
    onOpenWebSerialSetup(event);
  }

  const channelLabel = channel.toUpperCase();

  return (
    <section
      aria-label={`${channelLabel} ${t("serial.console")}`}
      className="flex min-h-0 min-w-0 flex-1 flex-col overflow-hidden rounded-xl border border-line/80 bg-terminal shadow-inner transition-colors"
    >
      <div className="flex min-h-11 flex-wrap items-center justify-between gap-2 border-b border-line/70 bg-terminal px-3 py-2 text-terminal-ink transition-colors">
        <div className="flex min-w-0 items-center gap-2">
          <span className={`h-2 w-2 shrink-0 rounded-full ${source ? "bg-ok shadow-[0_0_8px_rgb(var(--c-ok))]" : connecting ? "animate-pulse bg-warn" : "bg-ink-dim/50"}`} />
          <span className="text-[11px] font-bold uppercase tracking-[0.12em]">{channelLabel}</span>
          <span className="text-[10px] text-ink-dim">
            {connecting ? t("serial.connecting") : source ? t("serial.connected") : t("serial.disconnected")}
          </span>
          {portInfo && <span className="max-w-[180px] truncate text-[10px] text-ink-dim">{portInfo}</span>}
        </div>
        <div className="flex flex-wrap items-center justify-end gap-1">
          {!source && !connecting && (
            <>
              <Button
                variant={webSerialSupported ? "primary" : "danger"}
                className="min-h-7 rounded-md px-2 py-1 text-[10px]"
                onClick={handleWebSerialClick}
              >
                <Usb size={12} /> {t("serial.webSerial")}
              </Button>
              <Button variant="default" className="min-h-7 rounded-md px-2 py-1 text-[10px]" onClick={connectBridge}>
                <Plug size={12} /> {t("serial.bridge")}
              </Button>
            </>
          )}
          {(source || connecting) && (
            <Button variant="ghost" disabled={automationActive} className="min-h-7 rounded-md px-2 py-1 text-[10px]" onClick={() => void disconnect()}>
              {t("serial.disconnect")}
            </Button>
          )}
          <select
            value={baud}
            onChange={(event) => setBaud(Number(event.target.value))}
            disabled={!!source || connecting}
            aria-label={`${channelLabel} ${t("serial.baud")}`}
            className="h-7 rounded-md border border-line/80 bg-panel px-1.5 text-[10px] text-ink outline-none transition-colors hover:bg-panel2 disabled:opacity-60"
          >
            {BAUD_RATES.map((rate) => <option key={rate} value={rate}>{rate}</option>)}
          </select>
          <select
            value={lineEnding}
            onChange={(event) => setLineEnding(event.target.value as LineEnding)}
            aria-label={`${channelLabel} ${t("serial.lineEnding")}`}
            className="h-7 rounded-md border border-line/80 bg-panel px-1.5 text-[10px] font-medium text-ink outline-none transition-colors hover:bg-panel2"
          >
            <option value="crlf">CRLF</option>
            <option value="cr">CR</option>
            <option value="lf">LF</option>
          </select>
          <button type="button" onClick={() => setFontSize((size) => Math.max(MIN_FONT_SIZE, size - 1))} className="grid h-7 w-7 place-items-center rounded-md text-ink-dim transition-colors hover:bg-panel2 hover:text-ink" aria-label={`${channelLabel} ${t("serial.fontDown")}`}><Minus size={13} /></button>
          <button type="button" onClick={() => setFontSize((size) => Math.min(MAX_FONT_SIZE, size + 1))} className="grid h-7 w-7 place-items-center rounded-md text-ink-dim transition-colors hover:bg-panel2 hover:text-ink" aria-label={`${channelLabel} ${t("serial.fontUp")}`}><Plus size={13} /></button>
          <button type="button" onClick={() => void copySelection()} className="grid h-7 w-7 place-items-center rounded-md text-ink-dim transition-colors hover:bg-panel2 hover:text-ink" aria-label={`${channelLabel} ${t("serial.copy")}`}><Copy size={13} /></button>
          <button type="button" disabled={cachedLogBytes === 0 && !pendingLogRef.current} onClick={() => void downloadCachedLog()} className="grid h-7 w-7 place-items-center rounded-md text-ink-dim transition-colors hover:bg-panel2 hover:text-ink disabled:opacity-30" aria-label={`${channelLabel} ${t("serial.downloadLog")}`}><Download size={13} /></button>
          <button type="button" onClick={clear} className="grid h-7 w-7 place-items-center rounded-md text-ink-dim transition-colors hover:bg-panel2 hover:text-ink" aria-label={`${channelLabel} ${t("serial.clear")}`}><Trash2 size={13} /></button>
          <button type="button" onClick={() => void toggleFullscreen()} className="grid h-7 w-7 place-items-center rounded-md text-ink-dim transition-colors hover:bg-panel2 hover:text-ink" aria-label={`${channelLabel} ${t("serial.fullscreen")}`}><Maximize2 size={13} /></button>
        </div>
      </div>

      {error && (
        <div role="alert" className="border-b border-danger/30 bg-danger/10 px-3 py-2 text-xs">
          <p className="break-words font-medium text-danger">{error}</p>
          {showLinuxSerialHelp && (
            <div className="mt-2 max-w-[72ch] text-ink">
              <p className="font-semibold">{t("serial.linuxPermissionTitle")}</p>
              <p className="mt-1 text-ink-dim">{t("serial.linuxPermissionBody")}</p>
              <p className="mt-1 text-ink-dim">{t("serial.linuxPermissionDevices")}</p>
              <div className="mt-2 flex min-w-0 flex-wrap items-center gap-2">
                <code className="min-w-0 flex-1 overflow-x-auto rounded-md border border-line/80 bg-panel px-2 py-1.5 text-[11px] text-ink">
                  {LINUX_SERIAL_PERMISSION_COMMAND}
                </code>
                <Button
                  variant="default"
                  className="min-h-8 shrink-0 rounded-md px-2 py-1 text-[11px]"
                  onClick={() => void copyLinuxSerialFix()}
                >
                  <Copy size={12} />
                  {serialFixCopyState === "copied"
                    ? t("serial.linuxPermissionCopied")
                    : serialFixCopyState === "failed"
                      ? t("serial.linuxPermissionCopyFailed")
                      : t("serial.linuxPermissionCopy")}
                </Button>
              </div>
              <p className="mt-2 text-ink-dim">{t("serial.linuxPermissionRestart")}</p>
              <p className="mt-1 text-ink-dim">{t("serial.linuxPermissionBusy")}</p>
            </div>
          )}
        </div>
      )}
      {logCacheError && (
        <div className="border-b border-warn/30 bg-warn/10 px-3 py-2 text-xs text-warn">
          {t("serial.logCacheUnavailable").replaceAll("{error}", logCacheError)}
        </div>
      )}

      <div
        ref={terminalPanelRef}
        className="flex min-h-0 flex-1 flex-col bg-terminal fullscreen:h-screen fullscreen:w-screen"
      >
        <div
          ref={terminalHostRef}
          role="application"
          aria-label={`${channelLabel} ${t("serial.terminalAria")}`}
          tabIndex={0}
          onClick={() => terminalHostRef.current?.focus()}
          onKeyDownCapture={handleTerminalKeyDown}
          onPasteCapture={handleTerminalPaste}
          className={`${compact ? "h-[clamp(320px,52vh,680px)]" : "h-[clamp(380px,58vh,740px)]"} terminal-host min-h-0 flex-1 bg-terminal p-2 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-inset focus-visible:ring-brand/40 [&_.xterm]:h-full`}
        />

        <div className="flex min-h-9 flex-wrap items-center gap-x-4 gap-y-1 border-t border-line/70 bg-terminal px-3 py-1.5 text-[10px] text-ink-dim transition-colors">
          <span>{automationActive ? t("serial.automationInputLocked") : source ? t("serial.directInput") : t("serial.connectToInput")}</span>
          <span className="font-mono" title={t("serial.logCacheHint")}>{t("serial.logCached")} {formatBytes(cachedLogBytes)}</span>
          <span className="ml-auto font-mono">RX {rxBytes.toLocaleString()}</span>
          <span className="font-mono">TX {txBytes.toLocaleString()}</span>
          <button type="button" disabled={!source || automationActive} onClick={() => void enqueueSerial("\u0003")} className="rounded border border-line/80 px-1.5 py-0.5 font-mono text-ink-dim transition-colors hover:bg-panel2 hover:text-ink disabled:opacity-30">Ctrl-C</button>
        </div>
      </div>
    </section>
  );
});
