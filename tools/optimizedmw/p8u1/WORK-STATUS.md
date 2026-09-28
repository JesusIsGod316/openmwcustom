# P8U1 resource repair / upstream donor work

Base: exact P8G4 commit 8ed1f63beed5fa2d7b3e46e272cbaa2f9b27f228,
tree c3fd8b0f7c7a29835a870ad3abb7c21bfe0a5fdb. Publication/build authorized by the user. No runtime performance claim. Keep CULL-CPU as the conservative
comparison configuration, LOD2 available but its tail verdict unresolved.

## Invariants

- No hard-duration guarantee for an indivisible OpenGL call.
- No removal of required-resource progress or OSG dynamic-draw synchronization.
- No P1/P2/P7 or general resource-cache replacement.
- No new public BAT. Future isolation belongs in the existing launcher menu.
- No live OSG traversal on audio/CPU workers. All queued paths own their storage.
- 3 cascades / 2048 / 4096 and existing far shadow policy remain unchanged.

## Implementation checklist

- [x] Canonical terrain texture publication and same-context, revision-checked
  reuse of already-prepared TextureManager textures (not generated blendmaps).
- [x] Cooperative background composite map budget; immediate coverage preserved.
- [x] Weak-value per-state Lua object/cell userdata caches with world reset.
  Native cache algorithm tested; production sol/LuaJIT integration compile pending.
- [x] Bounded sound queue/head-cache integration and loading-time sound discovery.
  Queue/VFS/cache tested; production FFmpeg/SoundManager compile pending.
  Active-cell reprioritization is not implemented.
- [x] Independent startup-opt-in runtime distance/fade/resolution consistency.
  No new shadow GUI or runtime enable/cascade-count/caster-policy changes.
- [ ] Full required-consumer readiness / publication linkage.
- [ ] Shared-context upload experiment only after ownership tests.

## DLSS track / recovery disclosure

Earlier chat reported local commits da5cac7 and 79b6ecb on
p8g5-dlss-temporal-local. Their bytes and Git objects were NOT recovered in the
current runtime, Project/Library search, or Drive search. Do not claim these
commits were preserved. The requirements and reported history are retained;
reconstructed work gets new identifiers and a restorable bundle.

DLSS SR is the first required consumer. Keep native and NIS intact, no fake
selectable DLSS setting, no frame generation, no promised DLSS 5 SDK integration.
Dense moving/deforming object vectors, camera jitter, current/previous matrices,
per-view history reset, shared-API resource ownership, and actual NVIDIA runtime
validation remain necessary. Temporal-only tests do not establish DLSS support.

## Validation performed in this checkpoint

- Six focused native CTest cases pass: canonical cache publication with 16
  competing producers, real-GL composite pixels and yielding, real-GL texture
  revision reuse/update, real-Lua weak-registry behavior, bounded warming queue,
  and real VFS/head-cache replay and ownership.
- The same six pass with Clang AddressSanitizer/UndefinedBehaviorSanitizer.
  The first unsuppressed run reported Mesa GLX process-lifetime leaks in the two
  GL tests (108124 bytes / 481 allocations each, all in libGLX_mesa.so.0).
  The subsequent run suppresses that library only; raw failure evidence is kept.
  This is NOT a general leak-free assertion for OpenMW or the graphics driver.
- Production OpenMWIncrementalCompileOperation, TextureManager, Terrain::World,
  and ChunkManager translation units compile against real OSG 3.6.5 headers.
  CompositeMapRenderer, HeadCache, VFS::Manager and WarmQueue are linked in tests.
- Eight retained source contracts (P3, P4, P6, P7, P8, P8G2, P8G3, P8G4) pass.
  The additional P8U1 source contract checks startup-off flags and retained
  synchronization, loading-only audio discovery, texture identity, and no new
  workflow or BAT. Contracts do not substitute for compilation or runtime QA.

## Explicit validation limits

The local environment lacks the full production LuaJIT, FFmpeg, SDL/Qt and
Recast/Detour SDK set. The Lua fixture uses an explicitly test-only subset of the
public Lua 5.4 C ABI with the native Lua 5.4 runtime. It is not the game's sol
binding test. SoundManager, FFmpegDecoder, LuaManager and the complete executable
have NOT been compiled/linked here. No fresh Windows build, user gameplay,
latency measurement, NVIDIA-hardware test or performance promotion occurred.

The resource changes are an initial safe slice, not completion of the whole
readiness/publication architecture. There is no cross-context upload worker,
GPU completion proof, large-transfer chunking, nor hard GL-call deadline.
Required work can still stall; late texture/blendmap/composite first use remains
an investigation target. LOD2's isolated tail remains workload-confounded.

## Source provenance / preservation

Lua adapter: OpenMW dbe2d5a0fd8d2da80f750deee88897df2593426e.
Sound design donors: 9ec49cfb4709cbfd8f14e97f5b9a558b71b8184f,
62dc7eb099805f8d4021ff20b37bf3ee998ad895,
14f15063544144478a1c43043735a7001945472c, with audio-file filtering from
cf6998aa1e5ef809c7ff06cfdcd3df46a38937ed. This is a selective adaptation,
not an upstream merge. It intentionally does not import a new per-cell world
scan or the global worker-priority helper change.

Current P8G4 branch/Windows artifact, all shaders, one-launcher packaging,
VulkanMW source, saves and user configurations remain unchanged. The next
integration needs full SDK compilation, isolated controls, and runtime
acceptance before a combined release candidate can be called ready.

## P8U1 integration continuation

The resource and reconstructed temporal branches are merged without losing either
source tree. Six comparison modes use one installed BAT. The reference renders
P8G4 CULL-CPU with all new feature flags off; every mode uses the same 96 MiB
head-cache budget. Resource, Lua, warming and shadow settings have independent
startup flags. LOD2 and the inactive shadow proxy candidate remain off in this
comparison, not deleted or declared disproven.

The new CI gate REQUIRES real LuaJIT/sol object-adapter tests, real FFmpeg open/
warm/decode PCM comparisons, production donor translation-unit compilation,
sanitisers, the preserved temporal RG16F tests, retained P8G4/P3/P4 regressions,
Windows PowerShell 5.1, a full Windows executable/test build and installed-package
identity. These are requirements, not claims they have already passed. The old
minimal Lua 5.4 fixture is allowed only for SDK-limited local checks.

Decoder utility definitions were moved verbatim from SoundManager to the existing
sounddecoder compilation unit so the real decoder can be linked in the focused
test without a fake world. Optional warming has loading/shutdown snapshots and
terrain preparation has post-capture request/reuse/submission counters. Neither
claims actual GPU completion or physical transfer bytes.

The shadow patch selectively adapts ca26c7369ee90fd8074908586de3ad4c9e30560a and
29f3f4b071c64ac91c4cc575ae9b08bf6f35c272. Parameter changes stop existing rendering
threads once, preserve their prior running state, replace captured uniforms, and
recreate changed map texture/camera attachments without explicitly releasing old
objects still referenced by a frame. Debug dimensions and far-resolution rules
are shared with the constructor. Invalid dimensions/nonfinite inputs are rejected.

The DLSS source remains standalone engine groundwork. Actual in-game temporal
integration, dynamic/deformed/wind motion, Vulkan interop and the NVIDIA runtime
are not included in this playable candidate. No frame generation, artificial
DLSS setting, or DLSS 5 availability claim is introduced.
