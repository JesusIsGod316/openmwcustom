from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one guarded match, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# Thread the evaluated-effect draw list through the immutable semantic frame.
replace_once(
    "components/rendercore/framerenderstate.hpp",
    '#include "records.hpp"\n',
    '#include "records.hpp"\n#include "effectframe.hpp"\n',
)
replace_once(
    "components/rendercore/framerenderstate.hpp",
    """        std::vector<MorphWeightState> morphWeights;\n        std::vector<DynamicMaterialState> dynamicMaterials;\n""",
    """        std::vector<MorphWeightState> morphWeights;\n        std::vector<DynamicMaterialState> dynamicMaterials;\n        std::vector<ImmediateEffectDraw> immediateEffectDraws;\n""",
)
replace_once(
    "components/rendercore/framerenderstate.hpp",
    """        [[nodiscard]] const std::vector<DynamicMaterialState>& dynamicMaterials() const noexcept\n        {\n            return mDesc.dynamicMaterials;\n        }\n\n        [[nodiscard]] bool valid() const noexcept\n""",
    """        [[nodiscard]] const std::vector<DynamicMaterialState>& dynamicMaterials() const noexcept\n        {\n            return mDesc.dynamicMaterials;\n        }\n        [[nodiscard]] const std::vector<ImmediateEffectDraw>& immediateEffectDraws() const noexcept\n        {\n            return mDesc.immediateEffectDraws;\n        }\n\n        [[nodiscard]] bool valid() const noexcept\n""",
)
replace_once(
    "components/rendercore/framerenderstate.hpp",
    """            }\n            return true;\n        }\n\n    private:\n""",
    """            }\n\n            for (std::size_t i = 0; i < mDesc.immediateEffectDraws.size(); ++i)\n            {\n                if (!validImmediateEffectDraw(mDesc.immediateEffectDraws[i]))\n                    return false;\n                for (std::size_t j = i + 1; j < mDesc.immediateEffectDraws.size(); ++j)\n                {\n                    if (mDesc.immediateEffectDraws[i].identity == mDesc.immediateEffectDraws[j].identity)\n                        return false;\n                }\n            }\n            return true;\n        }\n\n    private:\n""",
)

replace_once(
    "components/rendercore/frameproducer.hpp",
    """        std::vector<MorphWeightInput> morphWeights;\n        bool invalidateHistory = false;\n""",
    """        std::vector<MorphWeightInput> morphWeights;\n        std::vector<ImmediateEffectDraw> immediateEffectDraws;\n        bool invalidateHistory = false;\n""",
)
replace_once(
    "components/rendercore/frameproducer.hpp",
    """            desc.historyValid = continuous;\n            desc.environment = input.environment;\n            desc.renderTargets.push_back(RenderTargetDesc{\n""",
    """            desc.historyValid = continuous;\n            desc.environment = input.environment;\n            desc.immediateEffectDraws = input.immediateEffectDraws;\n            desc.renderTargets.push_back(RenderTargetDesc{\n""",
)

replace_once(
    "apps/openmw/mwrender/v4engineframesource.hpp",
    """        std::vector<RenderCore::MorphWeightInput> morphWeights;\n        bool invalidateHistory = false;\n""",
    """        std::vector<RenderCore::MorphWeightInput> morphWeights;\n        std::vector<RenderCore::ImmediateEffectDraw> immediateEffectDraws;\n        bool invalidateHistory = false;\n""",
)
replace_once(
    "apps/openmw/mwrender/v4enginerenderbridge.cpp",
    """        input.skeletonPoses = source.skeletonPoses;\n        input.morphWeights = source.morphWeights;\n        input.invalidateHistory = source.invalidateHistory || mGuiOnlyFramePresented;\n""",
    """        input.skeletonPoses = source.skeletonPoses;\n        input.morphWeights = source.morphWeights;\n        input.immediateEffectDraws = source.immediateEffectDraws;\n        input.invalidateHistory = source.invalidateHistory || mGuiOnlyFramePresented;\n""",
)

for path in [
    "components/rendercore/framerenderstate.hpp",
    "components/rendercore/frameproducer.hpp",
    "apps/openmw/mwrender/v4engineframesource.hpp",
    "apps/openmw/mwrender/v4enginerenderbridge.cpp",
]:
    text = Path(path).read_text(encoding="utf-8")
    if "immediateEffectDraw" not in text:
        raise RuntimeError(f"{path}: evaluated effect frame wiring missing")

print("CP4F evaluated-effect frame wiring applied")
