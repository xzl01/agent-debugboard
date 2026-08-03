import assert from "node:assert/strict";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";
import { checkNightlyWorkflow, formatFailures } from "./check-nightly-workflow.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const WORKFLOW_PATH = ".github/workflows/nightly.yml";
const PAYLOADS = ["radxa-linkr-debugger-rp2350.uf2", "radxa-linkr-debugger-rp2350-ota.bin", "radxa-linkr-debugger-rp2350.elf", "radxa-linkr-debugger-rp2350.map", "radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz", "radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz", "radxa-linkr-debuggerctl-rust_windows_amd64.zip", "skills-radxa-linkr-debugger.tar.gz"];
const ASSETS = [...PAYLOADS, "SHA256SUMS.txt"];
const LEGACY_SCHEDULE_MANUAL_TRIGGER = `on:
  schedule:
    - cron: "0 3 * * *"
  workflow_dispatch:
`;
const DEV_PUSH_TRIGGER = `on:
  push:
    branches:
      - dev
`;
const DEV_PUSH_AND_SCHEDULE_TRIGGER = `on:
  push:
    branches:
      - dev
  schedule:
    - cron: "0 3 * * *"
`;
const DEV_PUSH_AND_WORKFLOW_DISPATCH_TRIGGER = `on:
  push:
    branches:
      - dev
  workflow_dispatch:
`;
const BARE_PUSH_TRIGGER = `on:
  push:
`;
const MAIN_ONLY_PUSH_TRIGGER = `on:
  push:
    branches:
      - main
`;
const DEV_AND_MAIN_PUSH_TRIGGER = `on:
  push:
    branches:
      - dev
      - main
`;
const MANIFEST = `expected_assets=(${ASSETS.join(" ")})`;
const BASELINE = await readFile(path.join(ROOT, WORKFLOW_PATH), "utf8");
const CRLF_BASELINE = BASELINE.replace(/\n/g, "\r\n");

function pureLoc(source) {
  return source.split(/\r?\n/).filter((line) => {
    const trimmed = line.trim();
    return trimmed !== "" && !trimmed.startsWith("//") && !trimmed.startsWith("/*") && !trimmed.startsWith("*");
  }).length;
}
const READMES = Object.fromEntries(await Promise.all(["README.md", "README.zh-CN.md"].map(async (relativePath) => [relativePath, await readFile(path.join(ROOT, relativePath), "utf8")] )));

function replaceOnce(source, before, after) {
  const index = source.indexOf(before);
  assert.notEqual(index, -1, `missing mutation target: ${before}`);
  assert.equal(index, source.lastIndexOf(before), `ambiguous mutation target: ${before}`);
  return `${source.slice(0, index)}${after}${source.slice(index + before.length)}`;
}

function replaceRootTrigger(source, trigger) {
  const normalizedSource = source.replace(/\r\n?/g, "\n");
  const normalizedTrigger = trigger.replace(/\r\n?/g, "\n");
  assert.ok(normalizedTrigger.startsWith("on:\n"), "trigger must include the root on: block");
  assert.ok(normalizedTrigger.endsWith("\n"), "trigger block must end with a newline");
  const permissionsIndex = normalizedSource.indexOf("\npermissions:\n");
  assert.notEqual(permissionsIndex, -1, "missing permissions block");
  const rootSection = normalizedSource.slice(0, permissionsIndex);
  const rootMatches = [...rootSection.matchAll(/^on:\s*$/gm)];
  assert.equal(rootMatches.length, 1, "root trigger block must be unique");
  const rootIndex = rootMatches[0].index ?? -1;
  assert.notEqual(rootIndex, -1, "missing root trigger block");
  assert.ok(rootIndex < permissionsIndex, "root trigger block must precede permissions");
  return `${normalizedSource.slice(0, rootIndex)}${normalizedTrigger}${normalizedSource.slice(permissionsIndex)}`;
}

