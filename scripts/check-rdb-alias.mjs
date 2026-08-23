import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

export const RDB_POLICY = Object.freeze({
  ".github/workflows/build.yml": [
    'ln -s radxa-linkr-debuggerctl "$stage_dir/rdb"',
    'New-Item -ItemType HardLink -Path (Join-Path $stageDir "rdb.exe")',
  ],
  ".github/workflows/release.yml": [
    'ln -s radxa-linkr-debuggerctl "$stage_dir/rdb"',
    'New-Item -ItemType HardLink -Path (Join-Path $stageDir "rdb.exe")',
  ],
  ".github/workflows/nightly.yml": [
    'ln -s radxa-linkr-debuggerctl "$stage_dir/rdb"',
    'New-Item -ItemType HardLink -Path (Join-Path $stageDir "rdb.exe")',
  ],
  "skills/radxa-linkr-debugger/scripts/install.sh": [
    'alias_path="$install_dir/rdb"',
    'ln -s radxa-linkr-debuggerctl "$alias_path"',
  ],
  "skills/radxa-linkr-debugger/scripts/install.ps1": [
    '$aliasTarget = Join-Path $installDir "rdb.exe"',
    'New-Item -ItemType HardLink -Path $aliasTarget -Target $target',
  ],
  "nix/package.nix": ['ln -s radxa-linkr-debuggerctl "$out/bin/rdb"'],
  "flake.nix": [
    'program = "${pkgs.radxa-linkr-debuggerctl}/bin/rdb";',
    'expected_target="radxa-linkr-debuggerctl"',
    'actual_target="$("${pkgs.coreutils}/bin/readlink" "${pkgs.radxa-linkr-debuggerctl}/bin/rdb")"',
    'test "$actual_target" = "$expected_target"',
    'primary_version="$TMPDIR/primary-version"',
    'rdb_version="$TMPDIR/rdb-version"',
    '"${pkgs.radxa-linkr-debuggerctl}/bin/radxa-linkr-debuggerctl" --version > "$primary_version"',
    '"${pkgs.radxa-linkr-debuggerctl}/bin/rdb" --version > "$rdb_version"',
    'cmp "$primary_version" "$rdb_version"',
  ],
});

export function checkRdbAliasContents(contents) {
  const failures = [];
  for (const [relativePath, fragments] of Object.entries(RDB_POLICY)) {
    const source = contents.get(relativePath) ?? "";
    for (const fragment of fragments) {
      if (!source.includes(fragment)) {
        failures.push(`${relativePath}: missing ${JSON.stringify(fragment)}`);
      }
    }
  }
  return { ok: failures.length === 0, failures };
}

export async function checkRdbAlias(repositoryRoot) {
  const root = path.resolve(repositoryRoot);
  const contents = new Map(
    await Promise.all(
      Object.keys(RDB_POLICY).map(async (relativePath) => [
        relativePath,
        await readFile(path.join(root, relativePath), "utf8"),
      ]),
    ),
  );
  return checkRdbAliasContents(contents);
}

export function formatFailures(failures) {
  return ["rdb alias check failed:", ...failures.map((failure) => `- ${failure}`)].join("\n");
}

async function main() {
  const args = process.argv.slice(2);
  if (args.length !== 2 || args[0] !== "--root") {
    console.error("usage: node scripts/check-rdb-alias.mjs --root <repository-root>");
    process.exitCode = 2;
    return;
  }
  const result = await checkRdbAlias(args[1]);
  if (!result.ok) {
    console.error(formatFailures(result.failures));
    process.exitCode = 1;
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) await main();
