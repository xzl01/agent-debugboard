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
  ".github/workflows/version-bump.yml",
  "AGENTS.md",
  "apps/radxa_linkr_debugger/prj.conf",
  "apps/radxa_linkr_debugger/src/linkr_debugger_http.c",
  "apps/radxa_linkr_debugger/src/linkr_debugger_ws.c",
  "docs/testing/hil-functional-test-spec.md",
  "skills/radxa-linkr-debugger/scripts/web-ota-hil.sh",
  "Makefile",
  "flake.nix",
  "nix/openocd-latest.nix",
  "nix/overlay.nix",
  "shell.nix",
  "web/package.json",
  "docs/developer/build.md",
  "docs/developer/build.zh-CN.md",
]);

const REQUIRED_GATE_NEEDS = Object.freeze([
  "source", "version-gate", "decoder", "nix", "web", "scripts", "windows-installer",
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

export function checkRepositoryGateContents(contents) {
  const failures = [];
  const build = contents.get(".github/workflows/build.yml") ?? "";
  const pages = contents.get(".github/workflows/pages.yml") ?? "";
  const release = contents.get(".github/workflows/release.yml") ?? "";
  const nightly = contents.get(".github/workflows/nightly.yml") ?? "";
  const versionBump = contents.get(".github/workflows/version-bump.yml") ?? "";
  const agents = contents.get("AGENTS.md") ?? "";
  const makefile = contents.get("Makefile") ?? "";
  const flake = contents.get("flake.nix") ?? "";
  const openocd = contents.get("nix/openocd-latest.nix") ?? "";
  const overlay = contents.get("nix/overlay.nix") ?? "";
  const shell = contents.get("shell.nix") ?? "";
  const webPackage = contents.get("web/package.json") ?? "";
  const buildEn = contents.get("docs/developer/build.md") ?? "";
  const buildZh = contents.get("docs/developer/build.zh-CN.md") ?? "";
  const statusSource = contents.get("apps/radxa_linkr_debugger/src/linkr_debugger_http.c") ?? "";
  const wsSource = contents.get("apps/radxa_linkr_debugger/src/linkr_debugger_ws.c") ?? "";
  const hilSpec = contents.get("docs/testing/hil-functional-test-spec.md") ?? "";
  const prjConfig = contents.get("apps/radxa_linkr_debugger/prj.conf") ?? "";
  const webOtaHil = contents.get("skills/radxa-linkr-debugger/scripts/web-ota-hil.sh") ?? "";

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
  if (!reusableValidation(release) || !needs(job(release, "rust-cli-release")).includes("validation")
      || !needs(job(release, "release")).includes("rust-cli-release")) {
    fail(failures, "G05", ".github/workflows/release.yml", "release publication must transitively depend on validation");
  }
  if (!reusableValidation(nightly) || !exact(needs(job(nightly, "rust-cli-release")), ["validation"])
      || !exact(needs(job(nightly, "nightly-assets")), ["rust-cli-release"])
      || !exact(needs(job(nightly, "publish-nightly")), ["nightly-assets"])) {
    fail(failures, "G06", ".github/workflows/nightly.yml", "nightly publication must follow the validated job chain");
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
  if (!checkWorkflowContracts({ build, release, pages, versionBump, makefile })) {
    fail(failures, "G12", ".github/workflows/{build,release,pages,version-bump}.yml + Makefile", "source selection, release lineage, action pins, and permanent gates must be immutable");
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
    fail(failures, "G17", "skills/radxa-linkr-debugger/scripts/web-ota-hil.sh", "negative full-body OTA uploads must use UPLOAD_TIMEOUT while status and OTA control requests use SHORT_TIMEOUT");
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
