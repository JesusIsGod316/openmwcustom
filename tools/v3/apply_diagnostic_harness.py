from pathlib import Path


# Branch-local router for the shared Windows workflow. The V3.10 layered harness
# uses apply_diagnostic_harness_legacy.py internally, so this entry point can route
# directly to V3.10 without recursion.
v310 = Path(__file__).with_name("apply_diagnostic_harness_v310.py")
exec(compile(v310.read_text(encoding="utf-8"), str(v310), "exec"),
    {"__file__": str(v310), "__name__": "__main__"})
