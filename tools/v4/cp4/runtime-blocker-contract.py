#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
actor = (root / 'components/nifrender/actormodelcomposer.hpp').read_text()
state_hpp = (root / 'components/lua/luastate.hpp').read_text()
state_cpp = (root / 'components/lua/luastate.cpp').read_text()
container = (root / 'components/lua/scriptscontainer.cpp').read_text()
lua_manager = (root / 'apps/openmw/mwlua/luamanagerimp.cpp').read_text()
lua_worker_cpp = (root / 'apps/openmw/mwlua/worker.cpp').read_text()
engine = (root / 'apps/openmw/engine.cpp').read_text()
bridge = (root / 'apps/openmw/mwrender/v4enginerenderbridge.cpp').read_text()
translator = (root / 'components/nifrender/niftranslator.cpp').read_text()
material_pass = (root / 'components/nifrender/materialpass.hpp').read_text()
dynamic_actor = (root / 'components/render/backend/vsg/dynamicactorplan.hpp').read_text()
osg_loader = (root / 'components/nifosg/nifloader.cpp').read_text()
debugging = (root / 'components/debug/debugging.cpp').read_text()

required = {
    'forced actor skeleton helper': 'buildForcedActorSkeleton' in actor,
    'forced actor skeleton excludes cleaned geometry containers':
        'source.name.empty() || source.mesh' in actor
        and 'CleanObjectRootVisitor' in actor
        and 'startsFolded(source.name, "tri ")' in actor,
    'first-match duplicate actor bones': 'if (names.find(folded) != names.end())\n                continue;' in actor,
    'canonical structural actor root exclusion': 'const bool canonicalGroupRoot = !source.parent.valid()' in actor,
    'direct sandbox ScriptId parameter': 'ScriptId scriptId = {}' in state_hpp,
    'direct container ScriptId handoff': 'ScriptId{ this, scriptId }' in container,
    'runtime forced skeleton publication': 'runtime:forced-actor-skeleton:' in bridge,
    'canonical support-root classification': 'TranslationDisposition::Ignored' in translator,
    'actor-part asset diagnostic': 'NPC part \'" + std::string(part.model.value()) + "\' failed:' in bridge,
    'actor-part translation diagnostic': 'diagnostic.code' in bridge,
    'actor morph authoritative mesh identity': 'candidate.mesh == *node.mesh' in bridge,
    'actor morph per-part topology check': 'evaluated morph topology does not match its published model' in bridge,
    'direct drawable morph discovery': 'void apply(osg::Drawable& drawable) override' in bridge
        and 'dynamic_cast<SceneUtil::MorphGeometry*>(&drawable)' in bridge,
    'actor morph CopyRig selection parity': 'if (hasSkinnedGeometry)\n                        return;' in bridge,
    'Vulkan package prototype quarantine':
        'mLua.setPackagePrototypeReuse(requestedPackagePrototypeReuse && !vulkanBackend)' in lua_manager,
    'Vulkan immutable frame capture before Lua release':
        engine.index('prepareVulkanFrame(frametime, false);')
        < engine.index('mLuaWorker->allowUpdate(frameStart, frameNumber, *stats);')
        < engine.index('presentPreparedVulkanFrame();'),
    'threaded Lua worker available to both renderers':
        'if (Settings::lua().mLuaNumThreads > 0)\n            mThread = std::thread([this] { run(); });' in lua_worker_cpp,
    'transition callbacks present immutable GUI-only frames':
        'presentCallback = [this] { presentVulkanGuiFrame(); };' in engine
        and 'mV4RenderBridge->renderGuiFrame(simulationTime, 0.0)' in engine,
    'automation-safe fatal diagnostic path': 'OPENMW_SUPPRESS_FATAL_DIALOG' in debugging
        and 'if (!suppressFatalDialog)' in debugging,
    'canonical OSG root filter': 'dynamic_cast<const Nif::NiAVObject*>' in osg_loader,
    'split geometry receives source material on every expansion':
        'std::vector<TranslatedModelNode*>' in material_pass
        and 'std::size_t& next = mNextGeometryNode[found->first];' in material_pass
        and '*found->second[next++]' in material_pass,
    'rigid actor controllers retain model-local transforms':
        'non-matching\n        // names retain their authored local transform' in dynamic_actor,
}
for label, ok in required.items():
    if not ok:
        raise SystemExit(f'FAIL: missing {label}')

forbidden = {
    'V4-only duplicate-bone rejection': 'ambiguous case-insensitive bone name' in actor,
    'unsafe ScriptId Lua userdata recovery': '.get<sol::optional<ScriptId>>(ScriptsContainer::sScriptIdKey)' in state_cpp,
    'speculative new-game player-cell fallback': 'New-game transitions can establish the authoritative player cell' in engine,
    'old actor canonical-skeleton hard reject': 'active actor source model has no publishable canonical skeleton' in bridge,
    'support-root translation rejection': 'root.not_avobject' in translator,
    'opaque actor-part failure': 'NPC part is missing from the winning VFS or failed translation' in bridge,
    'invalid actor morph count equality': 'collector.morphs.size() != morphNodes.size()' in bridge,
    'cross-part actor morph name matching': 'evaluated actor morph nodes do not match translated node' in bridge,
    'Vulkan threaded Lua worker quarantine': 'V4 Vulkan: bypassing the threaded Lua worker' in lua_worker_cpp,
    'rigid attachment controller skeleton rejection': 'animated actor node is absent from its skeleton' in dynamic_actor,
}
for label, present in forbidden.items():
    if present:
        raise SystemExit(f'FAIL: {label} remains')

print('CP4F runtime blocker contract: PASS')
