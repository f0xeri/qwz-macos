#!/usr/bin/env python3
"""Read dependency settings from config/sources.json; used by shell scripts."""
import argparse
import json
from pathlib import Path
import re
import tempfile
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("get", "list", "paths", "file-url", "set-revision"))
    parser.add_argument("args", nargs="+")
    args = parser.parse_args()
    path = Path(__file__).resolve().parent.parent / "config/sources.json"
    data = json.loads(path.read_text())
    if args.command == "get":
        if len(args.args) != 1:
            parser.error("get requires one dotted key")
        value = data
        for key in args.args[0].split("."):
            value = value[key]
        print("\n".join(value) if isinstance(value, (dict, list)) else value)
    elif args.command == "list":
        if len(args.args) != 1 or args.args[0] not in ("build", "import"):
            parser.error("list requires build or import")
        print("\n".join(name for name, entry in data["sources"].items()
                        if entry["usage"] == args.args[0]))
    elif args.command == "paths":
        if len(args.args) != 1:
            parser.error("paths requires a source name")
        source = data["sources"][args.args[0]]
        if "manifest" in source:
            manifest = path.parent.parent / source["manifest"]
            paths = ["/" + line for line in manifest.read_text().splitlines()
                     if line and not line.startswith("#")]
        else:
            paths = source.get("paths", [])
        print("\n".join(paths))
    elif args.command == "file-url":
        if len(args.args) != 2:
            parser.error("file-url requires a source name and filename")
        source = data["sources"][args.args[0]]
        name = args.args[1]
        if name not in source["files"]:
            parser.error("file is not declared")
        print(f'{source["url"]}/{source["revision"]}/{source["path"]}/{name}')
    else:
        if len(args.args) != 2 or not re.fullmatch(r"[0-9a-f]{40}", args.args[1]):
            parser.error("set-revision requires a source name and full commit SHA")
        data["sources"][args.args[0]]["revision"] = args.args[1]
        tmp = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as out:
                tmp = Path(out.name)
                out.write(json.dumps(data, indent=2) + "\n")
            tmp.chmod(path.stat().st_mode & 0o777)
            tmp.replace(path)
        finally:
            if tmp is not None:
                tmp.unlink(missing_ok=True)



if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        print(f"sources: {error}", file=sys.stderr)
        sys.exit(1)
