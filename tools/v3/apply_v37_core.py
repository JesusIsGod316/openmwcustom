import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.7 core match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.7 core patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# V3.7 profile: promote the visually validated 2-pixel far-caster pruning into
# the normal performance profile while keeping a dedicated troubleshooting kill
# switch. New unproven work stays default-off until benchmarked.
# -----------------------------------------------------------------------------
replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<float> mV36FarCasterMinimumPixels{ mIndex, "V3", "v3.6 far caster minimum pixels",
            makeClampSanitizerFloat(0, 32) };''',
    '''        SettingValue<float> mV36FarCasterMinimumPixels{ mIndex, "V3", "v3.6 far caster minimum pixels",
            makeClampSanitizerFloat(0, 32) };
        SettingValue<bool> mV37DisableFarCasterPruning{
            mIndex, "V3", "v3.7 disable far caster pruning" };
        SettingValue<bool> mV37ActiveEventFastPath{ mIndex, "V3", "v3.7 active event fast path" };''',
)

replace_exact(
    "components/settings/v36profile.hpp",
    '''    inline bool coarseChunkOcclusionEnabled()
    {
        if (enabled())
            return !static_cast<bool>(cells().mV36DisableCoarseChunkOcclusion);
        return static_cast<bool>(camera().mV35CoarseChunkOcclusion);
    }
}''',
    '''    inline bool coarseChunkOcclusionEnabled()
    {
        if (enabled())
            return !static_cast<bool>(cells().mV36DisableCoarseChunkOcclusion);
        return static_cast<bool>(camera().mV35CoarseChunkOcclusion);
    }

    inline float farCasterMinimumPixels()
    {
        // V3.6/6144 A-B testing validated 2 px with no user-visible artifacts.
        // The V3.7 disable switch is deliberately independent so this proven
        // optimization can still be isolated without disabling the rest of the profile.
        if (enabled())
            return static_cast<bool>(cells().mV37DisableFarCasterPruning) ? 0.f : 2.f;
        return static_cast<float>(cells().mV36FarCasterMinimumPixels);
    }
}''',
)

replace_exact(
    "components/sceneutil/shadow.cpp",
    '''#include <components/settings/categories/cells.hpp>
#include <components/settings/categories/shadows.hpp>''',
    '''#include <components/settings/categories/cells.hpp>
#include <components/settings/categories/shadows.hpp>
#include <components/settings/v36profile.hpp>''',
)
replace_exact(
    "components/sceneutil/shadow.cpp",
    '''        mShadowTechnique->setV36FarCasterMinimumPixels(Settings::cells().mV36FarCasterMinimumPixels);''',
    '''        mShadowTechnique->setV36FarCasterMinimumPixels(Settings::V36Profile::farCasterMinimumPixels());''',
)

replace_exact(
    "files/settings-default.cfg",
    '''v3.6 async gpu profiler = false
v3.6 far caster minimum pixels = 0.0''',
    '''v3.6 async gpu profiler = false
v3.6 far caster minimum pixels = 0.0

# V3.7 proven-profile extension. Two-pixel far-cascade projected-size pruning is
# enabled by the normal performance profile; this switch turns only that feature off.
v3.7 disable far caster pruning = false

# Semantics-preserving bulk OnActive dispatch experiment. Default off until A/B tested.
v3.7 active event fast path = false''',
)

# -----------------------------------------------------------------------------
# Lua engine-event fast path. V3.6 showed thousands of OnActive events with no
# duplicates. Preserve order and exact callback count; only avoid empty global
# handler calls, repeated wrapper construction, and actor/item classification when
# no corresponding global handler exists. Local setActive() semantics are untouched.
# -----------------------------------------------------------------------------
replace_exact(
    "apps/openmw/mwlua/globalscripts.hpp",
    '''        void objectActive(const GObject& obj) { callEngineHandlers(mObjectActiveHandlers, obj); }
        void actorActive(const GObject& obj) { callEngineHandlers(mActorActiveHandlers, obj); }
        void itemActive(const GObject& obj) { callEngineHandlers(mItemActiveHandlers, obj); }
        void playerAdded(const GObject& obj) { callEngineHandlers(mPlayerAddedHandlers, obj); }''',
    '''        void objectActive(const GObject& obj) { callEngineHandlers(mObjectActiveHandlers, obj); }
        void actorActive(const GObject& obj) { callEngineHandlers(mActorActiveHandlers, obj); }
        void itemActive(const GObject& obj) { callEngineHandlers(mItemActiveHandlers, obj); }
        void playerAdded(const GObject& obj) { callEngineHandlers(mPlayerAddedHandlers, obj); }

        bool hasObjectActiveHandlers() const { return !mObjectActiveHandlers.mList.empty(); }
        bool hasActorActiveHandlers() const { return !mActorActiveHandlers.mList.empty(); }
        bool hasItemActiveHandlers() const { return !mItemActiveHandlers.mList.empty(); }
        bool hasPlayerAddedHandlers() const { return !mPlayerAddedHandlers.mList.empty(); }''',
)

