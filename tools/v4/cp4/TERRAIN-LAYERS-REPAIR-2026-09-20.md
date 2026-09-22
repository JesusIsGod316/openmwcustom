# LAND material repair checkpoint — 2026-09-20

## Scope and source state

Addresses the source-level cause of white exterior terrain. The V4 producer
previously published terrain geometry and a white material without UVs, LAND
texture layers, or blend masks. This is not evidence of missing user textures.

Checkout: `C:/Users/LSCha/Documents/ChatGPT/OpenMW custom Build-cp4f`.
Branch: `v4.0-cp4f-exterior-closeout`.
HEAD: `f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b` plus existing and new uncommitted WIP.
No commit, push, save modification, persistent settings change, or game launch.
Existing dirty work and prior dynamic-compilation/GUI-order repairs are retained.

The archive control block and subsequent current archive events were reviewed.
The repository's native terrain storage, UV buffers, material pass construction,
and compatibility terrain shaders supplied the terrain semantics. No arbitrary
gamma adjustment, texture-quality reduction, or new whole-graph eviction policy.

## Implementation

- Capture the actual LAND layers and blend maps from `TerrainStorage`.
- Publish immutable tiled diffuse/normal UVs and separate native blend-mask UVs.
  Preserve the native Morrowind blend-map texmat/nudge; ESM4 bypass is retained.
- Resolve diffuse and normal maps by winning VFS content identity, using a
  cache owned by the terrain storage rather than a global/thread-local cache.
- Carry generated masks as immutable, validated RGBA8 pixels in neutral texture
  records. Their identity includes extent and content hash; mask alpha remains
  linear. Preparation/publication byte budgets include these pixels.
- Publish one ordered material pass per layer, sharing the chunk mesh. First
  pass uses source-alpha/zero; later passes source-alpha/one and equal depth.
  Each layer receives ordinary fog before weighted accumulation.
- Keep diffuse alpha as terrain specular intensity rather than opacity. Carry
  normal-map alpha as optional height; RG/BC5 normals reconstruct Z and do not
  incorrectly treat an absent height channel as parallax.
- Retire every layer's material and texture handles with the terrain chunk.
  Malformed replacement publication preserves the previously accepted terrain.
- Use material-set binding 16 for the mask. Review caught and corrected an
  initial overlap with enchantment bindings 14/15 before this checkpoint.

Same-executable causal control (presence enables old geometry-only behavior):
`OPENMW_V4_GEOMETRY_ONLY_TERRAIN_CONTROL=1`.
It is scoped to test subprocesses, not persisted in settings or globally.

## Exact files edited for this checkpoint

1. `apps/openmw/mwrender/terrainstorage.hpp`
2. `apps/openmw/mwrender/terrainstorage.cpp`
3. `apps/openmw/mwrender/v4terrainsource.cpp`
4. `components/rendercore/records.hpp`
5. `components/rendercore/renderworld.hpp`
6. `components/rendercore/realizationkeys.hpp`
7. `components/rendercore/terrainchunkproducer.hpp`
8. `components/rendercore/terrainpreparationservice.hpp`
9. `components/render/backend/vsg/staticassetrealizer.cpp`
10. `components/render/backend/vsg/statictexturedecode.cpp`
11. `components/render/backend/vsg/legacymaterialshader.cpp`
12. `components/render/backend/vsg/legacymaterialshader.hpp`
13. `tools/v4/cp4/terrain-pixel-tests.hpp` (new)
14. `tools/v4/cp4/water-pixel-tests.hpp`
15. `tools/v4/cp4/streaming-contract.py`
16. `tools/v4/cp3b3/legacy-material-shader-smoke.cpp`
17. `tools/v4/cp3b3/enchanted-material-shader-smoke.cpp`
18. This record.

This list describes this checkpoint, not the entire pre-existing dirty checkout.

## Compiler and test evidence

Main build/evidence directory:
`C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/openmw-build-cp4f-qc`.

MSVC 2022 RelWithDebInfo production executable and affected graphics fixtures
build successfully (`terrain-final-build.log`). The initial build failed because
the new ESM4 check needed `components/esm/util.hpp`; the include was added and
the rebuild passed. Existing compiler warnings remain; not a warning-free claim.

