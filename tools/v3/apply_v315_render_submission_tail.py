import os
import re
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.15 match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.15 patched {rel} ({count} match(es))")


# V3.15 switches are default-off so Mode80 can be an exact V3.14 Mode79 control.
replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<int> mV314GroundcoverCompileMode{ mIndex, "V3", "v3.14 groundcover compile mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<bool> mV314PostfxCompileWarmup{ mIndex, "V3", "v3.14 postfx compile warmup" };''',
    '''        SettingValue<int> mV314GroundcoverCompileMode{ mIndex, "V3", "v3.14 groundcover compile mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<bool> mV314PostfxCompileWarmup{ mIndex, "V3", "v3.14 postfx compile warmup" };

        SettingValue<bool> mV315PremergeStateCanonicalization{ mIndex, "V3",
            "v3.15 premerge state canonicalization" };
        SettingValue<int> mV315PacketizedPremergeMode{ mIndex, "V3", "v3.15 packetized premerge mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<int> mV315AdaptiveCompileGovernor{ mIndex, "V3", "v3.15 adaptive compile governor",
            makeClampSanitizerInt(0, 2) };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''# Feed active PostFX pass state/programs through the existing IncrementalCompileOperation.
v3.14 postfx compile warmup = false

[Cells]''',
    '''# Feed active PostFX pass state/programs through the existing IncrementalCompileOperation.
v3.14 postfx compile warmup = false

# V3.15 renderer/submission + traversal-tail controls.
# Canonicalize structurally equal immutable StateSets before MERGE_GEOMETRY so
# OpenMW's existing merger can combine more render-equivalent static geometry.
v3.15 premerge state canonicalization = false
# Temporary hierarchical premerge packets for strong prepared active-grid chunks.
# 0=off, 1=4 spatial packets, 2=16 spatial packets. Packets are recombined into
# one final strong topology before the existing Mode79-quality final optimizer.
v3.15 packetized premerge mode = 0
# Adapt ICO's speculative GL compile budget from actual frame duration.
# 0=off/inherited V3.8 pacing, 1=balanced, 2=aggressive tail protection.
v3.15 adaptive compile governor = 0

[Cells]''',
)

