"""Launch VulkanMW with a persistent, fully isolated gameplay profile.

On first use, copy normal settings, bindings, Lua storage and saves one way.
Subsequent play writes only UserProfile; normal mod directories are read-only.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import contextlib
import uuid

HERE = Path(__file__).resolve().parent
PACKAGE = HERE.parents[1]

def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

dc = load("vulkanmw_play_config", PACKAGE / "tools/v4/cp4/diagnosticconfig.py")
gameplay = load("vulkanmw_play_gameplay", PACKAGE / "tools/v4/cp4/gameplay-diagnostics.py")
profile = load("vulkanmw_play_profile", HERE / "vulkan-profile.py")
private_profile = load("vulkanmw_private_profile", HERE / "vulkan-user-profile.py")

P1_P2_CONTROLS = {
    "OPENMW_VK_GROUP_PUBLICATION": "1",
    "OPENMW_VK_CHUNK_TRANSACTIONS": "1",
    "OPENMW_VK_RESOURCE_INVENTORIES": "1",
    "OPENMW_VK_PERSISTENT_ACTORS": "1",
    "OPENMW_VK_CACHED_OBJECT_ADMISSION": "1",
    "OPENMW_VK_CHANGE_DRIVEN_OBJECTS": "1",
    "OPENMW_VK_PRODUCER_DIRTY_QUEUES": "1",
    "OPENMW_VK_SPLIT_PARTICLE_CAPTURE": "1",
    "OPENMW_VK_SUPPORTED_CONTINUOUS_PRODUCERS": "1",
    "OPENMW_VK_GPU_SCENE_TABLES": "1",
    "OPENMW_V4_STATIC_FRUSTUM": "1",
    "OPENMW_V4_TERRAIN_OCCLUSION": "1",
}

P3B_CONTROLS = {
    "OPENMW_VK_GPU_CULL_QUEUE": "1",
    "OPENMW_VK_GPU_CULL_INDIRECT": "1",
    "OPENMW_VK_GPU_CULL_COMPACT": "1",
}


def controls_for(*, persistent_populations=False, object_binding_updates=False, binding_control=False,
                 p1_only=False, p3b_control=False):
    controls: dict[str, str] = {}
    gameplay.configure_cpu_fastpaths(controls, "vulkan", "retained")
    controls.update(P1_P2_CONTROLS)
    if p1_only:
        controls.pop("OPENMW_VK_GPU_SCENE_TABLES", None)
    elif persistent_populations or object_binding_updates or p3b_control:
        controls.update(P3B_CONTROLS)
        if (persistent_populations or object_binding_updates) and not p3b_control:
            controls["OPENMW_VK_PERSISTENT_POPULATIONS"] = "1"
        if object_binding_updates and not binding_control and not p3b_control:
            controls["OPENMW_VK_OBJECT_BINDING_UPDATES"] = "1"
    return controls

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, default=PACKAGE)
    parser.add_argument("--user-config", type=Path, default=dc.normal_default())
    parser.add_argument("--p1-only", action="store_true",
                        help="Disable only the P2 GPU-table switch for visual A/B")
    parser.add_argument("--persistent-populations", action="store_true",
                        help="Enable Stage 1 retained GPU population rows and the prerequisite P3B stack")
    parser.add_argument("--object-binding-updates", action="store_true",
                        help="Enable Stage 1 load-bound object updates, controller coverage and retained populations")
    parser.add_argument("--binding-control", action="store_true",
                        help="Disable only load-bound object updates; keep retained populations for matched visual QA")
    parser.add_argument("--p3b-control", action="store_true",
                        help="Use P3B but disable Stage 1, including when enabled by the installed launcher")
    parser.add_argument("--benchmark", action="store_true",
                        help="Record a manual gameplay session using normal settings/content; quit the game normally")
    parser.add_argument("--frame-profile", action="store_true",
                        help="Add detailed GPU/CPU frame profiling to a manual benchmark; may add overhead")
    parser.add_argument("--dry-run", action="store_true", help="Validate the package and launch configuration without starting the game")
    args = parser.parse_args()

    package = args.package.resolve()
    normal = args.user_config.resolve()
    exe = package / "openmw.exe"
    if not exe.is_file() or not (package / "resources").is_dir():
        raise SystemExit("Incomplete VulkanMW package: openmw.exe/resources are required")
    # This validates the selected normal chain without modifying it.
    chain, original_hashes = dc.inspect_chain(package, normal, dc.normal_default().resolve())
    identity_path = package / "source-changes.json"
    identity = json.loads(identity_path.read_text(encoding="utf-8-sig")) if identity_path.is_file() else None
    if identity and identity.get("executable_sha256", "").lower() != dc.digest(exe):
        raise SystemExit("Package executable no longer matches source-changes.json; nothing launched")

    if args.frame_profile and not args.benchmark:
        parser.error("--frame-profile requires --benchmark")
    controls = controls_for(persistent_populations=args.persistent_populations,
                            object_binding_updates=args.object_binding_updates, binding_control=args.binding_control,
                            p1_only=args.p1_only, p3b_control=args.p3b_control)
    evidence = None
    if args.benchmark:
        evidence = package / "Benchmarks" / ("manual-" + datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6])
        evidence.mkdir(parents=True)
        controls.update({
            "OPENMW_GAMEPLAY_DIAGNOSTICS": "1",
            "OPENMW_GAMEPLAY_DIAGNOSTICS_FILE": str(evidence / "gameplay.jsonl"),
            "OPENMW_RUNTIME_DIAGNOSTICS": "standard",
            "OPENMW_RUNTIME_DIAGNOSTICS_FILE": str(evidence / "runtime.jsonl"),
        })
        if args.frame_profile:
            controls["OPENMW_VK_FRAME_PROFILE"] = "1"
    env, removed = profile.environment(os.environ, controls)

    private = private_profile.prepare(package, normal, chain, dc, profile)
    with contextlib.nullcontext(private) as overlay:
        command = private_profile.command(exe, private, dc)
        print("VulkanMW isolated gameplay QA")
        print("Source profile (read only):", normal)
        print("Private configuration/storage:", private)
        print("P2 GPU scene tables:", "OFF (P1 control)" if args.p1_only else "ON")
        print("Stage 1 persistent populations:", "ON" if "OPENMW_VK_PERSISTENT_POPULATIONS" in controls else "OFF")
        print("Stage 1 object binding updates:", "ON" if "OPENMW_VK_OBJECT_BINDING_UPDATES" in controls else "OFF")
        print("Normal settings/content/saves are not rewritten by this launcher.")
        print("Gameplay uses private saves, screenshots, caches and Lua storage.")
        print("Normal profile files are never a live engine configuration or write target.")
        if evidence:
            manifest = {
                "state": "prepared", "created_utc": datetime.now(timezone.utc).isoformat(),
                "source": identity, "executable_sha256": dc.digest(exe),
                "command": command, "controls": controls, "removed_environment_names": removed,
                "normal_config_chain": [str(path) for path in chain], "original_chain_hashes": original_hashes,
                "settings_mode": "initial normal settings copy plus private Vulkan-only renderer override",
                "private_profile": str(private), "profile_policy": "one-way import; no copy back",
                "measurement_scope": "manual gameplay session, not a controlled performance verdict",
            }
            manifest_path = evidence / "manifest.json"
            manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            print("Session logs:", evidence)
        if args.dry_run:
            print("Dry run passed; no game process started.")
            return
        if evidence:
            try:
                with (evidence / "console.log").open("wb") as console:
                    manifest.update(state="running")
                    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
                    result = subprocess.run(command, cwd=package, env=env, stdout=console, stderr=subprocess.STDOUT)
                manifest.update(state="exited", exit_code=result.returncode,
                                original_chain_unchanged={path: Path(path).is_file() and dc.digest(Path(path)) == digest
                                                          for path, digest in original_hashes.items()})
            except BaseException:
                manifest.update(state="interrupted_or_failed")
                raise
            finally:
                manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            print("Session complete; logs remain in:", evidence)
        else:
            result = subprocess.run(command, cwd=package, env=env)
        raise SystemExit(result.returncode)

if __name__ == "__main__":
    main()
