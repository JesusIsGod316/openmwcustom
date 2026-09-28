#ifndef OPENMW_SCENEUTIL_OPAQUESHADOWBATCH_H
#define OPENMW_SCENEUTIL_OPAQUESHADOWBATCH_H

#include "shadowproxygroup.hpp"
#include "shadowbatchcounters.hpp"
#include <osg/AlphaFunc>
#include <osg/Geometry>
#include <osgUtil/CullVisitor>
#include <cmath>
#include <limits>
#include <map>
#include <string_view>
#include <tuple>
#include <vector>

namespace SceneUtil::OpaqueShadowBatch
{
    struct Result
    {
        osg::ref_ptr<osg::Group> mShadowRoot;
        std::size_t mCandidateDrawables = 0, mEligibleDrawables = 0;
        std::size_t mRejectedState = 0, mRejectedGeometry = 0, mSourceIndices = 0;
    };

    inline bool opaqueState(const osg::StateSet* state)
    {
        if (!state) return true;
        if (state->getDataVariance() == osg::Object::DYNAMIC || state->getUpdateCallback()
            || state->getRenderingHint() == osg::StateSet::TRANSPARENT_BIN
            || (state->getMode(GL_BLEND) & osg::StateAttribute::ON)) return false;
        // Normal NIF/PBR materials carry these uniforms even when fully opaque.
        // ShadowsBin removes this material-only state on its no-alpha depth path.
        // Unknown uniforms (deformation, fading, custom effects) still fall back.
        static constexpr std::array<std::string_view, 8> materialUniforms{
            "material.diffuse", "material.ambient", "material.specular", "material.emission",
            "material.shininess", "material.emissiveMult", "material.specStrength", "material.vertexColorMode" };
        for (const auto& [name, value] : state->getUniformList())
        {
            if (!value.first || value.first->getUpdateCallback()
                || (value.second & osg::StateAttribute::PROTECTED)) return false;
            if (std::find(materialUniforms.begin(), materialUniforms.end(), name) != materialUniforms.end()) continue;
            if (name == "useDiffuseMapForShadowAlpha")
            {
                bool alpha = true;
                if (value.first->get(alpha) && !alpha) continue;
            }
            if (name == "alphaRef")
            {
                float reference = 0.f;
                if (value.first->get(reference) && std::isfinite(reference)) continue;
            }
            return false;
        }
        for (const auto& [type, value] : state->getAttributeList())
        {
            if (value.second & osg::StateAttribute::PROTECTED) return false;
            switch (type.first)
            {
                case osg::StateAttribute::ALPHAFUNC:
                    if (static_cast<const osg::AlphaFunc*>(value.first.get())->getFunction() != GL_ALWAYS)
                        return false;
                    break;
                // The engine shadow program overrides unprotected material programs.
                // These remaining material attributes do not alter opaque depth.
                case osg::StateAttribute::MATERIAL:
                case osg::StateAttribute::PROGRAM:
                case osg::StateAttribute::BLENDFUNC:
                case osg::StateAttribute::SHADEMODEL:
                case osg::StateAttribute::LIGHTMODEL:
                    break;
                default: return false; // Depth, winding, offset, masks, unknown state: exact fallback.
            }
        }
        for (const auto& [mode, value] : state->getModeList())
        {
            if (value & osg::StateAttribute::PROTECTED) return false;
            if (mode != GL_BLEND && mode != GL_ALPHA_TEST && mode != GL_LIGHTING
                && mode != GL_NORMALIZE && mode != GL_RESCALE_NORMAL) return false;
        }
        return true;
    }

    class ProxyVisit final : public osg::DrawableCullCallback
    {
    public:
        explicit ProxyVisit(std::uint64_t indices) : mIndices(indices) {}
        bool cull(osg::NodeVisitor* nv, osg::Drawable*, osg::RenderInfo*) const override
        {
            if (nv->asCullVisitor() && ShadowTraversalScope::active())
            {
                const unsigned index = std::min(ShadowTraversalScope::cascade(), 7u);
                ShadowBatchCounters::add(ShadowBatchCounters::proxyVisits[index]);
                ShadowBatchCounters::add(ShadowBatchCounters::proxyIndices[index], mIndices);
            }
            return false;
        }
    private:
        std::uint64_t mIndices;
    };