async function withRepo(workflow, operation) {
  const root = await mkdtemp(path.join(os.tmpdir(), "nightly-workflow-"));
  try {
    await Promise.all(Object.entries({ ...READMES, [WORKFLOW_PATH]: workflow }).map(async ([relativePath, content]) => {
      await mkdir(path.dirname(path.join(root, relativePath)), { recursive: true });
      await writeFile(path.join(root, relativePath), content);
    }));
    await operation(root);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
}

test("guard 1/4: checker modules stay within the source-size contract", async () => {
  for (const relativePath of ["scripts/check-nightly-workflow.mjs", "scripts/check-nightly-workflow.test.mjs"]) assert.ok(pureLoc(await readFile(path.join(ROOT, relativePath), "utf8")) <= 250, relativePath);
});

test("guard 2/4: current nightly workflow baseline passes", async () => {
  await withRepo(BASELINE, async (root) => assert.equal((await checkNightlyWorkflow(root)).ok, true));
});

test("guard 3/4: exact dev-only trigger contract", async () => {
  await withRepo(replaceRootTrigger(BASELINE, DEV_PUSH_TRIGGER), async (root) => {
    const result = await checkNightlyWorkflow(root);
    assert.equal(result.ok, true, formatFailures(result.failures));
  });
});

test("guard 4/4: CRLF baseline survives root-trigger replacement", async () => {
  await withRepo(replaceRootTrigger(CRLF_BASELINE, DEV_PUSH_TRIGGER), async (root) => {
    const result = await checkNightlyWorkflow(root);
    assert.equal(result.ok, true, formatFailures(result.failures));
  });
});

const TRIGGER_MUTATIONS = [
  ["rejects legacy schedule+workflow_dispatch trigger", LEGACY_SCHEDULE_MANUAL_TRIGGER],
  ["rejects dev+schedule trigger", DEV_PUSH_AND_SCHEDULE_TRIGGER],
  ["rejects dev+workflow_dispatch trigger", DEV_PUSH_AND_WORKFLOW_DISPATCH_TRIGGER],
  ["rejects bare unfiltered push trigger", BARE_PUSH_TRIGGER],
  ["rejects main-only push trigger", MAIN_ONLY_PUSH_TRIGGER],
  ["rejects dev+main push trigger", DEV_AND_MAIN_PUSH_TRIGGER],
];

const ROOT_TRIGGER_REGRESSIONS = [
  ["rejects missing root on block", "W01", replaceOnce(BASELINE, DEV_PUSH_TRIGGER, "")],
  ["rejects two root on blocks before permissions", "W01", replaceOnce(BASELINE, DEV_PUSH_TRIGGER, `${DEV_PUSH_TRIGGER}${MAIN_ONLY_PUSH_TRIGGER}`)],
  ["rejects second root on block after permissions", "W01", replaceOnce(replaceRootTrigger(BASELINE, DEV_PUSH_TRIGGER), "\npermissions:\n  contents: read\n", `\npermissions:\n  contents: read\n\n${MAIN_ONLY_PUSH_TRIGGER}`)],
];

const MUTATIONS = [
  ["rejects bypassing the complete validation workflow", "W04", "    uses: ./.github/workflows/build.yml", "    uses: ./.github/workflows/other.yml"],
  ["rejects an unlocked nightly CLI build", "W14", "cargo build --locked --release --manifest-path", "cargo build --release --manifest-path"],
  ["rejects the old multi-path artifact root", "W09", "          path: app/dist", "          path: |\n            app/dist/release\n            app/dist/release-notes.md"],
  ["rejects a non-draft candidate release", "W10", "-F draft=true", "-F draft=false"],
  ["rejects deleting the public nightly during candidate preparation", "W10", "          CANDIDATE_TAG=\"nightly-candidate-$GITHUB_RUN_ID\"\n          candidate_rows=", "          CANDIDATE_TAG=\"nightly-candidate-$GITHUB_RUN_ID\"\n          gh release delete nightly --repo \"$GITHUB_REPOSITORY\" --yes --cleanup-tag\n          candidate_rows="],
  ["rejects missing remote checksum verification", "W10", "          (cd \"$verify_dir\" && sha256sum -c SHA256SUMS.txt)\n", ""],
  ["rejects missing remote manifest", "W08", `          ${MANIFEST}\n          gh release upload \"$CANDIDATE_TAG\"`, "          gh release upload \"$CANDIDATE_TAG\""],
  ["rejects omitted first payload upload", "W10", `bundle/release/${PAYLOADS[0]}`, ""],
  ["rejects incomplete final-readback manifest", "W10", `test "$tag_sha" = "$GITHUB_SHA"\n          ${MANIFEST}\n          final_state=`, `test "$tag_sha" = "$GITHUB_SHA"\n          expected_assets=(${ASSETS.slice(0, -1).join(" ")})\n          final_state=`],
  ["rejects incomplete checksum generation", "W08", "skills-radxa-linkr-debugger.tar.gz > SHA256SUMS.txt", "> SHA256SUMS.txt"],
  ["rejects gh api process substitutions", "W10", "candidate_rows=\"$(gh api --paginate \"repos/$GITHUB_REPOSITORY/releases?per_page=100\" --jq \".[] | select(.tag_name == \\\"$CANDIDATE_TAG\\\")", "mapfile -t candidate_rows < <(gh api --paginate \"repos/$GITHUB_REPOSITORY/releases?per_page=100\" --jq \".[] | select(.tag_name == \\\"$CANDIDATE_TAG\\\")"],
  ["rejects deleting nightly before candidate verification", "W10", "          remote_asset_rows=", "          gh release delete nightly --repo \"$GITHUB_REPOSITORY\" --yes --cleanup-tag\n          remote_asset_rows="],
  ["rejects promotion without cleaning the old tag", "W10", " --yes --cleanup-tag", " --yes"],
  ["rejects promotion without renaming the verified candidate", "W10", " --tag nightly --target", " --target"],
  ["rejects candidate cleanup that is not unconditional", "W10", "        if: ${{ always() }}", "        if: ${{ success() }}"],
  ["rejects candidate cleanup without a draft guard", "W10", "          if [ \"$candidate_draft\" != true ]; then", "          if false; then"],
  ["rejects direct tag SHA assignment", "W10", `tag_sha="$(gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/nightly" --jq .object.sha)"`, "tag_sha=\"$GITHUB_SHA\""],
  ["rejects git-ref final-state readback", "W10", `final_state="$(gh api "repos/$GITHUB_REPOSITORY/releases/$CANDIDATE_RELEASE_ID")"`, `final_state="$(gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/nightly")"`],
  ["rejects release-level asset download", "W10", "repos/$GITHUB_REPOSITORY/releases/assets/${asset_ids[$expected]}", "repos/$GITHUB_REPOSITORY/releases/$NIGHTLY_RELEASE_ID"],
  ["rejects clobber on staged upload", "W11", `            --repo "$GITHUB_REPOSITORY"
          gh release upload "$CANDIDATE_TAG" bundle/release/SHA256SUMS.txt`, `            --clobber --repo "$GITHUB_REPOSITORY"
          gh release upload "$CANDIDATE_TAG" bundle/release/SHA256SUMS.txt`],
  ["rejects disabled candidate duplicate rejection", "W12", "if [[ \"$candidate_rows\" == *$'\\n'* ]]; then\n            printf 'duplicate nightly candidate releases", "if false; then\n            printf 'duplicate nightly candidate releases"],
];

for (const [index, [name, workflow]] of TRIGGER_MUTATIONS.entries()) {
  test(`trigger ${index + 1}/${TRIGGER_MUTATIONS.length}: ${name}`, async () => {
    await withRepo(replaceRootTrigger(BASELINE, workflow), async (root) => {
      const result = await checkNightlyWorkflow(root);
      assert.ok(result.failures.some((failure) => failure.code === "W01"), formatFailures(result.failures));
    });
  });
}

for (const [index, [name, code, workflow]] of ROOT_TRIGGER_REGRESSIONS.entries()) {
  test(`root trigger ${index + 1}/${ROOT_TRIGGER_REGRESSIONS.length}: ${name}`, async () => {
    await withRepo(workflow, async (root) => {
      const result = await checkNightlyWorkflow(root);
      assert.ok(result.failures.some((failure) => failure.code === code), formatFailures(result.failures));
    });
  });
}

for (const [index, [name, code, before, after]] of MUTATIONS.entries()) {
  test(`mutation ${index + 1}/${MUTATIONS.length}: ${name}`, async () => {
    await withRepo(replaceOnce(BASELINE, before, after), async (root) => {
      const result = await checkNightlyWorkflow(root);
      assert.ok(result.failures.some((failure) => failure.code === code), formatFailures(result.failures));
    });
  });
}
