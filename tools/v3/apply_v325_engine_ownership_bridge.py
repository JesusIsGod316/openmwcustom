import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.25 match(es), found {count}: {old[:100]!r}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.25 patched {rel} ({count} match(es))")


# V3.25 CP1 is deliberately causal:
# - Mode149: inherited V3.24 behavior control (QoS infrastructure, async MSOC OFF).
# - Mode150: identical control + actor animation-source batching.
# The first mechanism removes repeated full-actor AssignControllerSourcesVisitor
# traversals from NpcAnimation::updateNpcBase while preserving source order and
# generic addAnimSource immediate semantics outside an explicit batch.
# Mode151/152 parallel prepare/publish work is intentionally NOT exposed until
# this serial structural reduction compiles and the residual safe kernel is known.

animation_hpp = "apps/openmw/mwrender/animation.hpp"
animation_cpp = "apps/openmw/mwrender/animation.cpp"
npc_cpp = "apps/openmw/mwrender/npcanimation.cpp"

replace_exact(
    animation_hpp,
    "        void addAnimSource(std::string_view model, const std::string& baseModel);\n"
    "        std::shared_ptr<AnimSource> addSingleAnimSource(VFS::Path::NormalizedView kfname, const std::string& baseModel);\n",
    "        void addAnimSource(std::string_view model, const std::string& baseModel);\n"
    "        std::shared_ptr<AnimSource> addSingleAnimSource(VFS::Path::NormalizedView kfname, const std::string& baseModel);\n\n"
    "        // V3.25 bridge primitive: defer actor-global source finalization across a\n"
    "        // known-safe group of ordered animation-source additions. Generic callers\n"
    "        // retain the historical immediate-finalization behavior.\n"
    "        void beginAnimSourceBatch();\n"
    "        void endAnimSourceBatch();\n",
)

replace_exact(
    animation_hpp,
    "        mutable NodeMap mNodeMap;\n"
    "        mutable bool mNodeMapCreated;\n",
    "        mutable NodeMap mNodeMap;\n"
    "        mutable bool mNodeMapCreated;\n\n"
    "        unsigned int mAnimSourceBatchDepth = 0;\n"
    "        bool mAnimSourceBatchNeedsControllerAssignment = false;\n",
)

path = ROOT / animation_cpp
text = path.read_text(encoding="utf-8")
anchor = "    void Animation::addAnimSource(std::string_view model, const std::string& baseModel)\n"
if text.count(anchor) != 1:
    raise RuntimeError(f"{animation_cpp}: V3.25 addAnimSource anchor expected once, found {text.count(anchor)}")
batch_impl = '''    void Animation::beginAnimSourceBatch()
    {
        if (mAnimSourceBatchDepth++ == 0)
            mAnimSourceBatchNeedsControllerAssignment = false;
    }

    void Animation::endAnimSourceBatch()
    {
        if (mAnimSourceBatchDepth == 0)
            return;

        --mAnimSourceBatchDepth;
        if (mAnimSourceBatchDepth != 0 || !mAnimSourceBatchNeedsControllerAssignment)
            return;

        mAnimSourceBatchNeedsControllerAssignment = false;
        if (mObjectRoot)
        {
            SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);
            mObjectRoot->accept(assignVisitor);
        }
    }

'''
text = text.replace(anchor, batch_impl + anchor, 1)
path.write_text(text, encoding="utf-8", newline="\n")
print("V3.25 inserted animation-source batch primitive")

replace_exact(
    animation_cpp,
    "        SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);\n"
    "        mObjectRoot->accept(assignVisitor);\n",
    "        if (mAnimSourceBatchDepth != 0)\n"
    "            mAnimSourceBatchNeedsControllerAssignment = true;\n"
    "        else\n"
    "        {\n"
    "            SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);\n"
    "            mObjectRoot->accept(assignVisitor);\n"
    "        }\n",
)

replace_exact(
    npc_cpp,
    '#include "npcanimation.hpp"\n\n',
    '#include "npcanimation.hpp"\n\n#include <cstdlib>\n',
)

path = ROOT / npc_cpp
text = path.read_text(encoding="utf-8")
namespace_anchor = "namespace\n{\n\n"
if text.count(namespace_anchor) != 1:
    raise RuntimeError(f"{npc_cpp}: anonymous namespace anchor drifted")
