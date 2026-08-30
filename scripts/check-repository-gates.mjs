import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { checkHilContracts } from "./repository-gates/hil-contracts.mjs";
import { checkWorkflowContracts } from "./repository-gates/workflow-contracts.mjs";

export const POLICY_FILES = Object.freeze([
  ".github/workflows/build.yml",
  ".github/workflows/pages.yml",
  ".github/workflows/release.yml",
  ".github/workflows/nightly.yml",
  ".github/workflows/native-packages.yml",
  ".github/workflows/version-bump.yml",
  "AGENTS.md",
  "apps/radxa_linkr_debugger/CMakeLists.txt",
  "apps/radxa_linkr_debugger/prj.conf",
  "apps/radxa_linkr_debugger/sections-ram.ld",
  "apps/radxa_linkr_debugger/src/linkr_debugger_capture_arena.c",
  "apps/radxa_linkr_debugger/src/linkr_debugger_capture_arena.h",
  "apps/radxa_linkr_debugger/src/linkr_debugger_control.c",
  "apps/radxa_linkr_debugger/src/linkr_debugger_http.c",
  "apps/radxa_linkr_debugger/src/linkr_debugger_logic_analyzer.c",
  "apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.c",
  "apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.h",
  "apps/radxa_linkr_debugger/src/linkr_debugger_ws.c",
  "apps/radxa_linkr_debugger/src/linkr_debugger_ws.h",
  "docs/README.md",
  "docs/reference/logic-analyzer.md",
  "docs/testing/hil-functional-test-spec.md",
  "docs/testing/scripts/web-ota-hil.sh",
  "Makefile",
  "flake.nix",
  "nix/openocd-latest.nix",
  "nix/overlay.nix",
  "shell.nix",
  "web/package.json",
  "web/package-lock.json",
  "docs/developer/build.md",
  "docs/developer/build.zh-CN.md",
  "debian/control",
  "debian/rules",
  "packaging/redhat/radxa-linkr-debugger.spec",
  "packaging/archlinux/PKGBUILD",
]);

const REQUIRED_GATE_NEEDS = Object.freeze([
  "source", "version-gate", "decoder", "nix", "web", "desktop-release", "scripts", "windows-installer",
  "firmware", "host-cli", "host-tools", "skill-bundle",
]);

function job(workflow, name) {
  const body = workflow.slice(Math.max(0, workflow.search(/^jobs:\s*$/m)));
  const headers = [...body.matchAll(/^  ([a-z0-9-]+):\s*$/gm)];
  const index = headers.findIndex((header) => header[1] === name);
  if (index < 0) return "";
  return body.slice(headers[index].index, headers[index + 1]?.index);
}

function needs(jobBody) {
  const scalar = jobBody.match(/^    needs:[ \t]*([a-z0-9-]+)[ \t]*$/m);
  if (scalar) return [scalar[1]];
  const block = jobBody.match(/^    needs:[ \t]*\n((?:      - [a-z0-9-]+[ \t]*\n?)+)/m)?.[1] ?? "";
  return [...block.matchAll(/^      - ([a-z0-9-]+)[ \t]*$/gm)].map((match) => match[1]);
}

function exact(actual, expected) {
  return actual.length === expected.length && [...actual].sort().every((value, index) => value === [...expected].sort()[index]);
}

function reusableValidation(workflow) {
  return /^    uses:\s*\.\/\.github\/workflows\/build\.yml\s*$/m.test(job(workflow, "validation"));
}

function fail(failures, code, surface, detail) {
  failures.push({ code, surface, detail });
}

