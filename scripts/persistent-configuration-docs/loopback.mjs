import assert from "node:assert/strict";
import { createServer } from "node:http";
import path from "node:path";
import { execFile } from "node:child_process";
import { promisify } from "node:util";

const execFileAsync = promisify(execFile);
let cliBuild;

function expectedRequest(example) {
  const item = /gpio\/GP5/.test(example.command) ? "gpio/GP5" : "power/5v_out";
  return {
    "curl-config-get": { method: "GET", path: "/api/v1/config", body: null },
    "curl-config-save-safe": { method: "PUT", path: "/api/v1/config", body: { items: ["switch/sd"], confirm: false } },
    "curl-config-save-dangerous": { method: "PUT", path: "/api/v1/config", body: { items: [item], confirm: true } },
    "curl-config-save-dangerous-unconfirmed": { method: "PUT", path: "/api/v1/config", body: { items: [item], confirm: false } },
    "curl-config-clear": { method: "DELETE", path: "/api/v1/config", body: null },
    "cli-config-show": { method: "GET", path: "/api/v1/config", body: null },
    "cli-config-save-safe": { method: "PUT", path: "/api/v1/config", body: { items: ["switch/sd"], confirm: false } },
    "cli-config-save-dangerous": { method: "PUT", path: "/api/v1/config", body: { items: [item], confirm: true } },
    "cli-config-clear": { method: "DELETE", path: "/api/v1/config", body: null },
  }[example.id];
}

const UNCONFIRMED_DANGEROUS_SAVE = Object.freeze({
  schema: "radxa-linkr-debugger.v1", ok: false, command: "config", action: "save",
  error: { code: "confirmation_required", message: "confirmation is required" }, dangerous_items: ["power/5v_out"],
});

function responseFor(request) {
  if (request.method === "PUT" && request.path === "/api/v1/config" && request.body?.confirm === false &&
      request.body.items?.some((item) => ["power/5v_out", "gpio/GP5"].includes(item))) {
    return { status: 409, payload: { ...UNCONFIRMED_DANGEROUS_SAVE, dangerous_items: request.body.items } };
  }
  const base = { schema: "radxa-linkr-debugger.v1", ok: true, command: "config" };
  if (request.method === "GET") return { status: 200, payload: {
    ...base, action: "get", backend: { available: true, reason: "ready" }, snapshot: { present: true, version: 1 }, pending: 0,
    items: [
      { id: "switch/sd", kind: "switch", current: { route: "target" }, saved: null, selected: false, requires_confirm: false, apply_state: "not_saved" },
      { id: "power/5v_out", kind: "power", current: { state: "on" }, saved: null, selected: false, requires_confirm: true, apply_state: "not_saved" },
    ],
  } };
  if (request.method === "PUT") return { status: 200, payload: {
    ...base, action: "save", saved_items: request.body.items, confirmation_items: [], applied_items: request.body.items, snapshot: { present: true, version: 1 }, pending: 0,
  } };
  return { status: 200, payload: { ...base, action: "clear", noop: false, snapshot: { present: false, version: null }, pending: 0 } };
}

export async function startMock() {
  let requests = [], connections = 0;
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
  server.on("connection", () => { connections++; });
  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  const address = server.address();
  assert.equal(typeof address, "object");
  return {
    baseUrl: `http://127.0.0.1:${address.port}`,
    requests: () => requests,
    connections: () => connections,
    reset: () => { requests = []; },
    close: () => new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve())),
  };
}

// This is deliberately a tiny argv grammar, not a shell interpreter. Only
// double-quoted BOARD_URL expands; all other substitutions/operators fail.
function tokenize(command, baseUrl) {
  const words = [];
  let word = "", quote = "", started = false;
  for (let i = 0; i < command.length; i++) {
    const c = command[i];
    if (c === "\\" && command[i + 1] === "\n" && quote !== "'") { i++; continue; }
    if (quote === "'") {
      if (c === "'") quote = ""; else word += c;
    } else if (c === quote && quote) {
      quote = "";
    } else if (c === '"' || (c === "'" && !quote)) {
      quote = c; started = true;
    } else if (quote === '"' && c === "$" && command.startsWith("$BOARD_URL", i)) {
      assert.ok(!/[A-Za-z0-9_]/.test(command[i + 10] ?? ""), "unsupported variable");
      word += baseUrl; i += 9;
    } else if (quote === '"' && c === "\\") {
      const next = command[++i];
      assert.ok(next === '"' || next === "\\", "unsupported quoted escape");
      word += next;
    } else if (quote === '"') {
      assert.ok(c !== "$" && c !== "`" && c !== "\n" && c !== "\r", "unsupported substitution");
      word += c;
    } else if (c === " " || c === "\t") {
      if (started) { words.push(word); word = ""; started = false; }
    } else {
      assert.match(c, /^[A-Za-z0-9_./:-]$/, "only one literal command is allowed");
      word += c; started = true;
    }
  }
  assert.equal(quote, "", "unclosed quote");
  if (started) words.push(word);
  return words;
}

