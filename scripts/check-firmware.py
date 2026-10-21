#!/usr/bin/env python3
"""Verify the firmware files against the SHA-256 sums in config/sources.json.
Offline; never fetches, regenerates or repairs them."""
import hashlib
import json
from pathlib import Path
import sys


def check(root):
    firmware = json.loads((root / "config/sources.json").read_text())["sources"]["firmware"]
    for name in ("amss.bin", "m3.bin", "board-2.bin"):
        if name not in firmware["files"]:
            raise ValueError(f"config/sources.json declares no {name}")
    for name, digest in firmware["files"].items():
        source = root / firmware["directory"] / name
        if source.is_symlink() or hashlib.sha256(source.read_bytes()).hexdigest() != digest:
            raise ValueError(f"checksum mismatch: {source.relative_to(root)}; inspect it, do not overwrite automatically")
    print(f"firmware: {len(firmware['files'])} files verified")


if __name__ == "__main__":
    try:
        check(Path(__file__).resolve().parent.parent)
    except (OSError, ValueError, KeyError) as error:
        print(f"check-firmware: {error}", file=sys.stderr)
        sys.exit(1)
