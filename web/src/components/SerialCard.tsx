import { forwardRef, useEffect, useImperativeHandle, useRef, useState } from "react";
import type { ClipboardEvent as ReactClipboardEvent, KeyboardEvent as ReactKeyboardEvent } from "react";
import {
  Copy,
  Maximize2,
  Minus,
  Plug,
  Plus,
  ShieldAlert,
  Terminal as TerminalIcon,
  Trash2,
  Usb,
} from "lucide-react";
import type { Terminal as XTerm } from "@xterm/xterm";
import type { FitAddon } from "@xterm/addon-fit";
import "@xterm/xterm/css/xterm.css";
import { Button, Card } from "./ui";
import { useI18n } from "@/lib/i18n";

const BAUD_RATES = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1500000];
const CH347_VID = 0x1a86;
const BRIDGE_PORT = 8787;
const MIN_FONT_SIZE = 10;
const MAX_FONT_SIZE = 20;

type Source = "webserial" | "bridge" | null;
type LineEnding = "crlf" | "cr" | "lf";

export interface SerialAutomationHandle {
  isConnected: () => boolean;
  clear: () => void;
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

export const SerialCard = forwardRef<SerialAutomationHandle, {
  vinRoute?: string;
  onSetVin: (route: "1.8v" | "3.3v") => Promise<void>;
}>(function SerialCard({
  vinRoute,
  onSetVin,
}, automationRef) {
  const { t } = useI18n();
  const [webSerialSupported] = useState(
    typeof navigator !== "undefined" && "serial" in navigator
  );
  const [source, setSource] = useState<Source>(null);
  const [connecting, setConnecting] = useState(false);
  const [changingVoltage, setChangingVoltage] = useState(false);
  const [baud, setBaud] = useState(115200);
  const [fontSize, setFontSize] = useState(13);
  const [lineEnding, setLineEnding] = useState<LineEnding>("crlf");
  const [portInfo, setPortInfo] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [rxBytes, setRxBytes] = useState(0);
  const [txBytes, setTxBytes] = useState(0);

  const sourceRef = useRef<Source>(null);
  const portRef = useRef<SerialPort | null>(null);
  const grantedPortsRef = useRef<SerialPort[]>([]);
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

  const append = (text: string) => {
    if (termRef.current) termRef.current.write(text);
    else pendingOutputRef.current += text;
  };

  const appendReceived = (text: string) => {
    append(text);
    const receivedAtMs = performance.now();
    receiveListenersRef.current.forEach((listener) => listener(text, receivedAtMs));
  };

  useEffect(() => {
    if (!webSerialSupported) return;
    let active = true;
    void navigator.serial.getPorts().then((ports) => {
      if (active) grantedPortsRef.current = ports;
    }).catch(() => {
      if (active) grantedPortsRef.current = [];
    });
    return () => { active = false; };
  }, [webSerialSupported]);

  useImperativeHandle(automationRef, () => ({
    isConnected: () => !!sourceRef.current,
    clear: () => {
      setRxBytes(0);
      termRef.current?.clear();
      termRef.current?.write("\u001b[2J\u001b[H");
    },
    subscribe: (listener) => {
      receiveListenersRef.current.add(listener);
      return () => receiveListenersRef.current.delete(listener);
    },
  }), []);

  useEffect(() => {
    sourceRef.current = source;
    if (termRef.current) {
      termRef.current.options.cursorBlink = !!source;
      if (source) termRef.current.focus();
    }
  }, [source]);

  useEffect(() => {
    const host = terminalHostRef.current;
    if (!host) return;
    let disposed = false;
    let term: XTerm | null = null;
    let fitAddon: FitAddon | null = null;
    let resizeObserver: ResizeObserver | null = null;
    let themeObserver: MutationObserver | null = null;
    const fit = () => {
      try { fitAddon?.fit(); } catch { /* Ignore transient fullscreen layout changes. */ }
    };
    document.addEventListener("fullscreenchange", fit);

    void (async () => {
      const [{ Terminal }, { FitAddon }] = await Promise.all([
        import("@xterm/xterm"),
        import("@xterm/addon-fit"),
      ]);
      if (disposed) return;
      const activeSource = sourceRef.current;
      term = new Terminal({
        cursorBlink: !!activeSource,
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
      if (activeSource) term.focus();
      requestAnimationFrame(fit);
    })();

    return () => {
      disposed = true;
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
    if (!termRef.current) return;
    termRef.current.options.fontSize = fontSize;
    requestAnimationFrame(() => {
      try { fitAddonRef.current?.fit(); } catch { /* Ignore transient layout changes. */ }
    });
  }, [fontSize]);

  async function disconnect() {
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
    setSource(null);
    setPortInfo("");
  }

  async function connectWebSerial() {
    setError(null);
    setConnecting(true);
    try {
      const matchingPorts = grantedPortsRef.current.filter((candidate) =>
        candidate.getInfo().usbVendorId === CH347_VID
      );
      const port = matchingPorts.length === 1
        ? matchingPorts[0]
        : await navigator.serial.requestPort({ filters: [{ usbVendorId: CH347_VID }] });
      if (!grantedPortsRef.current.includes(port)) grantedPortsRef.current.push(port);
      portRef.current = port;
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
        } catch (e) {
          if (readerRef.current === reader) {
            append(`\r\n[read error: ${e instanceof Error ? e.message : e}]\r\n`);
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
      setPortInfo(`CH347F · Web Serial`);
      setConnecting(false);
      port.addEventListener("disconnect", () => {
        if (portRef.current !== port) return;
        append("\r\n[device disconnected]\r\n");
        void disconnect();
      }, { once: true });
    } catch (e) {
      await disconnect();
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  function connectBridge() {
    setError(null);
    if (wsRef.current) {
      try { wsRef.current.close(); } catch { /* Ignore a stale bridge socket. */ }
      wsRef.current = null;
    }
    setConnecting(true);
    const ws = new WebSocket(`ws://127.0.0.1:${BRIDGE_PORT}/serial`);
    wsRef.current = ws;
    ws.onopen = () => ws.send(JSON.stringify({ type: "open", baud }));
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
          setPortInfo(`${msg.path} · bridge`);
        } else if (msg.type === "error") {
          setConnecting(false);
          setError(msg.message);
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
    if (!activeSource || !data) return;
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
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  function enqueueSerial(data: string) {
    if (!sourceRef.current || !data) return;
    const generation = writeGenerationRef.current;
    const operation = writeQueueRef.current.then(() => {
      if (generation !== writeGenerationRef.current) return;
      return writeSerial(data);
    });
    writeQueueRef.current = operation.catch(() => {});
  }

  async function changeVin(next: "1.8v" | "3.3v") {
    if (next === vinRoute) return;
    const current = vinRoute === "1.8v" || vinRoute === "3.3v" ? vinRoute : t("serial.vioUnknown");
    const confirmed = window.confirm(
      t("serial.vioConfirm").replaceAll("{current}", current).replaceAll("{next}", next)
    );
    if (!confirmed) return;

    setChangingVoltage(true);
    setError(null);
    try {
      if (sourceRef.current || connecting) await disconnect();
      await onSetVin(next);
      append(`\r\n[VIO: ${current} → ${next}]\r\n`);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setChangingVoltage(false);
    }
  }

  async function copySelection() {
    const selection = termRef.current?.getSelection();
    if (!selection) return;
    try { await navigator.clipboard.writeText(selection); } catch { /* Clipboard permission may be denied. */ }
  }

  async function toggleFullscreen() {
    const panel = terminalPanelRef.current;
    if (!panel) return;
    if (document.fullscreenElement === panel) await document.exitFullscreen();
    else await panel.requestFullscreen();
  }

  function handleTerminalKeyDown(event: ReactKeyboardEvent<HTMLDivElement>) {
    const data = terminalKeyData(event, lineEnding);
    if (!data || !sourceRef.current) return;
    event.preventDefault();
    event.stopPropagation();
    enqueueSerial(data);
  }

  function handleTerminalPaste(event: ReactClipboardEvent<HTMLDivElement>) {
    const data = event.clipboardData.getData("text");
    if (!data || !sourceRef.current) return;
    event.preventDefault();
    event.stopPropagation();
    const normalized = data.replaceAll("\r\n", "\n").replaceAll("\r", "\n");
    enqueueSerial(normalized.replaceAll("\n", LINE_ENDINGS[lineEnding]));
  }

  return (
    <Card
      title={t("serial.title")}
      subtitle={t("serial.subtitle")}
      icon={TerminalIcon}
      contentClassName="flex min-h-0 flex-col"
      right={
        <div className="flex max-w-full flex-wrap items-center justify-end gap-2">
          <label
            className="inline-flex min-h-10 items-center gap-2 rounded-xl border border-warn/35 bg-warn/10 px-2.5 text-xs font-medium text-warn"
            title={t("serial.vioWarning")}
          >
            <ShieldAlert size={15} aria-hidden="true" />
            <span>VIO</span>
            <select
              aria-label={t("serial.vioLabel")}
              value={vinRoute === "1.8v" || vinRoute === "3.3v" ? vinRoute : ""}
              onChange={(event) => void changeVin(event.target.value as "1.8v" | "3.3v")}
              disabled={!vinRoute || changingVoltage}
              className="rounded-md border border-warn/30 bg-panel px-2 py-1 text-xs font-semibold text-ink outline-none focus-visible:ring-2 focus-visible:ring-warn/40 disabled:opacity-50"
            >
              <option value="" disabled>—</option>
              <option value="3.3v">3.3V</option>
              <option value="1.8v">1.8V</option>
            </select>
          </label>
          {source ? (
            <Button variant="ghost" onClick={() => void disconnect()}>{t("serial.disconnect")}</Button>
          ) : (
            <div className="flex max-w-full flex-wrap gap-2">
              {webSerialSupported && (
                <Button variant="primary" onClick={connectWebSerial} disabled={connecting}>
                  <Usb size={16} /> {t("serial.webSerial")}
                </Button>
              )}
              <Button variant="default" onClick={connectBridge} disabled={connecting}>
                <Plug size={16} /> {t("serial.bridge")}
              </Button>
            </div>
          )}
        </div>
      }
    >
      {error && (
        <div className="mb-3 rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
          {error}
        </div>
      )}

      <div
        ref={terminalPanelRef}
        className="flex min-h-0 flex-1 flex-col overflow-hidden rounded-xl border border-line/80 bg-terminal shadow-inner transition-colors fullscreen:h-screen fullscreen:w-screen fullscreen:rounded-none fullscreen:border-0"
      >
        <div className="flex min-h-11 flex-wrap items-center justify-between gap-2 border-b border-line/70 bg-terminal px-3 py-2 text-terminal-ink transition-colors">
          <div className="flex min-w-0 items-center gap-2">
            <span className={`h-2 w-2 shrink-0 rounded-full ${source ? "bg-ok shadow-[0_0_8px_rgb(var(--c-ok))]" : "bg-ink-dim/50"}`} />
            <span className="text-[11px] font-semibold uppercase tracking-[0.12em]">
              {source ? t("serial.connected") : t("serial.disconnected")}
            </span>
            {portInfo && <span className="max-w-[240px] truncate text-[10px] text-ink-dim">{portInfo}</span>}
          </div>
          <div className="flex items-center gap-1">
            <select
              value={baud}
              onChange={(event) => setBaud(Number(event.target.value))}
              disabled={!!source || connecting}
              aria-label={t("serial.baud")}
              className="h-7 rounded-md border border-line/80 bg-panel px-1.5 text-[10px] text-ink outline-none transition-colors hover:bg-panel2 disabled:opacity-60"
            >
              {BAUD_RATES.map((rate) => <option key={rate} value={rate}>{rate}</option>)}
            </select>
            <select
              value={lineEnding}
              onChange={(event) => setLineEnding(event.target.value as LineEnding)}
              aria-label={t("serial.lineEnding")}
              className="h-7 rounded-md border border-line/80 bg-panel px-1.5 text-[10px] font-medium text-ink outline-none transition-colors hover:bg-panel2"
            >
              <option value="crlf">CRLF</option>
              <option value="cr">CR</option>
              <option value="lf">LF</option>
            </select>
            <button type="button" onClick={() => setFontSize((size) => Math.max(MIN_FONT_SIZE, size - 1))} className="grid h-7 w-7 place-items-center rounded-md text-ink-dim transition-colors hover:bg-panel2 hover:text-ink" aria-label={t("serial.fontDown")}><Minus size={13} /></button>
            <button type="button" onClick={() => setFontSize((size) => Math.min(MAX_FONT_SIZE, size + 1))} className="grid h-7 w-7 place-items-center rounded-md text-ink-dim transition-colors hover:bg-panel2 hover:text-ink" aria-label={t("serial.fontUp")}><Plus size={13} /></button>
            <button type="button" onClick={() => void copySelection()} className="grid h-7 w-7 place-items-center rounded-md text-ink-dim transition-colors hover:bg-panel2 hover:text-ink" aria-label={t("serial.copy")}><Copy size={13} /></button>
            <button type="button" onClick={() => termRef.current?.clear()} className="grid h-7 w-7 place-items-center rounded-md text-ink-dim transition-colors hover:bg-panel2 hover:text-ink" aria-label={t("serial.clear")}><Trash2 size={13} /></button>
            <button type="button" onClick={() => void toggleFullscreen()} className="grid h-7 w-7 place-items-center rounded-md text-ink-dim transition-colors hover:bg-panel2 hover:text-ink" aria-label={t("serial.fullscreen")}><Maximize2 size={13} /></button>
          </div>
        </div>

        <div
          ref={terminalHostRef}
          role="application"
          aria-label={t("serial.terminalAria")}
          tabIndex={0}
          onClick={() => terminalHostRef.current?.focus()}
          onKeyDownCapture={handleTerminalKeyDown}
          onPasteCapture={handleTerminalPaste}
          className="terminal-host h-[clamp(380px,58vh,740px)] min-h-0 flex-1 bg-terminal p-2 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-inset focus-visible:ring-brand/40 [&_.xterm]:h-full"
        />

        <div className="flex min-h-9 flex-wrap items-center gap-x-4 gap-y-1 border-t border-line/70 bg-terminal px-3 py-1.5 text-[10px] text-ink-dim transition-colors">
          <span>{source ? t("serial.directInput") : t("serial.connectToInput")}</span>
          <span className="ml-auto font-mono">RX {rxBytes.toLocaleString()}</span>
          <span className="font-mono">TX {txBytes.toLocaleString()}</span>
          <button type="button" disabled={!source} onClick={() => enqueueSerial("\u0003")} className="rounded border border-line/80 px-1.5 py-0.5 font-mono text-ink-dim transition-colors hover:bg-panel2 hover:text-ink disabled:opacity-30">Ctrl-C</button>
        </div>
      </div>
    </Card>
  );
});