mode_helper = '''namespace
{
    bool v325ActorSourceBatchEnabled()
    {
        static const bool enabled = [] {
            const char* value = std::getenv("OPENMW_V325_ACTOR_SOURCE_BATCH");
            return value && value[0] == '1' && value[1] == '\\0';
        }();
        return enabled;
    }

'''
text = text.replace(namespace_anchor, mode_helper, 1)
path.write_text(text, encoding="utf-8", newline="\n")
print("V3.25 inserted actor-source batching runtime gate")

old_sources = '''        if (!base.empty())
            addAnimSource(base, smodel);

        if (defaultSkeleton != base)
            addAnimSource(defaultSkeleton, smodel);

        if (isCustomModel)
            addAnimSource(smodel, smodel);

        const bool customArgonianSwim = !is1stPerson && !isWerewolf && isBeast && mNpc->mRace.contains("argonian");
        if (customArgonianSwim)
            addAnimSource(Settings::models().mXargonianswimkna.get().value(), smodel);
'''
new_sources = '''        const bool v325BatchSources = v325ActorSourceBatchEnabled();
        if (v325BatchSources)
            beginAnimSourceBatch();

        try
        {
            if (!base.empty())
                addAnimSource(base, smodel);

            if (defaultSkeleton != base)
                addAnimSource(defaultSkeleton, smodel);

            if (isCustomModel)
                addAnimSource(smodel, smodel);

            const bool customArgonianSwim
                = !is1stPerson && !isWerewolf && isBeast && mNpc->mRace.contains("argonian");
            if (customArgonianSwim)
                addAnimSource(Settings::models().mXargonianswimkna.get().value(), smodel);
        }
        catch (...)
        {
            if (v325BatchSources)
                endAnimSourceBatch();
            throw;
        }

        if (v325BatchSources)
            endAnimSourceBatch();
'''
replace_exact(npc_cpp, old_sources, new_sources)

replace_exact(
    "apps/openmw/engine.cpp",
    "openmw-custom-v3.23-parallel-msoc / openmw-custom-v3.24-frame-job-qos",
    "openmw-custom-v3.23-parallel-msoc / openmw-custom-v3.24-frame-job-qos / openmw-custom-v3.25-engine-ownership-bridge",
)

launcher_path = ROOT / "tools/v3/launchers/V3_Lab.ps1"
launcher = launcher_path.read_text(encoding="utf-8")

menu146 = next((line for line in launcher.splitlines() if "146 = V3.24" in line), None)
if not menu146:
    raise RuntimeError("V3.25 launcher lost Mode146 menu anchor")
launcher = launcher.replace(
    menu146 + "\n",
    menu146 + "\n"
    + "Write-Host '149 = V3.25 V3.24-behavior control (actor batching OFF)'\n"
    + "Write-Host '150 = V3.25 batched NPC animation-source finalization'\n",
    1,
)

choice_line = next((line for line in launcher.splitlines() if "135-146" in line), None)
if not choice_line:
    raise RuntimeError("V3.25 launcher choice prompt anchor drifted")
new_choice = choice_line.replace("135-146", "135-146, 149-150", 1)
if ",'146'))" not in new_choice:
    raise RuntimeError("V3.25 launcher allowlist anchor drifted")
new_choice = new_choice.replace(",'146'))", ",'146','149','150'))", 1)
launcher = launcher.replace(choice_line, new_choice, 1)

v324_var = "$V324AsyncMsoc = '0'"
if launcher.count(v324_var) != 1:
    raise RuntimeError("V3.25 launcher V3.24 variable anchor drifted")
launcher = launcher.replace(v324_var, v324_var + "\n$V325ActorSourceBatch = '0'", 1)

line145 = next((line for line in launcher.splitlines() if line.lstrip().startswith("'145'")), None)
line146 = next((line for line in launcher.splitlines() if line.lstrip().startswith("'146'")), None)
if not line145 or not line146:
    raise RuntimeError("V3.25 launcher could not recover V3.24 mode bodies")
mode145_body = line145[line145.index("{") + 1:line145.rindex("}")].strip()
mode149 = f"        '149' {{ {mode145_body} }}"
mode150 = f"        '150' {{ {mode145_body}; $V325ActorSourceBatch = '1' }}"
launcher = launcher.replace(line146 + "\n", line146 + "\n" + mode149 + "\n" + mode150 + "\n", 1)

manifest_anchor = '    "v324_async_msoc=$V324AsyncMsoc",'
if launcher.count(manifest_anchor) != 1:
    raise RuntimeError("V3.25 launcher manifest anchor drifted")
launcher = launcher.replace(
    manifest_anchor,
    manifest_anchor + '\n    "v325_actor_source_batch=$V325ActorSourceBatch",',
    1,
)

