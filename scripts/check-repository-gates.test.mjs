import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { POLICY_FILES, checkRepositoryGateContents } from "./check-repository-gates.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const PRJ_CONFIG = "apps/radxa_linkr_debugger/prj.conf";
const HIL_SPEC = "docs/testing/hil-functional-test-spec.md";
const OTA_HIL = "skills/radxa-linkr-debugger/scripts/web-ota-hil.sh";
const BUILD_WORKFLOW = ".github/workflows/build.yml";
const PAGES_WORKFLOW = ".github/workflows/pages.yml";
const RELEASE_WORKFLOW = ".github/workflows/release.yml";
const VERSION_BUMP_WORKFLOW = ".github/workflows/version-bump.yml";
const APP_CMAKE = "apps/radxa_linkr_debugger/CMakeLists.txt";
const RAM_SECTIONS = "apps/radxa_linkr_debugger/sections-ram.ld";
const CAPTURE_ARENA_HEADER = "apps/radxa_linkr_debugger/src/linkr_debugger_capture_arena.h";
const CAPTURE_ARENA_SOURCE = "apps/radxa_linkr_debugger/src/linkr_debugger_capture_arena.c";
const WS_HEADER = "apps/radxa_linkr_debugger/src/linkr_debugger_ws.h";
const WS_SOURCE = "apps/radxa_linkr_debugger/src/linkr_debugger_ws.c";
const SIGROK_HEADER = "apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.h";
const SIGROK_SOURCE = "apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.c";
const LOGIC_ANALYZER_SOURCE = "apps/radxa_linkr_debugger/src/linkr_debugger_logic_analyzer.c";
const CONTROL_SOURCE = "apps/radxa_linkr_debugger/src/linkr_debugger_control.c";
const LOGIC_ANALYZER_DOC = "docs/reference/logic-analyzer.md";
const DOCS_INDEX = "docs/README.md";
const MEMORY_POLICY_FILES = Object.freeze([
  APP_CMAKE,
  RAM_SECTIONS,
  CAPTURE_ARENA_HEADER,
  CAPTURE_ARENA_SOURCE,
  WS_HEADER,
  WS_SOURCE,
  SIGROK_HEADER,
  SIGROK_SOURCE,
  LOGIC_ANALYZER_SOURCE,
  CONTROL_SOURCE,
  PRJ_CONFIG,
  LOGIC_ANALYZER_DOC,
  DOCS_INDEX,
]);
const VALID_SHA = "0123456789abcdef0123456789abcdef01234567";

async function repositoryContents() {
  return new Map(await Promise.all(POLICY_FILES.map(async (relative) => {
    try {
      return [relative, await readFile(path.join(ROOT, relative), "utf8")];
    } catch (error) {
      if (error?.code === "ENOENT") return [relative, ""];
      throw error;
    }
  })));
}

function mutation(contents, relative, replace, replacement = "") {
  const changed = new Map(contents);
  const original = changed.get(relative);
  assert.ok(original.includes(replace), `mutation marker missing: ${relative}: ${replace}`);
  changed.set(relative, original.replace(replace, replacement));
  return changed;
}

function mutationAll(contents, relative, replace, replacement = "") {
  const changed = new Map(contents);
  const original = changed.get(relative);
  assert.ok(original.includes(replace), `mutation marker missing: ${relative}: ${replace}`);
  changed.set(relative, original.replaceAll(replace, replacement));
  return changed;
}

function actionMutation(contents, { relative, action, reference }) {
  const changed = new Map(contents);
  const original = changed.get(relative) ?? "";
  const actionReference = new RegExp(`(uses:\\s*${action}@)[^\\s#]+(?:\\s+#\\s*[^\\n]+)?`);
  assert.match(original, actionReference, `action mutation marker missing: ${relative}: ${action}`);
  changed.set(relative, original.replace(actionReference, `$1${reference}`));
  return changed;
}

function setConfig(source, key, value) {
  const assignment = new RegExp(`^${key}=.*$`, "m");
  return assignment.test(source)
    ? source.replace(assignment, `${key}=${value}`)
    : `${source}\n${key}=${value}\n`;
}

function appendIfMissing(source, marker, fixture) {
  return source.includes(marker) ? source : `${source}\n${fixture}`;
}

