// Local device gateway for the GitHub Pages UI.
//
// It exposes the board HTTP/WebSocket API on loopback with the CORS and Private
// Network Access headers required by a public HTTPS page. The same process also
// keeps the optional CH347F target-UART bridge available at /serial.
//
//   npm run device-bridge
//
import http from "node:http";
import WebSocket, { WebSocketServer } from "ws";
import { SerialPort } from "serialport";
import { startNcmLoopback } from "./scripts/ncm-loopback.mjs";

const BRIDGE_HOST = "127.0.0.1";
const BRIDGE_PORT = Number(process.env.LINKR_BRIDGE_PORT || 8787);
const configuredBoardHttp = process.env.LINKR_BOARD_URL || "http://172.29.203.1:8080";
const boardForwarder = await startNcmLoopback(configuredBoardHttp);
const BOARD_HTTP = boardForwarder.target;
const BOARD_WS = BOARD_HTTP.replace(/^http/, "ws");

function durationFromEnv(name, fallback) {
  const value = Number(process.env[name]);
  return Number.isFinite(value) && value >= 1_000 ? value : fallback;
}

const WS_HEARTBEAT_MS = durationFromEnv("LINKR_WS_HEARTBEAT_MS", 15_000);
const WS_CONNECT_TIMEOUT_MS = durationFromEnv("LINKR_WS_CONNECT_TIMEOUT_MS", 10_000);
const BOARD_HTTP_AGENT = new http.Agent({
  keepAlive: true,
  maxSockets: 1,
  maxFreeSockets: 1,
});
const CH347_VID = "1a86";
const TRUSTED_ORIGINS = new Set(
  (process.env.LINKR_TRUSTED_ORIGINS || "https://xzl01.github.io")
    .split(",")
    .map((origin) => origin.trim())
    .filter(Boolean)
);

function isAllowedOrigin(origin) {
  if (!origin) return true;
  if (TRUSTED_ORIGINS.has(origin)) return true;
  try {
    const url = new URL(origin);
    return url.protocol === "http:" &&
      (url.hostname === "127.0.0.1" || url.hostname === "localhost");
  } catch {
    return false;
  }
}

function corsHeaders(req) {
  return {
    "access-control-allow-origin": req.headers.origin || "*",
    "access-control-allow-methods": "GET, POST, PUT, DELETE, OPTIONS",
    "access-control-allow-headers": "Content-Type, X-Linkr-Ota-Size, X-Linkr-Ota-Sha256",
    "access-control-allow-private-network": "true",
    vary: "Origin",
  };
}

const server = http.createServer((req, res) => {
  if (!isAllowedOrigin(req.headers.origin)) {
    res.writeHead(403, { "content-type": "application/json; charset=utf-8" });
    res.end(JSON.stringify({ ok: false, error: { message: "origin is not allowed" } }));
    return;
  }

  if (req.method === "OPTIONS") {
    res.writeHead(204, corsHeaders(req));
    res.end();
    return;
  }

  if (!req.url?.startsWith("/api/")) {
    res.writeHead(404, {
      ...corsHeaders(req),
      "content-type": "application/json; charset=utf-8",
    });
    res.end(JSON.stringify({ ok: false, error: { message: "unknown gateway path" } }));
    return;
  }

  const target = new URL(req.url, BOARD_HTTP);
  const headers = { ...req.headers, host: target.host };
  delete headers.origin;
  headers.connection = "keep-alive";

  // The firmware HTTP service intentionally has a small client pool and keeps
  // clients alive. Reuse one upstream socket instead of consuming a new slot
  // for every browser poll.
  const upstream = http.request(target, { method: req.method, headers, agent: BOARD_HTTP_AGENT }, (upstreamResponse) => {
    res.writeHead(upstreamResponse.statusCode || 502, {
      ...upstreamResponse.headers,
      ...corsHeaders(req),
    });
    upstreamResponse.pipe(res);
  });
  upstream.on("error", (error) => {
    if (res.headersSent) {
      res.destroy(error);
      return;
    }
    res.writeHead(502, {
      ...corsHeaders(req),
      "content-type": "application/json; charset=utf-8",
    });
    res.end(JSON.stringify({
      ok: false,
      error: {
        code: "board_unreachable",
        message: `Cannot reach ${BOARD_HTTP}: ${error.message}`,
      },
    }));
  });
  req.pipe(upstream);
});

