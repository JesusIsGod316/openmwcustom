#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_IMMEDIATEEFFECTREALIZER_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_IMMEDIATEEFFECTREALIZER_H

#include "staticassetconformance.hpp"
#include "immediateeffectcontract.hpp"

#include <components/rendercore/effectframe.hpp>
#include <components/rendercore/renderworld.hpp>

#include <memory>
#include <cstring>
#include <cstddef>
#include <type_traits>
#include <string>

namespace RenderVsg
{
    struct ImmediateEffectRealization
    {
        vsg::ref_ptr<vsg::Group> root;
        std::vector<StaticRealizationResult::MutableDrawStreams> mutableDraws;
        std::string diagnostic;

        [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(root); }
    };

    // Realize one low-volume, already-evaluated gameplay effect using the same
    // legacy material/texture path as persistent NIF resources. The temporary
    // RenderWorld exists only to provide typed handles while descriptors and VSG
    // objects are built; no transient gameplay state is published globally.
    [[nodiscard]] inline ImmediateEffectRealization realizeImmediateEffectDraw(
        const RenderCore::ImmediateEffectDraw& draw, const StaticTextureResolver& textureResolver,
        vsg::ref_ptr<vsg::SharedObjects> sharedObjects, bool dynamicData = true)
    {
        using namespace RenderCore;

        ImmediateEffectRealization result;
        if (!validImmediateEffectDraw(draw) || !textureResolver)
        {
            result.diagnostic = "evaluated effect draw failed neutral validation";
            return result;
        }

        RenderWorld world;
        MaterialRecord material = draw.material;
        material.sourceIdentity = draw.identity + ":material";
        material.textures.clear();
        material.textures.reserve(draw.textures.size());

        for (const EffectTextureSnapshot& snapshot : draw.textures)
        {
            const std::optional<TextureHandle> handle = world.reserveTexture();
            if (!handle)
            {
                result.diagnostic = "evaluated effect texture handle reservation failed";
                return result;
            }
            TextureRecord texture = snapshot.texture;
            texture.revision = InitialResourceRevision;
            if (!world.commit(*handle, std::move(texture)))
            {
                result.diagnostic = "evaluated effect texture publication failed";
                return result;
            }
            TextureBinding binding = snapshot.binding;
            binding.texture = *handle;
            material.textures.push_back(std::move(binding));
        }

        const std::optional<MaterialHandle> materialHandle = world.reserveMaterial();
        if (!materialHandle)
        {
            result.diagnostic = "evaluated effect material handle reservation failed";
            return result;
        }
        material.revision = InitialResourceRevision;
        if (!world.commit(*materialHandle, std::move(material)))
        {
            result.diagnostic = "evaluated effect material publication failed";
            return result;
        }

        const std::optional<MeshHandle> meshHandle = world.reserveMesh();
        if (!meshHandle)
        {
            result.diagnostic = "evaluated effect mesh handle reservation failed";
            return result;
        }
        MeshRecord mesh;
        mesh.revision = InitialResourceRevision;
        mesh.sourceIdentity = draw.identity + ":mesh";
        mesh.bounds = draw.bounds;
        mesh.surfaceCount = static_cast<std::uint32_t>(draw.meshData().surfaces.size());
        mesh.payload = std::make_shared<const MeshPayload>(draw.meshData());
        if (!world.commit(*meshHandle, std::move(mesh)))
        {
            result.diagnostic = "evaluated effect mesh publication failed";
            return result;
        }

        auto payload = std::make_shared<ModelPayload>();
        ModelNodeRecord geometry;
        geometry.name = draw.identity + ":geometry";
        geometry.kind = ModelNodeKind::Geometry;
        geometry.mesh = *meshHandle;
        geometry.materials = { *materialHandle };
        if (draw.billboard)
        {
            ModelNodeRecord billboard;
            billboard.name = draw.identity + ":billboard";
            billboard.kind = ModelNodeKind::Billboard;
            billboard.billboard = draw.billboard;
            payload->nodes.push_back(std::move(billboard));
            geometry.parent = ModelNodeIndex{ 0u };
            payload->nodes.push_back(std::move(geometry));
            payload->roots.emplace_back(0u);
        }
        else
        {
            payload->nodes.push_back(std::move(geometry));
            payload->roots.emplace_back(0u);
        }

        const std::optional<ModelHandle> modelHandle = world.reserveModel();
        if (!modelHandle)
        {
            result.diagnostic = "evaluated effect model handle reservation failed";
            return result;
        }
        ModelRecord model;
        model.revision = InitialResourceRevision;
        model.sourceIdentity = draw.identity + ":model";
        model.contentIdentity = draw.identity + ":frame";
        model.bounds = draw.bounds;
        model.dynamicRequirements = 0;
        model.payload = std::move(payload);
        if (!world.commit(*modelHandle, std::move(model)))
        {
            result.diagnostic = "evaluated effect model publication failed";
            return result;
        }

        const std::optional<StaticAssetPlan> plan = buildStaticAssetPlan(world, *modelHandle);
        if (!plan)
        {
            result.diagnostic = "evaluated effect could not produce a legacy draw plan";
            return result;
        }

        StaticRealizationResult realized = realizeStaticAssetConformant(
            world, *modelHandle, *plan, textureResolver, std::move(sharedObjects), {}, {}, {}, 1.0f, dynamicData);
        if (!realized.valid() || realized.stats.runtimeContextEffects != 0
            || realized.stats.unsupportedTextureBindings != 0)
        {
            result.diagnostic = realized.diagnostics.empty()
                ? "evaluated effect requires an unsupported legacy realization semantic"
                : realized.diagnostics.front();
            result.diagnostic += " [evaluated-draw='" + draw.identity + "']";
            result.diagnostic += " [source='" + draw.material.sourceIdentity + "', environment_mode="
                + std::to_string(static_cast<unsigned>(draw.material.environmentMapMode))
                + ", bump=" + std::to_string(draw.material.bumpParametersEnabled) + "]";
            // Failure evidence is independent of sampled probe budgets. Report
            // simultaneous causes instead of hiding everything after the first.
            for (std::size_t i=1;i<realized.diagnostics.size();++i)
                result.diagnostic += " [cause=" + realized.diagnostics[i] + "]";
            for (std::size_t i=0;i<std::min(draw.textures.size(),std::size_t{16});++i)
                result.diagnostic += " [texture='" + draw.textures[i].texture.sourceIdentity
                    + "', role=" + std::to_string(static_cast<unsigned>(draw.textures[i].binding.role))
                    + ", uv=" + std::to_string(draw.textures[i].binding.transform.uvSet) + "]";
            return result;
        }
        result.root = std::move(realized.root);
        result.mutableDraws = std::move(realized.mutableDraws);
        return result;
    }

