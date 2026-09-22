// No Vulkan device: exercise the pinned allocator/Value upload boundary.
#include <components/render/backend/vsg/legacymaterialshader.hpp>
#include <components/render/backend/vsg/legacybumpmaterialshader.hpp>
#include <components/render/backend/vsg/enchantedmaterialshader.hpp>
#include <vsg/core/Array.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
int main()
{
    try
    {
        // VSG 1.1.15 IntrusiveAllocator defaults to 8-byte object alignment.
        // GPU std140 offsets are checked in the owning headers, independently.
        if (alignof(RenderVsg::LegacyMaterialUniformValue)>8
            || alignof(RenderVsg::LegacyBumpUniformValue)>8
            || alignof(RenderVsg::EnchantedEnvironmentUniformValue)>8)
            throw std::runtime_error("VSG uniform Value requests unsupported host over-alignment");
        std::vector<vsg::ref_ptr<vsg::Data>> live;
        for (unsigned i=0;i<256;++i)
        {
            live.push_back(vsg::ubyteArray::create(1+i%17));
            RenderCore::MaterialRecord material;
            material.ambient={.25f,.5f,.75f,1.f}; material.shininess=float(i);
            auto value=RenderVsg::makeLegacyCompatibilityMaterial(material);
            if(value->dataSize()!=160 || value->value().parameters.x!=float(i))
                throw std::runtime_error("material payload or scalar lost at upload boundary");
            float rows[40]{};std::memcpy(rows,value->dataPointer(),sizeof(rows));
            if(rows[0]!=.25f || rows[1]!=.5f || rows[2]!=.75f || rows[16]!=float(i))
                throw std::runtime_error("std140 payload offsets changed");
            auto bump=RenderVsg::LegacyBumpUniformValue::create();
            auto enchant=RenderVsg::EnchantedEnvironmentUniformValue::create();
            if(bump->dataSize()!=32 || enchant->dataSize()!=32)
                throw std::runtime_error("secondary uniform payload size changed");
            live.push_back(value);live.push_back(bump);live.push_back(enchant);
        }
        std::cout<<"PASS uniform host alignment and unchanged std140 offsets across 256 mixed allocations\n";
        return 0;
    }
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
