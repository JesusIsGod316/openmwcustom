# CP4F texture residency repair — 2026-09-19

## Scope and evidence

Active checkout: `OpenMW custom Build-cp4f`, branch
`v4.0-cp4f-exterior-closeout`, HEAD
`f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`. Existing recovery WIP is preserved.
No commit, push, CI dispatch, normal-profile modification, or checkpoint promotion.
The authoritative context control block and relevant later recovery records were
reviewed before implementation. This is a resource-ownership correctness repair,
not the deferred CP6 performance optimization pass.

The user's 21:08 manual capture (`20260919-210813-gameplay-75508`) failed with
`Context::reserve() failed-2` while compiling the incremental actor/effect graph.
The previous morph-controller failure was not the failure in this capture.
The trace recorded 45,260.224 ms in change_to_exterior, including 37,772.530 ms in
terrain_preload; subsequent renderer allocation still failed. These are nested
operation observations, not a completed transition benchmark.

Inspection of the installed VSG 1.1.15 API and its tagged implementation showed
that `ImageInfo(sampler, data)` constructs a new image/view even when decoded
pixels are shared. Independent effect/actor realizations used that path. The
existing CPU decode cache also included a RenderWorld-local handle in its key,
preventing reliable sharing across temporary effect worlds.

Reference: https://github.com/vsg-dev/VulkanSceneGraph/blob/v1.1.15/src/vsg/state/ImageInfo.cpp

These are demonstrated duplication defects. The old log does not identify the
exact failed resource, so this record does not assert that duplication was the
only cause of the user's device-memory allocation failure.

## Implemented

- Decode-cache identity now uses immutable source/content/revision and texture
  interpretation, not a world-local handle. Different color spaces/formats and
  changed content remain distinct; failed decodes remain retryable.
- A bounded weak image-info index shares immutable sampled images across
  independent realizer configurations and frame generations. It compares the
  complete sampler contract, including mip allocation behavior; dynamic images
  bypass the index. Weak references do not extend GPU resource lifetime.
- Ordinary and enchanted-flipbook material bindings use the same sharing path.
  Mutable geometry, material state, and fence-based retirement remain separate.
- `OPENMW_V4_UNSHARED_TEXTURE_IMAGES=1` retains the independent-image control in
  the same executable. The existing `OPENMW_V4_UNCACHED_TEXTURE_DECODE=1` control
  also remains available. Controls are environment-only; no settings were edited.
- Sparse opt-in allocation censuses report unique images/payloads, pending image
  and buffer bytes, and VSG pool reserved/free bytes. A failed compile always
  emits a final census. These counts exclude driver/pipeline costs and image
  padding/generated mip overhead; they are not an exact VRAM-budget measurement.
- Terrain diagnostics split worker queue delay from terrain preparation time.
  The report collector recognizes both new event types. The canonical preload
  is retained because it also provides paging/gameplay bookkeeping; this repair
  does not claim to eliminate its observed 37.8-second delay.

## Exact files changed in this repair

1. `components/render/backend/vsg/livetexturecache.hpp`
2. `components/render/backend/vsg/livetextureimages.hpp` (new)
3. `components/render/backend/vsg/staticassetrealizer.cpp`
4. `components/render/backend/vsg/allocationdiagnostics.hpp` (new)
5. `components/render/backend/vsg/vsgruntimehost.cpp`
6. `apps/openmw/mwworld/cellpreloader.cpp`
7. `apps/openmw/CMakeLists.txt`
8. `tools/v4/cp4/effect-capture-tests.cpp`
9. `tools/v4/cp4/texture-residency-tests.cpp` (new)
10. `tools/v4/cp4/gameplay-diagnostics.py`
11. `tools/v4/cp4/test-gameplay-diagnostics.py`
12. `tools/v4/cp4/TEXTURE-RESIDENCY-REPAIR-2026-09-19.md` (this file)

Other dirty files predate this repair and were not reverted or staged.

## Validation

Local MSVC RelWithDebInfo production build and all selected regression targets:
PASS. An initial test-fixture ambiguous vector initializer failed compilation;
it was corrected to an explicitly typed VSG vector before the successful build.
No compiler errors were waived. Existing unrelated warnings remain.

- CTest: **13/13 PASS**, rerun after the final production build.
- Python diagnostic collector: **16/16 PASS**.
- All **seven CP4 source contracts PASS**; `git diff --check` PASS.
- Real Vulkan GUI isolation regression: PASS on the final build.
- New production-realizer regression: 96 independent effect graphs with the same
  texture and distinct material values resolve to **one decoded payload and one
  image object** with the repair. The same test executable with the image-sharing
  control disabled resolves to **96 image objects**, failing the duplication
  assertion as expected. This control failure is intentional, not a waived gate.
- Both modes rendered 150 real Vulkan frames with 96 effects and alternating
  material values. In the repaired mode, VSG pool allocated bytes were
  **85,551,408 at warm-up and at completion**. The control was **85,902,640 at both
  points**. These pool numbers are fixture observations, not full-game VRAM or
  FPS claims; VSG can additionally deduplicate resources during compilation.
  The final production edit after this GPU comparison only restricted allocation
  diagnostics to sparse samples; it did not change the sharing mechanism.

Build/evidence directory:
`C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/openmw-build-cp4f-qc`

Relevant logs: `texture-residency-final-build.log`, `texture-ctest-final.log`,
`texture-gpu-tests.log`, `texture-gpu-control.log`, `texture-gui-final.log`.

Built `openmw.exe` SHA256:
`D34779368718CF8C6D6229414556AA61E912BDB746800733A003D65C82725E26`.

The normal settings, input bindings, shader configuration, global storage, and
player storage hashes still match the last manual capture's original hashes.
The protected `_zZz__Wake_up.omwsave` SHA256 remains
`D34685C838BD1B4318DB34B1E0C1F00A7A2FE9A990FD21A120973297083AF99B`.

## Remaining acceptance and diagnostics

The full modded ship-to-Seyda-Neen transition was not rerun in this repair pass.
No conclusion about its crash resolution, load-time improvement, 8 GB VRAM
headroom, or gameplay FPS is promoted from the synthetic tests. PBR material
interpretation is unchanged. No extra manual screenshots or diagnostics are
required before that test: the existing manual capture harness automatically
records the new allocation and terrain events.

Next acceptance: launch this exact build with the manual capture harness, repeat
the hatch transition, and inspect the allocation/terrain report. If the exterior
loads, repeat a return/exit transition to check resource retirement. Any remaining
failure must be reported with the new resource counts, not silently skipped or
treated as a successful runtime pass.
