const ACTION_PINS = Object.freeze({
  "actions/checkout": ["d23441a48e516b6c34aea4fa41551a30e30af803", "v6.1.0"],
  "actions/configure-pages": ["983d7736d9b0ae728b81ab479565c72886d7745b", "v5.0.0"],
  "actions/deploy-pages": ["d6db90164ac5ed86f2b6aed7e0febac5b3c0c03e", "v4.0.5"],
  "actions/download-artifact": ["018cc2cf5baa6db3ef3c5f8a56943fffe632ef53", "v6.0.0"],
  "actions/setup-node": ["49933ea5288caeca8642d1e84afbd3f7d6820020", "v4.4.0"],
  "actions/setup-python": ["ece7cb06caefa5fff74198d8649806c4678c61a1", "v6.3.0"],
  "actions/upload-artifact": ["b7c566a772e6b6bfb58ed0dc250532a479d7789f", "v6.0.0"],
  "actions/upload-pages-artifact": ["7b1f4a764d45c48632c6b24a0339c27f5614fb0b", "v4.0.0"],
  "cachix/install-nix-action": ["630ae543ea3a38a9a4166f03376c02c50f408342", "v31.11.0"],
  "dtolnay/rust-toolchain": ["4360b52568e2003a75bf9bc1d59f33a8e3fc893c", "stable"],
  "Swatinem/rust-cache": ["6323deb102c322ba6fcbdcafc7e3dddab59af2b6", "v2.9.2"],
  "zephyrproject-rtos/action-zephyr-setup": ["be8136a8bba01580485d98b7ad2d32477c36a49a", "v1"],
});

const EFFECTIVE_SOURCE_REF = "ref: ${{ needs.source.outputs.effective_source_sha }}";
const DURABLE_GATE_COMMANDS = Object.freeze([
  ["scripts/check-nightly-workflow.test.mjs", "node scripts/check-nightly-workflow.mjs --root ."],
  ["scripts/check-rdb-alias.test.mjs", "node scripts/check-rdb-alias.mjs --root ."],
  ["scripts/check-repository-gates.test.mjs", "node scripts/check-repository-gates.mjs --root ."],
  ["scripts/check-test-registration.test.mjs", "node scripts/check-test-registration.mjs --root ."],
  ["scripts/check-doc-layout.test.mjs", "node scripts/check-doc-layout.mjs --root ."],
  ["scripts/check-skill-boundary.test.mjs", "node scripts/check-skill-boundary.mjs --root ."],
]);
const PERSISTENT_DOCS_COMMANDS = Object.freeze([
  "scripts/check-persistent-configuration-docs.test.mjs",
  "node scripts/check-persistent-configuration-docs.mjs --root .",
]);
const WORKTREE_SCOPE_TEST = "scripts/check-worktree-scope.test.mjs";
const WORKTREE_SCOPE_COMMAND = "node scripts/check-worktree-scope.mjs --root .";

function count(text, pattern) {
  return [...text.matchAll(pattern)].length;
}

function jobs(workflow) {
  const body = workflow.slice(Math.max(0, workflow.search(/^jobs:\s*$/m)));
  const headers = [...body.matchAll(/^  ([a-z0-9-]+):\s*$/gm)];
  return new Map(headers.map((header, index) => [header[1], body.slice(header.index, headers[index + 1]?.index)]));
}

function job(workflow, name) {
  return jobs(workflow).get(name) ?? "";
}

function makeTarget(makefile, name) {
  const headers = [...makefile.matchAll(/^([A-Za-z0-9_.-]+):.*$/gm)];
  const index = headers.findIndex((header) => header[1] === name);
  return index < 0 ? "" : makefile.slice(headers[index].index, headers[index + 1]?.index);
}

function needs(jobBody) {
  const scalar = jobBody.match(/^    needs:[ \t]*([a-z0-9-]+)[ \t]*$/m);
  if (scalar) return [scalar[1]];
  const block = jobBody.match(/^    needs:[ \t]*\n((?:      - [a-z0-9-]+[ \t]*\n?)+)/m)?.[1] ?? "";
  return [...block.matchAll(/^      - ([a-z0-9-]+)[ \t]*$/gm)].map((match) => match[1]);
}

function steps(jobBody) {
  return jobBody.split(/(?=^      - )/m).filter((step) => step.startsWith("      - "));
}