Production executable: `openmw.exe` in the build directory above.
SHA256: `B746F21AF66230338744F5ACC52612EBBFD00A574A3E9E3F18566B94039A8117`.
This checkpoint does not replace the desktop shortcut's installed executable.

- Rebuilt registered unit binaries: **14/14 passed** (`terrain-ctest.log`). An
  initial invocation without the dependency PATH could not load two binaries;
  the correctly configured rerun passed all tests.
- All seven CP4 source-contract scripts passed. Python gameplay diagnostic
  tests: **16/16 passed**. `git diff --check` passed.
- Isolated production-path terrain pixels passed (`terrain-pixels-final.log`):
  left red=255/green=0, right red=0/green=255, blended red/green=186/189,
  nearer blue object=0/0/255. Diffuse alpha is zero in the fixture, so treating
  it as terrain opacity fails. UV0 tiles eight times while mask UV1 does not.
  Invalid image publication and failed-replacement rollback are exercised.
  Texture/material handles are zero after retirement. **Zero Vulkan validation
  errors** in the offscreen-only run.
- Same executable, geometry-only control: **expected exit 1**, white terrain
  (255/255 at the color probes), depth occluder still blue; zero validation
  errors (`terrain-pixels-final-control.log`).
- Water/sky and late-GUI image assertions pass in those same isolated runs.
- Final fixture adds vertically asymmetric masks to verify generated row
  orientation as well as separate UV selection. Rebuild and pixel assertions
  pass with zero validation errors (`terrain-orientation-build.log`,
  `terrain-orientation-pixels.log`). The final same-binary control again fails
  exactly on white terrain (`terrain-orientation-control.log`, expected exit 1,
  zero validation errors). The production executable is unchanged by this
  fixture-only extension.
- Stable dynamic actor test: 12 frames, warm/final pool **52,325,824 bytes**;
  recompilation assertions pass (`terrain-dynamic-stable.log`). This is a
  synthetic reuse check, not an FPS benchmark.
- GUI isolation assertions pass (`terrain-gui-isolation.log`).
- Separate material smoke build at
  `C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/terrain-material-smokes`:
  **4/4 passed** (`ctest-final.log`), including ordinary material, enchantment
  GLSL variants, realization, and routing. Added checks cover terrain flag/fog
  packing, equal-depth selection, ordinary-material isolation, and non-overlap
  with enchantment descriptors. `/W4 /WX` compiler checks remain enabled.
  Harness setup needed the existing CRT deprecation definition, normal MSVC
  exception flags, and Release imported dependencies with unconfigured fallback
  (`CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO=Release;`). Initial harness failures
  and missing-debug-DLL attempts remain in its logs; production settings were
  not altered to accommodate the smoke harness.

Normal/height shader compilation and selectors are covered, but the unlit color
fixture does not prove full in-game normal, parallax, or specular lighting parity.

## Still failing / acceptance boundary

Presentation-based actor/GUI tests pass their behavioral assertions but each
still emits ten swapchain/image-view validation errors:
`VUID-VkSwapchainCreateInfoKHR-imageFormat-01778` and
`VUID-VkImageViewCreateInfo-usage-02275`.

The full water-transition run (`terrain-transition.log`) passed its transition
assertions but also emitted those ten errors PLUS
`VUID-vkBindImageMemory-memory-01047`: an image required memoryTypeBits 0x3,
while its allocation used memoryTypeIndex 2. This additional allocation error
remains unresolved and must not be folded into a clean-pass claim or attributed
to a specific overlay/driver without evidence. Offscreen-only terrain runs did
not reproduce it. No overlay process or global setting was changed.

White-ground source implementation is repaired and controlled pixels validate
the path, but the actual new-game/intro/ship-to-exterior route has not been
accepted on this executable. Exterior freezes/FPS, static synchronization cost,
real exterior/cave water appearance, cyan/green object material artifacts, and
presentation/allocation validation failures remain open. No overall stability
or performance improvement is claimed from these fixtures.