# Temporary packetization. V3.12 spatial mode remains historical; V3.15 only
# activates when that final-topology experiment is off.
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        const int v312SpatialBatchMode = static_cast<int>(Settings::cells().mV312SpatialBatchMode);
        const bool v312SpatialPrepared = v312SpatialBatchMode > 0 && activeGrid && compile
            && static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode) >= 2;
        std::vector<osg::ref_ptr<osg::Group>> v312MergeGroups;
        v312MergeGroups.emplace_back(new osg::Group);
        if (v312SpatialPrepared)
        {
            v312MergeGroups.emplace_back(new osg::Group);
            v312MergeGroups.emplace_back(new osg::Group);
            v312MergeGroups.emplace_back(new osg::Group);
        }
        osg::Group* mergeGroup = v312MergeGroups.front().get();''',
    '''        const int v312SpatialBatchMode = static_cast<int>(Settings::cells().mV312SpatialBatchMode);
        const bool v312SpatialPrepared = v312SpatialBatchMode > 0 && activeGrid && compile
            && static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode) >= 2;
        const int v315PacketizedPremergeMode = static_cast<int>(Settings::cells().mV315PacketizedPremergeMode);
        const bool v315PacketizedPrepared = !v312SpatialPrepared && v315PacketizedPremergeMode > 0
            && activeGrid && compile
            && static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode) >= 2
            && static_cast<int>(Settings::cells().mV38WorldBatchingMode) >= 2;

        std::vector<osg::ref_ptr<osg::Group>> v312MergeGroups;
        const unsigned int v315PacketCount
            = v315PacketizedPrepared ? (v315PacketizedPremergeMode >= 2 ? 16u : 4u)
                                     : (v312SpatialPrepared ? 4u : 1u);
        v312MergeGroups.reserve(v315PacketCount);
        for (unsigned int i = 0; i < v315PacketCount; ++i)
            v312MergeGroups.emplace_back(new osg::Group);
        osg::Group* mergeGroup = v312MergeGroups.front().get();''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''                if (merge)
                {
                    if (v312SpatialPrepared)
                    {
                        const unsigned int cluster = (nodePos.x() >= 0.f ? 1u : 0u)
                            | (nodePos.y() >= 0.f ? 2u : 0u);
                        attachTo = v312MergeGroups[cluster].get();
                    }
                    else
                        attachTo = mergeGroup;
                }''',
    '''                if (merge)
                {
                    if (v312SpatialPrepared)
                    {
                        const unsigned int cluster = (nodePos.x() >= 0.f ? 1u : 0u)
                            | (nodePos.y() >= 0.f ? 2u : 0u);
                        attachTo = v312MergeGroups[cluster].get();
                    }
                    else if (v315PacketizedPrepared)
                    {
                        unsigned int cluster = 0;
                        if (v315PacketizedPremergeMode <= 1)
                        {
                            cluster = (nodePos.x() >= 0.f ? 1u : 0u)
                                | (nodePos.y() >= 0.f ? 2u : 0u);
                        }
                        else
                        {
                            const float chunkWorldSize = size * static_cast<float>(cellSize);
                            const float halfChunkWorldSize = chunkWorldSize * 0.5f;
                            const float normalizedX = chunkWorldSize > 0.f
                                ? std::clamp((nodePos.x() + halfChunkWorldSize) / chunkWorldSize, 0.f, 0.999999f)
                                : 0.f;
                            const float normalizedY = chunkWorldSize > 0.f
                                ? std::clamp((nodePos.y() + halfChunkWorldSize) / chunkWorldSize, 0.f, 0.999999f)
                                : 0.f;
                            const unsigned int x = static_cast<unsigned int>(normalizedX * 4.f);
                            const unsigned int y = static_cast<unsigned int>(normalizedY * 4.f);
                            cluster = x | (y << 2u);
                        }
                        attachTo = v312MergeGroups[cluster].get();
                    }
                    else
                        attachTo = mergeGroup;
                }''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        Debug::V36StructureTrace::StructureStats v36BeforeStats;
        if (v36StructureEnabled)''',
    '''        if (v315PacketizedPrepared)
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "v315_packet_premerge", activeGrid ? "active_grid" : "distant", 0.1);
            const osg::Vec3f v315RelativeViewPoint = viewPoint - worldCenter;
            const bool v315CanonicalizePackets
                = static_cast<bool>(Settings::cells().mV315PremergeStateCanonicalization);

            for (const osg::ref_ptr<osg::Group>& packetRef : v312MergeGroups)
            {
                osg::Group* const packet = packetRef.get();
                if (!packet->getNumChildren())
                    continue;

                if (v315CanonicalizePackets)
                    mSceneManager->shareState(packet);

                SceneUtil::Optimizer packetOptimizer;
                if (size > 1 / 8.f)
                {
                    packetOptimizer.setViewPoint(v315RelativeViewPoint);
                    packetOptimizer.setMergeAlphaBlending(true);
                }
                packetOptimizer.setIsOperationPermissibleForObjectCallback(new CanOptimizeCallback);
                constexpr unsigned int v315PacketOptions = SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS
                    | SceneUtil::Optimizer::REMOVE_REDUNDANT_NODES | SceneUtil::Optimizer::MERGE_GEOMETRY;
                packetOptimizer.optimize(packet, v315PacketOptions);
            }

            // Recombine every temporary packet before the final Mode79-quality
            // optimizer. No packet survives as final render topology.
            osg::ref_ptr<osg::Group> recombined = new osg::Group;
            for (const osg::ref_ptr<osg::Group>& packetRef : v312MergeGroups)
            {
                osg::Group* const packet = packetRef.get();
                while (packet->getNumChildren() > 0)
                {
                    osg::ref_ptr<osg::Node> child = packet->getChild(0);
                    packet->removeChild(0);
                    recombined->addChild(child);
                }
            }
            v312MergeGroups.clear();
            v312MergeGroups.emplace_back(recombined);
            mergeGroup = recombined.get();
        }

        Debug::V36StructureTrace::StructureStats v36BeforeStats;
        if (v36StructureEnabled)''',
)

# Exact shared-state canonicalization before the existing geometry merger.
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            optimizer.optimize(mergeGroup, options);

            const bool v39ShareState''',
    '''            const bool v315CanonicalizeBeforeMerge
                = static_cast<bool>(Settings::cells().mV315PremergeStateCanonicalization)
                && compile && v38BatchingMode >= 2;
            if (v315CanonicalizeBeforeMerge)
                mSceneManager->shareState(mergeGroup);

            optimizer.optimize(mergeGroup, options);

            const bool v39ShareState''',
)

