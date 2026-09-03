import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEEP_INCLUDE = "#include <components/debug/v3deeptelemetry.hpp>"


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.24 telemetry match(es), found {count}: {old[:100]!r}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.24 telemetry patched {rel} ({count} match(es))")


def add_include(rel, anchor):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    if DEEP_INCLUDE in text:
        return
    if text.count(anchor) != 1:
        raise RuntimeError(f"{rel}: telemetry include anchor drifted: {anchor!r}")
    path.write_text(text.replace(anchor, anchor + DEEP_INCLUDE + "\n", 1), encoding="utf-8", newline="\n")


def insert_scope(rel, signature, category, name, detail=''):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    start = text.find(signature)
    if start < 0:
        raise RuntimeError(f"{rel}: telemetry signature not found: {signature}")
    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"{rel}: telemetry body not found: {signature}")
    scope = f'        Debug::V324DeepTelemetry::Scope v324DeepScope("{category}", "{name}"'
    if detail:
        scope += f", {detail}"
    scope += ");\n"
    if scope.strip() in text[brace:brace + 500]:
        raise RuntimeError(f"{rel}: telemetry scope already present: {signature}")
    text = text[:brace + 1] + "\n" + scope + text[brace + 1:]
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"V3.24 telemetry scoped {rel}: {name}")


# The user explicitly permits invasive profiling. This layer therefore prioritizes
# attribution breadth over minimal observer cost, while making the observer cost
# measurable. Deep telemetry remains runtime-switchable so identical-binary OFF/ON
# runs can bound cache, scheduling, allocation and other indirect perturbations.

# --- Mechanics: broad manager + actor/object split. ---
mech = "apps/openmw/mwmechanics/mechanicsmanagerimp.cpp"
add_include(mech, "#include <components/misc/rng.hpp>\n")
insert_scope(mech, "    void MechanicsManager::update(float duration, bool paused)", "mechanics", "manager_update")
replace_exact(
    mech,
    "        mActors.update(duration, paused);\n        mObjects.update(duration, paused);",
    "        {\n"
    "            Debug::V324DeepTelemetry::Scope scope(\"mechanics\", \"actors_update\");\n"
    "            mActors.update(duration, paused);\n"
    "        }\n"
    "        {\n"
    "            Debug::V324DeepTelemetry::Scope scope(\"mechanics\", \"objects_update\");\n"
    "            mObjects.update(duration, paused);\n"
    "        }",
)

# --- Physics: preparation, scheduled movement/collision work, and world commit. ---
physics = "apps/openmw/mwphysics/physicssystem.cpp"
add_include(physics, "#include <components/debug/debuglog.hpp>\n")
insert_scope(physics, "    void PhysicsSystem::prepareSimulation(bool willSimulate, std::vector<Simulation>& simulations)",
             "physics", "prepare_simulation")
insert_scope(physics, "    void PhysicsSystem::stepSimulation(", "physics", "step_simulation")
insert_scope(physics, "    void PhysicsSystem::moveActors()", "physics", "move_actors_commit")
replace_exact(
    physics,
    "            mTaskScheduler->applyQueuedMovements(mTimeAccum, simulations, frameStart, frameNumber, stats);",
    "            {\n"
    "                Debug::V324DeepTelemetry::Scope scope(\"physics\", \"apply_queued_movements\",\n"
    "                    std::to_string(simulations.size()));\n"
    "                mTaskScheduler->applyQueuedMovements(mTimeAccum, simulations, frameStart, frameNumber, stats);\n"
    "            }",
)

# --- Steady animation and transition/controller construction. ---
animation = "apps/openmw/mwrender/animation.cpp"
add_include(animation, "#include <components/debug/debuglog.hpp>\n")
insert_scope(animation, "    void Animation::addAnimSource(std::string_view model, const std::string& baseModel)",
             "animation", "add_anim_source", "std::string(model)")
