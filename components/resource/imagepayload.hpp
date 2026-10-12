#ifndef OPENMW_COMPONENTS_RESOURCE_IMAGEPAYLOAD_H
#define OPENMW_COMPONENTS_RESOURCE_IMAGEPAYLOAD_H

#include <osg/Image>
#include <osg/Texture> // OSG compatibility enumerants missing from Windows GL1.1.

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Resource
{
    // Metadata-only semantic payload extent for supported tightly laid out 2D
    // images. No pixel read, allocation, GL query or allocator/RAM claim.
    // Unknown layouts remain ordinary images, but cannot grant byte admission.
    struct ImagePayloadExtent
    {
        enum class Refusal : unsigned char
        { None, NoBacking, Dimensions, Depth, Packing, Rows, Format, Type, Mips, Overflow };
        std::uint64_t bytes = 0;
        Refusal refusal = Refusal::NoBacking;
        explicit operator bool() const noexcept { return refusal == Refusal::None; }
    };
    namespace ImagePayloadDetail
    {
        // Core/EXT aliases have identical values. Windows' system GL1.1
        // header omits the core names; do not depend on include order or
        // redefine its macros. Verified against the installed Khronos headers.
        constexpr GLenum RedInteger=0x8D94, RgbInteger=0x8D98, RgbaInteger=0x8D99,
            BgrInteger=0x8D9A, BgraInteger=0x8D9B;
        // Fixed-rate block families whose actual DDS layout is qualified.
        // PVRTC, generic driver compression, ASTC and unknown plugin layouts
        // deliberately have no guessed byte claim.
        inline unsigned blockBytes(GLenum format) noexcept
        {
            switch(format)
            {
                case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
                case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
                case GL_COMPRESSED_RED_RGTC1_EXT:
                case GL_COMPRESSED_SIGNED_RED_RGTC1_EXT: return 8;
                case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
                case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
                case GL_COMPRESSED_RED_GREEN_RGTC2_EXT:
                case GL_COMPRESSED_SIGNED_RED_GREEN_RGTC2_EXT: return 16;
                default: return 0;
            }
        }
        inline unsigned channels(GLenum format) noexcept
        {
            switch(format)
            {
                case GL_ALPHA: case GL_LUMINANCE: case GL_INTENSITY: case GL_RED:
                case RedInteger: case GL_DEPTH_COMPONENT: return 1;
                case GL_LUMINANCE_ALPHA: case GL_RG: case GL_RG_INTEGER: return 2;
                case GL_RGB: case GL_BGR: case RgbInteger: case BgrInteger: return 3;
                case GL_RGBA: case GL_BGRA: case RgbaInteger: case BgraInteger: return 4;
                default: return 0;
            }
        }
        inline unsigned pixelBits(GLenum format, GLenum type) noexcept
        {
            const auto count=channels(format);
            if (!count) return 0;
            switch(type)
            {
                case GL_UNSIGNED_BYTE: case GL_BYTE: return count*8;
                case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: return count*16;
                case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return count*32;
                case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: return count==3 ? 8 : 0;
                case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV: return count==3 ? 16 : 0;
                case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV:
                case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV: return count==4 ? 16 : 0;
                case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV:
                case GL_UNSIGNED_INT_10_10_10_2: case GL_UNSIGNED_INT_2_10_10_10_REV: return count==4 ? 32 : 0;
                default: return 0;
            }
        }
    }
    inline ImagePayloadExtent imagePayloadExtent(const osg::Image& image) noexcept
    {
        using R=ImagePayloadExtent::Refusal;
        const auto refuse=[](R why){return ImagePayloadExtent{0,why};};
        if (!image.data()) return refuse(R::NoBacking);
        if (image.s()<=0 || image.t()<=0 || image.r()<=0) return refuse(R::Dimensions);
        if (image.r()!=1) return refuse(R::Depth); // Volume/array slice layout is not inferred from an Image.
        const auto packing=image.getPacking();
        if (packing!=1 && packing!=2 && packing!=4 && packing!=8) return refuse(R::Packing);
        if (image.getRowLength()!=0 && image.getRowLength()!=image.s()) return refuse(R::Rows);
        const auto block=ImagePayloadDetail::blockBytes(image.getPixelFormat());
        const auto bits=block ? 0 : ImagePayloadDetail::pixelBits(image.getPixelFormat(),image.getDataType());
        if (!block && !ImagePayloadDetail::channels(image.getPixelFormat())) return refuse(R::Format);
        if ((!block && !bits) || (block && image.getDataType()!=GL_UNSIGNED_BYTE)) return refuse(R::Type);
        std::uint64_t width=image.s(),height=image.t();
        unsigned maximumLevels=1;
        for(auto dimension=(std::max)(width,height);dimension>1;dimension/=2) ++maximumLevels;
        const auto levels=image.getNumMipmapLevels();
        if (!levels || levels>maximumLevels || levels>32) return refuse(R::Mips);
        std::uint64_t end=0;
        // OSG3.6 mip offsets and decoder allocation lengths use unsigned int.
        // Check in64bits before multiplication; never inherit their wraparound.
        constexpr auto byteLimit=static_cast<std::uint64_t>((std::numeric_limits<unsigned int>::max)());
        for(unsigned level=0;level<levels;++level)
        {
            if (image.getMipmapOffset(level)!=end) return refuse(R::Mips); // No overlapping or unknown gaps.
            const auto rows=block ? (height+3)/4 : height;
            const auto rawRow=block ? ((width+3)/4)*block : width*(bits/8);
            const auto row=(rawRow+packing-1)/packing*packing;
            if (!row || row>byteLimit || rows>byteLimit/row) return refuse(R::Overflow);
            const auto bytes=row*rows;
            if (bytes>byteLimit-end) return refuse(R::Overflow);
            end+=bytes;
            width=(std::max)(std::uint64_t{1},width/2);
            height=(std::max)(std::uint64_t{1},height/2);
        }
        return {end,R::None};
    }
}
#endif
