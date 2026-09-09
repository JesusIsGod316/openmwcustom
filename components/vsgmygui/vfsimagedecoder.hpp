#ifndef OPENMW_COMPONENTS_VSGMYGUI_VFSIMAGEDECODER_H
#define OPENMW_COMPONENTS_VSGMYGUI_VFSIMAGEDECODER_H

#include "texture.hpp"

namespace VFS
{
    class Manager;
}

namespace VsgMyGui
{
    [[nodiscard]] ImageDecoder makeVfsImageDecoder(const VFS::Manager& vfs);
}

#endif