async function optimizedMemoryContents() {
  const contents = await repositoryContents();
  const configValues = [
    ["CONFIG_NET_PKT_RX_COUNT", "16"],
    ["CONFIG_NET_PKT_TX_COUNT", "16"],
    ["CONFIG_NET_BUF_RX_COUNT", "64"],
    ["CONFIG_NET_BUF_TX_COUNT", "64"],
    ["CONFIG_HEAP_MEM_POOL_SIZE", "49152"],
    ["CONFIG_NET_SOCKETS_SERVICE_STACK_SIZE", "2400"],
    ["CONFIG_NET_IPV6", "n"],
    ["CONFIG_I2C", "n"],
    ["CONFIG_SPI", "n"],
    ["CONFIG_MAIN_STACK_SIZE", "2048"],
  ];
  let prjConfig = contents.get(PRJ_CONFIG);
  for (const [key, value] of configValues) prjConfig = setConfig(prjConfig, key, value);
  contents.set(PRJ_CONFIG, prjConfig);
  contents.set(APP_CMAKE, appendIfMissing(
    contents.get(APP_CMAKE),
    "zephyr_linker_sources(RAM_SECTIONS SORT_KEY 0 sections-ram.ld)",
    "zephyr_linker_sources(RAM_SECTIONS SORT_KEY 0 sections-ram.ld)\n",
  ));
  contents.set(RAM_SECTIONS, `SECTION_PROLOGUE(.bss.pre_capture, (NOLOAD),)
{
  KEEP(*(.bss.pre_capture.sigrok_runtime))
  KEEP(*(.bss.pre_capture.ws_clients))
} GROUP_NOLOAD_LINK_IN(RAMABLE_REGION, RAMABLE_REGION)
ASSERT(SIZEOF(.bss.pre_capture) == 0xDA40, "pre-capture size")
ASSERT(__data_region_end <= 0x20010000, "pre-capture start")
ASSERT(__bss_start == 0x20010000, "generic BSS start")
ASSERT((__bss_start & 0x7fff) == 0, "generic BSS alignment")
`);
  let sigrokSource = contents.get(SIGROK_SOURCE);
  sigrokSource = appendIfMissing(sigrokSource, "Z_GENERIC_SECTION(.bss.pre_capture.sigrok_runtime)", `static struct linkr_debugger_sigrok_linkr_runtime linkr_debugger_sigrok_linkr_runtime
  Z_GENERIC_SECTION(.bss.pre_capture.sigrok_runtime);`);
  sigrokSource = appendIfMissing(sigrokSource, "BUILD_ASSERT(sizeof(linkr_debugger_sigrok_linkr_runtime) == 27168U)", "BUILD_ASSERT(sizeof(linkr_debugger_sigrok_linkr_runtime) == 27168U);");
  sigrokSource = appendIfMissing(sigrokSource, "static K_THREAD_STACK_DEFINE(server_stack, 2048U)", "static K_THREAD_STACK_DEFINE(server_stack, 2048U);");
  sigrokSource = appendIfMissing(sigrokSource, "memset(&linkr_debugger_sigrok_linkr_runtime, 0,", `void memory_contract_sigrok_init(void)
{
  memset(&linkr_debugger_sigrok_linkr_runtime, 0,
    sizeof(linkr_debugger_sigrok_linkr_runtime));
  linkr_debugger_sigrok_linkr_runtime.listen_fd = -1;
  linkr_debugger_sigrok_linkr_runtime.client_fd = -1;
  linkr_debugger_sigrok_linkr_runtime.next_sequence_id = 1U;
  k_fifo_init(&linkr_debugger_sigrok_linkr_runtime.stream_fifo);
}`);
  contents.set(SIGROK_SOURCE, sigrokSource);
  let wsSource = contents.get(WS_SOURCE);
  wsSource = appendIfMissing(wsSource, "Z_GENERIC_SECTION(.bss.pre_capture.ws_clients)", `static struct linkr_debugger_ws_client linkr_debugger_ws_clients
  Z_GENERIC_SECTION(.bss.pre_capture.ws_clients);`);
  wsSource = appendIfMissing(wsSource, "BUILD_ASSERT(sizeof(linkr_debugger_ws_clients) == 28704U)", "BUILD_ASSERT(sizeof(linkr_debugger_ws_clients) == 28704U);");
  wsSource = appendIfMissing(wsSource, "static K_THREAD_STACK_DEFINE(linkr_debugger_adc_sampler_stack, 2048)", "static K_THREAD_STACK_DEFINE(linkr_debugger_adc_sampler_stack, 2048);");
  wsSource = appendIfMissing(wsSource, "memset(linkr_debugger_ws_clients, 0, sizeof(linkr_debugger_ws_clients))", `void memory_contract_ws_init(void)
{
  memset(linkr_debugger_ws_clients, 0, sizeof(linkr_debugger_ws_clients));
  k_mutex_init(&linkr_debugger_ws_clients_lock);
}`);
  contents.set(WS_SOURCE, wsSource);
  let logicAnalyzerSource = contents.get(LOGIC_ANALYZER_SOURCE);
  logicAnalyzerSource = appendIfMissing(logicAnalyzerSource, "#define LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE 2048U", "#define LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE 2048U");
  logicAnalyzerSource = appendIfMissing(logicAnalyzerSource, "K_THREAD_STACK_DEFINE(la_stream_ring_thread_stack, LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE)", "K_THREAD_STACK_DEFINE(la_stream_ring_thread_stack, LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE);");
  logicAnalyzerSource = appendIfMissing(logicAnalyzerSource, "K_THREAD_STACK_DEFINE(la_stream_ring_consumer_thread_stack, LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE)", "K_THREAD_STACK_DEFINE(la_stream_ring_consumer_thread_stack, LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE);");
  contents.set(LOGIC_ANALYZER_SOURCE, logicAnalyzerSource);
  contents.set(CONTROL_SOURCE, appendIfMissing(
    contents.get(CONTROL_SOURCE),
    "K_THREAD_STACK_DEFINE(linkr_debugger_watchdog_supervisor_stack, 1024)",
    "K_THREAD_STACK_DEFINE(linkr_debugger_watchdog_supervisor_stack, 1024);",
  ));
  return contents;
}

function workflowJob(workflow, name) {
  const body = workflow.slice(Math.max(0, workflow.search(/^jobs:\s*$/m)));
  const headers = [...body.matchAll(/^  ([a-z0-9-]+):\s*$/gm)];
  const index = headers.findIndex((header) => header[1] === name);
  assert.notEqual(index, -1, `workflow job missing: ${name}`);
  return body.slice(headers[index].index, headers[index + 1]?.index);
}

function sourceGateScript(workflow) {
  const source = workflowJob(workflow, "source");
  const marker = "        run: |\n";
  const start = source.indexOf(marker);
  assert.notEqual(start, -1, "source gate run step missing");
  const run = source.slice(start + marker.length);
  const nextStep = run.search(/^      - /m);
  return run.slice(0, nextStep < 0 ? undefined : nextStep).replace(/^          /gm, "");
}

async function runSourceGate(workflow, requested, inherited) {
  const directory = await mkdtemp(path.join(tmpdir(), "agent-debugboard-source-gate-"));
  const output = path.join(directory, "github-output");
  await writeFile(output, "");
  try {
    const result = spawnSync("bash", ["-euo", "pipefail", "-c", sourceGateScript(workflow)], {
      encoding: "utf8",
      env: {
        ...process.env,
        GITHUB_OUTPUT: output,
        INHERITED_SOURCE_SHA: inherited,
        REQUESTED_SOURCE_SHA: requested,
      },
    });
    return { output: await readFile(output, "utf8"), status: result.status, stderr: result.stderr };
  } finally {
    await rm(directory, { force: true, recursive: true });
  }
}

test("accepts the complete repository gate contract", async () => {
  assert.deepEqual(checkRepositoryGateContents(await repositoryContents()), { ok: true, failures: [] });
});

