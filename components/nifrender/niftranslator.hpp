#ifndef OPENMW_COMPONENTS_NIFRENDER_NIFTRANSLATOR_H
#define OPENMW_COMPONENTS_NIFRENDER_NIFTRANSLATOR_H

#include "translationbundle.hpp"

namespace Nif
{
    class FileView;
}

namespace VFS
{
    class Manager;
}

namespace NifRender
{
    struct TranslatorOptions
    {
        // Matches normal game loading. Editor-only marker geometry remains
        // classified and diagnosed, but is not emitted as visible geometry.
        bool showMarkers = false;
    };

    // Structural translation entry retained for focused source/graph tests.
    // It does not have enough information to resolve VFS-backed textures or
    // external shader materials and therefore is not the production static path.
    [[nodiscard]] TranslationBundle translateNif(Nif::FileView file, TranslatorOptions options = {});

    // Complete CP3B static translation entry: existing parsed FileView plus the
    // winning OpenMW VFS view -> immutable neutral model/material/texture bundle.
    // This prevents production callers from accidentally publishing geometry
    // while omitting VFS texture and BGSM/BGEM resolution.
    [[nodiscard]] TranslationBundle translateStaticNif(
        Nif::FileView file, const VFS::Manager& vfs, TranslatorOptions options = {});
}

#endif
