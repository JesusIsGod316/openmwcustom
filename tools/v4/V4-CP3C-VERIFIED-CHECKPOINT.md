# V4.0 CP3C verified checkpoint

Status: **accepted**
Date: 2026-09-08
Implementation branch: `v4.0-cp3c-runtime-integration`
Accepted commit: `976f48e1a23e109f725502e776b051d857d08054`
Accepted tree: `29ee82a2b9e34cfaaaa759e435e60288204a992a`
CI workflow: `V4.0 CP3C runtime integration QC`
CI run: https://github.com/JesusIsGod316/openmwcustom/actions/runs/34284897017
Result: **success**

## Accepted CP3C state

This checkpoint establishes a build-gated, production-linked VSG/Vulkan runtime
ownership seam while keeping the shipping OpenGL path safe.

Accepted capabilities include:

- renderer-neutral RenderCore world, frame, camera, environment, material,
  texture, light, lifetime, and backend-selection contracts;
- authoritative active-cell publication and retirement with stable handles,
  revisions, world epochs, and fail-closed validation;
- real OpenMW NIF/VFS translation for the accepted static compatibility slice;
- legacy/Gamebryo material realization covering the accepted vertex color and
  alpha, detail, UV, blend, alpha-test, culling, lighting, texture identity,
  local-light, material-fog, and interior-fog semantics;
- VSG runtime host, SDL Vulkan window bootstrap, checked acquire/submit/present,
  frame-safe history commit, persistent semantic session, sticky failure
  diagnostics, and orderly idle/destruction behavior;
- OpenMW-facing scene lifecycle, settings, render bridge, and authoritative
  frame-coordinator seams;
- production OpenMW compilation and final linkage with the guarded runtime;
- a single shared backend source manifest and engine-adapter source manifest,
  preventing production and conformance targets from drifting apart.

## Verification evidence

Run 34284897017 passed all of the following:

- CP3C ownership contracts and retained CP3B regressions;
- neutral RenderCore boundary and compatibility-gate enforcement;
- Windows OpenGL control build and component/OpenMW/OpenMW-CS tests;
- Windows VSG static realization compilation and tests;
- real-NIF Vulkan conformance executable compilation and linkage;
- production OpenMW compilation and final linkage with
  `OPENMW_ENABLE_V4_VULKAN_RUNTIME=ON`;
- real-NIF command-surface launch;
- CP3B4 local conformance-package staging and artifact upload.

Local pre-push validation also passed 13 available CP3B/CP3C ownership,
planning, lifetime, routing, and compatibility smoke executables.

## Failure lessons locked into the checkpoint

Three late Windows failures preceded acceptance:

1. MSVC's per-token string-literal size limit was exceeded by the embedded
   compatibility shader.
2. The new `openmw-lib` link call mixed CMake's keyword and plain signatures.
3. The production runtime source list omitted
   `staticassetconformance.cpp`, leaving two final-link symbols unresolved.

The accepted tree prevents repeats through:

- an early raw-string size audit across V4 source surfaces;
- an early mixed-`target_link_libraries` signature audit;
- shared authoritative source manifests consumed by production and conformance
  targets.

## Compatibility boundary

CP3C is an integration checkpoint, not full playable Vulkan parity.

The Vulkan route remains unadvertised and `Auto` remains on OpenGL. The
runtime intentionally fails closed for unsupported content instead of silently
dropping or approximating it. Remaining major compatibility surfaces include:

- actors, skinned animation, animated objects, and dynamic transforms/materials;
- terrain, sky, water, weather, particles, and remaining light/controller types;
- UI, video, post-processing, shader-mod compilation, and full composition;
- complete input/application-loop ownership and runtime backend selection;
- exterior streaming/paging, GPU-driven submission, and broader multiview;
- real mod-corpus qualification, including Beautiful Cities of Morrowind,
  Tamriel Rebuilt/Project Tamriel assets, dense replacers, Lua behavior, saves,
  and representative shader configurations.

No Vulkan route may claim general mod or shader compatibility until those
surfaces are implemented and pass the V4 compatibility contract.

## Exact continuation point

Continue from accepted commit
`976f48e1a23e109f725502e776b051d857d08054`.

The next architectural slice should create the distinct backend-owned Vulkan
application route and gameplay-update boundary without a hidden OpenGL window
or dual renderer. Preserve current input, GUI, world, VFS, settings, Lua, save,
and mod behavior while incrementally replacing render-side OSG ownership.
Build on the existing frame coordinator, render bridge, scene lifecycle,
persistent session, and shared source manifests. Keep OpenGL as the automatic
fallback until the full compatibility-facet gate is qualified.
