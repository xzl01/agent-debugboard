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
const MANIFEST = `expected_assets=(${ASSETS.join(" ")})`;
const BASELINE = await readFile(path.join(ROOT, WORKFLOW_PATH), "utf8");

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

test("guard 1/2: checker modules stay within the source-size contract", async () => {
  for (const relativePath of ["scripts/check-nightly-workflow.mjs", "scripts/check-nightly-workflow.test.mjs"]) assert.ok(pureLoc(await readFile(path.join(ROOT, relativePath), "utf8")) <= 250, relativePath);
});

test("guard 2/2: current nightly workflow baseline passes", async () => {
  await withRepo(BASELINE, async (root) => assert.equal((await checkNightlyWorkflow(root)).ok, true));
});

const MUTATIONS = [
  ["rejects the old multi-path artifact root", "W09", "          path: app/dist", "          path: |\n            app/dist/release\n            app/dist/release-notes.md"],
  ["rejects inverted published-release handling", "W10", "if [ \"$is_draft\" = false ]; then", "if [ \"$is_draft\" = true ]; then"],
  ["rejects missing remote checksum verification", "W10", "          (cd \"$verify_dir\" && sha256sum -c SHA256SUMS.txt)\n", ""],
  ["rejects missing remote manifest", "W08", `          ${MANIFEST}\n          old_asset_ids=`, "          old_asset_ids="],
  ["rejects omitted first payload upload", "W10", `bundle/release/${PAYLOADS[0]}`, ""],
  ["rejects incomplete final-readback manifest", "W10", `--draft=false\n          ${MANIFEST}\n          final_state=`, `--draft=false\n          expected_assets=(${ASSETS.slice(0, -1).join(" ")})\n          final_state=`],
  ["rejects incomplete checksum generation", "W08", "skills-radxa-linkr-debugger.tar.gz > SHA256SUMS.txt", "> SHA256SUMS.txt"],
  ["rejects gh api process substitutions", "W10", "release_rows=\"$(gh api --paginate", "mapfile -t release_rows < <(gh api --paginate"],
  ["rejects hiding a published release after its branch", "W10", `if [ "$is_draft" = false ]; then
              # existing published release
              gh release edit nightly --repo "$GITHUB_REPOSITORY" --draft
            else
              # retry draft
              :
            fi`, `if [ "$is_draft" = false ]; then
              # existing published release
              :
            else
              # retry draft
              :
            fi
            gh release edit nightly --repo "$GITHUB_REPOSITORY" --draft`],
  ["rejects undrafting before tag readback", "W10", `tag_sha="$(gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/nightly" --jq .object.sha)"
          test "$tag_sha" = "$GITHUB_SHA"
          gh release edit nightly --repo "$GITHUB_REPOSITORY" --title nightly --notes-file bundle/release-notes.md --prerelease --latest=false --draft=false`, `gh release edit nightly --repo "$GITHUB_REPOSITORY" --title nightly --notes-file bundle/release-notes.md --prerelease --latest=false --draft=false
          tag_sha="$(gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/nightly" --jq .object.sha)"
          test "$tag_sha" = "$GITHUB_SHA"`],
  ["rejects direct tag SHA assignment", "W10", `tag_sha="$(gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/nightly" --jq .object.sha)"`, "tag_sha=\"$GITHUB_SHA\""],
  ["rejects git-ref final-state readback", "W10", `final_state="$(gh api "repos/$GITHUB_REPOSITORY/releases/$NIGHTLY_RELEASE_ID")"`, `final_state="$(gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/nightly")"`],
  ["rejects release-level asset download", "W10", "repos/$GITHUB_REPOSITORY/releases/assets/${asset_ids[$expected]}", "repos/$GITHUB_REPOSITORY/releases/$NIGHTLY_RELEASE_ID"],
  ["rejects clobber on explicit payload upload", "W11", `            --repo "$GITHUB_REPOSITORY"
          gh release upload nightly bundle/release/SHA256SUMS.txt`, `            --clobber --repo "$GITHUB_REPOSITORY"
          gh release upload nightly bundle/release/SHA256SUMS.txt`],
  ["rejects disabled duplicate-release rejection", "W12", "if [[ \"$release_rows\" == *$'\\n'* ]]; then", "if false; then"],
  ["rejects comment-shadowed tag readback", "W10", `tag_sha="$(gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/nightly" --jq .object.sha)"`, `tag_sha="$GITHUB_SHA"
          # tag_sha="$(gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/nightly" --jq .object.sha)"`],
  ["rejects comment-shadowed final-state readback", "W10", `final_state="$(gh api "repos/$GITHUB_REPOSITORY/releases/$NIGHTLY_RELEASE_ID")"`, `final_state="$(gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/nightly")"
          # final_state="$(gh api "repos/$GITHUB_REPOSITORY/releases/$NIGHTLY_RELEASE_ID")"`],
  ["rejects comment-shadowed asset download", "W10", "for expected in \"${expected_assets[@]}\"; do gh api -H \"Accept: application/octet-stream\" \"repos/$GITHUB_REPOSITORY/releases/assets/${asset_ids[$expected]}\" > \"$verify_dir/$expected\"; done", "for expected in \"${expected_assets[@]}\"; do gh api -H \"Accept: application/octet-stream\" \"repos/$GITHUB_REPOSITORY/releases/$NIGHTLY_RELEASE_ID\" > \"$verify_dir/$expected\"; done\n          # for expected in \"${expected_assets[@]}\"; do gh api -H \"Accept: application/octet-stream\" \"repos/$GITHUB_REPOSITORY/releases/assets/${asset_ids[$expected]}\" > \"$verify_dir/$expected\"; done"],
];

for (const [index, [name, code, before, after]] of MUTATIONS.entries()) {
  test(`mutation ${index + 1}/${MUTATIONS.length}: ${name}`, async () => {
    await withRepo(replaceOnce(BASELINE, before, after), async (root) => {
      const result = await checkNightlyWorkflow(root);
      assert.ok(result.failures.some((failure) => failure.code === code), formatFailures(result.failures));
    });
  });
}
