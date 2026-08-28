from pathlib import Path

# Reuse the V3.14 packager verbatim, changing only the V3.14 source-layer entrypoint
# so it composes with the already-instrumented V3.6 Lua path.
base = Path(__file__).with_name("apply_lab_packaging_v314.py")
text = base.read_text(encoding="utf-8")
old = 'v314 = Path(__file__).with_name("apply_v314_lua_gpu_efficiency.py")'
new = 'v314 = Path(__file__).with_name("apply_v314_lua_gpu_efficiency_fixed.py")'
if text.count(old) != 1:
    raise RuntimeError("Unable to route V3.14 packager through compatibility wrapper")
text = text.replace(old, new, 1)
exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})