const apiWss = new WebSocketServer({ noServer: true });
const serialWss = new WebSocketServer({ noServer: true });
const upstreamSockets = new Set();

function trackHeartbeat(ws) {
  ws.__heartbeatAlive = true;
  ws.on("pong", () => {
    ws.__heartbeatAlive = true;
  });
}

function heartbeat(ws) {
  if (ws.readyState !== WebSocket.OPEN) return;
  if (ws.__heartbeatAlive === false) {
    ws.terminate();
    return;
  }
  ws.__heartbeatAlive = false;
  ws.ping();
}

function activityHeartbeat(ws) {
  if (ws.readyState !== WebSocket.OPEN) return;
  if (ws.__heartbeatAlive === false) {
    ws.terminate();
    return;
  }
  ws.__heartbeatAlive = false;
}

function forwardedCloseCode(code) {
  if (code >= 3_000 && code <= 4_999) return code;
  if ([1_000, 1_001, 1_002, 1_003, 1_007, 1_008, 1_009, 1_010, 1_011, 1_012, 1_013, 1_014].includes(code)) {
    return code;
  }
  return 1_011;
}

const heartbeatTimer = setInterval(() => {
  for (const client of apiWss.clients) heartbeat(client);
  for (const client of serialWss.clients) heartbeat(client);
  // Zephyr's server does not currently answer protocol-level ping frames. A
  // subscribed live session emits data continuously, so use received frames as
  // the upstream liveness signal while retaining ping/pong for browser peers.
  for (const upstream of upstreamSockets) activityHeartbeat(upstream);
}, WS_HEARTBEAT_MS);
heartbeatTimer.unref();

apiWss.on("connection", (client, request) => {
  const target = new URL(request.url || "/", BOARD_WS);
  const upstream = new WebSocket(target);
  const pending = [];

  trackHeartbeat(client);
  upstream.__heartbeatAlive = true;
  upstreamSockets.add(upstream);
  const connectTimer = setTimeout(() => {
    if (upstream.readyState !== WebSocket.CONNECTING) return;
    upstream.terminate();
    client.close(1011, "board websocket connection timed out");
  }, WS_CONNECT_TIMEOUT_MS);
  connectTimer.unref();

  client.on("message", (data, isBinary) => {
    if (upstream.readyState === WebSocket.OPEN) {
      upstream.send(data, { binary: isBinary });
    } else if (upstream.readyState === WebSocket.CONNECTING) {
      pending.push([data, isBinary]);
    }
  });
  upstream.on("open", () => {
    clearTimeout(connectTimer);
    upstream.__heartbeatAlive = true;
    for (const [data, isBinary] of pending.splice(0)) {
      upstream.send(data, { binary: isBinary });
    }
  });
  upstream.on("message", (data, isBinary) => {
    upstream.__heartbeatAlive = true;
    if (client.readyState === WebSocket.OPEN) client.send(data, { binary: isBinary });
  });
  upstream.on("error", () => {
    clearTimeout(connectTimer);
    client.close(1011, "board websocket unavailable");
  });
  upstream.on("close", (code, reason) => {
    clearTimeout(connectTimer);
    upstreamSockets.delete(upstream);
    if (client.readyState === WebSocket.OPEN) {
      const safeCode = forwardedCloseCode(code);
      client.close(safeCode, safeCode === code ? reason : "board websocket disconnected");
    }
  });
  client.on("close", () => {
    clearTimeout(connectTimer);
    upstreamSockets.delete(upstream);
    if (upstream.readyState === WebSocket.CONNECTING || upstream.readyState === WebSocket.OPEN) {
      upstream.terminate();
    }
  });
});

function send(ws, obj) {
  if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
}

function selectSerialPort(ports, channel) {
  const sorted = [...ports].sort((left, right) => left.path.localeCompare(right.path));
  const suffix = channel === "uart1" ? /D3$/i : /D1$/i;
  return sorted.find((port) => suffix.test(port.path)) ??
    sorted[channel === "uart1" ? 1 : 0];
}