insert_scope(animation, "    std::shared_ptr<Animation::AnimSource> Animation::addSingleAnimSource(",
             "animation", "add_single_anim_source")
insert_scope(animation, "    osg::Vec3f Animation::runAnimation(float duration)", "animation", "run_animation")

npc = "apps/openmw/mwrender/npcanimation.cpp"
add_include(npc, "#include <components/debug/debuglog.hpp>\n")
insert_scope(npc, "    void NpcAnimation::rebuild()", "animation", "npc_rebuild")
insert_scope(npc, "    void NpcAnimation::updateNpcBase()", "animation", "npc_update_base")
insert_scope(npc, "    osg::Vec3f NpcAnimation::runAnimation(float timepassed)", "animation", "npc_run_animation")

# --- Main-thread rendering update envelope. Existing V3 streams continue to own
# cull/draw/paging/postfx detail; these scopes expose update-side candidates. ---
render = "apps/openmw/mwrender/renderingmanager.cpp"
add_include(render, "#include <components/debug/debuglog.hpp>\n")
insert_scope(render, "    void RenderingManager::update(float dt, bool paused)", "render_update", "manager_update")
replace_exact(
    render,
    "        updateNavMesh();\n        updateRecastMesh();",
    "        {\n"
    "            Debug::V324DeepTelemetry::Scope scope(\"render_update\", \"navmesh_update\");\n"
    "            updateNavMesh();\n"
    "        }\n"
    "        {\n"
    "            Debug::V324DeepTelemetry::Scope scope(\"render_update\", \"recast_update\");\n"
    "            updateRecastMesh();\n"
    "        }",
)
replace_exact(
    render,
    "        mCamera->update(dt, paused);",
    "        {\n"
    "            Debug::V324DeepTelemetry::Scope scope(\"render_update\", \"camera_update\");\n"
    "            mCamera->update(dt, paused);\n"
    "        }",
)

# --- Frame-critical QoS admission/execution/wait behavior. ---
job = "components/sceneutil/framejobservice.hpp"
add_include(job, "#include <thread>\n")
replace_exact(
    job,
    "        bool trySubmit(Lane lane, std::uint64_t generation, std::function<void()> task)\n"
    "        {\n"
    "            return worker(lane).trySubmit(generation, std::move(task));\n"
    "        }",
    "        bool trySubmit(Lane lane, std::uint64_t generation, std::function<void()> task)\n"
    "        {\n"
    "            if (!Debug::V324DeepTelemetry::enabled())\n"
    "                return worker(lane).trySubmit(generation, std::move(task));\n\n"
    "            const std::string laneName = lane == Lane::Critical ? \"critical\" : \"opportunistic\";\n"
    "            auto wrapped = [laneName, generation, task = std::move(task)]() mutable {\n"
    "                Debug::V324DeepTelemetry::Scope scope(\"framejob\", \"execute\",\n"
    "                    laneName + \":\" + std::to_string(generation));\n"
    "                task();\n"
    "            };\n"
    "            const bool accepted = worker(lane).trySubmit(generation, std::move(wrapped));\n"
    "            Debug::V324DeepTelemetry::event(\"framejob\", accepted ? \"submit_accepted\" : \"submit_rejected\",\n"
    "                laneName + \":\" + std::to_string(generation));\n"
    "            return accepted;\n"
    "        }",
)
replace_exact(
    job,
    "        void wait(Lane lane, std::uint64_t generation) { worker(lane).wait(generation); }\n\n"
    "        void noteCallerRuns(Lane lane) { worker(lane).noteCallerRuns(); }\n"
    "        void noteSkipped(Lane lane) { worker(lane).noteSkipped(); }",
    "        void wait(Lane lane, std::uint64_t generation)\n"
    "        {\n"
    "            Debug::V324DeepTelemetry::Scope scope(\"framejob\", \"wait\",\n"
    "                std::string(lane == Lane::Critical ? \"critical:\" : \"opportunistic:\") + std::to_string(generation));\n"
    "            worker(lane).wait(generation);\n"
    "        }\n\n"
    "        void noteCallerRuns(Lane lane)\n"
    "        {\n"
    "            worker(lane).noteCallerRuns();\n"
    "            Debug::V324DeepTelemetry::event(\"framejob\", \"caller_runs\",\n"
    "                lane == Lane::Critical ? \"critical\" : \"opportunistic\");\n"
    "        }\n"
    "        void noteSkipped(Lane lane)\n"
    "        {\n"
    "            worker(lane).noteSkipped();\n"
    "            Debug::V324DeepTelemetry::event(\"framejob\", \"skipped\",\n"
    "                lane == Lane::Critical ? \"critical\" : \"opportunistic\");\n"
    "        }",
)

