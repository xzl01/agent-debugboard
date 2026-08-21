import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

export const POLICY_FILES = Object.freeze([
  ".github/workflows/build.yml",
  ".github/workflows/pages.yml",
  ".github/workflows/release.yml",
  ".github/workflows/nightly.yml",
  "AGENTS.md",
  "web/package.json",
  "web/package-lock.json",
]);

const REQUIRED_GATE_NEEDS = Object.freeze([
  "version-gate", "decoder", "nix", "web", "desktop-release", "scripts", "windows-installer",
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

export function checkRepositoryGateContents(contents) {
  const failures = [];
  const build = contents.get(".github/workflows/build.yml") ?? "";
  const pages = contents.get(".github/workflows/pages.yml") ?? "";
  const release = contents.get(".github/workflows/release.yml") ?? "";
  const nightly = contents.get(".github/workflows/nightly.yml") ?? "";
  const agents = contents.get("AGENTS.md") ?? "";
  const webPackage = contents.get("web/package.json") ?? "";
  const webPackageLock = contents.get("web/package-lock.json") ?? "";

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
  if (!reusableValidation(pages) || !exact(needs(job(pages, "build")), ["validation"])
      || !exact(needs(job(pages, "deploy")), ["build"])) {
    fail(failures, "G04", ".github/workflows/pages.yml", "Pages must validate, build, then deploy in order");
  }
  if (!reusableValidation(release) || !needs(job(release, "rust-cli-release")).includes("validation")
      || !needs(job(release, "host-desktop-release")).includes("validation")
      || !needs(job(release, "release")).includes("rust-cli-release")
      || !needs(job(release, "release")).includes("host-desktop-release")) {
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
