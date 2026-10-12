#include "stateupdater.hpp"

#include <osg/BufferIndexBinding>
#include <osg/BufferObject>
#include <osgUtil/CullVisitor>

#include <components/resource/scenemanager.hpp>

namespace Fx
{
    std::string StateUpdater::sDefinition = UniformData::getDefinition("_omw_data");

    StateUpdater::StateUpdater(bool useUBO)
        : mUseUBO(useUBO)
    {
    }

    void StateUpdater::setDefaults(osg::StateSet* stateset)
    {
        if (mUseUBO)
        {
            osg::ref_ptr<osg::UniformBufferObject> ubo = new osg::UniformBufferObject;

            osg::ref_ptr<osg::BufferTemplate<UniformData::BufferType>> data
                = new osg::BufferTemplate<UniformData::BufferType>();
            data->setBufferObject(ubo);

            osg::ref_ptr<osg::UniformBufferBinding> ubb = new osg::UniformBufferBinding(
                static_cast<int>(Resource::SceneManager::UBOBinding::PostProcessor), data, 0, mData.getGPUSize());

            stateset->setAttributeAndModes(ubb, osg::StateAttribute::ON);
        }
        else
        {
            const auto createUniform = [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                std::string name = "omw." + std::string(T::sName);
                stateset->addUniform(new osg::Uniform(name.c_str(), mData.get<T>()));
            };

            std::apply([&](const auto&... v) { (createUniform(v), ...); }, mData.getData());
        }
    }

    void StateUpdater::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        if (mUseUBO)
        {
            osg::UniformBufferBinding* ubb = dynamic_cast<osg::UniformBufferBinding*>(
                stateset->getAttribute(osg::StateAttribute::UNIFORMBUFFERBINDING,
                    static_cast<int>(Resource::SceneManager::UBOBinding::PostProcessor)));
            if (!ubb)
                throw std::runtime_error("StateUpdater::apply: failed to get an UniformBufferBinding!");

            auto& dest = static_cast<osg::BufferTemplate<UniformData::BufferType>*>(ubb->getBufferData())->getData();
            mData.copyTo(dest);

            ubb->getBufferData()->dirty();
        }
        else
        {
            const auto setUniform = [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                std::string name = "omw." + std::string(T::sName);
                stateset->getUniform(name)->set(mData.get<T>());
            };

            std::apply([&](const auto&... v) { (setUniform(v), ...); }, mData.getData());
        }

        if (mPointLightBuffer)
            mPointLightBuffer->applyUniforms(nv->getTraversalNumber(), stateset);
    }

    osg::ref_ptr<osg::StateSet> StateUpdater::ownedFrame(osgUtil::CullVisitor* cv)
    {
        if (!cv) return nullptr;
        const auto* source = getCvDependentStateset(cv);
        osg::ref_ptr<osg::StateSet> current = new osg::StateSet(*source);
        if (mPointLightBuffer)
        {
            // The legacy updater adds these uniforms only once. An acquired
            // visitor can return on a different frame parity after skipped or
            // repeated IDs, so capture the actual current provider arrays.
            current->removeUniform("omw_PointLights");
            current->removeUniform("omw_PointLightsCount");
            mPointLightBuffer->applyUniforms(cv->getTraversalNumber(), current);
        }
        osg::ref_ptr<osg::StateSet> result = new osg::StateSet(*current,
            osg::CopyOp::DEEP_COPY_UNIFORMS | osg::CopyOp::DEEP_COPY_ARRAYS);
        for (const auto& [key, attribute] : source->getAttributeList())
        {
            const auto* binding = dynamic_cast<const osg::UniformBufferBinding*>(attribute.first.get());
            if (!binding || typeid(*binding) != typeid(osg::UniformBufferBinding)) return nullptr;
            const auto* data = dynamic_cast<const osg::BufferTemplate<UniformData::BufferType>*>(binding->getBufferData());
            if (!data) return nullptr;
            // A generic StateSet deep copy does not guarantee private GPU
            // storage for BufferData. Copy the actual data and give it a new
            // buffer object explicitly, preserving the binding's range.
            osg::ref_ptr<osg::BufferTemplate<UniformData::BufferType>> captured
                = new osg::BufferTemplate<UniformData::BufferType>(*data);
            captured->setBufferObject(new osg::UniformBufferObject);
            osg::ref_ptr<osg::UniformBufferBinding> owned = new osg::UniformBufferBinding(*binding);
            owned->setBufferData(captured);
            result->setAttribute(owned, attribute.second);
        }
        return result;
    }

    void StateUpdater::installOwnedFrame(osg::StateSet& destination, const osg::StateSet& captured,
        osg::StateSet& retiredSlotBindings)
    {
        for (const auto& [name, uniform] : captured.getUniformList())
            destination.addUniform(uniform.first, uniform.second);
        for (const auto& [key, attribute] : captured.getAttributeList())
        {
            const auto* source = dynamic_cast<const osg::UniformBufferBinding*>(attribute.first.get());
            const auto* data = source
                ? dynamic_cast<const osg::BufferTemplate<UniformData::BufferType>*>(source->getBufferData()) : nullptr;
            if (!data) throw std::runtime_error("Unsupported finalized PostFX buffer binding");
            auto* binding = dynamic_cast<osg::UniformBufferBinding*>(
                retiredSlotBindings.getAttribute(key.first, key.second));
            auto* retiredData = binding
                ? dynamic_cast<osg::BufferTemplate<UniformData::BufferType>*>(binding->getBufferData()) : nullptr;
            if (!retiredData || binding->getTarget() != source->getTarget()
                || binding->getOffset() != source->getOffset() || binding->getSize() != source->getSize())
            {
                osg::ref_ptr<osg::BufferTemplate<UniformData::BufferType>> replacement
                    = new osg::BufferTemplate<UniformData::BufferType>(*data);
                replacement->setBufferObject(new osg::UniformBufferObject);
                osg::ref_ptr<osg::UniformBufferBinding> owned = new osg::UniformBufferBinding(*source);
                owned->setBufferData(replacement);
                retiredSlotBindings.setAttribute(owned, attribute.second);
                binding = owned;
            }
            else
            {
                // The exact acquired SceneView's previous draw has retired.
                // Reuse only its private GPU buffer, never another slot/parity.
                retiredData->setData(data->getData());
            }
            destination.setAttribute(binding, attribute.second);
        }
    }
}
