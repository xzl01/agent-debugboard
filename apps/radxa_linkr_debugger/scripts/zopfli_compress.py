#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
"""Atomically publish verified, reproducible gzip from Zopfli 1.0.3."""

import argparse
import gzip
import hashlib
from pathlib import Path
import subprocess
import tempfile
import zlib


def validate_gzip(compressed: bytes, source: bytes) -> None:
    # Forbid optional headers (including source filenames) and timestamps.
    if compressed[:8] != b"\x1f\x8b\x08\x00\x00\x00\x00\x00":
        raise ValueError("gzip must have no optional headers and mtime=0")
    if gzip.decompress(compressed) != source:
        raise ValueError("gzip decompression does not match the source bytes")


def compress(zopfli: str, source: Path, output: Path) -> None:
    source = source.resolve(strict=True)
    if source == output.resolve():
        raise ValueError("gzip output must not overwrite the source")
    original = source.read_bytes()
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [zopfli, "--i15", "-c", str(source)]
    # A sibling temporary file keeps replace atomic, even on separate mounts.
    temporary = tempfile.NamedTemporaryFile(
        dir=output.parent, prefix=f".{output.name}.", suffix=".tmp", delete=False
    )
    temporary_path = Path(temporary.name)
    try:
        with temporary:
            subprocess.run(command, stdout=temporary, check=True)
        compressed = temporary_path.read_bytes()
        validate_gzip(compressed, original)
        repeated = subprocess.run(command, stdout=subprocess.PIPE, check=True).stdout
        validate_gzip(repeated, original)
        if hashlib.sha256(compressed).digest() != hashlib.sha256(repeated).digest():
            raise ValueError("repeated Zopfli compression is not reproducible")
        temporary_path.replace(output)
    finally:
        temporary_path.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zopfli", required=True, help="Zopfli 1.0.3 executable")
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        compress(args.zopfli, args.input, args.output)
    except (OSError, ValueError, EOFError, zlib.error, subprocess.CalledProcessError) as error:
        parser.exit(1, f"zopfli compression failed: {error}\n")


if __name__ == "__main__":
    main()