replace_exact(
    "apps/openmw/mwlua/engineevents.cpp",
    '''        void operator()(const OnActive& event) const
        {
            MWWorld::Ptr ptr = getPtr(event.mObject);
            if (ptr.isEmpty())
                return;
            if (ptr.getCellRef().getRefId() == "player")
                mGlobalScripts.playerAdded(GObject(ptr));
            else
            {
                mGlobalScripts.objectActive(GObject(ptr));
                const MWWorld::Class& objClass = ptr.getClass();
                if (objClass.isActor())
                    mGlobalScripts.actorActive(GObject(ptr));
                if (objClass.isItem(ptr))
                    mGlobalScripts.itemActive(GObject(ptr));
            }
            if (auto* scripts = getLocalScripts(ptr))
                scripts->setActive(true);
        }''',
    '''        void operator()(const OnActive& event) const
        {
            MWWorld::Ptr ptr = getPtr(event.mObject);
            if (ptr.isEmpty())
                return;

            if (static_cast<bool>(Settings::cells().mV37ActiveEventFastPath))
            {
                if (ptr.getCellRef().getRefId() == "player")
                {
                    if (mGlobalScripts.hasPlayerAddedHandlers())
                    {
                        const GObject object(ptr);
                        mGlobalScripts.playerAdded(object);
                    }
                }
                else
                {
                    const bool objectHandlers = mGlobalScripts.hasObjectActiveHandlers();
                    const bool actorHandlers = mGlobalScripts.hasActorActiveHandlers();
                    const bool itemHandlers = mGlobalScripts.hasItemActiveHandlers();
                    if (objectHandlers || actorHandlers || itemHandlers)
                    {
                        const GObject object(ptr);
                        if (objectHandlers)
                            mGlobalScripts.objectActive(object);
                        if (actorHandlers || itemHandlers)
                        {
                            const MWWorld::Class& objClass = ptr.getClass();
                            if (actorHandlers && objClass.isActor())
                                mGlobalScripts.actorActive(object);
                            if (itemHandlers && objClass.isItem(ptr))
                                mGlobalScripts.itemActive(object);
                        }
                    }
                }
            }
            else if (ptr.getCellRef().getRefId() == "player")
                mGlobalScripts.playerAdded(GObject(ptr));
            else
            {
                mGlobalScripts.objectActive(GObject(ptr));
                const MWWorld::Class& objClass = ptr.getClass();
                if (objClass.isActor())
                    mGlobalScripts.actorActive(GObject(ptr));
                if (objClass.isItem(ptr))
                    mGlobalScripts.itemActive(GObject(ptr));
            }

            // Do not skip or defer local activation: this can materialize a previously
            // unloaded container and executing onActive at the original semantic point
            // is required for compatibility.
            if (auto* scripts = getLocalScripts(ptr))
                scripts->setActive(true);
        }''',
)

# -----------------------------------------------------------------------------
# Repair V3.6 static-batching topology measurement. OpenMW/OSG can expose
# drawables directly to NodeVisitor; Geode-only accounting produced zeros in the
# first V3.6 runtime dataset. This is telemetry-only and does not change paging.
# -----------------------------------------------------------------------------
replace_exact(
    "components/debug/v36structuretrace.hpp",
    '''#include <osg/Geode>
#include <osg/Geometry>''',
    '''#include <osg/Drawable>
#include <osg/Geometry>''',
)
replace_exact(
    "components/debug/v36structuretrace.hpp",
    '''        void apply(osg::Geode& geode) override
        {
            for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
            {
                ++mStats.mDrawables;
                if (const osg::Geometry* geometry = geode.getDrawable(i)->asGeometry())
                    if (const osg::Array* vertices = geometry->getVertexArray())
                        mStats.mVertices += vertices->getNumElements();
            }
            traverse(geode);
        }''',
    '''        void apply(osg::Drawable& drawable) override
        {
            ++mStats.mDrawables;
            if (const osg::Geometry* geometry = drawable.asGeometry())
                if (const osg::Array* vertices = geometry->getVertexArray())
                    mStats.mVertices += vertices->getNumElements();
        }''',
)

print("V3.7 core profile, Lua active-event fast path, and batching-metric repair completed successfully.")
