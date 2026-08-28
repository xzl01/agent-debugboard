# Versioning and Release Gate

[中文](versioning.zh-CN.md)

The root [`VERSION`](../../VERSION) file is the single source of truth for the
project release version. Do not edit Cargo, npm, WASM, or Nix version fields by
hand.

## Update the Version

From the repository root, run:

```sh
python3 scripts/version_sync.py set 0.3.0
python3 scripts/version_sync.py check
```

The `set` command updates all managed fields and then runs the same consistency
check used by CI. Versions must be SemVer values without a leading `v`.
Prerelease values such as `0.3.0-rc.1` are supported. Before writing the
firmware field, `set` applies the same Zephyr representability checks used by
CI: each numeric field must be `0..255`, `VERSION_TWEAK` must be `0`, and the
prerelease suffix must contain only lowercase `[a-z0-9.-]`.

The **Version Bump** GitHub Actions workflow provides the same operation for
maintainers. Run it manually, choose `dev` or `main` as the base branch, and it
will create a pull request containing only the synchronized version files.

## Managed Fields

The script keeps these files synchronized:

- `VERSION`
- `cmd-ng/Cargo.toml` and the CLI package entry in `cmd-ng/Cargo.lock`
- `web/package.json` and both root fields in `web/package-lock.json`
- `web/decoder/Cargo.toml` and the decoder entry in `web/decoder/Cargo.lock`
- `nix/package.nix`
- `apps/radxa_linkr_debugger/VERSION` (Zephyr application version)

## CI and Release Behavior

The Build workflow runs the version-tool unit tests and consistency gate before
starting any platform build. If a managed field differs from `VERSION`, the
gate prints every mismatch and all downstream jobs are skipped.

The Release workflow runs an additional tag gate. A release proceeds only when
the tag is exactly `v<VERSION>`; for example, `VERSION=0.3.0` requires the tag
`v0.3.0`.
