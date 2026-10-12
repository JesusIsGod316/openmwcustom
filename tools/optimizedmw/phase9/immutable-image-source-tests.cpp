#include <components/resource/immutableimagesource.hpp>
#include <components/vfs/archive.hpp>
#include <atomic>
#include <iostream>
#include <sstream>
#include <thread>

namespace
{
    unsigned checks=0;
    void require(bool value,const char* message)
    { ++checks; if (!value) throw std::runtime_error(message); }
    using namespace Resource;
    using Bytes=DerivedAssets::Bytes;
    OpenGlPressureSample sample(SpeculativeBudget& budget)
    {
        OpenGlPressureSample s;
        s.generation=1; s.growthWatermarkValid=true; s.accountedGrowth=budget.watermark()->load();
        s.memory.physicalValid=s.memory.commitValid=true;
        s.memory.physicalTotal=32ull*1024*SpeculativeBudget::MiB;
        s.memory.physicalAvailable=16ull*1024*SpeculativeBudget::MiB;
        s.memory.commitAvailable=24ull*1024*SpeculativeBudget::MiB;
        s.decision.state=OpenGlPressure::Normal; s.decision.admissionsPerSample=4;
        s.decision.limits=OpenGlPressurePolicy{}.limits(s.memory.physicalTotal);
        return s;
    }
    struct File final : VFS::File
    {
        Bytes bytes;
        std::atomic_uint opens{0};
        bool estimated=true,fail=false;
        int sizeDelta=0;
        std::uint64_t peak=0;
        std::optional<VFS::SourceOpenEstimate> getOpenEstimate() const override
        {
            if (!estimated) return {};
            return VFS::SourceOpenEstimate{static_cast<std::uint64_t>(static_cast<std::int64_t>(bytes.size())+sizeDelta),peak};
        }
        Files::IStreamPtr open() override
        {
            ++opens;
            if (fail) throw std::runtime_error("Native missing/unreadable source");
            return std::make_unique<Files::IMemStream>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        }
        std::filesystem::file_time_type getLastModified() const override { return {}; }
        std::string getStem() const override { return "immutable-source-native"; }
    };
    struct Archive final : VFS::Archive
    {
        File file;
        VFS::Path::Normalized path;
        explicit Archive(std::string name) : path(name) {}
        void listResources(VFS::FileMap& out) override { out.emplace(path,&file); }
        bool contains(VFS::Path::NormalizedView p) const override { return p.value()==path.value(); }
        std::string getDescription() const override { return "deliberately-identical-archive-description"; }
    };
    struct Fixture
    {
        std::shared_ptr<VFS::Manager> vfs=std::make_shared<VFS::Manager>();
        std::shared_ptr<ImageSource::Authority> authority;
        VFS::Path::Normalized path;
        File* file=nullptr;
        explicit Fixture(std::string name,Bytes bytes) : path(name)
        {
            auto archive=std::make_unique<Archive>(name);
            file=&archive->file; file->bytes=std::move(bytes);
            vfs->addArchive(std::move(archive)); vfs->buildIndex();
            authority=std::make_shared<ImageSource::Authority>(vfs);
        }
        osg::ref_ptr<osg::Image> read() const
        {
            auto* reader=osgDB::Registry::instance()->getReaderWriterForExtension(osgDB::getLowerCaseFileExtension(std::string(path.value())));
            if (!reader) throw std::runtime_error("Actual installed reader missing");
            auto input=vfs->get(path);
            auto result=reader->readImage(*input,nullptr);
            osg::ref_ptr<osg::Image> image=result.takeImage();
            if (image) image->setDataVariance(osg::Object::STATIC);
            return image;
        }
    };
    void put(Bytes& bytes,unsigned offset,std::uint32_t value)
    { for (unsigned i=0;i<4;++i) bytes[offset+i]=static_cast<std::uint8_t>(value>>(8*i)); }
    Bytes dds(unsigned width,unsigned height,unsigned levels,unsigned blockBytes)
    {
        std::size_t payload=0;
        for (unsigned i=0;i<levels;++i)
            payload+=std::size_t((std::max(1u,width>>i)+3)/4)*((std::max(1u,height>>i)+3)/4)*blockBytes;
        Bytes bytes(128+payload,0);
        put(bytes,0,0x20534444);put(bytes,4,124);put(bytes,8,0xa1007);
        put(bytes,12,height);put(bytes,16,width);put(bytes,20,static_cast<std::uint32_t>(payload));put(bytes,28,levels);
        put(bytes,76,32);put(bytes,80,4);put(bytes,84,blockBytes==8?0x31545844:0x35545844);put(bytes,108,0x401008);
        for (std::size_t i=128;i<bytes.size();i+=blockBytes)
        {
            if (blockBytes==16) bytes[i]=bytes[i+1]=255;
            const auto color=i+(blockBytes==16?8:0);
            bytes[color]=0xe0;bytes[color+1]=0x07;
        }
        return bytes;
    }
    Bytes encoded(std::string extension,unsigned width,unsigned height)
    {
        osg::ref_ptr<osg::Image> image=new osg::Image;
        image->allocateImage(width,height,1,GL_RGBA,GL_UNSIGNED_BYTE);
        for (unsigned y=0;y<height;++y)
            for (unsigned x=0;x<width;++x)
            {
                auto* pixel=image->data(x,y);
                pixel[0]=static_cast<unsigned char>(x*19);pixel[1]=static_cast<unsigned char>(y*23);
                pixel[2]=170;pixel[3]=255;
            }
        auto* reader=osgDB::Registry::instance()->getReaderWriterForExtension(extension);
        require(reader!=nullptr,"Actual writer available");
        std::ostringstream stream(std::ios::binary);
        require(reader->writeImage(*image,stream).success(),"Actual writer serializes arbitrary small dimensions");
        const auto text=stream.str();return Bytes(text.begin(),text.end());
    }
}
int main() try
{
    using namespace Resource;
    require(DerivedAssets::digest("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA256 revision uses the standard known answer");
    SpeculativeBudget::Config limits;limits.transientLimit=16*SpeculativeBudget::MiB;
    limits.experimentLimit=16*SpeculativeBudget::MiB;limits.experimentScratchLimit=16*SpeculativeBudget::MiB;
    SpeculativeBudget budget(limits);
    ImageSource::Config config;config.maximumDecodedBytes=64*1024;config.decoderScratchAllowance=128*1024;
    for (unsigned block : {8u,16u})
        for (unsigned levels : {1u,2u,4u})
        {
            Fixture f("textures/blocks.dds",dds(11,7,levels,block));
            auto image=f.read();require(image.valid(),"Installed DDS reader decodes the exact native source");
            auto recipe=ImageSource::Recipe::capture(f.authority,f.path,*image,&budget,sample(budget),config);
            require(bool(recipe),"Non-square block format/partial or full mip chain captures a checked immutable recipe");
            require(recipe->revision().sourceSha256==DerivedAssets::digest(f.file->bytes),"Revision hashes all winning source bytes");
            require(recipe->revision().layout.bytes==imagePayloadExtent(*image).bytes,"Decoded revision uses exact supported block extent");
            auto second=ImageSource::Recipe::capture(f.authority,f.path,*image,&budget,sample(budget),config);
            require(second && second->revision()==recipe->revision(),"Equal winning source and decoder contracts preserve equal comparison identity");
            auto loaded=recipe->reconstruct(&budget,sample(budget));
            require(bool(loaded) && recipe->matches(*loaded.image),"Actual VFS DDS reconstruction reproduces exact layout and SHA256 pixels");
            require(budget.stats().experimentFutureBytes>0,"Decoded lease retains admission while output is alive");
        }
    require(budget.stats().futureBytes==0,"Each returned source/output lease retires admission without leaks");
    for (const char* extension : {"dds","tga"})
    {
        Fixture f(std::string("textures/pixels.")+extension,encoded(extension,7,5));
        auto image=f.read();require(image.valid(),"Installed raw pixel reader succeeds");
        auto recipe=ImageSource::Recipe::capture(f.authority,f.path,*image,&budget,sample(budget),config);
        require(bool(recipe),"Arbitrary small raw dimensions retain an immutable source recipe");
        auto value=recipe->reconstruct(&budget,sample(budget));
        require(value && recipe->matches(*value.image),"Raw pixel source reconstructs exact decoder origin, layout and content");
        value={};
        const auto before=f.file->opens.load();
        auto critical=sample(budget);critical.decision.state=OpenGlPressure::Critical;
        require(!recipe->reconstruct(&budget,critical),"Critical host policy refuses optional reconstruction");
        require(f.file->opens==before,"Refusal happens before opening/decompressing a winning provider");
        f.file->estimated=false;
        require(!recipe->reconstruct(&budget,sample(budget)),"Unknown provider opening peak preserves original path");
        require(f.file->opens==before,"Unknown opening peak cannot allocate a speculative stream");f.file->estimated=true;
        f.file->peak=std::numeric_limits<std::uint64_t>::max();
        require(!recipe->reconstruct(&budget,sample(budget)),"Oversized provider materialization cannot bypass the envelope");f.file->peak=0;
        for (int delta : {-1,1})
        {
            f.file->sizeDelta=delta;
            require(!recipe->reconstruct(&budget,sample(budget)),"Truncated/growing source cannot inherit the captured content authority");
            require(budget.stats().futureBytes==0,"Short or excess input releases the reserved output before return");
        }
        f.file->sizeDelta=0;f.file->fail=true;
        require(!recipe->reconstruct(&budget,sample(budget)),"Missing/unreadable source returns optional refusal, without creating a warning recipe");f.file->fail=false;
        auto saved=f.file->bytes;f.file->bytes.back()^=1;
        require(!recipe->reconstruct(&budget,sample(budget)),"Timestamp-preserving in-place content change invalidates immutable source reconstruction");
        f.file->bytes=saved;
        auto mutated=osg::ref_ptr<osg::Image>(static_cast<osg::Image*>(image->clone(osg::CopyOp::DEEP_COPY_ALL)));
        mutated->data()[0]^=1;
        require(!recipe->matches(*mutated),"Distinct changed decoded color cannot compare as the old source");
        auto huge=f.file->bytes;
        if (std::string_view(extension)=="dds") put(huge,16,0x40000000);
        else {huge[12]=255;huge[13]=255;}
        f.file->bytes=std::move(huge);
        require(!ImageSource::Recipe::capture(f.authority,f.path,*image,&budget,sample(budget),config),
            "Oversized source dimensions decline before entering the installed decoder");f.file->bytes=saved;
        f.vfs->buildIndex();
        const auto reads=f.file->opens.load();
        require(!recipe->reconstruct(&budget,sample(budget)),"Winning VFS generation replacement invalidates the old recipe");
        require(f.file->opens==reads,"Stale generation refuses before source open");
        auto current=ImageSource::Recipe::capture(f.authority,f.path,*image,&budget,sample(budget),config);
        require(current && current->revision()!=recipe->revision(),"New generation issues distinct source authority even for equal paths and timestamps");
        std::vector<std::thread> threads;std::atomic_bool all{true};
        for (unsigned i=0;i<6;++i) threads.emplace_back([&] {
            for (unsigned j=0;j<8;++j) {auto decoded=current->reconstruct(&budget,sample(budget));
                if (!decoded || !current->matches(*decoded.image)) all=false;}
        });
        for (auto& thread:threads) thread.join();
        require(all,"Concurrent source reconstruction has independent decoder/buffer leases and stable content");
        require(budget.stats().futureBytes==0,"Concurrent decode completion leaves no admission debt");
        auto winning=std::make_unique<Archive>(std::string(f.path.value()));
        winning->file.bytes=encoded(extension,9,3);
        f.vfs->addArchive(std::move(winning));f.vfs->buildIndex();
        require(!current->reconstruct(&budget,sample(budget)),"Same-description winning archive override cannot inherit stale recipe");
        auto replacement=f.read();
        auto changed=ImageSource::Recipe::capture(f.authority,f.path,*replacement,&budget,sample(budget),config);
        require(changed && changed->revision().sourceSha256!=current->revision().sourceSha256,
            "Winning source hash changes with actual replacement bytes, independently of archive description");
        f.authority->close();
        require(!changed->reconstruct(&budget,sample(budget)),"Closed provider cannot reopen source through a surviving recipe");
        require(budget.stats().futureBytes==0,"Close/missing/changed-source refusals leave no reservation debt");
    }
    require(budget.stats().experimentFutureBytes==0,"All child reservations are retired at the final boundary");
    std::cout<<"PASS immutable image source: "<<checks<<" assertions; actual winning VFS DDS/TGA SHA256, source changes, bounded admission and concurrent reconstruction\n";
    std::cout<<"LIMITATIONS: source/decoded extent and admission proof only; no hard opaque-plugin scratch ceiling, factory/CPU-consumer conversion, GL retirement, RAM saving or game FPS qualification. Other plugins preserve ordinary backing.\n";
    return 0;
}
catch (const std::exception& e) {std::cerr<<"FAIL immutable image source after "<<checks<<" assertions: "<<e.what()<<'\n';return 1;}
