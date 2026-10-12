# UI reflow candidate

`OPENMW_HITCH_UI_REFLOW=1` enables a startup-frozen candidate. Missing, `0`, and
malformed values retain the control. The environment is read once before main;
widget, property, and measurement hot paths do not call getenv.

The candidate avoids MyGUI EditBox invalidation for an unchanged actual size,
nonzero font height, or wrap setting. Initial font application is retained to
establish custom font semantics, and an explicit zero font height still reaches
MyGUI to select the font default. Both LuaText and LuaTextEdit parse current Lua
property/template values on every explicit update; mutable table identity is
never a cache key.

Text measurement uses MyGUI's existing dirty cache. No second measurement cache
can retain stale text after caption, interactive edit, font/skin, width, or
viewport changes. Actual size changes always reach MyGUI. Recursive layout,
caption replacement, multiline/static setters, selection and undo effects,
callbacks, detach/attach order, and element lifetime remain on their existing
paths. This is not the broader transactional-layout proposal.

The candidate's private EditBox child forwards the unchanged MyGUI skin
initialization while advancing a generation. Font/wrap/size reuse is invalidated
after client recreation; raw pointer equality is not an ownership proof. The
control continues to construct the original MyGUI EditBox child. LuaText resets
its applied-property state on its own skin initialization.

The production fixture is `ui-reflow-tests.cpp`, with paired-process validation
in `verify-ui-reflow.py`. It uses the real components and MyGUI engine, the
production `openmw_lua.xml` skins, nested LuaFlex/LuaContainer/LuaTextEdit/LuaText
elements, mutable property/template updates, wrapped autosize, width and font
changes, zero/default font, resize, actual keyboard edit/undo, Lua textChanged
callbacks, selection, focus, and destruction. A synthetic counting font records
actual glyph visits from MyGUI's text measurement; it does not reproduce the
candidate or implement layout. Geometry/text/edit/event snapshots must match
control, and repeated stable layout must make fewer glyph visits. Missing and
malformed gates must reproduce control; runtime environment mutation must not
change subsequent widget policy.

A fifth process enables the existing Deep HitchTrace writer for the candidate.
It must retain identical layout/edit/event snapshots and glyph reuse, include
new UI attribution rows with valid parent links and wall-only timing, and finish
with no dropped rows, allocation failure, or active scopes. Fast subspans remain
subject to the production threshold; this check does not require every setter
to produce a row.

Integration into the existing game build can add executable `p9-ui-reflow` from
this source, link `openmw-lib` (as the existing p9-postfx-ownership fixture does),
include the source root, and define `P9_SOURCE="${PROJECT_SOURCE_DIR}"`. It needs
the same real MyGUI, LuaJIT/sol, SDL3, OSG, and components dependencies as the
game, not a stub component library. Add a CTest invocation of the Python
validator with the executable path and a private validation output directory.
The fixture does not launch the game, create a native window, or start WPR.

Source/test/build success must be reported separately from user runtime
acceptance. Reduced glyph visits prove removal of repeated CPU layout work,
not a claimed number of recovered milliseconds or a general FPS improvement.

Deep captures also use the existing bounded HitchTrace writer to split UI work
into element/widget/content/template/event updates, generic coordinate and
child layout, Flex layout, Container measurement, and text/edit property,
caption, font, wrap, size, and text measurement operations. These scopes are
wall envelopes: they do not query OS CPU timers, force retention, or replace
the existing enclosing UI CPU measurement. Their parent links preserve nested
inclusive/exclusive interpretation. The existing 250 microsecond threshold and
child retention rules apply, so fast calls may leave no individual row.
Naturally retained property/layout rows include applied/skipped setter counts
and child/track counts. Missing child rows or residual parent time are not
proof that a particular fast operation took zero time; capture loss still
invalidates completeness. Normal gameplay creates no capture rows.
