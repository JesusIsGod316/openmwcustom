from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

# V3.18 is deliberately additive over the exact, already-validated V3.17 stack.
# Running the full V3.17 harness first also guarantees its executable-bound
# identity marker exists before the V3.18 identity extension is applied.
v317 = HERE / "apply_diagnostic_harness_v317.py"
exec(
    compile(v317.read_text(encoding="utf-8"), str(v317), "exec"),
    {"__file__": str(v317), "__name__": "__main__"},
)

for layer_name in (
    "apply_v318_render_scale.py",
    "apply_v318_runtime_modes.py",
):
    layer = HERE / layer_name
    exec(
        compile(layer.read_text(encoding="utf-8"), str(layer), "exec"),
        {"__file__": str(layer), "__name__": "__main__"},
    )

readme_path = ROOT / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.18 renderer efficiency — internal render resolution P0
=========================================================

Purpose
-------
V3.18 begins the GPU-efficiency phase without disturbing the validated V3.17
runtime/audio work. P0 separates native display/UI resolution from the internal
3D scene resolution and adds a single bilinear presentation stage. This measures
how much of the current GPU/PostFX cost is resolution-dependent before NIS is
introduced, and establishes the provider-neutral insertion point needed by NIS,
FSR-style spatial scalers, and later DLSS Super Resolution work.

Resolution architecture
-----------------------
The SDL/window size and main camera projection aspect remain native. The HUD camera
also stays at native output resolution. Scene color/depth/normals/distortion and
PostFX render targets use Video/render scale. PingPongCull pushes the internal
viewport only while the 3D scene is rendered. PingPongCanvas then executes the
entire PostFX chain at internal resolution and performs exactly one final native-
resolution bilinear presentation draw.

P0 safety boundaries
--------------------
Stereo/multiview remains native-resolution in P0. NIS is not silently emulated:
Video/upscaler accepts only "bilinear" until the dedicated OpenGL compute layer is
present and validated. Existing PBR/material/shadow/ObjectPaging behavior is not
changed by this layer. Scale is clamped to 0.5..1.0.

P0 causal modes
---------------
95 = stock-Lua V3.17 control + 100% render scale.
96 = identical foundation + 85% internal scale + bilinear upscale.
97 = identical foundation + 77% internal scale + bilinear upscale.
98 = identical foundation + 66.7% internal scale + bilinear upscale.

Do not run all four automatically. First useful causal comparison is 95 -> 97.
If the GPU/PostFX reduction is meaningful and visual behavior is correct, the next
V3.18 layer replaces the final bilinear resolve with NVIDIA Image Scaling while
keeping these exact resolution and native-UI semantics.
''')

# Final snapshot must represent the source that Windows will compile, not the
# earlier V3.17 intermediate patch.
subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"], cwd=ROOT, check=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)
stat = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

patch_text = patch.decode("utf-8", errors="replace")
launcher_text = (ROOT / "tools/v3/launchers/V3_Lab.ps1").read_text(encoding="utf-8")
readme_text = readme_path.read_text(encoding="utf-8")

for marker in (
    'SettingValue<float> mRenderScale',
    '"Video", "render scale"',
    'renderScalingActive() const',
    'Settings::video().mRenderScale',
    'const bool scaledOutput',
    'Present exactly once at native resolution',
    'openmw-custom-v3.18-render-scale-p0',
):
    if marker not in patch_text:
        raise RuntimeError(f"V3.18 generated source snapshot missing marker: {marker}")

for marker in (
    "95 = V3.18 native-resolution control",
    "v318-bilinear-85",
    "v318-bilinear-77",
    "v318-bilinear-667",
    "Enter 1 through 98",
    "v318_render_scale=$V318RenderScale",
):
    if marker not in launcher_text:
        raise RuntimeError(f"V3.18 generated launcher missing marker: {marker}")

for marker in (
    "V3.18 renderer efficiency",
    "Resolution architecture",
    "P0 safety boundaries",
    "P0 causal modes",
):
    if marker not in readme_text:
        raise RuntimeError(f"V3.18 README marker missing: {marker}")

# Fail closed against the two most dangerous regressions: scaling the HUD itself,
# or allowing NIS selection before an actual NIS path exists.
post_hpp = (ROOT / "apps/openmw/mwrender/postprocessor.hpp").read_text(encoding="utf-8")
post_cpp = (ROOT / "apps/openmw/mwrender/postprocessor.cpp").read_text(encoding="utf-8")
video = (ROOT / "components/settings/categories/video.hpp").read_text(encoding="utf-8")
canvas = (ROOT / "apps/openmw/mwrender/pingpongcanvas.cpp").read_text(encoding="utf-8")
assert 'mHUDCamera->resize(mWidth, mHeight);' in post_cpp
assert 'mViewer->getCamera()->resize(mWidth, mHeight);' in post_cpp
assert 'makeEnumSanitizerString({ "bilinear" })' in video
assert 'pass.mResolve && index == filtered.back() && !scaledOutput' in canvas
assert 'resolveViewport->apply(state);' in canvas
assert 'outputWidth() const { return mWidth; }' in post_hpp

print("V3.18 P0 generated-source policy invariants passed")
