# Rafael and parallax reference intake - September 23

User supplied these for reference, not installation. Inspected in place with the
installed 7-Zip reader; no archive scripts executed, active mod settings changed,
textures converted, or resource shader files replaced. Only selected text was
streamed to stdout; the archives were not unpacked into a game installation.

## Exact inputs

Location: `C:\Users\LSCha\Downloads\OpenMW future mods`.

- `v1.2 Parallax Replacement-58303-1-2-1776114723.zip`: 4653 bytes; SHA-256 `524f8de1bb9188286674d5a116e59b524558f68c9fe20d461cc7a4b763929e73`.
- `Rafael's Shader Pack 53667 2.2 2026-09-20T14-31Z p6TN64jb.7z`: 23455749 bytes; SHA-256 `7e7cedfebd94c8b6760413a8e87ad6bdcc5177dc22e8444c3f81cd528b7797ef`.
- `Enhanced PBR Lighting for OpenMW 0.49-0.51 53667 2.2 2026-09-20T14-32Z nOlkON6C.7z`: 2361225 bytes; SHA-256 `9fb787c9cb8206746d48466ddce19f9a64e9dc744d7ccc9c885d00bd5060e78e`.
- `Various Data Enhancements 53667 2.2 2026-09-20T14-32Z 9AnoA02H.7z`: 2494611 bytes; SHA-256 `9e5cb5e7d9304a7e4e360a5224da33ecf49e8f31d954dc38adce470fe5e3dec6`.

Later inputs, directly under `C:\Users\LSCha\Downloads`:

- `PBR Conversion Scripts-53667-1-9b-1765665748.7z`: 2329 bytes; SHA-256 `312fb2223f9458d34378f9a5c8a3b77206f41ae45c9c232cd945052d721d9d1c`.
- `Enhanced Shadows for OpenMW 0.49 and 0.50-53667-1-9c-1774013674.7z`: 14918 bytes; SHA-256 `838970639d71a2cd0c76511623437fe790957d0d34fa48e19506748bce8bf8c5`.

## Initial source findings

- Rafael's Shader Pack contains nine .omwfx files: AdvancedDebugger, DIVE,
  godrays, HBAO, SMAA, SMB, tonemap, VAIO and wetworld, plus their texture inputs.
  This is the postprocessing package, distinct from per-material lighting.
- Enhanced PBR Lighting contains separate 0.49/0.50 and 0.51 resource-shader
  trees for objects, terrain, groundcover, water, lighting and parallax.
  The 0.51 tree is a reference, not automatically ABI-compatible with this
  custom 0.52 renderer. Its included README explicitly warns against mixing
  legacy specular maps and packed PBR maps under one global interpretation.
- In the 0.51 lighting.glsl, SPECULAR_MAP_INTERPRETATION defaults to 2:
  R=metallicity, G=roughness, B=ambient visibility, A=inverse subsurface input.
  Lines 876-948 establish defaults, decoding, missing-map/sentinel behavior and
  further SSS modulation. The existing native Vulkan legacy RGB-specular path
  cannot substitute for this interpretation. This corroborates the material
  mismatch hypothesis; it is not proof for every artifact in the screenshots.
- The replacement ZIP contains only parallax.glsl (16,250 bytes), not a complete
  shader suite or an engine plugin. It configures 4-32 layers, eight soft-shadow
  steps, distance-dependent POM/iterative transitions, tangent-basis derivation
  choices, normal-map alpha height sampling and optional AO modes. It is a
  materially different implementation from Rafael's included 3,613-byte file.
  Its quality/performance tradeoffs need separate testing, not automatic enable.
- Various Data Enhancements is an asset package: meshes/textures plus selectable
  core, sky, moss, Ghostgate, PBR water mesh and light-related options. Do not
  treat it as another postprocessing pass. FOMOD configuration was read only;
  no install choices were applied.

## Integration requirements

Keep a legacy-material profile and an explicit Rafael-compatible interpretation.
Do not infer packed PBR encoding from the _spec filename alone. Preserve material
channel color-space treatment, defaults, BRDF, normal/tangent/UV conventions,
terrain blends, animated textures, alpha behavior, water, shadows and previews.
Port parallax only after this baseline is stable, with independent quality and
control paths. Preserve/check source licenses and attribution before incorporating
third-party code; this intake copies no third-party implementation into the engine.

