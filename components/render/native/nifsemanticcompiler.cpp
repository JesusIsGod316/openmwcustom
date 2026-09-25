#include "nifsemanticcompiler.hpp"

#include <components/nif/niffile.hpp>
#include <components/vfs/manager.hpp>

#include <exception>
#include <string>

namespace RenderNative
{
    NifSemanticCompileResult NifSemanticCompiler::compile(
        Nif::FileView file, NifRender::TranslatorOptions options) const
    {
        NifSemanticCompileResult result;
        try
        {
            result.bundle = NifRender::translateStaticNif(file, mVfs, options, mTextureIdentities);
            result.status = NifSemanticCompileStatus::Compiled;
            if (!result.bundle.valid())
                result.diagnostic = "native NIF semantic compiler produced an invalid neutral bundle";
            else if (result.bundle.hasErrors())
                result.diagnostic = "native NIF semantic compiler produced fail-closed translation diagnostics";
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
