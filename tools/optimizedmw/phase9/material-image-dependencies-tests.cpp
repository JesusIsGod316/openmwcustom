#include <components/resource/materialimagedependencies.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/preloadestimate.hpp>
#include <components/nifosg/controller.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>
#include <osg/Group>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace
{
    void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
    struct MemoryFile final : VFS::File
    {
        std::string data;
        Files::IStreamPtr open() override { return std::make_unique<std::istringstream>(data, std::ios::binary); }
        std::optional<VFS::SourceOpenEstimate> getOpenEstimate() const override { return VFS::SourceOpenEstimate{data.size(),data.size()+65536}; }
        std::filesystem::file_time_type getLastModified() const override { return {}; }
        std::string getStem() const override { return "material dependency fixture"; }
    };
    struct Archive final : VFS::Archive
    {
        std::map<std::string, std::unique_ptr<MemoryFile>> files;
        void listResources(VFS::FileMap& out) override { for (auto& [path,file] : files) out.emplace(VFS::Path::Normalized(path),file.get()); }
        bool contains(VFS::Path::NormalizedView path) const override { return files.contains(std::string(path.value())); }
        std::string getDescription() const override { return "material dependency fixture"; }
    };
    std::string dds(unsigned side, unsigned mipCount)
    {
        std::string data(128, '\0');
        const auto put = [&](unsigned at, unsigned value) { std::memcpy(data.data()+at,&value,4); };
        put(0,0x20534444); put(4,124); put(8,0x2100f); put(12,side); put(16,side); put(20,side*4); put(28,mipCount);
        put(76,32); put(80,0x41); put(88,32); put(92,0xff); put(96,0xff00); put(100,0xff0000); put(104,0xff000000); put(108,0x401008);
        for(unsigned level=0;level<mipCount;++level) { data.append(side*side*4, '\x3f'); side=(std::max)(1u,side/2); }
        return data;
    }
    Resource::OpenGlPressureSample sample(void* context)
    {
        auto& budget=*static_cast<Resource::SpeculativeBudget*>(context);
        Resource::OpenGlPressureSample value; value.generation=1; value.growthWatermarkValid=true; value.accountedGrowth=budget.watermark()->load();
        value.memory.physicalValid=value.memory.commitValid=true;
        value.memory.physicalTotal=32ull*1024*Resource::SpeculativeBudget::MiB;
        value.memory.physicalAvailable=value.memory.commitAvailable=16ull*1024*Resource::SpeculativeBudget::MiB;
        value.decision.state=Resource::OpenGlPressure::Normal; value.decision.admissionsPerSample=4;
        value.decision.limits=Resource::OpenGlPressurePolicy{}.limits(value.memory.physicalTotal);
        return value;
    }
}
int main() try
{
    const auto large=dds(2048,12);
    const auto bytes=std::span(reinterpret_cast<const unsigned char*>(large.data()),148);
    const auto historical=Resource::estimateImage(bytes,large.size());
    const auto precise=Resource::estimateImage(bytes,large.size(),true);
    require(precise.knownDimensions && precise.retained==large.size()-128 && precise.peak<historical.peak,
        "raw DDS authored mip allowance must cover actual pixels without codec assumptions");
    auto compressed=large; unsigned fourcc=0x31545844, flags=4; std::memcpy(compressed.data()+80,&flags,4); std::memcpy(compressed.data()+84,&fourcc,4);
    const auto compressedBytes=std::span(reinterpret_cast<const unsigned char*>(compressed.data()),148);
    require(Resource::estimateImage(compressedBytes,compressed.size(),true).peak==Resource::estimateImage(compressedBytes,compressed.size()).peak,
        "compressed/unqualified DDS must retain historical estimate");
    VFS::Manager vfs; auto archive=std::make_unique<Archive>();
    for(const auto* path : {"textures/animated_n.dds","textures/animated_nh.dds","textures/animated_s.dds"})
    { auto file=std::make_unique<MemoryFile>(); file->data=dds(16,5); archive->files.emplace(path,std::move(file)); }
    vfs.addArchive(std::move(archive)); vfs.buildIndex();
    const auto normal=Resource::materialNormalDependency(vfs,"Textures/Animated.dds","_nh","_n");
    require(normal && normal->normalHeight && normal->path.value()=="textures/animated_nh.dds", "production height-map precedence/VFS normalization");
    require(!Resource::materialNormalDependency(vfs,"textures/missing.dds","_nh","_n"), "missing companion must not be fabricated");
    Resource::ImageManager images(&vfs,60); Resource::SpeculativeBudget budget; images.setSpeculativeBudget(&budget);
    Resource::SceneManager scenes(&vfs,&images,nullptr,nullptr,60);
    scenes.setAutoUseNormalMaps(true); scenes.setNormalMapPattern("_n"); scenes.setNormalHeightMapPattern("_nh");
    scenes.setAutoUseSpecularMaps(true); scenes.setSpecularMapPattern("_s");
    osg::ref_ptr<osg::Image> diffuse=new osg::Image; diffuse->setFileName("textures/animated.dds");
    osg::ref_ptr<osg::Texture2D> texture=new osg::Texture2D(diffuse);
    osg::ref_ptr<osg::Group> model=new osg::Group;
    model->setUpdateCallback(new NifOsg::FlipController(0,.2f,{texture}));
    std::vector<Resource::ObjectCacheLease> owners; std::uint64_t retained=0;
    {
        Resource::SpeculativeScope scope(&budget,sample,&budget);
        scenes.prepareMaterialImages(*model,owners,retained);
        require(owners.empty(), "control must not decode or pin future material images");
        scenes.configureMaterialImagePreparation(true,16);
        scenes.prepareMaterialImages(*model,owners,retained);
        require(owners.size()==2 && retained==2*(16*16+8*8+4*4+2*2+1)*4, "future texture controller must prepare height/specular images including all mips");
        const auto first=images.getImage(normal->path); require(first==owners[0].mValue, "ordinary demand must adopt the exact worker publication");
        scenes.prepareMaterialImages(*model,owners,retained); require(owners.size()==2, "repeated source must not create duplicate cell pins");
        bool denied=false; try { images.prepareImage(normal->path,1024*1024,1); } catch(const Resource::SpeculativeDeferred&) { denied=true; }
        require(denied,"retained allowance must reject an oversized existing image");
    }
    require(images.imageLeaseCurrent(normal->path,owners[0]), "lease valid at matching VFS generation");
    vfs.buildIndex(); require(!images.imageLeaseCurrent(normal->path,owners[0]), "new VFS generation must invalidate preparation ownership");
    std::cout << "PASS material discovery, exact raw-DDS mips, ordinary cache adoption, bounded pins, generation invalidation\n";
    return 0;
}
catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
