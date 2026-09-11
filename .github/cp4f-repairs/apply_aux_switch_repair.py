from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one guarded match, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


header = "components/render/backend/vsg/vsgruntimehost.hpp"
source = "components/render/backend/vsg/vsgruntimehost.cpp"

# VSG 1.1.15 RenderGraph has no traversal mask. Keep a stable Switch in the
# command graph so inactive persistent auxiliary targets skip the whole render
# pass instead of merely hiding the View inside an otherwise-recorded pass.
replace_once(
    header,
    """            std::optional<RenderCore::RenderTargetFormat> depthFormat = RenderCore::RenderTargetFormat::Depth32Float;\n            OffscreenRenderTarget target;\n            FrameCameraObjects camera;\n""",
    """            std::optional<RenderCore::RenderTargetFormat> depthFormat = RenderCore::RenderTargetFormat::Depth32Float;\n            OffscreenRenderTarget target;\n            vsg::ref_ptr<vsg::Switch> commandVisibility;\n            FrameCameraObjects camera;\n""",
)

replace_once(
    source,
    """        if (!mCommandGraph || !found->target.renderGraph)\n        {\n            mLastDiagnostic = \"persistent auxiliary target retirement found an invalid command graph\";\n            return false;\n        }\n\n        waitIdle();\n        const auto graph = std::find(mCommandGraph->children.begin(), mCommandGraph->children.end(), found->target.renderGraph);\n""",
    """        if (!mCommandGraph || !found->target.renderGraph || !found->commandVisibility)\n        {\n            mLastDiagnostic = \"persistent auxiliary target retirement found an invalid command graph\";\n            return false;\n        }\n\n        waitIdle();\n        const auto graph = std::find(\n            mCommandGraph->children.begin(), mCommandGraph->children.end(), found->commandVisibility);\n""",
)

replace_once(
    source,
    """        for (AuxiliaryViewRuntime& runtime : mAuxiliaryViews)\n        {\n            runtime.active = false;\n            if (runtime.target.renderGraph)\n                runtime.target.renderGraph->mask = vsg::MASK_OFF;\n            if (runtime.view)\n                runtime.view->mask = vsg::MASK_OFF;\n        }\n""",
    """        for (AuxiliaryViewRuntime& runtime : mAuxiliaryViews)\n        {\n            runtime.active = false;\n            if (runtime.commandVisibility)\n                runtime.commandVisibility->setAllChildren(false);\n            if (runtime.view)\n                runtime.view->mask = vsg::MASK_OFF;\n        }\n""",
)

replace_once(
    source,
    """                if (!created.target)\n                {\n                    mLastDiagnostic = \"Vulkan auxiliary offscreen target allocation failed\";\n                    return false;\n                }\n                created.camera = FrameCameraObjects::create(view);\n""",
    """                if (!created.target)\n                {\n                    mLastDiagnostic = \"Vulkan auxiliary offscreen target allocation failed\";\n                    return false;\n                }\n                created.commandVisibility = vsg::Switch::create();\n                if (!created.commandVisibility)\n                {\n                    mLastDiagnostic = \"Vulkan auxiliary view could not create its command visibility switch\";\n                    return false;\n                }\n                created.commandVisibility->addChild(false, created.target.renderGraph);\n                created.camera = FrameCameraObjects::create(view);\n""",
)

replace_once(
    source,
    """                mCommandGraph->children.insert(mCommandGraph->children.end() - 1, created.target.renderGraph);\n                mAuxiliaryViews.push_back(std::move(created));\n""",
    """                mCommandGraph->children.insert(mCommandGraph->children.end() - 1, created.commandVisibility);\n                mAuxiliaryViews.push_back(std::move(created));\n""",
)

replace_once(
    source,
    """            runtime->active = true;\n            runtime->target.renderGraph->mask = vsg::MASK_ALL;\n            runtime->view->mask = vsg::MASK_ALL;\n""",
    """            if (!runtime->commandVisibility)\n            {\n                mLastDiagnostic = \"persistent auxiliary view lost its command visibility switch\";\n                return false;\n            }\n            runtime->active = true;\n            runtime->commandVisibility->setAllChildren(true);\n            runtime->view->mask = vsg::MASK_ALL;\n""",
)

header_text = Path(header).read_text(encoding="utf-8")
source_text = Path(source).read_text(encoding="utf-8")
for needle in [
    "vsg::ref_ptr<vsg::Switch> commandVisibility;",
    "runtime.commandVisibility->setAllChildren(false);",
    "created.commandVisibility->addChild(false, created.target.renderGraph);",
    "mCommandGraph->children.end() - 1, created.commandVisibility",
    "runtime->commandVisibility->setAllChildren(true);",
]:
    if needle not in header_text + source_text:
        raise RuntimeError(f"auxiliary switch repair missing {needle!r}")
if "renderGraph->mask" in source_text:
    raise RuntimeError("unsupported RenderGraph::mask usage remains after repair")

print("CP4F pinned-VSG auxiliary switch repair applied")
