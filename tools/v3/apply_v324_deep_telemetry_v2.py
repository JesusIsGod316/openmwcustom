from pathlib import Path

HERE = Path(__file__).resolve().parent
ORIGINAL = HERE / "apply_v324_deep_telemetry.py"

if not ORIGINAL.is_file():
    raise RuntimeError("V3.24 deep telemetry v2 failure: original telemetry layer is missing")

source = ORIGINAL.read_text(encoding="utf-8")


def replace_source_exact(old: str, new: str, label: str) -> None:
    global source
    count = source.count(old)
    if count != 1:
        raise RuntimeError(f"V3.24 deep telemetry v2 failure: {label} expected once, found {count}")
    source = source.replace(old, new, 1)


# Preserve the telemetry-OFF path as a practical identical-binary control. C++
# evaluates function arguments before entering Scope/event, so dynamic detail
# expressions such as std::string(model) or std::to_string(...) would otherwise
# allocate/format even when Scope immediately discovers telemetry is disabled.
# Dynamic detail remains only in FrameJobService::trySubmit, where the generated
# code first returns through the original implementation when telemetry is OFF.
replace_source_exact(
    'insert_scope(animation, "    void Animation::addAnimSource(std::string_view model, const std::string& baseModel)",\n'
    '             "animation", "add_anim_source", "std::string(model)")',
    'insert_scope(animation, "    void Animation::addAnimSource(std::string_view model, const std::string& baseModel)",\n'
    '             "animation", "add_anim_source")',
    "addAnimSource dynamic detail",
)
replace_source_exact(
    '    "                Debug::V324DeepTelemetry::Scope scope(\\\"physics\\\", \\\"apply_queued_movements\\\",\\n"\n'
    '    "                    std::to_string(simulations.size()));\\n"',
    '    "                Debug::V324DeepTelemetry::Scope scope(\\\"physics\\\", \\\"apply_queued_movements\\\");\\n"',
    "physics dynamic detail",
)
replace_source_exact(
    '    "            Debug::V324DeepTelemetry::Scope scope(\\\"framejob\\\", \\\"wait\\\",\\n"\n'
    '    "                std::string(lane == Lane::Critical ? \\\"critical:\\\" : \\\"opportunistic:\\\") + std::to_string(generation));\\n"',
    '    "            Debug::V324DeepTelemetry::Scope scope(\\\"framejob\\\", \\\"wait\\\");\\n"',
    "framejob wait dynamic detail",
)
replace_source_exact(
    '    "                Debug::V324DeepTelemetry::Scope scope(\\\"msoc\\\", \\\"async_input_copy\\\",\\n"\n'
    '    "                    std::to_string(worldPositions.size()) + \\\":\\\" + std::to_string(indices.size()));\\n"',
    '    "                Debug::V324DeepTelemetry::Scope scope(\\\"msoc\\\", \\\"async_input_copy\\\");\\n"',
    "MSOC copy dynamic detail",
)
replace_source_exact(
    '    "                    Debug::V324DeepTelemetry::Scope scope(\\\"msoc\\\", \\\"async_terrain_worker_raster\\\",\\n"\n'
    '    "                        std::to_string(generation));\\n"',
    '    "                    Debug::V324DeepTelemetry::Scope scope(\\\"msoc\\\", \\\"async_terrain_worker_raster\\\");\\n"',
    "MSOC worker dynamic detail",
)

# The inherited launcher has changed its profiler-variable list repeatedly over the
# project. Patch the $allVars block structurally instead of requiring one exact line.
start_marker = 'vars_anchor = "    \'OPENMW_V3_TRACE_FILE\',\'OPENMW_V3_MSOC_DETAIL_FILE\',\'OPENMW_V3_SHADOW_FILE\',\'OPENMW_OSG_STATS_FILE\',\'OPENMW_OSG_STATS_LIST\'"'
start = source.find(start_marker)
end_marker = '\n\nframe_anchor = "$env:OPENMW_V3_FRAME_FILE = Join-Path $ProfileDir \'v3-frames.csv\'"'
end = source.find(end_marker, start if start >= 0 else 0)
if start < 0 or end < 0:
    raise RuntimeError("V3.24 deep telemetry v2 failure: could not locate brittle variable-list patch block")

replacement = r'''vars_start = launcher.find("$allVars = @(")
if vars_start < 0:
    raise RuntimeError("V3.24 deep telemetry variable-list start drifted")
vars_end = launcher.find("\n)", vars_start)
if vars_end < 0:
    raise RuntimeError("V3.24 deep telemetry variable-list end drifted")
vars_block = launcher[vars_start:vars_end]
if "OPENMW_V324_DEEP_TELEMETRY" not in vars_block:
    before_close = launcher[:vars_end]
    separator = "" if before_close.rstrip().endswith(",") else ","
    launcher = (
        launcher[:vars_end]
        + separator
        + "\n    'OPENMW_V324_DEEP_TELEMETRY','OPENMW_V324_DEEP_FILE'"
        + launcher[vars_end:]
    )
'''

source = source[:start] + replacement + source[end:]

exec(
    compile(source, str(ORIGINAL), "exec"),
    {"__file__": str(ORIGINAL), "__name__": "__main__"},
)
