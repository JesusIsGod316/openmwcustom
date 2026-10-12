#ifndef OPENMW_MWRENDER_TEMPORALDYNAMIC_H
#define OPENMW_MWRENDER_TEMPORALDYNAMIC_H

#include <components/sceneutil/temporalmotionidentity.hpp>
#include <components/sceneutil/temporalvertexlineage.hpp>
#include <components/sceneutil/generatedstaticlod.hpp>
#include <components/shader/assetgeometrycontract.hpp>
#include <osg/Geometry>
#include <osg/AlphaFunc>
#include <osg/MatrixTransform>
#include <osg/Program>
#include <osg/Shader>
#include <osgUtil/RenderBin>
#include <osgUtil/RenderLeaf>
#include <osgUtil/StateGraph>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_set>
#include <vector>
#include <string_view>
#include <typeinfo>

namespace MWRender
{
    namespace TemporalOpaque
    {
        inline constexpr std::size_t MaximumPathStates = 128;
        inline constexpr GLenum SampleAlphaToCoverage = 0x809E; // GL core / GL_ARB_multisample mode
        // Resolve the actual RenderLeaf StateGraph, not node names or a shader
        // prefix. These are OSG's pushMode/Attribute/UniformList override rules.
        inline bool resolve(const std::vector<const osg::StateSet*>& path, const osg::Program*& program,
            const osg::Uniform*& projection)
        {
            if (path.size() > MaximumPathStates) return false;
            osg::StateAttribute::GLModeValue blend = osg::StateAttribute::OFF;
            osg::StateAttribute::GLModeValue alphaMode = osg::StateAttribute::OFF;
            osg::StateAttribute::GLModeValue alphaCoverage = osg::StateAttribute::OFF;
            const osg::StateSet::RefAttributePair *programPair = nullptr, *alphaPair = nullptr, *stencilPair = nullptr;
            const osg::StateSet::RefUniformPair *alphaUniform = nullptr, *projectionUniform = nullptr;
            const auto mode = [](osg::StateAttribute::GLModeValue& current, const osg::StateSet& state, GLenum key) {
                const auto found = state.getModeList().find(key);
                if (found != state.getModeList().end()
                    && (!(current & osg::StateAttribute::OVERRIDE) || (found->second & osg::StateAttribute::PROTECTED)))
                    current = found->second;
            };
            const auto attribute = [](const osg::StateSet::RefAttributePair*& current,
                const osg::StateSet& state, osg::StateAttribute::Type type) {
                const auto* next = state.getAttributePair(type);
                if (next && (!current || !(current->second & osg::StateAttribute::OVERRIDE)
                    || (next->second & osg::StateAttribute::PROTECTED))) current = next;
            };
            const auto uniform = [](const osg::StateSet::RefUniformPair*& current,
                const osg::StateSet& state, const char* name) {
                const auto found = state.getUniformList().find(name);
                if (found != state.getUniformList().end()
                    && (!current || !(current->second & osg::StateAttribute::OVERRIDE)
                        || (found->second.second & osg::StateAttribute::PROTECTED))) current = &found->second;
            };
            for (const auto* state : path)
            {
                if (!state || state->getUpdateCallback() || state->getEventCallback()
                    || state->getAttributeList().size() > 256 || state->getUniformList().size() > 256
                    || state->getDefineList().size() > 256
                    || state->getRenderingHint() == osg::StateSet::TRANSPARENT_BIN) return false;
                mode(blend, *state, GL_BLEND); mode(alphaMode, *state, GL_ALPHA_TEST);
                mode(alphaCoverage, *state, SampleAlphaToCoverage);
                attribute(programPair, *state, osg::StateAttribute::PROGRAM);
                attribute(alphaPair, *state, osg::StateAttribute::ALPHAFUNC);
                attribute(stencilPair, *state, osg::StateAttribute::STENCIL);
                uniform(alphaUniform, *state, "alphaTest"); uniform(projectionUniform, *state, "projectionMatrix");
            }
            if ((blend | alphaMode | alphaCoverage) & osg::StateAttribute::ON) return false;
            // RemovedAlphaFunc remains semantically active in shader/shadow
            // coverage when fixed-function GL_ALPHA_TEST was disabled.
            if (alphaPair)
            {
                const auto* alpha = dynamic_cast<const osg::AlphaFunc*>(alphaPair->first.get());
                if (!alpha || alpha->getFunction() != osg::AlphaFunc::ALWAYS) return false;
            }
            if (stencilPair) return false;
            if (alphaUniform)
            {
                bool enabled = true;
                if (!alphaUniform->first || alphaUniform->first->getUpdateCallback()
                    || alphaUniform->first->getEventCallback() || !alphaUniform->first->get(enabled) || enabled) return false;
            }
            program = programPair ? dynamic_cast<const osg::Program*>(programPair->first.get()) : nullptr;
            if (programPair && !program) return false;
            projection = projectionUniform ? projectionUniform->first.get() : nullptr;
            return true;
        }
        class Proof
        {
        public:
            static std::optional<Proof> capture(const std::vector<const osg::StateSet*>& path,
                bool indexLineage, const osg::Uniform*& projection)
            {
                Shader::AssetGeometry::ProducerReadScope producer;
                const osg::Program* program = nullptr;
                if (!resolve(path, program, projection) || (indexLineage && !program)) return {};
                std::optional<Shader::AssetGeometry::Snapshot> source;
                if (program)
                {
                    // Reviewed object producers always consume this uniform.
                    // A cull matrix alone cannot prove their actual position.
                    osg::Matrixf matrix;
                    if (!projection || projection->getUpdateCallback() || projection->getEventCallback()
                        || !projection->get(matrix)) return {};
                    for (unsigned r=0;r<4;++r) for (unsigned c=0;c<4;++c)
                        if (!std::isfinite(matrix(r,c))) return {};
                    source = Shader::AssetGeometry::capture(*program, path);
                    if (!source || !reviewedOpaqueProducer(*source, *program)) return {};
                }
                Proof result; result.mProgram = program; result.mSource = std::move(source);
                result.mPath.reserve(path.size());
                for (const auto* state : path) result.mPath.emplace_back(state);
                return result;
            }
            bool current(const osg::Geometry& geometry, bool checkProjection = false,
                const osg::Uniform* expectedProjection = nullptr, const osg::RefMatrix* matrix = nullptr) const
            {
                Shader::AssetGeometry::ProducerReadScope producer;
                try
                {
                    std::vector<const osg::StateSet*> path; path.reserve(mPath.size());
                    for (const auto& state : mPath) path.push_back(state.get());
                    const osg::Program* program = nullptr; const osg::Uniform* projection = nullptr;
                    if (!resolve(path, program, projection) || program != mProgram.get()) return false;
                    if (checkProjection)
                    {
                        if (projection != expectedProjection) return false;
                        if (projection)
                        {
                            osg::Matrixf actual;
                            if (!matrix || projection->getUpdateCallback() || projection->getEventCallback()
                                || !projection->get(actual) || osg::Matrixd(actual) != *matrix) return false;
                        }
                    }
                    if (!SceneUtil::GeneratedStaticLod::temporalDrawRecipeCurrent(geometry, path)) return false;
                    return program ? mSource && Shader::AssetGeometry::current(*mSource, *program, path)
                        == Shader::AssetGeometry::Refusal::None : !mSource;
                }
                catch (const std::bad_alloc&) { return false; }
            }
            std::size_t metadataBytes() const
            { return sizeof(Proof) + mPath.size() * sizeof(osg::ref_ptr<const osg::StateSet>) + (mSource ? mSource->metadataBytes() : 0); }
        private:
            static bool reviewedOpaqueProducer(const Shader::AssetGeometry::Snapshot& source, const osg::Program& program)
            {
                // AssetGeometry proves source provenance, not temporal coverage.
                // This separate domain admits only the six reviewed object
                // producers whose vertex path is gl_ModelViewMatrix plus the
                // actual projectionMatrix, with no unproved effect/deformation.
                if (program.getNumShaders() != 6) return false;
                unsigned found = 0;
                for (unsigned i = 0; i < program.getNumShaders(); ++i)
                {
                    const auto* shader = program.getShader(i);
                    const auto status = shader ? Shader::AssetGeometry::sourceStatus(*shader) : Shader::AssetGeometry::SourceStatus{};
                    if (!shader || status.refusal != Shader::AssetGeometry::Refusal::None) return false;
                    unsigned bit = 0;
                    if (shader->getType() == osg::Shader::VERTEX)
                    {
                        if (status.name == "compatibility/objects.vert") bit = 1;
                        else if (status.name == "lib/core/vertex.glsl") bit = 2;
                        else if (status.name == "lib/core/lighting_vertex.glsl"
                            || status.name == "lib/core/lighting_vertex_legacy.glsl") bit = 4;
                    }
                    else if (shader->getType() == osg::Shader::FRAGMENT)
                    {
                        if (status.name == "compatibility/objects.frag") bit = 8;
                        else if (status.name == "lib/core/fragment.glsl") bit = 16;
                        else if (status.name == "lib/core/lighting_fragment.glsl"
                            || status.name == "lib/core/lighting_fragment_legacy.glsl") bit = 32;
                    }
                    if (!bit || (found & bit)) return false;
                    found |= bit;
                }
                for (const auto* feature : {"parallax","diffuseParallax","particleOcclusion","softParticles",
                    "additiveBlending","useOVR_multiview","alphaToCoverage"})
                {
                    const auto value = source.define(feature);
                    if (value != "0" && value != "0.0") return false;
                }
                // The approved fragment shader itself owns this comparison.
                // GL_ALPHA_TEST/RemovedAlphaFunc alone cannot prove opacity.
                return found == 63 && source.define("alphaFunc") == "519"; // GL_ALWAYS
            }
            osg::ref_ptr<const osg::Program> mProgram;
            std::optional<Shader::AssetGeometry::Snapshot> mSource;
            std::vector<osg::ref_ptr<const osg::StateSet>> mPath;
        };
    }
    struct TemporalDynamicFrame
    {
        struct Surface
        {
            struct Primitive
            {
                osg::ref_ptr<const osg::PrimitiveSet> owner;
                unsigned modified = 0, indices = 0, instances = 0;
                GLenum mode = 0;
            };
            osg::ref_ptr<const osg::Drawable> owner;
            osg::ref_ptr<const SceneUtil::TemporalMotionIdentity> deformation;
            osg::ref_ptr<const SceneUtil::TemporalVertexLineage> lineage;
            osg::ref_ptr<osg::Geometry> geometry;
            osg::ref_ptr<const osg::RefMatrix> projection;
            osg::ref_ptr<const osg::Uniform> projectionOwner;
            std::optional<TemporalOpaque::Proof> materialProof;
            osg::ref_ptr<const osg::Array> sourceVertices;
            osg::ref_ptr<const osg::Drawable::DrawCallback> sourceCallback;
            unsigned vertexModified = 0;
            std::vector<Primitive> sourcePrimitives;
            osg::Matrixd modelView;
            std::uint64_t instance = 0, topology = 0;
            bool materialCurrent(bool checkCurrentProjection = true) const
            {
                Shader::AssetGeometry::ProducerReadScope producer;
                const auto* source = owner ? owner->asGeometry() : nullptr;
                if (!source || source->getDrawCallback() != sourceCallback.get()
                    || source->getVertexArray() != sourceVertices.get() || !sourceVertices
                    || sourceVertices->getModifiedCount() != vertexModified
                    || source->getNumPrimitiveSets() != sourcePrimitives.size()) return false;
                for (unsigned i = 0; i < sourcePrimitives.size(); ++i)
                {
                    const auto& captured = sourcePrimitives[i]; const auto* primitive = source->getPrimitiveSet(i);
                    if (primitive != captured.owner.get() || !primitive || primitive->getModifiedCount() != captured.modified
                        || primitive->getNumIndices() != captured.indices || primitive->getNumInstances() != captured.instances
                        || primitive->getMode() != captured.mode) return false;
                }
                return materialProof && materialProof->current(*source,checkCurrentProjection,projectionOwner.get(),projection.get());
            }
            bool compatibleHistory(const Surface& previous) const
            {
                Shader::AssetGeometry::ProducerReadScope producer;
                const auto* previousGeometry = previous.owner ? previous.owner->asGeometry() : nullptr;
                // Rig/morph source buffers may already be reused for a later
                // CPU pose. Previous positions/topology are the owned immutable
                // snapshot, not that live buffer; its material recipe still
                // must be valid. Index lineage additionally requires immutable
                // live source/tier recipes on both sides.
                if(instance!=previous.instance || !materialCurrent() || !previousGeometry || !previous.materialProof
                    || !previous.materialProof->current(*previousGeometry)) return false;
                if(lineage || previous.lineage)
                {
                    const auto* currentGeometry=owner ? owner->asGeometry() : nullptr;
                    return previous.materialCurrent(false) && lineage && lineage==previous.lineage
                        && currentGeometry && previousGeometry
                        && lineage->matches(*currentGeometry) && lineage->matches(*previousGeometry);
                }
                return topology==previous.topology && (deformation ? deformation==previous.deformation : owner==previous.owner);
            }
        };
        std::vector<Surface> surfaces;
        std::uint64_t bytes = 0;
        unsigned unsupported = 0;
        // Snapshot/previous-pose storage has a fixed engineering ceiling.
        // Rejection preserves camera/static motion and never marks completeness.
        static constexpr std::uint64_t MaxBytes = 16u * 1024u * 1024u;
        static constexpr std::size_t MaxSurfaces = 2048;