# --- V3.24 async MSOC: copy/submission-side cost and actual worker raster. ---
msoc = "components/sceneutil/occlusionculling.cpp"
add_include(msoc, "#include <components/sceneutil/framejobservice.hpp>\n")
insert_scope(msoc, "    void OcclusionCuller::rasterizeTerrainOccluder(", "msoc", "terrain_rasterize_entry")
replace_exact(
    msoc,
    "            auto positions = std::make_shared<std::vector<osg::Vec3f>>(worldPositions);\n"
    "            auto ownedIndices = std::make_shared<std::vector<unsigned int>>(indices);",
    "            std::shared_ptr<std::vector<osg::Vec3f>> positions;\n"
    "            std::shared_ptr<std::vector<unsigned int>> ownedIndices;\n"
    "            {\n"
    "                Debug::V324DeepTelemetry::Scope scope(\"msoc\", \"async_input_copy\",\n"
    "                    std::to_string(worldPositions.size()) + \":\" + std::to_string(indices.size()));\n"
    "                positions = std::make_shared<std::vector<osg::Vec3f>>(worldPositions);\n"
    "                ownedIndices = std::make_shared<std::vector<unsigned int>>(indices);\n"
    "            }",
)
replace_exact(
    msoc,
    "                    if (OcclusionCuller::v324RenderOccluderTo(workerMoc, *positions, *ownedIndices, vp))\n"
    "                        readyGeneration->store(generation, std::memory_order_release);",
    "                    Debug::V324DeepTelemetry::Scope scope(\"msoc\", \"async_terrain_worker_raster\",\n"
    "                        std::to_string(generation));\n"
    "                    if (OcclusionCuller::v324RenderOccluderTo(workerMoc, *positions, *ownedIndices, vp))\n"
    "                        readyGeneration->store(generation, std::memory_order_release);",
)

# --- Launcher: explicit identical-binary telemetry OFF/ON selection, plus post-run
# direct-cost aggregation. Indirect observer effect is measured by the OFF/ON A/B. ---
launcher_rel = "tools/v3/launchers/V3_Lab.ps1"
launcher_path = ROOT / launcher_rel
launcher = launcher_path.read_text(encoding="utf-8")

stamp_anchor = "$Stamp = Get-Date -Format 'yyyyMMdd_HHmmss'"
if launcher.count(stamp_anchor) != 1:
    raise RuntimeError("V3.24 deep telemetry launcher stamp anchor drifted")
prompt = '''$V324DeepTelemetry = '0'\nWrite-Host ''\nWrite-Host 'V3.24 deep optimization telemetry:' -ForegroundColor Cyan\nWrite-Host '  0 = OFF (identical-binary observer-effect control)'\nWrite-Host '  1 = ON  (invasive self-accounting mechanics/physics/animation/render/QoS/MSOC trace)'\ndo { $deepChoice = Read-Host 'Enter 0 or 1' } until ($deepChoice -in @('0','1'))\nif ($deepChoice -eq '1') {\n    $V324DeepTelemetry = '1'\n    $Experiment = "$Experiment-deep"\n}\n\n'''
launcher = launcher.replace(stamp_anchor, prompt + stamp_anchor, 1)

