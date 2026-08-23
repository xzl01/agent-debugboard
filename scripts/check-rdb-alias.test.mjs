import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import {
  RDB_POLICY,
  checkRdbAliasContents,
  formatFailures,
} from "./check-rdb-alias.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

async function repositoryContents() {
  return new Map(
    await Promise.all(
      Object.keys(RDB_POLICY).map(async (relativePath) => [
        relativePath,
        await readFile(path.join(ROOT, relativePath), "utf8"),
      ]),
    ),
  );
}

test("current distribution exposes the rdb alias on every supported path", async () => {
  const result = checkRdbAliasContents(await repositoryContents());
  assert.equal(result.ok, true, formatFailures(result.failures));
});

test("requires the flake check to validate the package-local relative alias and version parity", () => {
  assert.deepEqual(RDB_POLICY["flake.nix"], [
    'program = "${pkgs.radxa-linkr-debuggerctl}/bin/rdb";',
    'expected_target="radxa-linkr-debuggerctl"',
    'actual_target="$("${pkgs.coreutils}/bin/readlink" "${pkgs.radxa-linkr-debuggerctl}/bin/rdb")"',
    'test "$actual_target" = "$expected_target"',
    'primary_version="$TMPDIR/primary-version"',
    'rdb_version="$TMPDIR/rdb-version"',
    '"${pkgs.radxa-linkr-debuggerctl}/bin/radxa-linkr-debuggerctl" --version > "$primary_version"',
    '"${pkgs.radxa-linkr-debuggerctl}/bin/rdb" --version > "$rdb_version"',
    'cmp "$primary_version" "$rdb_version"',
  ]);
});

test("removing any required alias operation fails the distribution contract", async (t) => {
  const baseline = await repositoryContents();
  for (const [relativePath, fragments] of Object.entries(RDB_POLICY)) {
    for (const fragment of fragments) {
      await t.test(`${relativePath}: ${fragment}`, () => {
        const mutated = new Map(baseline);
        mutated.set(relativePath, mutated.get(relativePath).replace(fragment, ""));
        const result = checkRdbAliasContents(mutated);
        assert.equal(result.ok, false);
        assert.ok(result.failures.some((failure) => failure.startsWith(`${relativePath}:`)));
      });
    }
  }
});
