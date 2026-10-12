// Actual ImageManager precondition gate. Does not mark its images STATIC.
#include <components/resource/imagemanager.hpp>
#include <components/resource/immutableimagesource.hpp>
#include <components/vfs/archive.hpp>
#include <osgDB/Registry>
#include <array>
#include <iostream>
#include <sstream>

namespace
{
    using namespace Resource;
    using Bytes=DerivedAssets::Bytes;
    unsigned checks=0;
    void require(bool value,const char* message) { ++checks;if (!value) throw std::runtime_error(message); }
    struct File final : VFS::File
    {
        Bytes bytes;unsigned opens=0;
        Files::IStreamPtr open() override { ++opens;return std::make_unique<ImageSource::SourceStream>(bytes); }
        std::optional<VFS::SourceOpenEstimate> getOpenEstimate() const override { return VFS::SourceOpenEstimate{bytes.size(),1024}; }
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
}
int main() try
{
    for (const std::string extension : {"dds","tga","png"})for (bool canonical : {false,true})
    {
        auto vfs=std::make_shared<VFS::Manager>();
        auto archive=std::make_unique<Archive>("textures/actual."+extension);
        auto* file=&archive->file;file->bytes=encoded(extension);
        const auto path=archive->path;vfs->addArchive(std::move(archive));vfs->buildIndex();
        ImageManager manager(vfs.get(),120);manager.configureCanonicalPublication(canonical);
        auto image=manager.getImage(path);
        require(image && image!=manager.getWarningImage(),"Actual ImageManager selects the installed decoder and returns a required image");
        require(image->s()==7 && image->t()==5,"Actual publication preserves decoded dimensions");
        require(image->getFileName()==path.value(),"Actual publication preserves the canonical resource filename");
        require(!image->requiresUpdateCall(),"Actual file image has no decoder update work");
        require(image->getAllocationMode()!=osg::Image::NO_DELETE,"Actual file image owns its decoded allocation");
        require(imagePayloadExtent(*image).bytes>0,"Actual image has a supported checked payload extent");
        const auto variance=image->getDataVariance();
        const auto modified=image->getModifiedCount();
        auto cached=manager.getImage(path);
        require(cached==image && file->opens==1,"Warm lookup returns the canonical winner without another source open");
        require(cached->getDataVariance()==variance && cached->getModifiedCount()==modified,
            "Warm cache lookup does not invent an immutable producer contract");
        auto cpu=image;
        manager.clearCache();
        require(cpu->data()!=nullptr,"Ordinary external CPU reference remains a strong payload pin after clear");
        auto replacement=manager.getImage(path);
        require(replacement!=image && file->opens==2,"Clear creates a fresh actual decoder publication while the old CPU owner survives");
        require(image->getDataVariance()==variance,"No gate changes actual decoder data variance");
        std::cout<<"OBS actual ImageManager extension="<<extension<<" canonical="<<canonical
            <<" variance="<<static_cast<int>(variance)<<" update="<<image->requiresUpdateCall()
            <<" origin="<<image->getOrigin()<<" bytes="<<imagePayloadExtent(*image).bytes
            <<" class="<<image->className()<<'\n';
    }
    std::cout<<"PASS actual image source publication: "<<checks<<" assertions; required decoder, canonical winner, actual variance and CPU pins\n";
    std::cout<<"LIMITATIONS: native actual ImageManager gate; no source receipt, factory, image retirement or RAM/FPS proof.\n";
    return 0;
}
catch(const std::exception& e) {std::cerr<<"FAIL actual image source publication after "<<checks<<" assertions: "<<e.what()<<'\n';return 1;}
