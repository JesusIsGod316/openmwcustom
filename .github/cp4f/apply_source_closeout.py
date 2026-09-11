from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one guarded match, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# Realize frame-local evaluated effect draws through the same legacy VSG
# compatibility path used for persistent NIF models.
replace_once(
    "components/render/backend/vsg/vsgruntimehost.cpp",
    '#include "dynamicactorplan.hpp"\n',
    '#include "dynamicactorplan.hpp"\n#include "immediateeffectrealizer.hpp"\n',
)

replace_once(
    "components/render/backend/vsg/vsgruntimehost.cpp",
    """            nextRoot->addChild(maskedNode(placementMask(castsShadow, actor.semanticFlags), std::move(placed)));\n        }\n        if (!compileForViewer(*mViewer, nextRoot))\n""",
    """            nextRoot->addChild(maskedNode(placementMask(castsShadow, actor.semanticFlags), std::move(placed)));\n        }\n\n        for (const RenderCore::ImmediateEffectDraw& effect : frame.immediateEffectDraws())\n        {\n            ImmediateEffectRealization realized\n                = realizeImmediateEffectDraw(effect, mTextureResolver, mSharedObjects);\n            if (!realized.valid())\n            {\n                mLastDiagnostic = realized.diagnostic.empty()\n                    ? \"evaluated gameplay effect could not be realized by the VSG compatibility path\"\n                    : realized.diagnostic;\n                return false;\n            }\n            auto placed = vsg::MatrixTransform::create(toVsgMatrix(effect.worldTransform));\n            placed->addChild(realized.root);\n            const bool castsShadow\n                = hasSemanticFlag(effect.semanticFlags, RenderCore::InstanceSemanticFlag::ShadowCaster);\n            nextRoot->addChild(maskedNode(placementMask(castsShadow, effect.semanticFlags), std::move(placed)));\n        }\n        if (!compileForViewer(*mViewer, nextRoot))\n""",
)

text = Path("components/render/backend/vsg/vsgruntimehost.cpp").read_text(encoding="utf-8")
if '#include "immediateeffectrealizer.hpp"' not in text:
    raise RuntimeError("evaluated effect realizer include missing")
if "frame.immediateEffectDraws()" not in text:
    raise RuntimeError("evaluated effect frame loop missing")

print("CP4F evaluated-effect backend wiring applied")
