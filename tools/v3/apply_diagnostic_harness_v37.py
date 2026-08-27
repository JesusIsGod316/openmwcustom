from pathlib import Path


base = Path(__file__).with_name("apply_diagnostic_harness_legacy.py")
text = base.read_text(encoding="utf-8")
old = '''v37_core = Path(__file__).with_name("apply_v37_core.py")
exec(compile(v37_core.read_text(encoding="utf-8"), str(v37_core), "exec"), {"__file__": str(v37_core), "__name__": "__main__"})
packaging = Path(__file__).with_name("apply_lab_packaging.py")
exec(compile(packaging.read_text(encoding="utf-8"), str(packaging), "exec"), {"__file__": str(packaging), "__name__": "__main__"})
'''
new = '''v37_core = Path(__file__).with_name("apply_v37_core.py")
exec(compile(v37_core.read_text(encoding="utf-8"), str(v37_core), "exec"), {"__file__": str(v37_core), "__name__": "__main__"})
packaging = Path(__file__).with_name("apply_lab_packaging_v37.py")
exec(compile(packaging.read_text(encoding="utf-8"), str(packaging), "exec"), {"__file__": str(packaging), "__name__": "__main__"})
'''
if text.count(old) != 1:
    raise RuntimeError("Unable to replace V3.7 harness packaging tail")
text = text.replace(old, new, 1)
exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})
