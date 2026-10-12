#ifndef OPENMW_PHASE9_RETAINED_MATERIAL_BUCKET_PROTOTYPE_HPP
#define OPENMW_PHASE9_RETAINED_MATERIAL_BUCKET_PROTOTYPE_HPP

// Native-only replacement experiment. Not included by the game renderer.
// The first gate isolates RenderLeaf dispatch reduction; Geometry/VBO setup is
// deliberately retained and counted. No GPU indirect/visibility work is added.
#include <osg/Camera>
#include <osg/ColorMask>
#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/Geometry>
#include <osg/Geode>
#include <osg/Group>
#include <osg/LightModel>
#include <osg/Material>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Viewport>
#include <osg/Version>
#include <osgUtil/CullVisitor>
#include <osgUtil/StateGraph>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace SceneUtil::RetainedMaterialBucketPrototype
{
    constexpr unsigned MaximumRecords = 32;
    constexpr std::uint64_t MaximumOwnedBytes = 1024 * 1024;
    constexpr const char* VertexSource =
        "#version 120\nvoid main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;}\n";
    constexpr const char* FragmentSource =
        "#version 120\nuniform vec4 materialColor;uniform float viewScale;"
        "void main(){gl_FragColor=vec4(materialColor.rgb*viewScale,materialColor.a);}\n";

    struct Work
    {
        std::atomic<std::uint64_t> cullCalls{ 0 }, sourceRecordGuards{ 0 }, materialGuards{ 0 };
        std::atomic<std::uint64_t> candidateLeaves{ 0 }, replacedOrdinaryLeaves{ 0 };
        std::atomic<std::uint64_t> visibleRecords{ 0 }, culledRecords{ 0 }, fallbacks{ 0 }, busyFrames{ 0 };
        std::atomic<std::uint64_t> bucketDraws{ 0 }, geometrySetupsPreserved{ 0 };
        std::atomic<std::uint64_t> materialPatches{ 0 }, customRestorationCalls{ 0 };
        std::atomic<std::uint64_t> liveOwnedBytes{ 0 }, invalidations{ 0 };
        std::atomic<unsigned> lastDecline{ 0 };
        osg::ref_ptr<const osg::StateSet> diagnosticDeclinedState;
    };

    inline bool closedProgram(const osg::Program* program)
    {
        if (!program || typeid(*program) != typeid(osg::Program) || program->getNumShaders() != 2
            || !program->getShaderDefines().empty() || program->getProgramBinary()
            || !program->getAttribBindingList().empty() || !program->getFragDataBindingList().empty()
            || !program->getUniformBlockBindingList().empty() || program->getNumTransformFeedBackVaryings() != 0)
            return false;
        bool vertex = false, fragment = false;
        for (unsigned i = 0; i < program->getNumShaders(); ++i)
        {
            const auto* shader = program->getShader(i);
            if (!shader || typeid(*shader) != typeid(osg::Shader) || shader->getDataVariance() == osg::Object::DYNAMIC
                || shader->getShaderBinary() || !shader->getShaderDefines().empty() || !shader->getShaderRequirements().empty()) return false;
            if (shader->getType() == osg::Shader::VERTEX && shader->getShaderSource() == VertexSource) vertex = true;
            else if (shader->getType() == osg::Shader::FRAGMENT && shader->getShaderSource() == FragmentSource) fragment = true;
            else return false;
        }
        return vertex && fragment;
    }

    inline bool safeState(const osg::StateSet* state, bool requireMaterial)
    {
        if (!state) return !requireMaterial;
        if (state->getDataVariance() == osg::Object::DYNAMIC || state->getUpdateCallback() || state->getEventCallback()
            || state->getRenderingHint() == osg::StateSet::TRANSPARENT_BIN || state->useRenderBinDetails()
            || !state->getTextureAttributeList().empty() || !state->getTextureModeList().empty()
            || !state->getDefineList().empty()) return false;
        for (const auto& [mode, value] : state->getModeList())
        {
            if (mode == GL_DEPTH_TEST) { if (!(value & osg::StateAttribute::ON)) return false; }
            else if (mode == GL_BLEND || mode == GL_ALPHA_TEST || mode == GL_STENCIL_TEST || mode == GL_LIGHTING)
            { if (value & osg::StateAttribute::ON) return false; }
            else return false;
        }
        for (const auto& [type, pair] : state->getAttributeList())
        {
            if (type.second != 0 || !pair.first || pair.first->getDataVariance() == osg::Object::DYNAMIC
                || pair.first->getUpdateCallback() || pair.first->getEventCallback()) return false;
            if (type.first == osg::StateAttribute::PROGRAM)
            { if (!closedProgram(dynamic_cast<const osg::Program*>(pair.first.get()))) return false; }
            else if (type.first == osg::StateAttribute::DEPTH)
            {
                const auto* depth = dynamic_cast<const osg::Depth*>(pair.first.get());
                if (!depth || typeid(*depth) != typeid(osg::Depth) || depth->getFunction() != osg::Depth::LESS || !depth->getWriteMask()
                    || depth->getZNear() != 0 || depth->getZFar() != 1) return false;
            }
            else if (type.first == osg::StateAttribute::VIEWPORT && !requireMaterial)
            { if (typeid(*pair.first) != typeid(osg::Viewport)) return false; }
            else if (type.first == osg::StateAttribute::COLORMASK && !requireMaterial)
            {
                const auto* mask = dynamic_cast<const osg::ColorMask*>(pair.first.get());
                if (!mask || typeid(*mask) != typeid(osg::ColorMask)
                    || !mask->getRedMask() || !mask->getGreenMask() || !mask->getBlueMask() || !mask->getAlphaMask()) return false;
            }
            // View's base fixed-function defaults are inert for the literal
            // closed shader. Any ancestor enabling lighting or blending is
            // refused above; no source material admits these attributes.
            else if (!requireMaterial && type.first == osg::StateAttribute::MATERIAL)
            { if (typeid(*pair.first) != typeid(osg::Material)) return false; }
            else if (!requireMaterial && type.first == osg::StateAttribute::LIGHTMODEL)
            { if (typeid(*pair.first) != typeid(osg::LightModel)) return false; }
            else if (!requireMaterial && type.first == osg::StateAttribute::BLENDFUNC)
            { if (typeid(*pair.first) != typeid(osg::BlendFunc)) return false; }
            else return false;
        }
        for (const auto& [name, pair] : state->getUniformList())
        {
            if (!pair.first || typeid(*pair.first) != typeid(osg::Uniform) || pair.first->getDataVariance() == osg::Object::DYNAMIC
                || pair.first->getUpdateCallback() || pair.first->getEventCallback()
                || pair.first->getNumElements() != 1) return false;
            const auto uniformType = pair.first->getType();
            if (name == "materialColor") { if (uniformType != osg::Uniform::FLOAT_VEC4) return false; }
            else if (name == "viewScale") { if (uniformType != osg::Uniform::FLOAT) return false; }
            // SceneView supplies these exact inherited globals. The literal
            // closed program above does not consume them; both paths retain
            // the same StateGraph ancestors. This is not an arbitrary osg_*
            // namespace exception or permission for source material uniforms.
            else if (!requireMaterial && (name == "osg_DeltaFrameTime" || name == "osg_DeltaSimulationTime"
                || name == "osg_FrameTime" || name == "osg_SimulationTime"))
            { if (uniformType != osg::Uniform::FLOAT) return false; }
            else if (!requireMaterial && name == "osg_FrameNumber")
            { if (uniformType != osg::Uniform::UNSIGNED_INT) return false; }
            else if (!requireMaterial && (name == "osg_ViewMatrix" || name == "osg_ViewMatrixInverse"))
            { if (uniformType != osg::Uniform::FLOAT_MAT4) return false; }
            else return false;
        }
        return !requireMaterial || (closedProgram(dynamic_cast<const osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM)))
            && dynamic_cast<const osg::Depth*>(state->getAttribute(osg::StateAttribute::DEPTH))
            && (state->getMode(GL_DEPTH_TEST) & osg::StateAttribute::ON));
    }

    inline bool safeGeometry(const osg::Geometry& geometry)
    {
        if (typeid(geometry) != typeid(osg::Geometry) || geometry.getDataVariance() == osg::Object::DYNAMIC
            || geometry.getUpdateCallback() || geometry.getEventCallback() || geometry.getCullCallback()
            || geometry.getDrawCallback() || geometry.getComputeBoundingBoxCallback() || geometry.getComputeBoundingSphereCallback()
            || !geometry.getUseVertexBufferObjects() || geometry.getUseDisplayList() || geometry.getNumPrimitiveSets() != 1
            || geometry.getNormalArray() || geometry.getColorArray() || geometry.getSecondaryColorArray()
            || geometry.getFogCoordArray() || !geometry.getTexCoordArrayList().empty() || !geometry.getVertexAttribArrayList().empty()) return false;
        const auto* vertices = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
        const auto* draw = dynamic_cast<const osg::DrawArrays*>(geometry.getPrimitiveSet(0));
        if (!vertices || typeid(*vertices) != typeid(osg::Vec3Array) || vertices->getDataVariance() == osg::Object::DYNAMIC
            || !draw || typeid(*draw) != typeid(osg::DrawArrays) || draw->getDataVariance() == osg::Object::DYNAMIC
            || draw->getMode() != GL_TRIANGLES || draw->getNumInstances() != 0
            || draw->getFirst() != 0 || draw->getCount() < 3 || draw->getCount() % 3 != 0
            || static_cast<unsigned>(draw->getCount()) != vertices->size()) return false;
        return geometry.getBoundingBox().valid();
    }

    inline bool safeRoot(const osg::Group& original)
    {
        return (typeid(original) == typeid(osg::Group) || typeid(original) == typeid(osg::Geode))
            && original.getDataVariance() != osg::Object::DYNAMIC && !original.getUpdateCallback() && !original.getEventCallback()
            && !original.getCullCallback() && !original.getComputeBoundingSphereCallback()
            && safeState(original.getStateSet(), false);
    }

    struct Record
    {
        osg::ref_ptr<osg::Geometry> original, owned;
        osg::ref_ptr<const osg::Array> sourceVertices;
        osg::ref_ptr<const osg::PrimitiveSet> sourcePrimitive;
        unsigned verticesRevision = 0, primitiveRevision = 0;
        osg::Node::NodeMask mask = 0;
        osg::BoundingBox bounds;
        bool matches() const
        {
            return safeGeometry(*original) && original->getVertexArray() == sourceVertices.get()
                && original->getPrimitiveSet(0) == sourcePrimitive.get() && sourceVertices->getModifiedCount() == verticesRevision
                && sourcePrimitive->getModifiedCount() == primitiveRevision && original->getNodeMask() == mask;
        }
    };

    struct Payload
    {
        struct UniformStamp
        {
            osg::ref_ptr<const osg::Uniform> owner;
            unsigned revision = 0;
            osg::StateAttribute::OverrideValue flags = 0;
        };
        osg::ref_ptr<osg::Group> original;
        osg::ref_ptr<osg::StateSet> originalMaterial, ownedMaterial;
        std::vector<Record> records;
        std::vector<UniformStamp> uniformStamps;
        osg::StateSet::ModeList materialModes;
        osg::StateSet::AttributeList materialAttributes;
        osg::BoundingBox bounds;
        osg::observer_ptr<osg::Camera> camera;
        std::shared_ptr<Work> work;
        std::atomic<bool> invalid{ false };
        const std::thread::id ownerThread = std::this_thread::get_id();
        std::uint64_t ownedBytes = 0;
        ~Payload() { if (work) work->liveOwnedBytes.fetch_sub(ownedBytes, std::memory_order_relaxed); }
        bool sourceMatches() const
        {
            ++work->materialGuards;
            if (invalid || !safeRoot(*original) || original->getNumChildren() != records.size() || !safeState(originalMaterial, true)
                || originalMaterial->getUniformList().size() != uniformStamps.size()
                || originalMaterial->getModeList() != materialModes || originalMaterial->getAttributeList() != materialAttributes) return false;
            for (const auto& stamp : uniformStamps)
            {
                const auto* pair = originalMaterial->getUniformPair(stamp.owner->getName());
                if (stamp.owner->getModifiedCount() != stamp.revision || !pair
                    || pair->first.get() != stamp.owner.get() || pair->second != stamp.flags) return false;
            }
            for (unsigned i = 0; i < records.size(); ++i)
            {
                ++work->sourceRecordGuards;
                if (original->getChild(i) != records[i].original.get() || records[i].original->getStateSet() != originalMaterial.get()
                    || !records[i].matches()) return false;
            }
            return true;
        }
    };

    class Frame final : public osg::Drawable
    {
    public:
        explicit Frame(std::shared_ptr<Payload> payload) : mPayload(std::move(payload))
        {
            setDataVariance(osg::Object::STATIC); setUseDisplayList(false); setUseVertexBufferObjects(false);
            setStateSet(mPayload->ownedMaterial);
        }
        Frame(const Frame& other, const osg::CopyOp& op = osg::CopyOp::SHALLOW_COPY) : osg::Drawable(other, op) {}
        META_Object(SceneUtil, Frame)
        Frame() = default;
        void prepare(const std::array<unsigned, MaximumRecords>& records, unsigned count)
        { mVisible = records; mCount = count; }
#ifdef P9_RETAINED_BUCKET_TEST_HOOKS
        const Payload& testPayload() const { return *mPayload; }
#endif
        osg::BoundingBox computeBoundingBox() const override { return mPayload ? mPayload->bounds : osg::BoundingBox{}; }
        void drawImplementation(osg::RenderInfo& info) const override
        {
            if (!mPayload) return;
            ++mPayload->work->bucketDraws;
            // RenderLeaf performs the one material/model-view application. This
            // prototype leaves each original base Geometry setup in place.
            // Publisher edits are restricted to the single owner thread before
            // cull; production generation/reader integration is still absent.
            for (unsigned i = 0; i < mCount; ++i)
            {
                ++mPayload->work->geometrySetupsPreserved;
                mPayload->records[mVisible[i]].owned->drawImplementation(info);
            }
        }
    private:
        std::shared_ptr<Payload> mPayload;
        std::array<unsigned, MaximumRecords> mVisible{};
        unsigned mCount = 0;
    };

    class Group final : public osg::Group
    {
    public:
        Group() = default;
        Group(const Group& other, const osg::CopyOp& op = osg::CopyOp::SHALLOW_COPY) : osg::Group(other, op) {}
        META_Node(SceneUtil, Group)
        explicit Group(std::shared_ptr<Payload> payload) : mPayload(std::move(payload))
        {
            addChild(mPayload->original); setName("R1-Native-MaterialBucket-Prototype");
            for (auto& frame : mFrames) frame = new Frame(mPayload);
        }
        void invalidate()
        { if (mPayload) { mPayload->invalid.store(true); ++mPayload->work->invalidations; } }
        std::shared_ptr<Work> work() const { return mPayload ? mPayload->work : nullptr; }
        void traverse(osg::NodeVisitor& visitor) override
        {
            auto* cv = visitor.asCullVisitor();
            if (!mPayload || !cv) { osg::Group::traverse(visitor); return; }
            ++mPayload->work->cullCalls;
            const auto fallback = [&] { ++mPayload->work->fallbacks; osg::Group::traverse(visitor); };
            unsigned refusal = 0;
            if (std::this_thread::get_id() != mPayload->ownerThread) refusal = 1;
            else if (typeid(*cv) != typeid(osgUtil::CullVisitor)) refusal = 2;
            else if (cv->getCurrentCamera() != mPayload->camera.get()) refusal = 3;
            else if (!cv->getCurrentStateGraph() || !cv->getModelViewMatrix()) refusal = 4;
            else if (cv->getComputeNearFarMode() != osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR) refusal = 5;
            else if (!cv->getCurrentCullingSet().getStateFrustumList().empty()) refusal = 6;
            else if (!mPayload->sourceMatches()) refusal = 7;
            else if (!safeState(mPayload->original->getStateSet(), false)) refusal = 8;
            if (refusal) { mPayload->work->lastDecline = refusal; fallback(); return; }
            for (auto* graph = cv->getCurrentStateGraph(); graph; graph = graph->_parent)
                if (!safeState(graph->getStateSet(), false))
                { mPayload->work->lastDecline = 9; mPayload->work->diagnosticDeclinedState = graph->getStateSet(); fallback(); return; }
            // The original state is retained on the same StateGraph ancestry;
            // uniforms with legal OVERRIDE/PROTECTED continue to resolve in OSG.
            if (!visitor.validNodeMask(*mPayload->original) || cv->isCulled(*mPayload->original)) return;
            std::unique_lock lock(mMutex, std::try_to_lock);
            if (!lock.owns_lock()) { ++mPayload->work->busyFrames; fallback(); return; }
            osg::ref_ptr<Frame> frame;
            for (auto& candidate : mFrames)
                if (candidate->referenceCount() == 1) { frame = candidate; break; }
            if (!frame) { ++mPayload->work->busyFrames; fallback(); return; }
            std::array<unsigned, MaximumRecords> visible{};
            unsigned count = 0;
            for (unsigned i = 0; i < mPayload->records.size(); ++i)
            {
                const auto& record = mPayload->records[i];
                if (!visitor.validNodeMask(*record.original) || (record.original->isCullingActive() && cv->isCulled(record.bounds)))
                { ++mPayload->work->culledRecords; continue; }
                visible[count++] = i;
            }
            if (!count) return;
            frame->prepare(visible, count);
            const auto center = mPayload->bounds.center();
            const auto& matrix = *cv->getModelViewMatrix();
            const float depth = static_cast<float>(-(center.x() * matrix(0,2) + center.y() * matrix(1,2)
                + center.z() * matrix(2,2) + matrix(3,2)));
            if (!std::isfinite(depth)) { fallback(); return; }
            visitor.pushOntoNodePath(mPayload->original);
            const auto* rootState = mPayload->original->getStateSet();
            if (rootState) cv->pushStateSet(rootState);
            cv->pushStateSet(mPayload->ownedMaterial);
            cv->addDrawableAndDepth(frame, cv->getModelViewMatrix(), depth);
            cv->popStateSet(); if (rootState) cv->popStateSet();
            visitor.popFromNodePath();
            ++mPayload->work->candidateLeaves;
            mPayload->work->visibleRecords.fetch_add(count);
            mPayload->work->replacedOrdinaryLeaves.fetch_add(count);
            // No custom GL bind/restore or per-frame material patch was performed.
        }
    private:
        std::shared_ptr<Payload> mPayload;
        std::array<osg::ref_ptr<Frame>, 3> mFrames;
        std::mutex mMutex;
    };

    inline osg::ref_ptr<osg::Node> build(osg::Group& original, osg::Camera& camera, std::shared_ptr<Work> work,
        bool requested, std::uint64_t byteLimit = MaximumOwnedBytes)
    {
        if (!requested || std::strcmp(osgGetVersion(), "3.6.5") != 0 || !work || byteLimit > MaximumOwnedBytes
            || !safeRoot(original)
            || original.getNumChildren() < 2 || original.getNumChildren() > MaximumRecords) return &original;
        osg::StateSet* material = nullptr;
        std::uint64_t bytes = sizeof(Payload) + sizeof(Group) + 3 * sizeof(Frame) + sizeof(osg::StateSet)
            + sizeof(osg::Program) + 2 * sizeof(osg::Shader) + sizeof(osg::Depth) + 2 * sizeof(osg::Uniform) + 4096;
        for (unsigned i = 0; i < original.getNumChildren(); ++i)
        {
            auto* geometry = original.getChild(i)->asGeometry();
            if (!geometry || !safeGeometry(*geometry) || !safeState(geometry->getStateSet(), true)) return &original;
            if (!material) material = geometry->getStateSet();
            if (geometry->getStateSet() != material) return &original;
            const auto* vertices = static_cast<const osg::Vec3Array*>(geometry->getVertexArray());
            // Array::getTotalDataSize returns unsigned int in this pinned OSG.
            // Charge the exact accepted Vec3 count in 64 bits before any copy;
            // a large array must not wrap that API and bypass the allowance.
            bytes += sizeof(Record) + sizeof(osg::Geometry) + sizeof(osg::Vec3Array) + sizeof(osg::VertexBufferObject)
                + static_cast<std::uint64_t>(vertices->size()) * sizeof(osg::Vec3) + sizeof(osg::DrawArrays);
            if (bytes > byteLimit) return &original;
            for (const auto& vertex : *vertices)
                for (unsigned axis = 0; axis < 3; ++axis) if (!std::isfinite(vertex[axis])) return &original;
        }
        // Shared known-payload charge covers pending copies and retiring native
        // generations, not a fresh per-group allowance. Allocator/driver bytes
        // remain outside this conservative fixture charge.
        auto charged = work->liveOwnedBytes.load(std::memory_order_relaxed);
        do
        {
            if (charged > byteLimit || bytes > byteLimit - charged) return &original;
        } while (!work->liveOwnedBytes.compare_exchange_weak(charged, charged + bytes, std::memory_order_relaxed));
        struct PendingCharge
        {
            std::shared_ptr<Work> work;
            std::uint64_t bytes;
            bool held = true;
            ~PendingCharge() { if (held) work->liveOwnedBytes.fetch_sub(bytes, std::memory_order_relaxed); }
        } pending{work,bytes};
        try
        {
            auto payload = std::make_shared<Payload>();
            payload->ownedBytes = bytes; payload->work = work; pending.held = false;
            payload->original = &original; payload->originalMaterial = material; payload->camera = &camera;
            payload->records.reserve(original.getNumChildren());
            // Construct only the closed consumed state. Generic Object copies
            // can carry arbitrary names/UserDataContainers and uncharged owner
            // references, so neither geometry nor material uses them here.
            payload->ownedMaterial = new osg::StateSet;
            payload->ownedMaterial->setDataVariance(osg::Object::STATIC);
            for (const auto& [mode, value] : material->getModeList()) payload->ownedMaterial->setMode(mode, value);
            osg::ref_ptr<osg::Program> privateProgram = new osg::Program;
            privateProgram->addShader(new osg::Shader(osg::Shader::VERTEX, VertexSource));
            privateProgram->addShader(new osg::Shader(osg::Shader::FRAGMENT, FragmentSource));
            for (const auto& [type, pair] : material->getAttributeList())
                payload->ownedMaterial->setAttribute(type.first == osg::StateAttribute::PROGRAM
                    ? static_cast<osg::StateAttribute*>(privateProgram.get())
                    : static_cast<osg::StateAttribute*>(new osg::Depth(osg::Depth::LESS, 0, 1, true)), pair.second);
            payload->materialModes = material->getModeList(); payload->materialAttributes = material->getAttributeList();
            for (const auto& [name, pair] : material->getUniformList())
            {
                payload->uniformStamps.push_back({ pair.first, pair.first->getModifiedCount(), pair.second });
                osg::ref_ptr<osg::Uniform> privateUniform;
                if (name == "materialColor")
                { osg::Vec4 value; if (!pair.first->get(value)) return &original; privateUniform = new osg::Uniform(name.c_str(), value); }
                else
                { float value; if (!pair.first->get(value)) return &original; privateUniform = new osg::Uniform(name.c_str(), value); }
                payload->ownedMaterial->addUniform(privateUniform, pair.second);
            }
            for (unsigned i = 0; i < original.getNumChildren(); ++i)
            {
                auto* geometry = original.getChild(i)->asGeometry();
                Record record;
                record.original = geometry; record.sourceVertices = geometry->getVertexArray();
                record.sourcePrimitive = geometry->getPrimitiveSet(0); record.mask = geometry->getNodeMask();
                record.verticesRevision = record.sourceVertices->getModifiedCount(); record.primitiveRevision = record.sourcePrimitive->getModifiedCount();
                record.bounds = geometry->getBoundingBox(); payload->bounds.expandBy(record.bounds);
                record.owned = new osg::Geometry;
                record.owned->setDataVariance(osg::Object::STATIC);
                record.owned->setUseDisplayList(false); record.owned->setUseVertexBufferObjects(true);
                osg::ref_ptr<osg::Vec3Array> privateVertices = new osg::Vec3Array;
                const auto* sourceVertices = static_cast<const osg::Vec3Array*>(geometry->getVertexArray());
                privateVertices->assign(sourceVertices->begin(), sourceVertices->end());
                privateVertices->setVertexBufferObject(new osg::VertexBufferObject);
                record.owned->setVertexArray(privateVertices);
                record.owned->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, static_cast<int>(privateVertices->size())));
                payload->records.emplace_back(std::move(record));
            }
            return new Group(std::move(payload));
        }
        catch (const std::bad_alloc&) { return &original; }
    }
}
#endif