test("Given an inherited or immutable source SHA, when the source gate runs, then it emits the effective SHA", async (t) => {
  const build = (await repositoryContents()).get(BUILD_WORKFLOW);

  await t.test("uses the inherited SHA for an empty reusable input", async () => {
    const result = await runSourceGate(build, "", VALID_SHA);
    assert.equal(result.status, 0);
    assert.equal(result.output, `effective_source_sha=${VALID_SHA}\n`);
  });

  await t.test("uses a valid immutable reusable input", async () => {
    const requested = "fedcba9876543210fedcba9876543210fedcba98";
    const result = await runSourceGate(build, requested, VALID_SHA);
    assert.equal(result.status, 0);
    assert.equal(result.output, `effective_source_sha=${requested}\n`);
  });
});

test("Given an invalid reusable source SHA, when the source gate runs, then it fails before checkout", async (t) => {
  const build = (await repositoryContents()).get(BUILD_WORKFLOW);
  const cases = [
    ["short", VALID_SHA.slice(0, -1)],
    ["uppercase", VALID_SHA.toUpperCase()],
    ["symbolic", "main"],
    ["whitespace", `${VALID_SHA} `],
  ];

  for (const [name, requested] of cases) {
    await t.test(name, async () => {
      const result = await runSourceGate(build, requested, VALID_SHA);
      assert.notEqual(result.status, 0);
      assert.match(result.stderr, /source_sha must be exactly 40 lowercase hexadecimal characters/);
      assert.equal(result.output, "");
    });
  }
});

test("rejects immutable source selection and action pin regressions", async (t) => {
  const baseline = await repositoryContents();
  const cases = [
    ["source input validation", BUILD_WORKFLOW, 'if [[ -n "$REQUESTED_SOURCE_SHA" && ! "$REQUESTED_SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]]; then', "if false; then"],
    ["source input grammar", BUILD_WORKFLOW, "^[0-9a-f]{40}$", "^[0-9a-fA-F]{40}$"],
    ["version gate source dependency", BUILD_WORKFLOW, "  version-gate:\n    name: Check synchronized project version\n    needs: source\n", "  version-gate:\n    name: Check synchronized project version\n"],
    ["Nix source dependency", BUILD_WORKFLOW, "  nix:\n    name: Check Nix flake\n    needs: source\n", "  nix:\n    name: Check Nix flake\n"],
    ["aggregate source dependency", BUILD_WORKFLOW, "    needs:\n      - source\n      - version-gate\n", "    needs:\n      - version-gate\n"],
    ["build mutable action", BUILD_WORKFLOW, "uses: actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803 # v6.1.0", "uses: actions/checkout@v6 # v6.1.0"],
    ["build wrong action pin", BUILD_WORKFLOW, "uses: actions/setup-python@ece7cb06caefa5fff74198d8649806c4678c61a1 # v6.3.0", "uses: actions/setup-python@d23441a48e516b6c34aea4fa41551a30e30af803 # v6.3.0"],
    ["build wrong action comment", BUILD_WORKFLOW, "uses: actions/setup-node@49933ea5288caeca8642d1e84afbd3f7d6820020 # v4.4.0", "uses: actions/setup-node@49933ea5288caeca8642d1e84afbd3f7d6820020 # v4.0.0"],
    ["release mutable action", RELEASE_WORKFLOW, "uses: actions/download-artifact@018cc2cf5baa6db3ef3c5f8a56943fffe632ef53 # v6.0.0", "uses: actions/download-artifact@v6 # v6.0.0"],
    ["release wrong action pin", RELEASE_WORKFLOW, "uses: dtolnay/rust-toolchain@4360b52568e2003a75bf9bc1d59f33a8e3fc893c # stable", "uses: dtolnay/rust-toolchain@d23441a48e516b6c34aea4fa41551a30e30af803 # stable"],
    ["release wrong action comment", RELEASE_WORKFLOW, "uses: zephyrproject-rtos/action-zephyr-setup@be8136a8bba01580485d98b7ad2d32477c36a49a # v1", "uses: zephyrproject-rtos/action-zephyr-setup@be8136a8bba01580485d98b7ad2d32477c36a49a # v2"],
  ];

  for (const [name, relative, replace, replacement] of cases) {
    await t.test(name, () => {
      const result = checkRepositoryGateContents(mutation(baseline, relative, replace, replacement));
      assert.equal(result.ok, false);
      assert.ok(result.failures.some((failure) => failure.code === "G12"));
    });
  }
});

test("rejects mutable, wrong, and missing action pins in every governed workflow", async (t) => {
  const baseline = await repositoryContents();
  baseline.set(VERSION_BUMP_WORKFLOW, await readFile(path.join(ROOT, VERSION_BUMP_WORKFLOW), "utf8"));
  const workflows = [
    {
      name: "build",
      relative: BUILD_WORKFLOW,
      action: "actions/checkout",
      sha: "d23441a48e516b6c34aea4fa41551a30e30af803",
      tag: "v6",
      comment: "v6.1.0",
    },
    {
      name: "release",
      relative: RELEASE_WORKFLOW,
      action: "actions/checkout",
      sha: "d23441a48e516b6c34aea4fa41551a30e30af803",
      tag: "v6",
      comment: "v6.1.0",
    },
    {
      name: "Pages",
      relative: PAGES_WORKFLOW,
      action: "actions/configure-pages",
      sha: "983d7736d9b0ae728b81ab479565c72886d7745b",
      tag: "v5",
      comment: "v5.0.0",
    },
    {
      name: "Version Bump",
      relative: VERSION_BUMP_WORKFLOW,
      action: "actions/setup-python",
      sha: "ece7cb06caefa5fff74198d8649806c4678c61a1",
      tag: "v6",
      comment: "v6.3.0",
    },
  ];
  const mutations = [
    ["mutable", ({ tag, comment }) => `${tag} # ${comment}`],
    ["wrong", ({ comment }) => `${VALID_SHA} # ${comment}`],
    ["missing comment", ({ sha }) => sha],
  ];

  for (const workflow of workflows) {
    for (const [kind, reference] of mutations) {
      await t.test(`${workflow.name} ${kind} action pin`, () => {
        const result = checkRepositoryGateContents(actionMutation(baseline, {
          relative: workflow.relative,
          action: workflow.action,
          reference: reference(workflow),
        }));
        assert.equal(result.ok, false);
        assert.ok(result.failures.some((failure) => failure.code === "G12"));
      });
    }
  }
});