function parseExample(example, kind, baseUrl) {
  const base = new URL(baseUrl);
  assert.ok(base.protocol === "http:" && base.hostname === "127.0.0.1" && base.port && baseUrl === base.origin, "mock must be an explicit loopback origin");
  const command = example.command.replaceAll("\r\n", "\n").trim();
  const expectedExit = example.id === "curl-config-save-dangerous-unconfirmed" ? 22 : 0;
  const argv = tokenize(command, baseUrl);
  if (kind === "curl") {
    assert.deepEqual(argv.slice(0, 2), ["curl", "-q"], "curl must disable curlrc as its first argument");
    let target, method = "GET", data, header;
    const seen = new Set();
    for (let i = 2; i < argv.length; i++) {
      const arg = argv[i];
      if (["-fsS", "-sS", "--fail-with-body", "-X", "-H", "--data"].includes(arg)) {
        assert.ok(!seen.has(arg), `duplicate option ${arg}`); seen.add(arg);
        if (arg === "-X") method = argv[++i];
        if (arg === "-H") header = argv[++i];
        if (arg === "--data") data = JSON.parse(argv[++i]);
      } else {
        assert.equal(arg, `${baseUrl}/api/v1/config`, "only the designated mock config URL is allowed");
        assert.equal(target, undefined, "exactly one URL is allowed"); target = arg;
      }
    }
    assert.ok(target && ["GET", "PUT", "DELETE"].includes(method), "unsupported request");
    assert.ok(seen.has("-fsS") || (seen.has("-sS") && seen.has("--fail-with-body")), "missing documented curl error flags");
    assert.ok(!(seen.has("-fsS") && seen.has("--fail-with-body")), "conflicting error flags");
    assert.ok(header === undefined || header === "Content-Type: application/json", "unsupported header");
    assert.equal(data !== undefined, method === "PUT", "PUT alone requires an explicit JSON body");
    if (method === "PUT") assert.equal(header, "Content-Type: application/json");
    if (expectedExit === 22) assert.ok(seen.has("--fail-with-body"), "must preserve the error body");
  } else {
    assert.equal(argv[0], "radxa-linkr-debuggerctl");
    let i = 1, target, json = false;
    while (["--url", "--json"].includes(argv[i])) {
      if (argv[i++] === "--url") { assert.equal(target, undefined); target = argv[i++]; }
      else { assert.equal(json, false); json = true; }
    }
    assert.ok(target === baseUrl && json && argv[i++] === "config", "CLI requires explicit mock URL and JSON config mode");
    const action = argv[i++], rest = argv.slice(i);
    assert.ok(["show", "save", "clear"].includes(action), "unsupported CLI action");
    if (action === "save") {
      assert.ok(rest.filter((arg) => arg === "--confirm").length <= 1);
      const items = rest.filter((arg) => arg !== "--confirm");
      assert.ok(items.length === 1 && ["switch/sd", "power/5v_out", "gpio/GP5"].includes(items[0]), "unsupported save arguments");
    } else assert.deepEqual(rest, [], "extra CLI arguments");
  }
  return { argv, expectedExit };
}

export function classifyExample(example) {
  const executable = /\b(curl|radxa-linkr-debuggerctl)\b/.exec(example.command)?.[1];
  return executable === "curl" ? "curl" : executable ? "cli" : "non-executable-for-loopback";
}

function assertUnconfirmedDangerousSaveResponse(example, stdout) {
  if (example.id === "curl-config-save-dangerous-unconfirmed") {
    assert.deepEqual(JSON.parse(stdout), { ...UNCONFIRMED_DANGEROUS_SAVE, dangerous_items: expectedRequest(example).body.items }, "must preserve the 409 JSON body");
  }
}

function assertConfigRequest(request) {
  assert.ok(["GET", "PUT", "DELETE"].includes(request.method), `unsupported method ${request.method}`);
  assert.ok(request.path === "/api/v1/config", `unsupported path ${request.path}`);
  assert.ok(request.body === null || (typeof request.body === "object" && !Array.isArray(request.body)), "request body must be parseable JSON");
}

export async function executeExamples(examples, kind, mock, environment = process.env) {
  const executed = [];
  const skipped = examples.filter((example) => classifyExample(example) === "non-executable-for-loopback").map(({ id }) => id);
  for (const example of examples.filter((candidate) => classifyExample(candidate) === kind)) {
    mock.reset();
    const { argv, expectedExit } = parseExample(example, kind, mock.baseUrl);
    const env = Object.fromEntries(Object.entries(environment).filter(([key]) => !["http_proxy", "https_proxy", "all_proxy", "no_proxy"].includes(key.toLowerCase())));
    Object.assign(env, { BOARD_URL: mock.baseUrl, NO_PROXY: "*", no_proxy: "*", LINKR_SKIP_DESKTOP: "1" });
    const file = kind === "curl" ? "curl" : env.LINKR_REAL_CLI;
    assert.ok(kind === "curl" || (file && path.isAbsolute(file)), "CLI wrapper must supply an absolute executable path");
    let result;
    try { result = { ...await execFileAsync(file, argv.slice(1), { env, timeout: 30_000, maxBuffer: 1024 * 1024 }), code: 0 }; }
    catch (error) { if (error.code !== expectedExit) throw error; result = error; }
    assert.equal(result.code, expectedExit, "documented curl exit status");
    const { stdout } = result;
    assert.equal(mock.requests().length, 1, `${example.id} must send exactly one request`);
    assertConfigRequest(mock.requests()[0]);
    const expected = expectedRequest(example);
    if (expected) assert.deepEqual(mock.requests(), [expected], `${example.id} did not send the frozen request`);
    assertUnconfirmedDangerousSaveResponse(example, stdout);
    executed.push(example.id);
  }
  return { executed, skipped };
}

export async function buildCli(root) {
  if (!cliBuild) {
    cliBuild = execFileAsync("cargo", ["build", "--locked", "--offline", "--manifest-path", "cmd-ng/Cargo.toml"], {
      cwd: root, timeout: 180_000, maxBuffer: 4 * 1024 * 1024,
    }).then(() => path.join(root, "cmd-ng/target/debug/radxa-linkr-debuggerctl"));
  }
  return cliBuild;
}

export async function withCliWrapper(cliPath, baseUrl, callback) {
  return callback({ ...process.env, LINKR_REAL_CLI: cliPath, BOARD_URL: baseUrl, LINKR_SKIP_DESKTOP: "1" });
}
