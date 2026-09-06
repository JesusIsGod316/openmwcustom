# V4 CP3B4 — static NIF corpus and closeout

CP3B4 turns the CP3B3 single-asset conformance executable into a repeatable,
machine-auditable compatibility gate. It deliberately does **not** make game
assets part of the repository or make local install paths part of renderer
identity.

## Design rules

- The authoritative source path remains OpenMW VFS -> current NIF parser ->
  neutral RenderCore model -> backend-private VSG realization.
- A corpus manifest stores only VFS-relative NIF paths plus expected semantic
  dispositions. Local `--data` roots and `--archive` files are supplied at run
  time and are never serialized into the portable manifest.
- The conformance VFS mirrors `components/vfs/registerarchives.cpp`: archives
  are registered first and loose data roots afterward, so loose files override
  archive content. Within archives and within data roots, later entries have the
  higher-priority position. Duplicate loose roots are ignored after their first
  occurrence, matching OpenMW's registration path.
- Archive member names use the same configurable legacy-to-UTF-8 conversion as
  OpenMW. `--encoding` accepts `win1250`, `win1251`, or `win1252`, with OpenMW's
  `win1252` default. The corpus report records the chosen encoding so results do
  not silently mix different archive path interpretations.
- The backend reproduces OpenMW's visible magenta warning-image substitution
  when an external image cannot be opened or decoded, but every substitution is
  counted and diagnosed in `textureDecode`. CP3B4 allows **zero** warning-image
  fallbacks by default. A corpus can raise `maxWarningTextureFallbacks` only for
  a deliberately accepted known case, so compatibility fallback behavior never
  turns into silent asset loss.
- `unsupported`, `deferred`, unsupported texture bindings, and warning-image
  substitutions fail closed by default. A corpus may allow a known
  deferred/static-excluded or fallback case explicitly; this is visible in the
  manifest instead of being hidden in log text.
- An asset must produce at least one meaningful rendered/collision-only/hidden
  outcome by default. This prevents a parsed-but-effectively-empty model from
  satisfying CP3B4 merely because it emitted no error.
- Hidden and collision-only source records are valid dispositions. A rendered
  model should opt into `minRendered` / `minDraws`; a collision-only fixture
  should use its own explicit expectation.
- A suite can declare `requiredTags`; the runner rejects the manifest before any
  tool launch if its asset set does not cover every required compatibility class.
  This makes corpus coverage itself machine-auditable instead of relying on a
  checklist outside the evidence bundle.
- VFS asset names must be normalized relative `.nif` paths. Host drive paths,
  parent traversal, and empty path components are rejected from manifests.
- Human console output remains useful for interactive diagnosis, but automation
  consumes the versioned JSON report contract rather than scraping prose.
- Repeated corpus invocations can require deterministic report equality with
  `--determinism-runs`. This catches source/order/cache nondeterminism without
  changing render semantics.
- No CP3B4 result is a performance claim.

## Report contracts

`openmw-vulkan-nif-conformance --report-json <path>` writes
`openmw-v4-cp3b4-asset-report-v1`. The report records the final conformance
stage, publication status, semantic disposition counts, VSG realization counts,
texture-decode warning fallback count/diagnostics, and structured
translation/realization diagnostics.

`corpus-runner.py` consumes manifests with schema
`openmw-v4-cp3b4-corpus-v1` and writes aggregate reports with schema
`openmw-v4-cp3b4-corpus-report-v1`. The aggregate includes the SHA-256 identity
of both the executable and exact manifest, archive encoding, covered/required
tags, per-asset results, and corpus-wide translation/realization/texture-decode
totals.

The schema version is intentionally explicit so CP3C/CP4 regression tooling can
consume old reports without depending on unstable console wording.

## Local real-asset run

Create a local manifest from `corpus.local.example.json` and replace its
placeholder VFS paths with assets from the user's installed content. The example
already encodes the high-value closeout coverage tags so deleting a case without
updating the suite contract fails immediately. The real set should cover:

- a base Morrowind-style opaque static model;
- alpha-test and alpha-blend models;
- multi-surface / multi-texture material cases;
- switch, LOD, billboard, and sort-sensitive models;
- collision-only / marker-hidden semantic cases;
- modern Bethesda-style NIF/BS geometry and material content;
- Project Cyrodiil and Tamriel Data representative statics.

Example invocation on Windows:

```powershell
python tools/v4/cp3b4/corpus-runner.py `
  --tool .\openmw-vulkan-nif-conformance.exe `
  --manifest .\cp3b4-local-corpus.json `
  --data "D:\Games\OpenMW\Data Files" `
  --archive "D:\Games\OpenMW\Data Files\Morrowind.bsa" `
  --encoding win1252 `
  --output .\cp3b4-corpus-report.json `
  --determinism-runs 2
```

Repeat `--data` in OpenMW's data-root order and `--archive` in the configured
archive order. The tool applies OpenMW's cross-class rule automatically:
archives mount first, then loose data roots, so a loose replacement wins over an
archive entry regardless of CLI grouping. Use the same `encoding` value as the
target OpenMW configuration. Use `--render-id <asset-id>` for one or more
selected visible assets; those entries use the Vulkan window path for
`--render-frames` frames instead of `--realize-only`.

Do not raise `maxWarningTextureFallbacks` merely to make a corpus pass. The
normal closeout target is zero. A nonzero allowance is evidence that a specific
asset is intentionally being accepted with OpenMW's magenta fallback and should
remain visible in the corpus report until the underlying texture issue is
resolved or explicitly deferred by project decision.

## Acceptance boundary refinement

CP3B4 owns static translation/realization correctness, deterministic corpus
accounting, a visible local Vulkan render, and a packaged local test surface.
It does **not** invent a second GPU residency owner solely to satisfy a test.
The current CP3B3 realizer creates VSG resources but is not yet the gameplay
asset residency/cache owner. Therefore persistent device-residency retirement,
repeated in-engine cell load/unload, and authoritative Vulkan memory-budget
churn are gated in CP3C when the real engine producer and backend residency
lifetime are connected. CP2's residency ledger and the 8 GB hard constraint
remain architectural requirements; they are not replaced or weakened here.

This refinement avoids a disposable conformance-only cache that CP3C would
immediately remove, while preserving the stronger compatibility requirement:
no silent content loss and no performance/promotion claim before the real
lifetime path is validated.
