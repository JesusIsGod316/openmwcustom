from pathlib import Path


# Reuse the mature V3.7 diagnostic harness orchestration, changing only the
# packaging layer. V3.10 packaging includes the complete V3.8 + V3.9 stacks first.
base = Path(__file__).with_name("apply_diagnostic_harness_v37.py")
text = base.read_text(encoding="utf-8")
old = '''packaging = Path(__file__).with_name("apply_lab_packaging_v37.py")'''
new = '''packaging = Path(__file__).with_name("apply_lab_packaging_v310.py")'''
if text.count(old) != 1:
    raise RuntimeError("Unable to route V3.10 harness to V3.10 packager")
text = text.replace(old, new, 1)
exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})