    namespace effect_update_detail
    {
    template <class Source, class Array, class Value>
    void updatePackedStream(const std::vector<Source>& source, Array& destination, const Value& fallback) noexcept
    {
        const auto convert = [](const Source& value) {
            if constexpr (Value::size() == 2)
                return Value(value.x, value.y);
            else if constexpr (Value::size() == 3)
                return Value(value.x, value.y, value.z);
            else
                return Value(value.x, value.y, value.z, value.w);
        };
        // Reading object representations is safe, but VSG vectors have a
        // nontrivial copy constructor. Never memcpy into those objects. Only
        // compare bytes when both layouts consist solely of matching floats;
        // padded/aligned builds retain component-wise comparison/conversion.
        constexpr auto packed = []<class T>() {
            if constexpr (!std::is_standard_layout_v<T> || sizeof(T) != Value::size() * sizeof(float))
                return false;
            else
            {
                bool result = offsetof(T, x) == 0 && offsetof(T, y) == sizeof(float);
                if constexpr (Value::size() >= 3) result &= offsetof(T, z) == 2 * sizeof(float);
                if constexpr (Value::size() >= 4) result &= offsetof(T, w) == 3 * sizeof(float);
                return result;
            }
        };
        bool changed = false;
        if (source.empty())
        {
            changed = std::any_of(destination.data(), destination.data() + destination.size(),
                [&](const auto& value) { return value != fallback; });
            if (changed)
                std::fill_n(destination.data(), destination.size(), fallback);
        }
        else
        {
            if constexpr (packed.template operator()<Source>() && packed.template operator()<Value>())
                changed = std::memcmp(destination.data(), source.data(), source.size() * sizeof(Source)) != 0;
            else
                for (std::size_t i = 0; i < source.size() && !changed; ++i)
                    changed = destination[i] != convert(source[i]);
            if (changed)
                for (std::size_t i = 0; i < source.size(); ++i)
                    destination.set(i, convert(source[i]));
        }
        if (changed)
            destination.dirty();
    }

