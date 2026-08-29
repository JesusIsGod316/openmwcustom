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
    "apply_v318_nis.py",
    "apply_v318_nis_modes.py",
):
    layer = HERE / layer_name
    exec(
        compile(layer.read_text(encoding="utf-8"), str(layer), "exec"),
        {"__file__": str(layer), "__name__": "__main__"},
    )

readme_path = ROOT / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.18 renderer efficiency — internal resolution + NVIDIA Image Scaling
======================================================================

Purpose
-------
V3.18 begins the GPU-efficiency phase without disturbing the validated V3.17
runtime/audio work. The first layer separates native display/UI resolution from
the internal 3D scene resolution and provides a single bilinear presentation
stage. The second layer replaces only that final presentation stage with NVIDIA
Image Scaling (NIS) when selected.

Resolution architecture
-----------------------
The SDL/window size and main camera projection aspect remain native. The HUD camera
also stays at native output resolution. Scene color/depth/normals/distortion and
PostFX render targets use Video/render scale. PingPongCull pushes the internal
viewport only while the 3D scene is rendered. PingPongCanvas then executes the
entire PostFX chain at internal resolution and performs exactly one final native-
resolution presentation before the HUD/UI.

NIS provider
------------
NIS is pinned to NVIDIA Image Scaling SDK 1.0.3 source commit
35e13ba316c98eeecf16f37eae70ce88019911f6. The NVIDIA NVScaler algorithm and
coefficient tables are retained. NVIDIA's GLSL example uses Vulkan-style separate
texture/sampler objects; the V3.18 wrapper adapts only those texture-access macros
to ordinary OpenGL combined sampler2D objects and dispatches through OpenGL 4.3
compute/image-load-store using OSG GLExtensions.

NIS runs after the low-resolution PostFX chain and before native-resolution UI.
It writes a dedicated RGBA8 native-resolution output texture, then the existing
fullscreen presentation path samples that texture. A shader/extension/config
failure logs a warning and explicitly falls back to bilinear. NIS is never silently
emulated. Current implementation is FP32, 32x24 output blocks, 128 threads/group,
with adjustable Video/upscaler sharpness. Stereo/multiview remains native-only in
this first V3.18 implementation.

Causal modes
------------
95  = stock-Lua V3.17 control + 100% render scale.
96  = 85% internal scale + bilinear upscale.
97  = 77% internal scale + bilinear upscale.
98  = 66.7% internal scale + bilinear upscale.
99  = 85% internal scale + NIS, sharpness 0.20.
100 = 77% internal scale + NIS, sharpness 0.20. FIRST NIS TEST.
101 = 66.7% internal scale + NIS, sharpness 0.20.

