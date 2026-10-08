#include "generatedstaticlod.hpp"
#include "assetpaging.hpp"
#include "depth.hpp"

#include <components/resource/derivedassetcache.hpp>
#include <components/resource/speculativebudget.hpp>
#include <components/resource/assetgpubudget.hpp>
#include <components/debug/debuglog.hpp>
#include <sstream>
#include <components/nifosg/matrixtransform.hpp>
#include <components/shader/assetgeometrycontract.hpp>
#include <components/shader/programvariant.hpp>
#include <components/sceneutil/shadowbatchcounters.hpp>
#include <components/sceneutil/shadowproxygroup.hpp>
#include <components/sceneutil/pagingwork.hpp>
#include <components/sceneutil/temporalvertexlineage.hpp>
#include <components/sceneutil/persistentopaquepackets.hpp>
#include <osg/AlphaFunc>
#include <osg/Camera>
#include <osg/ClipControl>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osg/Geode>
#include <osg/GraphicsContext>
#include <osg/Program>
#include <osg/Texture2D>
#include <osg/State>
#include <osg/Texture>
#include <osg/Material>
#include <osg/CullFace>
#include <osg/FrontFace>
#include <osg/Depth>
#include <osg/PolygonOffset>
#include <osg/BlendFunc>
#include <osg/TexEnv>
#include "material.hpp"
#include "texturetype.hpp"
#include <osg/Viewport>
#include <osgUtil/CullVisitor>
#include <osgUtil/StateGraph>
#include <meshoptimizer.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <stdexcept>
#include <vector>
#include <set>
#include <limits>

