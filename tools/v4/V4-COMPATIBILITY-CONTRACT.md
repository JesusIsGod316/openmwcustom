# OpenMW Custom Build V4 compatibility contract

Compatibility is a primary release goal, not a secondary convenience.

The intended V4 distribution is a normal downloadable OpenMW build that can use existing OpenMW content, Lua/script mods, resource replacers, shaders/post-processing configurations, saves, data paths, and launcher workflows without requiring mod authors to target a private fork. A custom settings profile, launcher option, or small launch BAT may select the modern backend or a compatibility profile, but ordinary users should not need to rebuild mods or maintain a fork-specific installation layout.

## Contract

- Mod-facing OpenMW behavior is the compatibility contract. V3.25 gameplay, animation, content, scripting, save/record, resource lookup, shader/post-processing, and supported configuration semantics remain the reference unless an intentional upstream-compatible extension is documented.
- The modern VSG/Vulkan backend must translate those public semantics rather than expose donor/VSG ownership to mods.
- The legacy OSG/OpenGL backend remains in the same distributable build as a switchable control and compatibility escape path until the modern backend reaches parity.
- Unsupported modern-backend behavior must never be silently omitted. During migration it must either be implemented, explicitly diagnosed, or routed to a documented legacy compatibility path.
- Backend selection is startup/configuration state. It may be surfaced in the launcher/settings and optionally by convenience BAT profiles; it is not a per-frame semantic fork.
- Shader and post-processing compatibility is a first-class parity requirement. Where OpenGL shader source cannot be consumed literally by Vulkan, V4 must preserve the public effect/configuration contract through translation, equivalent implementation, or the legacy backend. A private incompatible shader ecosystem is not the goal.
- Existing mods should not have to know whether the renderer is OSG/OpenGL or VSG/Vulkan. Backend-specific objects stay below the neutral RenderCore boundary.
- New V4 features should prefer additive settings/API capabilities over breaking existing OpenMW-facing behavior.

## Acceptance consequence

No V4 renderer checkpoint is considered final parity merely because stock assets render. Compatibility testing must include representative real OpenMW mods, shader/post-processing configurations, Lua/script behavior, resource replacers, saves, and launcher/settings reuse. Performance claims remain blocked until the applicable parity gate is met.