test("loads the Version Bump workflow for action pin validation", () => {
  assert.ok(POLICY_FILES.includes(VERSION_BUMP_WORKFLOW));
});

test("rejects a Pages validation source SHA override", async () => {
  const baseline = await repositoryContents();
  const result = checkRepositoryGateContents(mutation(
    baseline,
    PAGES_WORKFLOW,
    "    uses: ./.github/workflows/build.yml\n",
    "    uses: ./.github/workflows/build.yml\n    with:\n      source_sha: ${{ github.sha }}\n",
  ));
  assert.equal(result.ok, false);
  assert.ok(result.failures.some((failure) => failure.code === "G04"));
});

test("rejects per-step release checkout and permission-scope regressions", async (t) => {
  const baseline = await repositoryContents();
  const cases = [
    ["second release checkout without a resolved ref", RELEASE_WORKFLOW, "          persist-credentials: false\n\n      - name: Set up Python\n", "          persist-credentials: false\n\n      - name: Checkout injected source\n        uses: actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803 # v6.1.0\n        with:\n          persist-credentials: false\n\n      - name: Set up Python\n"],
    ["root id-token permission", RELEASE_WORKFLOW, "permissions:\n  contents: read\n\njobs:\n", "permissions:\n  contents: read\n  id-token: write\n\njobs:\n"],
    ["root packages permission", RELEASE_WORKFLOW, "permissions:\n  contents: read\n\njobs:\n", "permissions:\n  contents: read\n  packages: read\n\njobs:\n"],
    ["root actions permission", RELEASE_WORKFLOW, "permissions:\n  contents: read\n\njobs:\n", "permissions:\n  contents: read\n  actions: read\n\njobs:\n"],
    ["blank-separated root permission", RELEASE_WORKFLOW, "permissions:\n  contents: read\n\njobs:\n", "permissions:\n  contents: read\n\n  id-token: write\n\njobs:\n"],
    ["nonpublisher id-token permission", RELEASE_WORKFLOW, "  version-gate:\n    name: Check synchronized project version and tag\n    needs: resolve\n", "  version-gate:\n    name: Check synchronized project version and tag\n    needs: resolve\n    permissions:\n      contents: read\n      id-token: write\n"],
    ["nonpublisher packages permission", RELEASE_WORKFLOW, "  version-gate:\n    name: Check synchronized project version and tag\n    needs: resolve\n", "  version-gate:\n    name: Check synchronized project version and tag\n    needs: resolve\n    permissions:\n      contents: read\n      packages: read\n"],
    ["nonpublisher actions permission", RELEASE_WORKFLOW, "  version-gate:\n    name: Check synchronized project version and tag\n    needs: resolve\n", "  version-gate:\n    name: Check synchronized project version and tag\n    needs: resolve\n    permissions:\n      contents: read\n      actions: read\n"],
    ["blank-separated nonpublisher permission", RELEASE_WORKFLOW, "  version-gate:\n    name: Check synchronized project version and tag\n    needs: resolve\n", "  version-gate:\n    name: Check synchronized project version and tag\n    needs: resolve\n    permissions:\n      contents: read\n\n      id-token: write\n"],
    ["publisher extra id-token permission", RELEASE_WORKFLOW, "    permissions:\n      contents: write\n    needs:\n", "    permissions:\n      contents: write\n      id-token: write\n    needs:\n"],
    ["blank-separated publisher permission", RELEASE_WORKFLOW, "    permissions:\n      contents: write\n    needs:\n", "    permissions:\n      contents: write\n\n      id-token: write\n    needs:\n"],
  ];

  for (const [name, relative, replace, replacement] of cases) {
    await t.test(name, () => {
      const result = checkRepositoryGateContents(mutation(baseline, relative, replace, replacement));
      assert.equal(result.ok, false);
      assert.ok(result.failures.some((failure) => failure.code === "G12"));
    });
  }
});

test("includes every source that defines the exported OpenOCD contract", () => {
  assert.deepEqual(
    POLICY_FILES.filter((relative) => relative.startsWith("nix/")).sort(),
    ["nix/openocd-latest.nix", "nix/overlay.nix"],
  );
  assert.ok(POLICY_FILES.includes("Makefile"));
  assert.ok(POLICY_FILES.includes("flake.nix"));
});

test("includes the firmware local-only DHCP policy", () => {
  assert.ok(POLICY_FILES.includes(PRJ_CONFIG));
});

test("includes the Web OTA HIL timeout policy", () => {
  assert.ok(POLICY_FILES.includes(OTA_HIL));
});

