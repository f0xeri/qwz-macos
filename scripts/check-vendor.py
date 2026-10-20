#!/usr/bin/env python3
"""Verify build inputs offline; never fetch, regenerate or repair them."""
import hashlib
import json
import argparse
from pathlib import Path
import sys


def check(root, firmware_only=False):
    config = json.loads((root / "config/sources.json").read_text())
    firmware = config["sources"]["firmware"]
    inputs = {firmware["directory"] + "/" + name: sha for name, sha in firmware["files"].items()}
    for name in (() if firmware_only else ("airport/abi/SHA256SUMS",)):
        for line in (root / name).read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            digest, path = line.split(maxsplit=1)
            if path in inputs:
                raise ValueError(f"duplicate input: {path}")
            inputs[path] = digest
    required = {"firmware/amss.bin", "firmware/m3.bin", "firmware/board-2.bin"}
    if not firmware_only:
        required |= {"airport/WCL.hpp", "airport/abi/UPSTREAM", "airport/abi/LICENSE",
                     "airport/abi/qwzairport.patch"}
    if not required <= inputs.keys():
        raise ValueError("input manifest is missing a required firmware or vendor file")
    if not firmware_only:
        abi = root / "airport/abi"
        expected = {path for path in inputs if path.startswith("airport/abi/")}
        actual = {p.relative_to(root).as_posix() for p in abi.rglob("*")
                  if p.is_file() and p != abi / "SHA256SUMS"}
        if actual != expected or any(p.is_symlink() for p in abi.rglob("*")):
            raise ValueError("airport/abi inventory differs from SHA256SUMS; inspect before reimporting")
        imported = (abi / "UPSTREAM").read_text().splitlines()[0].split()[1]
        if imported != config["sources"]["laobamac"]["revision"]:
            raise ValueError("ABI import revision differs from config/sources.json; review and reimport")
    for path, digest in inputs.items():
        source = root / path
        if source.is_symlink() or hashlib.sha256(source.read_bytes()).hexdigest() != digest:
            raise ValueError(f"checksum mismatch: {path}; inspect it, do not overwrite automatically")
    print(f"vendor inputs: {len(inputs)} files verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware-only", action="store_true", help="verify downloaded firmware before ABI import")
    args = parser.parse_args()
    try:
        check(Path(__file__).resolve().parent.parent, args.firmware_only)
    except (OSError, ValueError, KeyError, IndexError) as error:
        print(f"check-vendor: {error}", file=sys.stderr)
        sys.exit(1)
