#!/usr/bin/env python3
"""Create a self-contained, restore-tested backup of committed source.

Never deletes source, rewrites history, or includes untracked worktree files.
Run in a clean checkout; preserve any local WIP separately before using cleanup.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

BASE = "be2869dd00a49774ff2642ec238d98f1456b2b8a"
REFERENCE = "40227915a9069d74743eb1a344e28cdeb9ee68d6"
TAGS = {
    "opimizedmw-gl-p0-precleanup-20260924": BASE,
    "archive/vulkan-retained-20260924-be2869": BASE,
    "archive/opimizedmw-opengl-reference-20260924": REFERENCE,
}


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(root), *args], check=True,
                          text=True, stdout=subprocess.PIPE).stdout.strip()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def create_archive(root: Path, out: Path) -> dict:
    root, out = root.resolve(), out.resolve()
    if git(root, "rev-parse", "--is-shallow-repository") != "false":
        raise RuntimeError("A shallow checkout is not an archival source")
    if git(root, "status", "--porcelain", "--untracked-files=all"):
        raise RuntimeError("Checkout is dirty: preserve reviewed WIP before archival")
    if out == root or root in out.parents:
        raise RuntimeError("Archive output must be outside the source checkout")
    out.mkdir(parents=True, exist_ok=False)
    records = []
    for name, commit in TAGS.items():
        git(root, "cat-file", "-e", commit + "^{commit}")
        ref = "refs/tags/" + name
        exists = subprocess.run(["git", "-C", str(root), "show-ref", "--verify", "--quiet", ref]).returncode == 0
        if not exists:
            git(root, "-c", "user.name=OpimizedMW archive", "-c", "user.email=opimizedmw@users.noreply.github.com",
                "tag", "-a", name, commit, "-m", "Preserved before OpimizedMW GL-P0 cleanup. Source only; not runtime acceptance.")
        if git(root, "cat-file", "-t", ref) != "tag" or git(root, "rev-parse", ref + "^{}") != commit:
            raise RuntimeError("Existing archive tag does not match: " + name)
        records.append({"tag": name, "tag_object": git(root, "rev-parse", ref),
                        "commit": commit, "tree": git(root, "rev-parse", commit + "^{tree}")})
    bundle = out / "OpimizedMW-GL-P0-precleanup.bundle"
    git(root, "bundle", "create", str(bundle), *("refs/tags/" + name for name in TAGS))
    # Verify in an EMPTY repository: this rejects hidden prerequisite objects.
    with tempfile.TemporaryDirectory(prefix="opimizedmw-restore-") as temp:
        restored = Path(temp) / "source"
        restored.mkdir()
        git(restored, "init", "--quiet")
        verification = git(restored, "bundle", "verify", str(bundle))
        git(restored, "fetch", "--quiet", str(bundle), "refs/tags/*:refs/tags/*")
        git(restored, "fsck", "--full", "--strict")
        for item in records:
            if git(restored, "rev-parse", item["tag"] + "^{}") != item["commit"]:
                raise RuntimeError("Restored commit mismatch")
            if git(restored, "rev-parse", item["commit"] + "^{tree}") != item["tree"]:
                raise RuntimeError("Restored tree mismatch")
        git(restored, "checkout", "--quiet", "--detach", BASE)
        git(restored, "diff", "--exit-code")
        gate = restored / "tools/v4/V4-CP0A-Verify-Materialized-Generated-Outputs.py"
        subprocess.run(["python3", str(gate)], cwd=restored, check=True)
        git(restored, "diff", "--exit-code")
        inventory = git(restored, "ls-tree", "-r", "-l", "--full-tree", BASE)
        (out / "source-tree.txt").write_text(inventory + "\n", encoding="utf-8")
        gitlinks = [line for line in inventory.splitlines() if line.startswith("160000 ")]
        # Record external dependency declarations separately; do not claim that
        # git stores downloaded SDK binaries or uncommitted Windows worktrees.
        dependencies = {}
        for path in (".gitmodules", ".gitattributes", "CI/deps_versions.msvc.sh", "vcpkg.json", "vcpkg-configuration.json"):
            file = restored / path
            if file.is_file():
                dependencies[path] = {"sha256": sha256(file), "content": file.read_text(encoding="utf-8")}
        (out / "dependency-provenance.json").write_text(json.dumps(dependencies, indent=2) + "\n", encoding="utf-8")
    result = {"project": "OpimizedMW", "stage": "GL-P0", "source": BASE,
              "references": records, "bundle": bundle.name, "bundle_sha256": sha256(bundle),
              "bundle_bytes": bundle.stat().st_size, "empty_repository_restore": "PASS",
              "git_fsck": "PASS", "materialized_source_verifier": "PASS",
              "gitlinks": gitlinks, "bundle_verification": verification,
              "scope": "Committed named lineages and their ancestors, not all remote branches",
              "local_windows_worktrees": "NOT_ACCESSED_NOT_MODIFIED_NOT_BACKED_UP",
              "external_dependency_binaries": "NOT_INCLUDED; reconstruct using pinned recipe",
              "production_build": "NOT_RUN_BY_ARCHIVE_SCRIPT", "runtime_acceptance": False}
    (out / "archive-manifest.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    (out / "RESTORE.md").write_text(
        "# OpimizedMW GL-P0 source recovery\n\n"
        "Verify SHA256SUMS first. From a new directory run:\n\n```sh\n"
        "git init restored\n"
        "git -C restored bundle verify ../OpimizedMW-GL-P0-precleanup.bundle\n"
        "git -C restored fetch ../OpimizedMW-GL-P0-precleanup.bundle 'refs/tags/*:refs/tags/*'\n"
        "git -C restored switch -c recovered opimizedmw-gl-p0-precleanup-20260924\n"
        "git -C restored fsck --full --strict\n```\n\n"
        "Full Windows recipe: `.github/workflows/windows.yml` and `CI/deps_versions.msvc.sh` "
        "at the preserved commit. Vulkan runtime must be OFF for the OpenGL baseline. "
        "Do not run historical V3 patch generators over the materialized source.\n\n"
        "This backup includes committed repository history only. It does not preserve "
        "uncommitted/untracked Windows worktrees, downloaded dependencies, local game data, "
        "private settings or saves. Gitlink dependencies, if any, are listed in the manifest. "
        "A successful source restore is not a production build or gameplay test.\n", encoding="utf-8")
    sums = "".join(f"{sha256(path)}  {path.name}\n" for path in sorted(out.iterdir()) if path.is_file())
    (out / "SHA256SUMS").write_text(sums, encoding="utf-8")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path.cwd())
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(create_archive(args.source, args.output), indent=2))


if __name__ == "__main__":
    main()
