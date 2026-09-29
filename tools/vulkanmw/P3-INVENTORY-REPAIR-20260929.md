# VulkanMW P3 inventory-boundary repair — 2026-09-29

Parent: `vulkanmw/p3b-compacted-indirect@f0ef81b541870a4492019fe85b53475462c25a52`.
This is a P3A/P3B activation/correctness repair, not P3C or a performance promotion.

## Failure and implementation

The normal inventory gates wrap conformant draw topology in `PipelineInventoryNode`,
which derives from `vsg::Node`, not `vsg::Group`. P3's Group-only construction walk
therefore found no draws when called after conformance returned. The earlier grouping
repair found eligible populations but did not fix this downstream boundary.

`StaticGraphFinalizer` now runs after billboard/sort conformance and before immutable
pipeline-inventory sealing. The host uses that construction-only seam for P3 and
retains both pipeline and resource inventory gates. No live inventory is unwrapped,
no retained graph is re-mutated, and the direct control remains available.

The builder reports empty/missing/incomplete topology and invalid instance-stream
contracts explicitly. It refuses partial graph replacement and bounds P3A indirect
draw count by the device limit. A P3B-to-P3A downgrade also records its reason.

The same queue-feature cohort now requests and checks `multiDrawIndirect` and
`drawIndirectFirstInstance` before logical-device creation. VSG 1.1.15 window traits
only enabled anisotropy by default; reaching P3 without enabling these features would
violate indexed indirect draw requirements. No Vulkan dependency fork is introduced.

## Gates

The retained metadata, source, sanitizer, Windows real-VSG, production compile/link,
OSG capture, installed executable, identity, and launcher checks remain mandatory.
New construction tests cover neither, each, and both inventory gates with startup
flag caching active. New Linux tests link actual production P3, conformance, and
material sources against pinned VSG 1.1.15 and execute Vulkan through Mesa lavapipe.
They require nonzero P3A and compacted P3B coverage, exact direct/P3A/P3B pixel equality,
main-camera visible/empty/repopulation transitions, and an independently visible
secondary camera using the original direct instance stream. Validation errors fail
the job. The original post-seal call is exercised as an explicit negative control
against a separately constructed direct graph.

Windows packaging depends on the real GPU regression job; a source-text pass alone
cannot qualify this repair. The small Windows fixture has no device requirement and
still exercises the real VSG construction seam.

## Runtime acceptance remains open

After the consolidated gate passes, use the installed `START-VulkanMW-P3B-Test.bat`.
Require nonzero `gpu_cull_groups`, `gpu_cull_placements`, and `gpu_indirect_commands`
in both P3A controls and P3B candidates, plus nonzero `gpu_compact_groups` in candidates.
These are sampled population-construction counters, not per-frame visible counts.
Preserve visual QA and matched performance/memory rules. Headless fixture results
are not user RTX 5050 performance or comprehensive modded-world acceptance. Do not
start P3C from a supposed validated base until the new path is actually exercised.