async function openSerial(ws, baud, channel) {
  const ports = await SerialPort.list();
  const candidates = ports.filter((port) =>
    (port.vendorId || "").toLowerCase() === CH347_VID
  );
  const target = selectSerialPort(candidates, channel);
  if (!target) {
    throw new Error(`No CH347F ${channel.toUpperCase()} serial port found`);
  }

  const serial = new SerialPort({ path: target.path, baudRate: baud, autoOpen: false });
  ws.__serial = serial;
  serial.on("data", (chunk) => send(ws, { type: "data", text: chunk.toString("utf8") }));
  serial.on("error", (error) => send(ws, { type: "error", message: error.message }));
  serial.on("close", () => send(ws, { type: "closed" }));

  try {
    await new Promise((resolve, reject) => {
      serial.open((error) => error ? reject(error) : resolve());
    });
  } catch (error) {
    if (ws.__serial === serial) ws.__serial = undefined;
    throw error;
  }

  if (ws.readyState !== WebSocket.OPEN) {
    serial.close(() => {});
    if (ws.__serial === serial) ws.__serial = undefined;
    throw new Error("Serial client disconnected while opening the port");
  }

  send(ws, { type: "opened", channel, path: target.path, baud });
  return serial;
}

serialWss.on("connection", (ws) => {
  let opening = false;
  let opened = false;

  trackHeartbeat(ws);

  ws.on("message", (data) => {
    if (!opened && !opening) {
      let msg;
      try {
        msg = JSON.parse(data.toString());
      } catch {
        return;
      }
      if (msg.type === "open") {
        opening = true;
        const channel = msg.channel === "uart1" ? "uart1" : "uart0";
        void openSerial(ws, msg.baud || 115200, channel)
          .then((serial) => {
            opened = true;
            serial.once("close", () => {
              if (ws.__serial === serial) ws.__serial = undefined;
              opened = false;
            });
          })
          .catch((error) => {
            send(ws, {
              type: "error",
              message: `serial open failed: ${error instanceof Error ? error.message : String(error)}`,
            });
          })
          .finally(() => {
            opening = false;
          });
      }
      return;
    }
    if (ws.__serial?.writable) ws.__serial.write(data.toString());
  });

  ws.on("close", () => {
    try {
      ws.__serial?.close();
    } catch {
      // Ignore a port that was already closed by the OS.
    }
  });
});

server.on("upgrade", (request, socket, head) => {
  if (!isAllowedOrigin(request.headers.origin)) {
    socket.write("HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n");
    socket.destroy();
    return;
  }
  const target = request.url?.startsWith("/api/") ? apiWss : serialWss;
  target.handleUpgrade(request, socket, head, (ws) => {
    target.emit("connection", ws, request);
  });
});

server.listen(BRIDGE_PORT, BRIDGE_HOST, () => {
  console.log(`Linkr device gateway listening on http://${BRIDGE_HOST}:${BRIDGE_PORT}`);
  if (boardForwarder.forwarded) {
    console.log(`USB-NCM loopback: ${BOARD_HTTP} -> ${boardForwarder.upstream}`);
  }
  console.log(`Forwarding board API to ${BOARD_HTTP}`);
});

server.on("error", (error) => {
  console.error(`Linkr device gateway failed: ${error.message}`);
  void boardForwarder.close().finally(() => process.exit(1));
});

server.on("close", () => {
  clearInterval(heartbeatTimer);
  void boardForwarder.close();
});

let shuttingDown = false;
function shutdown(exitCode = 0) {
  if (shuttingDown) return;
  shuttingDown = true;
  for (const client of apiWss.clients) client.terminate();
  for (const client of serialWss.clients) client.terminate();
  for (const upstream of upstreamSockets) upstream.terminate();
  server.close(() => {
    void boardForwarder.close().finally(() => process.exit(exitCode));
  });
  setTimeout(() => process.exit(exitCode || 1), 2_000).unref();
}

void boardForwarder.exited.then((error) => {
  if (!error || shuttingDown) return;
  console.error(error.message);
  shutdown(1);
});

process.once("SIGINT", () => shutdown(0));
process.once("SIGTERM", () => shutdown(0));
