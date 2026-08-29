from pathlib import Path

# Reuse the mature diagnostic orchestration, changing only the packaging layer.
base = Path(__file__).with_name("apply_diagnostic_harness_v37.py")
text = base.read_text(encoding="utf-8")
old = '''packaging = Path(__file__).with_name("apply_lab_packaging_v37.py")'''
new = '''packaging = Path(__file__).with_name("apply_lab_packaging_v316.py")'''
if text.count(old) != 1:
    raise RuntimeError("Unable to route V3.16 harness to V3.16 packager")
text = text.replace(old, new, 1)
exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})
