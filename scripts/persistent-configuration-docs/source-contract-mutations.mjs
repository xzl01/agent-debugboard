import assert from "node:assert/strict";
import { checkPersistentConfigurationDocs, formatFailures } from "../check-persistent-configuration-docs.mjs";
import { fixtureDocuments, withFixture } from "./fixtures.mjs";

const MUTATIONS = Object.freeze([
  {
    name: "requires the canonical Automatic Current Synchronization section",
    surface: "docs/reference/persistent-configuration.md",
    before: "## Automatic Current Synchronization",
    after: "",
    code: "heading-missing",
  },
  {
    name: "requires the skill Automatic Current Synchronization section",
    surface: "skills/radxa-linkr-debugger/SKILL.md",
    before: "### Automatic Current Synchronization",
    after: "",
    code: "heading-missing",
  },
  {
    name: "rejects missing canonical current-source marker",
    surface: "docs/reference/persistent-configuration.md",
    before: "current-source:Current-from-/api/v1/config",
    after: "",
    code: "current-sync-marker",
  },
  {
    name: "rejects drifted canonical current-no-write literal",
    surface: "docs/reference/persistent-configuration.md",
    before: "current-no-write:display-sync-no-auto-save-no-flash-no-apply",
    after: "current-no-write:display-sync-auto-save-writes-flash",
    code: "current-sync-marker",
  },
  {
    name: "rejects drifted canonical current-no-flood literal",
    surface: "docs/reference/persistent-configuration.md",
    before: "current-no-flood:one-transition-one-refresh;identical-frames-zero-GETs",
    after: "current-no-flood:every-frame-issues-a-GET",
    code: "current-sync-marker",
  },
  {
    name: "rejects drifted canonical current-hil-boundary literal",
    surface: "docs/reference/persistent-configuration.md",
    before: "current-hil-boundary:Todo-6-post-fix-HIL-still-required",
    after: "current-hil-boundary:Todo-6-HIL-already-passed",
    code: "current-sync-marker",
  },
  {
    name: "rejects unknown canonical marker ID",
    surface: "docs/reference/persistent-configuration.md",
    before: "current-no-write:display-sync-no-auto-save-no-flash-no-apply",
    after: "current-no-write:display-sync-no-auto-save-no-flash-no-apply\ncurrent-makes-coffee:true",
    code: "current-sync-marker",
  },
  {
    name: "rejects missing skill current-refresh-recovery marker",
    surface: "skills/radxa-linkr-debugger/SKILL.md",
    before: "current-refresh-recovery:Refresh-manual-recovery-not-required",
    after: "",
    code: "current-sync-marker",
  },
  {
    name: "rejects drifted skill current-refresh-recovery literal",
    surface: "skills/radxa-linkr-debugger/SKILL.md",
    before: "current-refresh-recovery:Refresh-manual-recovery-not-required",
    after: "current-refresh-recovery:Refresh-runs-on-every-transition",
    code: "current-sync-marker",
  },
  {
    name: "rejects auto-save drift even with valid markers",
    surface: "docs/reference/persistent-configuration.md",
    before: "## CDC ACM Fallback",
    after: "Auto-save every current value automatically.\n\n## CDC ACM Fallback",
    code: "source-contract",
  },
  {
    name: "rejects client overlay drift even with valid markers",
    surface: "skills/radxa-linkr-debugger/SKILL.md",
    before: "auto-refresh Current",
    after: "Web invents the Current column from status and WebSocket fields",
    code: "skill-policy",
  },
  {
    name: "requires storage replacement after corrupt or unsupported snapshots",
    surface: "docs/reference/persistent-configuration.md",
    before: "Save does not gate on the service reason and can replace a corrupt or unsupported snapshot",
    after: "Only clear may recover a corrupt or unsupported snapshot",
    code: "source-contract",
  },
  {
    name: "requires divergent Web and Rust WS summary behavior",
    surface: "docs/reference/persistent-configuration.md",
    before: "Web preserves the last valid summary when the next WS value is absent or malformed. Rust TUI clears support, focus, and confirmation when `snapshot.config` is `None`.",
    after: "All clients clear configuration state when a WS summary is absent or malformed.",
    code: "source-contract",
  },
  {
    name: "requires CDC primary lines before detail lines",
    surface: "docs/reference/persistent-configuration.md",
    before: "The primary result or error line precedes `confirmation_id`, `applied_id`, `failed_id`, and `pending_id` detail lines; prompt or echo is not a result.",
    after: "Detail lines precede their primary result or error line.",
    code: "source-contract",
  },
  {
    name: "requires independent BOOTSEL and OTA-test marker IDs",
    surface: "docs/reference/persistent-configuration.md",
    before: "BOOTSEL scratch index 0 marker `0xadb00751` and OTA-test scratch index 2 marker `0x07a7e571` are independent of Settings snapshot.",
    after: "BOOTSEL and OTA-test share Settings snapshot state.",
    code: "source-contract",
  },
  {
    name: "requires app facade rules and action-specific error fields",
    surface: "apps/radxa_linkr_debugger/README.md",
    before: "A `busy` error carries `activity`. An `apply_failed` error carries `applied_items`, `failed_item`, and `pending_items`.",
    after: "A busy error has no action-specific fields.",
    code: "action-fields",
  },
  {
    name: "requires HIL pacing and enumerated cleanup",
    surface: "docs/testing/hil-functional-test-spec.md",
    before: "Each OTA upload uses --limit-rate 64K.",
    after: "OTA upload pacing is optional.",
    code: "hil-policy",
  },
  {
    name: "requires the persistent-configuration local dry-run in testing docs",
    surface: "docs/testing/hil-functional-test-spec.md",
    before: "sh skills/radxa-linkr-debugger/scripts/config-persistence-hil.sh --dry-run safe-reboot",
    after: "the persistent-configuration local dry-run is documented elsewhere",
    code: "hil-policy",
  },
  {
    name: "requires the marked fail-with-body dangerous-save example",
    surface: "skills/radxa-linkr-debugger/SKILL.md",
    before: "--fail-with-body",
    after: "--fail",
    code: "skill-policy",
  },
]);

function applyMutation(documents, mutation) {
  const content = documents[mutation.surface];
  assert.ok(content.includes(mutation.before), `fixture missing ${mutation.name}`);
  documents[mutation.surface] = content.replace(mutation.before, mutation.after);
}

export async function registerSourceContractMutationTests(suite) {
  for (const mutation of MUTATIONS) {
    await suite.test(mutation.name, async () => {
      const documents = fixtureDocuments();
      applyMutation(documents, mutation);
      await withFixture(async (root) => {
        const result = await checkPersistentConfigurationDocs(root);
        assert.ok(result.failures.some((failure) => failure.code === mutation.code), formatFailures(result.failures));
      }, documents);
    });
  }
}
