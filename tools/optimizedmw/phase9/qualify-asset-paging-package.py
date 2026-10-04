#!/usr/bin/env python3
"""Freeze real package inputs and a retained private shader mutation workspace.

This prepares evidence, not an engine/certificate substitute or release PASS.
Only a new workspace is written. Package/config/settings inputs are read-only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
from datetime import datetime, timezone

SCHEMA = "optimizedmw.asset-paging-package-input.v1"
MAX_FILES = 20000
MAX_BYTES = 512 * 1024 * 1024
MAX_FILE_BYTES = 64 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def linked(path):
    return path.is_symlink() or getattr(path, "is_junction", lambda: False)()


def fingerprint(path):
    path = Path(path)
    require(path.is_file() and not linked(path), f"Missing or linked input: {path}")
    before = path.stat()
    require(before.st_size <= MAX_FILE_BYTES, f"Input exceeds bounded file size: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    after = path.stat()
    require((before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns),
            f"Input changed while hashing: {path}")
    return {"bytes": after.st_size, "sha256": digest.hexdigest()}


def inventory(root):
    root = Path(root)
    require(root.is_dir() and not linked(root), f"Missing or linked resource tree: {root}")
    result, folded, total = {}, set(), 0
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in dirs:
            require(not linked(Path(directory) / name), "Linked resource directory is unsupported")
        for name in sorted(files):
            path = Path(directory) / name
            relative = path.relative_to(root).as_posix()
            require(relative.casefold() not in folded, f"Case-colliding resource identity: {relative}")
            folded.add(relative.casefold())
            result[relative] = fingerprint(path)
            total += result[relative]["bytes"]
            require(len(result) <= MAX_FILES and total <= MAX_BYTES, "Resource inventory exceeds bounded population")
    require(result, "Resource inventory is empty")
    return dict(sorted(result.items()))


def input_files(package, config, settings):
    return {
        "defaults": {"path": str(package / "defaults.bin"), **fingerprint(package / "defaults.bin")},
        "version": {"path": str(package / "resources" / "version"), **fingerprint(package / "resources" / "version")},
        "config": {"path": str(config), **fingerprint(config)},
        "settings": {"path": str(settings), **fingerprint(settings)},
    }


def prepare(package, config, settings, workspace):
    package, config, settings, workspace = (Path(p).absolute() for p in (package, config, settings, workspace))
    require(not workspace.exists(), "Workspace already exists; retain it and choose a fresh explicit path")
    resources = package / "resources"
    immutable = input_files(package, config, settings)
    tree = inventory(resources)
    shader_source = resources / "shaders"
    shaders = inventory(shader_source)
    for required in ("compatibility/objects.vert", "compatibility/objects.frag", "lib/core/vertex.glsl"):
        require(required in shaders, f"Missing actual object shader dependency: {required}")
    require(not workspace.is_relative_to(package) and not package.is_relative_to(workspace),
            "Mutation workspace must be separate from the assembled package")
    workspace.mkdir(parents=True)
    mirror = workspace / "shaders"
    # Copy only verified shader files, never linked directory content. Preserve
    # all ordinary input timestamps and leave mutations/receipts for review.
    for name in shaders:
        destination = mirror / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(shader_source / name, destination)
    require(inventory(mirror) == shaders, "Shader mirror does not exactly match the assembled package")
    require(inventory(resources) == tree and input_files(package, config, settings) == immutable,
            "Immutable package/captured inputs changed during workspace preparation")
    receipt = {
        "schema": SCHEMA,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "status": "PACKAGE_INPUTS_FROZEN_NOT_RENDER_QUALIFIED",
        "package_root": str(package),
        "resources": str(resources),
        "shader_source": str(shader_source),
        "shader_workspace": str(mirror),
        "immutable_inputs": immutable,
        "resource_files": tree,
        "shader_files": shaders,
        "shader_copy_exact": True,
        "capture_modified": False,
        "synthetic_shader_certificate": False,
        "source_defaults_substituted": False,
        "performance_acceptance": "PENDING_USER_GAMEPLAY",
    }
    target = workspace / "PACKAGE-QUALIFICATION-INPUT.json"
    target.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return target


def verify(receipt_path, read_only=False):
    receipt_path = Path(receipt_path)
    require(receipt_path.stat().st_size <= 16 * 1024 * 1024, "Receipt exceeds metadata bound")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8-sig"))
    require(receipt.get("schema") == SCHEMA and receipt.get("shader_copy_exact") is True,
            "Missing exact package preparation identity")
    package = Path(receipt["package_root"])
    immutable = receipt["immutable_inputs"]
    require(input_files(package, Path(immutable["config"]["path"]), Path(immutable["settings"]["path"])) == immutable,
            "Captured/default/version inputs changed")
    require(inventory(package / "resources") == receipt["resource_files"], "Assembled resource closure changed")
    require(inventory(package / "resources" / "shaders") == receipt["shader_files"], "Package shader closure changed")
    require(inventory(Path(receipt["shader_workspace"])) == receipt["shader_files"],
            "Private shader mutations were not restored exactly")
    output = receipt_path.with_name("PACKAGE-QUALIFICATION-UNCHANGED.json")
    document = {
        "schema": SCHEMA,
        "status": "IMMUTABLE_INPUTS_AND_SHADER_MIRROR_UNCHANGED",
        "input_receipt_sha256": fingerprint(receipt_path)["sha256"],
        "package_root": str(package), "resource_files": len(receipt["resource_files"]),
        "shader_files": len(receipt["shader_files"]), "render_qualification": "SEPARATE_REQUIRED_RECEIPTS",
        "performance_acceptance": "PENDING_USER_GAMEPLAY",
    }
    if not read_only:
        output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    else:
        require(output.is_file() and json.loads(output.read_text(encoding="utf-8-sig")) == document,
                "The bound fixture-era unchanged receipt is missing or inconsistent")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-root")
    parser.add_argument("--config")
    parser.add_argument("--settings")
    parser.add_argument("--workspace")
    parser.add_argument("--verify", metavar="INPUT_RECEIPT")
    parser.add_argument("--read-only", action="store_true", help="Reverify without rewriting the bound unchanged receipt")
    args = parser.parse_args()
    try:
        if args.verify:
            require(not any((args.package_root, args.config, args.settings, args.workspace)), "Verify takes its bound paths from the receipt")
            output = verify(args.verify, args.read_only)
        else:
            require(not args.read_only, "Read-only requires --verify")
            require(all((args.package_root, args.config, args.settings, args.workspace)), "Preparation requires package-root, config, settings and fresh workspace")
            output = prepare(args.package_root, args.config, args.settings, args.workspace)
        print(f"PACKAGE_QUALIFICATION_INPUT_OK {output}")
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"PACKAGE_QUALIFICATION_INPUT_BLOCKED {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