test("rejects Web OTA negative-upload timeout regressions", async (t) => {
  const baseline = await repositoryContents();
  const cases = [
    ["bad SHA dry-run upload", 'plan run_timeout "$UPLOAD_TIMEOUT" curl -sS -o /tmp/linkr-ota-bad-sha.json', 'plan run_timeout "$SHORT_TIMEOUT" curl -sS -o /tmp/linkr-ota-bad-sha.json'],
    ["bad SHA upload", 'bad_sha_http=$(run_timeout "$UPLOAD_TIMEOUT" curl -sS -o "$bad_sha_body"', 'bad_sha_http=$(run_timeout "$SHORT_TIMEOUT" curl -sS -o "$bad_sha_body"'],
    ["bad Content-Type dry-run upload", 'plan run_timeout "$UPLOAD_TIMEOUT" curl -sS -o /tmp/linkr-ota-bad-type.json', 'plan run_timeout "$SHORT_TIMEOUT" curl -sS -o /tmp/linkr-ota-bad-type.json'],
    ["bad Content-Type upload", 'bad_type_http=$(run_timeout "$UPLOAD_TIMEOUT" curl -sS -o "$bad_type_body"', 'bad_type_http=$(run_timeout "$SHORT_TIMEOUT" curl -sS -o "$bad_type_body"'],
    ["status request", 'run_timeout "$SHORT_TIMEOUT" curl -fsS "$(api_url "$1")"', 'run_timeout "$UPLOAD_TIMEOUT" curl -fsS "$(api_url "$1")"'],
    ["OTA test control", 'maybe_run run_timeout "$SHORT_TIMEOUT" curl -fsS -X POST "$(api_url /ota/test)"', 'maybe_run run_timeout "$UPLOAD_TIMEOUT" curl -fsS -X POST "$(api_url /ota/test)"'],
    ["OTA confirmation control", 'maybe_run run_timeout "$SHORT_TIMEOUT" curl -fsS -X POST "$(api_url /ota/confirm)"', 'maybe_run run_timeout "$UPLOAD_TIMEOUT" curl -fsS -X POST "$(api_url /ota/confirm)"'],
    ["post-negative-upload status", 'status_body=$(run_timeout "$SHORT_TIMEOUT" curl -fsS "$(api_url /ota)")', 'status_body=$(run_timeout "$UPLOAD_TIMEOUT" curl -fsS "$(api_url /ota)")'],
  ];

  for (const [name, replace, replacement] of cases) {
    await t.test(name, () => {
      const result = checkRepositoryGateContents(mutation(baseline, OTA_HIL, replace, replacement));
      assert.equal(result.ok, false);
      assert.ok(result.failures.some((failure) => failure.code === "G17"));
    });
  }
});

test("rejects captive-portal local-only DHCP regressions", async (t) => {
  const baseline = await repositoryContents();
  baseline.set(PRJ_CONFIG, await readFile(path.join(ROOT, PRJ_CONFIG), "utf8"));
  const specTitle = "# HIL 功能测试规范\n";
  const cases = [
    ["DHCP router option", PRJ_CONFIG, "CONFIG_NET_DHCPV4_SERVER_OPTION_ROUTER=n\n", "CONFIG_NET_DHCPV4_SERVER_OPTION_ROUTER=y\n", "G16"],
    ["DHCP DNS address", PRJ_CONFIG, "CONFIG_NET_DHCPV4_SERVER_OPTION_DNS_ADDRESS=\"\"\n", "CONFIG_NET_DHCPV4_SERVER_OPTION_DNS_ADDRESS=\"1.1.1.1\"\n", "G16"],
    ["board wildcard DNS expectation", HIL_SPEC, specTitle, `${specTitle}\n\`\`\`sh\ndig @172.29.203.1 anything.example A\n\`\`\`\n`, "G16"],
    ["router and DNS in ACK", HIL_SPEC, specTitle, `${specTitle}\nDHCPACK 必须包含 DHCP option 3（router）和 DHCP option 6（DNS）。\n`, "G16"],
  ];

  for (const [name, relative, replace, replacement, code] of cases) {
    await t.test(name, () => {
      const result = checkRepositoryGateContents(mutation(baseline, relative, replace, replacement));
      assert.equal(result.ok, false);
      assert.ok(result.failures.some((failure) => failure.code === code));
    });
  }
});

test("requires durable CI and local gate registrations", async (t) => {
  const baseline = await repositoryContents();
  const cases = [
    ["CI rdb alias test", BUILD_WORKFLOW, "scripts/check-rdb-alias.test.mjs ", ""],
    ["CI test-registration checker", BUILD_WORKFLOW, "node scripts/check-test-registration.mjs --root .\n", ""],
    ["local nightly test", "Makefile", "scripts/check-nightly-workflow.test.mjs \\\n", ""],
    ["local skill-boundary checker", "Makefile", "node scripts/check-skill-boundary.mjs --root .", ""],
  ];

  for (const [name, relative, replace, replacement] of cases) {
    await t.test(name, () => {
      const result = checkRepositoryGateContents(mutation(baseline, relative, replace, replacement));
      assert.equal(result.ok, false);
      assert.ok(result.failures.some((failure) => failure.code === "G12"));
    });
  }
});

test("requires the rdb alias test and checker in Makefile gates", async (t) => {
  const baseline = await repositoryContents();
  const cases = [
    ["test", "scripts/check-rdb-alias.test.mjs \\\n"],
    ["checker", "node scripts/check-rdb-alias.mjs --root ."],
  ];

  for (const [name, replace] of cases) {
    await t.test(name, () => {
      const result = checkRepositoryGateContents(mutation(baseline, "Makefile", replace, ""));
      assert.equal(result.ok, false);
      assert.ok(result.failures.some((failure) => failure.code === "G12"));
    });
  }
});

test("rejects permanent worktree-scope registration", async (t) => {
  const baseline = await repositoryContents();
  const cases = [
    ["CI checker", BUILD_WORKFLOW, "          node scripts/check-skill-boundary.mjs --root .\n", "          node scripts/check-skill-boundary.mjs --root .\n          node scripts/check-worktree-scope.mjs --root .\n"],
    ["Makefile checker", "Makefile", "node scripts/check-skill-boundary.mjs --root .", "node scripts/check-skill-boundary.mjs --root . && \\\n\t$(NIX) \"node scripts/check-worktree-scope.mjs --root .\""],
  ];

  for (const [name, relative, replace, replacement] of cases) {
    await t.test(name, () => {
      const result = checkRepositoryGateContents(mutation(baseline, relative, replace, replacement));
      assert.equal(result.ok, false);
      assert.ok(result.failures.some((failure) => failure.code === "G12"));
    });
  }
});

test("requires CI and Makefile durable gate parity", async () => {
  const baseline = await repositoryContents();
  const result = checkRepositoryGateContents(mutation(
    baseline,
    "Makefile",
    "scripts/check-doc-layout.test.mjs \\\n",
    "",
  ));
  assert.equal(result.ok, false);
  assert.ok(result.failures.some((failure) => failure.code === "G12"));
});

