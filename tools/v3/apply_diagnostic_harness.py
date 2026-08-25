from pathlib import Path

base = Path(__file__).with_name("apply_diagnostic_harness_base.py")
text = base.read_text(encoding="utf-8")
old = '''replace_once(
    "components/sceneutil/occlusionculling.hpp",
    \'\'\'            mFrameActive = false;
        }

        unsigned int getNumOccluded() const\'\'\',
    \'\'\'            if (mDetailedTelemetryEnabled)
                writeDetailedTelemetryRow();

            mFrameActive = false;
        }

        unsigned int getNumOccluded() const\'\'\'
)'''
new = '''replace_once(
    "components/sceneutil/occlusionculling.hpp",
    \'\'\'            mFrameActive = false;
        }

        void setTelemetryFrameNumber\'\'\',
    \'\'\'            if (mDetailedTelemetryEnabled)
                writeDetailedTelemetryRow();

            mFrameActive = false;
        }

        void setTelemetryFrameNumber\'\'\'
)'''
if old not in text:
    raise RuntimeError("Unable to apply V3 harness ordering fix")
text = text.replace(old, new, 1)
exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})

extra = Path(__file__).with_name("apply_diagnostic_harness_extra.py")
exec(compile(extra.read_text(encoding="utf-8"), str(extra), "exec"), {"__file__": str(extra), "__name__": "__main__"})

lab = Path(__file__).with_name("apply_optimization_lab.py")
exec(compile(lab.read_text(encoding="utf-8"), str(lab), "exec"), {"__file__": str(lab), "__name__": "__main__"})
