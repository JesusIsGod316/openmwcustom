from pathlib import Path

script = Path(__file__).with_name("apply_v32_hibernation.py")
text = script.read_text(encoding="utf-8")

original_replace = '''def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one V3.2 hibernation match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\\n")
    print(f"V3.2 hibernation patched {rel}")'''

compatible_replace = """def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding=\"utf-8\")
    count = text.count(old)

    if count == 0 and rel == \"apps/openmw/mwrender/renderingmanager.cpp\" \\
            and \"void RenderingManager::removeCell\" in old and \"beginExteriorHibernation\" in new:
        anchor = \"    void RenderingManager::enableTerrain\"
        if text.count(anchor) != 1:
            raise RuntimeError(f\"{rel}: V3.2 compatibility anchor count was {text.count(anchor)}\")
        start = new.find(\"    bool RenderingManager::beginExteriorHibernation()\")
        if start < 0:
            raise RuntimeError(f\"{rel}: V3.2 compatibility insertion start not found\")
        text = text.replace(anchor, new[start:], 1)
        path.write_text(text, encoding=\"utf-8\", newline=\"\\n\")
        print(f\"V3.2 hibernation inserted methods into instrumented {rel}\")
        return

    if count == 0 and rel == \"apps/openmw/mwworld/scene.cpp\" \\
            and \"insertObjectRendering\" in old and \"consumeRestoredExteriorObject\" in new:
        old_guard = '''        if (ptr.getRefData().getBaseNode() || physics.getActor(ptr))
        {
            Log(Debug::Warning) << \"Warning: Tried to add \" << ptr.getCellRef().getRefId() << \" to the scene twice\";
            return;
        }'''
        new_guard = '''        bool restoredRendering = false;
        if (ptr.getRefData().getBaseNode())
            restoredRendering = rendering.consumeRestoredExteriorObject(ptr);
        if ((ptr.getRefData().getBaseNode() && !restoredRendering) || physics.getActor(ptr))
        {
            Log(Debug::Warning) << \"Warning: Tried to add \" << ptr.getCellRef().getRefId() << \" to the scene twice\";
            return;
        }'''
        if text.count(old_guard) != 1:
            raise RuntimeError(f\"{rel}: V3.2 instrumented addObject guard count was {text.count(old_guard)}\")
        text = text.replace(old_guard, new_guard, 1)

        old_render = '''        ESM::RefNum refnum = ptr.getCellRef().getRefNum();
        const bool paged = refnum.hasContentFile() && std::binary_search(pagedRefs.begin(), pagedRefs.end(), refnum);
        auto phaseStart = stats ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        if (!paged)
        {
            ptr.getClass().insertObjectRendering(ptr, model, rendering);
            if (stats)
                ++stats->mRenderedRefs;
        }
        else
            ptr.getRefData().setBaseNode(pagedNode);
        setNodeRotation(ptr, rendering, rotation);
        if (stats)
            stats->mRenderMs += Debug::V3Diagnostics::elapsedMs(phaseStart);'''
        new_render = '''        ESM::RefNum refnum = ptr.getCellRef().getRefNum();
        const bool paged = refnum.hasContentFile() && std::binary_search(pagedRefs.begin(), pagedRefs.end(), refnum);
        if (restoredRendering && paged)
        {
            // Paging policy may change while indoors. Never keep both forms.
            rendering.removeObject(ptr);
            restoredRendering = false;
        }
        auto phaseStart = stats ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        if (!restoredRendering)
        {
            if (!paged)
            {
                ptr.getClass().insertObjectRendering(ptr, model, rendering);
                if (stats)
                    ++stats->mRenderedRefs;
            }
            else
                ptr.getRefData().setBaseNode(pagedNode);
        }
        else if (stats)
            ++stats->mRenderedRefs;
        setNodeRotation(ptr, rendering, rotation);
        if (stats)
            stats->mRenderMs += Debug::V3Diagnostics::elapsedMs(phaseStart);'''
        if text.count(old_render) != 1:
            raise RuntimeError(f\"{rel}: V3.2 instrumented render block count was {text.count(old_render)}\")
        text = text.replace(old_render, new_render, 1)
        path.write_text(text, encoding=\"utf-8\", newline=\"\\n\")
        print(f\"V3.2 hibernation layered reuse into instrumented {rel} addObject\")
        return

    if count != 1:
        raise RuntimeError(f\"{rel}: expected exactly one V3.2 hibernation match, found {count}\")
    path.write_text(text.replace(old, new, 1), encoding=\"utf-8\", newline=\"\\n\")
    print(f\"V3.2 hibernation patched {rel}\")"""

if text.count(original_replace) != 1:
    raise RuntimeError("Unable to install V3.2 hibernation compatibility wrapper")

text = text.replace(original_replace, compatible_replace, 1)
exec(compile(text, str(script), "exec"), {"__file__": str(script), "__name__": "__main__"})
