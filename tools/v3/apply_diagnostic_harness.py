from pathlib import Path

# Branch-local router for the shared Windows workflow. V3.14 layers on top of the
# validated V3.13 stack; the generic Windows entry point must route to V3.14.
v314 = Path(__file__).with_name("apply_diagnostic_harness_v314.py")
print("[V3.14] generic Windows router -> apply_diagnostic_harness_v314.py")
exec(compile(v314.read_text(encoding="utf-8"), str(v314), "exec"),
    {"__file__": str(v314), "__name__": "__main__"})
