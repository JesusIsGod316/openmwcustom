#ifndef OPENMW_COMPONENTS_RENDER_NATIVE_NIFKEYFRAMECLIP_H
#define OPENMW_COMPONENTS_RENDER_NATIVE_NIFKEYFRAMECLIP_H

#include "nifcontrollerprogram.hpp"

#include <components/vfs/pathutil.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace Nif
{
    class FileView;
}

namespace VFS
{
    class Manager;
}

namespace RenderNative
{
    // OSG-free equivalent of the subset of SceneUtil::TextKeyMap consumed by
    // OpenMW's animation state machine. Text normalization is performed by the
    // compiler so downstream playback sees the same keys as the compatibility path.
    class NativeTextKeyMap
    {
    public:
        using Storage = std::multimap<float, std::string>;
        using ConstIterator = Storage::const_iterator;

        [[nodiscard]] bool empty() const noexcept { return mTextKeys.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return mTextKeys.size(); }

        [[nodiscard]] auto begin() const noexcept { return mTextKeys.begin(); }
        [[nodiscard]] auto end() const noexcept { return mTextKeys.end(); }
        [[nodiscard]] auto rbegin() const noexcept { return mTextKeys.rbegin(); }
        [[nodiscard]] auto rend() const noexcept { return mTextKeys.rend(); }

        [[nodiscard]] auto lowerBound(float time) const { return mTextKeys.lower_bound(time); }
        [[nodiscard]] auto upperBound(float time) const { return mTextKeys.upper_bound(time); }

        void emplace(float time, std::string text);

        [[nodiscard]] bool hasGroupStart(std::string_view groupName) const
        {
            return mGroups.count(groupName) != 0;
        }

        [[nodiscard]] auto findGroupStart(std::string_view groupName) const
        {
            return std::find_if(mTextKeys.begin(), mTextKeys.end(),
                [groupName](const Storage::value_type& value) {
                    return value.second.starts_with(groupName)
                        && value.second.compare(groupName.size(), 2, ": ") == 0;
                });
        }

        [[nodiscard]] const std::set<std::string, std::less<>>& groups() const noexcept { return mGroups; }

    private:
        std::set<std::string, std::less<>> mGroups;
        Storage mTextKeys;
    };

    struct NamedTransformTrack
    {
        std::string name;
        ControllerTiming timing;
        TransformControllerTrack track;
    };

    struct NifKeyframeClip
    {
        NativeTextKeyMap textKeys;
        // Matches SceneUtil::KeyframeHolder semantics: keyframe controller names
        // are unique and the first authored duplicate wins.
        std::map<std::string, NamedTransformTrack, std::less<>> controllers;
        std::uint32_t ignoredPairs = 0;
        std::uint32_t unsupportedControllers = 0;

        [[nodiscard]] bool usable() const noexcept
        {
            return !textKeys.empty() && !controllers.empty();
        }
    };

    enum class NifKeyframeCompileStatus : std::uint8_t
    {
        Compiled,
        InvalidSource,
        MissingSource,
        ParseFailed,
    };

    struct NifKeyframeCompileResult
    {
        NifKeyframeCompileStatus status = NifKeyframeCompileStatus::InvalidSource;
        NifKeyframeClip clip;
        std::string diagnostic;

        [[nodiscard]] bool compiled() const noexcept
        {
            return status == NifKeyframeCompileStatus::Compiled;
        }

        [[nodiscard]] bool usable() const noexcept
        {
            return compiled() && clip.usable();
        }
    };

    class NifKeyframeClipCompiler final
    {
    public:
        explicit NifKeyframeClipCompiler(const VFS::Manager& vfs) noexcept
            : mVfs(vfs)
        {
        }

        [[nodiscard]] NifKeyframeCompileResult compile(VFS::Path::NormalizedView path) const;
        [[nodiscard]] NifKeyframeCompileResult compile(Nif::FileView file) const;

    private:
        const VFS::Manager& mVfs;
    };
}

#endif
