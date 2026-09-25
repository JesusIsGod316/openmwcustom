# VulkanMW Phase 1 — Native NIF Semantic Compiler

Status: implementation started
Branch: vulkanmw/phase1-nif-semantic-compiler
Parent: vulkanmw/phase0-foundation@086732cf494c31d4b823bc774e2929a6a75d4d67

## Source audit result

Phase 1 does not need a second NIF translator.

The retained CP3B `components/nifrender` path already performs the essential native conversion:

`Nif::FileView -> NifRender::TranslationBundle -> RenderCore semantic records`

It directly consumes current OpenMW NIF parser records and emits neutral meshes, materials, textures, model hierarchy, skins, morphs and skeleton data. It does not traverse an OSG scene graph.

Some NIF parser records still expose OSG math container types such as `osg::Vec*`. That is a parser/math legacy seam, not live scene-graph ownership. Phase 1 does not require a project-wide NIF parser math rewrite.

## Phase 1 implementation decision

Promote the proven CP3B translator behind `RenderNative::NifSemanticCompiler` rather than duplicating it.

The compiler owns the source boundary:
- winning VFS existence
- NIF parse
- direct static semantic translation
- explicit parse/translation status
- fail-closed diagnostics

The existing `TranslationBundle` remains staging data until deterministic publication assigns stable RenderWorld handles.

## Phase 1 stages

P1A:
- add NifSemanticCompiler API
- add no-live-OSG source contract
- compile it in the Vulkan runtime source closure
- route the existing static lifecycle translation call through it

P1B:
- define compiler/cache identity around winning VFS path + content identity + semantic schema
- move static model lookup/compile/publish behind one native asset service
- eliminate duplicate source parsing where metadata inspection can be folded into native semantic metadata

P1C:
- expand corpus and deterministic output checks
- verify base Morrowind + Project Cyrodiil + Tamriel Rebuilt representative NIFs
- preserve explicit fail-closed diagnostics for dynamic/unsupported semantics
- prepare Phase 2 native static-world lifecycle to depend only on the native asset service

## Phase 1 acceptance

Phase 1 is not complete until:
- native compiler source contract is green
- production Vulkan source closure compiles
- representative static NIF corpus translates deterministically
- static model publication consumes native compiler output
- normal Phase 1 static compilation constructs no OSG rendering nodes
- OpenGL/NifOsg remains unchanged as control/reference
- no Vulkan runtime performance claim is made before later parity gates
