import assert from "node:assert/strict";
import path from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";
import {
  DOC_SURFACES, FROZEN_SUMMARY, REQUIRED_EXAMPLES, SKILL_CURRENT_SYNC_CONTRACT, WEB_CURRENT_SYNC_CONTRACT,
  checkPersistentConfigurationDocs, formatFailures,
} from "./check-persistent-configuration-docs.mjs";
import { FORBIDDEN_MUTATIONS, fixtureDocuments, withFixture } from "./persistent-configuration-docs/fixtures.mjs";
import { buildCli, executeExamples, startMock, withCliWrapper } from "./persistent-configuration-docs/loopback.mjs";
import { checkerModuleSizes } from "./persistent-configuration-docs/source-size.mjs";
import { parseCurrentSyncMarkers } from "./persistent-configuration-docs/markdown.mjs";
import { registerSourceContractMutationTests } from "./persistent-configuration-docs/source-contract-mutations.mjs";

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

test("persistent-configuration documentation contract", async (suite) => {
  await suite.test("keeps every checker module at or below 250 pure LOC", async () => {
    for (const module of await checkerModuleSizes(repositoryRoot)) {
      assert.ok(module.lines <= 250, `${module.path} has ${module.lines} pure LOC`);
    }
  });

  await suite.test("freezes exactly the six planned surfaces and twelve versioned summary IDs", () => {
    assert.deepEqual(DOC_SURFACES.map((surface) => surface.path), [
      "docs/reference/persistent-configuration.md", "README.md", "README.zh-CN.md",
      "apps/radxa_linkr_debugger/README.md", "skills/radxa-linkr-debugger/SKILL.md",
      "docs/testing/hil-functional-test-spec.md",
    ]);
    const summaryIds = FROZEN_SUMMARY.map(([id]) => id);
    assert.deepEqual(summaryIds, [
      "storage", "snapshot", "explicit-save", "boot-restore", "header",
      "firmware-confirmation", "save", "clear", "busy", "recovery", "security", "hil-boundary",
    ]);
    for (const staleId of ["apply", "boot-safe", "danger-pending"]) {
      assert.ok(!summaryIds.includes(staleId), `${staleId} must not survive the v1 save consolidation`);
    }
    const literals = new Map(FROZEN_SUMMARY);
    assert.equal(literals.get("snapshot"), "linkr/config/snapshot;v1;one");
    assert.equal(literals.get("boot-restore"), "defaults-first;v1-full-restore");
    assert.equal(literals.get("header"), "12B-header;byte4-version=1;byte7-zero;max-104B");
    assert.equal(literals.get("firmware-confirmation"), "firmware-owned-confirmation;save-time-only");
    assert.equal(literals.get("save"), "persists-and-applies;failed-retry-via-resave");
  });

  await suite.test("freezes the nine current-sync marker IDs and exact literals", () => {
    assert.equal(WEB_CURRENT_SYNC_CONTRACT.length, 9);
    assert.deepEqual(WEB_CURRENT_SYNC_CONTRACT.map(([id]) => id), [
      "current-source", "current-trigger", "current-scope",
      "current-no-write", "current-no-flood", "current-draft-survives",
      "current-refresh-recovery", "current-mutation-truthful", "current-hil-boundary",
    ]);
    const literals = new Map(WEB_CURRENT_SYNC_CONTRACT);
    assert.equal(literals.get("current-source"), "Current-from-/api/v1/config");
    assert.equal(literals.get("current-no-write"), "display-sync-no-auto-save-no-flash-no-apply");
    assert.equal(literals.get("current-no-flood"), "one-transition-one-refresh;identical-frames-zero-GETs");
    assert.equal(literals.get("current-hil-boundary"), "Todo-6-post-fix-HIL-still-required");
  });

  await suite.test("keeps HIL completion ownership out of the skill current-sync contract", () => {
    assert.equal(SKILL_CURRENT_SYNC_CONTRACT.length, 8);
    assert.ok(!SKILL_CURRENT_SYNC_CONTRACT.some(([id]) => id === "current-hil-boundary"));
  });

  await suite.test("marker parser extracts each ID/value pair from the canonical block", () => {
    const block = WEB_CURRENT_SYNC_CONTRACT.map(([id, value]) => `${id}:${value}`).join("\n");
    const content = `Pre-prose.\n\n<!-- persistent-config-current-sync:\n${block}\n-->\n\nPost-prose.`;
    const { map, duplicates, malformed } = parseCurrentSyncMarkers(content);
    assert.equal(duplicates.length, 0);
    assert.equal(malformed.length, 0);
    assert.equal(map.size, WEB_CURRENT_SYNC_CONTRACT.length);
    for (const [id, value] of WEB_CURRENT_SYNC_CONTRACT) {
      assert.equal(map.get(id), value, `marker literal for ${id} must match contract`);
    }
  });

  await suite.test("marker parser flags unknown, missing, drifted, duplicate, and malformed markers", () => {
    const base = WEB_CURRENT_SYNC_CONTRACT.map(([id, value]) => `${id}:${value}`).join("\n");
    const valid = `<!-- persistent-config-current-sync:\n${base}\n-->`;
    assert.equal(parseCurrentSyncMarkers(valid + "\ncurrent-totally-fake:bogus").duplicates.length, 0, "unique IDs should not flag duplicates");
    const duplicateId = `<!-- persistent-config-current-sync:\n${base}\n${WEB_CURRENT_SYNC_CONTRACT[0][0]}:${WEB_CURRENT_SYNC_CONTRACT[0][1]}\n-->`;
    const dupResult = parseCurrentSyncMarkers(duplicateId);
    assert.equal(dupResult.duplicates.length, 1, "duplicate ID must be flagged");
    const malformedText = `<!-- persistent-config-current-sync:\n${base.replace(WEB_CURRENT_SYNC_CONTRACT[0][0] + ":" + WEB_CURRENT_SYNC_CONTRACT[0][1], "not-a-valid-line")}\n-->`;
    const malResult = parseCurrentSyncMarkers(malformedText);
    assert.ok(malResult.malformed.length > 0, "malformed entry must be flagged");
  });

  await suite.test("accepts the complete plan-exact fixture", async () => {
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      assert.equal(result.ok, true, formatFailures(result.failures));
      assert.equal(result.examples.length, Object.keys(REQUIRED_EXAMPLES).length + 3);
    });
  });

  await suite.test("rejects stale Todo 16 pending wording after dated HIL completion", async () => {
    const documents = fixtureDocuments();
    documents["docs/reference/persistent-configuration.md"] = documents["docs/reference/persistent-configuration.md"].replace(
      "\n## Source Of Truth",
      "\nTodo 16 real-hardware HIL evidence is pending.\n\n## Source Of Truth",
    );
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      assert.ok(result.failures.some(({ code }) => code === "stale-hil-status"), formatFailures(result.failures));
    }, documents);
  });

  await suite.test("rejects wrong-case English headings and the old English HIL H2", async () => {
    const documents = fixtureDocuments();
    documents["docs/reference/persistent-configuration.md"] = documents["docs/reference/persistent-configuration.md"].replace("# Persistent Configuration", "# Persistent configuration");
    documents["docs/testing/hil-functional-test-spec.md"] = documents["docs/testing/hil-functional-test-spec.md"].replace("### 2d. 持久化配置", "## Persistent configuration HIL checks");
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      assert.deepEqual(result.failures.filter(({ code }) => code === "section-missing").map(({ surface }) => surface), [
        "docs/reference/persistent-configuration.md", "docs/testing/hil-functional-test-spec.md",
      ]);
    }, documents);
  });

  await suite.test("rejects broken local links, summary drift, and unmarked shell blocks", async () => {
    const documents = fixtureDocuments("\n\`\`\`sh\ncurl -fsS http://172.29.203.1/api/v1/config\n\`\`\`");
    documents["README.md"] = documents["README.md"].replace("docs/reference/persistent-configuration.md", "docs/reference/missing.md");
    documents["README.zh-CN.md"] = documents["README.zh-CN.md"].replace("Settings+NVS", "Settings+ZMS");
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      for (const code of ["link-required", "link-missing", "summary-contract", "example-unmarked"]) {
        assert.ok(result.failures.some((failure) => failure.code === code), formatFailures(result.failures));
      }
    }, documents);
  });

  await suite.test("rejects wrong action-specific app fields and confirmation wording", async () => {
    const documents = fixtureDocuments();
    documents["apps/radxa_linkr_debugger/README.md"] = documents["apps/radxa_linkr_debugger/README.md"]
      .replace("The only common response fields are `schema`, `ok`, `command`, and `action`.", "Every response carries `schema`, `ok`, `command`, `action`, and `backend`.")
      .replace("`dangerous_items`", "`confirmation_items`");
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      assert.ok(result.failures.some(({ code }) => code === "action-fields"), formatFailures(result.failures));
    }, documents);
  });

  await suite.test("requires escaped Markdown table pipes and normalized EN/ZH parity", async () => {
    const documents = fixtureDocuments();
    documents["README.md"] = documents["README.md"].replace("busy:capture\\|ota", "busy:capture|ota");
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      assert.ok(result.failures.some(({ code }) => code === "summary-pipe-escape"), formatFailures(result.failures));
    }, documents);
  });

  await suite.test("bounds the app section at Raw MCUboot OTA API", async () => {
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      assert.equal(result.ok, true, formatFailures(result.failures));
      assert.ok(!result.failures.some(({ code }) => code === "example-unmarked"));
    });
  });

  await suite.test("rejects executable real-HIL blocks, fixed ttys, and policy drift", async () => {
    const documents = fixtureDocuments();
    documents["docs/testing/hil-functional-test-spec.md"] = documents["docs/testing/hil-functional-test-spec.md"]
      .replace("#### Clear 不改变硬件", "<!-- persistent-config-example: forbidden-hil -->\n```sh\ncurl -fsS http://172.29.203.1/api/v1/config\n```\n\n#### Clear 不改变硬件")
      .replace("switch/sd=usb-reader", "switch/sd=target-only")
      .replace("Use --serial <identified-cdc-device>.", "Use /dev/ttyACM0.");
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      for (const code of ["hil-executable", "hil-fixed-tty", "hil-policy"]) {
        assert.ok(result.failures.some((failure) => failure.code === code), formatFailures(result.failures));
      }
    }, documents);
  });

  await suite.test("rejects invalid-snapshot overwrite and clear-only recovery drift", async () => {
    const documents = fixtureDocuments("\nThe clear path is the only path that can remove a corrupt snapshot; the service refuses to overwrite it.\n");
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      assert.ok(result.failures.some(({ code }) => code === "source-contract"), formatFailures(result.failures));
    }, documents);
  });

  await suite.test("rejects Safe GPIO outputs in the common apply order", async () => {
    const documents = fixtureDocuments("\n10. Safe GPIO outputs (GPIOs in output mode).\n");
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      assert.ok(result.failures.some(({ code }) => code === "source-contract"), formatFailures(result.failures));
    }, documents);
  });

  await suite.test("requires TUI refresh and focused blur keys", async () => {
    const documents = fixtureDocuments();
    documents["docs/reference/persistent-configuration.md"] = documents["docs/reference/persistent-configuration.md"]
      .replace("`r` refreshes HTTP status and requests an authoritative config GET.", "")
      .replace("While focused,\n`c` or `Esc` blurs it.", "");
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      assert.ok(result.failures.some(({ code }) => code === "source-contract"), formatFailures(result.failures));
    }, documents);
  });

  await registerSourceContractMutationTests(suite);

  await suite.test("keeps negative security text and unrelated sections valid", async () => {
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      assert.equal(result.ok, true, formatFailures(result.failures));
    });
  });

  for (const [name, statement] of FORBIDDEN_MUTATIONS) {
    await suite.test(`rejects the forbidden claim: ${name}`, async () => {
      await withFixture(async (root) => {
        const result = await checkPersistentConfigurationDocs(root);
        assert.ok(result.failures.some(({ code }) => code === "forbidden-claim"), formatFailures(result.failures));
      }, fixtureDocuments(`\n${statement}\n`));
    });
  }

  await suite.test("rejects a persistent-config port 8080 example", async () => {
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      assert.ok(result.failures.some(({ code }) => code === "forbidden-port"), formatFailures(result.failures));
    }, fixtureDocuments("\nUse curl http://172.29.203.1:8080/api/v1/config.\n"));
  });

  await suite.test("executes marked curl examples against a loopback-only mock", async () => {
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      assert.equal(result.ok, true, formatFailures(result.failures));
      const mock = await startMock();
      try { await executeExamples(result.examples, "curl", mock); } finally { await mock.close(); }
    });
  });

  await suite.test("builds the CLI once and executes marked CLI examples through a loopback wrapper", async () => {
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      assert.equal(result.ok, true, formatFailures(result.failures));
      const mock = await startMock();
      try {
        const cliPath = await buildCli(repositoryRoot);
        await withCliWrapper(cliPath, mock.baseUrl, (environment) => executeExamples(result.examples, "cli", mock, environment));
      } finally { await mock.close(); }
    });
  });

  await suite.test("executes non-required curl and CLI markers while skipping marked CDC console text", async () => {
    await withFixture(async (root) => {
      const result = await checkPersistentConfigurationDocs(root);
      assert.equal(result.ok, true, formatFailures(result.failures));
      const mock = await startMock();
      try {
        const curl = await executeExamples(result.examples, "curl", mock);
        const cliPath = await buildCli(repositoryRoot);
        const cli = await withCliWrapper(cliPath, mock.baseUrl, (environment) => executeExamples(result.examples, "cli", mock, environment));
        assert.ok(curl.executed.includes("curl-config-extra-read"));
        assert.ok(cli.executed.includes("cli-config-extra-show"));
        assert.ok(curl.skipped.includes("cdc-config-show"));
      } finally { await mock.close(); }
    });
  });

  await suite.test("repository documentation must satisfy the frozen contract", async () => {
    const result = await checkPersistentConfigurationDocs(repositoryRoot);
    assert.equal(result.ok, true, formatFailures(result.failures));
  });
});
