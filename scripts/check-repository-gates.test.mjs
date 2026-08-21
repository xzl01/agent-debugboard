import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { POLICY_FILES, checkRepositoryGateContents } from "./check-repository-gates.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

async function repositoryContents() {
  return new Map(await Promise.all(POLICY_FILES.map(async (relative) => [
    relative,
    await readFile(path.join(ROOT, relative), "utf8"),
  ])));
}

function mutation(contents, relative, replace, replacement = "") {
  const changed = new Map(contents);
  const original = changed.get(relative);
  assert.ok(original.includes(replace), `mutation marker missing: ${relative}: ${replace}`);
  changed.set(relative, original.replace(replace, replacement));
  return changed;
}

test("accepts the complete repository gate contract", async () => {
  assert.deepEqual(checkRepositoryGateContents(await repositoryContents()), { ok: true, failures: [] });
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
