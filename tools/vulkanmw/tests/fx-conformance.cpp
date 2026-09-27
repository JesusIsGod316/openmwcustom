#include <components/fx/technique.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/settings/parser.hpp>
#include <components/settings/settings.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/filesystemarchive.hpp>
#include <components/vfs/manager.hpp>
#include <vsg/utils/ShaderCompiler.h>
#include <vsg/state/ShaderStage.h>

#include <filesystem>
#include <iostream>
#include <set>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: openmw-vulkan-fx-conformance defaults.bin data-directory [data-directory ...]\n";
        return 2;
    }
    try
    {
        Settings::SettingsFileParser parser;
        parser.loadSettingsFile(argv[1], Settings::Manager::mDefaultSettings, true, false);
        Settings::StaticValues::initDefaults();
        Settings::StaticValues::init();
        VFS::Manager vfs;
        std::set<std::string> names;
        for (int i = 2; i < argc; ++i)
        {
            const std::filesystem::path root(argv[i]);
            vfs.addArchive(std::make_unique<VFS::FileSystemArchive>(root));
            if (!std::filesystem::is_directory(root / "shaders")) continue;
            for (const auto& entry : std::filesystem::directory_iterator(root / "shaders"))
                if (entry.path().extension() == ".omwfx") names.insert(entry.path().stem().string());
        }
        vfs.buildIndex();
        Resource::ImageManager images(&vfs, 0.0);
        auto compiler = vsg::ShaderCompiler::create();
        unsigned passed = 0, failed = 0;
        // Debug mode makes scalar/vector options dynamic, exercising both
        // native descriptor-backed parameters and normal constant folding.
        for (const auto mode : {Settings::ShaderManager::Mode::Normal, Settings::ShaderManager::Mode::Debug})
        {
            Settings::ShaderManager::get().setMode(mode);
            for (const auto& name : names)
            {
                Fx::Technique technique(vfs, images, Fx::Technique::makeFileName(name), name, 1920, 1080, true, false);
                if (!technique.compile())
                {
                    std::cerr << "FAIL parse " << name << ": " << technique.getLastError() << '\n';
                    ++failed;
                    continue;
                }
                if (mode == Settings::ShaderManager::Mode::Normal)
                    for (const auto& texture : technique.getTextures())
                        if (const auto* image = texture->getImage(0))
                            std::cout << "IMAGE " << name << '/' << texture->getName() << " size=" << image->s()
                                << ',' << image->t() << ',' << image->r() << " pixel=" << image->getPixelFormat()
                                << " type=" << image->getDataType() << " internal=" << texture->getInternalFormat()
                                << " min=" << texture->getFilter(osg::Texture::MIN_FILTER)
                                << " mip=" << image->getNumMipmapLevels() << '\n';
                for (const auto& pass : technique.getPasses())
                {
                    const std::string label = name + "/" + pass->getName()
                        + (mode == Settings::ShaderManager::Mode::Debug ? "/dynamic" : "/static");
                    try
                    {
                        auto source = pass->getVulkanSources(technique);
                        auto stages = vsg::ShaderStages{
                            vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", source.vertex),
                            vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main", source.fragment)};
                        if (!compiler->compile(stages) || stages[0]->module->code.empty() || stages[1]->module->code.empty())
                            throw std::runtime_error("Vulkan/SPIR-V compilation failed");
                        ++passed;
                        std::cout << "PASS " << label << " samplers=" << source.samplers.size() << '\n';
                    }
                    catch (const std::exception& error)
                    {
                        ++failed;
                        std::cerr << "FAIL " << label << ": " << error.what() << '\n';
                    }
                }
            }
        }
        std::cout << "Vulkan real-effect shader checks: passed=" << passed << " failed=" << failed << '\n';
        return failed || !passed ? 1 : 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
