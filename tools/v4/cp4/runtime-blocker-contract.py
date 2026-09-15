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
runtime_host = (root / 'components/render/backend/vsg/vsgruntimehost.cpp').read_text()
runtime_host_hpp = (root / 'components/render/backend/vsg/vsgruntimehost.hpp').read_text()
submission = (root / 'components/render/backend/vsg/vsgsubmission.hpp').read_text()
camera = (root / 'apps/openmw/mwrender/camera.cpp').read_text()
camera_hpp = (root / 'apps/openmw/mwrender/camera.hpp').read_text()
semantic_source = (root / 'apps/openmw/mwrender/v4semanticsource.cpp').read_text()
enchanted_glow = (root / 'components/nifrender/enchantedglow.hpp').read_text()
slot_table = (root / 'components/rendercore/slottable.hpp').read_text()
runtime_qc = (root / 'tools/v4/cp4/run-runtime-qc.ps1').read_text()

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
    'late auxiliary framebuffer view registered before compilation':
        'viewer.compileManager->add(framebuffer, view);' in submission
        and 'compileForNewFramebufferView' in runtime_host,
    'late auxiliary pipeline realization validated before publication':
        'graphicsPipelinesRealizedForView' in runtime_host
        and 'validated_vk(mViewId)' in submission,
    'active view pipeline census precedes Vulkan submission':
        runtime_host.index('ensureActiveGraphicsPipelinesRealized()')
        < runtime_host.index('submitAndPresentChecked(*mViewer)'),
    'pipeline census reports every unresolved active pipeline':
        'for (const std::string& issue : unresolved)' in runtime_host
        and 'mAudit.unrealized.push_back' in submission,
    'pipeline census attempts exact-view repair before failing':
        'compileForViewerView(*mViewer, *active.view, active.view)' in runtime_host,
    'pipeline census includes hidden shadow pre-render view':
        'mOpenMwViewState->shadowMaps.front()' in runtime_host
        and 'views.push_back({ "shadow", shadow.view })' in runtime_host,
    'strict QC captures active view identities and frame transform evidence':
        'OPENMW_V4_STRICT_QC' in runtime_host
        and 'V4 strict QC active views=' in runtime_host
        and 'cameraFromView=' in runtime_host
        and 'viewOrthoError=' in runtime_host
        and 'frame.dynamicTransforms().size()' in runtime_host,
    'strict QC reports exact-view pipeline repair':
        'V4 strict QC repairing ' in runtime_host
        and 'active.view->viewID' in runtime_host,
    'single-command runtime QC preserves diagnostic evidence':
        'OPENMW_V4_STRICT_QC' in runtime_qc
        and 'runtime-qc-summary.txt' in runtime_qc
        and 'openmw-crash.dmp' in runtime_qc
        and 'Unexpected destruction of LuaWorker' in runtime_qc,
    'interactive runtime QC permits normal input capture':
        '[switch] $NoGrab' in runtime_qc
        and "if ($NoGrab)" in runtime_qc,
    'runtime QC preserves configured log and dump directory':
        '[string] $LogDirectory' in runtime_qc
        and "$log = Join-Path $LogDirectory 'openmw.log'" in runtime_qc,
    'Vulkan GUI snapshot precedes threaded Lua release':
        'mBridge.prepareGuiFrame()' in (root / 'apps/openmw/mwrender/v4engineframecoordinator.cpp').read_text()
        and 'prepareGuiFrame(frametime' not in engine
        and 'bool VsgRuntimeHost::prepareGui()' in runtime_host
        and 'mGuiPreparedRoot' in runtime_host_hpp
        and 'if (!mGuiPrepared && !prepareGui())' in runtime_host,
    'evaluated object capture visits direct drawable nodes':
        'void apply(osg::Drawable& drawable) override' in bridge
        and 'V4 strict QC evaluated object produced no draws' in bridge
        and 'void apply(osg::Drawable& drawable) override' in (root / 'apps/openmw/mwrender/v4effectcapture.hpp').read_text(),
    'strict QC reports auxiliary camera and readback content':
        'V4 strict QC auxiliary kind=' in runtime_host
        and 'staticChildren=' in runtime_host
        and 'V4 strict QC auxiliary readback target=' in (root / 'components/render/backend/vsg/auxiliaryreadback.cpp').read_text(),
    'Vulkan camera uses current controller state without OSG render traversal':
        'calculateViewMatrix() const' in camera_hpp
        and 'osg::Matrixf Camera::calculateViewMatrix() const' in camera
        and 'camera.calculateViewMatrix()' in semantic_source
        and 'camera.getViewMatrix()' not in semantic_source,
    'enchanted model snapshots precede same-family reservation':
        enchanted_glow.index('const ModelRecord sourceRecord = *source;')
        < enchanted_glow.index('world.reserveModel()'),
    'enchanted materials snapshot before replacement reservation':
        enchanted_glow.index('MaterialRecord record = *baseMaterial;')
        < enchanted_glow.index('world.reserveMaterial()'),
    'slot-table borrowed pointer invalidation documented':
        'Producers that reserve another handle of the same family must first' in slot_table,
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
    'late auxiliary graph compiled only against stale viewer contexts':
        'compileForViewer(*mViewer, created.target.renderGraph)' in runtime_host,
}
for label, present in forbidden.items():
    if present:
        raise SystemExit(f'FAIL: {label} remains')

print('CP4F runtime blocker contract: PASS')