Do not run the whole matrix automatically. The first useful rendering comparison
is 95 -> 97 to measure the raw resolution-dependent GPU ceiling, followed by
97 -> 100 to isolate NIS cost/quality at identical 77% internal resolution.
''')

# The NIS provider is generated from pinned upstream SDK sources during patch
# application. Mark those new files intent-to-add so `git diff` includes their
# exact bytes in V3-applied-source.patch. Without this, Git omits untracked files
# and artifact QC could falsely claim the final source snapshot was complete.
generated_nis_files = [
    "apps/openmw/mwrender/nisscaler.hpp",
    "apps/openmw/mwrender/nisscaler.cpp",
    "apps/openmw/mwrender/v318_nis_config.hpp",
    "apps/openmw/mwrender/v318_nis_shader.hpp",
]
subprocess.run(["git", "add", "-N", "--", *generated_nis_files], cwd=ROOT, check=True)

# Final snapshot must represent the source that Windows will compile, not an
# intermediate V3.17/P0 patch.
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
    'class NisScaler',
    'NVScalerUpdateConfig',
    'glDispatchCompute',
    'glMemoryBarrier',
    'mNisScaler->dispatch',
    '"bilinear", "nis"',
):
    if marker not in patch_text:
        raise RuntimeError(f"V3.18 generated source snapshot missing marker: {marker}")

# Verify every generated NIS source is represented as a newly-added file in the
# exact patch, not just present transiently in the CI worktree.
for path in generated_nis_files:
    if f"diff --git a/{path} b/{path}" not in patch_text or f"+++ b/{path}" not in patch_text:
        raise RuntimeError(f"V3.18 exact patch omitted generated NIS source: {path}")

for marker in (
    "95 = V3.18 native-resolution control",
    "v318-bilinear-85",
    "v318-bilinear-77",
    "v318-bilinear-667",
    "v318-nis-85",
    "v318-nis-77",
    "v318-nis-667",
    "Enter 1 through 101",
    "v318_render_scale=$V318RenderScale",
    "v318_upscaler_sharpness=$V318UpscalerSharpness",
):
    if marker not in launcher_text:
        raise RuntimeError(f"V3.18 generated launcher missing marker: {marker}")

for marker in (
    "V3.18 renderer efficiency",
    "Resolution architecture",
    "NIS provider",
    "35e13ba316c98eeecf16f37eae70ce88019911f6",
    "97 -> 100",
):
    if marker not in readme_text:
        raise RuntimeError(f"V3.18 README marker missing: {marker}")

provenance = (ROOT / "V3.18-NIS-PROVENANCE.txt").read_text(encoding="utf-8")
for marker in (
    "NVIDIA Image Scaling SDK 1.0.3",
    "source_commit=35e13ba316c98eeecf16f37eae70ce88019911f6",
    "NIS_Scaler.h_git_blob=02f645c2c01b0235d340d25c6cfc913000f7cc1b",
    "NIS_Config.h_git_blob=b8982217d7c4ad99a4725af54336d7a5b24de443",
    "fallback=explicit bilinear",
):
    if marker not in provenance:
        raise RuntimeError(f"V3.18 NIS provenance missing marker: {marker}")

# Fail closed against the dangerous integration regressions: scaling the HUD,
# applying NIS before the low-res PostFX chain, losing bilinear fallback, or
# allowing an unpinned/unknown NIS source.
post_hpp = (ROOT / "apps/openmw/mwrender/postprocessor.hpp").read_text(encoding="utf-8")
post_cpp = (ROOT / "apps/openmw/mwrender/postprocessor.cpp").read_text(encoding="utf-8")
video = (ROOT / "components/settings/categories/video.hpp").read_text(encoding="utf-8")
canvas = (ROOT / "apps/openmw/mwrender/pingpongcanvas.cpp").read_text(encoding="utf-8")
nis_cpp = (ROOT / "apps/openmw/mwrender/nisscaler.cpp").read_text(encoding="utf-8")
nis_shader = (ROOT / "apps/openmw/mwrender/v318_nis_shader.hpp").read_text(encoding="utf-8")
assert 'mHUDCamera->resize(mWidth, mHeight);' in post_cpp
assert 'mViewer->getCamera()->resize(mWidth, mHeight);' in post_cpp
assert 'makeEnumSanitizerString({ "bilinear", "nis" })' in video
assert 'pass.mResolve && index == filtered.back() && !scaledOutput' in canvas
assert 'if (scaledOutput)' in canvas
assert 'mNisScaler->dispatch' in canvas
assert 'presentationTexture = nisTexture' in canvas
assert 'state.applyTextureAttribute(0, presentationTexture);' in canvas
assert 'outputWidth() const { return mWidth; }' in post_hpp
assert 'GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT' in nis_cpp
assert 'falling back to bilinear upscale' in nis_cpp
assert '#version 430 core' in nis_shader
assert '#define NIS_USE_HALF_PRECISION 0' in nis_shader
assert '#define NIS_THREAD_GROUP_SIZE 128' in nis_shader
assert 'NVScaler(gl_WorkGroupID.xy, gl_LocalInvocationID.x);' in nis_shader
assert 'sampler2D(x, samplerLinearClamp)' not in nis_shader

print("V3.18 internal-resolution + pinned NIS generated-source policy invariants passed")
