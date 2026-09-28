#ifndef OPENMW_RENDERCORE_TEMPORALFRAME_H
#define OPENMW_RENDERCORE_TEMPORALFRAME_H

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace RenderCore::Temporal
{
    struct Extent
    {
        std::uint32_t width = 0, height = 0;
        [[nodiscard]] bool valid() const noexcept { return width != 0 && height != 0; }
        friend bool operator==(const Extent&, const Extent&) = default;
    };

    struct ViewIdentity
    {
        std::uint64_t view = 0, worldEpoch = 0, cameraEpoch = 0, projectionEpoch = 0;
        friend bool operator==(const ViewIdentity&, const ViewIdentity&) = default;
    };

    enum Reset : std::uint32_t
    {
        NoReset = 0, FirstFrame = 1u << 0, FrameGap = 1u << 1,
        ViewChange = 1u << 2, WorldChange = 1u << 3, CameraChange = 1u << 4,
        ProjectionChange = 1u << 5, ExtentChange = 1u << 6,
        ExplicitCut = 1u << 7, InvalidPreviousInput = 1u << 8,
    };

    enum class DepthRange { NegativeOneToOne, ZeroToOne };

    // This is the final rendered camera, not a mutable update-side camera.
    // GLM uses column vectors here. Both matrices MUST exclude sampling jitter.
    struct FrameInput
    {
        ViewIdentity identity;
        std::uint64_t frame = 0;
        Extent render, output;
        glm::dmat4 view{1.0}, projection{1.0};
        DepthRange depthRange = DepthRange::NegativeOneToOne;
        bool cameraCut = false;
        bool jitterEnabled = true;
        std::uint32_t jitterPeriod = 8;
    };

    struct Frame
    {
        FrameInput input;
        std::uint64_t ticket = 0, previousFrame = 0;
        std::uint32_t resetReasons = FirstFrame;
        glm::dvec2 jitterPixels{0.0}, previousJitterPixels{0.0};
        glm::dmat4 currentViewProjection{1.0}, previousViewProjection{1.0};
        glm::dmat4 inverseViewProjection{1.0}, clipToPreviousClip{1.0};
        glm::dmat4 jitteredProjection{1.0};
        [[nodiscard]] bool hasHistory() const noexcept { return resetReasons == NoReset; }
    };

    inline bool finite(const glm::dmat4& matrix) noexcept
    {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (!std::isfinite(matrix[c][r])) return false;
        return true;
    }

    inline double halton(std::uint64_t index, std::uint32_t base) noexcept
    {
        if (base < 2) return 0.0;
        double factor = 1.0, value = 0.0;
        while (index != 0)
        {
            factor /= base;
            value += factor * static_cast<double>(index % base);
            index /= base;
        }
        return value;
    }

    // Pixel jitter is positive right/down. GL clip-space Y points upwards.
    inline glm::dmat4 withJitter(const glm::dmat4& projection, Extent extent, glm::dvec2 pixels)
    {
        glm::dmat4 shift{1.0};
        shift[3][0] = 2.0 * pixels.x / extent.width;
        shift[3][1] = -2.0 * pixels.y / extent.height;
        return shift * projection;
    }

    // One instance per independently rendered view, on its render owner thread.
    // prepare() never advances history. Only a successfully submitted temporal
    // evaluation commits it. Aborted/skipped views cannot become previous data.
    class History final
    {
    public:
        [[nodiscard]] std::optional<Frame> prepare(const FrameInput& input)
        {
            if (mPending || mNextTicket == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
            if (mLast && input.frame <= mLast->input.frame) return std::nullopt;
            if (!input.identity.view || !input.render.valid() || !input.output.valid()
                || input.render.width > input.output.width || input.render.height > input.output.height
                || input.jitterPeriod < 2 || input.jitterPeriod > 1024 || !finite(input.view) || !finite(input.projection))
                return invalidInput();
            const glm::dmat4 vp = input.projection * input.view;
            const double determinant = glm::determinant(vp);
            if (!std::isfinite(determinant) || determinant == 0.0) return invalidInput();
            const glm::dmat4 inverse = glm::inverse(vp);
            if (!finite(inverse)) return invalidInput();
            const glm::dmat4 identity = vp * inverse;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    if (std::abs(identity[c][r] - (c == r ? 1.0 : 0.0)) > 1e-7) return invalidInput();

            Frame next;
            next.input = input;
            next.ticket = ++mNextTicket;
            next.resetReasons = mInvalidated ? InvalidPreviousInput : NoReset;
            if (!mLast) next.resetReasons |= FirstFrame;
            if (input.cameraCut) next.resetReasons |= ExplicitCut;
            if (mLast)
            {
                const auto& previous = mLast->input;
                if (previous.frame == std::numeric_limits<std::uint64_t>::max()
                    || input.frame != previous.frame + 1) next.resetReasons |= FrameGap;
                if (input.identity.view != previous.identity.view) next.resetReasons |= ViewChange;
                if (input.identity.worldEpoch != previous.identity.worldEpoch) next.resetReasons |= WorldChange;
                if (input.identity.cameraEpoch != previous.identity.cameraEpoch) next.resetReasons |= CameraChange;
                if (input.identity.projectionEpoch != previous.identity.projectionEpoch
                    || input.depthRange != previous.depthRange || input.jitterEnabled != previous.jitterEnabled
                    || input.jitterPeriod != previous.jitterPeriod) next.resetReasons |= ProjectionChange;
                if (input.render != previous.render || input.output != previous.output) next.resetReasons |= ExtentChange;
            }
            const auto sample = (next.hasHistory() ? mCommittedSamples : 0) % input.jitterPeriod + 1;
            if (input.jitterEnabled) next.jitterPixels = {halton(sample, 2) - .5, halton(sample, 3) - .5};
            next.currentViewProjection = vp;
            next.inverseViewProjection = inverse;
            next.previousViewProjection = next.hasHistory() ? mLast->currentViewProjection : vp;
            next.previousFrame = next.hasHistory() ? mLast->input.frame : input.frame;
            next.previousJitterPixels = next.hasHistory() ? mLast->jitterPixels : next.jitterPixels;
            next.clipToPreviousClip = next.previousViewProjection * inverse;
            next.jitteredProjection = withJitter(input.projection, input.render, next.jitterPixels);
            mPending = next;
            return next;
        }

        bool commit(std::uint64_t ticket)
        {
            if (!mPending || mPending->ticket != ticket) return false;
            if (!mPending->hasHistory()) mCommittedSamples = 0;
            ++mCommittedSamples;
            mLast = std::move(mPending);
            mPending.reset();
            mInvalidated = false;
            return true;
        }
        bool abort(std::uint64_t ticket)
        {
            if (!mPending || mPending->ticket != ticket) return false;
            mPending.reset();
            return true;
        }
        void invalidate() noexcept
        {
            mLast.reset(); mPending.reset(); mCommittedSamples = 0; mInvalidated = true;
        }
    private:
        std::optional<Frame> invalidInput() { invalidate(); return std::nullopt; }
        std::optional<Frame> mLast, mPending;
        std::uint64_t mNextTicket = 0, mCommittedSamples = 0;
        bool mInvalidated = false;
    };

    inline std::optional<glm::dvec2> pixelPosition(const glm::dvec4& clip, Extent extent)
    {
        if (!extent.valid() || !std::isfinite(clip.w) || clip.w <= 1e-12) return std::nullopt;
        const glm::dvec2 xy{clip.x / clip.w, clip.y / clip.w};
        if (!std::isfinite(xy.x) || !std::isfinite(xy.y)) return std::nullopt;
        return glm::dvec2{(xy.x + 1.0) * .5 * extent.width, (1.0 - xy.y) * .5 * extent.height};
    }

    // Current-to-previous flow, in unjittered render pixels, positive right/down.
    // This helper does not discover deformation: callers must supply the actual
    // previous vertex/transform for dynamic geometry, or declare it unsupported.
    inline std::optional<glm::dvec2> worldMotion(const Frame& frame,
        const glm::dvec3& currentWorld, const glm::dvec3& previousWorld)
    {
        const auto current = pixelPosition(frame.currentViewProjection * glm::dvec4(currentWorld, 1.0), frame.input.render);
        const auto previous = pixelPosition(frame.previousViewProjection * glm::dvec4(previousWorld, 1.0), frame.input.render);
        if (!current || !previous) return std::nullopt;
        if (!frame.hasHistory()) return glm::dvec2{0.0};
        return *previous - *current;
    }

    // Static geometry only. pixel is a jittered raster location; windowDepth
    // comes from that exact frame's depth buffer. Reverse Z is represented by
    // the supplied projection, never guessed or inverted a second time.
    inline std::optional<glm::dvec2> staticMotionFromDepth(const Frame& frame,
        glm::dvec2 pixel, double windowDepth)
    {
        if (!frame.input.render.valid() || !std::isfinite(pixel.x) || !std::isfinite(pixel.y)
            || !std::isfinite(windowDepth) || windowDepth < 0.0 || windowDepth > 1.0) return std::nullopt;
        const double z = frame.input.depthRange == DepthRange::NegativeOneToOne ? windowDepth * 2.0 - 1.0 : windowDepth;
        const glm::dvec2 unjitteredPixel = pixel - frame.jitterPixels;
        const glm::dvec4 clip{2.0 * unjitteredPixel.x / frame.input.render.width - 1.0,
            1.0 - 2.0 * unjitteredPixel.y / frame.input.render.height, z, 1.0};
        const auto previous = pixelPosition(frame.clipToPreviousClip * clip, frame.input.render);
        if (!previous) return std::nullopt;
        if (!frame.hasHistory()) return glm::dvec2{0.0};
        return *previous - unjitteredPixel;
    }

    // Adapter utility only: Streamline-facing storage is row-major. This does
    // not change the mathematical transform convention used above.
    inline std::array<float, 16> rowMajor(const glm::dmat4& value)
    {
        std::array<float,16> result{};
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                result[row * 4 + col] = static_cast<float>(value[col][row]);
        return result;
    }
}
#endif