test("rejects every publication and branch-policy bypass", async (t) => {
  const baseline = await repositoryContents();
  const cases = [
    ["build dependency", ".github/workflows/build.yml", "      - host-cli\n", "", "G03"],
    ["desktop dependency", ".github/workflows/build.yml", "      - desktop-release\n", "", "G03"],
    ["Host Tools dependency", ".github/workflows/build.yml", "      - host-tools\n", "", "G03"],
    ["Pages validation", ".github/workflows/pages.yml", "    needs: validation\n", "", "G04"],
    ["release validation", ".github/workflows/release.yml", "      - validation\n", "", "G05"],
    ["nightly validation", ".github/workflows/nightly.yml", "    needs: validation\n", "", "G06"],
    ["pull request policy", "AGENTS.md", "  must require pull requests, reject direct pushes, and require\n", "  should allow direct pushes and require\n", "G07"],
    ["Web discovery runner", "web/package.json", '"test": "node scripts/run-tests.mjs"', '"test": "node --test one.test.mjs"', "G08"],
    ["embedded Web Rust toolchain", "shell.nix", "    pkgs.cargo\n", "", "G09"],
    ["Rust clippy toolchain", "shell.nix", "    pkgs.clippy\n", "", "G09"],
    ["Rust formatting toolchain", "shell.nix", "    pkgs.rustfmt\n", "", "G09"],
    ["OpenOCD source pin", "nix/openocd-latest.nix", '    rev = "da3920b0a52dc2d394afb222c688dac7e57acc1b";\n', "", "G10"],
    ["OpenOCD overlay export", "nix/overlay.nix", "  openocd-latest = final.callPackage ./openocd-latest.nix { };\n", "", "G10"],
    ["OpenOCD flake package", "flake.nix", "        packages = {\n          openocd-latest = pkgs.openocd-latest;\n", "        packages = {\n", "G10"],
    ["OpenOCD flake check", "flake.nix", "        checks = {\n          build = pkgs.radxa-linkr-debuggerctl;\n          openocd-latest = pkgs.openocd-latest;\n", "        checks = {\n          build = pkgs.radxa-linkr-debuggerctl;\n", "G10"],
    ["OpenOCD shell reuse", "shell.nix", "  openocdLatest = pkgs.callPackage ./nix/openocd-latest.nix { };\n", "", "G10"],
    ["persistent docs test", "Makefile", "scripts/check-persistent-configuration-docs.test.mjs", "", "G11"],
    ["persistent docs checker", "Makefile", "node scripts/check-persistent-configuration-docs.mjs --root .", "", "G11"],
    ["reusable source SHA input", ".github/workflows/build.yml", "      source_sha:\n", "", "G12"],
    ["complete build checkout binding", ".github/workflows/build.yml", "          ref: ${{ needs.source.outputs.effective_source_sha }}\n", "", "G12"],
    ["resolver tag peeling", ".github/workflows/release.yml", 'git rev-parse --verify "${tag_ref}^{commit}"', 'git rev-parse --verify "$tag_ref"', "G12"],
    ["resolver tag fetch", ".github/workflows/release.yml", "          fetch-tags: true\n", "", "G12"],
    ["validation SHA binding", ".github/workflows/release.yml", "      source_sha: ${{ needs.resolve.outputs.resolved_sha }}", "", "G12"],
    ["release checkout SHA", ".github/workflows/release.yml", "      - name: Checkout application\n        uses: actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803 # v6.1.0\n        with:\n          fetch-depth: 0\n          ref: ${{ needs.resolve.outputs.resolved_sha }}\n          path: app\n          persist-credentials: false\n\n      - name: Set up Python\n", "      - name: Checkout application\n        uses: actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803 # v6.1.0\n        with:\n          fetch-depth: 0\n          ref: ${{ needs.resolve.outputs.normalized_tag }}\n          path: app\n          persist-credentials: false\n\n      - name: Set up Python\n", "G12"],
    ["desktop checkout SHA", ".github/workflows/release.yml", "            asset_name: radxa-linkr-desktop_windows_amd64.zip\n\n    steps:\n      - name: Checkout application\n        uses: actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803 # v6.1.0\n        with:\n          ref: ${{ needs.resolve.outputs.resolved_sha }}\n", "            asset_name: radxa-linkr-desktop_windows_amd64.zip\n\n    steps:\n      - name: Checkout application\n        uses: actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803 # v6.1.0\n        with:\n          ref: ${{ needs.resolve.outputs.normalized_tag }}\n", "G12"],
    ["desktop resolve dependency", ".github/workflows/release.yml", "    needs:\n      - resolve\n      - validation\n      - version-gate\n", "    needs:\n      - validation\n      - version-gate\n", "G12"],
    ["direct resolver dependency", ".github/workflows/release.yml", "      - resolve\n      - validation\n", "      - validation\n", "G12"],
    ["remote tag peel", ".github/workflows/release.yml", '          while [[ "$object_type" == "tag" ]]; do', "          while false; do", "G12"],
    ["remote SHA comparison", ".github/workflows/release.yml", '          [[ "$object_sha" == "$RESOLVED_SHA" ]]\n', "", "G12"],
    ["write permission scope", ".github/workflows/release.yml", "  contents: read\n", "  contents: write\n", "G12"],
    ["English flake input", "docs/developer/build.md", '    agent-debugboard.url = "github:xzl01/agent-debugboard";\n', "", "G13"],
    ["Chinese flake input", "docs/developer/build.zh-CN.md", '    agent-debugboard.url = "github:xzl01/agent-debugboard";\n', "", "G13"],
    ["legacy import expression", "docs/developer/build.md", '        overlays = [ agent-debugboard.overlays.default ];\n', '        overlays = [ (import github:xzl01/agent-debugboard).overlays.default ];\n', "G13"],
    ["status response wording", "docs/testing/hil-functional-test-spec.md", "验证 HTTP 响应 JSON 长度低于专用 6144 字节 status buffer（协议限制检查）：\n", "验证 HTTP 响应 JSON 长度低于专用 4096 字节 status buffer（协议限制检查）：\n", "G14"],
    ["status response assertion", "docs/testing/hil-functional-test-spec.md", '[ "$LEN" -lt 6144 ] || { echo "status response must be below 6144 bytes"; exit 1; }\n', '[ "$LEN" -lt 4096 ] || { echo "status response must be below 4096 bytes"; exit 1; }\n', "G14"],
    ["status buffer capacity", "apps/radxa_linkr_debugger/src/linkr_debugger_http.c", "#define LINKR_DEBUGGER_HTTP_STATUS_JSON_BUFSZ 6144U\n", "#define LINKR_DEBUGGER_HTTP_STATUS_JSON_BUFSZ 4096U\n", "G14"],
    ["WS snapshot cadence", "docs/testing/hil-functional-test-spec.md", "const MAX_SNAPSHOTS = 1;\n", "const MAX_SNAPSHOTS = 3;\n", "G15"],
    ["WS state snapshot gate", "apps/radxa_linkr_debugger/src/linkr_debugger_ws.c", "if (events & LINKR_DEBUGGER_WS_EVENT_STATE) {\n", "if (events & LINKR_DEBUGGER_WS_EVENT_SAMPLE) {\n", "G15"],
    ["Web dependency registry", "web/package-lock.json", "https://registry.npmjs.org/react-grab/-/react-grab-0.1.50.tgz", "https://repo.huaweicloud.com/repository/npm/react-grab/-/react-grab-0.1.50.tgz", "G11"],
    ["desktop release preflight", ".github/workflows/build.yml", "          - os: windows-latest\n            asset_name: radxa-linkr-desktop_windows_amd64.zip\n", "", "G09"],
    ["desktop decoder toolchain", ".github/workflows/build.yml", "      - name: Install wasm target\n        run: rustup target add wasm32-unknown-unknown\n\n      - name: Set up Node.js\n", "      - name: Set up Node.js\n", "G09"],
    ["release desktop decoder toolchain", ".github/workflows/release.yml", "      - name: Install wasm target\n        run: rustup target add wasm32-unknown-unknown\n\n      - name: Set up Node.js\n", "      - name: Set up Node.js\n", "G09"],
    ["prerelease marking", ".github/workflows/release.yml", "            release_flags+=(--prerelease --latest=false)\n", "", "G10"],
    ["curated release notes", ".github/workflows/release.yml", '          release_notes="docs/releases/${RELEASE_TAG%%-*}.md"\n', "", "G10"],
  ];

  for (const [name, relative, replace, replacement, code] of cases) {
    await t.test(name, () => {
      const result = checkRepositoryGateContents(mutation(baseline, relative, replace, replacement));
      assert.equal(result.ok, false);
      assert.ok(result.failures.some((failure) => failure.code === code));
    });
  }
});

