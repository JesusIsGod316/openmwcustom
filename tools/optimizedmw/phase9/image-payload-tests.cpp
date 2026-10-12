#include <components/resource/imagepayload.hpp>
#include <components/resource/objectcache.hpp>
#include <components/resource/cachediagnostics.hpp>
#include <osgDB/Registry>
#include <osgDB/Options>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace
{
    unsigned checks=0;
    void require(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
    using Bytes=std::vector<unsigned char>;
    void put(Bytes& bytes,unsigned offset,unsigned value)
    {for(unsigned i=0;i<4;++i)bytes[offset+i]=static_cast<unsigned char>(value>>(8*i));}
    Bytes dds(unsigned width,unsigned height,unsigned block,unsigned fourcc,bool mips)
    {
        Bytes bytes(128,0);put(bytes,0,0x20534444);put(bytes,4,124);put(bytes,8,0x21007);
        put(bytes,12,height);put(bytes,16,width);put(bytes,76,32);put(bytes,80,4);put(bytes,84,fourcc);
        put(bytes,108,0x401008);
        unsigned count=0;
        do
        {
            bytes.resize(bytes.size()+((width+3)/4)*((height+3)/4)*block,0);
            ++count;
            if(!mips || (width==1 && height==1))break;
            width=(std::max)(1u,width/2);height=(std::max)(1u,height/2);
        }while(true);
        put(bytes,28,count);put(bytes,20,static_cast<unsigned>(bytes.size()-128));return bytes;
    }
    osg::ref_ptr<osg::Image> decode(const Bytes& bytes)
    {
        auto* reader=osgDB::Registry::instance()->getReaderWriterForExtension("dds");
        require(reader,"Installed DDS reader exists");
        std::istringstream input(std::string(bytes.begin(),bytes.end()),std::ios::binary);
        osg::ref_ptr<osgDB::Options> options=new osgDB::Options("dds_dxt1_detect_rgba ignoreTga2Fields");
        auto result=reader->readImage(input,options);
        require(result.success() && result.getImage(),"Actual DDS decoding succeeds");
        return result.getImage();
    }
    unsigned char descriptorByte=0;
    osg::ref_ptr<osg::Image> descriptor(int w,int h,int depth,GLenum format,GLenum type,unsigned packing=1)
    {
        osg::ref_ptr<osg::Image> result=new osg::Image;
        result->setImage(w,h,depth,format,format,type,&descriptorByte,osg::Image::NO_DELETE,packing);
        return result; // Metadata-only; never read this descriptor's fake backing.
    }
    void declined(const osg::Image& image,Resource::ImagePayloadExtent::Refusal reason,const char* message)
    {const auto extent=Resource::imagePayloadExtent(image);require(!extent && !extent.bytes && extent.refusal==reason,message);}
}
int main() try
{
    using namespace Resource;
    using R=ImagePayloadExtent::Refusal;
    // Actual reader allocations, not synthetic total-size labels. Covers tiny,
    // odd, rectangular and full chains in every ordinary S3TC DDS family.
    for(const auto codec:std::array<std::pair<unsigned,unsigned>,3>{{{8,0x31545844},{16,0x33545844},{16,0x35545844}}})
        for(const auto size:std::array<std::pair<unsigned,unsigned>,5>{{{32,32},{3,7},{1,1},{64,8},{9,5}}})
            for(const bool mips:{false,true})
            {
                const auto source=dds(size.first,size.second,codec.first,codec.second,mips);
                auto image=decode(source);const auto extent=imagePayloadExtent(*image);
                require(extent && extent.bytes==source.size()-128,"Exact actual DDS allocation span survives odd sizes and mip chains");
                require(std::equal(image->data(),image->data()+extent.bytes,source.begin()+128),"Qualified span covers precisely the actual decoded DDS block payload");
            }
    auto single=decode(dds(32,32,8,0x31545844,false));
    require(single->getTotalSizeInBytesIncludingMipmaps()==2048 && imagePayloadExtent(*single).bytes==512,
        "Real single-level DXT1 legacy4x overestimate is corrected");

    for(const auto format:{GL_ALPHA,GL_LUMINANCE_ALPHA,GL_RGB,GL_RGBA,GL_BGR,GL_BGRA,GL_RED,GL_RG})
        for(const auto packing:{1u,2u,4u,8u})
        {
            osg::ref_ptr<osg::Image> image=new osg::Image;
            image->allocateImage(3,7,1,format,GL_UNSIGNED_BYTE,packing);
            const auto row=(3*ImagePayloadDetail::channels(format)+packing-1)/packing*packing;
            const auto extent=imagePayloadExtent(*image);
            require(extent && extent.bytes==row*7,"Actual raw allocation row padding is counted with each valid packing");
            require(extent.bytes==image->getTotalSizeInBytesIncludingMipmaps(),"Ordinary supported raw accounting is unchanged");
        }
    for(const auto type:{GL_UNSIGNED_SHORT,GL_HALF_FLOAT,GL_FLOAT,GL_UNSIGNED_SHORT_4_4_4_4,GL_UNSIGNED_INT_8_8_8_8})
    {
        auto image=descriptor(4,2,1,GL_RGBA,type,4);const auto extent=imagePayloadExtent(*image);
        require(extent && extent.bytes==4*2*ImagePayloadDetail::pixelBits(GL_RGBA,type)/8,"Supported scalar and packed raw types have checked extent");
    }
    for(const auto format:{ImagePayloadDetail::RedInteger,ImagePayloadDetail::RgbInteger,ImagePayloadDetail::RgbaInteger,
            ImagePayloadDetail::BgrInteger,ImagePayloadDetail::BgraInteger})
    {
        auto image=descriptor(4,2,1,format,GL_UNSIGNED_INT,4);
        require(imagePayloadExtent(*image).bytes==4*2*ImagePayloadDetail::channels(format)*4,
            "Core and Windows EXT integer-format aliases share the exact metadata extent");
    }
    auto partial=descriptor(4,4,1,GL_RGBA,GL_UNSIGNED_BYTE,4);
    partial->setMipmapLevels({64});require(imagePayloadExtent(*partial).bytes==80,"Exact partial mip chain is supported");
    partial->setMipmapLevels({64,80});require(imagePayloadExtent(*partial).bytes==84,"Exact complete raw mip chain is supported");
    partial->setMipmapLevels({63});declined(*partial,R::Mips,"Overlapping mip offsets decline byte authority");
    partial->setMipmapLevels({65});declined(*partial,R::Mips,"Unknown padded mip gaps decline byte authority");
    partial->setMipmapLevels({64,80,84});declined(*partial,R::Mips,"Repeated1x1 tail mip levels are invalid");
    partial->setMipmapLevels({});partial->setRowLength(5);declined(*partial,R::Rows,"Noncontiguous row layout is unmeasured");
    partial->setRowLength(4);require(imagePayloadExtent(*partial).bytes==64,"Explicit equal row length stays ordinary contiguous layout");
    partial->setPacking(3);declined(*partial,R::Packing,"Unsupported packing is not silently rounded");
    auto empty=osg::ref_ptr<osg::Image>(new osg::Image);declined(*empty,R::NoBacking,"Absent CPU data is not a fabricated resident payload");
    auto invalid=descriptor(0,4,1,GL_RGBA,GL_UNSIGNED_BYTE);declined(*invalid,R::Dimensions,"Zero dimensions are invalid");
    invalid=descriptor(4,4,2,GL_RGBA,GL_UNSIGNED_BYTE);declined(*invalid,R::Depth,"Unknown volume or array slice layout stays unmeasured");
    invalid=descriptor(4,4,1,GL_COMPRESSED_RGBA_ARB,GL_UNSIGNED_BYTE);declined(*invalid,R::Format,"Generic driver compression is not a guessed block format");
    invalid=descriptor(4,4,1,GL_RGBA,GL_BITMAP);declined(*invalid,R::Type,"Unsupported raw element type is not known backing");
    invalid=descriptor((std::numeric_limits<int>::max)(),2,1,GL_RGBA,GL_FLOAT);declined(*invalid,R::Overflow,"Huge descriptor cannot wrap OSG's32bit byte extent");
    invalid=descriptor(65536,65536,1,GL_RGBA,GL_UNSIGNED_BYTE);declined(*invalid,R::Overflow,"4GiB boundary is declined before overflow");
    auto rgtc=descriptor(7,5,1,GL_COMPRESSED_RED_RGTC1_EXT,GL_UNSIGNED_BYTE);require(imagePayloadExtent(*rgtc).bytes==32,"Fixed-rate RGTC1 block layout is explicit");
    rgtc=descriptor(7,5,1,GL_COMPRESSED_RED_GREEN_RGTC2_EXT,GL_UNSIGNED_BYTE);require(imagePayloadExtent(*rgtc).bytes==64,"Fixed-rate RGTC2 block layout is explicit");

    SpeculativeBudget budget;
    osg::ref_ptr<GenericObjectCache<unsigned>> cache=new GenericObjectCache<unsigned>;
    cache->setSpeculativeBudget(&budget);
    cache->addEntryToObjectCache(1,single,0,1);
    cache->addEntryToObjectCache(2,single,0,1);
    auto lease=cache->getLeaseFromObjectCache(1);
    require(lease.bytes()==512 && budget.stats().knownOwnerBytes==512,"Actual cache charge uses corrected DDS extent and deduplicates aliases");
    require(cache->getLeaseFromObjectCache(2).mCharge==lease.mCharge,"Two cache slots retain one exact payload charge");
    cache->addEntryToObjectCache(3,invalid,0,1);
    require(cache->getLeaseFromObjectCache(3).bytes()==0 && budget.stats().unknownOwners==1,
        "Unsupported source remains required-readable but cannot grant speculative byte admission");
    CacheDiagnosticCensus census;
    census.add("dds",single,0,1);census.add("alias",single,0,1);census.add("unknown",invalid,0,1);
    require(census.knownPayloadBytes==512 && census.sharedPayloadReferences==1 && census.unmeasuredEntries==1,
        "Actual census and cache ledger agree on checked/deduplicated/unknown payload classes");
    cache->clear();require(budget.stats().knownOwnerBytes==512,"Held exact lease protects its own logical backing after cache retirement");
    lease={};require(budget.stats().knownOwnerBytes==0 && budget.stats().unknownOwners==0,"Actual final charges retire without ledger debt");
    std::cout<<"PASS image payload: "<<checks<<" assertions; actual DDS allocation bytes, raw rows, mip offsets, types, overflow, cache/lease/census\n"
             <<"LIMITATIONS: supported tight2D layouts only; unknown volume/array, noncontiguous/gapped, generic compression and custom layouts remain unmeasured/ordinary. Metadata span is not allocator capacity or process/GPU RAM. No memory/FPS gain claimed.\n";
    return 0;
}
catch(const std::exception& error){std::cerr<<"FAIL image payload: "<<error.what()<<'\n';return 1;}
