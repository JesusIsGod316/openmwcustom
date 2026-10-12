#ifndef OPENMW_COMPONENTS_SETTINGS_OPTIMIZATIONALIASES_H
#define OPENMW_COMPONENTS_SETTINGS_OPTIMIZATIONALIASES_H

#include "categories.hpp"

#include <string_view>
#include <utility>

namespace Settings::OptimizationAliases
{
    struct Alias
    {
        std::string_view oldCategory;
        std::string_view oldSetting;
        std::string_view category;
        std::string_view setting;
    };

    // Generated from files/optimization-settings-aliases.json. Both the launcher
    // and engine use that curated map; the generator's --check detects drift.
    inline constexpr Alias aliases[] = {
#include "optimizationaliases.inc"
    };
    using KeyView = std::pair<std::string_view, std::string_view>;

    inline KeyView canonical(std::string_view category, std::string_view setting)
    {
        for (const auto& alias : aliases)
            if (category == alias.oldCategory && setting == alias.oldSetting)
                return { alias.category, alias.setting };
        return { category, setting };
    }

    inline CategorySettingValueMap::const_iterator find(
        const CategorySettingValueMap& settings, std::string_view category, std::string_view setting)
    {
        const auto key = canonical(category, setting);
        if (const auto it = settings.find(key); it != settings.end())
            return it;
        // Supports callers that populate Manager's public maps directly.
        for (const auto& alias : aliases)
            if (key.first == alias.category && key.second == alias.setting)
                if (const auto it = settings.find(KeyView{ alias.oldCategory, alias.oldSetting }); it != settings.end())
                    return it;
        return settings.end();
    }

    inline CategorySettingValueMap normalize(const CategorySettingValueMap& settings)
    {
        CategorySettingValueMap result;
        for (const auto& [key, value] : settings)
        {
            const auto normalized = canonical(key.first, key.second);
            if (normalized == KeyView{ key.first, key.second })
                result.insert_or_assign(key, value);
            else if (!settings.contains(normalized))
                result.try_emplace(CategorySetting{ normalized.first, normalized.second }, value);
        }
        return result;
    }
}

#endif

