from pathlib import Path


# The shared Windows workflow knows legacy branch names through V3.9. On this
# dedicated V3.10 branch it falls through to apply_diagnostic_harness.py, so route
# that branch-local entry point into the complete V3.10 harness. This change is
# intentionally isolated to the V3.10 branch and does not alter older branches.
v310 = Path(__file__).with_name("apply_diagnostic_harness_v310.py")
exec(compile(v310.read_text(encoding="utf-8"), str(v310), "exec"),
    {"__file__": str(v310), "__name__": "__main__"})
