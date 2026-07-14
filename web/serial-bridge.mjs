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

const BRIDGE_HOST = "127.0.0.1";
const BRIDGE_PORT = Number(process.env.LINKR_BRIDGE_PORT || 8787);
const BOARD_HTTP = process.env.LINKR_BOARD_URL || "http://172.29.203.1:8080";
const BOARD_WS = BOARD_HTTP.replace(/^http/, "ws");
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
    "access-control-allow-headers": "Content-Type",
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

  const upstream = http.request(target, { method: req.method, headers }, (upstreamResponse) => {
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

apiWss.on("connection", (client, request) => {
  const target = new URL(request.url || "/", BOARD_WS);
  const upstream = new WebSocket(target);
  const pending = [];

  client.on("message", (data, isBinary) => {
    if (upstream.readyState === WebSocket.OPEN) {
      upstream.send(data, { binary: isBinary });
    } else if (upstream.readyState === WebSocket.CONNECTING) {
      pending.push([data, isBinary]);
    }
  });
  upstream.on("open", () => {
    for (const [data, isBinary] of pending.splice(0)) {
      upstream.send(data, { binary: isBinary });
    }
  });
  upstream.on("message", (data, isBinary) => {
    if (client.readyState === WebSocket.OPEN) client.send(data, { binary: isBinary });
  });
  upstream.on("error", () => client.close(1011, "board websocket unavailable"));
  upstream.on("close", (code, reason) => client.close(code, reason));
  client.on("close", () => upstream.close());
});

function send(ws, obj) {
  if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
}

function openSerial(ws, baud) {
  SerialPort.list()
    .then((ports) => {
      const target = ports.find((port) =>
        (port.vendorId || "").toLowerCase() === CH347_VID
      );
      if (!target) {
        send(ws, { type: "error", message: "No CH347F (VID 1a86) serial port found" });
        return;
      }

      const serial = new SerialPort({ path: target.path, baudRate: baud });
      serial.on("data", (chunk) => send(ws, { type: "data", text: chunk.toString("utf8") }));
      serial.on("error", (error) => send(ws, { type: "error", message: error.message }));
      serial.on("close", () => send(ws, { type: "closed" }));
      serial.on("open", () => send(ws, { type: "opened", path: target.path, baud }));
      ws.__serial = serial;
    })
    .catch((error) => send(ws, { type: "error", message: `port list failed: ${error.message}` }));
}

serialWss.on("connection", (ws) => {
  let opening = false;
  let opened = false;

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
        openSerial(ws, msg.baud || 115200);
        const check = setInterval(() => {
          if (ws.__serial?.isOpen) {
            opened = true;
            clearInterval(check);
          } else if (ws.readyState !== WebSocket.OPEN) {
            clearInterval(check);
          }
        }, 50);
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
  console.log(`Forwarding board API to ${BOARD_HTTP}`);
});
