from pathlib import Path

HERE = Path(__file__).resolve().parent
ORIGINAL = HERE / "apply_v324_deep_telemetry.py"

if not ORIGINAL.is_file():
    raise RuntimeError("V3.24 deep telemetry v2 failure: original telemetry layer is missing")

source = ORIGINAL.read_text(encoding="utf-8")

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
