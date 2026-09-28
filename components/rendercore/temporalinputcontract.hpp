#ifndef OPENMW_RENDERCORE_TEMPORALINPUTCONTRACT_H
#define OPENMW_RENDERCORE_TEMPORALINPUTCONTRACT_H

#include "temporalframe.hpp"

namespace RenderCore::Temporal
{
    enum class Format { Unknown, Rgba8, Rgba16Float, Rg16Float, Rg32Float, Depth32Float };
    enum class MotionSpace { Unknown, CurrentToPreviousRenderPixels };
    struct ImageInput
    {
        std::uint64_t id = 0, revision = 0, frame = 0, view = 0, device = 0;
        Extent extent;
        Format format = Format::Unknown;
    };
    struct Inputs
    {
        ImageInput color, depth, motion, output;
        MotionSpace motionSpace = MotionSpace::Unknown;
        bool colorExcludesHud = false;
        bool dynamicMotionComplete = false;
        bool exposureAvailable = false;
        bool allowAutoExposure = false;
        bool hdr = false;
    };
    enum class InputStatus
    {
        Valid, InvalidFrame, MissingImage, MismatchedFrame, MismatchedView,
        MismatchedDevice, InvalidExtent, InvalidFormat, AliasedImages,
        UnsupportedMotion, IncompleteMotion, HudInColor, MissingExposure,
    };

    // Metadata gate only. A graphics adapter MUST additionally validate actual
    // handles, device capabilities, layouts, producer completion and lifetimes.
    // Passing this gate is not evidence of an installed/running DLSS feature.
    inline InputStatus validateInputs(const Frame& frame, const Inputs& inputs)
    {
        if (!frame.ticket || !frame.input.identity.view || !frame.input.render.valid() || !frame.input.output.valid())
            return InputStatus::InvalidFrame;
        const std::array images{inputs.color, inputs.depth, inputs.motion, inputs.output};
        for (std::size_t i = 0; i < images.size(); ++i)
        {
            const auto& image = images[i];
            if (!image.id || !image.revision || !image.device) return InputStatus::MissingImage;
            if (image.frame != frame.input.frame) return InputStatus::MismatchedFrame;
            if (image.view != frame.input.identity.view) return InputStatus::MismatchedView;
            if (image.device != inputs.color.device) return InputStatus::MismatchedDevice;
            if (image.extent != (i == 3 ? frame.input.output : frame.input.render)) return InputStatus::InvalidExtent;
            for (std::size_t j = 0; j < i; ++j)
                if (image.id == images[j].id) return InputStatus::AliasedImages;
        }
        const auto colorFormat = [](Format f) { return f == Format::Rgba8 || f == Format::Rgba16Float; };
        if (!colorFormat(inputs.color.format) || !colorFormat(inputs.output.format)
            || inputs.depth.format != Format::Depth32Float
            || (inputs.motion.format != Format::Rg16Float && inputs.motion.format != Format::Rg32Float)
            || (inputs.hdr && (inputs.color.format != Format::Rgba16Float || inputs.output.format != Format::Rgba16Float)))
            return InputStatus::InvalidFormat;
        if (inputs.motionSpace != MotionSpace::CurrentToPreviousRenderPixels) return InputStatus::UnsupportedMotion;
        if (!inputs.dynamicMotionComplete) return InputStatus::IncompleteMotion;
        if (!inputs.colorExcludesHud) return InputStatus::HudInColor;
        if (!inputs.exposureAvailable && !inputs.allowAutoExposure) return InputStatus::MissingExposure;
        return InputStatus::Valid;
    }
}
#endif
