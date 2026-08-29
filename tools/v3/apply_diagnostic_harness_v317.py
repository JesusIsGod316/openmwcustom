from pathlib import Path

# Reuse the mature V3 diagnostic orchestration, changing only the packaging layer.
base = Path(__file__).with_name("apply_diagnostic_harness_v37.py")
text = base.read_text(encoding="utf-8")
old = '''packaging = Path(__file__).with_name("apply_lab_packaging_v37.py")'''
new = '''packaging = Path(__file__).with_name("apply_lab_packaging_v317.py")'''
if text.count(old) != 1:
    raise RuntimeError("Unable to route V3.17 harness to V3.17 packager")
text = text.replace(old, new, 1)
exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})

# Final executable-bound identity layer. The V3.17 source stack previously had
# plenty of source/README markers but no lowercase v3.17 literal guaranteed to
# survive LTO into openmw.exe, so the PE artifact gate correctly rejected it.
# Apply the marker only after every gameplay layer has settled, then refresh the
# exact generated patch snapshot to keep preflight and Windows source identical.
identity = Path(__file__).with_name("apply_v317_binary_identity.py")
exec(
    compile(identity.read_text(encoding="utf-8"), str(identity), "exec"),
    {"__file__": str(identity), "__name__": "__main__"},
)
