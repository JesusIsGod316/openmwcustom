from pathlib import Path


base = Path(__file__).with_name("apply_lab_packaging.py")
text = base.read_text(encoding="utf-8")
marker = '''# Install the V3 helper launchers and exact applied-source snapshot beside the
# runtime executable. This script only runs through the V3 harness, so upstream/default builds are unaffected.'''
insert = '''# V3.7 is deliberately layered after every V3.6 generator and before packaging
# validates/captures V3-applied-source.patch.
v37_core = Path(__file__).with_name("apply_v37_core.py")
exec(compile(v37_core.read_text(encoding="utf-8"), str(v37_core), "exec"),
    {"__file__": str(v37_core), "__name__": "__main__"})

''' + marker
if text.count(marker) != 1:
    raise RuntimeError("Unable to locate V3 packaging insertion marker")
text = text.replace(marker, insert, 1)
exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})
