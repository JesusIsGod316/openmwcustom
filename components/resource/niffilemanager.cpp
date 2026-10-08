#include <components/debug/hitchtrace.hpp>
#include "niffilemanager.hpp"

#include <iostream>
#include <algorithm>
#include <cmath>

#include <osg/Object>

#include <components/vfs/manager.hpp>
#include <components/debug/debuglog.hpp>

#include "objectcache.hpp"
#include "preloadestimate.hpp"
#include <components/nif/data.hpp>

namespace Resource
{

    class NifFileHolder : public osg::Object, public Debug::RuntimeDiagnostics::PayloadSource
    {
    public:
        NifFileHolder(const Nif::NIFFilePtr& file)
            : mNifFile(file)
        {
            if (Debug::RuntimeDiagnostics::enabled())
            {
                using Debug::RuntimeDiagnostics::capacityBytes;
                mDiagnosticBytes = sizeof(Nif::NIFFile) + capacityBytes(file->mRecords) + capacityBytes(file->mRoots);
                for (const auto& record : file->mRecords)
                {
                    if (const auto* data = dynamic_cast<const Nif::NiGeometryData*>(record.get()))
                    {
                        mDiagnosticBytes += capacityBytes(data->mVertices) + capacityBytes(data->mNormals)
                            + capacityBytes(data->mTangents) + capacityBytes(data->mBitangents)
                            + capacityBytes(data->mColors) + capacityBytes(data->mUVList);
                        for (const auto& uv : data->mUVList) mDiagnosticBytes += capacityBytes(uv);
                    }
                    if (const auto* triangles = dynamic_cast<const Nif::NiTriShapeData*>(record.get()))
                    {
                        mDiagnosticBytes += capacityBytes(triangles->mTriangles) + capacityBytes(triangles->mMatchGroups);
                        for (const auto& group : triangles->mMatchGroups) mDiagnosticBytes += capacityBytes(group);
                    }
                    if (const auto* strips = dynamic_cast<const Nif::NiTriStripsData*>(record.get()))
                    {
                        mDiagnosticBytes += capacityBytes(strips->mStrips);
                        for (const auto& strip : strips->mStrips) mDiagnosticBytes += capacityBytes(strip);
                    }
                }
            }
        }
        NifFileHolder(const NifFileHolder& copy, const osg::CopyOp& copyop)
            : mNifFile(copy.mNifFile)
            , mDiagnosticBytes(copy.mDiagnosticBytes)
        {
        }

        NifFileHolder() = default;

        META_Object(Resource, NifFileHolder)

        Debug::RuntimeDiagnostics::PayloadInfo diagnosticPayload() const noexcept override
        {
            return { mNifFile.get(), mDiagnosticBytes, mNifFile ? mNifFile->mRecords.size() : 0, mNifFile.use_count() > 1 };
        }
        Nif::NIFFilePtr mNifFile;
        std::uint64_t mDiagnosticBytes = 0;
    };

    NifFileManager::NifFileManager(const VFS::Manager* vfs, const ToUTF8::StatelessUtf8Encoder* encoder)
        // NIF files aren't needed any more once the converted objects are cached in SceneManager / BulletShapeManager,
        // so no point in using an expiry delay.
        : ResourceManager(vfs, 0)
        , mEncoder(encoder)
    {
    }

    NifFileManager::~NifFileManager()
    {
        if (mColdRetentionSeconds > 0)
        {
            const auto stats = mCache->getStats();
            Log(Debug::Info) << "RAM C2 parsed NIF retention: configured_ttl_s=" << mColdRetentionSeconds
                << " effective_ttl_s=" << effectiveExpiryDelay()
                << " independent=" << mIndependentColdRetention
                << " get=" << stats.mGet << " hit=" << stats.mHit
                << " expired_all_policies=" << stats.mExpired << " size=" << stats.mSize;
        }
    }

    void NifFileManager::setColdRetentionSeconds(double seconds,bool independent)
    {
        mColdRetentionSeconds = std::isfinite(seconds) && seconds > 0 ? seconds : 0;
        mIndependentColdRetention=independent && mColdRetentionSeconds>0;
        mCache->setExternalOwnerTest(mColdRetentionSeconds > 0
            ? +[](const osg::Object* object) noexcept {
                const auto* holder = dynamic_cast<const NifFileHolder*>(object);
                return holder && holder->mNifFile.use_count() > 1;
            } : nullptr);
    }

    void NifFileManager::updateCache(double referenceTime)
    {
        mCache->update(referenceTime,effectiveExpiryDelay());
    }

    Nif::NIFFilePtr NifFileManager::get(VFS::Path::NormalizedView name)
    {
        Debug::HitchTrace::Scope hitchNif("resource.nif_lookup", {}, name.value());
        Debug::ResourceLifecycle::RequestScope lifecycle(Debug::ResourceLifecycle::Kind::Nif,
            name.value(), mVFS, SpeculativeScope::active(), mCache.get());
        osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(name);
        const auto generation = mVFS->getIndexGeneration();
        if (mColdRetentionSeconds > 0 && obj)
        {
            const auto lease = mCache->getLeaseFromObjectCache(name, obj.get());
            if (!lease || lease.mSourceRevision != generation)
            {
                if (lease) mCache->removeIfSame(name.value(), lease);
                obj = nullptr;
            }
        }
        if (obj != nullptr)
            return static_cast<NifFileHolder*>(obj.get())->mNifFile;

        Debug::RuntimeDiagnostics::Operation diagnostic("nif_cache_miss", name.value());
        auto claim = SpeculativeScope::claim(SpeculativeScope::active()
            ? std::to_string(mVFS->getIndexGeneration()) + "|nif|" + std::string(name.value()) : std::string{});
        auto stream = mVFS->get(name);
        const auto estimate = SpeculativeScope::active() ? estimateStream(*stream, false) : PreloadEstimate{};
        SpeculativeScope::Stage memoryStage(estimate.peak, estimate.retained);
        auto file = std::make_shared<Nif::NIFFile>(name);
        Nif::Reader reader(*file, mEncoder);
        { Debug::HitchTrace::Scope hitchParse("resource.nif_parse", {}, name.value());
          reader.parse(std::move(stream)); }
        obj = new NifFileHolder(file);
        Nif::NIFFilePtr result = file;
        if (mColdRetentionSeconds > 0)
        {
            if (generation == mVFS->getIndexGeneration())
            {
                auto [winner, inserted] = mCache->replaceIfSameOrInsert(name.value(), nullptr, obj, generation);
                if (const auto* canonical = dynamic_cast<const NifFileHolder*>(winner.get())) result = canonical->mNifFile;
            }
        }
        else mCache->addEntryToObjectCache(name.value(), obj);
        return result;
    }

    void NifFileManager::reportRuntimeDiagnostics(double referenceTime) const noexcept
    {
        if (mDiagnosticSampler.due())
            mCache->reportRuntimeDiagnostics("parsed_nif", referenceTime,
                effectiveExpiryDelay());
    }

    void NifFileManager::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Nif", frameNumber, mCache->getStats(), *stats);
    }

}
