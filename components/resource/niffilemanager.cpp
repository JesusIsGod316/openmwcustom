#include "niffilemanager.hpp"

#include <iostream>

#include <osg/Object>

#include <components/vfs/manager.hpp>

#include "objectcache.hpp"
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

    NifFileManager::~NifFileManager() = default;

    Nif::NIFFilePtr NifFileManager::get(VFS::Path::NormalizedView name)
    {
        osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(name);
        if (obj != nullptr)
            return static_cast<NifFileHolder*>(obj.get())->mNifFile;

        Debug::RuntimeDiagnostics::Operation diagnostic("nif_cache_miss", name.value());
        auto file = std::make_shared<Nif::NIFFile>(name);
        Nif::Reader reader(*file, mEncoder);
        reader.parse(mVFS->get(name));
        obj = new NifFileHolder(file);
        mCache->addEntryToObjectCache(name.value(), obj);
        return file;
    }

    void NifFileManager::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Nif", frameNumber, mCache->getStats(), *stats);
    }

}