The provided Enhanced PBR archive already contains useful lighting implementation,
and the subsequent conversion/shadow archives extend reference coverage below.
No claim is made that these files are the active winning assets in the user's
load order; verify VFS provenance.

The profiled native Vulkan route disables legacy OpenGL postprocessing, so this
reference intake does not change the diagnosis of the roughly 225 ms CPU-bound
frames. Moving a shader into the engine is not by itself a performance win;
shared inputs/intermediates and pass scheduling are candidates for later measured
work. Keep that separate from MSOC visibility and the current CPU test build.

## Conversion scripts and shadow follow-up

Read all ten conversion-package text files and the shadow README/2048 shader;
compared the other three shadow shader variants as text. No batch scripts or
installation instructions were executed. Archive instructions are reference
material, not authorization to install or overwrite anything.

### Texture conversion contracts

- The tools distinguish old RGB specular maps, packed PBR roughness maps and
  smoothness maps. Smoothness conversion inverts G; absent AO is represented by
  an all-zero B plane which the helper fills with ones. Both ZZZ_ConvertPBR and
  ZZZ_InvertSmoothness actually perform this optional B fill as well as G inversion.
- The legacy-to-PBR variants request sRGB-to-linear conversion before a heuristic
  roughness expression, force R to zero (nonmetal), B/A to one, and write DXT1.
  Standard/rough/smoother presets use different roughness expressions. These are
  offline appearance approximations, not lossless recovery of material intent.
  Do not assume a ported formula reproduces ImageMagick's sequential channel
  operations without a controlled fixture comparison.
- Batch loops recurse through matching texture filenames and can overwrite
  existing DDS output. The packed-map helpers inspect only legacy DDS FourCC,
  recognize DXT3/DXT5, and otherwise select DXT1; they do not preserve arbitrary
  DX10/BC7 encodings. None was run. No ImageMagick installation is needed for
  inspecting the scripts.
- Keep material interpretation explicit and non-destructive. Do not invert every
  green channel, fill every zero blue value, convert the user's entire texture
  collection, or interpret all _spec files as PBR. Confirm winning VFS source,
  encoding, color space, defaults and missing-map behavior before repair.

### Enhanced shadow contracts

- Despite the downloaded filename, the archive tree is labelled OpenMW 0.49-0.51.
  It supplies compatibility/shadows_fragment.glsl replacements, not a Vulkan
  plugin or .omwfx postprocess. OpenGL varyings, shadow2D, gl_FragData and OpenMW
  shader-template substitutions need translation to native shader interfaces.
- Four variants couple map resolution to filtering: 512 uses FILTER_SIZE 3
  (nine optimized PCF fetches), 1024/2048 use 4 (16 Poisson fetches), and 4096
  uses 5 (25 Poisson fetches). Text differences are SHADOW_SIZE/FILTER_SIZE only.
  Matching resolution does not establish equal filtering cost or appearance.
- Higher-filter variants take sunStrength to widen the penumbra in weaker sun.
  The one-argument unshadowedLightRatio overload supplies 1.0, so weather-adaptive
  softness requires the lighting caller to pass the real value. ADAPTIVE_RADIUS
  is defined but never referenced elsewhere in these files: do not promise that
  flipping that macro alone changes behavior.
- Defaults use bias type 1 and disable cascade blending. The source also includes
  other bias/blending branches; each needs Vulkan depth/coordinate validation,
  cascade-edge and grazing-angle tests before adopting it. Do not copy every
  optional branch unquestioningly.
- Current native vsgruntimehost.cpp constructs vsg::HardShadows and supplies map
  size, distance, bias and split settings; it does not automatically consume this
  replacement compatibility shader. Implement native filtering as a separate,
  switchable compatibility/quality feature, preserving a hard-shadow control.
  Supply actual resolution and per-frame lighting inputs from the renderer.
- Shadow caster visibility must remain independent of main-camera MSOC rejection:
  an occluded caster can still cast a visible shadow. Keep this requirement in
  the native occlusion integration tests.

These are intake findings, not newly implemented shader features. The CPU package
binary remains unchanged by this follow-up. Source licensing/attribution must be
checked before copying shader implementation into native engine sources.
