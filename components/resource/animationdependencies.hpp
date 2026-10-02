#ifndef OPENMW_COMPONENTS_RESOURCE_ANIMATIONDEPENDENCIES_H
#define OPENMW_COMPONENTS_RESOURCE_ANIMATIONDEPENDENCIES_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include <components/vfs/manager.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

namespace Resource
{
    inline constexpr std::size_t MaximumAnimationPreloadSources = 256;
    inline constexpr std::size_t MaximumAnimationPreloadDependencies = 1024;
    inline constexpr std::size_t MaximumAnimationPreloadScannedPaths = 8192;
    inline constexpr std::uint64_t MaximumAnimationPreloadEstimate = 256ull * 1024 * 1024;

    inline bool animationDependencyPreparationAllowed(bool cancelled, bool pressureAllows,
        std::uint64_t snapshotGeneration, std::uint64_t currentGeneration, std::size_t prepared,
        std::uint64_t retainedEstimate) noexcept
    {
        return !cancelled && pressureAllows && snapshotGeneration == currentGeneration
            && prepared < MaximumAnimationPreloadDependencies && retainedEstimate < MaximumAnimationPreloadEstimate;
    }

    // Frozen before main, like the UI policy. No speculative discovery or
    // allocation is added to the historical control path.
    inline const bool AnimationDependencyPreloadEnabled = [] {
        const char* value = std::getenv("OPENMW_HITCH_ANIMATION_PRELOAD");
        return value && value[0] == '1' && value[1] == '\0';
    }();

    inline bool animationDependencyPreloadEnabled() noexcept
    {
        return AnimationDependencyPreloadEnabled;
    }

    inline const bool AnimationDependencyLifetimeEnabled = [] {
        const char* value = std::getenv("OPENMW_HITCH_ANIMATION_LIFETIME");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    inline const bool AnimationSourcePlanEnabled = [] {
        const char* value = std::getenv("OPENMW_HITCH_ANIMATION_PLAN");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    inline bool animationDependencyLifetimeEnabled() noexcept
    {
        return AnimationDependencyPreloadEnabled && AnimationDependencyLifetimeEnabled;
    }
    inline bool animationSourcePlanEnabled() noexcept { return AnimationSourcePlanEnabled; }

    inline std::string additionalAnimationDirectory(VFS::Path::NormalizedView model)
    {
        constexpr VFS::Path::NormalizedView meshes("meshes/");
        if (!model.value().starts_with(meshes.value()))
            return {};
        std::string path(model.value());
        path.replace(0, meshes.value().size(), "animations/");
        const auto extension = path.find_last_of(VFS::Path::extensionSeparator);
        if (extension == std::string::npos)
            return {};
        path.replace(extension, path.size() - extension, "/");
        return path;
    }

    inline bool npcAnimationUsesCustomModel(bool firstPerson, bool werewolf, bool hasModel) noexcept
    {
        return !firstPerson && !werewolf && hasModel;
    }

    inline bool npcAnimationUsesArgonianSwim(bool firstPerson, bool werewolf, bool beast, bool argonian) noexcept
    {
        return !firstPerson && !werewolf && beast && argonian;
    }

    // Preserve the runtime's exact source order, including intentional repeated
    // source paths. Only optional preparation deduplicates the resulting list.
    template <class Visitor>
    bool visitNpcAnimationSources(std::string_view base, std::string_view defaultSkeleton, std::string_view model,
        bool customModel, bool argonianSwim, std::string_view swim, Visitor&& visit)
    {
        if (!base.empty() && !visit(base))
            return false;
        if (defaultSkeleton != base && !visit(defaultSkeleton))
            return false;
        if (customModel && !visit(model))
            return false;
        if (argonianSwim && !visit(swim))
            return false;
        return true;
    }

    inline VFS::Path::Normalized animationKeyframePath(VFS::Path::NormalizedView model)
    {
        VFS::Path::Normalized result(model);
        if (result.extension() == VFS::Path::ExtensionView("nif"))
            result.changeExtension(VFS::Path::ExtensionView("kf"));
        return result;
    }

    // This is the historical Animation::loadAdditionalAnimations transformation
    // and VFS iteration order, shared by demand binding and optional discovery.
    // A visitor may stop optional work without changing demand source semantics.
    template <class Visitor, class Continue>
    bool visitAdditionalAnimationSources(
        const VFS::Manager& vfs, VFS::Path::NormalizedView model, Visitor&& visit, Continue&& keepGoing)
    {
        const auto path = additionalAnimationDirectory(model);
        if (path.empty())
            return true;
        for (const VFS::Path::Normalized& name : vfs.getRecursiveDirectoryIterator(path))
        {
            if (!keepGoing())
                return false;
            if (name.extension() == VFS::Path::ExtensionView("kf") && !visit(name))
                return false;
        }
        return true;
    }

    template <class Visitor>
    bool visitAdditionalAnimationSources(const VFS::Manager& vfs, VFS::Path::NormalizedView model, Visitor&& visit)
    {
        return visitAdditionalAnimationSources(vfs, model, visit, [] { return true; });
    }

    template <class Visitor, class Continue>
    bool visitAnimationDependencies(const VFS::Manager& vfs, VFS::Path::NormalizedView model, bool additionalSources,
        Visitor&& visit, Continue&& keepGoing)
    {
        if (!keepGoing())
            return false;
        const VFS::Path::Normalized primary = animationKeyframePath(model);
        if (!visit(primary))
            return false;
        return !additionalSources || visitAdditionalAnimationSources(vfs, primary, visit, keepGoing);
    }

    template <class Visitor>
    bool visitAnimationDependencies(
        const VFS::Manager& vfs, VFS::Path::NormalizedView model, bool additionalSources, Visitor&& visit)
    {
        return visitAnimationDependencies(vfs, model, additionalSources, visit, [] { return true; });
    }
}

#endif