manifest_anchor = '    "game_dir=$GameDir"'
if launcher.count(manifest_anchor) != 1:
    raise RuntimeError("V3.24 deep telemetry manifest anchor drifted")
launcher = launcher.replace(manifest_anchor, manifest_anchor + ',\n    "v324_deep_telemetry=$V324DeepTelemetry"', 1)

vars_anchor = "    'OPENMW_V3_TRACE_FILE','OPENMW_V3_MSOC_DETAIL_FILE','OPENMW_V3_SHADOW_FILE','OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'"
if launcher.count(vars_anchor) != 1:
    raise RuntimeError("V3.24 deep telemetry variable-list anchor drifted")
launcher = launcher.replace(
    vars_anchor,
    "    'OPENMW_V3_TRACE_FILE','OPENMW_V3_MSOC_DETAIL_FILE','OPENMW_V3_SHADOW_FILE','OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST',\n"
    "    'OPENMW_V324_DEEP_TELEMETRY','OPENMW_V324_DEEP_FILE'",
    1,
)

frame_anchor = "$env:OPENMW_V3_FRAME_FILE = Join-Path $ProfileDir 'v3-frames.csv'"
if launcher.count(frame_anchor) != 1:
    raise RuntimeError("V3.24 deep telemetry frame stream anchor drifted")
launcher = launcher.replace(
    frame_anchor,
    frame_anchor
    + "\nif ($V324DeepTelemetry -eq '1') {\n"
      "    $env:OPENMW_V324_DEEP_TELEMETRY = '1'\n"
      "    $env:OPENMW_V324_DEEP_FILE = Join-Path $ProfileDir 'v324-deep-trace.csv'\n"
      "}",
    1,
)

wait_anchor = "    $process.WaitForExit()"
if launcher.count(wait_anchor) != 1:
    raise RuntimeError("V3.24 deep telemetry process wait anchor drifted")
summary = r'''
    if ($V324DeepTelemetry -eq '1') {
        $deepPath = Join-Path $ProfileDir 'v324-deep-trace.csv'
        if (Test-Path -LiteralPath $deepPath) {
            $rows = @(Import-Csv -LiteralPath $deepPath)
            if ($rows.Count -gt 0) {
                $sumSetup = [double](($rows | Measure-Object -Property scope_setup_ms -Sum).Sum)
                $sumFormat = [double](($rows | Measure-Object -Property prev_format_ms -Sum).Sum)
                $sumLock = [double](($rows | Measure-Object -Property prev_lock_wait_ms -Sum).Sum)
                $sumOpen = [double](($rows | Measure-Object -Property prev_open_ms -Sum).Sum)
                $sumWrite = [double](($rows | Measure-Object -Property prev_write_ms -Sum).Sum)
                $sumFlush = [double](($rows | Measure-Object -Property prev_flush_ms -Sum).Sum)
                $sumBytes = [double](($rows | Measure-Object -Property prev_bytes -Sum).Sum)
                $direct = $sumSetup + $sumFormat + $sumLock + $sumOpen + $sumWrite + $sumFlush
                $overhead = @(
                    "rows=$($rows.Count)",
                    "reported_prev_bytes=$([long]$sumBytes)",
                    "scope_setup_ms=$('{0:F6}' -f $sumSetup)",
                    "format_ms=$('{0:F6}' -f $sumFormat)",
                    "lock_wait_ms=$('{0:F6}' -f $sumLock)",
                    "open_ms=$('{0:F6}' -f $sumOpen)",
                    "write_ms=$('{0:F6}' -f $sumWrite)",
                    "flush_ms=$('{0:F6}' -f $sumFlush)",
                    "direct_recorded_profiler_ms=$('{0:F6}' -f $direct)",
                    "accounting_note=writer costs are carried by the following row; only the final writer operation is not directly carried forward",
                    "observer_effect_note=compare identical mode telemetry OFF vs ON; that delta bounds cache scheduling allocation and the unreported final writer operation"
                )
                [System.IO.File]::WriteAllLines((Join-Path $ProfileDir 'v324-profiler-overhead-summary.txt'), $overhead, [System.Text.UTF8Encoding]::new($false))
            }
        }
    }'''
