#!/usr/bin/env python3
"""Synchronize and validate every project release version."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Sequence


PROJECT_ROOT = Path(__file__).resolve().parents[1]
SEMVER_PATTERN = re.compile(
    r"^(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)"
    r"(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$"
)


class VersionSyncError(RuntimeError):
    """Raised when a version file cannot be read or updated safely."""


def validate_version(version: str) -> str:
    if not SEMVER_PATTERN.fullmatch(version):
        raise VersionSyncError(
            f"invalid version {version!r}; expected SemVer without a leading 'v' "
            "(for example, 0.3.0 or 0.3.0-rc.1)"
        )
    return version


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise VersionSyncError(f"cannot read {path}: {error}") from error


def package_section(text: str, path: Path) -> re.Match[str]:
    match = re.search(
        r"(?ms)^\[package\][^\n]*\n.*?(?=^\[[^\n]+\]|\Z)", text
    )
    if match is None:
        raise VersionSyncError(f"{path}: missing [package] section")
    return match


def cargo_manifest_version(path: Path) -> str:
    block = package_section(read_text(path), path).group(0)
    matches = re.findall(r'(?m)^version\s*=\s*"([^"]+)"\s*$', block)
    if len(matches) != 1:
        raise VersionSyncError(f"{path}: expected one [package].version field")
    return matches[0]


def update_cargo_manifest(path: Path, version: str) -> str:
    text = read_text(path)
    section = package_section(text, path)
    block = section.group(0)
    updated, count = re.subn(
        r'(?m)^(version\s*=\s*")[^"]+("\s*)$',
        lambda match: f"{match.group(1)}{version}{match.group(2)}",
        block,
    )
    if count != 1:
        raise VersionSyncError(f"{path}: expected one [package].version field")
    return text[: section.start()] + updated + text[section.end() :]


def cargo_lock_block(text: str, path: Path, package_name: str) -> re.Match[str]:
    matches = []
    for block in re.finditer(
        r"(?ms)^\[\[package\]\][^\n]*\n.*?(?=^\[\[package\]\]|\Z)", text
    ):
        names = re.findall(r'(?m)^name\s*=\s*"([^"]+)"\s*$', block.group(0))
        if names == [package_name]:
            matches.append(block)
    if len(matches) != 1:
        raise VersionSyncError(
            f"{path}: expected one [[package]] entry named {package_name!r}"
        )
    return matches[0]


def cargo_lock_version(path: Path, package_name: str) -> str:
    text = read_text(path)
    block = cargo_lock_block(text, path, package_name).group(0)
    matches = re.findall(r'(?m)^version\s*=\s*"([^"]+)"\s*$', block)
    if len(matches) != 1:
        raise VersionSyncError(
            f"{path}: expected one version for package {package_name!r}"
        )
    return matches[0]


def update_cargo_lock(path: Path, package_name: str, version: str) -> str:
    text = read_text(path)
    package = cargo_lock_block(text, path, package_name)
    block = package.group(0)
    updated, count = re.subn(
        r'(?m)^(version\s*=\s*")[^"]+("\s*)$',
        lambda match: f"{match.group(1)}{version}{match.group(2)}",
        block,
    )
    if count != 1:
        raise VersionSyncError(
            f"{path}: expected one version for package {package_name!r}"
        )
    return text[: package.start()] + updated + text[package.end() :]


def load_json(path: Path) -> dict:
    try:
        value = json.loads(read_text(path))
    except json.JSONDecodeError as error:
        raise VersionSyncError(f"{path}: invalid JSON: {error}") from error
    if not isinstance(value, dict):
        raise VersionSyncError(f"{path}: expected a JSON object")
    return value


def json_version(path: Path) -> str:
    value = load_json(path).get("version")
    if not isinstance(value, str):
        raise VersionSyncError(f"{path}: expected a string version field")
    return value


def package_lock_versions(path: Path) -> list[tuple[str, str]]:
    value = load_json(path)
    root_version = value.get("version")
    packages = value.get("packages")
    root_package = packages.get("") if isinstance(packages, dict) else None
    package_version = (
        root_package.get("version") if isinstance(root_package, dict) else None
    )
    if not isinstance(root_version, str) or not isinstance(package_version, str):
        raise VersionSyncError(
            f"{path}: expected string versions at version and packages[''].version"
        )
    return [
        ("web/package-lock.json:version", root_version),
        ("web/package-lock.json:packages[''].version", package_version),
    ]


def update_json_version(path: Path, version: str) -> str:
    value = load_json(path)
    if not isinstance(value.get("version"), str):
        raise VersionSyncError(f"{path}: expected a string version field")
    value["version"] = version
    return json.dumps(value, ensure_ascii=False, indent=2) + "\n"


def update_package_lock(path: Path, version: str) -> str:
    value = load_json(path)
    packages = value.get("packages")
    root_package = packages.get("") if isinstance(packages, dict) else None
    if not isinstance(value.get("version"), str) or not isinstance(root_package, dict):
        raise VersionSyncError(
            f"{path}: expected version and packages[''] fields"
        )
    if not isinstance(root_package.get("version"), str):
        raise VersionSyncError(f"{path}: expected packages[''].version string")
    value["version"] = version
    root_package["version"] = version
    return json.dumps(value, ensure_ascii=False, indent=2) + "\n"


def nix_version(path: Path) -> str:
    matches = re.findall(r'(?m)^\s*version\s*=\s*"([^"]+)";\s*$', read_text(path))
    if len(matches) != 1:
        raise VersionSyncError(f"{path}: expected one version assignment")
    return matches[0]


def update_nix_version(path: Path, version: str) -> str:
    text = read_text(path)
    updated, count = re.subn(
        r'(?m)^(\s*version\s*=\s*")[^"]+(";\s*)$',
        lambda match: f"{match.group(1)}{version}{match.group(2)}",
        text,
    )
    if count != 1:
        raise VersionSyncError(f"{path}: expected one version assignment")
    return updated


def expected_version(root: Path) -> str:
    version = read_text(root / "VERSION").strip()
    return validate_version(version)


def managed_versions(root: Path) -> list[tuple[str, str]]:
    versions = [
        (
            "cmd-ng/Cargo.toml:[package].version",
            cargo_manifest_version(root / "cmd-ng/Cargo.toml"),
        ),
        (
            "cmd-ng/Cargo.lock:radxa-linkr-debuggerctl-ng",
            cargo_lock_version(
                root / "cmd-ng/Cargo.lock", "radxa-linkr-debuggerctl-ng"
            ),
        ),
        ("web/package.json:version", json_version(root / "web/package.json")),
    ]
    versions.extend(package_lock_versions(root / "web/package-lock.json"))
    versions.extend(
        [
            (
                "web/decoder/Cargo.toml:[package].version",
                cargo_manifest_version(root / "web/decoder/Cargo.toml"),
            ),
            (
                "web/decoder/Cargo.lock:radxa-logic-decoder",
                cargo_lock_version(
                    root / "web/decoder/Cargo.lock", "radxa-logic-decoder"
                ),
            ),
            ("nix/package.nix:version", nix_version(root / "nix/package.nix")),
        ]
    )
    return versions


def check_versions(root: Path, tag: str | None = None) -> bool:
    version = expected_version(root)
    fields = managed_versions(root)
    mismatches = [
        (location, actual, version)
        for location, actual in fields
        if actual != version
    ]

    if tag is not None and tag != f"v{version}":
        mismatches.append(("release tag", tag, f"v{version}"))

    if mismatches:
        print(f"Version gate failed; VERSION contains {version}:", file=sys.stderr)
        for location, actual, wanted in mismatches:
            print(
                f"  - {location}: found {actual!r}, expected {wanted!r}",
                file=sys.stderr,
            )
        return False

    suffix = f" and release tag {tag}" if tag is not None else ""
    print(f"Version gate passed: {version} across {len(fields)} fields{suffix}")
    return True


def version_updates(root: Path, version: str) -> dict[Path, str]:
    validate_version(version)
    package_json_path = root / "web/package.json"
    package_lock_path = root / "web/package-lock.json"
    return {
        root / "VERSION": f"{version}\n",
        root / "cmd-ng/Cargo.toml": update_cargo_manifest(
            root / "cmd-ng/Cargo.toml", version
        ),
        root / "cmd-ng/Cargo.lock": update_cargo_lock(
            root / "cmd-ng/Cargo.lock", "radxa-linkr-debuggerctl-ng", version
        ),
        package_json_path: update_json_version(package_json_path, version),
        package_lock_path: update_package_lock(package_lock_path, version),
        root / "web/decoder/Cargo.toml": update_cargo_manifest(
            root / "web/decoder/Cargo.toml", version
        ),
        root / "web/decoder/Cargo.lock": update_cargo_lock(
            root / "web/decoder/Cargo.lock", "radxa-logic-decoder", version
        ),
        root / "nix/package.nix": update_nix_version(
            root / "nix/package.nix", version
        ),
    }


def set_version(root: Path, version: str) -> bool:
    updates = version_updates(root, version)
    changed = []
    for path, content in updates.items():
        if read_text(path) != content:
            changed.append(path)

    for path in changed:
        path.write_text(updates[path], encoding="utf-8")

    if changed:
        print(f"Updated project version to {version}:")
        for path in changed:
            print(f"  - {path.relative_to(root)}")
    else:
        print(f"Project version is already {version}; no files changed")
    return check_versions(root)


def parser() -> argparse.ArgumentParser:
    argument_parser = argparse.ArgumentParser(description=__doc__)
    argument_parser.add_argument(
        "--root",
        type=Path,
        default=PROJECT_ROOT,
        help=argparse.SUPPRESS,
    )
    subparsers = argument_parser.add_subparsers(dest="command", required=True)

    check_parser = subparsers.add_parser(
        "check", help="fail if a managed version differs from VERSION"
    )
    check_parser.add_argument(
        "--tag", help="also require an exact v<VERSION> release tag"
    )

    set_parser = subparsers.add_parser(
        "set", help="update every managed version and VERSION"
    )
    set_parser.add_argument("version", help="new version without a leading v")
    return argument_parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    root = arguments.root.resolve()
    try:
        if arguments.command == "check":
            return 0 if check_versions(root, arguments.tag) else 1
        if arguments.command == "set":
            return 0 if set_version(root, arguments.version) else 1
    except VersionSyncError as error:
        print(f"Version sync error: {error}", file=sys.stderr)
        return 1
    raise AssertionError(f"unsupported command: {arguments.command}")


if __name__ == "__main__":
    raise SystemExit(main())