namespace SceneUtil::GeneratedStaticLod
{
    namespace
    {
        constexpr std::size_t MaximumVertices = 250000, MaximumIndices = 1500000;
        constexpr std::size_t MaximumCookBytes = 64 * 1024 * 1024;
        constexpr float RelativeError = .02f, MaximumPixels = 1.f;
        const osg::Program* effectiveProgram(const std::vector<const osg::StateSet*>& path)
        {
            const osg::StateAttribute* value=nullptr;
            unsigned flags=0;
            // Exact State::pushAttributeList override/protected rule, without
            // cloning unrelated per-frame light, shadow and camera uniforms.
            for(const auto* state:path) if(state)
                if(const auto* pair=state->getAttributePair(osg::StateAttribute::PROGRAM))
                    if(!value || !(flags&osg::StateAttribute::OVERRIDE) || (pair->second&osg::StateAttribute::PROTECTED))
                    {value=pair->first.get();flags=pair->second;}
            return dynamic_cast<const osg::Program*>(value);
        }
        const osg::ClipControl* effectiveClipControl(const std::vector<const osg::StateSet*>& path)
        {
            const osg::StateAttribute* value=nullptr;
            unsigned flags=0;
            for(const auto* state:path) if(state)
                if(const auto* pair=state->getAttributePair(osg::StateAttribute::CLIPCONTROL))
                    if(!value || !(flags&osg::StateAttribute::OVERRIDE) || (pair->second&osg::StateAttribute::PROTECTED))
                    {value=pair->first.get();flags=pair->second;}
            return dynamic_cast<const osg::ClipControl*>(value);
        }
        bool projectionMatches(const osg::Matrixd& cull,const osg::Matrixf& shader,
            const osg::ClipControl* clip)
        {
            const auto matches=[&](const osg::Matrixd& expected) {
                for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c)
                {
                    const float value=static_cast<float>(expected(r,c));
                    const float tolerance=4*std::numeric_limits<float>::epsilon()*std::max(1.f,std::abs(value));
                    if(!std::isfinite(value) || !std::isfinite(shader(r,c)) || std::abs(shader(r,c)-value)>tolerance) return false;
                }
                return true;
            };
            if(matches(cull)) return true;
            if(!AutoDepth::isReversed() || !clip || typeid(*clip)!=typeid(osg::ClipControl) || clip->getOrigin()!=osg::ClipControl::LOWER_LEFT
                || clip->getDepthMode()!=osg::ClipControl::ZERO_TO_ONE) return false;
            // RenderingManager retains ordinary finite perspective for OSG
            // culling, while the GPU uses depth z'=(w-z)/2. Its optional XY
            // post-translation changes neither this conversion nor the lateral
            // frustum terms. Do not accept an arbitrary shader depth projection.
            if(cull(0,0)<=0 || cull(1,1)<=0 || cull(2,2)>=-1 || cull(3,2)>=0
                || cull(0,2)!=0 || cull(1,2)!=0 || cull(0,3)!=0 || cull(1,3)!=0
                || cull(3,0)!=0 || cull(3,1)!=0 || cull(2,3)!=-1 || cull(3,3)!=0) return false;
            osg::Matrixd reversed=cull;
            for(unsigned r=0;r<4;++r) reversed(r,2)=(cull(r,3)-cull(r,2))*.5;
            return matches(reversed);
        }
        bool boundedAttribute(const osg::StateAttribute& value)
        {
            const auto& type=typeid(value);
            return type==typeid(osg::Material) || type==typeid(SceneUtil::Material) || type==typeid(osg::CullFace)
                || type==typeid(osg::FrontFace) || type==typeid(osg::Depth) || type==typeid(osg::PolygonOffset)
                || type==typeid(osg::AlphaFunc) || type==typeid(osg::BlendFunc) || type==typeid(osg::TexEnv)
                || type==typeid(SceneUtil::TextureType);
        }
        template<std::size_t N>
        void guardText(std::array<char,N>& out, std::string_view text) noexcept
        {
            out.fill(0);
            for(std::size_t i=0;i<std::min(text.size(),N-1);++i)
                out[i]=static_cast<unsigned char>(text[i])<32 || text[i]=='"' ? '?' : text[i];
        }
        struct OriginalReceipt final : PersistentOpaquePackets::RetainedOpaqueDrawCallback
        {
            osg::ref_ptr<osg::Drawable::DrawCallback> delegate;
            explicit OriginalReceipt(osg::Drawable::DrawCallback* value):delegate(value) {}
            void drawImplementation(osg::RenderInfo& info,const osg::Drawable* drawable) const override
            {
                const auto* geometry=drawable->asGeometry();
                if(geometry)
                {
                    std::uint64_t indices=0;
                    for(unsigned i=0;i<geometry->getNumPrimitiveSets();++i) indices+=geometry->getPrimitiveSet(i)->getNumIndices();
                    if(indices>=3) {++Counters::controlDraws;Counters::controlSubmittedIndices+=indices;}
                }
                if(delegate) delegate->drawImplementation(info,drawable);else drawable->drawImplementation(info);
            }
        };
        struct DrawReceipt final : PersistentOpaquePackets::RetainedOpaqueDrawCallback
        {
            explicit DrawReceipt(std::size_t count) : indices(count) {}
            std::size_t indices;
            osg::observer_ptr<const osg::Geometry> owner;
            std::shared_ptr<MaterialGuard> guard;
            std::vector<OriginalDraw> originals;
            osg::Matrixd derivedMatrix;
            std::vector<osg::ref_ptr<const osg::StateSet>> candidateStates;
            void drawImplementation(osg::RenderInfo& info, const osg::Drawable* drawable) const override
            {
                if (guard && !guard->current(info.getState()))
                {
                    ++Counters::materialDrawFallbacks;
                    drawOriginals(info, originals, derivedMatrix, drawable->getStateSet(),candidateStates);
                    return;
                }
                ++Counters::derivedDraws; Counters::submittedIndices.fetch_add(indices);
                drawable->drawImplementation(info);
            }
        };
        bool material(const osg::StateSet* state, bool perView = false, RefusalInfo* info = nullptr,
            std::set<const osg::Program*>* validated = nullptr)
        {
            if (!state) return true;
            const auto fail = [&](Detail detail) { if(info) info->detail=detail; return false; };
            if (!perView && (state->getDataVariance() == osg::Object::DYNAMIC || state->getUpdateCallback() || state->getEventCallback())) return fail(Detail::Dynamic);
            if (state->getMode(GL_BLEND) & osg::StateAttribute::ON) return fail(Detail::Blend);
            if ((state->getMode(GL_ALPHA_TEST) & osg::StateAttribute::ON) || state->getRenderingHint() == osg::StateSet::TRANSPARENT_BIN)
                return fail(Detail::Alpha);
            if (state->getAttribute(osg::StateAttribute::STENCIL) || (!perView && state->getUniform("projectionMatrix"))) return fail(Detail::Other);
            if (const auto* a = dynamic_cast<const osg::AlphaFunc*>(state->getAttribute(osg::StateAttribute::ALPHAFUNC)))
                if (a->getFunction() != GL_ALWAYS) return fail(Detail::Alpha);
            if (const auto* p = dynamic_cast<const osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM)))
                if ((!validated || validated->insert(p).second))
                if (const auto reason = Shader::AssetGeometry::validate(*p); reason != Shader::AssetGeometry::Refusal::None)
                { if(info) {info->program=p;info->detail=shaderDetail(static_cast<unsigned>(reason));} return false; }
            if (const auto* a = state->getUniform("alphaTest"))
            { bool enabled = true; if (!a->get(enabled) || enabled) return fail(Detail::Alpha); }
            return !state->useRenderBinDetails() || state->getBinName().empty() || state->getBinName() == "RenderBin";
        }
        bool array(const osg::Array* a, std::size_t vertices)
        {
            if (!a) return true;
            if (a->getDataVariance() == osg::Object::DYNAMIC) return false;
            if (a->getBinding() != osg::Array::BIND_PER_VERTEX && a->getBinding() != osg::Array::BIND_OVERALL)
                return false;
            if (a->getNumElements() != (a->getBinding() == osg::Array::BIND_OVERALL ? 1 : vertices)) return false;
            return dynamic_cast<const osg::Vec2Array*>(a) || dynamic_cast<const osg::Vec3Array*>(a)
                || dynamic_cast<const osg::Vec4Array*>(a) || dynamic_cast<const osg::Vec4ubArray*>(a);
        }
        Refusal layout(const osg::Geometry& geometry)
        {
            const auto* positions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
            if (!positions || positions->size() < 3 || positions->size() > MaximumVertices
                || geometry.getNumPrimitiveSets() != 1 || geometry.getNumVertexAttribArrays() != 0)
                return Refusal::Layout;
            for (const auto& p : *positions)
                if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || !std::isfinite(p.z())) return Refusal::Layout;
            const auto* primitive = geometry.getPrimitiveSet(0);
            if (!primitive || primitive->getMode() != GL_TRIANGLES || !primitive->getDrawElements()
                || primitive->getNumInstances() > 1
                || primitive->getNumIndices() < 3 || primitive->getNumIndices() > MaximumIndices
                || primitive->getNumIndices() % 3) return Refusal::Layout;
            for (unsigned i = 0; i < primitive->getNumIndices(); ++i)
                if (primitive->index(i) >= positions->size()) return Refusal::Layout;
            if (!array(geometry.getNormalArray(), positions->size()) || !array(geometry.getColorArray(), positions->size())
                || geometry.getSecondaryColorArray() || geometry.getFogCoordArray()) return Refusal::Layout;
            for (unsigned i = 0; i < geometry.getNumTexCoordArrays(); ++i)
                if (!array(geometry.getTexCoordArray(i), positions->size())) return Refusal::Layout;
            return Refusal::None;
        }
        struct Size
        {
            std::size_t nodes = 0, geometries = 0, vertices = 0, indices = 0, bytes = 0;
            std::uint64_t scratch = 0, serialized = 0, cacheOverlap = 0, graphBytes = 0;
            std::set<const osg::Program*> programs;
        };
        bool emptyLoaderMarker(const osg::Node& node, Size& size, unsigned depth)
        {
            unsigned record = ~0u;
            if (++size.nodes > 1024 || depth > 32 || node.getDataVariance() != osg::Object::STATIC
                || !node.getUserValue("recordIndex",record) || record == ~0u || node.asDrawable()
                || node.getUpdateCallback() || node.getEventCallback() || node.getCullCallback()
                || node.getComputeBoundingSphereCallback()
                || (typeid(node) != typeid(osg::Group) && typeid(node) != typeid(osg::MatrixTransform)
                    && typeid(node) != typeid(NifOsg::MatrixTransform))) return false;
            if (const auto* state = node.getStateSet(); state
                && (state->getDataVariance() == osg::Object::DYNAMIC || state->getUpdateCallback()
                    || state->getEventCallback())) return false;
            if (const auto* transform = dynamic_cast<const osg::MatrixTransform*>(&node))
            {
                if (transform->getReferenceFrame() != osg::Transform::RELATIVE_RF) return false;
                for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
                    if (!std::isfinite(transform->getMatrix()(r,c))) return false;
            }
            const auto* group = node.asGroup();
            if (!group) return false;
            for (unsigned i = 0; i < group->getNumChildren(); ++i)
                if (!emptyLoaderMarker(*group->getChild(i),size,depth + 1)) return false;
            return true;
        }
        Refusal inspectNode(const osg::Node& node, Size& size, unsigned depth, RefusalInfo* info = nullptr)
        {
            if(info) info->node=&node;
            // Loader-emitted hidden collision/marker hierarchies contain no
            // drawables. Certify this exact empty shape before CopyOp filters
            // it; a hidden mesh, controller or custom node is never admitted.
            if (!node.getNodeMask()) {if(info) info->detail=Detail::HiddenGraph;return emptyLoaderMarker(node,size,depth) ? Refusal::None : Refusal::Graph;}
            if (++size.nodes > 1024 || depth > 32) return Refusal::Capacity;
            size.graphBytes += 1024 + node.getName().size() * 2ull;
            for (const auto& description : node.getDescriptions()) size.graphBytes += 32 + description.size() * 2ull;
            if (const auto* children = node.asGroup()) size.graphBytes += children->getNumChildren() * 16ull;
            if (size.graphBytes > MaximumCookBytes) return Refusal::Capacity;
            std::string custom;
            if (node.getDataVariance() == osg::Object::DYNAMIC || node.getUpdateCallback() || node.getEventCallback()
                || node.getCullCallback() || node.getComputeBoundingSphereCallback()
                || node.getUserValue("shaderPrefix", custom) || dynamic_cast<const osg::LOD*>(&node))
            {if(info) info->detail=node.getDataVariance()==osg::Object::DYNAMIC?Detail::Dynamic:Detail::GraphCallback;return Refusal::Graph;}
            if (!material(node.getStateSet(), false, info,&size.programs)) return Refusal::Material;
            if (const auto* geometry = dynamic_cast<const osg::Geometry*>(&node))
            {
                if (typeid(*geometry) != typeid(osg::Geometry) || geometry->getDrawCallback()
                    || geometry->getComputeBoundingBoxCallback()) return Refusal::Graph;
                if (auto refusal = layout(*geometry); refusal != Refusal::None) return refusal;
                const auto* positions = static_cast<const osg::Vec3Array*>(geometry->getVertexArray());
                const auto* primitive = geometry->getPrimitiveSet(0);
                ++size.geometries;
                size.vertices += geometry->getVertexArray()->getNumElements();
                size.indices += geometry->getPrimitiveSet(0)->getNumIndices();
                size.bytes += geometry->getVertexArray()->getTotalDataSize() + geometry->getPrimitiveSet(0)->getNumIndices() * 4ull;
                for (const auto* a : {geometry->getNormalArray(), geometry->getColorArray()})
                    if (a) size.bytes += a->getTotalDataSize();
                for (unsigned i = 0; i < geometry->getNumTexCoordArrays(); ++i)
                    if (const auto* a = geometry->getTexCoordArray(i)) size.bytes += a->getTotalDataSize();
                std::uint64_t serialized = positions->getTotalDataSize() + primitive->getNumIndices() * 4ull;
                unsigned components = 0;
                const auto attribute = [&](const osg::Array* a) {
                    if (!a) return;
                    serialized += 8 + a->getTotalDataSize();
                    if (a->getBinding() == osg::Array::BIND_PER_VERTEX) components += a->getDataSize();
                };
                attribute(geometry->getNormalArray()); attribute(geometry->getColorArray());
                for (unsigned i = 0; i < geometry->getNumTexCoordArrays(); ++i) attribute(geometry->getTexCoordArray(i));
                // This includes the pinned simplifier's adjacency/collapse,
                // packed float attributes and per-attribute quadrics. Keep the
                // existing 64-MiB per-leaf admission; retained earlier output is
                // separately counted in the total cloned primitive allowance.
                const auto scratch = positions->size() * (256ull + components * 24ull)
                    + primitive->getNumIndices() * 64ull + serialized;
                if (components <= 32 && scratch <= 64 * Resource::SpeculativeBudget::MiB)
                {
                    size.scratch = std::max<std::uint64_t>(size.scratch, scratch);
                    size.cacheOverlap = std::max<std::uint64_t>(size.cacheOverlap, primitive->getNumIndices() * 12ull + 32);
                }
                size.serialized = std::max(size.serialized, serialized);
                return size.geometries > 64 || size.vertices > MaximumVertices || size.indices > MaximumIndices
                    || size.bytes > MaximumCookBytes ? Refusal::Capacity : Refusal::None;
            }
            if (const auto* transform = dynamic_cast<const osg::MatrixTransform*>(&node))
            { if (transform->getReferenceFrame() != osg::Transform::RELATIVE_RF
                    || !positiveUniformTransform(transform->getMatrix())) return Refusal::Transform; }
            else if (node.asTransform()) return Refusal::Transform;
            const auto* group = node.asGroup();
            if (!group || (typeid(node) != typeid(osg::Group) && typeid(node) != typeid(osg::Geode)
                && typeid(node) != typeid(osg::MatrixTransform)
                && typeid(node) != typeid(NifOsg::MatrixTransform))) return Refusal::Graph;
            for (unsigned i = 0; i < group->getNumChildren(); ++i)
                if (auto refusal = inspectNode(*group->getChild(i), size, depth + 1, info); refusal != Refusal::None) return refusal;
            return Refusal::None;
        }
        Refusal inspect(const osg::Node& node, RefusalInfo* info = nullptr) { Size size; return inspectNode(node, size, 0, info); }
        void append(Resource::DerivedAssets::Bytes& bytes, const void* data, std::size_t size)
        {
            if (bytes.size() + size > MaximumCookBytes) throw std::bad_alloc{};
            const auto* p = static_cast<const std::uint8_t*>(data); bytes.insert(bytes.end(), p, p + size);
        }
        struct Cook final : osg::NodeVisitor
        {
            explicit Cook(std::string_view source) : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), identity(source) {}
            std::string identity;
            std::size_t before = 0, after = 0, changed = 0;
            std::uint64_t newIndexBytes = 0;
            float error = 0.f, transformScale = 1.f;
            std::vector<osg::Geometry*> geometries;
            std::vector<bool> reducedGeometry;
            void apply(osg::Transform& transform) override
            {
                const auto* matrix = dynamic_cast<const osg::MatrixTransform*>(&transform);
                const float saved = transformScale;
                transformScale *= static_cast<float>(osg::Vec3d(
                    matrix->getMatrix()(0,0), matrix->getMatrix()(0,1), matrix->getMatrix()(0,2)).length());
                traverse(transform); transformScale = saved;
            }
            void apply(osg::Geometry& geometry) override
            {
                PagingWorkScope::checkpoint();
                geometries.push_back(&geometry);
                reducedGeometry.push_back(false);
                const auto* positions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
                const auto* primitive = geometry.getPrimitiveSet(0);
                std::vector<unsigned> indices(primitive->getNumIndices());
                for (unsigned i = 0; i < indices.size(); ++i) indices[i] = primitive->index(i);
                before += indices.size();
                geometry.setDrawCallback(new DrawReceipt(indices.size()));
                Resource::DerivedAssets::Bytes source;
                // Exact reserve avoids an unaccounted geometric-growth copy of
                // the hash input while the simplifier/cache allocation is live.
                std::size_t sourceBytes = positions->getTotalDataSize() + indices.size() * sizeof(unsigned);
                const auto serializedAttribute = [&](const osg::Array* a) {
                    if (a) sourceBytes += 8 + a->getTotalDataSize();
                };
                serializedAttribute(geometry.getNormalArray()); serializedAttribute(geometry.getColorArray());
                for (unsigned i = 0; i < geometry.getNumTexCoordArrays(); ++i)
                    serializedAttribute(geometry.getTexCoordArray(i));
                if (sourceBytes > MaximumCookBytes) throw std::bad_alloc{};
                source.reserve(sourceBytes);
                append(source, positions->getDataPointer(), positions->getTotalDataSize());
                append(source, indices.data(), indices.size() * sizeof(unsigned));
                std::vector<const osg::Array*> attributes;
                if (geometry.getNormalArray()) attributes.push_back(geometry.getNormalArray());
                if (geometry.getColorArray()) attributes.push_back(geometry.getColorArray());
                for (unsigned i = 0; i < geometry.getNumTexCoordArrays(); ++i)
                    if (geometry.getTexCoordArray(i)) attributes.push_back(geometry.getTexCoordArray(i));
                unsigned components = 0;
                for (const auto* a : attributes)
                {
                    const unsigned type = a->getType(), binding = a->getBinding();
                    append(source, &type, sizeof(type)); append(source, &binding, sizeof(binding));
                    append(source, a->getDataPointer(), a->getTotalDataSize());
                    if (binding == osg::Array::BIND_PER_VERTEX) components += a->getDataSize();
                }
                if (components > 32) { after += indices.size(); return; }
                // Bound the pinned simplifier's adjacency/collapse tables,
                // float attributes and per-attribute quadric gradients before
                // allocating them. The graph-derived reservation also covers
                // cloned primitives and earlier immutable tier outputs.
                const std::uint64_t scratch = positions->size() * (256ull + components * 24ull)
                    + indices.size() * 64ull + source.size();
                if (scratch > 64 * Resource::SpeculativeBudget::MiB)
                {
                    ++Counters::refused[static_cast<unsigned>(Refusal::Capacity)];
                    after += indices.size(); return;
                }
                std::vector<float> packed(positions->size() * components), weights(components, .1f);
                std::size_t offset = 0;
                for (const auto* a : attributes)
                {
                    if (a->getBinding() != osg::Array::BIND_PER_VERTEX) continue;
                    const auto size = static_cast<unsigned>(a->getDataSize());
                    for (std::size_t i = 0; i < positions->size(); ++i)
                        for (unsigned c = 0; c < size; ++c)
                        {
                            const auto value = a->getDataType() == GL_FLOAT
                                ? static_cast<const float*>(a->getDataPointer())[i * size + c]
                                : static_cast<const unsigned char*>(a->getDataPointer())[i * size + c] / 255.f;
                            if (!std::isfinite(value)) throw std::runtime_error("nonfinite LOD attribute");
                            packed[i * components + offset + c] = value;
                        }
                    offset += size;
                }
                const auto key = Resource::DerivedAssets::key(Resource::DerivedAssets::digest(source),
                    identity + "|L1-v2-meshopt9e1f07b159d3cb777f1c67ed31fc11fd117986f4-index-only-border-0.02-attrs0.1-target0.5");
                std::vector<unsigned> reduced;
                float deviation = 0.f;
                if (auto payload = Resource::DerivedAssets::cache().read(key, MaximumIndices * 4 + 8);
                    payload && payload->size() >= 8)
                {
                    unsigned count = 0; std::memcpy(&count, payload->data(), 4);
                    std::memcpy(&deviation, payload->data() + 4, 4);
                    if (count > 0 && count < indices.size() && count % 3 == 0 && payload->size() == count * 4ull + 8
                        && std::isfinite(deviation) && deviation >= 0 && deviation <= RelativeError)
                    {
                        reduced.resize(count); std::memcpy(reduced.data(), payload->data() + 8, count * 4ull);
                        if (std::any_of(reduced.begin(), reduced.end(), [&](auto i) { return i >= positions->size(); })) reduced.clear();
                        else ++Counters::diskHits;
                    }
                }
                if (reduced.empty())
                {
                    reduced.resize(indices.size());
                    const auto count = meshopt_simplifyWithAttributes(reduced.data(), indices.data(), indices.size(),
                        reinterpret_cast<const float*>(positions->getDataPointer()), positions->size(), sizeof(osg::Vec3),
                        components ? packed.data() : nullptr, components * sizeof(float),
                        components ? weights.data() : nullptr, components, nullptr,
                        (indices.size() / 6) * 3, RelativeError, meshopt_SimplifyLockBorder, &deviation);
                    reduced.resize(count);
                    if (count == 0 || count >= indices.size() || !std::isfinite(deviation) || deviation < 0 || deviation > RelativeError)
                    { after += indices.size(); return; }
                    Resource::DerivedAssets::Bytes payload;
                    const unsigned count32 = static_cast<unsigned>(count);
                    append(payload, &count32, 4); append(payload, &deviation, 4);
                    append(payload, reduced.data(), reduced.size() * 4);
                    Resource::DerivedAssets::cache().write(key, payload);
                }
                auto draw = osg::ref_ptr<osg::DrawElementsUInt>(new osg::DrawElementsUInt(GL_TRIANGLES));
                draw->reserve(reduced.size());
                draw->assign(reduced.begin(), reduced.end());
                draw->setElementBufferObject(new osg::ElementBufferObject);
                newIndexBytes += reduced.size() * sizeof(unsigned);
                reducedGeometry.back() = true;
                geometry.setPrimitiveSet(0, draw);
                geometry.setUseDisplayList(false); geometry.setUseVertexBufferObjects(true);
                geometry.setDrawCallback(new DrawReceipt(reduced.size()));
                geometry.dirtyBound();
                error = std::max(error, deviation * meshopt_simplifyScale(
                    reinterpret_cast<const float*>(positions->getDataPointer()), positions->size(), sizeof(osg::Vec3)) * transformScale);
                after += reduced.size(); ++changed;
            }
        };
    }
    bool positiveUniformTransform(const osg::Matrixd& matrix)
    {
        for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
            if (!std::isfinite(matrix(r,c))) return false;
        osg::Vec3d a(matrix(0,0),matrix(0,1),matrix(0,2));
        osg::Vec3d b(matrix(1,0),matrix(1,1),matrix(1,2));
        osg::Vec3d c(matrix(2,0),matrix(2,1),matrix(2,2));
        const double scale = a.length2(), tolerance = std::max(1e-12, scale * 1e-5);
        return scale > 1e-12 && std::abs(scale-b.length2()) <= tolerance && std::abs(scale-c.length2()) <= tolerance
            && std::abs(a*b) <= tolerance && std::abs(a*c) <= tolerance && std::abs(b*c) <= tolerance
            && ((a ^ b) * c) > 0 && matrix(0,3) == 0 && matrix(1,3) == 0 && matrix(2,3) == 0 && matrix(3,3) == 1;
    }
    bool isDelegatingDrawCallback(const osg::Drawable::DrawCallback* callback)
    { return callback && (dynamic_cast<const DrawReceipt*>(callback) || dynamic_cast<const OriginalReceipt*>(callback)); }
    bool temporalDrawRecipeCurrent(const osg::Geometry& geometry,
        const std::vector<const osg::StateSet*>& inherited, osg::State* drawState)
    {
        Shader::AssetGeometry::ProducerReadScope producerRead;
        const auto* callback=geometry.getDrawCallback();
        if(!callback) return true;
        if(const auto* receipt=dynamic_cast<const DrawReceipt*>(callback))
        {
            osg::ref_ptr<const osg::Geometry> owner;
            if(!receipt->owner.lock(owner) || owner.get()!=&geometry || !receipt->guard) return false;
            // Capture ancestry and the actual draw stack are separate proofs.
            // Passing ancestry must not hide a later overriding draw StateSet.
            return receipt->guard->current(nullptr,inherited)
                && (!drawState || receipt->guard->current(drawState));
        }
        if(const auto* receipt=dynamic_cast<const OriginalReceipt*>(callback))
            return !receipt->delegate; // Any delegated packet needs its own exact proof.
        return false;
    }
    Detail shaderDetail(unsigned reason)
    {
        switch (static_cast<Shader::AssetGeometry::Refusal>(reason))
        {
            case Shader::AssetGeometry::Refusal::Epoch: return Detail::ShaderEpoch;
            case Shader::AssetGeometry::Refusal::Source: return Detail::ShaderSource;
            case Shader::AssetGeometry::Refusal::Stage: return Detail::ShaderStage;
            case Shader::AssetGeometry::Refusal::Bindings: return Detail::ShaderBindings;
            case Shader::AssetGeometry::Refusal::Defines: return Detail::ShaderDefines;
            case Shader::AssetGeometry::Refusal::Features: return Detail::ShaderFeatures;
            case Shader::AssetGeometry::Refusal::Capacity: return Detail::ShaderCapacity;
            default: return Detail::ShaderFamily;
        }
    }
    const char* detailName(Detail value)
    {
        constexpr const char* names[]{"none","shader_family","shader_epoch","shader_source","shader_stage",
            "shader_bindings","shader_defines","shader_features","shader_capacity","alpha","blend","dynamic",
            "graph_callback","graph_type","hidden_graph","layout","other"};
        return static_cast<unsigned>(value) < static_cast<unsigned>(Detail::Count) ? names[static_cast<unsigned>(value)] : "unknown";
    }
    const char* buildStepName(BuildStep value)
    {
        constexpr const char* names[]{"none","scope","inspection","vertex_ownership","peak","host_admission","cook",
            "reduction","gpu_admission","gpu_retention","lineage","replay","guard","publication"};
        return static_cast<unsigned>(value) < static_cast<unsigned>(BuildStep::Count) ? names[static_cast<unsigned>(value)] : "unknown";
    }
    const char* guardRefusalName(GuardRefusal value)
    {
        constexpr const char* names[]{"none","path_capacity","null_node","child_capacity","stream_capacity",
            "state_capacity","uniform_recipe","define_capacity","attribute_recipe","texture_capacity",
            "texture_class","texture_image","texture_mips","texture_format","texture_dynamic",
            "metadata_capacity","transform_recipe","shader_missing","shader_imports","shader_certificate",
            "snapshot_allocation","snapshot_exception","stale"};
        return static_cast<unsigned>(value)<static_cast<unsigned>(GuardRefusal::Count) ? names[static_cast<unsigned>(value)] : "unknown";
    }
    const char* guardPhaseName(GuardPhase value)
    {
        constexpr const char* names[]{"none","preflight","snapshot","publication"};
        return static_cast<unsigned>(value)<std::size(names) ? names[static_cast<unsigned>(value)] : "unknown";
    }
    void recordBuildFailure(BuildStep step, Result& result, std::string_view identity,
        std::uint64_t peak, const Resource::SpeculativeReservationReceipt* cpu) noexcept
    {
        result.failureStep = step;
        const unsigned slot = static_cast<unsigned>(step);
        if (!slot || slot >= static_cast<unsigned>(BuildStep::Count)) return;
        ++Counters::buildFailures[slot];
        const bool firstStage=!Counters::firstBuildFailure[slot].exchange(true);
        bool firstGuard=false;
        if(result.guardRefusal.reason!=GuardRefusal::None)
        {
            const auto reason=static_cast<unsigned>(result.guardRefusal.reason);
            ++Counters::guardRefused[reason];
            firstGuard=!Counters::firstGuardFailure[reason].exchange(true);
        }
        if(!firstStage && !firstGuard) return;
        try
        {
            std::array<char,193> prefix{};
            const auto length=std::min<std::size_t>(192,identity.size());
            for (std::size_t i=0;i<length;++i)
                prefix[i]=static_cast<unsigned char>(identity[i])<32 || identity[i]=='"' ? '?' : identity[i];
            // Fixed FNV identity joins one receipt to the exact bounded cooker
            // key without allocating a full source string during refusal.
            std::uint64_t hash=14695981039346656037ull;
            for (unsigned char c:identity) {hash^=c;hash*=1099511628211ull;}
            std::ostringstream message;
            message << (firstStage ? "OptimizedMW L1 build first refusal: step=" : "OptimizedMW L1 guard first refusal: step=") << buildStepName(step)
                << " identity_hash=" << hash << " identity_prefix=\"" << prefix.data() << '"'
                << " peak_bytes=" << peak << " new_index_bytes=" << result.newIndexBytes
                << " gpu_reason_bits=" << result.gpuRefusalReasons << " retryable=" << result.retryable
                << " allocation_failure=" << result.allocationFailure
                << " cpu_reservation_receipt=" << (cpu!=nullptr)
                << " cpu_reason_bits=" << (cpu ? cpu->reasons : 0)
                << " cpu_requested=" << (cpu ? cpu->requested : 0)
                << " cpu_extra=" << (cpu ? cpu->extra : 0)
                << " guard_schema=1 guard_phase=" << guardPhaseName(result.guardPhase)
                << " guard_reason=" << guardRefusalName(result.guardRefusal.reason)
                << " guard_node_class=\"" << result.guardRefusal.nodeClass.data() << '"'
                << " guard_node_name=\"" << result.guardRefusal.nodeName.data() << '"'
                << " guard_attribute_class=\"" << result.guardRefusal.attributeClass.data() << '"'
                << " guard_program_name=\"" << result.guardRefusal.programName.data() << '"'
                << " guard_texture_unit=" << result.guardRefusal.textureUnit
                << " guard_attribute_type=" << result.guardRefusal.attributeType
                << " guard_shader_refusal=" << result.guardRefusal.shaderRefusal
                << " guard_observed=" << result.guardRefusal.observed << " guard_limit=" << result.guardRefusal.limit;
            Log(Debug::Info) << message.str();
        }
        catch (...) {} // Diagnostics may never turn an ordinary fallback into an exception.
    }
    Refusal certify(const osg::Node& original, RefusalInfo* info)
    {
        ++Counters::requests;
        if (info) *info = {};
        const auto refusal = inspect(original, info);
        if (info && refusal != Refusal::None && info->detail == Detail::None)
            info->detail = refusal == Refusal::Layout ? Detail::Layout : refusal == Refusal::Graph ? Detail::GraphType : Detail::Other;
        if (refusal == Refusal::None) ++Counters::certified;
        else ++Counters::refused[static_cast<unsigned>(refusal)];
        return refusal;
    }
    std::size_t sourceBytes(const osg::Node& original)
    { Size size; return inspectNode(original, size, 0) == Refusal::None ? size.bytes : 0; }
    std::uint64_t cookPeakBytes(const osg::Node& original)
    {
        Size size;
        if (inspectNode(original, size, 0) != Refusal::None || !size.geometries) return 0;
        // Arrays and StateSets remain shared with the immutable control. Deep
        // primitive clones and every earlier tier index array can coexist;
        // account two full uint32 index populations. The per-node allowance
        // covers graph vectors/OSG objects and the small lineage/draw receipts.
        // Source serialization, cache checksum/write payloads and metadata
        // remain charged alongside the largest sequential leaf operation.
        return Resource::DerivedAssets::MetadataScratchBytes + size.graphBytes
            + size.geometries * 8192ull + size.indices * 8ull
            + size.scratch + size.serialized + size.cacheOverlap;
    }
    unsigned conflictingMaterialOwners(std::span<const MaterialOwner> materials, bool known)
    {
        Shader::AssetGeometry::ProducerReadScope producerRead;
        if (!known || materials.size() > 256) return ~0u;
        for (const auto& material : materials)
            if (!material.state || !material.owners || (material.owners & ~((1u << 17) - 1))) return ~0u;
        unsigned conflict = 0;
        for (std::size_t i = 0; i < materials.size(); ++i)
        {
            unsigned owners = materials[i].owners;
            for (std::size_t j = i + 1; j < materials.size(); ++j)
                if (materials[i].state == materials[j].state
                    || materials[i].state->compare(*materials[j].state,true) == 0) owners |= materials[j].owners;
            if (owners & (owners - 1)) conflict |= owners & ~1u;
        }
        return conflict;
    }
    void EffectiveMaterialOwners::observe(const osg::Node& root, unsigned owner, unsigned mask)
    {
        Shader::AssetGeometry::ProducerReadScope producerRead;
        constexpr std::size_t MaximumMaterials = 256, MaximumMetadata = 4 * 1024 * 1024;
        if (!owner || (owner & ~((1u << 17) - 1))) { mUnknown = true; return; }
        if (mUnknown || mNull || mCapacity) return;
        std::vector<const osg::StateSet*> ancestry;
        std::vector<const osg::Node*> branch;
        unsigned nodes = 0;
        try
        {
            const auto stateBytes = [](const osg::StateSet& state)->std::size_t {
                const auto& attributes = state.getAttributeList();
                const auto& textureAttributes = state.getTextureAttributeList();
                const auto& textureModes = state.getTextureModeList();
                if (state.getUpdateCallback() || state.getEventCallback()) return 0;
                if (attributes.size() > 64 || textureAttributes.size() > 32 || textureModes.size() > 32
                    || state.getModeList().size() > 64 || state.getUniformList().size() > 128
                    || state.getDefineList().size() > 64 || state.getBinName().size() > 256) return 0;
                std::size_t entries = attributes.size() + state.getModeList().size()
                    + state.getUniformList().size() + state.getDefineList().size();
                std::size_t strings = state.getBinName().size();
                for (const auto& values : textureAttributes) entries += values.size();
                for (const auto& values : textureModes) entries += values.size();
                if (entries > 256) return 0;
                for (const auto& [key, value] : attributes) if (!value.first) return 0;
                for (const auto& values : textureAttributes)
                    for (const auto& [key, value] : values) if (!value.first) return 0;
                for (const auto& [name, value] : state.getUniformList())
                {
                    if (!value.first || name.size() > 256 || value.first->getNumElements() > 4096) return 0;
                    strings += name.size();
                }
                for (const auto& [name, value] : state.getDefineList())
                {
                    if (name.size() > 256 || value.first.size() > 1024) return 0;
                    strings += name.size() + value.first.size();
                }
                // Map-node/ref-pointer overhead, strings and object/vector
                // allowance; no cloned texture/image/Uniform payload is owned.
                return 1024 + entries * 128 + strings * 2;
            };
            const auto mergeList = [](auto& destination, const auto& incoming, auto flags) {
                for (const auto& [key, value] : incoming)
                {
                    const auto previous = destination.find(key);
                    if (previous == destination.end() || !(flags(previous->second) & osg::StateAttribute::OVERRIDE)
                        || (flags(value) & osg::StateAttribute::PROTECTED)) destination.insert_or_assign(key, value);
                }
            };
            const auto pairFlags = [](const auto& value) { return value.second; };
            const auto modeFlags = [](const auto& value) { return value; };
            const auto visit = [&](const auto& self, const osg::Node& node, unsigned depth)->void {
                if (!(node.getNodeMask() & mask)) return;
                if (++nodes > 4096 || depth > 32 || std::find(branch.begin(), branch.end(), &node) != branch.end())
                { mCapacity = true; return; }
                if (mUnknown || mNull || mCapacity) return;
                branch.push_back(&node);
                if (node.getStateSet()) ancestry.push_back(node.getStateSet());
                if (node.asGeometry())
                {
                    if (ancestry.empty()) mNull = true;
                    else
                    {
                        osg::ref_ptr<osg::StateSet> effective = new osg::StateSet;
                        std::size_t scratch = 1024;
                        for (const auto* state : ancestry)
                        {
                            const auto bytes = stateBytes(*state);
                            if (!bytes || bytes > 256 * 1024 - std::min(scratch, std::size_t(256 * 1024)))
                            { mCapacity = true; break; }
                            scratch += bytes;
                            mergeList(effective->getModeList(), state->getModeList(), modeFlags);
                            mergeList(effective->getAttributeList(), state->getAttributeList(), pairFlags);
                            auto& attributes = effective->getTextureAttributeList();
                            attributes.resize(std::max(attributes.size(), state->getTextureAttributeList().size()));
                            for (unsigned unit = 0; unit < state->getTextureAttributeList().size(); ++unit)
                                mergeList(attributes[unit], state->getTextureAttributeList()[unit], pairFlags);
                            auto& modes = effective->getTextureModeList();
                            modes.resize(std::max(modes.size(), state->getTextureModeList().size()));
                            for (unsigned unit = 0; unit < state->getTextureModeList().size(); ++unit)
                                mergeList(modes[unit], state->getTextureModeList()[unit], modeFlags);
                            mergeList(effective->getUniformList(), state->getUniformList(), pairFlags);
                            mergeList(effective->getDefineList(), state->getDefineList(), pairFlags);
                            const auto incoming = state->getRenderBinMode();
                            const auto previous = effective->getRenderBinMode();
                            if (incoming != osg::StateSet::INHERIT_RENDERBIN_DETAILS
                                && (!(previous & osg::StateSet::OVERRIDE_RENDERBIN_DETAILS)
                                    || (incoming & osg::StateSet::PROTECTED_RENDERBIN_DETAILS)))
                                effective->setRenderBinDetails(state->getBinNumber(), state->getBinName(), incoming);
                            // A false nest policy changes batching scope. Never
                            // discard it while constructing an equivalence key.
                            if (!state->getNestRenderBins()) effective->setNestRenderBins(false);
                        }
                        if (!mCapacity)
                        {
                            const auto bytes = stateBytes(*effective);
                            if (!bytes) mCapacity = true;
                            else
                            {
                                const auto existing = std::find_if(mMaterials.begin(), mMaterials.end(), [&](const auto& value) {
                                    return value.state->compare(*effective, true) == 0;
                                });
                                if (existing != mMaterials.end()) existing->owners |= owner;
                                else if (mMaterials.size() >= MaximumMaterials || bytes > MaximumMetadata - mBytes) mCapacity = true;
                                else
                                {
                                    mStates.push_back(effective);
                                    mMaterials.push_back({effective, owner});
                                    mBytes += bytes;
                                }
                            }
                        }
                    }
                }
                else if (node.asDrawable()) mUnknown = true;
                else if (const auto* group = node.asGroup())
                    for (unsigned i = 0; i < group->getNumChildren() && !mUnknown && !mNull && !mCapacity; ++i)
                        self(self, *group->getChild(i), depth + 1);
                else mUnknown = true;
                if (node.getStateSet()) ancestry.pop_back();
                branch.pop_back();
            };
            visit(visit, root, 0);
        }
        catch (const std::bad_alloc&) { mCapacity = true; }
    }
    void EffectiveMaterialOwners::replaceOwner(unsigned from, unsigned to)
    {
        if (!from || !to || (to & ~((1u << 17) - 1))) { mUnknown = true; return; }
        for (auto& value : mMaterials)
            if (value.owners & from) value.owners = (value.owners & ~from) | to;
    }
    bool EffectiveMaterialOwners::exclusivelyOwns(const osg::Node& root, unsigned owner, unsigned mask) const
    {
        Shader::AssetGeometry::ProducerReadScope producerRead;
        if (mUnknown || mNull || mCapacity || !owner || (owner & (owner - 1))) return false;
        EffectiveMaterialOwners actual;
        actual.observe(root,owner,mask);
        const auto proof = actual.report();
        if (proof.unknown || proof.nullMaterial || proof.capacity || actual.materials().empty()) return false;
        for (const auto& material : actual.materials())
        {
            unsigned owners = 0;
            for (const auto& original : mMaterials)
                if (material.state && original.state
                    && (material.state == original.state || material.state->compare(*original.state,true) == 0))
                    owners |= original.owners;
            if (owners != owner) return false;
        }
        return true;
    }
    MaterialOwnershipReport EffectiveMaterialOwners::report() const
    {
        Shader::AssetGeometry::ProducerReadScope producerRead;
        MaterialOwnershipReport value;
        value.unknown = mUnknown; value.nullMaterial = mNull; value.capacity = mCapacity;
        if (mUnknown || mNull || mCapacity) { value.conflicting = ~0u; return value; }
        for (const auto& material : mMaterials)
            if (material.owners & (material.owners - 1))
            {
                value.conflicting |= material.owners & ~1u;
                if (!value.shared) { value.firstOwners = material.owners & (~material.owners + 1u); value.secondOwners = material.owners & ~value.firstOwners; }
                value.shared = true;
            }
        return value;
    }
    std::vector<OriginalDraw> originalDraws(const osg::Node& root)
    {
        std::vector<OriginalDraw> result;
        std::vector<osg::ref_ptr<const osg::Node>> path;
        unsigned nodes=0;
        const auto visit = [&](const auto& self,const osg::Node& node,osg::Matrixd matrix,unsigned depth)->void {
            if (++nodes>2048 || depth>32 || result.size()>128) throw std::runtime_error("material replay capacity");
            if(!node.getNodeMask()) return;
            path.emplace_back(&node);
            if(const auto* selector=dynamic_cast<const Group*>(&node))
            { if(selector->original()) self(self,*selector->original(),matrix,depth+1); }
            else
            {
                if(const auto* transform=node.asTransform())
                    if(!transform->computeLocalToWorldMatrix(matrix,nullptr)) throw std::runtime_error("material replay transform");
                if(const auto* geometry=dynamic_cast<const osg::Geometry*>(&node))
                    result.push_back({const_cast<osg::Geometry*>(geometry),path,matrix});
                else if(const auto* group=node.asGroup())
                    for(unsigned i=0;i<group->getNumChildren();++i) self(self,*group->getChild(i),matrix,depth+1);
            }
            path.pop_back();
        };
        visit(visit,root,osg::Matrixd::identity(),0);
        return result;
    }
    bool installOriginalDrawReceipts(osg::Node& root)
    {
        const auto paths=originalDraws(root);
        for(const auto& path:paths)
        {
            auto* callback=path.geometry->getDrawCallback();
            if(callback && !dynamic_cast<PersistentOpaquePackets::RetainedOpaqueDrawCallback*>(callback)) return false;
        }
        for(const auto& path:paths)
            if(!dynamic_cast<OriginalReceipt*>(path.geometry->getDrawCallback()))
                path.geometry->setDrawCallback(new OriginalReceipt(path.geometry->getDrawCallback()));
        return !paths.empty();
    }
    void drawOriginals(osg::RenderInfo& info,std::span<const OriginalDraw> draws,
        const osg::Matrixd& derivedMatrix,const osg::StateSet* restoreLeaf,
        std::span<const osg::ref_ptr<const osg::StateSet>> candidateStates)
    {
        auto* state=info.getState(); if(!state) return;
        const auto savedStack=state->getStateSetStack();
        std::size_t depth=savedStack.size();
        for(std::size_t i=0;i<savedStack.size();++i)
            if(std::any_of(candidateStates.begin(),candidateStates.end(),[&](const auto& ss){return ss.get()==savedStack[i];}))
            {depth=i;break;}
        const osg::Matrixd saved=state->getModelViewMatrix();
        struct Restore
        {
            osg::State& state; const osg::State::StateSetStack& stack; std::size_t depth; osg::Matrixd matrix; const osg::StateSet* leaf;
            ~Restore() {state.popStateSetStackToSize(static_cast<unsigned>(depth));for(std::size_t i=depth;i<stack.size();++i) state.pushStateSet(stack[i]);state.applyModelViewMatrix(matrix);state.apply(leaf);}
        } restore{*state,savedStack,depth,saved,restoreLeaf};
        state->popStateSetStackToSize(static_cast<unsigned>(depth));
        for(const auto& draw:draws)
        {
            if(!draw.geometry) continue;
            osg::Matrixd local;
            bool visible=true;
            for(const auto& node:draw.path)
            {
                if(!node || !node->getNodeMask()) {visible=false;break;}
                if(const auto* transform=node->asTransform())
                    if(!transform->computeLocalToWorldMatrix(local,nullptr)) {visible=false;break;}
                if(node->getStateSet()) state->pushStateSet(node->getStateSet());
            }
            if(visible)
            {
                state->applyModelViewMatrix(local*osg::Matrixd::inverse(derivedMatrix)*saved);
                // Remove the directly applied candidate leaf (including atlas
                // textures). Re-resolve the retained original path, then invoke
                // the original callback exactly once through Drawable::draw.
                state->apply(); draw.geometry->draw(info);
            }
            state->popStateSetStackToSize(static_cast<unsigned>(depth));
        }
    }
    struct MaterialGuard::Impl
    {
        Resource::SpeculativeBudget::ChargePtr hostCharge;
        struct Texture
        {
            const osg::Texture2D* object=nullptr;
            const osg::Image* image=nullptr;
            osg::observer_ptr<const osg::Image> imageOwner;
            const unsigned char* data=nullptr;
            unsigned modified=0;
            std::size_t span=0;
            osg::Image::MipmapDataType mips;
            std::array<double,28> recipe{};
            int initialFormat=0,sourceFormat=0;
            static std::array<double,28> values(const osg::Texture2D& t)
            {
                return {double(t.getDataVariance()),double(t.getWrap(osg::Texture::WRAP_S)),double(t.getWrap(osg::Texture::WRAP_T)),
                    double(t.getWrap(osg::Texture::WRAP_R)),double(t.getFilter(osg::Texture::MIN_FILTER)),double(t.getFilter(osg::Texture::MAG_FILTER)),
                    t.getMaxAnisotropy(),t.getMinLOD(),t.getMaxLOD(),t.getLODBias(),double(t.getInternalFormatMode()),
                    double(t.getSourceFormat()),double(t.getSourceType()),double(t.getShadowComparison()),double(t.getShadowCompareFunc()),
                    double(t.getShadowTextureMode()),t.getShadowAmbient(),double(t.getBorderWidth()),
                    t.getBorderColor()[0],t.getBorderColor()[1],t.getBorderColor()[2],t.getBorderColor()[3],
                    double(t.getSwizzle()[0]),double(t.getSwizzle()[1]),double(t.getSwizzle()[2]),double(t.getSwizzle()[3]),
                    double(t.getResizeNonPowerOfTwoHint()),double(t.getUseHardwareMipMapGeneration())};
            }
            std::array<int,10> layout{};
            static std::array<int,10> imageLayout(const osg::Image& i)
            {return {i.s(),i.t(),i.r(),i.getInternalTextureFormat(),int(i.getPixelFormat()),int(i.getDataType()),
                int(i.getPacking()),i.getRowLength(),int(i.getOrigin()),int(i.getDataVariance())};}
            explicit Texture(const osg::Texture2D& t):object(&t),image(t.getImage()),imageOwner(t.getImage())
            {
                if(t.getSubloadCallback() || !image || image->requiresUpdateCall()) throw std::runtime_error("dynamic material texture");
                data=image->data();modified=image->getModifiedCount();span=image->getTotalSizeInBytesIncludingMipmaps();
                mips=image->getMipmapLevels();layout=imageLayout(*image);
                initialFormat=t.getInternalFormat();sourceFormat=image->getInternalTextureFormat()?image->getInternalTextureFormat():int(image->getPixelFormat());
                recipe=values(t); // Resolve the logical CPU format before freezing its recipe.
            }
            bool current() const
            {
                const auto* actual=object->getImage();
                osg::ref_ptr<const osg::Image> locked;
                if(!imageOwner.lock(locked) || locked.get()!=actual) return false;
                const bool format=object->getInternalFormat()==initialFormat || (initialFormat==0
                    && object->getInternalFormatMode()==osg::Texture::USE_IMAGE_DATA_FORMAT && object->getInternalFormat()==sourceFormat);
                return format && !object->getSubloadCallback() && values(*object)==recipe && actual==image && actual && !actual->requiresUpdateCall()
                    && actual->data()==data && actual->getModifiedCount()==modified && actual->getTotalSizeInBytesIncludingMipmaps()==span
                    && actual->getMipmapLevels()==mips && imageLayout(*actual)==layout;
            }
        };
        struct Node
        {
            osg::observer_ptr<const osg::Node> owner;
            osg::ref_ptr<osg::StateSet> frozen;
            const osg::StateSet* state=nullptr;
            unsigned mask=0;
            osg::Object::DataVariance variance{};
            const osg::Callback *update=nullptr,*event=nullptr,*cull=nullptr;
            const osg::Drawable::DrawCallback* draw=nullptr;
            std::vector<const osg::Node*> children;
            struct Stream
            {
                const osg::BufferData* identity=nullptr;
                osg::observer_ptr<const osg::BufferData> owner;
                unsigned modified=0;
                std::array<std::uint64_t,6> layout{};
                static std::array<std::uint64_t,6> values(const osg::BufferData* data)
                {
                    if(const auto* array=dynamic_cast<const osg::Array*>(data))
                        return {array->getNumElements(),array->getTotalDataSize(),std::uint64_t(array->getType()),
                            std::uint64_t(array->getBinding()),array->getNormalize(),std::uint64_t(array->getDataVariance())};
                    if(const auto* primitive=dynamic_cast<const osg::PrimitiveSet*>(data))
                        return {primitive->getNumIndices(),primitive->getTotalDataSize(),std::uint64_t(primitive->getType()),
                            primitive->getMode(),std::uint64_t(primitive->getNumInstances()),std::uint64_t(primitive->getDataVariance())};
                    return {};
                }
                explicit Stream(const osg::BufferData* data):identity(data),owner(data),modified(data?data->getModifiedCount():0),layout(values(data)) {}
                bool current(const osg::BufferData* data) const
                {
                    if(!identity) return !data;
                    osg::ref_ptr<const osg::BufferData> locked;
                    return data==identity && owner.lock(locked) && locked.get()==data && data->getModifiedCount()==modified && values(data)==layout;
                }
            };
            std::vector<Stream> streams;
            std::vector<Texture> textures;
            osg::Matrixd matrix;
            bool transform=false;
            explicit Node(const osg::Node& node):owner(&node),state(node.getStateSet()),mask(node.getNodeMask()),variance(node.getDataVariance()),
                update(node.getUpdateCallback()),event(node.getEventCallback()),cull(node.getCullCallback())
            {
                if(const auto* t=node.asTransform()) {transform=true;if(!t->computeLocalToWorldMatrix(matrix,nullptr)) throw std::runtime_error("material transform");}
                if(const auto* g=node.asGroup()) for(unsigned i=0;i<g->getNumChildren();++i) children.push_back(g->getChild(i));
                if(const auto* g=dynamic_cast<const osg::Geometry*>(&node))
                {
                    draw=g->getDrawCallback();
                    const auto add=[&](const osg::BufferData* data){streams.emplace_back(data);};
                    add(g->getVertexArray());add(g->getNormalArray());add(g->getColorArray());
                    add(g->getSecondaryColorArray());add(g->getFogCoordArray());
                    for(unsigned i=0;i<g->getNumTexCoordArrays();++i) add(g->getTexCoordArray(i));
                    for(unsigned i=0;i<g->getNumVertexAttribArrays();++i) add(g->getVertexAttribArray(i));
                    for(unsigned i=0;i<g->getNumPrimitiveSets();++i) add(g->getPrimitiveSet(i));
                }
                if(!state) return;
                frozen=new osg::StateSet(*state,osg::CopyOp::SHALLOW_COPY);
                for(const auto& [key,pair]:state->getAttributeList())
                    if(key.first!=osg::StateAttribute::PROGRAM)
                        frozen->setAttribute(dynamic_cast<osg::StateAttribute*>(pair.first->clone(osg::CopyOp::SHALLOW_COPY)),pair.second);
                for(const auto& [name,pair]:state->getUniformList())
                    frozen->addUniform(new osg::Uniform(*pair.first,osg::CopyOp::DEEP_COPY_ALL),pair.second);
                for(unsigned unit=0;unit<state->getTextureAttributeList().size();++unit)
                    for(const auto& [key,pair]:state->getTextureAttributeList()[unit])
                        if(key.first==osg::StateAttribute::TEXTURE)
                        {
                            const auto* t=dynamic_cast<const osg::Texture2D*>(pair.first.get());
                            if(!t) throw std::runtime_error("material texture class");
                            textures.emplace_back(*t);
                        }
                        else frozen->setTextureAttribute(unit,dynamic_cast<osg::StateAttribute*>(pair.first->clone(osg::CopyOp::SHALLOW_COPY)),pair.second);
            }
            bool current() const
            {
                osg::ref_ptr<const osg::Node> node; if(!owner.lock(node)) return false;
                if(node->getStateSet()!=state || node->getNodeMask()!=mask || node->getDataVariance()!=variance
                    || node->getUpdateCallback()!=update || node->getEventCallback()!=event || node->getCullCallback()!=cull) return false;
                if(transform) {osg::Matrixd value;if(!node->asTransform()->computeLocalToWorldMatrix(value,nullptr) || value!=matrix) return false;}
                if(const auto* g=node->asGroup())
                {if(g->getNumChildren()!=children.size()) return false;for(unsigned i=0;i<children.size();++i) if(g->getChild(i)!=children[i]) return false;}
                if(const auto* g=dynamic_cast<const osg::Geometry*>(node.get()))
                {
                    if(g->getDrawCallback()!=draw) return false;
                    std::size_t i=0;
                    const auto equal=[&](const osg::BufferData* data){return i<streams.size() && streams[i++].current(data);};
                    if(!equal(g->getVertexArray()) || !equal(g->getNormalArray()) || !equal(g->getColorArray()) || !equal(g->getSecondaryColorArray()) || !equal(g->getFogCoordArray())) return false;
                    for(unsigned u=0;u<g->getNumTexCoordArrays();++u) if(!equal(g->getTexCoordArray(u))) return false;
                    for(unsigned u=0;u<g->getNumVertexAttribArrays();++u) if(!equal(g->getVertexAttribArray(u))) return false;
                    for(unsigned u=0;u<g->getNumPrimitiveSets();++u) if(!equal(g->getPrimitiveSet(u))) return false;
                    if(i!=streams.size()) return false;
                }
                if(!state) return true;
                if(state->getDataVariance()!=frozen->getDataVariance() || state->getUpdateCallback()!=frozen->getUpdateCallback()
                    || state->getEventCallback()!=frozen->getEventCallback() || state->getModeList()!=frozen->getModeList()
                    || state->getTextureModeList()!=frozen->getTextureModeList() || state->getDefineList()!=frozen->getDefineList()
                    || state->getRenderingHint()!=frozen->getRenderingHint() || state->getBinNumber()!=frozen->getBinNumber()
                    || state->getBinName()!=frozen->getBinName() || state->getRenderBinMode()!=frozen->getRenderBinMode()) return false;
                const auto attributes=[](const auto& a,const auto& b)->bool {
                    if(a.size()!=b.size()) return false;
                    auto j=b.begin();for(const auto& [key,pair]:a)
                    {
                        if(j->first!=key || j->second.second!=pair.second) return false;
                        if(key.first==osg::StateAttribute::PROGRAM || key.first==osg::StateAttribute::TEXTURE)
                        {if(pair.first!=j->second.first) return false;}
                        else if(pair.first->compare(*j->second.first)!=0) return false;
                        ++j;
                    }
                    return true;
                };
                if(!attributes(state->getAttributeList(),frozen->getAttributeList()) || state->getTextureAttributeList().size()!=frozen->getTextureAttributeList().size()) return false;
                for(unsigned u=0;u<state->getTextureAttributeList().size();++u)
                    if(!attributes(state->getTextureAttributeList()[u],frozen->getTextureAttributeList()[u])) return false;
                if(state->getUniformList().size()!=frozen->getUniformList().size()) return false;
                auto uniform=frozen->getUniformList().begin();for(const auto& [name,pair]:state->getUniformList())
                {
                    if(name!=uniform->first || pair.second!=uniform->second.second || pair.first->getUpdateCallback()!=uniform->second.first->getUpdateCallback()
                        || pair.first->getEventCallback()!=uniform->second.first->getEventCallback() || pair.first->compare(*uniform->second.first)!=0) return false;
                    ++uniform;
                }
                return std::all_of(textures.begin(),textures.end(),[](const auto& texture){return texture.current();});
            }
        };
        struct Program
        {
            osg::ref_ptr<const osg::Program> owner;
            Shader::AssetGeometry::Snapshot snapshot;
            std::vector<osg::observer_ptr<const osg::Node>> path;
            std::vector<std::string> sources;
            std::string imports;
        };
        std::vector<Node> nodes;
        std::vector<Program> programs;
    };
    std::shared_ptr<MaterialGuard> MaterialGuard::captureDraws(std::span<const OriginalDraw> original,
        const OriginalDraw& derived,bool requireShader,GuardRefusalInfo* refusal)
    {
        // Non-certified HLOD recipes also read mutable Program/Shader source
        // and import lists. Hold the producer's read lease for one coherent
        // snapshot, including nested AssetGeometry certification calls.
        Shader::AssetGeometry::ProducerReadScope producerRead;
        GuardRefusalInfo local;
        auto& info=refusal ? *refusal : local;
        info={};
        const auto fail=[&](GuardRefusal reason,std::uint64_t observed=0,std::uint64_t limit=0)->std::shared_ptr<MaterialGuard>
        {info.reason=reason;info.observed=observed;info.limit=limit;return {};};
        const auto nodeContext=[&](const osg::Node* node) {
            guardText(info.nodeClass,node ? node->className() : "");
            guardText(info.nodeName,node ? std::string_view(node->getName()) : std::string_view{});
            info.attributeClass.fill(0);info.programName.fill(0);
            info.textureUnit=info.attributeType=~0u;info.shaderRefusal=0;
        };
        const auto attributeContext=[&](const osg::StateAttribute* attribute,unsigned type,unsigned unit=~0u) {
            guardText(info.attributeClass,attribute ? attribute->className() : "");
            info.attributeType=type;info.textureUnit=unit;
        };
        try
        {
        if(original.size()>128)
            return fail(GuardRefusal::PathCapacity,original.size(),128);
        if(derived.path.size()>32)
            return fail(GuardRefusal::PathCapacity,derived.path.size(),32);
        for(const auto& draw:original) if(draw.path.size()>32)
            return fail(GuardRefusal::PathCapacity,draw.path.size(),32);
        Resource::SpeculativeScope::Stage preflight(256*1024,0);
        std::vector<OriginalDraw> paths(original.begin(),original.end());paths.push_back(derived);
        std::set<const osg::Node*> unique;
        std::uint64_t estimate=4096;
        for(const auto& draw:paths)
        {
            if(draw.path.size()>32 || paths.size()>129) return fail(GuardRefusal::PathCapacity,draw.path.size(),32);
            for(const auto& node:draw.path) if(unique.insert(node.get()).second)
            {
                nodeContext(node.get());
                if(!node) return fail(GuardRefusal::NullNode);
                estimate+=4096;
                if(const auto* group=node->asGroup())
                {if(group->getNumChildren()>1024) return fail(GuardRefusal::ChildCapacity,group->getNumChildren(),1024);estimate+=group->getNumChildren()*32ull;}
                if(const auto* transform=node->asTransform())
                {osg::Matrixd matrix;if(!transform->computeLocalToWorldMatrix(matrix,nullptr)) return fail(GuardRefusal::TransformRecipe);}
                if(const auto* geometry=dynamic_cast<const osg::Geometry*>(node.get()))
                {
                    const auto streams=5ull+geometry->getNumTexCoordArrays()+geometry->getNumVertexAttribArrays()+geometry->getNumPrimitiveSets();
                    if(streams>64) return fail(GuardRefusal::StreamCapacity,streams,64);estimate+=streams*512;
                }
                if(const auto* ss=node->getStateSet())
                {
                    if(ss->getName().size()>4096 || ss->getUniformList().size()>128 || ss->getAttributeList().size()>64 || ss->getTextureAttributeList().size()>32 || ss->getDefineList().size()>128)
                        return fail(GuardRefusal::StateCapacity);
                    for(const auto& [name,pair]:ss->getUniformList())
                    {if(!pair.first || typeid(*pair.first)!=typeid(osg::Uniform) || pair.first->getUpdateCallback() || pair.first->getEventCallback()
                        || pair.first->getNumElements()>4096 || name.size()>1024) return fail(GuardRefusal::UniformRecipe);estimate+=1024+pair.first->getNumElements()*256ull;}
                    for(const auto& [name,pair]:ss->getDefineList())
                    {if(name.size()>1024 || pair.first.size()>4096) return fail(GuardRefusal::DefineCapacity);estimate+=name.size()+pair.first.size()+64;}
                    for(const auto& [key,pair]:ss->getAttributeList())
                    {
                        attributeContext(pair.first.get(),key.first);
                        if(!pair.first || pair.first->getName().size()>4096 || (key.first!=osg::StateAttribute::PROGRAM && !boundedAttribute(*pair.first)))
                            return fail(GuardRefusal::AttributeRecipe);
                        estimate+=1024+pair.first->getName().size()*2ull;
                        if(!requireShader && key.first==osg::StateAttribute::PROGRAM)
                        {
                            const auto* program=dynamic_cast<const osg::Program*>(pair.first.get());if(!program || program->getNumShaders()>32) return fail(GuardRefusal::ShaderMissing);
                            for(unsigned i=0;i<program->getNumShaders();++i)
                            {const auto size=program->getShader(i)->getShaderSource().size();if(size>64*1024) return fail(GuardRefusal::MetadataCapacity,size,64*1024);estimate+=size*2ull;}
                        }
                    }
                    unsigned textureAttributes=0;
                    unsigned unitIndex=0;
                    for(const auto& unit:ss->getTextureAttributeList())
                    {
                        if(unit.size()>16) return fail(GuardRefusal::TextureCapacity,unit.size(),16);
                        for(const auto& [key,pair]:unit)
                        {
                            attributeContext(pair.first.get(),key.first,unitIndex);
                            if(!pair.first) return fail(GuardRefusal::AttributeRecipe);
                            if(++textureAttributes>128 || pair.first->getName().size()>4096) return fail(GuardRefusal::TextureCapacity,textureAttributes,128);estimate+=1024+pair.first->getName().size()*2ull;
                            if(key.first==osg::StateAttribute::TEXTURE)
                            {
                                const auto* texture=dynamic_cast<const osg::Texture2D*>(pair.first.get());
                                if(!texture || texture->getNumImages()!=1) return fail(GuardRefusal::TextureClass);
                                if(!texture->getImage()) return fail(GuardRefusal::TextureImage);
                                if(texture->getImage()->getMipmapLevels().size()>32) return fail(GuardRefusal::TextureMips,texture->getImage()->getMipmapLevels().size(),32);
                                if(texture->getInternalFormatMode()!=osg::Texture::USE_IMAGE_DATA_FORMAT && texture->getInternalFormatMode()!=osg::Texture::USE_USER_DEFINED_FORMAT)
                                    return fail(GuardRefusal::TextureFormat,texture->getInternalFormatMode());
                                if(texture->getSubloadCallback() || texture->getImage()->requiresUpdateCall()) return fail(GuardRefusal::TextureDynamic);
                                estimate+=texture->getImage()->getMipmapLevels().size()*16ull;
                            }
                            else if(!boundedAttribute(*pair.first)) return fail(GuardRefusal::AttributeRecipe);
                        }
                        ++unitIndex;
                    }
                }
                if(unique.size()>2048 || estimate>4*Resource::SpeculativeBudget::MiB) return fail(GuardRefusal::MetadataCapacity,estimate,4*Resource::SpeculativeBudget::MiB);
            }
        }
        if(unique.size()>2048 || estimate>4*Resource::SpeculativeBudget::MiB) return fail(GuardRefusal::MetadataCapacity,estimate,4*Resource::SpeculativeBudget::MiB);
        Resource::SpeculativeScope::Stage admission(estimate,0);
        auto data=std::make_shared<Impl>();data->nodes.reserve(unique.size());
        for(const auto* node:unique) {nodeContext(node);data->nodes.emplace_back(*node);}
        for(const auto& draw:paths)
        {
            std::vector<const osg::StateSet*> states;
            for(const auto& node:draw.path) if(node->getStateSet()) states.push_back(node->getStateSet());
            // Resolve OVERRIDE/PROTECTED exactly as OSG does, rather than
            // trusting a root Program while an authored child supplies another.
            const auto* program=effectiveProgram(states);
            nodeContext(draw.geometry.get());
            if(!program) {if(requireShader) return fail(GuardRefusal::ShaderMissing);continue;}
            guardText(info.programName,program->getName());
            const auto imports=Shader::ProgramDefineSnapshot::fromStateSets(*program,states);
            if(!imports.bounded) return fail(GuardRefusal::ShaderImports);
            if(std::any_of(data->programs.begin(),data->programs.end(),[&](const auto& p){return p.owner==program && p.imports==imports.string;})) continue;
            Impl::Program entry;entry.owner=program;entry.imports=imports.string;
            for(const auto& node:draw.path) entry.path.emplace_back(node.get());
            if(requireShader)
            {Shader::AssetGeometry::Refusal reason=Shader::AssetGeometry::Refusal::None;
                auto snapshot=Shader::AssetGeometry::capture(*program,states,&reason);
                if(!snapshot) {info.shaderRefusal=static_cast<unsigned>(reason);return fail(GuardRefusal::ShaderCertificate);}entry.snapshot=*snapshot;}
            else for(unsigned i=0;i<program->getNumShaders();++i) entry.sources.push_back(program->getShader(i)->getShaderSource());
            data->programs.push_back(std::move(entry));
        }
        const auto context=Resource::SpeculativeScope::capture();
        if(context.budget) data->hostCharge=Resource::SpeculativeScope::track(context.budget,{data.get(),3u},estimate,false);
        info={};
        return std::shared_ptr<MaterialGuard>(new MaterialGuard(std::move(data)));
        }
        catch(const Resource::SpeculativeDeferred&) {throw;}
        catch(const std::bad_alloc&) {info.reason=GuardRefusal::SnapshotAllocation;throw;}
        catch(const std::exception&) {return fail(GuardRefusal::SnapshotException);}
    }
    std::shared_ptr<MaterialGuard> MaterialGuard::capture(const osg::Node& original,const osg::Node& derived,
        bool requireShader,GuardRefusalInfo* refusal)
    {
        if(refusal) *refusal={};
        auto paths=originalDraws(original);auto tiers=originalDraws(derived);
        if(paths.empty() || tiers.empty() || paths.size()+tiers.size()>128)
        {if(refusal) {refusal->reason=GuardRefusal::PathCapacity;refusal->observed=paths.size()+tiers.size();refusal->limit=128;}return {};}
        const auto last=tiers.back();tiers.pop_back();paths.insert(paths.end(),tiers.begin(),tiers.end());
        return captureDraws(paths,last,requireShader,refusal);
    }
    bool MaterialGuard::preflight(const osg::Node& original,GuardRefusalInfo* refusal)
    {
        // Original-to-original invokes the exact final recipe contract before
        // any simplification/clone allocation. This temporary snapshot is not
        // retained and never substitutes for the final derived snapshot.
        return bool(capture(original,original,true,refusal));
    }
    bool MaterialGuard::current(osg::State* state,const std::vector<const osg::StateSet*>& inherited) const
    {
        Shader::AssetGeometry::ProducerReadScope producerRead;
        if(!mImpl) return false;
        try
        {
            for(const auto& node:mImpl->nodes) if(!node.current()) return false;
            for(const auto& entry:mImpl->programs)
            {
                std::vector<const osg::StateSet*> path=inherited;
                if(state && path.empty()) for(const auto* ss:state->getStateSetStack()) if(ss) path.push_back(ss);
                for(const auto& weak:entry.path)
                {osg::ref_ptr<const osg::Node> node;if(!weak.lock(node)) return false;if(node->getStateSet()) path.push_back(node->getStateSet());}
                if(effectiveProgram(path)!=entry.owner.get()) return false;
                if(entry.snapshot)
                {if(Shader::AssetGeometry::current(entry.snapshot,*entry.owner,path)!=Shader::AssetGeometry::Refusal::None) return false;}
                else
                {
                    if(entry.owner->getNumShaders()!=entry.sources.size()) return false;
                    for(unsigned i=0;i<entry.sources.size();++i) if(entry.owner->getShader(i)->getShaderSource()!=entry.sources[i]) return false;
                }
            }
            return true;
        }
        catch(const std::bad_alloc&) {return false;}
    }
    bool CompileCompleted::compileCompleted(osgUtil::IncrementalCompileOperation::CompileSet* compiled)
    {
        // ICO completion means every compile operation ran; it does not mean a
        // Program linked successfully. Inspect only CPU status published by that
        // completion, never issue GL commands from the update/merge callback.
        bool ready = compiled && compiled->compiled() && compiled->_subgraphToCompile
            && !compiled->_compileMap.empty();
        struct Programs final : osg::NodeVisitor
        {
            Programs() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
            std::vector<osg::ref_ptr<osg::Program>> values;
            std::size_t nodes = 0;
            bool valid = true;
            void apply(osg::Node& node) override
            {
                if (++nodes > 65536 || values.size() > 256) { valid = false; return; }
                if (auto* state = node.getStateSet())
                    if (auto* program = dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM));
                        program && std::find(values.begin(),values.end(),program) == values.end()) values.emplace_back(program);
                traverse(node);
            }
        } programs;
        if (ready) compiled->_subgraphToCompile->accept(programs);
        ready = ready && programs.valid && !programs.values.empty();
        if (ready)
            for (const auto& [context,list] : compiled->_compileMap)
            {
                if (!context || !context->getState()) { ready = false; break; }
                for (const auto& program : programs.values)
                {
                    const auto* status = program->getPCP(*context->getState());
                    if (!status || !status->isLinked() || status->needsLink()) { ready = false; break; }
                }
                if (!ready) break;
            }
        mReadiness->ready.store(ready, std::memory_order_release);
        if (ready)
        {
            mReadiness->everReady.store(true, std::memory_order_release);
            AssetPaging::each(mReadiness->assetMask, [](AssetPaging::LaneCounters& value) { ++value.ready; });
        }
        return false;
    }
    Group::Group(const Group& copy, const osg::CopyOp& op)
        : osg::Group(copy, op), mError(copy.mError), mReadiness(std::make_shared<Readiness>()), mMainCamera(copy.mMainCamera)
        , mPreserveControlPopulation(copy.mPreserveControlPopulation) {}
    Group::Group(osg::Node& originalNode, osg::Node& derivedNode, float absoluteError,
        std::shared_ptr<Readiness> readiness, osg::Camera* mainCamera, bool preserveControlPopulation,
        GuardRefusalInfo* refusal)
        : mError(absoluteError), mReadiness(std::move(readiness)), mMainCamera(mainCamera)
        , mPreserveControlPopulation(preserveControlPopulation)
    {
        setNodeMask(originalNode.getNodeMask()); setDataVariance(osg::Object::DYNAMIC);
        addChild(&originalNode); addChild(&derivedNode);
        setName("OptimizedMW-generated-static-LOD");
        mMaterialGuard=MaterialGuard::capture(originalNode,derivedNode,true,refusal);
    }
    osg::BoundingSphere Group::computeBound() const
    {
        osg::BoundingSphere bound = original() ? original()->getBound() : osg::BoundingSphere{};
        if (derived()) bound.expandBy(derived()->getBound());
        return bound;
    }
    void Group::releaseGLObjects(osg::State* state) const
    {
        if (mReadiness) mReadiness->ready.store(false, std::memory_order_release);
        osg::Group::releaseGLObjects(state);
    }
    bool Group::wantsDerived(osgUtil::CullVisitor& visitor)
    {
        const auto* camera = visitor.getCurrentCamera();
        const auto* viewport = visitor.getViewport();
        if (!camera || camera != mMainCamera.get() || ShadowTraversalScope::active()
            || !viewport || viewport->height() <= 0 || !mReadiness || !mReadiness->ready.load(std::memory_order_acquire)
            || !visitor.getModelViewMatrix() || !visitor.getProjectionMatrix()) return false;
        const auto& projection = *visitor.getProjectionMatrix();
        if (std::abs(projection(2,3) + 1) > 1e-5 || std::abs(projection(3,3)) > 1e-5
            || projection(0,1) != 0 || projection(1,0) != 0) return false;
        std::vector<const osg::StateSet*> ancestry;
        for(auto* graph=visitor.getCurrentStateGraph();graph;graph=graph->_parent)
            if(graph->getStateSet()) ancestry.push_back(graph->getStateSet());
        std::reverse(ancestry.begin(),ancestry.end());
        const auto* clip=effectiveClipControl(ancestry);
        for (auto* graph = visitor.getCurrentStateGraph(); graph; graph = graph->_parent)
        {
            const auto* state = graph->getStateSet();
            if (!material(state, true)) return false;
            if (state)
                if (const auto* uniform = state->getUniform("projectionMatrix"))
                {
                    osg::Matrixf value;
                    if (!uniform->get(value)) return false;
                    if(!projectionMatches(projection,value,clip)) return false;
                }
        }
        if(!mMaterialGuard || !mMaterialGuard->current(nullptr,ancestry))
        {++Counters::materialCullFallbacks;return false;}
        const auto& matrix = *visitor.getModelViewMatrix();
        if (!positiveUniformTransform(matrix)) return false;
        if (mPreserveControlPopulation)
        {
            // HLOD may combine several independently culled control draws. Only
            // replace the complete qualified population while every member
            // would submit in this current view. Retained transforms/unknown
            // structure conservatively fall back; ordinary strong optimization
            // normally flattened these bounded material subtrees already.
            unsigned nodes = 0, leaves = 0;
            const auto visible = [&](const auto& self, const osg::Node& node, unsigned depth) -> bool {
                if (++nodes > 1024 || depth > 32 || !visitor.validNodeMask(node)) return false;
                if (const auto* selector = dynamic_cast<const Group*>(&node))
                    return selector->original() && self(self,*selector->original(),depth + 1);
                if (node.asTransform()) return false;
                visitor.pushCurrentMask();
                const auto* geometry = dynamic_cast<const osg::Geometry*>(&node);
                const bool culled = node.isCullingActive()
                    && (geometry ? visitor.isCulled(geometry->getBoundingBox()) : visitor.isCulled(node));
                visitor.popCurrentMask();
                if (culled) return false;
                if (geometry) return ++leaves <= 64;
                const auto* group = node.asGroup();
                if (!group || (typeid(node) != typeid(osg::Group) && typeid(node) != typeid(osg::Geode)
                    && typeid(node) != typeid(PersistentOpaquePackets::Group))) return false;
                for (unsigned i = 0; i < group->getNumChildren(); ++i)
                    if (!self(self,*group->getChild(i),depth + 1)) return false;
                return true;
            };
            if (!original() || !visible(visible,*original(),0) || !leaves) return false;
        }
        const auto bound = getBound();
        const osg::Vec3d center = osg::Vec3d(bound.center()) * matrix;
        const double scale = osg::Vec3d(matrix(0,0),matrix(0,1),matrix(0,2)).length();
        const double distance = -center.z() - bound.radius() * scale;
        std::unique_lock lock(mViewMutex, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        auto view = std::find_if(mViews.begin(), mViews.end(), [&](const View& v) { return v.owner == &visitor; });
        if (view == mViews.end()) view = std::find_if(mViews.begin(), mViews.end(), [](const View& v) { return !v.owner; });
        if (view == mViews.end()) return false;
        view->owner = &visitor;
        if (!std::isfinite(distance) || distance <= 0) { view->coarse = false; return false; }
        const double pixels = mError * scale * std::abs(projection(1,1)) * viewport->height() * .5 / distance;
        if (!std::isfinite(pixels) || pixels > MaximumPixels) view->coarse = false;
        else if (pixels <= MaximumPixels * .8) view->coarse = true;
        return view->coarse;
    }
    void Group::traverse(osg::NodeVisitor& visitor)
    {
        if (auto* cull = visitor.asCullVisitor())
        {
            const bool coarse = wantsDerived(*cull);
            if (coarse) ++Counters::coarseViews; else ++Counters::originalViews;
            if (auto* child = coarse ? derived() : original(); child && visitor.validNodeMask(*child)) child->accept(visitor);
        }
        else if (visitor.getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR || visitor.getVisitorType() == osg::NodeVisitor::INTERSECTION_VISITOR)
        { if (original()) original()->accept(visitor); }
        else osg::Group::traverse(visitor); // compile both immutable representations before activation
    }
    Result build(osg::Node& original, std::string_view identity, osg::Camera* mainCamera, bool reserveGpu)
    {
        Result result;
        BuildStep step=BuildStep::Scope;
        std::uint64_t peak=0;
        const auto fail=[&](Refusal reason,bool retryable=false,const Resource::SpeculativeReservationReceipt* cpu=nullptr) {
            result.refusal=reason; result.retryable=retryable; result.node=nullptr; result.readiness.reset();
            result.gpuReservation.reset(); // No ICO/publication occurred on a refused result.
            ++Counters::refused[static_cast<unsigned>(reason)];
            recordBuildFailure(step,result,identity,peak,cpu);
        };
        try
        {
            if (reserveGpu && (!PagingWorkScope::optionalOptimization() || !Resource::SpeculativeScope::active()))
            { fail(Refusal::Capacity); return result; }
            step=BuildStep::Inspection;
            Size inspected;
            if (auto r=inspectNode(original,inspected,0);r!=Refusal::None) {fail(r);return result;}
            struct Originals final : osg::NodeVisitor
            {
                Originals() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
                std::vector<osg::Geometry*> geometries;
                void apply(osg::Geometry& geometry) override { geometries.push_back(&geometry); }
            } originals;
            original.accept(originals);
            step=BuildStep::VertexOwnership;
            if (reserveGpu)
                for (const auto* geometry:originals.geometries)
                {
                    // Switching a display-list/client-array control to VBOs
                    // would make the tier's first vertex upload additional work.
                    // Admit index-only debt only when its exact ordinary control
                    // already owns this immutable VBO representation.
                    const auto hasVbo=[](const osg::Array* array) {return !array || array->getVertexBufferObject()!=nullptr;};
                    bool buffers=geometry->getUseVertexBufferObjects() && !geometry->getUseDisplayList();
                    for (const auto* array:{geometry->getVertexArray(),geometry->getNormalArray(),geometry->getColorArray()})
                        buffers=buffers && hasVbo(array);
                    for (unsigned unit=0;unit<geometry->getNumTexCoordArrays();++unit)
                        buffers=buffers && hasVbo(geometry->getTexCoordArray(unit));
                    if (!buffers) {fail(Refusal::Graph);return result;}
                }
            step=BuildStep::Guard;result.guardPhase=GuardPhase::Preflight;
            if(!MaterialGuard::preflight(original,&result.guardRefusal))
            {fail(Refusal::Material);return result;}
            result.guardPhase=GuardPhase::None;
            step=BuildStep::Peak;
            const auto graphPeak=cookPeakBytes(original);
            peak=graphPeak+identity.size()*2ull;
            if (!graphPeak || peak>96*Resource::SpeculativeBudget::MiB || identity.size()>256*1024)
            {fail(Refusal::Capacity);return result;}
            // This is the complete actual graph/primitive/leaf cook overlap;
            // it remains additive to the caller's admitted ordinary control.
            step=BuildStep::HostAdmission;
            Resource::SpeculativeScope::Stage reservation(peak,0);
            step=BuildStep::Cook;
            osg::ref_ptr<osg::Node> derived=osg::clone(&original,
                osg::CopyOp::DEEP_COPY_NODES|osg::CopyOp::DEEP_COPY_DRAWABLES|osg::CopyOp::DEEP_COPY_PRIMITIVES);
            Cook cook(identity); derived->accept(cook);
            result.originalIndices=cook.before;result.derivedIndices=cook.after;result.newIndexBytes=cook.newIndexBytes;
            if (!std::isfinite(cook.error)) throw std::runtime_error("LOD error scale invalid");
            step=BuildStep::Reduction;
            if (!cook.changed) {fail(Refusal::NoReduction);return result;}
            step=BuildStep::GpuAdmission;
            if (reserveGpu)
            {
                Resource::AssetGpuBudget::ReservationReceipt gpu;
                result.gpuReservation=Resource::AssetGpuBudget::reserve(cook.newIndexBytes,
                    Resource::AssetPolicy::TextureAllocationLimit,&gpu);
                result.gpuRefusalReasons=gpu.reasons;
                if (!result.gpuReservation) {fail(Refusal::Capacity,true);return result;}
            }
            step=BuildStep::Lineage;
            if (originals.geometries.size()!=cook.geometries.size()) throw std::runtime_error("LOD lineage mismatch");
            struct Rollback
            {
                struct Saved
                {
                    osg::Geometry* geometry;
                    osg::ref_ptr<osg::Drawable::DrawCallback> callback;
                    osg::ref_ptr<osg::UserDataContainer> data;
                };
                std::vector<Saved> values;
                bool committed=false;
                ~Rollback()
                {if (!committed) for (auto& saved:values) {saved.geometry->setDrawCallback(saved.callback);saved.geometry->setUserDataContainer(saved.data);}}
            } rollback;
            rollback.values.reserve(originals.geometries.size());
            for (std::size_t i=0;i<originals.geometries.size();++i)
            {
                auto* source=originals.geometries[i];auto* tier=cook.geometries[i];
                const auto same=[](const osg::Array* a,const osg::Array* b) {
                    return a==b && (!a || a->getVertexBufferObject()==b->getVertexBufferObject());
                };
                if (!same(source->getVertexArray(),tier->getVertexArray())
                    || !same(source->getNormalArray(),tier->getNormalArray())
                    || !same(source->getColorArray(),tier->getColorArray())
                    || source->getNumTexCoordArrays()!=tier->getNumTexCoordArrays())
                    throw std::runtime_error("LOD immutable vertex buffer lineage changed");
                for (unsigned unit=0;unit<source->getNumTexCoordArrays();++unit)
                    if (!same(source->getTexCoordArray(unit),tier->getTexCoordArray(unit)))
                        throw std::runtime_error("LOD immutable UV/tangent buffer lineage changed");
                // A cloned unchanged primitive is still another BufferData
                // entry. Reuse the actual immutable original primitive/EBO.
                if (!cook.reducedGeometry[i]) tier->setPrimitiveSet(0,source->getPrimitiveSet(0));
                rollback.values.push_back({source,source->getDrawCallback(),source->getUserDataContainer()});
                osg::ref_ptr<TemporalVertexLineage> lineage=new TemporalVertexLineage(source->getVertexArray());
                installTemporalVertexLineage(*source,lineage);installTemporalVertexLineage(*tier,lineage);
            }
            step=BuildStep::Replay;
            const auto sourcePaths=originalDraws(original),tierPaths=originalDraws(*derived);
            if (sourcePaths.size()!=tierPaths.size()) throw std::runtime_error("LOD material replay lineage mismatch");
            if (!installOriginalDrawReceipts(original)) throw std::runtime_error("LOD original callback replay unavailable");
            step=BuildStep::Guard;result.guardPhase=GuardPhase::Snapshot;
            for (std::size_t i=0;i<tierPaths.size();++i)
            {
                auto* tier=tierPaths[i].geometry.get();
                osg::ref_ptr<DrawReceipt> callback=new DrawReceipt(tier->getPrimitiveSet(0)->getNumIndices());
                callback->owner=tier;tier->setDrawCallback(callback);
                callback->originals.push_back(sourcePaths[i]);callback->derivedMatrix=tierPaths[i].matrix;
                for (const auto& node:tierPaths[i].path) if (node->getStateSet()) callback->candidateStates.emplace_back(node->getStateSet());
                callback->guard=MaterialGuard::captureDraws({&sourcePaths[i],1},tierPaths[i],true,&result.guardRefusal);
                if (!callback->guard) {fail(Refusal::Material);return result;}
            }
            step=BuildStep::Publication;result.guardPhase=GuardPhase::Publication;
            // Only new cloned node/primitive/container metadata and reduced
            // EBO payloads are incremental. Shared arrays/materials remain on
            // the ordinary owner; guard Impls have their own child charges.
            const auto retained=cook.newIndexBytes+inspected.graphBytes+inspected.geometries*8192ull+8192;
            if(retained>peak) {fail(Refusal::Capacity);return result;}
            AssetPaging::retainExperimentalOwner(*derived,retained,&original,nullptr,2u,inspected.bytes);
            result.readiness=std::make_shared<Readiness>();
            result.node=new Group(original,*derived,cook.error,result.readiness,mainCamera,false,&result.guardRefusal);
            if (!result.node->materialGuardCurrent())
            {
                if(result.guardRefusal.reason==GuardRefusal::None) result.guardRefusal.reason=GuardRefusal::Stale;
                result.node=nullptr;result.readiness.reset();fail(Refusal::Material);return result;
            }
            result.guardPhase=GuardPhase::None;
            rollback.committed=true;
            ++Counters::built;Counters::originalIndices.fetch_add(cook.before);Counters::derivedIndices.fetch_add(cook.after);
            Counters::newIndexGpuBytes.fetch_add(cook.newIndexBytes);
        }
        catch (const PagingWorkCancelled&) {throw;}
        catch (const Resource::SpeculativeDeferred& error)
        {fail(Refusal::Capacity,true,error.reservation ? &*error.reservation : nullptr);}
        catch (const std::bad_alloc&)
        {result.allocationFailure=true;fail(Refusal::Capacity);}
        catch (...)
        {
            if(result.guardPhase!=GuardPhase::None && result.guardRefusal.reason==GuardRefusal::None)
                result.guardRefusal.reason=GuardRefusal::SnapshotException;
            fail(Refusal::Capacity);
        }
        return result;
    }
}
