from pathlib import Path


# Branch-local router for the shared Windows workflow. V3.12 layers on top of the
# validated V3.11 stack, so the generic Windows entry point must route to V3.12.
v312 = Path(__file__).with_name("apply_diagnostic_harness_v312.py")
exec(compile(v312.read_text(encoding="utf-8"), str(v312), "exec"),
    {"__file__": str(v312), "__name__": "__main__"})
