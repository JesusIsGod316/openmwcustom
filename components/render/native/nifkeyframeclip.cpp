#include "nifkeyframeclip.hpp"

#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/nif/controller.hpp>
#include <components/nif/extra.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/vfs/manager.hpp>

#include <exception>
#include <sstream>
#include <utility>
#include <vector>

namespace RenderNative
{
    void NativeTextKeyMap::emplace(float time, std::string text)
    {
        const auto separator = text.find(": ");
        if (separator != std::string::npos)
            mGroups.emplace(text.substr(0, separator));
        mTextKeys.emplace(time, std::move(text));
    }

    namespace
    {
        void appendTextKeys(const Nif::NiTextKeyExtraData& source, NativeTextKeyMap& target)
        {
            for (const Nif::NiTextKeyExtraData::TextKey& key : source.mList)
            {
                std::vector<std::string> split;
                Misc::StringUtils::split(key.mText, split, "\r\n");
                for (std::string& text : split)
                {
                    Misc::StringUtils::trim(text);
                    Misc::StringUtils::lowerCaseInPlace(text);
                    if (!text.empty())
                        target.emplace(key.mTime, std::move(text));
                }
            }
        }

        [[nodiscard]] const Nif::NiSequenceStreamHelper* findSequenceRoot(Nif::FileView file) noexcept
        {
            for (std::size_t i = 0; i < file.numRoots(); ++i)
            {
                const Nif::Record* record = file.getRoot(i);
                if (record && record->mRecordType == Nif::RC_NiSequenceStreamHelper)
                    return static_cast<const Nif::NiSequenceStreamHelper*>(record);
            }
            return nullptr;
        }

        [[nodiscard]] std::string pairDiagnostic(const Nif::Record* extra, const Nif::NiTimeController* controller)
        {
            std::ostringstream stream;
            stream << "unexpected KF extra/controller pair";
            if (extra)
                stream << " extra=" << extra->mRecordName << "#" << extra->mRecordIndex;
            else
                stream << " extra=<null>";
            if (controller)
                stream << " controller=" << controller->mRecordName << "#" << controller->mRecordIndex;
            else
                stream << " controller=<null>";
            return stream.str();
        }
    }

    NifKeyframeCompileResult NifKeyframeClipCompiler::compile(Nif::FileView file) const
    {
        NifKeyframeCompileResult result;
        result.status = NifKeyframeCompileStatus::Compiled;

        const Nif::NiSequenceStreamHelper* sequence = findSequenceRoot(file);
        if (!sequence)
        {
            result.diagnostic = "KF source contains no NiSequenceStreamHelper root";
            return result;
        }

        const Nif::ExtraList extraList = sequence->getExtraList();
        if (extraList.empty() || extraList.front().empty())
        {
            result.diagnostic = "NiSequenceStreamHelper contains no text-key extra data";
            return result;
        }
        if (extraList.front()->mRecordType != Nif::RC_NiTextKeyExtraData)
        {
            result.diagnostic = "NiSequenceStreamHelper first extra is not NiTextKeyExtraData";
            return result;
        }

        appendTextKeys(*static_cast<const Nif::NiTextKeyExtraData*>(extraList.front().getPtr()), result.clip.textKeys);

        Nif::NiTimeControllerPtr controller = sequence->mController;
        for (std::size_t i = 1; i < extraList.size() && !controller.empty(); ++i, controller = controller->mNext)
        {
            const Nif::ExtraPtr& extra = extraList[i];
            if (extra.empty() || extra->mRecordType != Nif::RC_NiStringExtraData
                || controller->mRecordType != Nif::RC_NiKeyframeController)
            {
                ++result.clip.ignoredPairs;
                if (result.diagnostic.empty())
                    result.diagnostic = pairDiagnostic(extra.getPtr(), controller.getPtr());
                continue;
            }

            // Deliberately ignore controller active state for external KF files:
            // vanilla and OpenMW's compatibility loader both bind these tracks.
            const auto* name = static_cast<const Nif::NiStringExtraData*>(extra.getPtr());
            const auto* keyframe = static_cast<const Nif::NiKeyframeController*>(controller.getPtr());
            std::optional<TransformControllerTrack> track = NifControllerCompiler::compileTransformTrack(*keyframe);
            if (!track)
            {
                ++result.clip.unsupportedControllers;
                if (result.diagnostic.empty())
                {
                    result.diagnostic = "KF transform controller has empty data or unsupported interpolator: "
                        + std::to_string(keyframe->mRecordIndex);
                }
                continue;
            }

            NamedTransformTrack compiled;
            compiled.name = name->mData;
            compiled.timing = NifControllerCompiler::compileTiming(*keyframe);
            compiled.track = std::move(*track);

            // std::map::emplace intentionally preserves the first duplicate,
            // matching SceneUtil::KeyframeHolder.
            result.clip.controllers.emplace(compiled.name, std::move(compiled));
        }

        return result;
    }

    NifKeyframeCompileResult NifKeyframeClipCompiler::compile(VFS::Path::NormalizedView path) const
    {
        NifKeyframeCompileResult result;
        if (path.empty())
        {
            result.status = NifKeyframeCompileStatus::InvalidSource;
            result.diagnostic = "native KF compiler received an empty source path";
            return result;
        }
        if (!mVfs.exists(path))
        {
            result.status = NifKeyframeCompileStatus::MissingSource;
            result.diagnostic = "native KF compiler source is missing from the winning VFS: "
                + std::string(path.value());
            return result;
        }

        try
        {
            Nif::NIFFile file(path);
            Nif::Reader reader(file, nullptr);
            reader.parse(mVfs.get(path));
            return compile(Nif::FileView(file));
        }
        catch (const std::exception& error)
        {
            result.status = NifKeyframeCompileStatus::ParseFailed;
            result.diagnostic = "native KF compiler could not parse " + std::string(path.value()) + ": " + error.what();
            return result;
        }
        catch (...)
        {
            result.status = NifKeyframeCompileStatus::ParseFailed;
            result.diagnostic = "native KF compiler could not parse " + std::string(path.value())
                + ": unknown exception";
            return result;
        }
    }
}
