import assert from "node:assert/strict";
import { chmod, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { checkPersistentConfigurationDocs, formatFailures } from "../check-persistent-configuration-docs.mjs";
import { fixtureDocuments, withFixture } from "./fixtures.mjs";
import { buildCli, executeExamples, startMock, withCliWrapper } from "./loopback.mjs";
import { ITEM_COUNT, MAX_ENCODED_BYTES, SNAPSHOT_CONTRACT } from "./snapshot-contract.mjs";

export async function executeRepositoryExamples(examples, root) {
  const mock = await startMock();
  try {
    const curl = await executeExamples(examples, "curl", mock);
    assert.ok(curl.executed.includes("curl-config-save-dangerous-unconfirmed"));
    const cliPath = await buildCli(root);
    const cli = await withCliWrapper(cliPath, mock.baseUrl, (env) => executeExamples(examples, "cli", mock, env));
    assert.ok(cli.executed.includes("cli-config-save-dangerous"));
  } finally { await mock.close(); }
}

export async function registerCurrentContractTests(suite, root) {
  await suite.test("normalized snapshot facts match firmware rather than only synthetic fixtures", async () => {
    const source = async (name) => readFile(path.join(root, "apps/radxa_linkr_debugger", name), "utf8");
    const codec = await source("src/linkr_debugger_config_codec.h");
    for (const [name, expected] of Object.entries({
      VERSION: SNAPSHOT_CONTRACT.version, HEADER_SIZE: SNAPSHOT_CONTRACT.headerBytes,
      ENTRY_SIZE: SNAPSHOT_CONTRACT.entryBytes, MAX_ENTRIES: ITEM_COUNT, MAX_ENCODED_SIZE: MAX_ENCODED_BYTES,
    })) {
      assert.match(codec, new RegExp(`^#define LINKR_DEBUGGER_CONFIG_${name} ${expected}U$`, "m"));
    }
    assert.match(await source("boards/rpi_pico2_rp2350a_m33_mcuboot.conf"),
      new RegExp(`^CONFIG_SETTINGS_NVS_SECTOR_COUNT=${SNAPSHOT_CONTRACT.nvsSectors}$`, "m"));
    const replay = await source("src/linkr_debugger_config_replay.c");
    assert.match(replay, /POWER_5V_USB_HUB_ID[\s\S]*?POWER_5V_OTG_ID[\s\S]*?POWER_12V_OUT_ID/);
  });

  for (const [name, before, after] of [
    ["old 84-byte bound", "88 bytes", "84 bytes"],
    ["old 18-item catalog", "19 items: 6 power outputs", "18 items: 5 power outputs"],
    ["old NVS sector allocation", "CONFIG_SETTINGS_NVS_SECTOR_COUNT=32", "CONFIG_SETTINGS_NVS_SECTOR_COUNT=8"],
    ["missing OTG replay", "6. `power 5v_otg`", "6. omitted"],
    ["old output replay stage", "10. Saved GPIO outputs", "9. Saved GPIO outputs"],
    ["unquoted BOARD_URL", '"$BOARD_URL/api/v1/config"', "$BOARD_URL/api/v1/config"],
    ["old fixed board address", '"$BOARD_URL/api/v1/config"', "http://172.29.203.1/api/v1/config"],
  ]) {
    await suite.test(`rejects ${name}`, async () => {
      const docs = fixtureDocuments();
      const surface = "docs/reference/persistent-configuration.md";
      assert.ok(docs[surface].includes(before));
      docs[surface] = docs[surface].replace(before, after);
      await withFixture(async (fixture) => {
        const result = await checkPersistentConfigurationDocs(fixture);
        assert.equal(result.ok, false, `${name}: ${formatFailures(result.failures)}`);
      }, docs);
    });
  }

  await suite.test("loopback rejects hardcoded URLs and BOARD_URL replacement before sending requests", async () => {
    const mock = await startMock();
    try {
      for (const command of [
        "curl -fsS http://172.29.203.1/api/v1/config",
        "curl -fsS https://example.com/api/v1/config",
        'BOARD_URL=http://172.29.203.1; curl -fsS "$BOARD_URL/api/v1/config"',
        'curl -fsS $BOARD_URL/api/v1/config',
      ]) {
        await assert.rejects(executeExamples([{ id: "unsafe", command }], "curl", mock));
        assert.deepEqual(mock.requests(), []);
      }
    } finally { await mock.close(); }
  });

  await suite.test("loopback argv grammar rejects extra URLs, shell commands and curl connection redirection", async () => {
    const mock = await startMock(), outside = await startMock();
    const get = 'curl -q -fsS "$BOARD_URL/api/v1/config"';
    const other = `${new URL(outside.baseUrl).host}/outside-mock`;
    try {
      for (const command of [
        `${get} ${other}`, `curl -q -fsS ${other}`, `curl -q -fsS "${outside.baseUrl}/outside-mock"`,
        `${get}\ncurl -q -fsS ${other}`, `${get}; curl -q -fsS ${other}`,
        `${get} && curl -q -fsS ${other}`, `${get} | curl -q -fsS ${other}`, `${get} > /dev/null`,
        `${get} $(curl -q -fsS ${other})`, `${get} \`curl -q -fsS ${other}\``,
        `BOARD_URL=${outside.baseUrl} ${get}`, 'curl -fsS "$BOARD_URL/api/v1/config"',
        'curl -fsS -q "$BOARD_URL/api/v1/config"', 'curl -q -fsS "$BOARD_URL/api/v1/config?extra=1"',
        'curl -q -fsS "$BOARD_URL/api/v1/config" "$BOARD_URL/api/v1/config"',
        ...["--location", "-L", "--next", "--config /dev/null", "-K /dev/null", "--output /dev/null",
          `--proxy ${other}`, `-x ${other}`, `--preproxy ${other}`, '--noproxy ""',
          `--connect-to "${new URL(mock.baseUrl).host}:${new URL(outside.baseUrl).host}"`,
          `--resolve "127.0.0.1:${new URL(mock.baseUrl).port}:127.0.0.2"`,
          `--url ${other}`, `--url=${other}`, '--data "@/dev/null"'].map((option) => `${get} ${option}`),
      ]) {
        await assert.rejects(executeExamples([{ id: "unsafe", command }], "curl", mock));
        assert.deepEqual(mock.requests(), [], command);
        assert.equal(outside.connections(), 0, command);
      }
      const wrapped = `set +e\nresponse="$(${get})"\ncurl_status=$?\nset -e\n[ "$curl_status" -eq 22 ]\nprintf '%s\\n' "$response"`;
      await assert.rejects(executeExamples([{ id: "curl-config-save-dangerous-unconfirmed", command: wrapped }], "curl", mock));
      assert.deepEqual(mock.requests(), []);
      const cli = 'radxa-linkr-debuggerctl --url "$BOARD_URL" --json config show';
      for (const command of [
        `${cli}\ncurl -q -fsS ${other}`, `${cli}; echo unsafe`, `${cli} --desktop`,
        `${cli} --url "${outside.baseUrl}"`, `${cli} $(echo unsafe)`,
        `radxa-linkr-debuggerctl --url "${outside.baseUrl}" --json config show`,
      ]) {
        await assert.rejects(executeExamples([{ id: "unsafe-cli", command }], "cli", mock));
        assert.deepEqual(mock.requests(), [], command);
        assert.equal(outside.connections(), 0, command);
      }
      assert.deepEqual(outside.requests(), []);
    } finally { await mock.close(); await outside.close(); }
  });

  await suite.test("curl -q isolates a malicious curlrc and inherited proxies from a second sentinel", async () => {
    const mock = await startMock(), outside = await startMock();
    const directory = await mkdtemp(path.join(os.tmpdir(), "persistent-curlrc-"));
    try {
      await writeFile(path.join(directory, ".curlrc"), `url = "${new URL(outside.baseUrl).host}/outside-mock"\n`);
      await executeExamples([{ id: "curl-config-get", command: 'curl -q -fsS "$BOARD_URL/api/v1/config"' }], "curl", mock, {
        ...process.env, HOME: directory, CURL_HOME: directory, XDG_CONFIG_HOME: directory,
        HTTP_PROXY: outside.baseUrl, http_proxy: outside.baseUrl, HTTPS_PROXY: outside.baseUrl,
        ALL_PROXY: outside.baseUrl, all_proxy: outside.baseUrl, NO_PROXY: "", no_proxy: "",
      });
      assert.equal(mock.requests().length, 1);
      assert.equal(outside.connections(), 0);
      assert.deepEqual(outside.requests(), []);
    } finally { await mock.close(); await outside.close(); await rm(directory, { recursive: true, force: true }); }
  });

  await suite.test("CLI forces desktop skip=1 and never launches fake tray or probes another Host", async () => {
    const mock = await startMock(), outside = await startMock();
    const directory = await mkdtemp(path.join(os.tmpdir(), "persistent-cli-isolation-"));
    const marker = path.join(directory, "tray-started"), fakeTray = path.join(directory, "linkr-tray");
    const recorded = path.join(directory, "environment.json"), fakeCli = path.join(directory, "fake-cli");
    const previousSkip = process.env.LINKR_SKIP_DESKTOP;
    try {
      await writeFile(fakeTray, `#!${process.execPath}\nrequire("node:fs").writeFileSync(${JSON.stringify(marker)}, "started");\n`);
      await chmod(fakeTray, 0o755);
      await writeFile(fakeCli, `#!${process.execPath}\nconst fs = require("node:fs"), http = require("node:http");
fs.writeFileSync(${JSON.stringify(recorded)}, JSON.stringify(process.env));
const url = process.argv[process.argv.indexOf("--url") + 1];
http.get(url + "/api/v1/config", response => response.pipe(process.stdout));\n`);
      await chmod(fakeCli, 0o755);
      const command = 'radxa-linkr-debuggerctl --url "$BOARD_URL" --json config show';
      process.env.LINKR_SKIP_DESKTOP = "0";
      const run = (cliPath) => withCliWrapper(cliPath, mock.baseUrl, async (environment) => {
        assert.equal(environment.LINKR_SKIP_DESKTOP, "1", "wrapper must override inherited zero");
        await executeExamples([{ id: "cli-config-show", command }], "cli", mock, {
          ...environment, LINKR_SKIP_DESKTOP: "0", LINKR_TRAY_BIN: fakeTray,
          LINKR_BRIDGE_PORT: new URL(outside.baseUrl).port, DISPLAY: ":sentinel",
          HOME: directory, XDG_DATA_HOME: directory, HTTP_PROXY: outside.baseUrl,
          http_proxy: outside.baseUrl, HTTPS_PROXY: outside.baseUrl, ALL_PROXY: outside.baseUrl,
        });
      });
      await run(fakeCli);
      const environment = JSON.parse(await readFile(recorded, "utf8"));
      assert.equal(environment.LINKR_SKIP_DESKTOP, "1", "exec environment must also override zero");
      for (const key of ["HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "ALL_PROXY"]) assert.equal(environment[key], undefined);
      await run(await buildCli(root));
      await assert.rejects(readFile(marker), { code: "ENOENT" });
      assert.equal(outside.connections(), 0);
      assert.deepEqual(outside.requests(), []);
    } finally {
      if (previousSkip === undefined) delete process.env.LINKR_SKIP_DESKTOP; else process.env.LINKR_SKIP_DESKTOP = previousSkip;
      await mock.close(); await outside.close(); await rm(directory, { recursive: true, force: true });
    }
  });
}
