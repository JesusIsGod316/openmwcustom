# OptimizedMW GL-P6 stutter residency pack

Parent: optimizedmw/gl-p4r-scheduler-repair@4f0598cf558554bf1d94820013a4ec41670cfd5e

P6 treats the remaining 30-60 ms OpenGL calls as a residency problem rather than another scheduler-tuning problem.

This first switchable pack establishes safe prerequisites before any shared-context GL ownership transfer:
- producer urgency/deadline metadata;
- heavy background quarantine;
- byte-tier texture/drawable prediction;
- cold terrain prior independent of the old P5 heavy lane;
- immutable terrain vertex/VBO reuse across stitching variants;
- optional OSG PBO texture staging;
- benchmark-only all-frame/hitch/swap telemetry.

Normal gameplay telemetry is off. The P6 launcher explicitly enables benchmark telemetry, while a normal openmw.exe launch sets none of those environment variables.

The shared-context worker remains a follow-up after this matrix tells us which quarantined calls are genuinely required before visibility. It will require explicit publication/fence ownership so background GL work cannot race a live drawable.
