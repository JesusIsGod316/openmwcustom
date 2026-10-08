#include <components/settings/optimizationaliases.hpp>
#include <components/settings/parser.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
    std::string read(const std::filesystem::path& path)
    {
        std::ifstream stream(path);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    }
    void write(const std::filesystem::path& path, const std::string& content)
    {
        std::ofstream stream(path);
        stream << content;
        require(bool(stream), "fixture file write failed");
    }
    std::string setting(std::string_view category, std::string_view key, std::string_view value)
    {
        return "[" + std::string(category) + "]\n" + std::string(key) + " = " + std::string(value) + "\n";
    }
}
int main(int argc, char** argv)
{
    try
    {
        using namespace Settings;
        const auto directory = std::filesystem::path(argc > 1 ? argv[1] : "settings-alias-fixture");
        std::filesystem::create_directories(directory);
        const auto path = directory / "settings.cfg";
        SettingsFileParser parser;
        std::size_t count = 0;
        for (const auto& alias : OptimizationAliases::aliases)
        {
            const CategorySetting key{ alias.category, alias.setting };
            const auto legacy = setting(alias.oldCategory, alias.oldSetting, "old");
            const auto canonical = setting(alias.category, alias.setting, "canonical");
            for (const auto& contents : { legacy, canonical, legacy + canonical, canonical + legacy })
            {
                write(path, contents);
                CategorySettingValueMap values;
                parser.loadSettingsFile(path, values);
                require(values.size() == 1 && values.contains(key), "alias did not converge on one canonical key");
                require(values.at(key) == (contents == legacy ? "old" : "canonical"),
                    "canonical value did not win independently of line order");
                parser.saveSettingsFile(path, values);
                CategorySettingValueMap restored;
                parser.loadSettingsFile(path, restored);
                require(restored == values, "canonical save/load lost a value");
                const auto saved = read(path);
                parser.saveSettingsFile(path, restored);
                require(read(path) == saved, "second canonical save changed bytes");
            }
            // A later configuration layer must still override shipped canonical
            // defaults even when an existing user file uses its old spelling.
            CategorySettingValueMap layered{ { key, "default" } };
            write(path, legacy);
            parser.loadSettingsFile(path, layered, false, true);
            require(layered.at(key) == "old", "legacy user layer did not override canonical default");
            // Public Manager maps may also be populated directly by clients.
            layered.emplace(CategorySetting{ alias.oldCategory, alias.oldSetting }, "ignored");
            require(OptimizationAliases::find(layered, alias.oldCategory, alias.oldSetting)->second == "old",
                "canonical direct-map lookup did not win");
            ++count;
        }
        write(path, "# preserve me\n[Cells]\nv3 prepared instance cache = true\n"
            "[ModSpecific]\nunknown = literal value\n");
        CategorySettingValueMap mixed;
        parser.loadSettingsFile(path, mixed);
        parser.saveSettingsFile(path, mixed);
        CategorySettingValueMap roundtrip;
        parser.loadSettingsFile(path, roundtrip);
        require(roundtrip == mixed && roundtrip.at({ "ModSpecific", "unknown" }) == "literal value",
            "unknown setting was modified");
        require(read(path).find("# preserve me") != std::string::npos, "save discarded comments");
        require(read(path).find("v3 prepared instance cache =") == std::string::npos,
            "save emitted an active legacy key");

        for (const auto& duplicate : {
            std::string("[Cells]\nstreaming scheduler = off\nstreaming scheduler = adaptive\n"),
            std::string("[Cells]\nv3 streaming scheduler = off\nv3 streaming scheduler = adaptive\n"),
            std::string("[ModSpecific]\nunknown = one\nunknown = two\n") })
        {
            write(path, duplicate);
            CategorySettingValueMap values;
            bool rejected = false;
            try { parser.loadSettingsFile(path, values); }
            catch (const std::runtime_error&) { rejected = true; }
            require(rejected, "physical duplicate key was silently accepted");
            values.clear();
            parser.loadSettingsFile(path, values, false, true);
            require(values.size() == 1, "override-existing duplicate semantics changed");
        }
        if (argc > 2)
        {
            CategorySettingValueMap defaults;
            parser.loadSettingsFile(argv[2], defaults);
            for (const auto& alias : OptimizationAliases::aliases)
                require(defaults.contains(CategorySetting{ alias.category, alias.setting }),
                    "canonical default is missing");
            require(defaults.at({ "Optimizations", "experimental cache budget mb" }) == "256",
                "experimental default is missing");
        }
        std::cout << "Settings aliases: " << count
            << " complete legacy/canonical order and roundtrip cases; layered precedence, duplicate and unknown checks pass\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

