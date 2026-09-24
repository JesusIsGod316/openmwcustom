#ifndef OPENMW_MWRENDER_V4GEOMETRYSNAPSHOTCACHE_H
#define OPENMW_MWRENDER_V4GEOMETRYSNAPSHOTCACHE_H

#include <components/rendercore/effectframe.hpp>
#include <components/misc/environmentflag.hpp>
#include <osg/Geometry>
#include <osg/StateSet>
#include <osg/TexGen>
#include <osg/TexMat>
#include <osg/observer_ptr>
#include <cstring>
#include <cstdlib>
#include <unordered_map>
#include <typeinfo>

namespace MWRender::v4_effect_detail
{
    // Owner-thread cache. No live OSG object crosses the neutral handoff and
    // no modified-count assumption can hide an unmarked mod/controller edit.
    // Compare every input byte, but convert/allocate/validate only on a miss.
    class GeometrySnapshotCache
    {
    public:
        explicit GeometrySnapshotCache(std::size_t budget = 128u * 1024u * 1024u) : mBudget(budget) {}

        bool find(const osg::Geometry& geometry, const osg::StateSet& state, RenderCore::ImmediateEffectDraw& draw)
        {
            auto found = mEntries.find(&geometry);
            const bool candidate = found != mEntries.end() && found->second.geometry.valid();
            bool matches = false;
            if (candidate && Misc::environmentFlag<"OPENMW_V4_INCREMENTAL_CAPTURE">())
            {
                // Read/compare the old exact stamp in place. Warm frames no
                // longer allocate and memcpy all arrays just to compare them.
                mCompare = &found->second.stamp;
                mOffset = 0;
                mMatches = true;
                matches = stamp(geometry, state, draw) && mMatches && mOffset == mCompare->size();
                mCompare = nullptr;
            }
            if (!matches)
            {
                mStamp.clear();
                mAdmissible = stamp(geometry, state, draw) && mStamp.size() <= mBudget / 4;
                if (!mAdmissible) { ++misses; return false; }
                matches = candidate && found->second.stamp == mStamp;
                if (!matches) { ++misses; return false; }
            }
            auto& entry = found->second;
            entry.used = ++mClock;
            draw.meshSnapshot = entry.mesh;
            draw.bounds = entry.bounds;
            for (std::size_t i = 0; i < draw.textures.size(); ++i)
                draw.textures[i].binding.transform.uvSet = entry.uvSets[i];
            ++hits;
            return true;
        }

        void insert(const osg::Geometry& geometry, RenderCore::ImmediateEffectDraw& draw)
        {
            if (!mAdmissible) return;
            Entry entry;
            entry.geometry = &geometry;
            entry.stamp = mStamp;
            entry.mesh = std::make_shared<const RenderCore::FrozenEffectMesh>(draw.mesh);
            if (!entry.mesh->valid()) return;
            entry.bounds = draw.bounds;
            for (const auto& texture : draw.textures)
                entry.uvSets.push_back(texture.binding.transform.uvSet);
            const auto& mesh = draw.mesh;
            entry.bytes = entry.stamp.capacity()
                + (mesh.positions.capacity() + mesh.normals.capacity() + mesh.tangents.capacity()
                    + mesh.bitangents.capacity()) * sizeof(glm::vec3)
                + mesh.colors.capacity() * sizeof(glm::vec4) + mesh.indices.capacity() * sizeof(std::uint32_t)
                + mesh.surfaces.capacity() * sizeof(RenderCore::MeshSurface);
            for (const auto& uv : mesh.texCoordSets) entry.bytes += uv.capacity() * sizeof(glm::vec2);
            if (entry.bytes > mBudget) return;
            if (auto prior = mEntries.find(&geometry); prior != mEntries.end())
            { mBytes -= prior->second.bytes; mEntries.erase(prior); }
            while (!mEntries.empty() && (mBytes + entry.bytes > mBudget || mEntries.size() >= 4096))
            {
                auto oldest = mEntries.begin();
                for (auto it = mEntries.begin(); it != mEntries.end(); ++it)
                {
                    if (!it->second.geometry.valid()) { oldest = it; break; }
                    if (it->second.used < oldest->second.used) oldest = it;
                }
                mBytes -= oldest->second.bytes;
                mEntries.erase(oldest); // in-flight neutral owners remain alive
            }
            entry.used = ++mClock;
            mBytes += entry.bytes;
            draw.meshSnapshot = entry.mesh;
            draw.mesh = {};
            mEntries.emplace(&geometry, std::move(entry));
        }

