#!/usr/bin/env python3
"""VulkanMW Phase 0 architectural dependency guard.

This deliberately checks only hard dependency seams. Transitional v4 capture
code is quarantined outside the new native producer directories and is not
grandfathered into VulkanMW's target architecture.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".c", ".cc", ".cpp", ".cxx"}

RULES = [
    (
        ROOT / "components" / "rendercore",
        [
            (re.compile(r"#\s*include\s*[<\"]osg(?:/|[A-Z])"), "RenderCore must not include OSG"),
            (re.compile(r"\bosg::"), "RenderCore must not use osg:: types"),
            (re.compile(r"#\s*include\s*[<\"]vsg(?:/|[A-Z])"), "RenderCore must not include VSG"),
            (re.compile(r"\bvsg::"), "RenderCore must not use vsg:: types"),
            (re.compile(r"#\s*include\s*<vulkan/"), "RenderCore must not include Vulkan headers"),
        ],
    ),
    (
        ROOT / "components" / "render" / "native",
        [
            (re.compile(r"#\s*include\s*[<\"]osg(?:/|[A-Z])"), "native renderer producers must not include OSG"),
            (re.compile(r"\bosg::"), "native renderer producers must not use osg:: scene types"),
        ],
    ),
    (
        ROOT / "apps" / "openmw" / "mwrender" / "vulkanmw",
        [
            (re.compile(r"#\s*include\s*[<\"]osg(?:/|[A-Z])"), "VulkanMW gameplay producers must not include OSG"),
            (re.compile(r"\bosg::"), "VulkanMW gameplay producers must not use osg:: scene types"),
        ],
    ),
    (
        ROOT / "components" / "render" / "backend" / "vsg",
        [
            (re.compile(r"#\s*include\s*[<\"]osg(?:/|[A-Z])"), "VSG backend must not include OSG"),
            (re.compile(r"\bosg::"), "VSG backend must not traverse/use OSG"),
            (re.compile(r"#\s*include\s*[<\"].*apps/openmw/"), "VSG backend must not include OpenMW app/gameplay headers"),
            (re.compile(r"#\s*include\s*[<\"].*mwworld/"), "VSG backend must not depend directly on MWWorld"),
            (re.compile(r"#\s*include\s*[<\"].*mwmechanics/"), "VSG backend must not depend directly on MWMechanics"),
            (re.compile(r"#\s*include\s*[<\"].*mwlua/"), "VSG backend must not depend directly on MWLua"),
        ],
    ),
]


def source_files(directory: pathlib.Path):
    if not directory.exists():
        return
    for path in directory.rglob("*"):
        if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
            yield path


def code_only(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def check_vulkanmw_launcher_contract(failures: list[str]) -> None:
    launcher = ROOT / "tools" / "vulkanmw" / "START-VulkanMW-Test.bat"
    cmake = ROOT / "apps" / "openmw" / "CMakeLists.txt"
    if not launcher.is_file():
        failures.append("VulkanMW benchmark launcher is missing")
        return

    launcher_text = launcher.read_text(encoding="utf-8", errors="replace")
    for required in (
        "architecture-benchmark.py",
        "--renderer vulkan",
        "--fastpaths retained",
        "VulkanMW-Benchmark-",
    ):
        if required not in launcher_text:
            failures.append(f"VulkanMW benchmark launcher lost required contract: {required}")

    cmake_text = cmake.read_text(encoding="utf-8", errors="replace")
    contract_marker = "# VulkanMW packaging contract: every installed Windows Vulkan build carries"
    if contract_marker not in cmake_text:
        failures.append("VulkanMW CMake install contract marker is missing")
        return
    contract = cmake_text.split(contract_marker, 1)[1]
    contract = contract.split("endif()", 1)[0]
    for required in (
        "if(OPENMW_ENABLE_V4_VULKAN_RUNTIME)",
        "../../tools/vulkanmw/START-VulkanMW-Test.bat",
        "../../tools/v4/cp4/architecture-benchmark.py",
        "../../tools/v4/cp4/architecture-benchmark.lua",
    ):
        if required not in contract:
            failures.append(f"VulkanMW install contract lost required entry: {required}")


def main() -> int:
    failures: list[str] = []
    checked = 0

    for directory, patterns in RULES:
        for path in source_files(directory) or ():
            checked += 1
            text = code_only(path.read_text(encoding="utf-8", errors="replace"))
            for pattern, message in patterns:
                for match in pattern.finditer(text):
                    line = text.count("\n", 0, match.start()) + 1
                    failures.append(f"{path.relative_to(ROOT)}:{line}: {message}")

    check_vulkanmw_launcher_contract(failures)

    if failures:
        print("VulkanMW architecture boundary check FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print(f"VulkanMW architecture boundary check PASS ({checked} source files checked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
