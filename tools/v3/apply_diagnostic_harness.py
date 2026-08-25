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

fixups = Path(__file__).with_name("apply_optimization_lab_fixups.py")
exec(compile(fixups.read_text(encoding="utf-8"), str(fixups), "exec"), {"__file__": str(fixups), "__name__": "__main__"})

hitch = Path(__file__).with_name("apply_hitch_frametime_lab.py")
exec(compile(hitch.read_text(encoding="utf-8"), str(hitch), "exec"), {"__file__": str(hitch), "__name__": "__main__"})

safety = Path(__file__).with_name("apply_hitch_frametime_safety.py")
exec(compile(safety.read_text(encoding="utf-8"), str(safety), "exec"), {"__file__": str(safety), "__name__": "__main__"})

render = Path(__file__).with_name("apply_render_futureproof_lab.py")
exec(compile(render.read_text(encoding="utf-8"), str(render), "exec"), {"__file__": str(render), "__name__": "__main__"})

prepared = Path(__file__).with_name("apply_prepared_instance_lab.py")
exec(compile(prepared.read_text(encoding="utf-8"), str(prepared), "exec"), {"__file__": str(prepared), "__name__": "__main__"})

shader = Path(__file__).with_name("apply_shader_compile_lab.py")
exec(compile(shader.read_text(encoding="utf-8"), str(shader), "exec"), {"__file__": str(shader), "__name__": "__main__"})

metrics = Path(__file__).with_name("apply_futureproof_metrics.py")
exec(compile(metrics.read_text(encoding="utf-8"), str(metrics), "exec"), {"__file__": str(metrics), "__name__": "__main__"})
