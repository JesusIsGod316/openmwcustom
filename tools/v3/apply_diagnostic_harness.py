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

workqueue_safety = Path(__file__).with_name("apply_workqueue_telemetry_safety.py")
exec(compile(workqueue_safety.read_text(encoding="utf-8"), str(workqueue_safety), "exec"), {"__file__": str(workqueue_safety), "__name__": "__main__"})

render = Path(__file__).with_name("apply_render_futureproof_lab.py")
exec(compile(render.read_text(encoding="utf-8"), str(render), "exec"), {"__file__": str(render), "__name__": "__main__"})

postfx_startup_safety = Path(__file__).with_name("apply_postfx_startup_safety.py")
exec(compile(postfx_startup_safety.read_text(encoding="utf-8"), str(postfx_startup_safety), "exec"),
    {"__file__": str(postfx_startup_safety), "__name__": "__main__"})

prepared = Path(__file__).with_name("apply_prepared_instance_lab.py")
exec(compile(prepared.read_text(encoding="utf-8"), str(prepared), "exec"), {"__file__": str(prepared), "__name__": "__main__"})

prepared_safety = Path(__file__).with_name("apply_prepared_instance_safety.py")
exec(compile(prepared_safety.read_text(encoding="utf-8"), str(prepared_safety), "exec"), {"__file__": str(prepared_safety), "__name__": "__main__"})

settings_access_safety = Path(__file__).with_name("apply_settings_access_safety.py")
exec(compile(settings_access_safety.read_text(encoding="utf-8"), str(settings_access_safety), "exec"), {"__file__": str(settings_access_safety), "__name__": "__main__"})

shader = Path(__file__).with_name("apply_shader_compile_lab.py")
exec(compile(shader.read_text(encoding="utf-8"), str(shader), "exec"), {"__file__": str(shader), "__name__": "__main__"})

metrics = Path(__file__).with_name("apply_futureproof_metrics.py")
exec(compile(metrics.read_text(encoding="utf-8"), str(metrics), "exec"), {"__file__": str(metrics), "__name__": "__main__"})

runtime_safety = Path(__file__).with_name("apply_runtime_safety_audit.py")
exec(compile(runtime_safety.read_text(encoding="utf-8"), str(runtime_safety), "exec"),
    {"__file__": str(runtime_safety), "__name__": "__main__"})

prepared_generation_safety = Path(__file__).with_name("apply_prepared_generation_safety.py")
exec(compile(prepared_generation_safety.read_text(encoding="utf-8"), str(prepared_generation_safety), "exec"),
    {"__file__": str(prepared_generation_safety), "__name__": "__main__"})

prepared_fastpath_safety = Path(__file__).with_name("apply_prepared_fastpath_safety.py")
exec(compile(prepared_fastpath_safety.read_text(encoding="utf-8"), str(prepared_fastpath_safety), "exec"),
    {"__file__": str(prepared_fastpath_safety), "__name__": "__main__"})

generated_cpp_safety = Path(__file__).with_name("apply_generated_cpp_string_safety.py")
exec(compile(generated_cpp_safety.read_text(encoding="utf-8"), str(generated_cpp_safety), "exec"), {"__file__": str(generated_cpp_safety), "__name__": "__main__"})

telemetry_safety = Path(__file__).with_name("apply_telemetry_safety.py")
exec(compile(telemetry_safety.read_text(encoding="utf-8"), str(telemetry_safety), "exec"), {"__file__": str(telemetry_safety), "__name__": "__main__"})

feature_retention = Path(__file__).with_name("apply_feature_retention_preflight.py")
exec(compile(feature_retention.read_text(encoding="utf-8"), str(feature_retention), "exec"),
    {"__file__": str(feature_retention), "__name__": "__main__"})

launcher_safety = Path(__file__).with_name("apply_launcher_safety.py")
exec(compile(launcher_safety.read_text(encoding="utf-8"), str(launcher_safety), "exec"), {"__file__": str(launcher_safety), "__name__": "__main__"})

packaging = Path(__file__).with_name("apply_lab_packaging.py")
exec(compile(packaging.read_text(encoding="utf-8"), str(packaging), "exec"), {"__file__": str(packaging), "__name__": "__main__"})