# Adaptive ICO pacing. Only speculative/background GL compilation is governed;
# demand scene construction and V3.13 strong-wins installation are untouched.
replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''        reportStats();

        mResourceSystem->getSceneManager()->getShaderManager().update(*mViewer);''',
    '''        const int v315CompileGovernor
            = static_cast<int>(Settings::cells().mV315AdaptiveCompileGovernor);
        if (v315CompileGovernor > 0)
        {
            if (osgUtil::IncrementalCompileOperation* const ico = mViewer->getIncrementalCompileOperation())
            {
                const double frameMs = std::max(0.0, static_cast<double>(dt) * 1000.0);
                int bucket = 0;
                if (paused)
                    bucket = 3;
                else if (v315CompileGovernor == 1)
                {
                    if (frameMs >= 27.0)
                        bucket = 0;
                    else if (frameMs >= 23.0)
                        bucket = 1;
                    else if (frameMs >= 19.0)
                        bucket = 2;
                    else
                        bucket = 3;
                }
                else
                {
                    if (frameMs >= 25.0)
                        bucket = 0;
                    else if (frameMs >= 21.0)
                        bucket = 1;
                    else if (frameMs >= 18.0)
                        bucket = 2;
                    else
                        bucket = 3;
                }

                static int sV315CompileBucket = -1;
                static int sV315CompileMode = -1;
                if (bucket != sV315CompileBucket || v315CompileGovernor != sV315CompileMode)
                {
                    const double configuredTarget = static_cast<double>(Settings::cells().mTargetFramerate);
                    double targetFrameRate = configuredTarget;
                    unsigned int maxObjects = 1;
                    double conservativeRatio = 0.1;

                    if (v315CompileGovernor == 1)
                    {
                        switch (bucket)
                        {
                            case 0:
                                targetFrameRate = configuredTarget;
                                maxObjects = 1;
                                conservativeRatio = 0.10;
                                break;
                            case 1:
                                targetFrameRate = std::min(configuredTarget, 50.0);
                                maxObjects = 2;
                                conservativeRatio = 0.20;
                                break;
                            case 2:
                                targetFrameRate = std::min(configuredTarget, 42.0);
                                maxObjects = 4;
                                conservativeRatio = 0.35;
                                break;
                            default:
                                targetFrameRate = std::min(configuredTarget, 36.0);
                                maxObjects = 8;
                                conservativeRatio = 0.50;
                                break;
                        }
                    }
                    else
                    {
                        switch (bucket)
                        {
                            case 0:
                                targetFrameRate = configuredTarget;
                                maxObjects = 1;
                                conservativeRatio = 0.05;
                                break;
                            case 1:
                                targetFrameRate = std::min(configuredTarget, 55.0);
                                maxObjects = 1;
                                conservativeRatio = 0.15;
                                break;
                            case 2:
                                targetFrameRate = std::min(configuredTarget, 45.0);
                                maxObjects = 3;
                                conservativeRatio = 0.30;
                                break;
                            default:
                                targetFrameRate = std::min(configuredTarget, 33.0);
                                maxObjects = 12;
                                conservativeRatio = 0.60;
                                break;
                        }
                    }

                    ico->setTargetFrameRate(targetFrameRate);
                    ico->setMaximumNumOfObjectsToCompilePerFrame(maxObjects);
                    ico->setConservativeTimeRatio(conservativeRatio);
                    sV315CompileBucket = bucket;
                    sV315CompileMode = v315CompileGovernor;
                }
            }
        }

        reportStats();

        mResourceSystem->getSceneManager()->getShaderManager().update(*mViewer);''',
)

# Launcher modes 80-85. Mode80 exactly copies the generated Mode79 foundation.
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

old = "$V314PostfxCompileWarmup = 'false'\n$RendererProfiling"
new = """$V314PostfxCompileWarmup = 'false'
$V315PremergeStateCanonicalization = 'false'
$V315PacketizedPremergeMode = '0'
$V315AdaptiveCompileGovernor = '0'
$RendererProfiling"""
if text.count(old) != 1:
    raise RuntimeError("V3.15 launcher defaults anchor mismatch")
text = text.replace(old, new, 1)

old_menu = "Write-Host ' 79 = V3.14 aggressive recursive/groundcover preparation'"
new_menu = """Write-Host ' 79 = V3.14 aggressive recursive/groundcover preparation'
Write-Host ' 80 = V3.15 exact V3.14 Mode79 control'
Write-Host ' 81 = V3.15 premerge shared-state canonicalization'
Write-Host ' 82 = V3.15 canonicalization + 4-packet hierarchical premerge'
Write-Host ' 83 = V3.15 canonicalization + adaptive ICO governor'
Write-Host ' 84 = V3.15 full balanced candidate'
Write-Host ' 85 = V3.15 aggressive packet/governor candidate'"""
if text.count(old_menu) != 1:
    raise RuntimeError("V3.15 launcher menu anchor mismatch")
text = text.replace(old_menu, new_menu, 1)

text, n = re.subn(
    r"do \{ \$choice = Read-Host 'Enter 1 through 79' \} until \(\$choice -in @\(([^\n]+)\)\)",
    lambda m: "do { $choice = Read-Host 'Enter 1 through 85' } until ($choice -in @("
    + m.group(1) + ",'80','81','82','83','84','85'))",
    text,
    count=1,
)
if n != 1:
    raise RuntimeError("V3.15 launcher choice-range anchor mismatch")

mode79 = re.search(r"(?m)^    '79' \{[^\n]+\}\n\}", text)
if not mode79:
    raise RuntimeError("V3.15 launcher Mode79 anchor not found")
mode79_line = mode79.group(0).splitlines()[0]
mode79_body = mode79_line[mode79_line.index("{") + 1 : mode79_line.rindex("}")].strip()
control_body = re.sub(
    r"\$Experiment = 'v314-aggressive-prep'", "$Experiment = 'v315-mode79-control'", mode79_body, count=1)
addition = f"""{mode79.group(0)[:-2]}
    '80' {{ {control_body} }}
    '81' {{ {control_body.replace("v315-mode79-control", "v315-state-canonical")}; $V315PremergeStateCanonicalization = 'true' }}
    '82' {{ {control_body.replace("v315-mode79-control", "v315-packet4")}; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1' }}
    '83' {{ {control_body.replace("v315-mode79-control", "v315-ico-governor")}; $V315PremergeStateCanonicalization = 'true'; $V315AdaptiveCompileGovernor = '1' }}
    '84' {{ {control_body.replace("v315-mode79-control", "v315-balanced-full")}; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1' }}
    '85' {{ {control_body.replace("v315-mode79-control", "v315-aggressive-full")}; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '2'; $V315AdaptiveCompileGovernor = '2' }}
}}"""
text = text[:mode79.start()] + addition + text[mode79.end():]

old = '''    "v314_postfx_compile_warmup=$V314PostfxCompileWarmup",
    "shadow_distance=$ShadowDistance",'''
new = '''    "v314_postfx_compile_warmup=$V314PostfxCompileWarmup",
    "v315_premerge_state_canonicalization=$V315PremergeStateCanonicalization",
    "v315_packetized_premerge_mode=$V315PacketizedPremergeMode",
    "v315_adaptive_compile_governor=$V315AdaptiveCompileGovernor",
    "shadow_distance=$ShadowDistance",'''
if text.count(old) != 1:
    raise RuntimeError("V3.15 launcher run-metadata anchor mismatch")
text = text.replace(old, new, 1)

old = """    Set-IniValue $SettingsPath 'V3' 'v3.14 postfx compile warmup' $V314PostfxCompileWarmup
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath"""
new = """    Set-IniValue $SettingsPath 'V3' 'v3.14 postfx compile warmup' $V314PostfxCompileWarmup
    Set-IniValue $SettingsPath 'V3' 'v3.15 premerge state canonicalization' $V315PremergeStateCanonicalization
    Set-IniValue $SettingsPath 'V3' 'v3.15 packetized premerge mode' $V315PacketizedPremergeMode
    Set-IniValue $SettingsPath 'V3' 'v3.15 adaptive compile governor' $V315AdaptiveCompileGovernor
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath"""
if text.count(old) != 1:
    raise RuntimeError("V3.15 launcher settings-write anchor mismatch")
text = text.replace(old, new, 1)
launcher.write_text(text, encoding="utf-8", newline="\n")
print("V3.15 launcher matrix 80-85 patched successfully")
print("V3.15 render-submission/tail layer completed successfully.")
