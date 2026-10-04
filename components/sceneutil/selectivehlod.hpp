#ifndef OPENMW_COMPONENTS_SCENEUTIL_SELECTIVEHLOD_HPP
#define OPENMW_COMPONENTS_SCENEUTIL_SELECTIVEHLOD_HPP

#include <osg/Node>
#include <osg/ref_ptr>

#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <atomic>

namespace Resource::AssetGpuBudget { class Reservation; }

namespace SceneUtil
{
    // This certificate must be obtained from the original loader template, before
    // paging CopyOp erases controllers and replaces rig/morph drawables.
    bool certifySelectiveHlodTemplate(const osg::Node& original);
    // Cheap material/layout sieve, with no hash, image decode, disk or cook.
    // Apply alongside the ancestry certificate before source partitioning.
    bool qualifySelectiveHlodTemplate(const osg::Node& original, bool requireShippedObjectShader = true);
    struct HlodCounters
    {
        inline static std::atomic_uint64_t requests{0}, certified{0}, built{0}, refused{0};
        inline static std::atomic_uint64_t controlSubmissions{0}, proxySubmissions{0}, atlasBytes{0};
        inline static std::atomic_uint64_t consumedDraws{0}, consumedIndices{0};
    };

    struct HlodBuildOptions
    {
        std::size_t maxSourceGeometries = 64;
        std::size_t maxVertices = 250000;
        std::size_t maxHostBytes = 256 * 1024 * 1024;
        float maxClusterRadius = 4096.f;
        // Frozen far-atlas recipe: 256 texels/chart, 128 texels/gutter,
        // at most 16 charts and three 2048-square RGBA8 complete mip chains.
        // Co-registered diffuse, linear normal and linear specular roles only;
        // identity texture transforms, complete 1/2/4 REPEAT periods or in-range
        // CLAMP UVs. Parallax, reconstruction and additional maps stay original.
        // Source anisotropy is preserved up to 16x; sampling is capped while
        // the chart gutter still covers that bounded filter footprint. Lower mips
        // exist for completeness but are never sampled across chart boundaries.
        bool requireShippedObjectShader = true;
        // Fixtures can prepare an unsubmitted CPU representation without a
        // sampled adapter. Production always reserves shared GPU debt.
        bool reserveGpu = true;
    };

    struct HlodBuildResult
    {
        osg::ref_ptr<osg::Node> representation;
        std::uint64_t controlSubmissions = 0;
        std::uint64_t proxySubmissions = 0;
        std::uint64_t sourceVertices = 0;
        std::uint64_t sourceTriangles = 0;
        std::uint64_t distinctDiffuseImages = 0;
        std::uint64_t atlasBytes = 0;
        std::string rejection;
        bool retryable = false;
        std::shared_ptr<Resource::AssetGpuBudget::Reservation> gpuReservation;
        explicit operator bool() const { return representation.valid(); }
    };

    // Optional preparation only. Caller owns an immutable, deeply private
    // source partition certified before CopyOp, excluding authored LOD and
    // occluder owners. optimizedControl is that same partition AFTER the normal
    // strong optimizer, not a fabricated pre-merge submission baseline.
    // Null means use every original member. The result never reparents originals.
    HlodBuildResult buildSelectiveHlod(const osg::Node& certifiedSource,
        const osg::Node& optimizedControl, const HlodBuildOptions& options = {});
}

#endif
