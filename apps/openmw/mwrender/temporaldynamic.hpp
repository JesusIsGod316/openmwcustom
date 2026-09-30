#ifndef OPENMW_MWRENDER_TEMPORALDYNAMIC_H
#define OPENMW_MWRENDER_TEMPORALDYNAMIC_H

#include <components/sceneutil/temporalmotionidentity.hpp>
#include <osg/Geometry>
#include <osg/AlphaFunc>
#include <osg/MatrixTransform>
#include <osg/Program>
#include <osg/Shader>
#include <osgUtil/RenderBin>
#include <osgUtil/RenderLeaf>
#include <osgUtil/StateGraph>
#include <cstdint>
#include <unordered_set>
#include <vector>
#include <string_view>
#include <typeinfo>

namespace MWRender
{
    struct TemporalDynamicFrame
    {
        struct Surface
        {
            osg::ref_ptr<const osg::Drawable> owner;
            osg::ref_ptr<const SceneUtil::TemporalMotionIdentity> deformation;
            osg::ref_ptr<osg::Geometry> geometry;
            osg::ref_ptr<const osg::RefMatrix> projection;
            osg::Matrixd modelView;
            std::uint64_t instance = 0, topology = 0;
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
            std::unordered_set<const osgUtil::RenderLeaf*> seen;
            captureBin(bin, seen);
        }
    private:
        void captureBin(osgUtil::RenderBin& bin, std::unordered_set<const osgUtil::RenderLeaf*>& seen)
        {
            auto captureLeaf = [&](const osgUtil::RenderLeaf* leaf) {
                if (seen.size() >= MaxSurfaces * 4) { ++unsupported; return; }
                if (!leaf || !seen.insert(leaf).second) return;
                const auto* source = dynamic_cast<const osg::Geometry*>(leaf->getDrawable());
                const auto* positions = source ? dynamic_cast<const osg::Vec3Array*>(source->getVertexArray()) : nullptr;
                if (!source || !positions || !leaf->_modelview || !leaf->_projection || source->getDrawCallback()
                    || typeid(*source) != typeid(osg::Geometry))
                { ++unsupported; return; }
                // Transparent/cutout geometry and custom effects need their
                // own exact material coverage contract. Do not overlay them
                // using an opaque geometric approximation.
                for (auto* graph = leaf->_parent; graph; graph = graph->_parent)
                {
                    if (const auto* state = graph->getStateSet(); state
                        && ((state->getMode(GL_BLEND) & osg::StateAttribute::ON)
                            || (state->getMode(GL_ALPHA_TEST) & osg::StateAttribute::ON)))
                    { ++unsupported; return; }
                    if (const auto* state = graph->getStateSet())
                    {
                        // ShaderVisitor deliberately disables GL_ALPHA_TEST
                        // while retaining RemovedAlphaFunc for shader/shadow
                        // coverage. Its mode alone cannot prove opacity.
                        if (const auto* alpha = dynamic_cast<const osg::AlphaFunc*>(
                            state->getAttribute(osg::StateAttribute::ALPHAFUNC)); alpha
                            && alpha->getFunction() != osg::AlphaFunc::ALWAYS)
                        { ++unsupported; return; }
                        if (const auto* program = dynamic_cast<const osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM)))
                            for (unsigned i = 0; i < program->getNumShaders(); ++i)
                                if (const auto* shader = program->getShader(i); shader->getType() == osg::Shader::VERTEX
                                    && shader->getName().find("objects.vert") == std::string::npos)
                                { ++unsupported; return; }
                    }
                }
                const auto* deformation = SceneUtil::temporalMotionIdentity(*source);
                std::uint64_t instance = deformation ? deformation->id : reinterpret_cast<std::uintptr_t>(source);
                if (!deformation)
                {
                    const osg::Node* parent = source;
                    unsigned pathDepth = 0;
                    while (parent->getNumParents())
                    {
                        if (parent->getNumParents() != 1 || ++pathDepth > 128) { ++unsupported; return; }
                        parent = parent->getParent(0);
                        instance ^= reinterpret_cast<std::uintptr_t>(parent)
                            + 0x9e3779b97f4a7c15ull + (instance << 6) + (instance >> 2);
                    }
                }
                std::uint64_t needed = positions->getTotalDataSize(), topology = 1469598103934665603ull;
                for (unsigned i = 0; i < source->getNumPrimitiveSets(); ++i)
                {
                    const auto* primitive = source->getPrimitiveSet(i);
                    if (primitive->getNumInstances() > 1
                        || (primitive->getMode() != GL_TRIANGLES && primitive->getMode() != GL_TRIANGLE_STRIP
                            && primitive->getMode() != GL_TRIANGLE_FAN)
                        || (typeid(*primitive) != typeid(osg::DrawArrays)
                            && typeid(*primitive) != typeid(osg::DrawArrayLengths)
                            && typeid(*primitive) != typeid(osg::DrawElementsUByte)
                            && typeid(*primitive) != typeid(osg::DrawElementsUShort)
                            && typeid(*primitive) != typeid(osg::DrawElementsUInt)))
                    { ++unsupported; return; }
                    needed += primitive->getTotalDataSize();
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
                surface.owner = source; surface.deformation = deformation;
                surface.instance = instance; surface.topology = topology;
                surface.projection = leaf->_projection; surface.modelView = *leaf->_modelview;
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
