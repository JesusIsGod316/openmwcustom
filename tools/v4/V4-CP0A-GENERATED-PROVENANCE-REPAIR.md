# V4.0 CP0A — Generated-output provenance/materialization repair

Status: **AUDIT COMPLETE; SIX OMITTED OUTPUTS MATERIALIZED; FAIL-CLOSED QC ACTIVE; FULL CP1A REBUILD AUTHORIZED**

## Finding

The original CP0A materialization reproduced the frozen final V3.25 generator stack correctly, verified the frozen `V3-applied-source.patch`, and then staged exactly the 103 paths named by that patch. That rule was incomplete: `git diff` does not include newly-created untracked files unless the generator explicitly marks them intent-to-add. Final V3.25 therefore had generated helpers outside the 103-path patch that were present in the successful V3 build worktree but absent from the committed V4 lineage.

This is a CP0A source-provenance/materialization defect, not a RenderCore design or implementation failure.

## Reproduction audit

Authoritative clean pre-materialization base:

`0c47f593c9c464ef158bade8604bfda536a6d628`

Frozen V3.25 source identity:

`f7557829bcb14e339410cefb32b6612e5009e46d`

Dedicated audit branch:

`v4.0-cp0a-generated-provenance-audit`

The frozen V3.25 harness was rerun from the clean pre-materialization base. The audit found:

- 103 paths represented by the frozen source patch;
- 10 untracked generator outputs total;
- 4 already-recognized packaging/provenance outputs (`V3-applied-source.patch`, `V3-applied-source-stat.txt`, `V3-BUILD-IDENTITY.txt`, `V3-LAB-README.txt`);
- 6 additional generated outputs omitted from the 103-path materialization.

The six omissions are frozen in `V4-CP0A-GENERATED-OUTPUT-MANIFEST.txt`:

| Path | SHA-256 | Role |
| --- | --- | --- |
| `V3.16-HITCH-LAYER.txt` | `8dadeae8c25896e6c42f615f2e1eceffda8587e9b10863c34fbf520dfa9d3b0a` | generated V3.16 layer provenance |
| `V3.17-RUNTIME-LAYER.txt` | `100ee2b424d8bdc9053b0e35d5ed00b2add420270e11d7e9f7c0c211713a305e` | generated V3.17 layer provenance |
| `V3.18-NIS-PROVENANCE.txt` | `eef4f8aefe16b89b671334df7b952b9ddbd35696976d0a8477afb669c83c70e6` | generated NIS source/integration provenance |
| `V3.18-RENDER-LAYER.txt` | `60fc90c44914db5178b9c334d0106b037568b44f7656ff15fc68a36209704d10` | generated V3.18 render-layer provenance |
| `components/debug/v320luafastpath.hpp` | `33a2a4c3b29939ec235ade23acf75cbbe7f098413715860db1d5095e04513d99` | compile-critical generated V3.20 header |
| `components/resource/v321classifiedcompileset.hpp` | `a0959b688f283286c46e44fc28e1bc20dec5b5ff7c0d5443728217fffa390a2a` | compile-critical generated V3.21 header |

The audit branch then materialized those six exact files from a fresh frozen-generator reproduction and verified every byte against the audit hashes before committing them.

## Failure attribution

The first CP1A Windows build stopped in previously materialized V3 resource source because `components/resource/v321classifiedcompileset.hpp` was absent. A temporary one-header repair allowed compilation to continue, where the next build stopped in existing V3 Lua source because `components/debug/v320luafastpath.hpp` was absent.

Those failures are evidence for this CP0A defect. They are not CP1 RenderWorld/RenderCore implementation regressions and must not be counted as CP1 performance or correctness results.

## Repair on the active CP1A lineage

The active `v4.0-cp1a-rendercore-foundation` lineage now contains all six audited outputs. The previously-restored V3.21 header was checked against the frozen audit hash and is exact.

`tools/v4/V4-CP0A-Verify-Materialized-Generated-Outputs.py` is now a mandatory CP1A preflight gate. It:

1. locks the pre-materialization base, frozen V3.25 source identity, 103-patch-path count, and six-file omission set;
2. verifies the exact SHA-256 of every audited generated output;
3. requires every output to be tracked by Git;
4. scans tracked C/C++ source for repository-local V3 component-header includes and fails if any referenced V3 component header is unresolved.

The CP1A workflow runs this gate before RenderCore boundary checks and before any Windows build. This converts the original silent packaging hole into a cheap fail-closed preflight failure.

## Build sequencing

No full build was authorized while the six-file audit was incomplete. After the exact payload and QC gate were installed, the Linux CP1A preflight passed. This commit authorizes one full Windows CP1A rebuild from the coherently repaired lineage.

If that build fails, classify the failure from its actual compiler/test evidence. Do not automatically attribute a later failure to RenderCore or to the already-closed six-file omission set.
