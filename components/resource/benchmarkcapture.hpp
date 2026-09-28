#ifndef OPENMW_RESOURCE_BENCHMARKCAPTURE_H
#define OPENMW_RESOURCE_BENCHMARKCAPTURE_H

#include <components/debug/deferredcapture.hpp>
#include <osg/Stats>
#include <osgViewer/Viewer>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>

namespace Resource
{
    class BenchmarkCapture
    {
        struct RenderRecord
        {
            unsigned frame, readFrame, camera;
            bool mainCamera;
            double cull, draw, gpu;
            unsigned available;
        };
        static constexpr std::array<const char*, 27> ResourceNames{
            "StateSet",
            "P8G3 Groups Tested",
            "P8G3 Groups Rejected",
            "P8G3 Instances Rejected",
            "P8G3 Full Instances",
            "P8G3 Submitted Instances",
            "P8G3 Drawable Visits",
            "P8G4 LOD2 Full",
            "P8G4 LOD2 Early",
            "P8G4 LOD2 Mid",
            "P8G4 LOD2 Far",
            "P8G4 LOD2 Near Protected",
            "P8G4 LOD2 Prominent Protected",
            "P8G4 LOD2 Tier Limited",
            "P8G4 LOD2 Full Fallback",
            "P8G4 Shadow Built",
            "P8G4 Shadow Wrapped",
            "P8G4 Shadow Built Indices",
            "P8G4 Shadow Proxy Visits 0",
            "P8G4 Shadow Proxy Indices 0",
            "P8G4 Shadow Original Fallback 0",
            "P8G4 Shadow Proxy Visits 1",
            "P8G4 Shadow Proxy Indices 1",
            "P8G4 Shadow Original Fallback 1",
            "P8G4 Shadow Proxy Visits 2",
            "P8G4 Shadow Proxy Indices 2",
            "P8G4 Shadow Original Fallback 2"
        };
        struct ResourceRecord
        {
            unsigned frame;
            std::array<double, ResourceNames.size()> values;
        };
    public:
        BenchmarkCapture()
        {
            if (!Debug::DeferredCapture::enabled()) return;
            mRender.prepare(262144);
            mResources.prepare(8192);
            mCameras.reserve(32);
        }
        ~BenchmarkCapture() { finish(false); }
        void capture(unsigned currentFrame, osgViewer::Viewer& viewer)
        {
            if (!Debug::DeferredCapture::enabled()) return;
            // Read only CPU-side OSG results, NEVER query/wait on the GL context.
            // Missing delayed query results stay NaN, with explicit validity bits.
            constexpr unsigned Delay = 8;
            if (currentFrame < Delay) return;
            const unsigned last = currentFrame - Delay;
            unsigned first = mNextFrame;
            // A loading screen can advance hundreds of frames internally. Results
            // older than OSG's bounded history are counted, never invented as zero.
            const unsigned earliest = viewer.getViewerStats()->getEarliestFrameNumber();
            if (first < earliest) { mUnavailableFrames += earliest - first; first = earliest; }
            mCameras.clear();
            viewer.getCameras(mCameras, false);
            for (unsigned frame = first; frame <= last; ++frame)
            {
                unsigned index = 0;
                for (const auto* camera : mCameras)
                {
                    auto* stats = camera->getStats();
                    if (stats)
                    {
                        RenderRecord record{ frame, currentFrame, index, camera == viewer.getCamera(), NaN, NaN, NaN, 0 };
                        auto read = [&](const char* name, double& output, unsigned bit) {
                            double value;
                            if (stats->getAttribute(frame, name, value) && std::isfinite(value))
                            { output = value * 1000.; record.available |= bit; }
                        };
                        read("Cull traversal time taken", record.cull, 1);
                        read("Draw traversal time taken", record.draw, 2);
                        read("GPU draw time taken", record.gpu, 4);
                        mRender.push(record);
                    }
                    ++index;
                }
                if (frame % 60 == 0)
                {
                    ResourceRecord record; record.frame = frame; record.values.fill(NaN);
                    for (std::size_t i = 0; i < ResourceNames.size(); ++i)
                        viewer.getViewerStats()->getAttribute(frame, ResourceNames[i], record.values[i]);
                    mResources.push(record);
                }
                if (frame == std::numeric_limits<unsigned>::max()) break;
            }
            mNextFrame = last + 1;
            mLastReadFrame = currentFrame;
        }
        void finish(bool normal = true) noexcept
        {
            if (!Debug::DeferredCapture::enabled() || mFinished) return;
            mFinished = true;
            try
            {
                const char* raw = std::getenv("OPENMW_P8G4_CAPTURE_DIR");
                if (!raw || !*raw) return;
                const auto root = std::filesystem::path(reinterpret_cast<const char8_t*>(raw));
                std::ofstream render(root / "p8g4-render.csv"), resources(root / "p8g4-resource.csv");
                render << "frame,read_frame,camera,main_camera,cull_ms,draw_ms,gpu_ms,available_bits\n" << std::fixed << std::setprecision(6);
                for (const auto& record : mRender)
                    render << record.frame << ',' << record.readFrame << ',' << record.camera << ',' << record.mainCamera
                           << ',' << record.cull << ',' << record.draw << ',' << record.gpu << ',' << record.available << '\n';
                resources << "frame";
                for (const char* name : ResourceNames) resources << ',' << name;
                resources << '\n' << std::fixed << std::setprecision(3);
                for (const auto& record : mResources)
                {
                    resources << record.frame;
                    for (double value : record.values) resources << ',' << value;
                    resources << '\n';
                }
                render.flush(); resources.flush();
                std::ofstream status(root / "p8g4-render.capture-status.txt");
                status << "format=p8g4-numeric-v1\nnormal_finish=" << normal << "\nrender_rows=" << mRender.size()
                       << "\nrender_dropped=" << mRender.dropped() << "\nresource_rows=" << mResources.size()
                       << "\nresource_dropped=" << mResources.dropped()
                       << "\nallocation_failed=" << (mRender.allocationFailed() || mResources.allocationFailed())
                       << "\nexpired_origin_frames=" << mUnavailableFrames
                       << "\nlast_read_frame=" << mLastReadFrame << "\nread_delay_frames=8"
                       << "\nterminal_render_frames_not_read=8\nresource_sample_interval=60"
                       << "\noutput_ok=" << (render.good() && resources.good())
                       << "\nproxy_visits_are_cull_visits_not_driver_draws=1\n";
            }
            catch (...) { /* Missing status explicitly means incomplete capture. */ }
        }
    private:
        static constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
        Debug::DeferredCapture::Buffer<RenderRecord> mRender;
        Debug::DeferredCapture::Buffer<ResourceRecord> mResources;
        osgViewer::Viewer::Cameras mCameras;
        unsigned mNextFrame = 0, mLastReadFrame = 0;
        std::size_t mUnavailableFrames = 0;
        bool mFinished = false;
    };
}
#endif
