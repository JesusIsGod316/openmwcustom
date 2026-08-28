from pathlib import Path

# Branch-local router for the shared Windows workflow. V3.13 layers on top of the
# validated V3.12 stack; the generic Windows entry point must route to V3.13.
v313 = Path(__file__).with_name("apply_diagnostic_harness_v313.py")
print("[V3.13] generic Windows router -> apply_diagnostic_harness_v313.py")
exec(compile(v313.read_text(encoding="utf-8"), str(v313), "exec"),
    {"__file__": str(v313), "__name__": "__main__"})
