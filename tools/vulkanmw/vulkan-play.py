"""Launch the installed VulkanMW build against the user's normal profile.

The normal configuration/content chain and user-data directory remain authoritative.
Only a temporary settings overlay forces Vulkan/no-fallback. Engine feature controls
are process-local and disappear when the game exits. This launcher is intentionally
for gameplay/visual QA, not a clean performance benchmark.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile

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

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, default=PACKAGE)
    parser.add_argument("--user-config", type=Path, default=dc.normal_default())
    parser.add_argument("--p1-only", action="store_true",
                        help="Disable only the P2 GPU-table switch for visual A/B")
    args = parser.parse_args()

    package = args.package.resolve()
    normal = args.user_config.resolve()
    exe = package / "openmw.exe"
    if not exe.is_file() or not (package / "resources").is_dir():
        raise SystemExit("Incomplete VulkanMW package: openmw.exe/resources are required")
    # This validates the selected normal chain without modifying it.
    dc.inspect_chain(package, normal, dc.normal_default().resolve())

    controls: dict[str, str] = {}
    gameplay.configure_cpu_fastpaths(controls, "vulkan", "retained")
    controls.update(P1_P2_CONTROLS)
    if args.p1_only:
        controls.pop("OPENMW_VK_GPU_SCENE_TABLES", None)
    env, _ = profile.environment(os.environ, controls)

    with tempfile.TemporaryDirectory(prefix="VulkanMW-Play-") as temporary:
        overlay = Path(temporary)
        (overlay / "openmw.cfg").write_text(
            "# Temporary VulkanMW renderer overlay; normal content remains authoritative.\n",
            encoding="utf-8")
        (overlay / "settings.cfg").write_text(
            "[Video]\nrenderer backend = vulkan\nrenderer fallback = false\n",
            encoding="utf-8")
        command = [
            str(exe),
            "--replace=config",
            "--config", str(normal),
            "--config", str(overlay),
            "--user-data", str(normal),
            "--resources", str(package / "resources"),
            "--skip-menu=false",
            "--new-game=false",
        ]
        print("VulkanMW normal-play QA")
        print("Normal config/user data:", normal)
        print("P2 GPU scene tables:", "OFF (P1 control)" if args.p1_only else "ON")
        print("Normal settings/content/saves are not rewritten by this launcher.")
        print("Gameplay itself uses the normal user-data directory, just like an ordinary launch.")
        result = subprocess.run(command, cwd=package, env=env)
        raise SystemExit(result.returncode)

if __name__ == "__main__":
    main()
