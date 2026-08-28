from pathlib import Path

# Branch-local router for the shared Windows workflow. V3.15 layers on top of the
# validated V3.14 stack; the generic Windows entry point must route to V3.15 so
# the reusable Windows job builds the same generated source that preflight validates.
v315 = Path(__file__).with_name("apply_diagnostic_harness_v315.py")
print("[V3.15] generic Windows router -> apply_diagnostic_harness_v315.py")
exec(compile(v315.read_text(encoding="utf-8"), str(v315), "exec"),
    {"__file__": str(v315), "__name__": "__main__"})