    inline Result build(osg::Group& normal)
    {
        Result result;
        // The caller passes only the flattened private optional merge graph. A
        // dynamic container or nontrivial inherited state is not stripped.
        if (normal.getDataVariance() == osg::Object::DYNAMIC || normal.getUpdateCallback()
            || normal.getCullCallback() || normal.getEventCallback() || !opaqueState(normal.getStateSet()))
            return result;
        constexpr std::size_t MaxBytes = 8 * 1024 * 1024;
        constexpr std::size_t MaxVertices = 131072;
        constexpr std::size_t MaxIndices = 393216;
        constexpr std::size_t MaxBuckets = 16;
        struct Bucket
        {
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            osg::ref_ptr<osg::DrawElementsUInt> indices = new osg::DrawElementsUInt(GL_TRIANGLES);
            std::vector<osg::ref_ptr<osg::Node>> originals;
        };
        using Key = std::tuple<int, int, int, osg::Node::NodeMask>;
        std::map<Key, Bucket> buckets;
        osg::ref_ptr<osg::Group> shadow = new osg::Group;
        shadow->setStateSet(normal.getStateSet());
        shadow->setNodeMask(normal.getNodeMask());
        std::size_t bytes = 0;
        for (unsigned i = 0; i < normal.getNumChildren(); ++i)
        {
            auto* child = normal.getChild(i);
            auto* geometry = child ? child->asGeometry() : nullptr;
            auto fallback = [&] { if (child) shadow->addChild(child); };
            if (!geometry) { fallback(); continue; }
            ++result.mCandidateDrawables;
            if (!opaqueState(geometry->getStateSet()))
            {
                ++result.mRejectedState; fallback(); continue;
            }
            std::string shaderPrefix;
            if (geometry->getUserValue("shaderPrefix", shaderPrefix))
            {
                ++result.mRejectedState; fallback(); continue; // Explicit custom/shader-specialized content.
            }
            const auto* vertices = dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray());
            const auto box = geometry->getBoundingBox();
            bool valid = geometry->getDataVariance() != osg::Object::DYNAMIC
                && !geometry->getDrawCallback() && !geometry->getCullCallback()
                && !geometry->getUpdateCallback() && !geometry->getEventCallback()
                && !geometry->getComputeBoundingBoxCallback() && geometry->getCullingActive()
                && vertices && !vertices->empty() && vertices->size() <= MaxVertices
                && geometry->getNumPrimitiveSets() && box.valid() && std::isfinite(box.radius())
                && box.radius() <= 1024.f && box.center().length() < 1.e8f;
            std::size_t indexCount = 0;
            if (valid)
            {
                for (const auto& v : *vertices)
                    valid = valid && std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
                for (const auto& primitive : geometry->getPrimitiveSetList())
                {
                    if (!primitive || primitive->getMode() != GL_TRIANGLES || primitive->getNumInstances() != 0
                        || primitive->getNumIndices() % 3 != 0) { valid = false; break; }
                    indexCount += primitive->getNumIndices();
                    if (indexCount > MaxIndices) { valid = false; break; }
                    for (unsigned j = 0; j < primitive->getNumIndices(); ++j)
                        if (primitive->index(j) >= vertices->size()) { valid = false; break; }
                }
            }
            const std::size_t added = valid ? vertices->size() * sizeof(osg::Vec3f) + indexCount * sizeof(unsigned) : 0;
            if (!valid || !indexCount || added > MaxBytes - bytes)
            {
                ++result.mRejectedGeometry; fallback(); continue;
            }
            const auto center = box.center();
            const Key key{ static_cast<int>(std::floor(center.x() / 1024.f)),
                static_cast<int>(std::floor(center.y() / 1024.f)),
                static_cast<int>(std::floor(center.z() / 1024.f)), geometry->getNodeMask() };
            auto found = buckets.find(key);
            if (found == buckets.end())
            {
                if (buckets.size() == MaxBuckets) { ++result.mRejectedGeometry; fallback(); continue; }
                found = buckets.try_emplace(key).first;
            }
            auto& bucket = found->second;
            if (bucket.vertices->size() + vertices->size() > MaxVertices
                || bucket.indices->size() + indexCount > MaxIndices)
            {
                ++result.mRejectedGeometry; fallback(); continue;
            }
            const unsigned base = static_cast<unsigned>(bucket.vertices->size());
            bucket.vertices->insert(bucket.vertices->end(), vertices->begin(), vertices->end());
            for (const auto& primitive : geometry->getPrimitiveSetList())
                for (unsigned j = 0; j < primitive->getNumIndices(); ++j)
                    bucket.indices->push_back(base + primitive->index(j));
            bucket.originals.emplace_back(child);
            bytes += added;
        }
        for (auto& [key, bucket] : buckets)
        {
            if (bucket.originals.size() < 2)
            {
                for (auto& original : bucket.originals) shadow->addChild(original);
                continue;
            }
            osg::ref_ptr<osg::Geometry> proxy = new osg::Geometry;
            proxy->setDataVariance(osg::Object::STATIC);
            proxy->setNodeMask(std::get<3>(key));
            proxy->setVertexArray(bucket.vertices);
            proxy->addPrimitiveSet(bucket.indices);
            proxy->setUseVertexBufferObjects(true);
            proxy->setUseDisplayList(false);
            if (ShadowBatchCounters::enabled()) proxy->setCullCallback(new ProxyVisit(bucket.indices->size()));
            shadow->addChild(proxy);
            result.mEligibleDrawables += bucket.originals.size();
            result.mSourceIndices += bucket.indices->size();
            ShadowBatchCounters::add(ShadowBatchCounters::built);
            ShadowBatchCounters::add(ShadowBatchCounters::builtIndices, bucket.indices->size());
        }
        if (result.mEligibleDrawables >= 2) result.mShadowRoot = shadow;
        return result;
    }
}
#endif