        void capture(osgUtil::RenderBin& bin)
        {
            try
            {
                // At most 8192 seen-leaf records plus a 128-StateSet scratch
                // path; retained poses/proofs have the separate 16 MiB ceiling.
                std::unordered_set<const osgUtil::RenderLeaf*> seen;
                captureBin(bin, seen);
            }
            catch (const std::bad_alloc&) { ++unsupported; }
        }
    private:
        void captureBin(osgUtil::RenderBin& bin, std::unordered_set<const osgUtil::RenderLeaf*>& seen)
        {
            auto captureLeaf = [&](const osgUtil::RenderLeaf* leaf) {
                Shader::AssetGeometry::ProducerReadScope producer;
                if (seen.size() >= MaxSurfaces * 4) { ++unsupported; return; }
                if (!leaf || !seen.insert(leaf).second) return;
                const auto* source = dynamic_cast<const osg::Geometry*>(leaf->getDrawable());
                const auto* positions = source ? dynamic_cast<const osg::Vec3Array*>(source->getVertexArray()) : nullptr;
                const auto* lineage=source ? SceneUtil::temporalVertexLineage(*source) : nullptr;
                if (!source || !positions || !leaf->_modelview || !leaf->_projection
                    || positions->size() < 3 || positions->getTotalDataSize() > MaxBytes / 4
                    || !source->getNumPrimitiveSets() || source->getNumPrimitiveSets() > 64
                    || (source->getDrawCallback() && !(lineage && SceneUtil::GeneratedStaticLod::isDelegatingDrawCallback(source->getDrawCallback())))
                    || typeid(*source) != typeid(osg::Geometry))
                { ++unsupported; return; }
                if (const auto* user = source->getUserDataContainer())
                {
                    if (user->getNumUserObjects() > 64) { ++unsupported; return; }
                    for (unsigned i = 0; i < user->getNumUserObjects(); ++i)
                        if (dynamic_cast<const SceneUtil::TemporalVertexLineage*>(user->getUserObject(i)) && !lineage)
                        { ++unsupported; return; }
                }
                std::vector<const osg::StateSet*> materialPath;
                unsigned graphDepth = 0;
                for (auto* graph = leaf->_parent; graph; graph = graph->_parent)
                {
                    if (++graphDepth > TemporalOpaque::MaximumPathStates) { ++unsupported; return; }
                    if (const auto* state = graph->getStateSet()) materialPath.push_back(state);
                }
                std::reverse(materialPath.begin(), materialPath.end());
                const osg::Uniform* projectionUniform = nullptr;
                auto materialProof = TemporalOpaque::Proof::capture(materialPath, lineage != nullptr, projectionUniform);
                if (!materialProof || !SceneUtil::GeneratedStaticLod::temporalDrawRecipeCurrent(*source, materialPath))
                { ++unsupported; return; }
                const auto* deformation = SceneUtil::temporalMotionIdentity(*source);
                std::uint64_t instance = deformation ? deformation->id : lineage ? lineage->id : reinterpret_cast<std::uintptr_t>(source);
                if (!deformation)
                {
                    const osg::Node* parent = source;
                    unsigned pathDepth = 0;
                    while (parent->getNumParents())
                    {
                        if (parent->getNumParents() != 1 || ++pathDepth > 128) { ++unsupported; return; }
                        parent = parent->getParent(0);
                        if(!lineage) instance ^= reinterpret_cast<std::uintptr_t>(parent)
                            + 0x9e3779b97f4a7c15ull + (instance << 6) + (instance >> 2);
                    }
                }
                // Include geometry/array/VBO containers and conservative
                // primitive/vector/EBO overhead, not only index payload bytes.
                std::uint64_t needed = positions->getTotalDataSize() + 4096 + sizeof(Surface)
                    + materialProof->metadataBytes(), topology = 1469598103934665603ull;
                for (unsigned i = 0; i < source->getNumPrimitiveSets(); ++i)
                {
                    const auto* primitive = source->getPrimitiveSet(i);
                    if (primitive->getNumIndices() < 3 || primitive->getNumInstances() < 0 || primitive->getNumInstances() > 1
                        || (primitive->getMode() != GL_TRIANGLES && primitive->getMode() != GL_TRIANGLE_STRIP
                            && primitive->getMode() != GL_TRIANGLE_FAN)
                        || (typeid(*primitive) != typeid(osg::DrawArrays)
                            && typeid(*primitive) != typeid(osg::DrawArrayLengths)
                            && typeid(*primitive) != typeid(osg::DrawElementsUByte)
                            && typeid(*primitive) != typeid(osg::DrawElementsUShort)
                            && typeid(*primitive) != typeid(osg::DrawElementsUInt)))
                    { ++unsupported; return; }
                    needed += primitive->getTotalDataSize() + 1024;
                    if (needed > MaxBytes / 4 || primitive->getNumIndices() > MaxBytes / 16)
                    { ++unsupported; return; }
                    topology = (topology ^ primitive->getMode()) * 1099511628211ull;
                    topology = (topology ^ primitive->getNumIndices()) * 1099511628211ull;
                    for (unsigned j = 0; j < primitive->getNumIndices(); ++j)
                    {
                        if (primitive->index(j) >= positions->size()) { ++unsupported; return; }
                        topology = (topology ^ primitive->index(j)) * 1099511628211ull;
                    }
                }
                if (needed > MaxBytes / 4 || needed > MaxBytes - bytes || surfaces.size() >= MaxSurfaces)
                { ++unsupported; return; }
                Surface surface;
                surface.projectionOwner = projectionUniform;
                surface.owner = source; surface.deformation = deformation; surface.lineage=lineage;
                surface.sourceVertices = positions; surface.vertexModified = positions->getModifiedCount();
                surface.sourceCallback = source->getDrawCallback();
                surface.sourcePrimitives.reserve(source->getNumPrimitiveSets());
                for (unsigned i = 0; i < source->getNumPrimitiveSets(); ++i)
                {
                    const auto* primitive = source->getPrimitiveSet(i);
                    surface.sourcePrimitives.push_back({primitive,primitive->getModifiedCount(),primitive->getNumIndices(),
                        static_cast<unsigned>(primitive->getNumInstances()),primitive->getMode()});
                }
                surface.instance = instance; surface.topology = topology;
                surface.projection = leaf->_projection; surface.modelView = *leaf->_modelview;
                // Reviewed object shaders consume projectionMatrix, which may
                // differ from OSG's culling projection (for reversed depth).
                // Freeze the actual effective GPU matrix used by this draw.
                if (projectionUniform)
                {
                    osg::Matrixf matrix;
                    if (projectionUniform->getUpdateCallback() || projectionUniform->getEventCallback()
                        || !projectionUniform->get(matrix)) { ++unsupported; return; }
                    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
                        if (!std::isfinite(matrix(r,c))) { ++unsupported; return; }
                    surface.projection = new osg::RefMatrix(osg::Matrixd(matrix));
                    needed += sizeof(osg::RefMatrix);
                    if (needed > MaxBytes - bytes) { ++unsupported; return; }
                }
                surface.materialProof = std::move(materialProof);
                surface.geometry = new osg::Geometry;
                surface.geometry->setUseDisplayList(false); surface.geometry->setUseVertexBufferObjects(true);
                osg::ref_ptr<osg::Vec3Array> capturedPositions = new osg::Vec3Array(*positions);
                capturedPositions->setVertexBufferObject(new osg::VertexBufferObject);
                surface.geometry->setVertexArray(capturedPositions);
                for (unsigned i = 0; i < source->getNumPrimitiveSets(); ++i)
                {
                    osg::ref_ptr<osg::PrimitiveSet> primitive = static_cast<osg::PrimitiveSet*>(
                        source->getPrimitiveSet(i)->clone(osg::CopyOp::SHALLOW_COPY));
                    if (auto* elements = primitive->getDrawElements()) elements->setElementBufferObject(new osg::ElementBufferObject);
                    surface.geometry->addPrimitiveSet(primitive);
                }
                bytes += needed; surfaces.push_back(std::move(surface));
            };
            for (const auto* leaf : bin.getRenderLeafList()) captureLeaf(leaf);
            for (const auto* graph : bin.getStateGraphList())
                for (const auto& leaf : graph->_leaves) captureLeaf(leaf.get());
            for (const auto& child : bin.getRenderBinList()) captureBin(*child.second, seen);
        }
    };
}
#endif
