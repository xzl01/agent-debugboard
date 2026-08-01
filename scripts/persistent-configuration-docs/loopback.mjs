import assert from "node:assert/strict";
import { chmod, mkdtemp, rm, writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import os from "node:os";
import path from "node:path";
import { execFile } from "node:child_process";
import { promisify } from "node:util";

const execFileAsync = promisify(execFile);
let cliBuild;

function expectedRequest(id) {
  return {
    "curl-config-get": { method: "GET", path: "/api/v1/config", body: null },
    "curl-config-save-safe": { method: "PUT", path: "/api/v1/config", body: { items: ["switch/sd"], confirm: false } },
    "curl-config-save-dangerous": { method: "PUT", path: "/api/v1/config", body: { items: ["switch/usb"], confirm: true } },
    "curl-config-save-dangerous-unconfirmed": { method: "PUT", path: "/api/v1/config", body: { items: ["switch/usb"], confirm: false } },
    "curl-config-apply-dangerous": { method: "POST", path: "/api/v1/config/apply", body: { confirm: true } },
    "curl-config-clear": { method: "DELETE", path: "/api/v1/config", body: null },
    "cli-config-show": { method: "GET", path: "/api/v1/config", body: null },
    "cli-config-save-safe": { method: "PUT", path: "/api/v1/config", body: { items: ["switch/sd"], confirm: false } },
    "cli-config-save-dangerous": { method: "PUT", path: "/api/v1/config", body: { items: ["switch/usb"], confirm: true } },
    "cli-config-apply": { method: "POST", path: "/api/v1/config/apply", body: { confirm: true } },
    "cli-config-clear": { method: "DELETE", path: "/api/v1/config", body: null },
  }[id];
}

const UNCONFIRMED_DANGEROUS_SAVE = Object.freeze({
  schema: "radxa-linkr-debugger.v1", ok: false, command: "config", action: "save",
  error: { code: "confirmation_required", message: "confirmation is required" }, dangerous_items: ["switch/usb"],
});

function responseFor(request) {
  if (request.method === "PUT" && request.path === "/api/v1/config" &&
      JSON.stringify(request.body) === JSON.stringify({ items: ["switch/usb"], confirm: false })) {
    return { status: 409, payload: UNCONFIRMED_DANGEROUS_SAVE };
  }
  const base = { schema: "radxa-linkr-debugger.v1", ok: true, command: "config" };
  if (request.method === "GET") return { status: 200, payload: {
    ...base, action: "get", backend: { available: true, reason: "ready" }, snapshot: { present: true, version: 1 }, pending: 0,
    items: [{ id: "switch/sd", kind: "switch", current: { route: "target" }, saved: null, selected: false, requires_confirm: false, apply_state: "not_saved" }],
  } };
  if (request.method === "PUT") return { status: 200, payload: {
    ...base, action: "save", saved_items: request.body.items, confirmation_items: [], snapshot: { present: true, version: 1 }, pending: 0,
  } };
  if (request.method === "POST") return { status: 200, payload: {
    ...base, action: "apply", noop: false, applied_items: ["switch/usb"], failed_item: null, pending_items: [],
  } };
  return { status: 200, payload: { ...base, action: "clear", noop: false, snapshot: { present: false, version: null }, pending: 0 } };
}

export async function startMock() {
  let requests = [];
  const server = createServer(async (request, response) => {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    const source = Buffer.concat(chunks).toString("utf8");
    const body = source ? JSON.parse(source) : null;
    const captured = { method: request.method, path: new URL(request.url, "http://127.0.0.1").pathname, body };
    requests.push(captured);
    const result = responseFor(captured);
    const payload = JSON.stringify(result.payload);
    response.writeHead(result.status, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    response.end(payload);
  });
  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  const address = server.address();
  assert.equal(typeof address, "object");
  return {
    baseUrl: `http://127.0.0.1:${address.port}`,
    requests: () => requests,
    reset: () => { requests = []; },
    close: () => new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve())),
  };
}

function loopbackCommand(command, baseUrl) {
  const rewritten = command.replaceAll("http://172.29.203.1", baseUrl);
  assert.doesNotMatch(rewritten, /https?:\/\/(?!127\.0\.0\.1(?::\d+)?(?:\/|\b))/);
  return rewritten;
}

export function classifyExample(example) {
  if (/\bcurl\b/.test(example.command)) return "curl";
  if (/(?:^|\s)radxa-linkr-debuggerctl(?:\s|$)/.test(example.command)) return "cli";
  return "non-executable-for-loopback";
}

async function runShell(command, environment) {
  return execFileAsync("sh", ["-ceu", command], { env: environment, timeout: 30_000, maxBuffer: 1024 * 1024 });
}

function assertUnconfirmedDangerousSaveResponse(example, stdout) {
  if (example.id === "curl-config-save-dangerous-unconfirmed") {
    assert.deepEqual(JSON.parse(stdout), UNCONFIRMED_DANGEROUS_SAVE, "must preserve the 409 JSON body");
  }
}

function assertConfigRequest(request) {
  assert.ok(["GET", "PUT", "POST", "DELETE"].includes(request.method), `unsupported method ${request.method}`);
  assert.ok(["/api/v1/config", "/api/v1/config/apply"].includes(request.path), `unsupported path ${request.path}`);
  assert.ok(request.body === null || (typeof request.body === "object" && !Array.isArray(request.body)), "request body must be parseable JSON");
}

export async function executeExamples(examples, kind, mock, environment = process.env) {
  const executed = [];
  const skipped = examples.filter((example) => classifyExample(example) === "non-executable-for-loopback").map(({ id }) => id);
  for (const example of examples.filter((candidate) => classifyExample(candidate) === kind)) {
    mock.reset();
    const env = kind === "curl" ? { ...environment, BOARD_URL: mock.baseUrl } : environment;
    const { stdout } = await runShell(kind === "curl" ? loopbackCommand(example.command, mock.baseUrl) : example.command, env);
    assert.equal(mock.requests().length, 1, `${example.id} must send exactly one request`);
    assertConfigRequest(mock.requests()[0]);
    const expected = expectedRequest(example.id);
    if (expected) assert.deepEqual(mock.requests(), [expected], `${example.id} did not send the frozen request`);
    assertUnconfirmedDangerousSaveResponse(example, stdout);
    executed.push(example.id);
  }
  return { executed, skipped };
}

export async function buildCli(root) {
  if (!cliBuild) {
    cliBuild = execFileAsync("cargo", ["build", "--manifest-path", "cmd-ng/Cargo.toml"], {
      cwd: root, timeout: 180_000, maxBuffer: 4 * 1024 * 1024,
    }).then(() => path.join(root, "cmd-ng/target/debug/radxa-linkr-debuggerctl"));
  }
  return cliBuild;
}

export async function withCliWrapper(cliPath, baseUrl, callback) {
  const directory = await mkdtemp(path.join(os.tmpdir(), "persistent-config-cli-"));
  try {
    const wrapper = path.join(directory, "radxa-linkr-debuggerctl");
    await writeFile(wrapper, "#!/bin/sh\nexec \"$LINKR_REAL_CLI\" --url \"$LINKR_TEST_URL\" \"$@\"\n");
    await chmod(wrapper, 0o755);
    return await callback({
      ...process.env, LINKR_REAL_CLI: cliPath, LINKR_TEST_URL: baseUrl,
      PATH: `${directory}${path.delimiter}${process.env.PATH ?? ""}`,
    });
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}
