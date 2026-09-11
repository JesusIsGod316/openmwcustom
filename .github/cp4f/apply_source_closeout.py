from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one guarded match, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# Finish the evaluated-effect state audit before the aggregate Windows gate.
# Lossless legacy state is copied; state without a CP4F neutral/runtime facet
# fails explicitly instead of being silently discarded.
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    "#include <osg/Geometry>\n",
    "#include <osg/Geometry>\n#include <osg/FrontFace>\n",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    "#include <osg/PolygonMode>\n",
    "#include <osg/PolygonMode>\n#include <osg/Stencil>\n",
)

replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """        [[nodiscard]] inline std::optional<RenderCore::BlendEquation> blendEquation(GLenum value) noexcept\n        {\n            using RenderCore::BlendEquation;\n            switch (value)\n            {\n                case GL_FUNC_ADD: return BlendEquation::Add;\n                case GL_FUNC_SUBTRACT: return BlendEquation::Subtract;\n                case GL_FUNC_REVERSE_SUBTRACT: return BlendEquation::ReverseSubtract;\n                case GL_MIN: return BlendEquation::Minimum;\n                case GL_MAX: return BlendEquation::Maximum;\n                default: return std::nullopt;\n            }\n        }\n\n""",
    """        [[nodiscard]] inline std::optional<RenderCore::BlendEquation> blendEquation(GLenum value) noexcept\n        {\n            using RenderCore::BlendEquation;\n            switch (value)\n            {\n                case GL_FUNC_ADD: return BlendEquation::Add;\n                case GL_FUNC_SUBTRACT: return BlendEquation::Subtract;\n                case GL_FUNC_REVERSE_SUBTRACT: return BlendEquation::ReverseSubtract;\n                case GL_MIN: return BlendEquation::Minimum;\n                case GL_MAX: return BlendEquation::Maximum;\n                default: return std::nullopt;\n            }\n        }\n\n        [[nodiscard]] inline std::optional<RenderCore::StencilOp> stencilOp(GLenum value) noexcept\n        {\n            using RenderCore::StencilOp;\n            switch (value)\n            {\n                case GL_KEEP: return StencilOp::Keep;\n                case GL_ZERO: return StencilOp::Zero;\n                case GL_REPLACE: return StencilOp::Replace;\n                case GL_INCR: return StencilOp::Increment;\n                case GL_DECR: return StencilOp::Decrement;\n                case GL_INVERT: return StencilOp::Invert;\n                default: return std::nullopt;\n            }\n        }\n\n""",
)

replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """            MaterialRecord material;\n            material.textureApply = textureApplyMode(path);\n            material.unlit = noLightingShader(path);\n\n""",
    """            MaterialRecord material;\n            material.textureApply = textureApplyMode(path);\n            material.unlit = noLightingShader(path);\n\n            // Soft effects require the opaque-depth texture sampled by the OSG\n            // shader visitor. The immediate-effect Vulkan path does not expose\n            // that sampled attachment yet, so reject this optional (default-off)\n            // setting rather than drawing hard intersections silently.\n            if (state->getUniform(\"particleSize\") || state->getUniform(\"particleFade\")\n                || state->getUniform(\"softFalloffDepth\"))\n            {\n                diagnostic = \"evaluated effect requires soft-particle opaque-depth sampling\";\n                return false;\n            }\n            if (state->getUniform(\"distortionStrength\") || state->getBinName() == \"Distortion\")\n            {\n                diagnostic = \"evaluated effect requires the post-process distortion target\";\n                return false;\n            }\n\n""",
)

replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """            material.cullMode = CullMode::Back;\n            if (!stateEnabled(*state, GL_CULL_FACE, true))\n""",
    """            if (const auto* front = dynamic_cast<const osg::FrontFace*>(\n                    state->getAttribute(osg::StateAttribute::FRONTFACE)))\n            {\n                material.frontFace = front->getMode() == osg::FrontFace::CLOCKWISE\n                    ? FrontFaceWinding::Clockwise\n                    : FrontFaceWinding::CounterClockwise;\n            }\n\n            material.cullMode = CullMode::Back;\n            if (!stateEnabled(*state, GL_CULL_FACE, true))\n""",
)

replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """            material.depthTest = stateEnabled(*state, GL_DEPTH_TEST, true);\n            if (const auto* depth = dynamic_cast<const osg::Depth*>(\n""",
    """            material.stencil.enabled = stateEnabled(*state, GL_STENCIL_TEST, false);\n            if (material.stencil.enabled)\n            {\n                if (const auto* stencil = dynamic_cast<const osg::Stencil*>(\n                        state->getAttribute(osg::StateAttribute::STENCIL)))\n                {\n                    const auto compare = compareOp(static_cast<GLenum>(stencil->getFunction()));\n                    const auto fail = stencilOp(static_cast<GLenum>(stencil->getStencilFailOperation()));\n                    const auto depthFail\n                        = stencilOp(static_cast<GLenum>(stencil->getStencilPassAndDepthFailOperation()));\n                    const auto pass = stencilOp(static_cast<GLenum>(stencil->getStencilPassAndDepthPassOperation()));\n                    if (!compare || !fail || !depthFail || !pass || stencil->getFunctionRef() < 0\n                        || stencil->getWriteMask() != ~0u)\n                    {\n                        diagnostic = \"evaluated effect uses unsupported stencil state\";\n                        return false;\n                    }\n                    material.stencil.compare = *compare;\n                    material.stencil.reference = static_cast<std::uint32_t>(stencil->getFunctionRef());\n                    material.stencil.compareMask = stencil->getFunctionMask();\n                    material.stencil.fail = *fail;\n                    material.stencil.depthFail = *depthFail;\n                    material.stencil.pass = *pass;\n                }\n            }\n\n            material.depthTest = stateEnabled(*state, GL_DEPTH_TEST, true);\n            if (const auto* depth = dynamic_cast<const osg::Depth*>(\n""",
)

replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """            if (const auto* polygon = dynamic_cast<const osg::PolygonMode*>(\n                    state->getAttribute(osg::StateAttribute::POLYGONMODE)))\n""",
    """            if (stateEnabled(*state, GL_POLYGON_OFFSET_FILL, false)\n                || stateEnabled(*state, GL_POLYGON_OFFSET_LINE, false)\n                || stateEnabled(*state, GL_POLYGON_OFFSET_POINT, false))\n            {\n                diagnostic = \"evaluated effect requires authored polygon-offset realization\";\n                return false;\n            }\n\n            if (const auto* polygon = dynamic_cast<const osg::PolygonMode*>(\n                    state->getAttribute(osg::StateAttribute::POLYGONMODE)))\n""",
)

# OSG permits OVERALL-bound normal/color arrays with one entry. Replicate that
# constant value so the neutral mesh has the per-vertex streams expected by the
# already-tested legacy material realizer.
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """            if (const auto* normals = dynamic_cast<const osg::Vec3Array*>(geometry.getNormalArray()))\n            {\n                if (normals->size() == positions->size())\n                {\n                    draw.mesh.normals.reserve(normals->size());\n                    for (const osg::Vec3f& normal : *normals)\n                        draw.mesh.normals.push_back(toGlm(normal));\n                }\n            }\n\n""",
    """            if (const auto* normals = dynamic_cast<const osg::Vec3Array*>(geometry.getNormalArray()))\n            {\n                if (normals->size() == positions->size())\n                {\n                    draw.mesh.normals.reserve(normals->size());\n                    for (const osg::Vec3f& normal : *normals)\n                        draw.mesh.normals.push_back(toGlm(normal));\n                }\n                else if (normals->size() == 1u)\n                    draw.mesh.normals.assign(positions->size(), toGlm(normals->front()));\n                else if (!normals->empty())\n                {\n                    diagnostic = \"evaluated effect geometry uses a non-vertex normal binding\";\n                    return false;\n                }\n            }\n\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """            if (const auto* colors = dynamic_cast<const osg::Vec4Array*>(geometry.getColorArray()))\n            {\n                if (colors->size() == positions->size())\n                {\n                    draw.mesh.colors.reserve(colors->size());\n                    for (const osg::Vec4f& color : *colors)\n                        draw.mesh.colors.push_back(toGlm(color));\n                }\n            }\n            else if (const auto* colors = dynamic_cast<const osg::Vec4ubArray*>(geometry.getColorArray()))\n            {\n                if (colors->size() == positions->size())\n                {\n                    draw.mesh.colors.reserve(colors->size());\n                    for (const osg::Vec4ub& color : *colors)\n                        draw.mesh.colors.emplace_back(color.r() / 255.0f, color.g() / 255.0f,\n                            color.b() / 255.0f, color.a() / 255.0f);\n                }\n            }\n\n""",
    """            if (const auto* colors = dynamic_cast<const osg::Vec4Array*>(geometry.getColorArray()))\n            {\n                if (colors->size() == positions->size())\n                {\n                    draw.mesh.colors.reserve(colors->size());\n                    for (const osg::Vec4f& color : *colors)\n                        draw.mesh.colors.push_back(toGlm(color));\n                }\n                else if (colors->size() == 1u)\n                    draw.mesh.colors.assign(positions->size(), toGlm(colors->front()));\n                else if (!colors->empty())\n                {\n                    diagnostic = \"evaluated effect geometry uses a non-vertex color binding\";\n                    return false;\n                }\n            }\n            else if (const auto* colors = dynamic_cast<const osg::Vec4ubArray*>(geometry.getColorArray()))\n            {\n                const auto convert = [](const osg::Vec4ub& color) {\n                    return glm::vec4(color.r() / 255.0f, color.g() / 255.0f,\n                        color.b() / 255.0f, color.a() / 255.0f);\n                };\n                if (colors->size() == positions->size())\n                {\n                    draw.mesh.colors.reserve(colors->size());\n                    for (const osg::Vec4ub& color : *colors)\n                        draw.mesh.colors.push_back(convert(color));\n                }\n                else if (colors->size() == 1u)\n                    draw.mesh.colors.assign(positions->size(), convert(colors->front()));\n                else if (!colors->empty())\n                {\n                    diagnostic = \"evaluated effect geometry uses a non-vertex color binding\";\n                    return false;\n                }\n            }\n\n""",
)

capture = Path("apps/openmw/mwrender/v4effectcapture.hpp").read_text(encoding="utf-8")
for needle in [
    "soft-particle opaque-depth sampling",
    "post-process distortion target",
    "getStencilPassAndDepthPassOperation",
    "authored polygon-offset realization",
    "FrontFaceWinding::Clockwise",
    "normals->size() == 1u",
    "colors->size() == 1u",
]:
    if needle not in capture:
        raise RuntimeError(f"evaluated effect state hardening missing {needle!r}")

print("CP4F evaluated-effect state hardening applied")