    // Internal stream writer: public entry points below establish validation.
    [[nodiscard]] inline bool update(const RenderCore::ImmediateEffectDraw& draw,
        std::vector<StaticRealizationResult::MutableDrawStreams>& mutableDraws, bool bulkStreams) noexcept
    {
        if (mutableDraws.size() != draw.meshData().surfaces.size())
            return false;
        // Validate the entire destination before changing any resident array.
        // A malformed later surface/UV stream must not leave earlier surfaces
        // half updated when the caller rejects this reuse attempt.
        for (const auto& streams : mutableDraws)
        {
            if (!streams.positions || !streams.normals || !streams.colors
                || streams.positions->size() != draw.meshData().positions.size()
                || streams.normals->size() != draw.meshData().positions.size()
                || streams.colors->size() != draw.meshData().positions.size()
                || streams.texCoords.size() != draw.meshData().texCoordSets.size())
                return false;
            for (std::size_t set = 0; set < streams.texCoords.size(); ++set)
            {
                if (!streams.texCoords[set]
                    || streams.texCoords[set]->size() != draw.meshData().texCoordSets[set].size())
                    return false;
            }
        }
        // Immediate effects have identity local model transforms; the caller
        // updates their outer world placement. Billboards rotate about their
        // origin, so use an origin-centred sphere as in billboardDrawBound.
        const bool needsBounds = !bulkStreams || std::any_of(mutableDraws.begin(), mutableDraws.end(),
            [](const auto& streams) { return static_cast<bool>(streams.sorted); });
        glm::dvec3 minimum(draw.meshData().positions.front()), maximum(minimum);
        double billboardRadius = 0.0;
        if (needsBounds)
        for (const auto& position : draw.meshData().positions)
        {
            const glm::dvec3 p(position);
            minimum = glm::min(minimum, p);
            maximum = glm::max(maximum, p);
            if (!bulkStreams || draw.billboard)
                billboardRadius = std::max(billboardRadius, glm::length(p));
        }
        const glm::dvec3 center = draw.billboard ? glm::dvec3(0.0) : (minimum + maximum) * 0.5;
        const double radius = draw.billboard ? billboardRadius : glm::length(maximum - minimum) * 0.5;
        for (auto& streams : mutableDraws)
        {
            if (streams.sorted)
                streams.sorted->bound = vsg::dsphere(center.x, center.y, center.z, radius);
            // Exact byte comparison avoids per-component conversion on the
            // common unchanged path. Signed-zero differences may cause one
            // extra upload, never a missed change. Non-packed GLM builds keep
            // the scalar conversion path.
                if (bulkStreams)
                {
                    updatePackedStream(draw.meshData().positions, *streams.positions, vsg::vec3(0.0f, 0.0f, 0.0f));
                    updatePackedStream(draw.meshData().normals, *streams.normals, vsg::vec3(0.0f, 0.0f, 1.0f));
                    updatePackedStream(draw.meshData().colors, *streams.colors, vsg::vec4(1.0f, 1.0f, 1.0f, 1.0f));
                    for (std::size_t set = 0; set < streams.texCoords.size(); ++set)
                        if (!draw.meshData().texCoordSets[set].empty())
                            updatePackedStream(draw.meshData().texCoordSets[set], *streams.texCoords[set], vsg::vec2(0.0f, 0.0f));
                    continue;
                }
            bool positionsChanged = false;
            bool normalsChanged = false;
            bool colorsChanged = false;
            for (std::size_t i = 0; i < draw.meshData().positions.size(); ++i)
            {
                const glm::vec3& position = draw.meshData().positions[i];
                const vsg::vec3 nextPosition(position.x, position.y, position.z);
                if ((*streams.positions)[i] != nextPosition)
                {
                    streams.positions->set(i, nextPosition);
                    positionsChanged = true;
                }
                const glm::vec3 normal = draw.meshData().normals.empty()
                    ? glm::vec3(0.0f, 0.0f, 1.0f) : draw.meshData().normals[i];
                const vsg::vec3 nextNormal(normal.x, normal.y, normal.z);
                if ((*streams.normals)[i] != nextNormal)
                {
                    streams.normals->set(i, nextNormal);
                    normalsChanged = true;
                }
                const glm::vec4 color = draw.meshData().colors.empty() ? glm::vec4(1.0f) : draw.meshData().colors[i];
                const vsg::vec4 nextColor(color.x, color.y, color.z, color.w);
                if ((*streams.colors)[i] != nextColor)
                {
                    streams.colors->set(i, nextColor);
                    colorsChanged = true;
                }
            }
            // VSG uploads dirty DYNAMIC_DATA streams. Placement-only changes
            // must not trigger uploads of identical mesh, color and UV data.
            if (positionsChanged)
                streams.positions->dirty();
            if (normalsChanged)
                streams.normals->dirty();
            if (colorsChanged)
                streams.colors->dirty();
            for (std::size_t set = 0; set < streams.texCoords.size(); ++set)
            {
                bool changed = false;
                for (std::size_t i = 0; i < draw.meshData().texCoordSets[set].size(); ++i)
                {
                    const glm::vec2& uv = draw.meshData().texCoordSets[set][i];
                    const vsg::vec2 nextUv(uv.x, uv.y);
                    if ((*streams.texCoords[set])[i] != nextUv)
                    {
                        streams.texCoords[set]->set(i, nextUv);
                        changed = true;
                    }
                }
                if (changed)
                    streams.texCoords[set]->dirty();
            }
        }
        return true;
    }
    }

    [[nodiscard]] inline bool updateImmediateEffectRealization(const RenderCore::ImmediateEffectDraw& draw,
        std::vector<StaticRealizationResult::MutableDrawStreams>& mutableDraws, bool bulkStreams = false) noexcept
    {
        return RenderCore::validImmediateEffectDraw(draw)
            && effect_update_detail::update(draw, mutableDraws, bulkStreams);
    }

    // Validation reuse is available only through a live immutable owner plus a
    // checked index. No caller-supplied "already valid" flag or mutable alias.
    [[nodiscard]] inline bool updateImmediateEffectRealization(const RenderCore::OwnedImmediateEffects& owner,
        std::size_t index, std::vector<StaticRealizationResult::MutableDrawStreams>& mutableDraws,
        bool bulkStreams = false) noexcept
    {
        return owner.valid() && index < owner.draws().size()
            && effect_update_detail::update(owner.draws()[index], mutableDraws, bulkStreams);
    }
}

#endif
