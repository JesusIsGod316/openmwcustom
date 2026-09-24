#!/usr/bin/env python3
"""Verify the committed P2 source identities with the one FFPB setting addition."""

import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "tools/opimizedmw/gl-p2/source-manifest.json"
SETTING = "files/settings-default.cfg"
ANCHOR = b"v3.21 full body first person = false\n"
ADDITION = b"full body first person hybrid animations = false\n"


def git(*args):
    return subprocess.check_output(("git", *args), cwd=ROOT)


def main():
    files = json.loads(MANIFEST.read_text(encoding="utf-8"))["files"]
    for path, identity in files.items():
        original = identity["after"]
        if path == SETTING:
            baseline = git("cat-file", "blob", original)
            if baseline.count(ANCHOR) != 1:
                raise RuntimeError("P2 settings anchor is missing or ambiguous")
            expected = baseline.replace(ANCHOR, ANCHOR + ADDITION, 1)
            actual = git("show", "HEAD:" + path)
            if actual != expected:
                raise RuntimeError("FFPB setting is not the sole change to P2 settings")
        elif git("rev-parse", "HEAD:" + path).decode().strip() != original:
            raise RuntimeError("P2 source identity changed: " + path)
    print("Committed P2 source identities and exact FFPB settings addition verified.")


if __name__ == "__main__":
    main()