test("requires every firmware memory optimization policy input", () => {
  for (const relative of MEMORY_POLICY_FILES) assert.ok(POLICY_FILES.includes(relative));
});

test("accepts the approved firmware memory optimization contract", async () => {
  assert.deepEqual(checkRepositoryGateContents(await optimizedMemoryContents()), { ok: true, failures: [] });
});

test("rejects every firmware memory optimization contract regression", async (t) => {
  const baseline = await optimizedMemoryContents();
  const cases = [
    ["RAM linker registration", APP_CMAKE, "zephyr_linker_sources(RAM_SECTIONS SORT_KEY 0 sections-ram.ld)", "zephyr_linker_sources(SECTIONS sections-ram.ld)"],
    ["pre-capture NOBITS section", RAM_SECTIONS, "SECTION_PROLOGUE(.bss.pre_capture, (NOLOAD),)", "SECTION_PROLOGUE(.bss.pre_capture, (),)"],
    ["Sigrok pre-capture linker section", RAM_SECTIONS, ".bss.pre_capture.sigrok_runtime", ".bss.pre_capture.other"],
    ["WS pre-capture linker section", RAM_SECTIONS, ".bss.pre_capture.ws_clients", ".bss.pre_capture.other"],
    ["pre-capture no-load group", RAM_SECTIONS, "GROUP_NOLOAD_LINK_IN(RAMABLE_REGION, RAMABLE_REGION)", "GROUP_LINK_IN(RAMABLE_REGION)"],
    ["pre-capture size assertion", RAM_SECTIONS, "ASSERT(SIZEOF(.bss.pre_capture) == 0xDA40", "ASSERT(SIZEOF(.bss.pre_capture) == 0xDA41"],
    ["pre-capture start assertion", RAM_SECTIONS, "ASSERT(__data_region_end <= 0x20010000", "ASSERT(__data_region_end <= 0x2000ffff"],
    ["generic BSS start assertion", RAM_SECTIONS, "ASSERT(__bss_start == 0x20010000", "ASSERT(__bss_start == 0x20010001"],
    ["generic BSS alignment assertion", RAM_SECTIONS, "(__bss_start & 0x7fff)", "(__bss_start & 0x3fff)"],
    ["capture arena capacity", LOGIC_ANALYZER_DOC, "max(normal, burst)=149048 B", "max(normal, burst)=149047 B", true],
    ["capture arena alignment", CAPTURE_ARENA_HEADER, "#define LINKR_DEBUGGER_CAPTURE_ARENA_ALIGN 32768U", "#define LINKR_DEBUGGER_CAPTURE_ARENA_ALIGN 16384U"],
    ["capture arena alignment declaration", CAPTURE_ARENA_SOURCE, "__aligned(LINKR_DEBUGGER_CAPTURE_ARENA_ALIGN)", ""],
    ["RX packet pool", PRJ_CONFIG, "CONFIG_NET_PKT_RX_COUNT=16", "CONFIG_NET_PKT_RX_COUNT=15"],
    ["TX packet pool", PRJ_CONFIG, "CONFIG_NET_PKT_TX_COUNT=16", "CONFIG_NET_PKT_TX_COUNT=15"],
    ["RX net buffer", PRJ_CONFIG, "CONFIG_NET_BUF_RX_COUNT=64", "CONFIG_NET_BUF_RX_COUNT=63"],
    ["TX net buffer", PRJ_CONFIG, "CONFIG_NET_BUF_TX_COUNT=64", "CONFIG_NET_BUF_TX_COUNT=63"],
    ["heap size", PRJ_CONFIG, "CONFIG_HEAP_MEM_POOL_SIZE=49152", "CONFIG_HEAP_MEM_POOL_SIZE=49151"],
    ["socket-service stack", PRJ_CONFIG, "CONFIG_NET_SOCKETS_SERVICE_STACK_SIZE=2400", "CONFIG_NET_SOCKETS_SERVICE_STACK_SIZE=2399"],
    ["IPv6 disablement", PRJ_CONFIG, "CONFIG_NET_IPV6=n", "CONFIG_NET_IPV6=y"],
    ["I2C disablement", PRJ_CONFIG, "CONFIG_I2C=n", "CONFIG_I2C=y"],
    ["SPI disablement", PRJ_CONFIG, "CONFIG_SPI=n", "CONFIG_SPI=y"],
    ["main stack", PRJ_CONFIG, "CONFIG_MAIN_STACK_SIZE=2048", "CONFIG_MAIN_STACK_SIZE=2049"],
    ["WS client capacity", WS_HEADER, "#define LINKR_DEBUGGER_WS_MAX_CLIENTS 4", "#define LINKR_DEBUGGER_WS_MAX_CLIENTS 3"],
    ["WS send buffer", WS_SOURCE, "#define LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE 6144", "#define LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE 6143"],
    ["Sigrok ring", SIGROK_HEADER, "#define LINKR_DEBUGGER_SIGROK_LINKR_RING_BUFFER_BYTES 32768U", "#define LINKR_DEBUGGER_SIGROK_LINKR_RING_BUFFER_BYTES 32767U"],
    ["Sigrok stream queue depth", SIGROK_HEADER, "#define LINKR_DEBUGGER_SIGROK_LINKR_STREAM_QDEPTH_LIMIT 32U", "#define LINKR_DEBUGGER_SIGROK_LINKR_STREAM_QDEPTH_LIMIT 31U"],
    ["Sigrok WS data slots", SIGROK_HEADER, "#define LINKR_DEBUGGER_SIGROK_LINKR_WS_DATA_SLOT_COUNT 8U", "#define LINKR_DEBUGGER_SIGROK_LINKR_WS_DATA_SLOT_COUNT 7U"],
    ["Sigrok WS terminal slots", SIGROK_HEADER, "#define LINKR_DEBUGGER_SIGROK_LINKR_WS_TERMINAL_SLOT_COUNT 1U", "#define LINKR_DEBUGGER_SIGROK_LINKR_WS_TERMINAL_SLOT_COUNT 2U"],
    ["Sigrok raw burst slots", SIGROK_HEADER, "#define LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT 12U", "#define LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT 11U"],
    ["Sigrok raw burst queue budget", SIGROK_HEADER, "#define LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_QUEUE_MEMORY_LIMIT_BYTES 49152U", "#define LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_QUEUE_MEMORY_LIMIT_BYTES 49151U"],
    ["Sigrok pre-capture placement", SIGROK_SOURCE, "Z_GENERIC_SECTION(.bss.pre_capture.sigrok_runtime)", "__attribute__((section(\".bss.pre_capture.sigrok_runtime\")))"],
    ["Sigrok runtime size", SIGROK_SOURCE, "BUILD_ASSERT(sizeof(linkr_debugger_sigrok_linkr_runtime) == 27168U)", "BUILD_ASSERT(sizeof(linkr_debugger_sigrok_linkr_runtime) == 27167U)"],
    ["Sigrok runtime zeroing", SIGROK_SOURCE, "memset(&linkr_debugger_sigrok_linkr_runtime, 0,", ""],
    ["Sigrok listen descriptor restore", SIGROK_SOURCE, "linkr_debugger_sigrok_linkr_runtime.listen_fd = -1", "linkr_debugger_sigrok_linkr_runtime.listen_fd = 0"],
    ["Sigrok client descriptor restore", SIGROK_SOURCE, "linkr_debugger_sigrok_linkr_runtime.client_fd = -1", "linkr_debugger_sigrok_linkr_runtime.client_fd = 0"],
    ["Sigrok sequence restore", SIGROK_SOURCE, "linkr_debugger_sigrok_linkr_runtime.next_sequence_id = 1U", "linkr_debugger_sigrok_linkr_runtime.next_sequence_id = 0U"],
    ["Sigrok server stack", SIGROK_SOURCE, "static K_THREAD_STACK_DEFINE(server_stack, 2048U)", "static K_THREAD_STACK_DEFINE(server_stack, 2049U)"],
    ["WS pre-capture placement", WS_SOURCE, "Z_GENERIC_SECTION(.bss.pre_capture.ws_clients)", "__attribute__((section(\".bss.pre_capture.ws_clients\")))"],
    ["WS clients size", WS_SOURCE, "BUILD_ASSERT(sizeof(linkr_debugger_ws_clients) == 28704U)", "BUILD_ASSERT(sizeof(linkr_debugger_ws_clients) == 28703U)"],
    ["WS client zeroing", WS_SOURCE, "memset(linkr_debugger_ws_clients, 0, sizeof(linkr_debugger_ws_clients))", ""],
    ["ADC sampler stack", WS_SOURCE, "static K_THREAD_STACK_DEFINE(linkr_debugger_adc_sampler_stack, 2048)", "static K_THREAD_STACK_DEFINE(linkr_debugger_adc_sampler_stack, 2049)"],
    ["logic analyzer ring stacks", LOGIC_ANALYZER_SOURCE, "#define LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE 2048U", "#define LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE 2049U"],
    ["watchdog stack", CONTROL_SOURCE, "K_THREAD_STACK_DEFINE(linkr_debugger_watchdog_supervisor_stack, 1024)", "K_THREAD_STACK_DEFINE(linkr_debugger_watchdog_supervisor_stack, 1025)"],
    ["non-HIL documentation", DOCS_INDEX, "Local\nunit tests and CI gates are not HIL", "Local unit tests and CI gates are real-board HIL"],
  ];

  for (const [name, relative, replace, replacement, all] of cases) {
    await t.test(name, () => {
      const applyMutation = all ? mutationAll : mutation;
      const result = checkRepositoryGateContents(applyMutation(baseline, relative, replace, replacement));
      assert.equal(result.ok, false);
      assert.ok(result.failures.some((failure) => failure.code === "G18"));
    });
  }
});