        std::uint64_t hits = 0, misses = 0;
        std::uint64_t stampBytesCopied = 0;
        std::size_t bytes() const { return mBytes; }

    private:
        void bytes(const void* source, std::size_t size)
        {
            if (!size) return;
            if (mCompare)
            {
                if (mOffset > mCompare->size() || size > mCompare->size() - mOffset
                    || std::memcmp(mCompare->data() + mOffset, source, size) != 0) mMatches = false;
                mOffset += size;
                return;
            }
            const auto offset = mStamp.size();
            mStamp.resize(offset + size);
            std::memcpy(mStamp.data() + offset, source, size);
            stampBytesCopied += size;
        }
        template <class T> void value(const T& data) { bytes(&data, sizeof(data)); }
        void array(const osg::Array* data)
        {
            value(data); // UV alias identity affects stream deduplication
            if (!data) return;
            value(data->getType());
            value(data->getNumElements());
            value(data->getTotalDataSize());
            bytes(data->getDataPointer(), data->getTotalDataSize());
        }
        bool stamp(const osg::Geometry& geometry, const osg::StateSet& state,
            const RenderCore::ImmediateEffectDraw& draw)
        {
            array(geometry.getVertexArray());
            array(geometry.getNormalArray());
            array(geometry.getColorArray());
            value(geometry.getTexCoordArrayList().size());
            for (const auto& uv : geometry.getTexCoordArrayList()) array(uv.get());
            value(geometry.getNumPrimitiveSets());
            for (const auto& primitive : geometry.getPrimitiveSetList())
            {
                value(bool(primitive));
                if (!primitive) continue;
                // Unknown subclasses may override index() with extra state.
                const auto& type = typeid(*primitive);
                if (type != typeid(osg::DrawArrays) && type != typeid(osg::DrawArrayLengths)
                    && type != typeid(osg::DrawElementsUByte) && type != typeid(osg::DrawElementsUShort)
                    && type != typeid(osg::DrawElementsUInt)) return false;
                value(primitive->getType());
                value(primitive->getMode());
                value(primitive->getNumIndices());
                value(primitive->getTotalDataSize());
                if (primitive->getNumIndices()) value(primitive->index(0)); // DrawArrays/Lengths first
                bytes(primitive->getDataPointer(), primitive->getTotalDataSize());
            }
            value(draw.material.environmentMapMode);
            value(draw.textures.size());
            for (const auto& texture : draw.textures)
            {
                const auto unit = texture.binding.transform.uvSet;
                value(unit);
                value(texture.binding.role);
                for (const auto mode : {GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R, GL_TEXTURE_GEN_Q})
                    value(state.getTextureMode(unit, mode));
                const auto* generator = dynamic_cast<const osg::TexGen*>(
                    state.getTextureAttribute(unit, osg::StateAttribute::TEXGEN));
                value(bool(generator));
                if (generator) value(generator->getMode());
                const auto* matrix = dynamic_cast<const osg::TexMat*>(
                    state.getTextureAttribute(unit, osg::StateAttribute::TEXMAT));
                value(bool(matrix));
                if (matrix) bytes(matrix->getMatrix().ptr(), sizeof(osg::Matrix::value_type) * 16);
            }
            return true;
        }
        struct Entry
        {
            osg::observer_ptr<const osg::Geometry> geometry;
            std::vector<unsigned char> stamp;
            std::shared_ptr<const RenderCore::FrozenEffectMesh> mesh;
            RenderCore::AxisAlignedBounds bounds;
            std::vector<std::uint32_t> uvSets;
            std::size_t bytes = 0;
            std::uint64_t used = 0;
        };
        const std::size_t mBudget;
        std::size_t mBytes = 0;
        std::uint64_t mClock = 0;
        bool mAdmissible = false;
        std::vector<unsigned char> mStamp;
        const std::vector<unsigned char>* mCompare = nullptr;
        std::size_t mOffset = 0;
        bool mMatches = false;
        std::unordered_map<const osg::Geometry*, Entry> mEntries;
    };
}
#endif
