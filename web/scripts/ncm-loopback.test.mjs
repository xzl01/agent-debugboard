import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import fs from "node:fs";
import http from "node:http";
import net from "node:net";
import { PassThrough } from "node:stream";
import test from "node:test";
import { shouldUseNcmLoopback, startNcmLoopback } from "./ncm-loopback.mjs";

function mockForwarderChild(port, exitAfterReady = false) {
  const child = new EventEmitter();
  child.stdout = new PassThrough();
  child.stderr = new PassThrough();
  child.exitCode = null;
  child.signalCode = null;
  child.kill = (signal) => {
    if (child.exitCode !== null || child.signalCode !== null) return false;
    child.signalCode = signal;
    queueMicrotask(() => child.emit("exit", null, signal));
    return true;
  };
  queueMicrotask(() => {
    child.stdout.write(`${JSON.stringify({ ready: true, host: "127.0.0.1", port })}\n`);
    if (exitAfterReady) {
      setTimeout(() => {
        child.exitCode = 7;
        child.emit("exit", 7, null);
      }, 5);
    }
  });
  return child;
}

test("enables the loopback workaround only for macOS USB-NCM by default", () => {
  assert.equal(
    shouldUseNcmLoopback("http://172.29.203.1", { platform: "darwin", mode: "auto" }),
    true
  );
  assert.equal(
    shouldUseNcmLoopback("http://172.29.203.1", { platform: "linux", mode: "auto" }),
    false
  );
  assert.equal(
    shouldUseNcmLoopback("http://127.0.0.1:8787", { platform: "darwin", mode: "auto" }),
    false
  );
  assert.equal(
    shouldUseNcmLoopback("http://172.29.203.1", { platform: "darwin", mode: "off" }),
    false
  );
});

test("restarts an unexpectedly exited forwarder on the same loopback port", async () => {
  let spawnCount = 0;
  let restartCount = 0;
  const forwarder = await startNcmLoopback("http://172.29.203.1", {
    force: true,
    listenPort: 18787,
    restartDelayMs: 0,
    restartLimit: 1,
    spawnProcess: () => {
      spawnCount += 1;
      return mockForwarderChild(18787, spawnCount === 1);
    },
    onRestart: () => {
      restartCount += 1;
    },
  });
  try {
    await new Promise((resolve) => setTimeout(resolve, 20));
    assert.equal(spawnCount, 2);
    assert.equal(restartCount, 1);
    assert.equal(forwarder.target, "http://127.0.0.1:18787");
  } finally {
    await forwarder.close();
  }
  assert.equal(await forwarder.exited, null);
});

test("reports a terminal failure instead of leaving a dead proxy target", async () => {
  const forwarder = await startNcmLoopback("http://172.29.203.1", {
    force: true,
    listenPort: 18788,
    restartLimit: 0,
    spawnProcess: () => mockForwarderChild(18788, true),
  });
  const error = await forwarder.exited;
  assert(error instanceof Error);
  assert.match(error.message, /could not be recovered/);
  await forwarder.close();
});

test("forwards HTTP and raw duplex traffic over loopback", {
  skip: !fs.existsSync("/usr/bin/ruby"),
}, async () => {
  const upstream = http.createServer((request, response) => {
    response.writeHead(200, { "content-type": "application/octet-stream" });
    request.pipe(response);
  });
  await new Promise((resolve) => upstream.listen(0, "127.0.0.1", resolve));
  const address = upstream.address();
  assert(address && typeof address === "object");

  let forwarder;
  try {
    forwarder = await startNcmLoopback(`http://127.0.0.1:${address.port}`, {
      force: true,
    });
    const payload = Buffer.from([0, 1, 2, 127, 128, 254, 255]);
    const response = await fetch(`${forwarder.target}/echo`, {
      method: "POST",
      body: payload,
    });
    assert.equal(response.status, 200);
    assert.deepEqual(Buffer.from(await response.arrayBuffer()), payload);
  } finally {
    await forwarder?.close();
    await new Promise((resolve) => upstream.close(resolve));
  }

  const echoServer = net.createServer((socket) => socket.pipe(socket));
  await new Promise((resolve) => echoServer.listen(0, "127.0.0.1", resolve));
  const echoAddress = echoServer.address();
  assert(echoAddress && typeof echoAddress === "object");

  let rawForwarder;
  try {
    rawForwarder = await startNcmLoopback(`http://127.0.0.1:${echoAddress.port}`, {
      force: true,
    });
    const payload = Buffer.from([0, 1, 2, 127, 128, 254, 255]);
    const target = new URL(rawForwarder.target);
    const echoed = await new Promise((resolve, reject) => {
      const socket = net.connect(Number(target.port), target.hostname);
      const chunks = [];
      socket.on("connect", () => socket.write(payload));
      socket.on("data", (chunk) => {
        chunks.push(chunk);
        if (Buffer.concat(chunks).length >= payload.length) socket.end();
      });
      socket.on("end", () => resolve(Buffer.concat(chunks)));
      socket.on("error", reject);
    });
    assert.deepEqual(echoed, payload);
  } finally {
    await rawForwarder?.close();
    await new Promise((resolve) => echoServer.close(resolve));
  }
});
