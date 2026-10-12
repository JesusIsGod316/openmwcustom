#ifndef OPENMW_COMPONENTS_RESOURCE_CLOUDIMAGEPREPARATION_H
#define OPENMW_COMPONENTS_RESOURCE_CLOUDIMAGEPREPARATION_H

#include "imagemanager.hpp"
#include "speculativebudget.hpp"
#include <components/sceneutil/workqueue.hpp>
#include <components/vfs/manager.hpp>
#include <atomic>

namespace Resource
{
    class CloudImagePreparation final : public SceneUtil::WorkItem
    {
    public:
        static constexpr std::uint64_t MaximumEstimate = 32 * SpeculativeBudget::MiB;
        static constexpr std::uint64_t MetadataEstimate = 64 * 1024;
        CloudImagePreparation(ImageManager* images, SpeculativeScope::Context context,
            VFS::Path::Normalized path, std::uint64_t generation)
            : mImages(images), mContext(context), mPath(std::move(path)), mGeneration(generation)
        {
            auto* budget = context.budget;
            if (!images || !budget || !context.sample || mPath.value().size() > 4096) throw SpeculativeDeferred{};
            const auto sample = context.sample(context.owner);
            mJob = budget->tryJob(sample, SpeculativePriority::Background);
            if (!mJob) throw SpeculativeDeferred{};
            auto stage = budget->reserve(sample, 2 * MetadataEstimate, SpeculativePriority::Background);
            mMetadataCharge = budget->track({this, 6}, MetadataEstimate, false, &stage);
        }
        void abort() override { mCancelled.store(true, std::memory_order_release); }
        const VFS::Path::Normalized& path() const { return mPath; }
        std::uint64_t generation() const { return mGeneration; }
        bool cancelled() const { return mCancelled.load(std::memory_order_acquire); }
        bool ready() const { return mPublished.load(std::memory_order_acquire) && !cancelled(); }
        const ObjectCacheLease& lease() const { return mLease; }
        std::uint64_t retainedEstimate() const
        { return MetadataEstimate + (mPublished.load(std::memory_order_acquire) ? mLease.bytes() : 0); }
        template<class Release>
        static bool retireCompleted(osg::ref_ptr<CloudImagePreparation>& ticket, Release&& release)
        {
            // The callback takes this original retaining slot by reference.
            // Refusal must not move or drop it; acceptance transfers ownership
            // to the existing bounded maintenance queue before clearing it.
            return ticket && ticket->isDone() && release(ticket, ticket->retainedEstimate());
        }
        std::uint64_t receipt() const { return mReceipt; }
        bool current()
        {
            return ready() && static_cast<const osg::Image*>(mLease.mValue.get())->getModifiedCount() == mImageRevision
                && mImages->imageLeaseCurrent(mPath, mLease);
        }
        void doWork() override
        {
            Debug::HitchTrace::Scope work("weather.cloud_image_prepare", {}, mPath.value());
            mReceipt = work.id();
            try
            {
                if (!mCancelled.load(std::memory_order_acquire)
                    && mImages->getVFS()->getIndexGeneration() == mGeneration)
                {
                    SpeculativeScope scope(mContext);
                    auto lease = mImages->prepareImage(mPath, MaximumEstimate);
                    if (!mCancelled.load(std::memory_order_acquire)
                        && mImages->getVFS()->getIndexGeneration() == mGeneration)
                    {
                        mLease = std::move(lease);
                        mImageRevision = static_cast<const osg::Image*>(mLease.mValue.get())->getModifiedCount();
                        work.retain();
                        work.setMetric(0, "retained_bytes", mLease.bytes());
                        work.setMetric(1, "cache_revision", mLease.mRevision);
                        mPublished.store(true, std::memory_order_release);
                    }
                }
            }
            catch (const SpeculativeDeferred&) { work.setMetric(2, "deferred", 1); }
            catch (const std::exception&) { work.setMetric(3, "failed", 1); }
            mJob.finish();
        }
    private:
        // Charge outlives all descriptor/path/payload owners.
        SpeculativeBudget::ChargePtr mMetadataCharge;
        ObjectCacheLease mLease;
        SpeculativeBudget::Job mJob;
        ImageManager* mImages;
        SpeculativeScope::Context mContext;
        VFS::Path::Normalized mPath;
        const std::uint64_t mGeneration;
        std::uint64_t mReceipt = 0;
        unsigned mImageRevision = 0;
        std::atomic_bool mCancelled{false}, mPublished{false};
    };

}
#endif
