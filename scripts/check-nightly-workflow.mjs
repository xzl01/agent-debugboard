import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const WORKFLOW = ".github/workflows/nightly.yml";
const ACTIONS = Object.freeze({ "actions/checkout": "d23441a48e516b6c34aea4fa41551a30e30af803", "actions/upload-artifact": "b7c566a772e6b6bfb58ed0dc250532a479d7789f", "actions/download-artifact": "018cc2cf5baa6db3ef3c5f8a56943fffe632ef53", "actions/setup-python": "ece7cb06caefa5fff74198d8649806c4678c61a1", "actions/setup-node": "49933ea5288caeca8642d1e84afbd3f7d6820020", "Swatinem/rust-cache": "e18b497796c12c097a38f9edb9d0641fb99eee32", "dtolnay/rust-toolchain": "4cda84d5c5c54efe2404f9d843567869ab1699d4", "zephyrproject-rtos/action-zephyr-setup": "66a907961072acaa85313d2e064e9f071141265a" });
const JOBS = ["rust-cli-release", "nightly-assets", "publish-nightly"];
const PAYLOADS = ["radxa-linkr-debugger-rp2350.uf2", "radxa-linkr-debugger-rp2350-ota.bin", "radxa-linkr-debugger-rp2350.elf", "radxa-linkr-debugger-rp2350.map", "radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz", "radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz", "radxa-linkr-debuggerctl-rust_windows_amd64.zip", "skills-radxa-linkr-debugger.tar.gz"];
const ASSETS = [...PAYLOADS, "SHA256SUMS.txt"];
const DEV_ONLY_TRIGGER = normalized(`on:
  push:
    branches:
      - dev
`);

function normalized(text) { return text.replace(/\r\n?/g, "\n"); }
function fail(failures, code, detail, surface = WORKFLOW) { failures.push({ code, surface, detail }); }
function exact(value, expected) {
  const names = value.replaceAll("\\", " ").trim().split(/\s+/).filter(Boolean).sort();
  const required = [...expected].sort();
  return names.length === required.length && names.every((name, index) => name === required[index]);
}
function jobs(workflow) {
  const body = workflow.slice(Math.max(0, workflow.search(/^jobs:\s*$/m)));
  const headers = [...body.matchAll(/^  ([a-z0-9-]+):\s*$/gm)];
  return new Map(headers.map((header, index) => [header[1], body.slice(header.index, headers[index + 1]?.index)]));
}
function step(job, name) {
  const marker = `      - name: ${name}\n`;
  const start = job.indexOf(marker);
  if (start < 0) return "";
  const body = job.slice(start);
  const end = body.slice(marker.length).search(/^      - /m);
  return end < 0 ? body : body.slice(0, marker.length + end);
}
function arrays(text, name) { return [...text.matchAll(new RegExp(`${name}=\\(([\\s\\S]*?)\\)`, "g"))].map((entry) => entry[1]); }
function sole(text, name, expected) { const values = arrays(text, name); return values.length === 1 && exact(values[0], expected); }
function soleLine(text, expected) { return normalized(text).split("\n").map((line) => line.trim()).filter((line) => line === expected).length === 1; }
function ordered(text, markers) { const positions = markers.map((marker) => text.indexOf(marker)); return positions.every((position, index) => position >= 0 && (!index || positions[index - 1] < position)); }

function rootTrigger(workflow) {
  const text = normalized(workflow);
  const permissionsIndex = text.indexOf("\npermissions:\n");
  if (permissionsIndex < 0) return "";
  const matches = [...text.matchAll(/^on:\s*$/gm)];
  if (matches.length !== 1 || matches[0].index === undefined || matches[0].index >= permissionsIndex) return "";
  return text.slice(matches[0].index, permissionsIndex);
}

