# V4 CP3B3 static NIF → VSG conformance harness

CP3B3 owns the first executable static path from OpenMW's real NIF/VFS parser surface into backend-private VSG objects. It is deliberately a conformance harness, not gameplay renderer integration.

## Ownership boundary

The path is:

`VFS -> Nif::Reader/FileView -> NifRender translation -> RenderCore atomic publication -> static plan -> VSG realization -> CP3B3 compatibility routing -> Vulkan View`

`components/rendercore` remains source-format-, OSG-, VSG-, and Vulkan-free. The tool links OpenMW's authoritative `components` target for parser/VFS/BGSM behavior instead of cloning those systems into the VSG harness.

The production OpenMW CMake graph is not modified to depend on VSG. `OpenMWRealNifTool.cmake` is injected only for the CP3B3 validation configure and defers creation of `openmw-vulkan-nif-conformance` until the normal `components` and pinned `SDL3::SDL3` targets exist.

## Command surface

```text
openmw-vulkan-nif-conformance \
  --data <Data Files root> [--data <later override root> ...] \
  [--archive <BSA-or-BA2> ...] \
  --nif <VFS-relative/path.nif> \
  [--lod-distance <distance>] \
  [--camera-distance <distance>] \
  [--frames <count>] \
  [--realize-only]
```

Later mounted roots/archives retain OpenMW VFS override semantics. Texture/material bytes are reopened through the same winning `VFS::Manager` used for the parsed NIF.

Without `--realize-only`, the tool reuses the CP2 SDL3/Vulkan window foundation, installs the CP3B3 traversal and back-to-front compatibility bins, compiles the realized VSG graph, and presents it until the window closes or `--frames` is reached.

## Validation gate

The CP3B3 Windows workflow first builds and runs the isolated backend realization/routing tests, then configures the repository root with the CP3B3 injection, builds only `openmw-vulkan-nif-conformance` against the authoritative `components` target, and launches `--help` as a dependency/runtime command-surface check. The expensive gate is intentionally batched behind the `[cp3b3-build]` head marker.

## Scope

CP3B3 validates the executable static architecture and exact V3.25 compatibility routing for the supported static surface. Dynamic skinning, morphs, particle systems, and controller realization remain explicitly deferred to CP3D. Unsupported or unsafe static cases fail closed with diagnostics rather than silently crossing the neutral boundary.

No proprietary game asset is checked into the repository. Real local corpus execution and screenshot/evidence closeout are CP3B4 work; the CP3B3 Windows gate validates that the real-NIF executable itself builds and launches its command surface against the authoritative OpenMW components library.