function flakeOverlayContract(english, chinese) {
  const flakeInput = /\bagent-debugboard\.url\s*=\s*"github:xzl01\/agent-debugboard"/;
  const flakeOutput = /\boutputs\s*=[^{]*\{[^}]*\bagent-debugboard\b/;
  const overlayApplication = /overlays\s*=\s*\[\s*agent-debugboard\.overlays\.default\s*\]/;
  const systemRef = /\bsystem\b/;
  const legacy = /\(import github:xzl01\/agent-debugboard\)\.overlays\.default/;
  for (const doc of [english, chinese]) {
    if (!flakeInput.test(doc)) return false;
    if (!flakeOutput.test(doc)) return false;
    if (!overlayApplication.test(doc)) return false;
    if (!systemRef.test(doc)) return false;
    if (legacy.test(doc)) return false;
  }
  return true;
}

function hasOnlyConfigValue(source, key, value) {
  const assignments = [...source.matchAll(new RegExp(`^${key}=([^\\r\\n#]+)`, "gm"))];
  return assignments.length === 1 && assignments[0][1].trim() === value;
}

function firmwareMemoryCaptureContract({
  appCmake,
  sectionsRam,
  captureArenaHeader,
  captureArenaSource,
  controlSource,
  logicAnalyzerSource,
  logicAnalyzerDoc,
  sigrokHeader,
  sigrokSource,
  wsHeader,
  wsSource,
  prjConfig,
  docsIndex,
}) {
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
  const captureArena = /#define\s+LINKR_DEBUGGER_CAPTURE_ARENA_ALIGN\s+32768U\b/.test(captureArenaHeader)
    && /#define\s+LINKR_DEBUGGER_CAPTURE_ARENA_WS_SAMPLE_RING_BYTES\s+30720U\b/.test(captureArenaHeader)
    && /#define\s+LINKR_DEBUGGER_CAPTURE_ARENA_POWER_CAPTURE_BYTES\s+65672U\b/.test(captureArenaHeader)
    && /#define\s+LINKR_DEBUGGER_CAPTURE_ARENA_SIGROK_WS_POOL_BYTES\s+16624U\b/.test(captureArenaHeader)
    && /#define\s+LINKR_DEBUGGER_CAPTURE_ARENA_BURST_TOTAL_BYTES\s+144184U\b/.test(captureArenaHeader)
    && /LINKR_DEBUGGER_CAPTURE_ARENA_BYTES\s*\\?[\s\S]*LINKR_DEBUGGER_CAPTURE_ARENA_NORMAL_BYTES/.test(captureArenaHeader)
    && /static\s+uint8_t\s+linkr_debugger_capture_arena\s*\[\s*LINKR_DEBUGGER_CAPTURE_ARENA_BYTES\s*\]\s*__aligned\(\s*LINKR_DEBUGGER_CAPTURE_ARENA_ALIGN\s*\)/.test(captureArenaSource)
    && /max\(normal,\s*burst\)=148856 B/.test(logicAnalyzerDoc);
  const linker = /zephyr_linker_sources\(\s*RAM_SECTIONS\s+SORT_KEY\s+0\s+sections-ram\.ld\s*\)/.test(appCmake)
    && /SECTION_PROLOGUE\(\s*\.bss\.pre_capture\s*,\s*\(\s*NOLOAD\s*\)[\s\S]*?KEEP\(\s*\*\(\s*\.bss\.pre_capture\.sigrok_runtime\s*\)\s*\)[\s\S]*?KEEP\(\s*\*\(\s*\.bss\.pre_capture\.ws_clients\s*\)\s*\)[\s\S]*?GROUP_NOLOAD_LINK_IN\(\s*RAMABLE_REGION\s*,\s*RAMABLE_REGION\s*\)[\s\S]*?ASSERT\(\s*SIZEOF\(\s*\.bss\.pre_capture\s*\)\s*==\s*0xDA40/.test(sectionsRam)
    && /ASSERT\(\s*__data_region_end\s*<=\s*0x20010000/.test(sectionsRam)
    && /ASSERT\(\s*__bss_start\s*==\s*0x20010000/.test(sectionsRam)
    && /ASSERT\(\s*\(\s*__bss_start\s*&\s*0x7fff\s*\)\s*==\s*0/.test(sectionsRam);
  const sigrok = /#define\s+LINKR_DEBUGGER_SIGROK_LINKR_RING_BUFFER_BYTES\s+32768U\b/.test(sigrokHeader)
    && /#define\s+LINKR_DEBUGGER_SIGROK_LINKR_STREAM_QDEPTH_LIMIT\s+32U\b/.test(sigrokHeader)
    && /#define\s+LINKR_DEBUGGER_SIGROK_LINKR_WS_DATA_SLOT_COUNT\s+4U\b/.test(sigrokHeader)
    && /#define\s+LINKR_DEBUGGER_SIGROK_LINKR_WS_DATA_PAYLOAD_BYTES\s*\\?[\s\S]{0,80}LINKR_DEBUGGER_SIGROK_LINKR_MAX_DATA_BYTES/.test(sigrokHeader)
    && /#define\s+LINKR_DEBUGGER_SIGROK_LINKR_WS_TERMINAL_SLOT_COUNT\s+1U\b/.test(sigrokHeader)
    && /#define\s+LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_SLOT_COUNT\s+12U\b/.test(sigrokHeader)
    && /#define\s+LINKR_DEBUGGER_SIGROK_LINKR_RAW_BURST_QUEUE_MEMORY_LIMIT_BYTES\s+49152U\b/.test(sigrokHeader)
    && /LINKR_DEBUGGER_SIGROK_LINKR_COMPRESSION_PACKED_PALETTE2\s*=\s*6\b/.test(sigrokHeader)
    && /#define\s+LINKR_DEBUGGER_SIGROK_LINKR_TCP_STREAM_QDEPTH_LIMIT\s+10U\b/.test(sigrokSource)
    && /Z_GENERIC_SECTION\(\s*\.bss\.pre_capture\.sigrok_runtime\s*\)/.test(sigrokSource)
    && !/section\(\s*"\.bss\.pre_capture\.sigrok_runtime"\s*\)/.test(sigrokSource)
    && /BUILD_ASSERT\(\s*sizeof\(\s*linkr_debugger_sigrok_linkr_runtime\s*\)\s*==\s*27168U\s*\)/.test(sigrokSource)
    && /memset\(\s*&linkr_debugger_sigrok_linkr_runtime\s*,\s*0\s*,\s*sizeof\(\s*linkr_debugger_sigrok_linkr_runtime\s*\)\s*\)\s*;[\s\S]*?linkr_debugger_sigrok_linkr_runtime\.listen_fd\s*=\s*-1\s*;[\s\S]*?linkr_debugger_sigrok_linkr_runtime\.client_fd\s*=\s*-1\s*;[\s\S]*?linkr_debugger_sigrok_linkr_runtime\.next_sequence_id\s*=\s*1U\s*;[\s\S]*?k_(?:fifo|mutex|sem)_init\(/.test(sigrokSource)
    && /K_THREAD_STACK_DEFINE\(\s*server_stack\s*,\s*2048U\s*\)/.test(sigrokSource);
  const ws = /#define\s+LINKR_DEBUGGER_WS_MAX_CLIENTS\s+4\b/.test(wsHeader)
    && /#define\s+LINKR_DEBUGGER_WS_SEND_BUFFER_SIZE\s+6144\b/.test(wsSource)
    && /Z_GENERIC_SECTION\(\s*\.bss\.pre_capture\.ws_clients\s*\)/.test(wsSource)
    && !/section\(\s*"\.bss\.pre_capture\.ws_clients"\s*\)/.test(wsSource)
    && /BUILD_ASSERT\(\s*sizeof\(\s*linkr_debugger_ws_clients\s*\)\s*==\s*28704U\s*\)/.test(wsSource)
    && /memset\(\s*linkr_debugger_ws_clients\s*,\s*0\s*,\s*sizeof\(\s*linkr_debugger_ws_clients\s*\)\s*\)\s*;[\s\S]*?k_mutex_init\(/.test(wsSource)
    && /K_THREAD_STACK_DEFINE\(\s*linkr_debugger_adc_sampler_stack\s*,\s*2048\s*\)/.test(wsSource);
  const stacks = /#define\s+LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE\s+2048U\b/.test(logicAnalyzerSource)
    && /K_THREAD_STACK_DEFINE\(\s*la_stream_ring_thread_stack\s*,\s*LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE\s*\)/.test(logicAnalyzerSource)
    && /K_THREAD_STACK_DEFINE\(\s*la_stream_ring_consumer_thread_stack\s*,\s*LINKR_DEBUGGER_LA_STREAM_THREAD_STACK_SIZE\s*\)/.test(logicAnalyzerSource)
    && /K_THREAD_STACK_DEFINE\(\s*linkr_debugger_watchdog_supervisor_stack\s*,\s*1024\s*\)/.test(controlSource);
  return captureArena && linker && sigrok && ws && stacks
    && configValues.every(([key, value]) => hasOnlyConfigValue(prjConfig, key, value))
    && /Local\s+unit tests and CI gates are not HIL/.test(docsIndex);
}

function nativePackageReleaseContract({ release, nightly, nativePackages, debianControl, debianRules, rpmSpec, pkgbuild }) {
  const firmwareFiles = [
    "radxa-linkr-debugger-rp2350.uf2",
    "radxa-linkr-debugger-rp2350-ota.bin",
  ];
  const recipes = [debianRules, rpmSpec, pkgbuild];
  const twoPackages = debianControl.includes("Package: radxa-linkr-debuggerctl")
    && debianControl.includes("Package: radxa-linkr-debugger-firmware")
    && rpmSpec.includes("%package -n radxa-linkr-debugger-firmware")
    && pkgbuild.includes("pkgname=('radxa-linkr-debuggerctl' 'radxa-linkr-debugger-firmware')");
  const payloads = recipes.every((recipe) => firmwareFiles.every((name) => recipe.includes(name)))
    && recipes.every((recipe) => /test ! -e .*zephyr[.]uf2/.test(recipe));
  const noDesktopPayload = recipes.every((recipe) => !/(?:web[/]dist|linkr-host|linkr-tray|radxa-linkr-debugger[.]desktop)/.test(recipe));
  const nativeTools = nativePackages.includes("dpkg-buildpackage --build=binary --no-sign")
    && nativePackages.includes("rpmbuild -bb")
    && nativePackages.includes("makepkg --cleanbuild --noconfirm");
  const compatibilityBaselines = /debian:bookworm@sha256:[0-9a-f]{64}/.test(nativePackages)
    && /almalinux:9@sha256:[0-9a-f]{64}/.test(nativePackages)
    && /archlinux:latest@sha256:[0-9a-f]{64}/.test(nativePackages);
  const sourceNaming = rpmSpec.includes("agent-debugboard-%{upstream_version}.tar.gz")
    && nativePackages.includes("native-package-sources/agent-debugboard-$version.tar.gz")
    && [release, nightly].every((workflow) => workflow.includes("agent-debugboard-*.tar.gz"));
  const archiveStart = nativePackages.indexOf("git -C app archive --format=tar.gz");
  const archiveRevision = nativePackages.indexOf('"$SOURCE_SHA"', archiveStart);
  const sourceLineage = archiveStart >= 0 && archiveRevision > archiveStart
    && archiveRevision - archiveStart < 300;
  const checksumVariables = [
    "AGENT_DEBUGBOARD_SOURCE_SHA256",
    "AGENT_DEBUGBOARD_CLI_SHA256",
    "AGENT_DEBUGBOARD_UF2_SHA256",
    "AGENT_DEBUGBOARD_OTA_SHA256",
  ];
  const sourceChecksums = !pkgbuild.includes("SKIP")
    && checksumVariables.every((name) => pkgbuild.includes(name) && nativePackages.includes(name));
  const exactArchOutputs = pkgbuild.includes("options=('!debug')")
    && nativePackages.includes("find /home/builder/package -maxdepth 1 -name '*.pkg.tar.zst'");
  const releasePatterns = [
    "radxa-linkr-debuggerctl_*.deb",
    "radxa-linkr-debugger-firmware_*.deb",
    "radxa-linkr-debuggerctl-*.rpm",
    "radxa-linkr-debugger-firmware-*.rpm",
    "radxa-linkr-debuggerctl-*.pkg.tar.zst",
    "radxa-linkr-debugger-firmware-*.pkg.tar.zst",
  ];
  const releaseAssets = [release, nightly].every((workflow) => releasePatterns.every((pattern) => workflow.includes(pattern)));
  const sharedArtifact = /name:\s*native-release-packages/.test(nativePackages)
    && [release, nightly].every((workflow) => /name:\s*native-release-packages/.test(workflow)
      && /path:\s*native-package-assets/.test(workflow));
  return twoPackages && payloads && noDesktopPayload && nativeTools
    && compatibilityBaselines && sourceNaming && sourceLineage
    && sourceChecksums && exactArchOutputs && releaseAssets && sharedArtifact;
}

export function checkRepositoryGateContents(contents) {
  const failures = [];
  const build = contents.get(".github/workflows/build.yml") ?? "";
  const pages = contents.get(".github/workflows/pages.yml") ?? "";
  const release = contents.get(".github/workflows/release.yml") ?? "";
  const nightly = contents.get(".github/workflows/nightly.yml") ?? "";
  const nativePackages = contents.get(".github/workflows/native-packages.yml") ?? "";
  const versionBump = contents.get(".github/workflows/version-bump.yml") ?? "";
  const agents = contents.get("AGENTS.md") ?? "";
  const makefile = contents.get("Makefile") ?? "";
  const flake = contents.get("flake.nix") ?? "";
  const openocd = contents.get("nix/openocd-latest.nix") ?? "";
  const overlay = contents.get("nix/overlay.nix") ?? "";
  const shell = contents.get("shell.nix") ?? "";
  const webPackage = contents.get("web/package.json") ?? "";
  const webPackageLock = contents.get("web/package-lock.json") ?? "";
  const buildEn = contents.get("docs/developer/build.md") ?? "";
  const buildZh = contents.get("docs/developer/build.zh-CN.md") ?? "";
  const appCmake = contents.get("apps/radxa_linkr_debugger/CMakeLists.txt") ?? "";
  const sectionsRam = contents.get("apps/radxa_linkr_debugger/sections-ram.ld") ?? "";
  const captureArenaHeader = contents.get("apps/radxa_linkr_debugger/src/linkr_debugger_capture_arena.h") ?? "";
  const captureArenaSource = contents.get("apps/radxa_linkr_debugger/src/linkr_debugger_capture_arena.c") ?? "";
  const controlSource = contents.get("apps/radxa_linkr_debugger/src/linkr_debugger_control.c") ?? "";
  const logicAnalyzerSource = contents.get("apps/radxa_linkr_debugger/src/linkr_debugger_logic_analyzer.c") ?? "";
  const logicAnalyzerDoc = contents.get("docs/reference/logic-analyzer.md") ?? "";
  const sigrokSource = contents.get("apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.c") ?? "";
  const sigrokHeader = contents.get("apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.h") ?? "";
  const statusSource = contents.get("apps/radxa_linkr_debugger/src/linkr_debugger_http.c") ?? "";
  const wsHeader = contents.get("apps/radxa_linkr_debugger/src/linkr_debugger_ws.h") ?? "";
  const wsSource = contents.get("apps/radxa_linkr_debugger/src/linkr_debugger_ws.c") ?? "";
  const docsIndex = contents.get("docs/README.md") ?? "";
  const hilSpec = contents.get("docs/testing/hil-functional-test-spec.md") ?? "";
  const prjConfig = contents.get("apps/radxa_linkr_debugger/prj.conf") ?? "";
  const webOtaHil = contents.get("docs/testing/scripts/web-ota-hil.sh") ?? "";
  const debianControl = contents.get("debian/control") ?? "";
  const debianRules = contents.get("debian/rules") ?? "";
  const rpmSpec = contents.get("packaging/redhat/radxa-linkr-debugger.spec") ?? "";
  const pkgbuild = contents.get("packaging/archlinux/PKGBUILD") ?? "";

  if (!/^  pull_request:\s*$/m.test(build) || !/^  workflow_call:\s*$/m.test(build)) {
    fail(failures, "G01", ".github/workflows/build.yml", "complete validation must support pull_request and workflow_call");
  }
  const scripts = job(build, "scripts");
  if (!scripts.includes("check-repository-gates.test.mjs") || !scripts.includes("node scripts/check-repository-gates.mjs --root .")) {
    fail(failures, "G02", ".github/workflows/build.yml", "scripts job must run repository-gate contracts and checker");
  }
  const gate = job(build, "gate");
  if (!/^    name:\s*Required repository gate\s*$/m.test(gate)
      || !/^    if:\s*\$\{\{\s*always\(\)\s*\}\}\s*$/m.test(gate)
      || !exact(needs(gate), REQUIRED_GATE_NEEDS)
      || !/\.result == "success"/.test(gate)) {
    fail(failures, "G03", ".github/workflows/build.yml", "aggregate gate must require every validation job to succeed");
  }
  if (!reusableValidation(pages) || job(pages, "validation").includes("source_sha:")
      || !exact(needs(job(pages, "build")), ["validation"])
      || !exact(needs(job(pages, "deploy")), ["build"])) {
    fail(failures, "G04", ".github/workflows/pages.yml", "Pages must validate, build, then deploy in order");
  }
  const releaseNative = job(release, "native-packages");
  if (!reusableValidation(release) || !needs(job(release, "rust-cli-release")).includes("validation")
      || !needs(job(release, "host-desktop-release")).includes("validation")
      || !needs(releaseNative).includes("validation") || !needs(releaseNative).includes("rust-cli-release")
      || !/^    uses:\s*\.\/\.github\/workflows\/native-packages\.yml\s*$/m.test(releaseNative)
      || !releaseNative.includes("source_sha: ${{ needs.resolve.outputs.resolved_sha }}")
      || !needs(job(release, "release")).includes("rust-cli-release")
      || !needs(job(release, "release")).includes("host-desktop-release")
      || !needs(job(release, "release")).includes("native-packages")) {
    fail(failures, "G05", ".github/workflows/release.yml", "release publication must transitively depend on validation and the shared native package workflow");
  }
  const nightlyNative = job(nightly, "native-packages");
  if (!reusableValidation(nightly) || !exact(needs(job(nightly, "rust-cli-release")), ["validation"])
      || !exact(needs(nightlyNative), ["validation", "rust-cli-release"])
      || !/^    uses:\s*\.\/\.github\/workflows\/native-packages\.yml\s*$/m.test(nightlyNative)
      || !nightlyNative.includes("source_sha: ${{ github.sha }}")
      || !exact(needs(job(nightly, "nightly-assets")), ["native-packages"])
      || !exact(needs(job(nightly, "publish-nightly")), ["nightly-assets"])) {
    fail(failures, "G06", ".github/workflows/nightly.yml", "nightly publication must follow validation and the same native package workflow as tagged releases");
  }
  if (!/both `main` and `dev`[\s\S]*must require pull requests[\s\S]*reject direct pushes[\s\S]*`Required repository gate`/.test(agents)
      || !/block force pushes and branch\s+deletion/.test(agents)) {
    fail(failures, "G07", "AGENTS.md", "branch rules must require PRs, the aggregate gate, and destructive-operation protection");
  }
  try {
    if (JSON.parse(webPackage).scripts?.test !== "node scripts/run-tests.mjs") {
      fail(failures, "G08", "web/package.json", "Web tests must use the discovery runner");
    }
  } catch {
    fail(failures, "G08", "web/package.json", "package file must be valid JSON with the canonical test entry");
  }
  try {
    const lock = JSON.parse(webPackageLock);
    const nonRegistryPackages = Object.entries(lock.packages ?? {}).filter(([, metadata]) => {
      if (typeof metadata?.resolved !== "string") return false;
      try {
        return new URL(metadata.resolved).host !== "registry.npmjs.org";
      } catch {
        return true;
      }
    });
    if (nonRegistryPackages.length > 0) {
      fail(failures, "G11", "web/package-lock.json", "Web dependencies must resolve only from the canonical npm registry");
    }
  } catch {
    fail(failures, "G11", "web/package-lock.json", "Web package lock must be valid JSON");
  }
  const desktop = job(build, "desktop-release");
  const releaseDesktop = job(release, "host-desktop-release");
  if (!/ubuntu-latest/.test(desktop) || !/macos-latest/.test(desktop) || !/windows-latest/.test(desktop)
      || !/cargo build --locked --release --manifest-path host-tools\/Cargo\.toml/.test(desktop)
      || !/cargo build --locked --release --manifest-path cmd-ng\/Cargo\.toml/.test(desktop)
      || !/wasm-bindgen-cli --version 0\.2\.121 --locked/.test(desktop)
      || !/rustup target add wasm32-unknown-unknown/.test(desktop)
      || !/Verify bundled installer/.test(desktop) || !/Package desktop archive/.test(desktop)
      || !/wasm-bindgen-cli --version 0\.2\.121 --locked/.test(releaseDesktop)
      || !/rustup target add wasm32-unknown-unknown/.test(releaseDesktop)) {
    fail(failures, "G09", ".github/workflows/build.yml", "complete validation must preflight every formal desktop archive platform");
  }
  if (!["cargo", "rustc", "clippy", "rustfmt", "lld", "wasm-bindgen-cli"].every((tool) => new RegExp(`^\\s+pkgs\\.${tool}\\s*$`, "m").test(shell))) {
    fail(failures, "G09", "shell.nix", "classic Nix shell must provide Cargo, rustc, clippy, rustfmt, lld, and wasm-bindgen-cli for canonical Rust and embedded Web builds");
  }
  const openocdPackage = /openocd\.overrideAttrs/.test(openocd)
    && /owner\s*=\s*"openocd-org"/.test(openocd)
    && /repo\s*=\s*"openocd"/.test(openocd)
    && /rev\s*=\s*"da3920b0a52dc2d394afb222c688dac7e57acc1b"/.test(openocd)
    && /hash\s*=\s*"sha256-osZAASRIUDMbDhbH6lIuyx5KtKP7MYaj\+WlD6EWpIEo="/.test(openocd)
    && /version\s*=\s*"0\.12\.0\+dev-2026-07-28"/.test(openocd)
    && /autoreconfHook/.test(openocd)
    && /"--enable-ch347"/.test(openocd)
    && /adapter list/.test(openocd)
    && /\(\^\|\[\[:space:\]\]\)ch347\(\[\[:space:\]\]\|\$\)/.test(openocd);
  const exported = /openocd-latest\s*=\s*final\.callPackage\s+\.\/openocd-latest\.nix\s+\{\s*\};/.test(overlay)
    && /packages\s*=\s*\{\s*openocd-latest\s*=\s*pkgs\.openocd-latest;\s*radxa-linkr-debuggerctl\s*=/.test(flake)
    && /checks\s*=\s*\{\s*build\s*=\s*pkgs\.radxa-linkr-debuggerctl;\s*openocd-latest\s*=\s*pkgs\.openocd-latest;/.test(flake)
    && /openocdLatest\s*=\s*pkgs\.callPackage\s+\.\/nix\/openocd-latest\.nix\s+\{\s*\};/.test(shell)
    && /^\s+openocdLatest\s*$/m.test(shell);
  const formatted = /\.\/nix\/openocd-latest\.nix/.test(flake)
    && /\.\/shell\.nix/.test(flake)
    && /nixpkgs-fmt[\s\S]*openocd-latest\.nix[\s\S]*shell\.nix/.test(flake);
  if (!openocdPackage || !exported || !formatted) {
    fail(failures, "G10", "nix/openocd-latest.nix", "pinned CH347 OpenOCD must be the sole derivation, exported through overlay/flake, reused by shell.nix, and covered by formatting");
  }
  const makefileHeaders = [...makefile.matchAll(/^([A-Za-z0-9_.-]+):/gm)];
  const persistentDocsIndex = makefileHeaders.findIndex((header) => header[1] === "persistent-configuration-docs");
  const persistentDocs = persistentDocsIndex < 0 ? "" : makefile.slice(makefileHeaders[persistentDocsIndex].index, makefileHeaders[persistentDocsIndex + 1]?.index);
  if (!persistentDocs.includes("scripts/check-persistent-configuration-docs.test.mjs")
      || !persistentDocs.includes("node scripts/check-persistent-configuration-docs.mjs --root .")) {
    fail(failures, "G11", "Makefile", "persistent-configuration-docs must run its contract test and checker");
  }
  if (!checkWorkflowContracts({ build, release, nativePackages, pages, versionBump, makefile })) {
    fail(failures, "G12", ".github/workflows/{build,native-packages,release,pages,version-bump}.yml + Makefile", "source selection, release lineage, action pins, and permanent gates must be immutable");
  }
  if (!flakeOverlayContract(buildEn, buildZh)) {
    fail(failures, "G13", "docs/developer/build.{md,zh-CN.md}", "bilingual docs must use the flake inputs/outputs pattern and forbid the legacy import expression");
  }
  const hil = checkHilContracts({ hilSpec, prjConfig, statusSource, wsSource, webOtaHil });
  if (!hil.statusJson) {
    fail(failures, "G14", "apps/radxa_linkr_debugger/src/linkr_debugger_http.c + docs/testing/hil-functional-test-spec.md", "status JSON buffer must stay at 6144 bytes and the HIL spec must assert the same limit");
  }
  if (!hil.wsStatusSnapshots) {
    fail(failures, "G15", "apps/radxa_linkr_debugger/src/linkr_debugger_ws.c + docs/testing/hil-functional-test-spec.md", "WS status snapshot cadence must keep one initial snapshot, document event-driven follow-ups, and gate emission on LINKR_DEBUGGER_WS_EVENT_STATE");
  }
  if (!hil.captivePortalLocalOnlyDhcp) {
    fail(failures, "G16", "apps/radxa_linkr_debugger/prj.conf + docs/testing/hil-functional-test-spec.md", "captive-portal HIL must require DHCP option 114 while forbidding router/DNS advertisement and board-local wildcard DNS");
  }
  if (!hil.otaNegativeUploadTimeouts) {
    fail(failures, "G17", "docs/testing/scripts/web-ota-hil.sh", "negative full-body OTA uploads must use UPLOAD_TIMEOUT while status and OTA control requests use SHORT_TIMEOUT");
  }
  if (!firmwareMemoryCaptureContract({
    appCmake, sectionsRam, captureArenaHeader, captureArenaSource, controlSource,
    logicAnalyzerSource, logicAnalyzerDoc, sigrokHeader, sigrokSource, wsHeader,
    wsSource, prjConfig, docsIndex,
  })) {
    fail(failures, "G18", "apps/radxa_linkr_debugger/{CMakeLists.txt,sections-ram.ld,prj.conf,src/*} + docs/{README.md,reference/logic-analyzer.md}", "firmware memory and capture layout must retain approved capacities, pre-capture NOBITS initialization, stack targets, disabled features, and non-HIL documentation");
  }
  if (!nativePackageReleaseContract({ release, nightly, nativePackages, debianControl, debianRules, rpmSpec, pkgbuild })) {
    fail(failures, "G19", "debian/ + packaging/{redhat,archlinux}/ + .github/workflows/{native-packages,release,nightly}.yml", "each native packager must emit separate CLI and safe complete-firmware packages, exclude desktop/Web payloads, and publish all six package families");
  }
  if (!/release_notes="docs\/releases\/\$\{RELEASE_TAG%%-\*\}\.md"/.test(release)
      || !/release_flags=\(\)/.test(release)
      || !/\[\[ "\$RELEASE_TAG" == \*-\* \]\]/.test(release)
      || !/release_flags\+=\(--prerelease --latest=false\)/.test(release)
      || (release.match(/"\$\{release_flags\[@\]\}"/g) ?? []).length !== 2) {
    fail(failures, "G10", ".github/workflows/release.yml", "formal releases must use curated notes and mark prerelease tags as non-latest prereleases");
  }
  return { ok: failures.length === 0, failures };
}

export async function checkRepositoryGates(repositoryRoot) {
  const contents = new Map();
  const failures = [];
  for (const relative of POLICY_FILES) {
    try {
      contents.set(relative, await readFile(path.join(repositoryRoot, relative), "utf8"));
    } catch (error) {
      fail(failures, "file-missing", relative, error?.code === "ENOENT" ? "required file is missing" : String(error));
    }
  }
  const result = checkRepositoryGateContents(contents);
  return { ok: failures.length === 0 && result.ok, failures: [...failures, ...result.failures] };
}

export function formatFailures(failures) {
  return ["repository gate contract failed:", ...failures.map(({ code, surface, detail }) => `- [${code}] ${surface}: ${detail}`)].join("\n");
}

async function main() {
  const args = process.argv.slice(2);
  if (args.length !== 2 || args[0] !== "--root") {
    console.error("usage: node scripts/check-repository-gates.mjs --root <repository-root>");
    process.exitCode = 2;
    return;
  }
  const result = await checkRepositoryGates(path.resolve(args[1]));
  if (!result.ok) {
    console.error(formatFailures(result.failures));
    process.exitCode = 1;
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) await main();
