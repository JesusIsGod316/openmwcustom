#ifndef OPENMW_COMPONENTS_NIFRENDER_NIFTRANSLATOR_H
#define OPENMW_COMPONENTS_NIFRENDER_NIFTRANSLATOR_H

#include "translationbundle.hpp"

namespace Nif
{
    class FileView;
}

namespace NifRender
{
    struct TranslatorOptions
    {
        // Matches normal game loading. Editor-only marker geometry remains
        // classified and diagnosed, but is not emitted as visible geometry.
        bool showMarkers = false;
    };

    // Translate the already-parsed NIF view into unpublished immutable neutral
    // data. This function never mutates RenderWorld and never produces OSG/VSG
    // backend objects. Stable RenderCore handles are assigned only by the later
    // deterministic publication/binding stage.
    [[nodiscard]] TranslationBundle translateNif(Nif::FileView file, TranslatorOptions options = {});
}

#endif