env_anchor = "    $env:OPENMW_V324_ASYNC_MSOC = $V324AsyncMsoc"
if launcher.count(env_anchor) != 1:
    raise RuntimeError("V3.25 launcher V3.24 environment anchor drifted")
launcher = launcher.replace(
    env_anchor,
    env_anchor + "\n    $env:OPENMW_V325_ACTOR_SOURCE_BATCH = $V325ActorSourceBatch",
    1,
)

cleanup_anchor = "    Remove-Item Env:OPENMW_V324_ASYNC_MSOC -ErrorAction SilentlyContinue"
if launcher.count(cleanup_anchor) != 1:
    raise RuntimeError("V3.25 launcher cleanup anchor drifted")
launcher = launcher.replace(
    cleanup_anchor,
    "    Remove-Item Env:OPENMW_V325_ACTOR_SOURCE_BATCH -ErrorAction SilentlyContinue\n" + cleanup_anchor,
    1,
)

import_csv = "            $rows = @(Import-Csv -LiteralPath $deepPath)"
if launcher.count(import_csv) != 1:
    raise RuntimeError("V3.25 packaging repair could not find V3.24 full Import-Csv materialization")
launcher = launcher.replace(
    import_csv,
    "            # V3.25 packaging repair: never materialize the full deep trace in PowerShell.\n"
    "            # Offline analysis owns aggregation; runtime packaging must remain bounded-memory.\n"
    "            $rows = @()",
    1,
)

launcher_path.write_text(launcher, encoding="utf-8", newline="\n")
print("V3.25 patched launcher modes and bounded-memory packaging")

readme_path = ROOT / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as f:
    f.write('''

V3.25 ENGINE OWNERSHIP BRIDGE - CP1
====================================
V3.25 is the final V3.x line. Mode149 is the V3.24-behavior same-binary control:
frame-job QoS infrastructure remains available but async terrain MSOC and V3.25
actor batching are OFF. Mode150 enables only batched NPC animation-source
finalization. It preserves animation-source insertion order and generic immediate
addAnimSource semantics while deferring repeated actor-wide
AssignControllerSourcesVisitor traversal until the ordered NPC source batch ends.

The V3.24 deep trace remains available for diagnostic runs, but V3.25 no longer
materializes the entire CSV with PowerShell Import-Csv after OpenMW exits. This
prevents multi-gigabyte traces from blocking automatic ZIP creation. Analyze deep
trace aggregation offline instead.

Mode151/152 are intentionally absent in CP1. The next checkpoint will only expose
parallel prepare/publish after Mode150 compiles and establishes how much safe work
remains. V4.0 renderer rearchitecture begins after V3.25 closes.
''')

patch_text = subprocess.run(
    ["git", "diff", "--binary"], cwd=ROOT, check=True, capture_output=True, text=True
).stdout
(ROOT / "V3-applied-source.patch").write_text(patch_text, encoding="utf-8", newline="\n")
stat_text = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, capture_output=True, text=True
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat_text, encoding="utf-8", newline="\n")

checks = {
    animation_hpp: [
        "beginAnimSourceBatch",
        "mAnimSourceBatchDepth",
        "mAnimSourceBatchNeedsControllerAssignment",
    ],
    animation_cpp: [
        "void Animation::beginAnimSourceBatch()",
        "mAnimSourceBatchNeedsControllerAssignment = true",
    ],
    npc_cpp: [
        "OPENMW_V325_ACTOR_SOURCE_BATCH",
        "beginAnimSourceBatch();",
        "endAnimSourceBatch();",
    ],
    "apps/openmw/engine.cpp": ["openmw-custom-v3.25-engine-ownership-bridge"],
    "tools/v3/launchers/V3_Lab.ps1": [
        "149 = V3.25 V3.24-behavior control",
        "150 = V3.25 batched NPC animation-source finalization",
        "OPENMW_V325_ACTOR_SOURCE_BATCH",
        "V3.25 packaging repair",
    ],
}
for rel, markers in checks.items():
    data = (ROOT / rel).read_text(encoding="utf-8")
    for marker in markers:
        if marker not in data:
            raise RuntimeError(f"V3.25 generated-source identity missing {marker!r} in {rel}")

launcher_final = launcher_path.read_text(encoding="utf-8")
if "Import-Csv -LiteralPath $deepPath" in launcher_final:
    raise RuntimeError("V3.25 packaging repair failed: full deep-trace Import-Csv remains")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
print("V3.25 CP1 actor-source batching + bounded-memory packaging layer passed")
