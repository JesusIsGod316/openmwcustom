import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
branch = os.environ.get("GITHUB_REF_NAME", "")

# V3 Windows builds must route to the harness matching the branch version.
# Never silently fall back to an older stack: a missing/mismatched harness is a
# hard build failure so preflight and the actual Windows compile cannot diverge.
match = re.fullmatch(r"v3\.(\d+)(?:-.+)?", branch)
if match:
    minor = match.group(1)
    version = f"3.{minor}"
    harness_name = f"apply_diagnostic_harness_v3{minor}.py"
    harness = HERE / harness_name
    if not harness.is_file():
        raise RuntimeError(
            f"V3 build identity failure: branch {branch!r} requires {harness_name}, "
            "but that exact harness does not exist. Refusing to fall back."
        )

    print(f"[V3.{minor}] exact Windows router -> {harness_name}")
    exec(
        compile(harness.read_text(encoding="utf-8"), str(harness), "exec"),
        {"__file__": str(harness), "__name__": "__main__"},
    )

    variant = ""
    variant_layers = []
    if branch == "v3.21-cp2-fairness-repair":
        variant = "V3.21_CP2_FAIRNESS_REPAIR"
        variant_layers = ["apply_v321_cp2_fairness_repair.py"]
    elif branch == "v3.21-cp3-fullbody-first-person":
        variant = "V3.21_CP3_FULL_BODY_FIRST_PERSON"
        variant_layers = [
            "apply_v321_cp2_fairness_repair.py",
            "apply_v321_cp3_fullbody_first_person.py",
        ]
    elif branch == "v3.21-cp4-shadow-compat":
        variant = "V3.21_CP4_SHADOW_COMPAT"
        variant_layers = [
            "apply_v321_cp2_fairness_repair.py",
            "apply_v321_cp3_fullbody_first_person.py",
            "apply_v321_cp4_shadow_compat.py",
        ]
    elif branch == "v3.21-cp4-locomotion-compat":
        variant = "V3.21_CP4_LOCOMOTION_COMPAT"
        variant_layers = [
            "apply_v321_cp2_fairness_repair.py",
            "apply_v321_cp3_fullbody_first_person.py",
            "apply_v321_cp4_shadow_compat.py",
            "apply_v321_cp4_locomotion_compat.py",
        ]

    for layer_name in variant_layers:
        layer = HERE / layer_name
        if not layer.is_file():
            raise RuntimeError(
                f"V3 build identity failure: branch {branch!r} requires {layer_name}, "
                "but that exact variant layer does not exist."
            )
        print(f"[V3.21] exact branch variant -> {layer_name}")
        exec(
            compile(layer.read_text(encoding="utf-8"), str(layer), "exec"),
            {"__file__": str(layer), "__name__": "__main__"},
        )

    # Generated-source identity gates. These run in the same clean checkout that
    # will actually be compiled, not only in the separate preflight job.
    launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
    patch = ROOT / "V3-applied-source.patch"
    readme = ROOT / "V3-LAB-README.txt"
    for required in (launcher, patch, readme):
        if not required.is_file():
            raise RuntimeError(
                f"V3 build identity failure: exact harness {harness_name} did not generate {required.name}."
            )

    launcher_text = launcher.read_text(encoding="utf-8", errors="replace")
    patch_text = patch.read_text(encoding="utf-8", errors="replace")
    readme_text = readme.read_text(encoding="utf-8", errors="replace")
    version_label = f"V3.{minor}"
    version_key = f"v3.{minor}"
    if version_label not in launcher_text:
        raise RuntimeError(
            f"V3 build identity failure: generated launcher does not identify {version_label}."
        )
    if version_key not in patch_text.lower():
        raise RuntimeError(
            f"V3 build identity failure: generated source patch contains no {version_key} marker."
        )
    if version_label not in readme_text:
        raise RuntimeError(
            f"V3 build identity failure: generated README does not identify {version_label}."
        )
    if variant and variant not in patch_text and "V3.21 CP2" not in readme_text:
        raise RuntimeError(
            f"V3 build identity failure: branch variant {variant} left no generated-source/README identity."
        )

    manifest_lines = [
        f"branch={branch}",
        f"version={version_label}",
        f"harness={harness_name}",
        "routing=exact-fail-closed",
        "generated_source_identity=passed",
    ]
    if variant:
        manifest_lines.append(f"variant={variant}")
    manifest_lines.append("")

    manifest = ROOT / "V3-BUILD-IDENTITY.txt"
    manifest.write_text(
        "\n".join(manifest_lines),
        encoding="utf-8",
        newline="\n",
    )
    print(f"[{version_label}] generated-source identity gates passed")
else:
    raise RuntimeError(
        f"V3 build identity failure: generic V3 Windows router was invoked for unexpected branch {branch!r}. "
        "Refusing to guess which optimization stack to compile."
    )
