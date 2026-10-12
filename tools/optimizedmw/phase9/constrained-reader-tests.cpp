#include <components/bsa/bsafile.hpp>
#include <components/debug/debuglog.hpp>
#include <components/debug/hitchtrace.hpp>
#include <components/files/constrainedfilestream.hpp>
#include <components/nif/data.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/toutf8/toutf8.hpp>

#include <osg/Image>
#include <osgDB/Registry>
#include <osgDB/ReaderWriter>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void require(bool value, const char* reason)
    {
        if (!value) throw std::runtime_error(reason);
    }
    struct Digest
    {
        std::uint64_t value = 14695981039346656037ull;
        void bytes(const void* data, std::size_t size)
        {
            const auto* p = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i != size; ++i) { value ^= p[i]; value *= 1099511628211ull; }
        }
        template<class T> void scalar(const T& v) { bytes(&v, sizeof(v)); }
        void text(std::string_view s) { scalar(s.size()); bytes(s.data(), s.size()); }
        template<class T> void vector(const std::vector<T>& v) { scalar(v.size()); bytes(v.data(), v.size() * sizeof(T)); }
    };
    std::string readBytes(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        require(input.good(), "could not read image fixture");
        return std::string(std::istreambuf_iterator<char>(input), {});
    }
    std::filesystem::path wrap(const std::filesystem::path& prefix, const std::string& label, const std::string& bytes)
    {
        const auto path = std::filesystem::path(prefix.string() + "-" + label + ".bin");
        std::ofstream output(path, std::ios::binary);
        output << std::string(17, 'P');
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output << std::string(31, 'Q');
        require(output.good(), "could not write bounded parser fixture");
        return path;
    }
    std::uint64_t imageDigest(const osg::Image& image)
    {
        Digest d;
        d.scalar(image.s()); d.scalar(image.t()); d.scalar(image.r());
        d.scalar(image.getInternalTextureFormat()); d.scalar(image.getPixelFormat()); d.scalar(image.getDataType());
        d.scalar(image.getPacking()); d.scalar(image.getOrigin());
        d.scalar(image.getNumMipmapLevels());
        // OSG's generic total-size accessor can describe an uncompressed
        // pixel extent for compressed DDS. Hash actual semantic block payloads
        // at every mip offset, never unspecified padding or adjacent memory.
        int width = image.s(), height = image.t(), depth = image.r();
        for (unsigned level = 0; level != image.getNumMipmapLevels(); ++level)
        {
            std::size_t size;
            if (image.isCompressed())
            {
                const auto block = osg::Image::computeBlockFootprint(image.getPixelFormat());
                const auto blockSize = osg::Image::computeBlockSize(image.getPixelFormat(), image.getPacking());
                require(block.x() > 0 && block.y() > 0 && block.z() > 0 && blockSize > 0,
                    "unsupported compressed-image footprint in fixture");
                size = static_cast<std::size_t>((width + block.x() - 1) / block.x())
                    * ((height + block.y() - 1) / block.y()) * ((depth + block.z() - 1) / block.z()) * blockSize;
            }
            else
                size = osg::Image::computeImageSizeInBytes(width, height, depth,
                    image.getPixelFormat(), image.getDataType(), image.getPacking());
            d.scalar(image.getMipmapOffset(level)); d.scalar(size);
            d.bytes(image.getMipmapData(level), size);
            width = std::max(1, width / 2); height = std::max(1, height / 2); depth = std::max(1, depth / 2);
        }
        return d.value;
    }
    const osgDB::ReaderWriter* reader(std::string_view extension)
    {
        const auto* result = osgDB::Registry::instance()->getReaderWriterForExtension(std::string(extension));
        require(result != nullptr, "actual OSG image reader plugin was not found");
        return result;
    }
    std::uint64_t decodeImage(std::istream& stream, std::string_view extension)
    {
        auto result = reader(extension)->readImage(stream);
        require(result.validImage(), "actual image reader rejected fixture");
        return imageDigest(*result.getImage());
    }
    std::string generatedImage(std::string_view extension)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(512, 128, 1, GL_RGBA, GL_UNSIGNED_BYTE, 1);
        auto* data = image->data();
        for (unsigned i = 0; i != image->getTotalSizeInBytes(); ++i)
            data[i] = static_cast<unsigned char>((i * 73 + i / 13 + (i >> 11)) & 255);
        std::ostringstream output(std::ios::out | std::ios::binary);
        const auto result = reader(extension)->writeImage(*image, output);
        require(result.success(), "actual image writer rejected fixture");
        return output.str();
    }
    void imageCase(const std::filesystem::path& prefix, const std::string& label,
                   const std::string& bytes, std::string_view extension)
    {
        std::istringstream oracle(bytes, std::ios::in | std::ios::binary);
        const auto expected = decodeImage(oracle, extension);
        const auto path = wrap(prefix, label, bytes);
        auto constrained = Files::openConstrainedFileStream(path, 17, bytes.size());
        const auto actual = decodeImage(*constrained, extension);
        require(actual == expected, "image metadata or complete decoded/mipmap bytes changed");
        std::cout << "IMAGE " << label << ' ' << bytes.size() << ' ' << actual << '\n';
    }
    struct ConstrainedArchive : Bsa::BSAFile
    {
        void openWithConstrainedHeader(const std::filesystem::path& path)
        {
            mFilepath = path;
            auto stream = Files::openConstrainedFileStream(path);
            readHeader(*stream); // authoritative production parser, no format mirror
            mIsLoaded = true;
        }
    };
    std::uint64_t archiveDigest(const Bsa::BSAFile& archive)
    {
        Digest d;
        d.scalar(archive.getList().size());
        for (const auto& entry : archive.getList())
        {
            d.text(entry.name()); d.scalar(entry.mFileSize); d.scalar(entry.mOffset);
            d.scalar(entry.mHash.mLow); d.scalar(entry.mHash.mHigh);
        }
        return d.value;
    }
    std::uint64_t nifDigest(Files::IStreamPtr stream, std::string_view label)
    {
        const VFS::Path::Normalized normalized(label);
        Nif::NIFFile file{ VFS::Path::NormalizedView(normalized) };
        ToUTF8::StatelessUtf8Encoder encoder(ToUTF8::WINDOWS_1252);
        Nif::Reader parser(file, &encoder);
        parser.parse(std::move(stream));
        require(!file.mRecords.empty() && !file.mRoots.empty(), "NIF fixture did not contain records/roots");
        Digest d;
        d.scalar(file.mVersion); d.scalar(file.mUserVersion); d.scalar(file.mBethVersion);
        d.text(file.mHash); d.scalar(file.mUseSkinning); d.scalar(file.mRecords.size()); d.scalar(file.mRoots.size());
        for (const auto& record : file.mRecords)
        {
            d.scalar(record->mRecordType); d.scalar(record->mRecordIndex); d.text(record->mRecordName);
            if (const auto* named = dynamic_cast<const Nif::NiObjectNET*>(record.get())) d.text(named->mName);
            if (const auto* geometry = dynamic_cast<const Nif::NiGeometryData*>(record.get()))
            {
                d.scalar(geometry->mNumVertices); d.scalar(geometry->mDataFlags);
                d.vector(geometry->mVertices); d.vector(geometry->mNormals); d.vector(geometry->mTangents);
                d.vector(geometry->mBitangents); d.vector(geometry->mColors);
                for (const auto& uv : geometry->mUVList) d.vector(uv);
            }
            if (const auto* triangles = dynamic_cast<const Nif::NiTriShapeData*>(record.get())) d.vector(triangles->mTriangles);
            if (const auto* strips = dynamic_cast<const Nif::NiTriStripsData*>(record.get()))
                for (const auto& strip : strips->mStrips) d.vector(strip);
        }
        for (const auto* root : file.mRoots) d.scalar(root ? root->mRecordIndex : ~0u);
        return d.value;
    }
    void archiveAndNifCases(const std::filesystem::path& prefix, const std::filesystem::path& installed,
                            const std::string& dds, const std::string& tga)
    {
        Bsa::BSAFile oracleArchive; oracleArchive.open(installed);
        ConstrainedArchive actualArchive; actualArchive.openWithConstrainedHeader(installed);
        require(archiveDigest(oracleArchive) == archiveDigest(actualArchive), "BSA parsed index/header changed");
        std::cout << "BSA installed " << actualArchive.getList().size() << ' ' << archiveDigest(actualArchive) << '\n';
        std::array<std::string, 3> selectedBytes;
        std::array<const Bsa::BSAFile::FileStruct*, 3> selected{};
        for (const auto& entry : actualArchive.getList())
        {
            if (!entry.name().ends_with(".nif") || !entry.mFileSize || entry.mFileSize > 2000000) continue;
            const auto index = entry.mFileSize <= 8192 ? 0u : entry.mFileSize <= 65536 ? 1u : 2u;
            if (!selected[index]) selected[index] = &entry;
        }
        for (unsigned i = 0; i != selected.size(); ++i)
        {
            require(selected[i] != nullptr, "installed BSA lacks representative NIF size fixture");
            const auto& entry = *selected[i];
            // Independent byte oracle uses std::ifstream, never the candidate
            // buffer. The production BSA getFile then hashes/parses its region.
            std::ifstream physical(installed, std::ios::binary);
            physical.seekg(entry.mOffset);
            selectedBytes[i].resize(entry.mFileSize);
            physical.read(selectedBytes[i].data(), entry.mFileSize);
            require(physical.gcount() == entry.mFileSize, "NIF oracle region truncated");
            const auto expected = nifDigest(std::make_unique<std::istringstream>(selectedBytes[i]), entry.name());
            const auto actual = nifDigest(actualArchive.getFile(&entry), entry.name());
            require(actual == expected, "NIF hash/records/roots/names/geometry changed");
            const auto path = wrap(prefix, "loose-nif-" + std::to_string(i), selectedBytes[i]);
            require(nifDigest(Files::openConstrainedFileStream(path, 17, entry.mFileSize), entry.name()) == expected,
                "bounded loose NIF parser changed");
            std::cout << "NIF " << entry.name() << ' ' << entry.mFileSize << ' ' << actual << '\n';
        }
        const auto syntheticPath = std::filesystem::path(prefix.string() + "-real-writer.bsa");
        {
            Bsa::BSAFile writer; writer.open(syntheticPath);
            std::istringstream ddsInput(dds), tgaInput(tga), nifInput(selectedBytes[2]);
            writer.addFile("textures/large.dds", ddsInput);
            writer.addFile("textures/large.tga", tgaInput);
            writer.addFile("meshes/large.nif", nifInput);
        }
        ConstrainedArchive synthetic; synthetic.openWithConstrainedHeader(syntheticPath);
        require(synthetic.getList().size() == 3, "production BSA writer/reader entry count changed");
        for (const auto& entry : synthetic.getList())
        {
            auto stream = synthetic.getFile(&entry);
            if (entry.name().ends_with(".nif"))
            {
                auto oracle = std::make_unique<std::istringstream>(selectedBytes[2]);
                require(nifDigest(std::move(stream), entry.name()) == nifDigest(std::move(oracle), entry.name()),
                    "BSA nested NIF parse changed");
            }
            else
            {
                const bool isDds = entry.name().ends_with(".dds");
                std::istringstream oracle(isDds ? dds : tga);
                require(decodeImage(*stream, isDds ? "dds" : "tga") == decodeImage(oracle, isDds ? "dds" : "tga"),
                    "BSA nested image decode changed");
            }
        }
        std::cout << "BSA writer " << synthetic.getList().size() << ' ' << archiveDigest(synthetic) << '\n';
    }
}
int main(int argc, char** argv)
{
    if (argc != 4) return 2;
    try
    {
        Log::sMinDebugLevel = Debug::Error;
        const std::filesystem::path prefix(argv[1]), source(argv[2]), installed(argv[3]);
        const auto dds = generatedImage("dds"), tga = generatedImage("tga");
        imageCase(prefix, "large-dds", dds, "dds"); imageCase(prefix, "large-tga", tga, "tga");
        for (const auto* name : { "omw_map_color_palette.dds", "omw_menu_icon_active.dds", "omw_menu_scroll_center_h.dds" })
            imageCase(prefix, name, readBytes(source / "files/data/textures" / name), "dds");
        archiveAndNifCases(prefix, installed, dds, tga);
        Debug::HitchTrace::Capture::instance().finish();
        std::cout << "PASS authoritative DDS/TGA/NIF/BSA parser data and checksums\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
