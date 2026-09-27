#ifndef OPENMW_RENDER_VSG_FRAMEGPUPROFILE_H
#define OPENMW_RENDER_VSG_FRAMEGPUPROFILE_H

#include <components/debug/gameplaydiagnostics.hpp>
#include <vsg/app/CommandGraph.h>
#include <vsg/commands/Command.h>
#include <vsg/vk/CommandBuffer.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/PhysicalDevice.h>
#include <array>
#include <memory>
#include <optional>
#include <string>

namespace RenderVsg
{
    // Observation only: no query WAIT flag, new fence, or queue idle. A slot is
    // read/recycled only after the host's existing completion tracker proves
    // its command buffers finished. Sampling is bounded to eight pending frames.
    struct FrameGpuProfile
    {
        struct Slot
        {
            std::uint64_t frame = 0, diagnosticFrame = 0;
            bool busy = false, began = false, ended = false;
        };
        vsg::ref_ptr<vsg::Device> device;
        VkQueryPool pool = VK_NULL_HANDLE;
        std::array<Slot, 8> slots{};
        std::optional<unsigned> recording;
        std::string name;
        double period = 0;
        std::uint64_t mask = ~std::uint64_t(0);

        FrameGpuProfile(vsg::Device* value, unsigned family, std::string label)
            : device(value), name(std::move(label))
        {
            const auto physical = device->getPhysicalDevice()->vk();
            std::uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
            if (family >= count || families[family].timestampValidBits == 0) return;
            const auto bits = families[family].timestampValidBits;
            if (bits < 64) mask = (std::uint64_t(1) << bits) - 1;
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(physical, &properties);
            period = properties.limits.timestampPeriod;
            VkQueryPoolCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = static_cast<std::uint32_t>(slots.size() * 2);
            if (vkCreateQueryPool(device->vk(), &info, nullptr, &pool) != VK_SUCCESS)
                pool = VK_NULL_HANDLE;
        }
        ~FrameGpuProfile()
        {
            if (pool) vkDestroyQueryPool(device->vk(), pool, nullptr);
        }
        void prepare(std::uint64_t frame, std::optional<std::uint64_t> completed)
        {
            recording.reset();
            if (!pool) return;
            for (unsigned i = 0; i < slots.size(); ++i)
            {
                auto& slot = slots[i];
                if (!slot.busy || !completed || slot.frame > *completed) continue;
                if (!slot.began || !slot.ended)
                {
                    Debug::GameplayDiagnostics::recordEvent("profile_gpu_skipped", {
                        {"name", name}, {"source_frame", std::to_string(slot.diagnosticFrame)},
                        {"began", std::to_string(slot.began)}, {"ended", std::to_string(slot.ended)}}, true);
                    slot = {};
                    continue;
                }
                struct Result { std::uint64_t ticks, available; } results[2]{};
                const auto status = vkGetQueryPoolResults(device->vk(), pool, i * 2, 2,
                    sizeof(results), results, sizeof(Result),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
                if (status == VK_NOT_READY) continue;
                if (status == VK_SUCCESS && results[0].available && results[1].available)
                {
                    const auto ticks = (results[1].ticks - results[0].ticks) & mask;
                    Debug::GameplayDiagnostics::recordEvent("profile_gpu", {
                        {"name", name}, {"source_frame", std::to_string(slot.diagnosticFrame)},
                        {"semantic_frame", std::to_string(slot.frame)},
                        {"ms", std::to_string(double(ticks) * period / 1e6)},
                        {"begin_ticks", std::to_string(results[0].ticks & mask)},
                        {"end_ticks", std::to_string(results[1].ticks & mask)},
                        {"period_ns", std::to_string(period)}}, true);
                }
                else Debug::GameplayDiagnostics::recordEvent("profile_gpu_error", {
                    {"name", name}, {"status", std::to_string(status)}}, true);
                slot = {};
            }
            if (!Debug::FrameProfile::accumulator.active) return;
            for (unsigned i = 0; i < slots.size(); ++i)
                if (!slots[i].busy)
                {
                    slots[i] = {frame, Debug::GameplayDiagnostics::context.frame, true, false, false};
                    recording = i;
                    return;
                }
            Debug::GameplayDiagnostics::recordEvent("profile_gpu_dropped", {{"name", name}});
        }
    };

    class FrameTimestampCommand : public vsg::Inherit<vsg::Command, FrameTimestampCommand>
    {
    public:
        FrameTimestampCommand(std::shared_ptr<FrameGpuProfile> profile, bool start)
            : mProfile(std::move(profile)), mStart(start) {}
        void record(vsg::CommandBuffer& buffer) const override
        {
            if (!mProfile->recording) return;
            const auto index = *mProfile->recording;
            auto& slot = mProfile->slots[index];
            if (mStart)
            {
                vkCmdResetQueryPool(buffer.vk(), mProfile->pool, index * 2, 2);
                vkCmdWriteTimestamp(buffer.vk(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mProfile->pool, index * 2);
                slot.began = true;
            }
            else
            {
                vkCmdWriteTimestamp(buffer.vk(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mProfile->pool, index * 2 + 1);
                slot.ended = true;
            }
        }
    private:
        std::shared_ptr<FrameGpuProfile> mProfile;
        bool mStart;
    };

    inline std::shared_ptr<FrameGpuProfile> installFrameGpuProfile(vsg::CommandGraph& graph, std::string name)
    {
        auto profile = std::make_shared<FrameGpuProfile>(graph.device.get(), graph.queueFamily, std::move(name));
        if (!profile->pool)
        {
            Debug::GameplayDiagnostics::recordEvent("profile_gpu_unavailable", {{"name", profile->name}}, true);
            return {};
        }
        graph.children.insert(graph.children.begin(), FrameTimestampCommand::create(profile, true));
        graph.addChild(FrameTimestampCommand::create(profile, false));
        return profile;
    }
}
#endif
