#include "nifsemanticcompiler.hpp"

#include <components/misc/constants.hpp>
#include <components/nif/extra.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/rendercore/namedvisualsemantics.hpp>
#include <components/vfs/manager.hpp>

#include <exception>
#include <string>

namespace RenderNative
{
    namespace
    {
        [[nodiscard]] std::uint64_t inspectNamedVisualCapabilities(Nif::FileView file) noexcept
        {
            std::uint64_t result = 0;
            for (std::size_t rootIndex = 0; rootIndex < file.numRoots(); ++rootIndex)
            {
                const Nif::Record* record = file.getRoot(rootIndex);
                const auto* root = dynamic_cast<const Nif::NiAVObject*>(record);
                if (!root)
                    continue;
                for (const Nif::ExtraPtr& extra : root->getExtraList())
                {
                    if (extra.empty() || extra->mRecordType != Nif::RC_NiStringExtraData)
                        continue;
                    const auto* value = static_cast<const Nif::NiStringExtraData*>(extra.getPtr());
                    if (value->mData == Constants::NightDayLabel)
                        result |= RenderCore::NightDaySwitchCapabilitySemanticFlag;
                    else if (value->mData == Constants::HerbalismLabel)
                        result |= RenderCore::HerbalismSwitchCapabilitySemanticFlag;
                }
            }
            return result;
        }
    }

    NifSemanticCompileResult NifSemanticCompiler::compile(
        Nif::FileView file, NifRender::TranslatorOptions options) const
    {
        NifSemanticCompileResult result;
        try
        {
            result.namedVisualCapabilities = inspectNamedVisualCapabilities(file);
            result.bundle = NifRender::translateStaticNif(file, mVfs, options, mTextureIdentities);
            result.controllers = NifControllerCompiler::compile(file, result.bundle);
            result.status = NifSemanticCompileStatus::Compiled;
            if (!result.bundle.valid())
                result.diagnostic = "native NIF semantic compiler produced an invalid neutral bundle";
            else if (result.bundle.hasErrors())
                result.diagnostic = "native NIF semantic compiler produced fail-closed translation diagnostics";
            else if (!result.controllers.valid())
                result.diagnostic = "native NIF semantic compiler produced an invalid controller program";
            return result;
        }
        catch (const std::exception& error)
        {
            result.status = NifSemanticCompileStatus::TranslationFailed;
            result.diagnostic = error.what();
            return result;
        }
        catch (...)
        {
            result.status = NifSemanticCompileStatus::TranslationFailed;
            result.diagnostic = "native NIF semantic translation threw an unknown exception";
            return result;
        }
    }

    NifSemanticCompileResult NifSemanticCompiler::compile(
        VFS::Path::NormalizedView path, NifRender::TranslatorOptions options) const
    {
        NifSemanticCompileResult result;
        if (path.empty())
        {
            result.status = NifSemanticCompileStatus::InvalidSource;
            result.diagnostic = "native NIF semantic compiler received an empty source path";
            return result;
        }
        if (!mVfs.exists(path))
        {
            result.status = NifSemanticCompileStatus::MissingSource;
            result.diagnostic = "native NIF semantic compiler source is missing from the winning VFS: "
                + std::string(path.value());
            return result;
        }

        try
        {
            Nif::NIFFile nifFile(path);
            Nif::Reader reader(nifFile, nullptr);
            reader.parse(mVfs.get(path));
            return compile(Nif::FileView(nifFile), options);
        }
        catch (const std::exception& error)
        {
            result.status = NifSemanticCompileStatus::ParseFailed;
            result.diagnostic = "native NIF semantic compiler could not parse "
                + std::string(path.value()) + ": " + error.what();
            return result;
        }
        catch (...)
        {
            result.status = NifSemanticCompileStatus::ParseFailed;
            result.diagnostic = "native NIF semantic compiler could not parse "
                + std::string(path.value()) + ": unknown exception";
            return result;
        }
    }
}
