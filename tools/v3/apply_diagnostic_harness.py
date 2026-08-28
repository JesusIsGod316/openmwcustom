from pathlib import Path


# Branch-local router for the shared Windows workflow. The V3.11 layered harness
# reuses the validated V3.10 stack internally, so this entry point can route
# directly to V3.11 without recursion.
v311 = Path(__file__).with_name("apply_diagnostic_harness_v311.py")
exec(compile(v311.read_text(encoding="utf-8"), str(v311), "exec"),
    {"__file__": str(v311), "__name__": "__main__"})