function checkRoot(workflow, failures) {
  if (normalized(rootTrigger(workflow)) !== DEV_ONLY_TRIGGER) fail(failures, "W01", "requires exact push.branches: [dev] trigger");
  if (!/^permissions:\s*\n  contents:\s*read\s*$/m.test(workflow)) fail(failures, "W02", "workflow permissions must be contents: read");
  if (!/^  cancel-in-progress:\s*false\s*$/m.test(workflow)) fail(failures, "W03", "concurrency cancellation must be disabled");
}
function checkJobs(workflow, failures) {
  const map = jobs(workflow);
  const [rust, assets, publish] = JOBS.map((name) => map.get(name) ?? "");
  const permissions = [[rust, "read"], [assets, "read"], [publish, "write"]];
  if (map.size !== JOBS.length || JOBS.some((name) => !map.has(name)) || !/^    needs:\s*rust-cli-release\s*$/m.test(assets) || !/^    needs:\s*nightly-assets\s*$/m.test(publish) || permissions.some(([job, level]) => !new RegExp(`^    permissions:\\s*\\n      contents:\\s*${level}\\s*$`, "m").test(job))) fail(failures, "W04", "requires the least-privilege rust-cli-release -> nightly-assets -> publish-nightly chain");
  const uses = [...workflow.matchAll(/^\s*(?:-\s*)?uses:\s*([^\s#]+)(?:\s+#\s*([^\n]+))?$/gm)];
  if (uses.some((entry) => { const [action, sha] = entry[1].split("@"); return !/^[a-f0-9]{40}$/.test(sha ?? "") || ACTIONS[action] !== sha || !/^(?:v\d|stable)/.test(entry[2] ?? ""); })) fail(failures, "W05", "every action must use its approved immutable SHA and version comment");
  if ([rust, assets].some((job) => job.split(/(?=^      - )/m).some((entry) => entry.includes("actions/checkout@") && !/persist-credentials:\s*false/.test(entry)))) fail(failures, "W06", "each checkout must disable credential persistence");
  if (publish.includes("actions/checkout@")) fail(failures, "W07", "publisher must not check out source");
  checkArtifacts(assets, publish, failures);
}
function checkArtifacts(assets, publish, failures) {
  const prepare = step(assets, "Prepare nightly release assets");
  const verifyAssets = step(assets, "Verify nightly release assets");
  const upload = step(assets, "Upload nightly release bundle");
  const download = step(publish, "Download nightly release bundle");
  const verifyBundle = step(publish, "Verify downloaded release bundle");
  const draft = step(publish, "Prepare hidden nightly draft");
  const remote = step(publish, "Replace and remotely verify nightly assets");
  const final = step(publish, "Move tag and publish nightly release");
  const assetDownload = 'for expected in "${expected_assets[@]}"; do gh api -H "Accept: application/octet-stream" "repos/$GITHUB_REPOSITORY/releases/assets/${asset_ids[$expected]}" > "$verify_dir/$expected"; done';
  const tagReadback = 'tag_sha="$(gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/nightly" --jq .object.sha)"';
  const releaseReadback = 'final_state="$(gh api "repos/$GITHUB_REPOSITORY/releases/$NIGHTLY_RELEASE_ID")"';
  const checksum = prepare.match(/sha256sum\s+([\s\S]*?)>\s*SHA256SUMS\.txt/)?.[1] ?? "";
  if (!exact(checksum, PAYLOADS) || !sole(verifyAssets, "payloads", PAYLOADS) || !sole(verifyAssets, "expected_assets", ASSETS) || !sole(verifyBundle, "payloads", PAYLOADS) || !sole(verifyBundle, "expected_assets", ASSETS) || !sole(remote, "expected_assets", ASSETS)) fail(failures, "W08", "requires exact checksum inputs and named-step manifests");
  if (!/name:\s*nightly-release-bundle/.test(upload) || !/^          path:\s*app\/dist\s*$/m.test(upload) || !/name:\s*nightly-release-bundle/.test(download) || !/^          path:\s*bundle\s*$/m.test(download) || !/cd bundle\/release/.test(verifyBundle) || !/bundle\/release-notes\.md/.test(final)) fail(failures, "W09", "requires app/dist artifact root and bundle/release layout");
  const uploaded = remote.match(/gh release upload nightly\s+([\s\S]*?)\s+--repo/)?.[1].replaceAll("bundle/release/", "") ?? "";
  const publishedHide = /if \[ "\$is_draft" = false \]; then(?:(?!^\s*else\b)[\s\S])*?gh release edit nightly[^\n]*--draft\s*\n\s*else/m.test(draft);
  const draftOk = /release_rows="\$\(gh api --paginate/.test(draft) && publishedHide && /--method POST "repos\/\$GITHUB_REPOSITORY\/releases"/.test(draft);
  const remoteOk = !/< <\(gh api/.test(`${draft}\n${remote}`) && /old_asset_ids="\$\(gh api/.test(remote) && /remote_asset_rows="\$\(gh api/.test(remote) && /mapfile -t remote_assets <<</.test(remote) && exact(uploaded, PAYLOADS) && /gh release upload nightly bundle\/release\/SHA256SUMS\.txt/.test(remote) && soleLine(remote, assetDownload) && /\(cd "\$verify_dir" && sha256sum -c SHA256SUMS\.txt\)/.test(remote);
  const tagBranch = /if gh api "repos\/\$GITHUB_REPOSITORY\/git\/ref\/tags\/nightly"[\s\S]*--method PATCH[\s\S]*else[\s\S]*--method POST[\s\S]*fi/.test(final);
  const undrafts = final.match(/--draft=false/g) ?? [];
  const finalOrder = ordered(final, ["fi\n          tag_sha=", "tag_sha=", "test \"$tag_sha\" = \"$GITHUB_SHA\"", "gh release edit nightly", "expected_assets=", "final_state=", "final_asset_rows=", "mapfile -t final_assets", "for expected in \"${expected_assets[@]}\""]);
  const finalOk = tagBranch && undrafts.length === 1 && finalOrder && sole(final, "expected_assets", ASSETS) && soleLine(final, tagReadback) && soleLine(final, releaseReadback) && /final_asset_rows="\$\(jq/.test(final) && /mapfile -t final_assets <<</.test(final) && /for expected in "\$\{expected_assets\[@\]\}"/.test(final);
  if (!draftOk || !remoteOk || !finalOk || !ordered(publish, ["Prepare hidden nightly draft", "Replace and remotely verify nightly assets", "Move tag and publish nightly release"])) fail(failures, "W10", "requires scoped draft, remote verification, tag readback, and final-readback transitions");
  if (/--clobber\b/.test(publish) || /gh release upload nightly[\s\S]*?\*/.test(publish)) fail(failures, "W11", "publisher must not use wildcard or clobber uploads");
  if (!["first-release", "existing published release", "retry draft"].every((marker) => draft.includes(marker)) || !/\[\[ "\$release_rows" == \*\$'\\n'\* \]\]/.test(draft)) fail(failures, "W12", "must reject duplicate releases and represent all draft branches");
}
function checkReadme(text, failures, surface) {
  if (!/WIDE11[\s\S]{0,180}144184 B[\s\S]{0,180}(?:hardware slice|硬件切片)[\s\S]{0,180}30720 B[\s\S]{0,180}(?:WS telemetry ring|WS 遥测环)[\s\S]{0,180}149048 B[\s\S]{0,180}(?:total backing allocation|总后备分配)/i.test(text)) fail(failures, "W13", "requires the WIDE11 144184 B slice, 30720 B telemetry ring, and 149048 B allocation", surface);
}
export function formatFailures(failures) { return ["nightly workflow contract failed:", ...failures.map(({ code, surface, detail }) => `- [${code}] ${surface}: ${detail}`)].join("\n"); }
export async function checkNightlyWorkflow(repositoryRoot) {
  const root = path.resolve(repositoryRoot);
  const failures = [];
  const contents = new Map();
  for (const relativePath of [WORKFLOW, "README.md", "README.zh-CN.md"]) {
    try { contents.set(relativePath, normalized(await readFile(path.join(root, relativePath), "utf8"))); }
    catch (error) { fail(failures, "file-missing", error?.code === "ENOENT" ? "required file is missing" : String(error), relativePath); }
  }
  if (contents.has(WORKFLOW)) { checkRoot(contents.get(WORKFLOW), failures); checkJobs(contents.get(WORKFLOW), failures); }
  for (const readme of ["README.md", "README.zh-CN.md"]) if (contents.has(readme)) checkReadme(contents.get(readme), failures, readme);
  return { ok: failures.length === 0, failures };
}
async function main() {
  const args = process.argv.slice(2);
  if (args.length !== 2 || args[0] !== "--root") { console.error("usage: node scripts/check-nightly-workflow.mjs --root <repository-root>"); process.exitCode = 2; return; }
  const result = await checkNightlyWorkflow(args[1]);
  if (!result.ok) { console.error(formatFailures(result.failures)); process.exitCode = 1; }
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) await main();
