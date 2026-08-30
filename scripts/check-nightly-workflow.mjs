import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const WORKFLOW = ".github/workflows/nightly.yml";
const ACTIONS = Object.freeze({ "actions/checkout": "d23441a48e516b6c34aea4fa41551a30e30af803", "actions/upload-artifact": "b7c566a772e6b6bfb58ed0dc250532a479d7789f", "actions/download-artifact": "018cc2cf5baa6db3ef3c5f8a56943fffe632ef53", "actions/setup-python": "ece7cb06caefa5fff74198d8649806c4678c61a1", "actions/setup-node": "49933ea5288caeca8642d1e84afbd3f7d6820020", "Swatinem/rust-cache": "e18b497796c12c097a38f9edb9d0641fb99eee32", "dtolnay/rust-toolchain": "4cda84d5c5c54efe2404f9d843567869ab1699d4", "zephyrproject-rtos/action-zephyr-setup": "66a907961072acaa85313d2e064e9f071141265a" });
const JOBS = ["validation", "rust-cli-release", "native-packages", "nightly-assets", "publish-nightly"];
const FIXED_PAYLOADS = ["radxa-linkr-debugger-rp2350.uf2", "radxa-linkr-debugger-rp2350-ota.bin", "radxa-linkr-debugger-rp2350.elf", "radxa-linkr-debugger-rp2350.map", "radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz", "radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz", "radxa-linkr-debuggerctl-rust_windows_amd64.zip", "skills-radxa-linkr-debugger.tar.gz"];
const NATIVE_PAYLOADS = ["agent-debugboard-*.tar.gz", "radxa-linkr-debuggerctl_*.deb", "radxa-linkr-debugger-firmware_*.deb", "radxa-linkr-debuggerctl-*.rpm", "radxa-linkr-debugger-firmware-*.rpm", "radxa-linkr-debuggerctl-*.pkg.tar.zst", "radxa-linkr-debugger-firmware-*.pkg.tar.zst"];
const CHECKSUM_PAYLOADS = [...FIXED_PAYLOADS, ...NATIVE_PAYLOADS];
const PAYLOAD_ARRAY = [...FIXED_PAYLOADS, '"${native_payloads[@]}"'];
const ASSET_ARRAY = ['"${payloads[@]}"', "SHA256SUMS.txt"];
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
function arrays(text, name) { return [...text.matchAll(new RegExp(`^\\s*${name}=\\(([\\s\\S]*?)\\)`, "gm"))].map((entry) => entry[1]); }
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
  const [validation, rust, native, assets, publish] = JOBS.map((name) => map.get(name) ?? "");
  const permissions = [[validation, "read"], [rust, "read"], [native, "read"], [assets, "read"], [publish, "write"]];
  const nativeNeeds = /^    needs:\s*\n      - validation\s*\n      - rust-cli-release\s*$/m.test(native);
  const chain = /^    uses:\s*\.\/\.github\/workflows\/build\.yml\s*$/m.test(validation)
    && /^    needs:\s*validation\s*$/m.test(rust) && nativeNeeds
    && /^    uses:\s*\.\/\.github\/workflows\/native-packages\.yml\s*$/m.test(native)
    && /^      source_sha:\s*\$\{\{ github\.sha \}\}\s*$/m.test(native)
    && /^    needs:\s*native-packages\s*$/m.test(assets) && /^    needs:\s*nightly-assets\s*$/m.test(publish);
  if (map.size !== JOBS.length || JOBS.some((name) => !map.has(name)) || !chain || permissions.some(([job, level]) => !new RegExp(`^    permissions:\\s*\\n      contents:\\s*${level}\\s*$`, "m").test(job))) fail(failures, "W04", "requires validation -> Rust CLI -> shared native packages -> nightly assets -> publisher");
  const uses = [...workflow.matchAll(/^\s*(?:-\s*)?uses:\s*([^\s#]+)(?:\s+#\s*([^\n]+))?$/gm)];
  const localWorkflows = new Set(["./.github/workflows/build.yml", "./.github/workflows/native-packages.yml"]);
  if (uses.some((entry) => { if (entry[1].startsWith("./")) return !localWorkflows.has(entry[1]); const [action, sha] = entry[1].split("@"); return !/^[a-f0-9]{40}$/.test(sha ?? "") || ACTIONS[action] !== sha || !/^(?:v\d|stable)/.test(entry[2] ?? ""); })) fail(failures, "W05", "every action must use its approved immutable SHA and version comment");
  if ([rust, assets].some((body) => body.split(/(?=^      - )/m).some((entry) => entry.includes("actions/checkout@") && !/persist-credentials:\s*false/.test(entry)))) fail(failures, "W06", "each checkout must disable credential persistence");
  if (publish.includes("actions/checkout@")) fail(failures, "W07", "publisher must not check out source");
  if (!/x86_64-unknown-linux-musl/.test(rust) || !/cargo zigbuild --locked --release/.test(rust) || !/must not have dynamic library dependencies/.test(rust) || !/cargo build --locked --release/.test(rust)) fail(failures, "W14", "Linux nightly CLI must be locked, static musl while other platforms also use the lockfile");
  checkArtifacts(assets, publish, failures);
}
function checkArtifacts(assets, publish, failures) {
  const firmware = step(assets, "Build RP2350 firmware");
  const downloadNative = step(assets, "Download native release packages");
  const prepare = step(assets, "Prepare nightly release assets");
  const verifyAssets = step(assets, "Verify nightly release assets");
  const writeNotes = step(assets, "Write nightly release notes");
  const upload = step(assets, "Upload nightly release bundle");
  const download = step(publish, "Download nightly release bundle");
  const verifyBundle = step(publish, "Verify downloaded release bundle");
  const draft = step(publish, "Prepare staged nightly draft");
  const remote = step(publish, "Upload and verify staged nightly assets");
  const final = step(publish, "Promote staged nightly release");
  const cleanup = step(publish, "Clean up abandoned nightly candidate");
  const assetDownload = 'for expected in "${expected_assets[@]}"; do gh api -H "Accept: application/octet-stream" "repos/$GITHUB_REPOSITORY/releases/assets/${asset_ids[$expected]}" > "$verify_dir/$expected"; done';
  const tagReadback = 'tag_sha="$(gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/nightly" --jq .object.sha)"';
  const releaseReadback = 'final_state="$(gh api "repos/$GITHUB_REPOSITORY/releases/$CANDIDATE_RELEASE_ID")"';
  const canonicalUf2Copy = "cp build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2 release-assets/radxa-linkr-debugger-rp2350.uf2";
  const expectedReleaseAssetLines = ["mkdir -p release-assets", canonicalUf2Copy,
    "cp build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin release-assets/radxa-linkr-debugger-rp2350-ota.bin",
    "cp build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.elf release-assets/radxa-linkr-debugger-rp2350.elf",
    "cp build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.map release-assets/radxa-linkr-debugger-rp2350.map",
    "cp ../release-assets/radxa-linkr-debugger-rp2350.uf2 dist/release/",
    "cp ../release-assets/radxa-linkr-debugger-rp2350-ota.bin dist/release/",
    "cp ../release-assets/radxa-linkr-debugger-rp2350.elf dist/release/",
    "cp ../release-assets/radxa-linkr-debugger-rp2350.map dist/release/"];
  const releaseAssetLines = normalized(assets).split("\n").map((line) => line.trim()).filter((line) => line.includes("release-assets"));
  const releaseAssetsOk = releaseAssetLines.length === expectedReleaseAssetLines.length && expectedReleaseAssetLines.every((command) => releaseAssetLines.includes(command));
  const provenanceCmp = "cmp ../build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2 dist/release/radxa-linkr-debugger-rp2350.uf2";
  const provenanceOk = soleLine(writeNotes, provenanceCmp) && writeNotes.trim().endsWith(provenanceCmp) && assets.includes(`${provenanceCmp}\n      - name: Upload nightly release bundle`);
  const checksum = prepare.match(/sha256sum\s+([\s\S]*?)>\s*SHA256SUMS\.txt/)?.[1] ?? "";
  const strictManifest = (body) => sole(body, "native_payloads", NATIVE_PAYLOADS) && sole(body, "payloads", PAYLOAD_ARRAY)
    && sole(body, "expected_assets", ASSET_ARRAY) && /test "\$\{#native_payloads\[@\]\}" -eq 7/.test(body)
    && /test "\$\{#payloads\[@\]\}" -eq 15/.test(body);
  if (!soleLine(firmware, canonicalUf2Copy) || !releaseAssetsOk || !provenanceOk || /\bZEPHYR_BASE\b|mergehex\.py|uf2conv\.py|\bzephyr\.uf2\b/.test(assets)) fail(failures, "W15", "firmware packaging must copy and preserve the canonical combined UF2");
  if (!/name:\s*native-release-packages/.test(downloadNative) || !/^          path:\s*native-package-assets\s*$/m.test(downloadNative) || !/cp \.\.\/native-package-assets\/\* dist\/release\//.test(prepare)) fail(failures, "W16", "nightly must download and stage the shared native package artifact");
  const derivedRemoteManifest = (body) => /mapfile -t payloads < <\(awk '\{print \$2\}' bundle\/release\/SHA256SUMS\.txt\)/.test(body)
    && /test "\$\{#payloads\[@\]\}" -eq 15/.test(body) && sole(body, "expected_assets", ASSET_ARRAY);
  if (!exact(checksum, CHECKSUM_PAYLOADS) || !strictManifest(verifyAssets) || !strictManifest(verifyBundle) || !derivedRemoteManifest(remote) || !derivedRemoteManifest(final)) fail(failures, "W08", "requires checksums and strict manifests for eight fixed payloads, a source tar, and six native packages");
  if (!/name:\s*nightly-release-bundle/.test(upload) || !/^          path:\s*app\/dist\s*$/m.test(upload) || !/name:\s*nightly-release-bundle/.test(download) || !/^          path:\s*bundle\s*$/m.test(download) || !/cd bundle\/release/.test(verifyBundle) || !/bundle\/release-notes\.md/.test(final)) fail(failures, "W09", "requires app/dist artifact root and bundle/release layout");
  const uploadLoop = /for payload in "\$\{payloads\[@\]\}"; do[\s\S]*?gh release upload "\$CANDIDATE_TAG" "bundle\/release\/\$payload" --repo "\$GITHUB_REPOSITORY"[\s\S]*?done/.test(remote);
  const draftOk = /CANDIDATE_TAG="nightly-candidate-\$GITHUB_RUN_ID"/.test(draft) && /candidate_rows="\$\(gh api --paginate/.test(draft) && /-F draft=true/.test(draft) && /candidate_asset_ids="\$\(gh api/.test(draft) && !/gh release (?:delete|edit) nightly/.test(draft);
  const remoteOk = !/< <\(gh api/.test(`${draft}\n${remote}`) && /remote_asset_rows="\$\(gh api[\s\S]*CANDIDATE_RELEASE_ID/.test(remote) && /mapfile -t remote_assets <<< /.test(remote) && uploadLoop && /gh release upload "\$CANDIDATE_TAG" bundle\/release\/SHA256SUMS\.txt/.test(remote) && soleLine(remote, assetDownload) && /\(cd "\$verify_dir" && sha256sum -c SHA256SUMS\.txt\)/.test(remote) && !/gh release delete nightly/.test(remote);
  const promotion = /gh release delete nightly[^\n]*--cleanup-tag/.test(final) && /gh release edit "\$CANDIDATE_TAG"[^\n]*--tag nightly[^\n]*--draft=false/.test(final);
  const finalOrder = ordered(final, ["existing_rows=", "gh release edit \"$CANDIDATE_TAG\"", "tag_sha=", "test \"$tag_sha\" = \"$GITHUB_SHA\"", "mapfile -t payloads", "expected_assets=", "final_state=", "final_asset_rows=", "mapfile -t final_assets", "for expected in \"${expected_assets[@]}\""]);
  const finalOk = promotion && finalOrder && soleLine(final, tagReadback) && soleLine(final, releaseReadback) && /final_asset_rows="\$\(jq/.test(final) && /mapfile -t final_assets <<< /.test(final) && /for expected in "\$\{expected_assets\[@\]\}"/.test(final);
  const cleanupOk = /if:\s*\$\{\{\s*always\(\)\s*\}\}/.test(cleanup) && /candidate_tag="\$\{CANDIDATE_TAG:-nightly-candidate-\$GITHUB_RUN_ID\}"/.test(cleanup) && /select\(\.tag_name == \\"\$candidate_tag\\"\)/.test(cleanup) && /\[\.id, \.draft\]/.test(cleanup) && /if \[ "\$candidate_draft" != true \]; then/.test(cleanup) && /--method DELETE "repos\/\$GITHUB_REPOSITORY\/releases\/\$candidate_id"/.test(cleanup) && /--method DELETE "repos\/\$GITHUB_REPOSITORY\/git\/refs\/tags\/\$candidate_tag"/.test(cleanup) && !/gh release (?:delete|edit) nightly/.test(cleanup);
  if (!draftOk || !remoteOk || !finalOk || !cleanupOk || !ordered(publish, ["Prepare staged nightly draft", "Upload and verify staged nightly assets", "Promote staged nightly release", "Clean up abandoned nightly candidate"])) fail(failures, "W10", "requires staged remote verification, promotion, and draft-only candidate cleanup");
  if (/--clobber\b/.test(publish) || /gh release upload "\$CANDIDATE_TAG"[^\n]*bundle\/release\/\*/.test(publish)) fail(failures, "W11", "publisher must not use wildcard or clobber uploads");
  if (!/\[\[ "\$candidate_rows" == \*\$'\\n'\* \]\]/.test(draft) || !/\[\[ "\$existing_rows" == \*\$'\\n'\* \]\]/.test(final)) fail(failures, "W12", "must reject duplicate candidate and nightly releases");
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
