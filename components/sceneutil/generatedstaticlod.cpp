#include "generatedstaticlod.hpp"

#include <components/resource/derivedassetcache.hpp>
#include <components/resource/speculativebudget.hpp>
#include <components/resource/assetgpubudget.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/shader/indirectprojectioncontract.hpp>
#include <components/sceneutil/shadowbatchcounters.hpp>
#include <components/sceneutil/shadowproxygroup.hpp>
#include <components/sceneutil/pagingwork.hpp>
#include <components/sceneutil/temporalvertexlineage.hpp>
#include <components/sceneutil/persistentopaquepackets.hpp>
#include <osg/AlphaFunc>
#include <osg/Camera>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osg/Geode>
#include <osg/GraphicsContext>
#include <osg/Program>
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

namespace SceneUtil::GeneratedStaticLod
{
    namespace
    {
        constexpr std::size_t MaximumVertices = 250000, MaximumIndices = 1500000;
        constexpr std::size_t MaximumCookBytes = 64 * 1024 * 1024;
        constexpr float RelativeError = .02f, MaximumPixels = 1.f;
        struct DrawReceipt final : PersistentOpaquePackets::RetainedOpaqueDrawCallback
        {
            explicit DrawReceipt(std::size_t count) : indices(count) {}
            std::size_t indices;
            void drawImplementation(osg::RenderInfo& info, const osg::Drawable* drawable) const override
            {
                ++Counters::derivedDraws; Counters::submittedIndices.fetch_add(indices);
                drawable->drawImplementation(info);
            }
        };
        bool material(const osg::StateSet* state, bool perView = false)
        {
            if (!state) return true;
            if (state->getDataVariance() == osg::Object::DYNAMIC || state->getUpdateCallback()
                || (state->getMode(GL_BLEND) & osg::StateAttribute::ON)
                || (state->getMode(GL_ALPHA_TEST) & osg::StateAttribute::ON)
                || state->getRenderingHint() == osg::StateSet::TRANSPARENT_BIN
                || state->getAttribute(osg::StateAttribute::STENCIL)
                || (!perView && state->getUniform("projectionMatrix"))) return false;
            if (const auto* a = dynamic_cast<const osg::AlphaFunc*>(state->getAttribute(osg::StateAttribute::ALPHAFUNC)))
                if (a->getFunction() != GL_ALWAYS) return false;
            if (const auto* p = dynamic_cast<const osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM)))
                if (!Shader::IndirectProjection::matches(*p)) return false;
            if (const auto* a = state->getUniform("alphaTest"))
            { bool enabled = true; if (!a->get(enabled) || enabled) return false; }
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
        Refusal inspectNode(const osg::Node& node, Size& size, unsigned depth)
        {
            // Loader-emitted hidden collision/marker hierarchies contain no
            // drawables. Certify this exact empty shape before CopyOp filters
            // it; a hidden mesh, controller or custom node is never admitted.
            if (!node.getNodeMask()) return emptyLoaderMarker(node,size,depth) ? Refusal::None : Refusal::Graph;
            if (++size.nodes > 1024 || depth > 32) return Refusal::Capacity;
            std::string custom;
            if (node.getDataVariance() == osg::Object::DYNAMIC || node.getUpdateCallback() || node.getEventCallback()
                || node.getCullCallback() || node.getComputeBoundingSphereCallback()
                || node.getUserValue("shaderPrefix", custom) || dynamic_cast<const osg::LOD*>(&node)) return Refusal::Graph;
            if (!material(node.getStateSet())) return Refusal::Material;
            if (const auto* geometry = dynamic_cast<const osg::Geometry*>(&node))
            {
                if (typeid(*geometry) != typeid(osg::Geometry) || geometry->getDrawCallback()
                    || geometry->getComputeBoundingBoxCallback()) return Refusal::Graph;
                if (auto refusal = layout(*geometry); refusal != Refusal::None) return refusal;
                ++size.geometries;
                size.vertices += geometry->getVertexArray()->getNumElements();
                size.indices += geometry->getPrimitiveSet(0)->getNumIndices();
                size.bytes += geometry->getVertexArray()->getTotalDataSize() + geometry->getPrimitiveSet(0)->getNumIndices() * 4ull;
                for (const auto* a : {geometry->getNormalArray(), geometry->getColorArray()})
                    if (a) size.bytes += a->getTotalDataSize();
                for (unsigned i = 0; i < geometry->getNumTexCoordArrays(); ++i)
                    if (const auto* a = geometry->getTexCoordArray(i)) size.bytes += a->getTotalDataSize();
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
                if (auto refusal = inspectNode(*group->getChild(i), size, depth + 1); refusal != Refusal::None) return refusal;
            return Refusal::None;
        }
        Refusal inspect(const osg::Node& node) { Size size; return inspectNode(node, size, 0); }
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
            float error = 0.f, transformScale = 1.f;
            std::vector<osg::Geometry*> geometries;
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
                const auto* positions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
                const auto* primitive = geometry.getPrimitiveSet(0);
                std::vector<unsigned> indices(primitive->getNumIndices());
                for (unsigned i = 0; i < indices.size(); ++i) indices[i] = primitive->index(i);
                before += indices.size();
                geometry.setDrawCallback(new DrawReceipt(indices.size()));
                Resource::DerivedAssets::Bytes source;
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
                // allocating them. The 96-MiB stage also covers cloned primitive
                // storage and earlier immutable tier outputs in this partition.
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
                draw->assign(reduced.begin(), reduced.end());
                draw->setElementBufferObject(new osg::ElementBufferObject);
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
    { return callback && dynamic_cast<const DrawReceipt*>(callback); }
    Refusal certify(const osg::Node& original)
    {
        ++Counters::requests;
        const auto refusal = inspect(original);
        if (refusal == Refusal::None) ++Counters::certified;
        else ++Counters::refused[static_cast<unsigned>(refusal)];
        return refusal;
    }
    std::size_t sourceBytes(const osg::Node& original)
    { Size size; return inspectNode(original, size, 0) == Refusal::None ? size.bytes : 0; }
    unsigned conflictingMaterialOwners(std::span<const MaterialOwner> materials, bool known)
    {
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
        return false;
    }
    Group::Group(const Group& copy, const osg::CopyOp& op)
        : osg::Group(copy, op), mError(copy.mError), mReadiness(std::make_shared<Readiness>()), mMainCamera(copy.mMainCamera)
        , mPreserveControlPopulation(copy.mPreserveControlPopulation) {}
    Group::Group(osg::Node& originalNode, osg::Node& derivedNode, float absoluteError,
        std::shared_ptr<Readiness> readiness, osg::Camera* mainCamera, bool preserveControlPopulation)
        : mError(absoluteError), mReadiness(std::move(readiness)), mMainCamera(mainCamera)
        , mPreserveControlPopulation(preserveControlPopulation)
    {
        setNodeMask(originalNode.getNodeMask()); setDataVariance(osg::Object::DYNAMIC);
        addChild(&originalNode); addChild(&derivedNode);
        setName("OptimizedMW-generated-static-LOD");
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
        for (auto* graph = visitor.getCurrentStateGraph(); graph; graph = graph->_parent)
        {
            const auto* state = graph->getStateSet();
            if (!material(state, true)) return false;
            if (state)
                if (const auto* uniform = state->getUniform("projectionMatrix"))
                {
                    osg::Matrixf value;
                    if (!uniform->get(value)) return false;
                    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
                        if (std::abs(value(r,c) - projection(r,c)) > 1e-4) return false;
                }
        }
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
        // Production never cooks on a visible demand path or without host admission.
        // Standalone corpus fixtures explicitly opt out of GPU publication admission.
        if (reserveGpu && (!PagingWorkScope::optionalOptimization() || !Resource::SpeculativeScope::active()))
        { result.refusal = Refusal::Capacity; ++Counters::refused[static_cast<unsigned>(result.refusal)]; return result; }
        if (auto r = inspect(original); r != Refusal::None) { result.refusal = r; return result; }
        try
        {
            Resource::SpeculativeScope::Stage reservation(96 * Resource::SpeculativeBudget::MiB
                + Resource::DerivedAssets::MetadataScratchBytes, 0);
            osg::ref_ptr<osg::Node> derived = osg::clone(&original,
                osg::CopyOp::DEEP_COPY_NODES | osg::CopyOp::DEEP_COPY_DRAWABLES | osg::CopyOp::DEEP_COPY_PRIMITIVES);
            Cook cook(identity); derived->accept(cook);
            result.originalIndices = cook.before; result.derivedIndices = cook.after;
            if (!std::isfinite(cook.error)) throw std::runtime_error("LOD error scale invalid");
            if (!cook.changed) { result.refusal = Refusal::NoReduction; ++Counters::refused[static_cast<unsigned>(result.refusal)]; return result; }
            if (reserveGpu)
            {
                result.gpuReservation = Resource::AssetGpuBudget::reserve(cook.after * sizeof(unsigned));
                if (!result.gpuReservation)
                { result.refusal = Refusal::Capacity; ++Counters::refused[static_cast<unsigned>(result.refusal)]; return result; }
            }
            struct Originals final : osg::NodeVisitor
            {
                Originals() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
                std::vector<osg::Geometry*> geometries;
                void apply(osg::Geometry& geometry) override { geometries.push_back(&geometry); }
            } originals;
            original.accept(originals);
            if (originals.geometries.size() != cook.geometries.size()) throw std::runtime_error("LOD lineage mismatch");
            for (std::size_t i = 0; i < originals.geometries.size(); ++i)
            {
                auto* source = originals.geometries[i]; auto* tier = cook.geometries[i];
                if (source->getVertexArray() != tier->getVertexArray()) throw std::runtime_error("LOD vertex lineage changed");
                osg::ref_ptr<TemporalVertexLineage> lineage = new TemporalVertexLineage(source->getVertexArray());
                installTemporalVertexLineage(*source, lineage); installTemporalVertexLineage(*tier, lineage);
            }
            result.readiness = std::make_shared<Readiness>();
            result.node = new Group(original, *derived, cook.error, result.readiness, mainCamera);
            ++Counters::built; Counters::originalIndices.fetch_add(cook.before); Counters::derivedIndices.fetch_add(cook.after);
        }
        catch (const PagingWorkCancelled&) { throw; }
        catch (...) { result.refusal = Refusal::Capacity; ++Counters::refused[static_cast<unsigned>(result.refusal)]; }
        return result;
    }
}
