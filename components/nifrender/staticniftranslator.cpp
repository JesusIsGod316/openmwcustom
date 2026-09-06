#include "niftranslator.hpp"

#include "materialpass.hpp"

#include <components/nif/niffile.hpp>
#include <components/vfs/manager.hpp>

namespace NifRender
{
    TranslationBundle translateStaticNif(Nif::FileView file, const VFS::Manager& vfs, TranslatorOptions options)
    {
        TranslationBundle result = translateNif(file, options);
        applyStaticMaterialPass(file, vfs, result);
        return result;
    }
}