launcher = launcher.replace(wait_anchor, wait_anchor + summary, 1)
launcher_path.write_text(launcher, encoding="utf-8", newline="\n")
print("V3.24 telemetry patched launcher OFF/ON control and profiler-cost summary")

readme = ROOT / "V3-LAB-README.txt"
with readme.open("a", encoding="utf-8", newline="\n") as f:
    f.write('''\n\nV3.24 DEEP SELF-ACCOUNTING TELEMETRY\n=====================================\nThe launcher now asks for deep telemetry OFF or ON for every mode. OFF and ON use\nthe identical binary. ON emits v324-deep-trace.csv with invasive scopes for\nmechanics actor/object updates, physics preparation/simulation/commit, steady\nanimation, NPC rebuild/source construction, render-update work, FrameJobService\nadmission/execution/waits, and async MSOC copy/worker raster. Existing V3 paging,\nstreaming, Lua, resource, workqueue, render, postfx, MSOC-detail and OSG streams\nremain available.\n\nThe deep writer self-accounts scope setup plus formatting, writer-lock wait, file\nopen, write, flush and bytes. Writer costs are carried on the following trace row\nto avoid recursively timing the profiler. After the process exits the launcher\nproduces v324-profiler-overhead-summary.txt. The final writer operation is the only\ndirect writer cost not carried forward; the identical-mode telemetry OFF -> ON\ncomparison is the authoritative observer-effect bound and also captures cache,\nscheduling and allocation perturbation that direct profiler self-time cannot.\n\nRecommended diagnostic sequence: Mode135 OFF, Mode145 OFF, Mode145 ON, then\nMode146 ON. Use the same save/settings/mod cohort and route. Mode135 remains the\nclean historical control; Mode145 OFF isolates V3.24 QoS infrastructure; Mode145\nON sizes broader future threading targets; Mode146 ON adds the zero-wait async\nMSOC consumer and its worker-side telemetry.\n''')

# Rebuild generated source identity after the telemetry layer.
patch_text = subprocess.run(
    ["git", "diff", "--binary"], cwd=ROOT, check=True, capture_output=True, text=True
).stdout
(ROOT / "V3-applied-source.patch").write_text(patch_text, encoding="utf-8", newline="\n")
stat_text = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, capture_output=True, text=True
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat_text, encoding="utf-8", newline="\n")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)

# Fail loudly if any core diagnostic lane silently disappears.
checks = {
    "components/debug/v3deeptelemetry.hpp": ["OPENMW_V324_DEEP_TELEMETRY", "prev_lock_wait_ms"],
    mech: ["mechanics\", \"actors_update", "mechanics\", \"objects_update"],
    physics: ["physics\", \"prepare_simulation", "physics\", \"apply_queued_movements"],
    animation: ["animation\", \"run_animation", "animation\", \"add_single_anim_source"],
    npc: ["animation\", \"npc_rebuild", "animation\", \"npc_update_base"],
    render: ["render_update\", \"manager_update", "render_update\", \"camera_update"],
    job: ["framejob\", \"execute", "submit_accepted"],
    msoc: ["msoc\", \"async_input_copy", "msoc\", \"async_terrain_worker_raster"],
    launcher_rel: ["v324-deep-trace.csv", "v324-profiler-overhead-summary.txt", "observer-effect control"],
}
for rel, markers in checks.items():
    text = (ROOT / rel).read_text(encoding="utf-8")
    for marker in markers:
        if marker not in text:
            raise RuntimeError(f"V3.24 deep telemetry marker missing from {rel}: {marker}")

print("V3.24 invasive self-accounting deep telemetry layer passed")