function permissionEntries(text, indent) {
  const markers = [...text.matchAll(new RegExp(`^${indent}permissions:`, "gm"))];
  if (markers.length === 0) return null;
  if (markers.length !== 1 || markers[0].index === undefined) return [];
  const entries = [];
  const entryIndent = `${indent}  `;
  for (const line of text.slice(markers[0].index).split("\n").slice(1)) {
    if (line.trim() === "") continue;
    if (line.trim().startsWith("#")) {
      if (line.startsWith(entryIndent)) continue;
      break;
    }
    if (!line.startsWith(entryIndent)) break;
    const entry = line.match(new RegExp(`^${entryIndent}([a-z-]+):\\s*([a-z-]+)\\s*$`));
    if (!entry) return [];
    entries.push([entry[1], entry[2]]);
  }
  return entries;
}

function onlyContents(entries, access) {
  return entries?.length === 1 && entries[0][0] === "contents" && entries[0][1] === access;
}

function externalActionsArePinned(workflow) {
  const uses = [...workflow.matchAll(/^\s*(?:-\s*)?uses:\s*([^\s#]+)(?:\s+#\s*([^\n]+))?$/gm)];
  return uses.every((entry) => {
    const reference = entry[1];
    if (reference.startsWith("./")) return true;
    const [action, sha] = reference.split("@");
    const pin = ACTION_PINS[action];
    return /^[a-f0-9]{40}$/.test(sha ?? "") && pin?.[0] === sha && pin[1] === entry[2]?.trim();
  });
}

function permanentGateContract(build, makefile) {
  const scripts = job(build, "scripts");
  const ciGates = steps(scripts).find((step) => step.startsWith("      - name: Check CI contracts and test registration\n")) ?? "";
  const ciPersistentDocs = steps(scripts).find((step) => step.startsWith("      - name: Check persistent-configuration documentation\n")) ?? "";
  const localGates = makeTarget(makefile, "gates");
  const localPersistentDocs = makeTarget(makefile, "persistent-configuration-docs");
  const localCheck = makeTarget(makefile, "check");
  const durableCommandsPresent = (text) => DURABLE_GATE_COMMANDS.every((commands) => commands.every((command) => text.includes(command)));
  const persistentDocsPresent = (text) => PERSISTENT_DOCS_COMMANDS.every((command) => text.includes(command));
  const worktreeScopeTestsPresent = (text) => text.includes(WORKTREE_SCOPE_TEST);
  const worktreeScopeCommandAbsent = (text) => !text.includes(WORKTREE_SCOPE_COMMAND);
  const webTargetsUseNix = makeTarget(makefile, "web-test").includes('$(NIX) "cd web && npm test"')
    && makeTarget(makefile, "web").includes('$(NIX) "cd web && npm run build"');
  const obsoleteWrappersAbsent = !`${build}\n${makefile}`.includes("scripts/setup-zephyr.sh")
    && !`${build}\n${makefile}`.includes("scripts/build-firmware.sh");
  return durableCommandsPresent(ciGates) && durableCommandsPresent(localGates)
    && persistentDocsPresent(ciPersistentDocs) && persistentDocsPresent(localPersistentDocs)
    && PERSISTENT_DOCS_COMMANDS.every((command) => !localGates.includes(command))
    && worktreeScopeTestsPresent(build) && worktreeScopeTestsPresent(makefile)
    && worktreeScopeCommandAbsent(build) && worktreeScopeCommandAbsent(makefile)
    && /^check:.*\bgates\b.*\bpersistent-configuration-docs\b.*\bweb-test\b.*\bweb\b/m.test(localCheck)
    && webTargetsUseNix && obsoleteWrappersAbsent;
}

function sourceGateContract(build) {
  const source = job(build, "source");
  const gate = job(build, "gate");
  const checkoutJobs = [...jobs(build).values()].filter((body) => body.includes("uses: actions/checkout@"));
  const sourceInput = /^  workflow_call:\s*\n    inputs:\s*\n      source_sha:\s*\n        description:.+\n        required:\s*false\s*\n        type:\s*string\s*\n        default:\s*""\s*$/m.test(build);
  const sourceStep = source.includes("effective_source_sha: ${{ steps.effective_source.outputs.effective_source_sha }}")
    && source.includes("REQUESTED_SOURCE_SHA: ${{ inputs.source_sha }}")
    && source.includes("INHERITED_SOURCE_SHA: ${{ github.sha }}")
    && source.includes('if [[ -n "$REQUESTED_SOURCE_SHA" && ! "$REQUESTED_SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]]; then')
    && source.includes('effective_source_sha="${REQUESTED_SOURCE_SHA:-$INHERITED_SOURCE_SHA}"')
    && source.includes('if [[ ! "$effective_source_sha" =~ ^[0-9a-f]{40}$ ]]; then')
    && source.includes("printf 'effective_source_sha=%s\\n' \"$effective_source_sha\" >> \"$GITHUB_OUTPUT\"")
    && !source.includes("actions/checkout@");
  const allCheckoutsUseSource = checkoutJobs.length > 0
    && checkoutJobs.every((body) => needs(body).includes("source") && body.includes(EFFECTIVE_SOURCE_REF));
  const checkoutCount = count(build, /^\s*uses:\s*actions\/checkout@/gm);
  const effectiveRefCount = count(build, /^\s*ref:\s*\$\{\{ needs\.source\.outputs\.effective_source_sha \}\}\s*$/gm);
  return sourceInput && sourceStep && allCheckoutsUseSource && checkoutCount === effectiveRefCount
    && needs(gate).includes("source");
}

function releaseSourceContract(release) {
  const releaseJobs = jobs(release);
  const resolve = job(release, "resolve");
  const validation = job(release, "validation");
  const version = job(release, "version-gate");
  const rust = job(release, "rust-cli-release");
  const desktop = job(release, "host-desktop-release");
  const publish = job(release, "release");
  const resolver = /normalized_tag:\s*\$\{\{ steps\.resolve\.outputs\.normalized_tag \}\}/.test(resolve)
    && /resolved_sha:\s*\$\{\{ steps\.resolve\.outputs\.resolved_sha \}\}/.test(resolve)
    && /fetch-depth:\s*0/.test(resolve) && /fetch-tags:\s*true/.test(resolve)
    && /persist-credentials:\s*false/.test(resolve)
    && /\[\[ "\$tag" =~ \^v\[0-9\]\+\\\.\[0-9\]\+\\\.\[0-9\]\+/.test(resolve)
    && count(resolve, /git rev-parse --verify "\$\{tag_ref\}\^\{commit\}"/g) === 1
    && /resolved_sha"\s*==\s*"\$event_sha/.test(resolve);
  const checkoutSteps = [...releaseJobs.entries()].flatMap(([name, body]) => steps(body)
    .filter((step) => step.includes("uses: actions/checkout@"))
    .map((step) => [name, step]));
  const checkouts = checkoutSteps.length > 0 && checkoutSteps.every(([name, step]) => {
    const credentialsDisabled = /^          persist-credentials:\s*false\s*$/m.test(step);
    const resolvedRef = /^          ref:\s*\$\{\{ needs\.resolve\.outputs\.resolved_sha \}\}\s*$/m.test(step);
    return credentialsDisabled && (name === "resolve" || resolvedRef);
  });
  const dependencies = needs(validation).includes("resolve") && needs(version).includes("resolve")
    && needs(rust).includes("resolve") && needs(desktop).includes("resolve")
    && needs(publish).includes("resolve");
  const validationBinding = validation.includes("source_sha: ${{ needs.resolve.outputs.resolved_sha }}");
  const remoteReadback = publish.includes('gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/$RELEASE_TAG"')
    && publish.includes('while [[ "$object_type" == "tag" ]]')
    && publish.includes('gh api "repos/$GITHUB_REPOSITORY/git/tags/$object_sha"')
    && publish.includes('[[ "$object_sha" == "$RESOLVED_SHA" ]]')
    && publish.indexOf('[[ "$object_sha" == "$RESOLVED_SHA" ]]') < publish.indexOf("if gh release view");
  const root = release.slice(0, Math.max(0, release.search(/^jobs:\s*$/m)));
  const permissions = onlyContents(permissionEntries(root, ""), "read")
    && [...releaseJobs.entries()].every(([name, body]) => {
      const entries = permissionEntries(body, "    ");
      return name === "release" ? onlyContents(entries, "write") : entries === null || onlyContents(entries, "read");
    });
  return resolver && checkouts && dependencies && validationBinding && remoteReadback && permissions;
}

export function checkWorkflowContracts({ build, release, pages, versionBump, makefile }) {
  return sourceGateContract(build) && releaseSourceContract(release)
    && externalActionsArePinned(build) && externalActionsArePinned(release)
    && externalActionsArePinned(pages) && externalActionsArePinned(versionBump)
    && permanentGateContract(build, makefile);
}
