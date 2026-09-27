#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_LOCALLIGHTTILES_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_LOCALLIGHTTILES_H

#include <vsg/maths/mat4.h>
#include <vsg/maths/vec4.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace RenderVsg
{
    inline constexpr std::uint32_t LocalLightTileSize = 32;

    struct LocalLightTileSource
    {
        vsg::dvec3 eyePosition;
        float radius = 0.0f;
        float fade = 0.0f;
    };

    // A conservative screen-space light list. Bit n of each tile denotes the
    // n-th light in the existing five-vec4 buffer, so material/light semantics
    // remain in their original records. Invalid and near-plane bounds fail open.
    struct LocalLightTileGrid
    {
        std::uint32_t columns = 0;
        std::uint32_t rows = 0;
        std::size_t wordsPerTile = 0;
        std::size_t vec4sPerTile = 0;
        std::vector<std::uint32_t> words;

        [[nodiscard]] bool active() const noexcept { return columns != 0 && !words.empty(); }

        [[nodiscard]] bool includes(std::size_t light, std::uint32_t x, std::uint32_t y) const noexcept
        {
            if (!active() || x >= columns * LocalLightTileSize || y >= rows * LocalLightTileSize
                || light / 32 >= wordsPerTile)
                return false;
            const std::size_t tile = (y / LocalLightTileSize) * columns + x / LocalLightTileSize;
            return (words[tile * vec4sPerTile * 4 + light / 32] & (1u << (light % 32))) != 0;
        }
    };

    [[nodiscard]] inline LocalLightTileGrid buildLocalLightTileGrid(std::span<const LocalLightTileSource> lights,
        const vsg::dmat4& projection, double nearPlane, std::uint32_t width, std::uint32_t height,
        std::size_t availableVec4s, bool radiusFadeEnabled)
    {
        LocalLightTileGrid result;
        // Small light sets and unusual views retain the original shader loop.
        if (!radiusFadeEnabled || lights.size() < 16 || width == 0 || height == 0
            || !(nearPlane > 0.0) || !std::isfinite(nearPlane) || lights.size() > 4096)
            return result;

        const std::size_t columns = (std::size_t(width) + LocalLightTileSize - 1) / LocalLightTileSize;
        const std::size_t rows = (std::size_t(height) + LocalLightTileSize - 1) / LocalLightTileSize;
        const std::size_t wordsPerTile = (lights.size() + 31) / 32;
        const std::size_t vec4sPerTile = (wordsPerTile + 3) / 4;
        const std::size_t lightVec4s = lights.size() * 5;
        if (columns > std::numeric_limits<std::uint32_t>::max()
            || rows > std::numeric_limits<std::uint32_t>::max() || columns > 4096 || rows > 4096
            || lightVec4s > availableVec4s
            || columns > (availableVec4s - lightVec4s) / vec4sPerTile / rows)
            return result;

        result.columns = static_cast<std::uint32_t>(columns);
        result.rows = static_cast<std::uint32_t>(rows);
        result.wordsPerTile = wordsPerTile;
        result.vec4sPerTile = vec4sPerTile;
        result.words.resize(columns * rows * vec4sPerTile * 4);

        const auto mark = [&](std::size_t light, std::size_t minX, std::size_t maxX,
                              std::size_t minY, std::size_t maxY) {
            for (std::size_t y = minY; y <= maxY; ++y)
                for (std::size_t x = minX; x <= maxX; ++x)
                    result.words[(y * columns + x) * vec4sPerTile * 4 + light / 32]
                        |= 1u << (light % 32);
        };
        const auto markAll = [&](std::size_t light) { mark(light, 0, columns - 1, 0, rows - 1); };

        for (std::size_t light = 0; light < lights.size(); ++light)
        {
            const LocalLightTileSource& source = lights[light];
            if (source.fade < 0.001f)
                continue; // The original fragment shader skips this light too.
            if (!std::isfinite(source.radius) || !std::isfinite(source.eyePosition.x)
                || !std::isfinite(source.eyePosition.y) || !std::isfinite(source.eyePosition.z))
            {
                markAll(light);
                continue;
            }
            if (source.radius <= 0.0f)
                continue; // Radius fade already excludes non-positive radii.

            // Project the eight corners of a *larger* eye-space AABB. When it
            // intersects the near plane, projecting corners is not a bound:
            // select every tile rather than risk losing a visible light.
            const double radius = double(source.radius) * 1.001 + 0.01;
            if (source.eyePosition.z + radius >= -nearPlane)
            {
                markAll(light);
                continue;
            }
            double minX = std::numeric_limits<double>::infinity();
            double maxX = -minX, minY = minX, maxY = -minX;
            bool uncertain = false;
            for (int z : {-1, 1})
                for (int y : {-1, 1})
                    for (int x : {-1, 1})
                    {
                        const vsg::dvec4 corner(source.eyePosition.x + x * radius,
                            source.eyePosition.y + y * radius, source.eyePosition.z + z * radius, 1.0);
                        const vsg::dvec4 clip = projection * corner;
                        if (!(clip.w > 0.0) || !std::isfinite(clip.x) || !std::isfinite(clip.y)
                            || !std::isfinite(clip.w))
                        {
                            uncertain = true;
                            continue;
                        }
                        const double px = (clip.x / clip.w + 1.0) * 0.5 * width;
                        const double py = (clip.y / clip.w + 1.0) * 0.5 * height;
                        if (!std::isfinite(px) || !std::isfinite(py))
                        {
                            uncertain = true;
                            continue;
                        }
                        minX = std::min(minX, px);
                        maxX = std::max(maxX, px);
                        minY = std::min(minY, py);
                        maxY = std::max(maxY, py);
                    }
            if (uncertain)
            {
                markAll(light);
                continue;
            }
            // Pixel and float-upload allowance; tile boundaries are inclusive.
            minX -= 2.0;
            maxX += 2.0;
            minY -= 2.0;
            maxY += 2.0;
            if (maxX < 0.0 || maxY < 0.0 || minX >= width || minY >= height)
                continue;
            const auto tileX = [&](double px) {
                return static_cast<std::size_t>(std::clamp(px / LocalLightTileSize, 0.0, double(columns - 1)));
            };
            const auto tileY = [&](double py) {
                return static_cast<std::size_t>(std::clamp(py / LocalLightTileSize, 0.0, double(rows - 1)));
            };
            mark(light, tileX(minX), tileX(maxX), tileY(minY), tileY(maxY));
        }
        return result;
    }
}

#endif
