// Actual ImageManager precondition gate. Does not mark its images STATIC.
#include <components/resource/imagemanager.hpp>
#include <components/resource/immutableimagesource.hpp>
#include <components/vfs/archive.hpp>
#include <osgDB/Registry>
#include <array>
#include <iostream>
#include <sstream>
#include <atomic>

namespace
{
    using namespace Resource;
    using Bytes=DerivedAssets::Bytes;
    unsigned checks=0;
    void require(bool value,const char* message) { ++checks;if (!value) throw std::runtime_error(message); }
    struct File final : VFS::File
    {
        Bytes bytes;std::atomic_uint opens{0};bool estimated=true;
        Files::IStreamPtr open() override { ++opens;return std::make_unique<ImageSource::SourceStream>(bytes); }
        std::optional<VFS::SourceOpenEstimate> getOpenEstimate() const override
        { if (!estimated) return {};return VFS::SourceOpenEstimate{bytes.size(),1024}; }
        std::filesystem::file_time_type getLastModified() const override { return {}; }
        std::string getStem() const override { return "actual-image-manager-source"; }
    };
    struct Archive final : VFS::Archive
    {
        File file;VFS::Path::Normalized path;
        explicit Archive(std::string name) : path(name) {}
        void listResources(VFS::FileMap& out) override { out.insert_or_assign(path,&file); }
        bool contains(VFS::Path::NormalizedView p) const override { return p.value()==path.value(); }
        std::string getDescription() const override { return "actual-image-source-native"; }
    };
    Bytes encoded(const std::string& extension)
    {
        osg::ref_ptr<osg::Image> image=new osg::Image;
        image->allocateImage(7,5,1,GL_RGBA,GL_UNSIGNED_BYTE);
        for (int y=0;y<5;++y)for (int x=0;x<7;++x)
        { auto* p=image->data(x,y);p[0]=static_cast<unsigned char>(x*29);p[1]=static_cast<unsigned char>(y*37);p[2]=123;p[3]=255; }
        auto* reader=osgDB::Registry::instance()->getReaderWriterForExtension(extension);
        require(reader!=nullptr,"Actual installed encoder available");
        std::ostringstream stream(std::ios::binary);
        require(reader->writeImage(*image,stream).success(),"Actual installed encoder creates the publication fixture");
        const auto text=stream.str();return Bytes(text.begin(),text.end());
    }
    OpenGlPressureSample sample(void* owner)
    {
        auto& budget=*static_cast<SpeculativeBudget*>(owner);
        OpenGlPressureSample s;
        s.generation=1;s.growthWatermarkValid=true;s.accountedGrowth=budget.watermark()->load();
        s.memory.physicalValid=s.memory.commitValid=true;
        s.memory.physicalTotal=32ull*1024*SpeculativeBudget::MiB;
        s.memory.physicalAvailable=16ull*1024*SpeculativeBudget::MiB;
        s.memory.commitAvailable=24ull*1024*SpeculativeBudget::MiB;
        s.decision.state=OpenGlPressure::Normal;s.decision.admissionsPerSample=4;
        s.decision.limits=OpenGlPressurePolicy{}.limits(s.memory.physicalTotal);return s;
    }
}
int main() try
{
    for (const std::string extension : {"dds","tga","png"})for (bool canonical : {false,true})
        for (bool sources : {false,true})for (bool disableFlip : {false,true})
    {
        auto vfs=std::make_shared<VFS::Manager>();
        auto archive=std::make_unique<Archive>("textures/actual."+extension);
        auto* file=&archive->file;file->bytes=encoded(extension);
        const auto path=archive->path;vfs->addArchive(std::move(archive));vfs->buildIndex();
        SpeculativeBudget budget({512*SpeculativeBudget::MiB,4,1,256*SpeculativeBudget::MiB,64*SpeculativeBudget::MiB,1});
        auto manager=std::make_unique<ImageManager>(vfs.get(),120);manager->configureCanonicalPublication(canonical);
        manager->setSpeculativeBudget(&budget);manager->configureImageSources(sources,sample,&budget);
        auto image=manager->getImage(path,disableFlip);
        require(image && image!=manager->getWarningImage(),"Actual ImageManager selects the installed decoder and returns a required image");
        require(image->s()==7 && image->t()==5,"Actual publication preserves decoded dimensions");
        require(image->getFileName()==path.value(),"Actual publication preserves the canonical resource filename");
        require(!image->requiresUpdateCall(),"Actual file image has no decoder update work");
        require(image->getAllocationMode()!=osg::Image::NO_DELETE,"Actual file image owns its decoded allocation");
        require(imagePayloadExtent(*image).bytes>0,"Actual image has a supported checked payload extent");
        const auto variance=image->getDataVariance();
        const auto modified=image->getModifiedCount();
        auto cached=manager->getImage(path,disableFlip);
        require(cached==image && file->opens==1,"Warm lookup returns the canonical winner without another source open");
        require(cached->getDataVariance()==variance && cached->getModifiedCount()==modified,
            "Warm cache lookup does not invent an immutable producer contract");
        const auto recipe=ImageManager::imageSourceRecipe(*image);
        const bool eligible=sources && extension!="png";
        require(bool(recipe)==eligible,"Supported source receipts engage in the actual pipeline; ordinary/other plugins preserve their backing");
        const auto bytes=imagePayloadExtent(*image).bytes;
        require(budget.stats().knownOwnerBytes==bytes && !budget.stats().futureBytes,
            "Receipt and actual image cache share one backing charge; no scratch reservation survives publication");
        if (recipe)
        {
            require(recipe->matches(*image) && recipe->revision().sourceSha256==DerivedAssets::digest(file->bytes)
                && recipe->revision().imageManagerPostprocess && recipe->revision().disableFlip==disableFlip
                && recipe->revision().options=="dds_dxt1_detect_rgba ignoreTga2Fields",
                "Actual winning source bytes/options/origin/pixels are sealed without another read or decoder");
            auto rebuilt=recipe->reconstruct(&budget,sample(&budget));
            require(rebuilt && recipe->matches(*rebuilt.image) && rebuilt.image->getFileName()==path.value(),
                "Actual ImageManager normalization reconstructs exact asymmetric source pixels and metadata");
            require(rebuilt.image->getDataVariance()==variance,"Reconstruction uses actual decoder variance without STATIC relabeling");
            require(file->opens==2 && budget.stats().futureBytes>0,"Reconstruction input opens once and output retains its admission grant");
        }
        auto cpu=image;
        cached=nullptr;manager->clearCache();
        require(cpu->data()!=nullptr,"Ordinary external CPU reference remains a strong payload pin after clear");
        require(budget.stats().knownOwnerBytes==(eligible?bytes:0),
            "Receipted image keeps its backing charge with actual external CPU owner after cache clear");
        auto replacement=manager->getImage(path,disableFlip);
        require(replacement!=image && file->opens==(eligible?3u:2u),"Clear creates a fresh actual decoder publication while the old CPU owner survives");
        require(image->getDataVariance()==variance,"No gate changes actual decoder data variance");
        std::cout<<"OBS actual ImageManager extension="<<extension<<" canonical="<<canonical
            <<" sources="<<sources<<" disableFlip="<<disableFlip<<" variance="<<static_cast<int>(variance)<<" update="<<image->requiresUpdateCall()
            <<" origin="<<image->getOrigin()<<" bytes="<<imagePayloadExtent(*image).bytes
            <<" class="<<image->className()<<'\n';
        replacement=nullptr;manager.reset();
        if (recipe) require(!recipe->reconstruct(&budget,sample(&budget)),"ImageManager destruction closes its borrowed VFS source authority despite surviving recipes/CPU readers");
        require(cpu->data()!=nullptr,"Manager destruction never frees an externally pinned CPU payload");
        image=nullptr;cpu=nullptr;
        require(!budget.stats().futureBytes && !budget.stats().knownOwnerBytes && !budget.stats().experimentRetainedBytes,
            "Final actual image backing retirement repays all surviving unique charges");
    }
    // Unknown materialization remains one ordinary required open. A candidate
    // refusal must never turn a valid source into the warning image.
    {
        VFS::Manager vfs;auto archive=std::make_unique<Archive>("textures/unknown.dds");
        auto* file=&archive->file;file->bytes=encoded("dds");file->estimated=false;
        const auto path=archive->path;vfs.addArchive(std::move(archive));vfs.buildIndex();
        SpeculativeBudget budget({512*SpeculativeBudget::MiB,4,1,256*SpeculativeBudget::MiB,64*SpeculativeBudget::MiB,1});
        ImageManager manager(&vfs,120);manager.setSpeculativeBudget(&budget);manager.configureImageSources(true,sample,&budget);
        auto image=manager.getImage(path);
        require(image && image!=manager.getWarningImage() && file->opens==1 && !ImageManager::imageSourceRecipe(*image),
            "Unknown source admission uses one ordinary required decoder and no false warning or retirement authority");
    }
    std::cout<<"PASS actual image source publication: "<<checks<<" assertions; required decoder, canonical winner, actual variance and CPU pins\n";
    std::cout<<"LIMITATIONS: native actual ImageManager source receipts; no factory/image retirement, distinct clone-allocation charging, arbitrary plugin, RAM/FPS or game proof.\n";
    return 0;
}
catch(const std::exception& e) {std::cerr<<"FAIL actual image source publication after "<<checks<<" assertions: "<<e.what()<<'\n';return 1;}
