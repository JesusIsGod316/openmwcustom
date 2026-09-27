#include <components/render/backend/vsg/locallighttiles.hpp>

#include <vsg/maths/transform.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    void conservativeSphereCoverage()
    {
        constexpr std::uint32_t width = 1920, height = 1080;
        const auto projection = vsg::perspective(1.1, double(width) / height, 0.1, 1000.0);
        std::mt19937 rng(1776);
        std::uniform_real_distribution<double> lateral(-40.0, 40.0), depth(5.0, 100.0), radius(0.1, 7.0);
        std::uniform_real_distribution<double> offset(-1.0, 1.0);
        std::vector<RenderVsg::LocalLightTileSource> lights;
        for (int i = 0; i < 90; ++i)
            lights.push_back({{lateral(rng), lateral(rng), -depth(rng)}, float(radius(rng)), 1.0f});
        const auto grid = RenderVsg::buildLocalLightTileGrid(lights, projection, 0.1, width, height, 20480, true);
        require(grid.active() && grid.columns == 60 && grid.rows == 34, "tile grid not built");

        unsigned visibleSamples = 0;
        for (std::size_t light = 0; light < lights.size(); ++light)
            for (unsigned sample = 0; sample < 200; ++sample)
            {
                const auto& source = lights[light];
                const double r = source.radius * 0.999;
                const vsg::dvec3 point = source.eyePosition
                    + vsg::dvec3(offset(rng) * r, offset(rng) * r, offset(rng) * r);
                if (vsg::length(point - source.eyePosition) > r) continue;
                const vsg::dvec4 clip = projection * vsg::dvec4(point.x, point.y, point.z, 1.0);
                if (clip.w <= 0.0) continue;
                const double px = (clip.x / clip.w + 1.0) * 0.5 * width;
                const double py = (clip.y / clip.w + 1.0) * 0.5 * height;
                if (px < 0.0 || py < 0.0 || px >= width || py >= height) continue;
                ++visibleSamples;
                require(grid.includes(light, std::uint32_t(px), std::uint32_t(py)),
                    "finite-radius light omitted from a fragment tile");
            }
        require(visibleSamples > 1000, "coverage test did not exercise enough visible samples");
    }

    void failOpenAndFallback()
    {
        const auto projection = vsg::perspective(1.0, 1.0, 0.1, 100.0);
        std::vector<RenderVsg::LocalLightTileSource> lights(32,
            {{0.0, 0.0, -20.0}, 0.25f, 1.0f});
        lights[0] = {{0.0, 0.0, -0.2}, 1.0f, 1.0f}; // Near-plane crossing.
        lights[1] = {{0.0, 0.0, -20.0}, std::numeric_limits<float>::infinity(), 1.0f};
        lights[2] = {{0.0, 0.0, -20.0}, 2.0f, 0.0f};
        lights[3] = {{0.0, 0.0, -20.0}, -1.0f, 1.0f};
        const auto grid = RenderVsg::buildLocalLightTileGrid(lights, projection, 0.1, 256, 256, 20480, true);
        require(grid.active(), "expected active conservative grid");
        for (std::uint32_t y = 0; y < 256; y += 31)
            for (std::uint32_t x = 0; x < 256; x += 31)
            {
                require(grid.includes(0, x, y) && grid.includes(1, x, y),
                    "unbounded/near light must select every tile");
                require(!grid.includes(2, x, y) && !grid.includes(3, x, y),
                    "fragment-rejected light need not select a tile");
            }
        require(!RenderVsg::buildLocalLightTileGrid(lights, projection, 0.1, 256, 256, 1, true).active(),
            "capacity failure must select full-loop fallback");
        require(!RenderVsg::buildLocalLightTileGrid(lights, projection, 0.1, 256, 256, 20480, false).active(),
            "disabled radius fade must select full-loop fallback");
        require(!RenderVsg::buildLocalLightTileGrid(lights, projection, 0.1, 0, 256, 20480, true).active(),
            "empty viewport must select full-loop fallback");
    }
}

int main()
{
    try
    {
        conservativeSphereCoverage();
        failOpenAndFallback();
        std::cout << "conservative local-light tiles passed\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
