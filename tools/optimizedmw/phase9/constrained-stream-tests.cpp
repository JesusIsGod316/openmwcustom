#include <components/bsa/bsafile.hpp>
#include <components/debug/hitchtrace.hpp>
#include <components/files/constrainedfilestream.hpp>
#include <components/files/constrainedreadpolicy.hpp>
#include <components/files/hash.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    namespace Policy = Files::ConstrainedReadPolicy;
    using Stream = std::istream;
    void require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    std::string data(std::size_t size)
    {
        std::mt19937 generator(0x91e02u);
        std::string result(size, '\0');
        for (char& value : result) value = static_cast<char>(generator() & 255);
        return result;
    }
    std::filesystem::path fixture(const std::filesystem::path& prefix, const std::string& name, const std::string& bytes)
    {
        auto path = std::filesystem::path(prefix.string() + "-" + name + ".bin");
        std::ofstream output(path, std::ios::binary);
        const std::string before(17, 'P'), after(41, 'Q');
        output.write(before.data(), before.size());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.write(after.data(), after.size());
        require(static_cast<bool>(output), "could not write fixture");
        return path;
    }
    void compare(std::istream& actual, std::istream& expected)
    {
        require(actual.rdstate() == expected.rdstate(), "differential stream flags changed");
        if (!actual.fail()) require(actual.tellg() == expected.tellg(), "differential logical position changed");
    }
    void semantics(const std::filesystem::path& prefix)
    {
        for (const auto length : { 0u, 1u, 8191u, 8192u, 8193u, 65535u, 65536u, 65537u, 1048583u })
        {
            const auto bytes = data(length);
            const auto path = fixture(prefix, "random-" + std::to_string(length), bytes);
            auto actual = Files::openConstrainedFileStream(path, 17, length);
            std::istringstream expected(bytes);
            std::mt19937 generator(0x4107u + length);
            for (unsigned step = 0; step != 1200; ++step)
            {
                switch (generator() % 6)
                {
                    case 0:
                        require(actual->peek() == expected.peek(), "peek changed bytes");
                        break;
                    case 1:
                        require(actual->get() == expected.get(), "get changed bytes");
                        break;
                    case 2:
                    {
                        const auto count = generator() % 70000;
                        std::string a(count, 'a'), b(count, 'b');
                        actual->read(a.data(), count); expected.read(b.data(), count);
                        require(actual->gcount() == expected.gcount(), "read count changed");
                        require(a.substr(0, actual->gcount()) == b.substr(0, expected.gcount()), "read bytes changed");
                        break;
                    }
                    case 3:
                    {
                        actual->clear(); expected.clear();
                        const auto position = length ? generator() % (length + 1) : 0;
                        actual->seekg(position); expected.seekg(position);
                        break;
                    }
                    case 4:
                    {
                        actual->clear(); expected.clear();
                        const auto magnitude = length ? generator() % (length + 1) : 0;
                        actual->seekg(-static_cast<std::streamoff>(magnitude), std::ios::end);
                        expected.seekg(-static_cast<std::streamoff>(magnitude), std::ios::end);
                        break;
                    }
                    case 5:
                        if (!actual->fail()) require(actual->tellg() == expected.tellg(), "tell changed");
                        break;
                }
                compare(*actual, expected);
            }
            actual->clear(); actual->seekg(0);
            std::string all(length, '\0'); actual->read(all.data(), length);
            require(all == bytes, "whole region changed");
            require(actual->peek() == std::char_traits<char>::eof(), "read escaped constrained region");
            actual->clear(); actual->seekg(0);
            require(actual->get() == (length ? std::char_traits<char>::to_int_type(bytes[0]) : std::char_traits<char>::eof()),
                "EOF clear/rewind changed");
        }
    }
    void seekAndPutback(const std::filesystem::path& prefix)
    {
        const auto bytes = data(200003);
        const auto path = fixture(prefix, "seeks", bytes);
        auto stream = Files::openConstrainedFileStream(path, 17, bytes.size());
        auto* buffer = stream->rdbuf();
        require(buffer->pubseekoff(0, std::ios::cur, std::ios::in) == 0, "tell on empty get area failed");
        require(buffer->sgetn(nullptr, 0) == 0, "zero read changed");
        require(buffer->sgetc() == std::char_traits<char>::to_int_type(bytes[0]), "initial peek changed");
        const auto bad = std::streampos(std::streamoff(-1));
        for (auto offset : { std::streamoff(-1), std::numeric_limits<std::streamoff>::min(),
                             std::numeric_limits<std::streamoff>::max() })
            require(buffer->pubseekoff(offset, std::ios::beg, std::ios::in) == bad, "invalid begin seek accepted");
        require(buffer->pubseekoff(std::numeric_limits<std::streamoff>::max(), std::ios::end, std::ios::in) == bad,
            "overflow seek accepted");
        require(buffer->pubseekpos(bad, std::ios::in) == bad, "negative seekpos accepted");
        require(buffer->pubseekoff(0, std::ios::beg, std::ios::out) == bad, "output seek accepted");
        require(buffer->sgetc() == std::char_traits<char>::to_int_type(bytes[0]), "invalid seek mutated window");
        for (unsigned i = 0; i != 64; ++i)
            require(buffer->sbumpc() == std::char_traits<char>::to_int_type(bytes[i]), "putback setup changed");
        require(buffer->sputbackc(bytes[63]) != std::char_traits<char>::eof(), "matching putback failed");
        require(buffer->sputbackc(static_cast<char>(bytes[62] ^ 1)) == std::char_traits<char>::eof(),
            "mismatched putback unexpectedly wrote read-only data");
        require(buffer->sbumpc() == std::char_traits<char>::to_int_type(bytes[63]), "putback changed data");
        for (unsigned i = 0; i != 64; ++i) require(buffer->sungetc() != std::char_traits<char>::eof(), "bounded unget failed");
        require(buffer->sungetc() == std::char_traits<char>::eof(), "putback escaped region start");
        require(stream->tellg() == 0, "putback tell wrong");
        stream->seekg(1234);
        require(stream->peek() == std::char_traits<char>::to_int_type(bytes[1234]), "resident seek wrong");
        stream->seekg(65535);
        require(stream->get() == std::char_traits<char>::to_int_type(bytes[65535]), "window edge seek wrong");
        stream->seekg(bytes.size());
        require(stream->peek() == std::char_traits<char>::eof(), "seek end escaped region");
        stream->clear(); stream->seekg(0);
        require(stream->get() == std::char_traits<char>::to_int_type(bytes[0]), "EOF seek retry wrong");
        auto second = Files::openConstrainedFileStream(path, 17, bytes.size());
        second->seekg(112233);
        require(second->get() == std::char_traits<char>::to_int_type(bytes[112233]), "second handle changed");
        require(stream->get() == std::char_traits<char>::to_int_type(bytes[1]), "independent handle position changed");
    }
    void hashAndSequential(const std::filesystem::path& prefix)
    {
        for (const auto length : { 4095u, 4096u, 4097u, 8191u, 8192u, 8193u, 65535u, 65536u, 65537u, 1048579u })
        {
            const auto bytes = data(length);
            const auto path = fixture(prefix, "hash-" + std::to_string(length), bytes);
            std::istringstream oracle(bytes);
            oracle.seekg(3);
            oracle.exceptions(std::ios::failbit | std::ios::badbit);
            const auto expected = Files::getHash("oracle", oracle);
            auto stream = Files::openConstrainedFileStream(path, 17, length);
            stream->seekg(3);
            stream->exceptions(std::ios::failbit | std::ios::badbit);
            require(Files::getHash("constrained", *stream) == expected, "4096-block hash changed");
            require(stream->tellg() == 3, "hash rewind changed");
            require(stream->exceptions() == oracle.exceptions(), "hash exception mask changed");
            char next; stream->get(next); require(next == bytes[3], "parse after hash changed");
        }
        const auto bytes = data(1048579);
        const auto path = fixture(prefix, "sequential", bytes);
        auto stream = Files::openConstrainedFileStream(path, 17, bytes.size());
        std::string actual(bytes.size(), '\0');
        stream->read(actual.data(), static_cast<std::streamsize>(actual.size()));
        require(actual == bytes, "sequential data changed");
        require(stream->peek() == std::char_traits<char>::eof(), "sequential EOF changed");
        // Warm window hash/read repeats exercise independent I2 reuse.
        const auto small = fixture(prefix, "reuse", bytes.substr(0, 4097));
        auto reusable = Files::openConstrainedFileStream(small, 17, 4097);
        char piece[128]; reusable->read(piece, sizeof(piece));
        require(reusable->tellg() == 128, "resident tell changed");
        reusable->seekg(0); reusable->read(piece, sizeof(piece));
        require(std::string(piece, sizeof(piece)) == bytes.substr(0, sizeof(piece)), "resident repeat changed bytes");
    }
    void failures(const std::filesystem::path& prefix)
    {
        const auto bytes = data(200003);
        const auto path = fixture(prefix, "failures", bytes);
        {
            auto stream = Files::openConstrainedFileStream(path, 17, bytes.size());
            Policy::TestHooks::allocationFailures = 1;
            require(stream->get() == std::char_traits<char>::to_int_type(bytes[0]), "state bad_alloc did not fall back");
            Policy::TestHooks::allocationFailures = 0;
        }
        {
            auto stream = Files::openConstrainedFileStream(path, 17, bytes.size());
            require(stream->tellg() == 0, "buffer-failure preparation failed");
            Policy::TestHooks::allocationFailures = 1;
            require(stream->get() == std::char_traits<char>::to_int_type(bytes[0]), "buffer bad_alloc did not fall back");
            Policy::TestHooks::allocationFailures = 0;
        }
        {
            auto stream = Files::openConstrainedFileStream(path, 17, bytes.size());
            stream->exceptions(std::ios::badbit);
            Policy::TestHooks::readFailures = 1;
            bool failed = false;
            try { stream->get(); } catch (const std::exception&) { failed = true; }
            require(failed && stream->bad(), "read failure did not propagate");
            stream->clear();
            require(stream->get() == std::char_traits<char>::to_int_type(bytes[0]), "read failure retry changed position");
            Policy::TestHooks::seekFailures = 1;
            try { stream->seekg(150000); } catch (const std::exception&) { failed = true; }
            require(stream->bad(), "native seek failure did not propagate");
            stream->clear();
            require(stream->get() == std::char_traits<char>::to_int_type(bytes[1]), "seek failure lost valid window");
        }
        {
            auto stream = Files::openConstrainedFileStream(path, 17, bytes.size());
            Policy::TestHooks::nextReadLimit = 13;
            std::string actual(200, '\0'); stream->read(actual.data(), actual.size());
            require(actual == bytes.substr(0, 200), "short native read skipped/repeated bytes");
        }
        {
            auto stream = Files::openConstrainedFileStream(path, 17, bytes.size());
            Policy::TestHooks::zeroReads = 1;
            require(stream->get() == std::char_traits<char>::eof(), "zero read did not return EOF");
            stream->clear();
            require(stream->get() == std::char_traits<char>::to_int_type(bytes[0]), "zero read retry skipped bytes");
        }
        {
            auto stream = Files::openConstrainedFileStream(path, 17, bytes.size());
            stream->exceptions(std::ios::badbit);
            Policy::TestHooks::nextReadLimit = 13;
            Policy::TestHooks::readFailuresAfterAdvance = 1;
            bool failed = false;
            try { stream->get(); } catch (const std::exception&) { failed = true; }
            require(failed && stream->bad(), "advanced read failure did not propagate");
            stream->clear();
            require(stream->get() == std::char_traits<char>::to_int_type(bytes[13]), "failed read lost actual handle position");
            Policy::TestHooks::seekFailuresAfterAdvance = 1;
            failed = false;
            try { stream->seekg(150000); } catch (const std::exception&) { failed = true; }
            require(failed && stream->bad(), "advanced seek failure did not propagate");
            stream->clear();
            // CONTROL historically retains its old get area on failed seek.
            // I2 reconciles changed native position and discards stale bytes.
            const bool resident = Policy::Options::startup().residentSeek
                && Policy::Budget::instance().snapshot().states != 0;
            require(stream->get() == std::char_traits<char>::to_int_type(bytes[resident ? 150000 : 14]),
                "seek failure changed retained/reconciled window semantics");
        }
        if (Policy::Options::startup().residentSeek && Policy::Options::startup().budgetBytes >= 400)
        {
            auto stream = Files::openConstrainedFileStream(path, 17, bytes.size());
            stream->exceptions(std::ios::badbit);
            Policy::TestHooks::nextReadLimit = 13;
            Policy::TestHooks::readFailuresAfterAdvance = 1;
            Policy::TestHooks::tellFailures = 1;
            try { stream->get(); } catch (const std::exception&) {}
            require(stream->bad(), "read/tell compound failure did not propagate original error");
            stream->clear();
            require(stream->get() == std::char_traits<char>::to_int_type(bytes[13]), "unknown position retry used stale tracked position");
        }
        auto rejected = [&](std::size_t origin, std::size_t length) {
            bool failed = false;
            try { auto stream = Files::openConstrainedFileStream(path, origin, length); }
            catch (const std::runtime_error&) { failed = true; }
            require(failed, "unsupported region was accepted");
        };
        rejected(0, std::numeric_limits<std::size_t>::max() - 1);
#ifdef _WIN32
        rejected(std::numeric_limits<std::uint32_t>::max(), 1);
#else
        rejected(static_cast<std::size_t>(std::numeric_limits<std::streamoff>::max()), 1);
        const auto largePath = std::filesystem::path(prefix.string() + "-large-sparse.bin");
        constexpr std::size_t offset = (std::size_t{ 1 } << 32) + 19;
        {
            std::ofstream sparse(largePath, std::ios::binary);
            sparse.seekp(offset); sparse.put('Z');
            require(bool(sparse), "large sparse fixture could not be written");
        }
        {
            auto large = Files::openConstrainedFileStream(largePath, offset, 1);
            require(large->get() == 'Z' && large->get() == std::char_traits<char>::eof(), "POSIX region truncated to DWORD");
            large->clear(); large->seekg(0);
            require(large->get() == 'Z', "POSIX large region rewind changed data");
        }
#endif
        rejected(bytes.size() + 59, std::numeric_limits<std::size_t>::max());
        if (Policy::Options::startup().residentSeek || Policy::Options::startup().refillBytes == Policy::CandidateBytes)
            rejected(17, bytes.size() + 42);
        require(Policy::Budget::instance().snapshot().used == 0, "failure/bounds path leaked budget");
    }
    void boundedOwnership(const std::filesystem::path& prefix)
    {
        const auto bytes = data(131073);
        const auto path = fixture(prefix, "budget", bytes);
        std::vector<std::unique_ptr<Stream>> held;
        for (unsigned i = 0; i != 600; ++i)
        {
            auto stream = Files::openConstrainedFileStream(path, 17, bytes.size());
            require(stream->peek() == std::char_traits<char>::to_int_type(bytes[0]), "cap fallback changed bytes");
            held.push_back(std::move(stream));
            const auto snapshot = Policy::Budget::instance().snapshot();
            require(snapshot.used <= snapshot.limit, "aggregate reservation exceeded limit");
        }
        auto moved = std::move(held.back()); held.pop_back();
        require(moved->get() == std::char_traits<char>::to_int_type(bytes[0]), "moving owner lost buffered data");
        held.clear(); moved.reset();
        require(Policy::Budget::instance().snapshot().used == 0, "close/move leaked budget");
        std::barrier barrier(9);
        std::atomic<bool> okay{ true };
        std::vector<std::thread> threads;
        for (unsigned i = 0; i != 8; ++i)
            threads.emplace_back([&] {
                std::vector<std::unique_ptr<Stream>> local;
                try
                {
                    for (unsigned j = 0; j != 75; ++j)
                    {
                        auto stream = Files::openConstrainedFileStream(path, 17, bytes.size());
                        if (stream->peek() != std::char_traits<char>::to_int_type(bytes[0])) okay = false;
                        local.push_back(std::move(stream));
                    }
                }
                catch (...) { okay = false; }
                barrier.arrive_and_wait(); barrier.arrive_and_wait();
            });
        barrier.arrive_and_wait();
        require(Policy::Budget::instance().snapshot().used <= Policy::Options::startup().budgetBytes,
            "concurrent streams exceeded budget");
        barrier.arrive_and_wait();
        for (auto& thread : threads) thread.join();
        const auto snapshot = Policy::Budget::instance().snapshot();
        require(okay && snapshot.used == 0 && snapshot.states == 0 && snapshot.buffers == 0,
            "concurrent close leaked reservation or changed reads");
        std::cout << "budget_peak=" << snapshot.peak << " budget_limit=" << snapshot.limit << '\n';
    }
    void portableArchive(const std::filesystem::path& prefix)
    {
        const auto path = std::filesystem::path(prefix.string() + "-portable-writer.bsa");
        // Only this exact fixture output is replaced, never an input archive.
        std::filesystem::remove(path);
        const std::array<std::string, 3> payloads{ data(13), data(65537), data(1048579) };
        const std::array<std::string, 3> names{ "small.bin", "boundary.bin", "large.bin" };
        {
            Bsa::BSAFile writer; writer.open(path);
            for (unsigned i = 0; i != names.size(); ++i)
            {
                std::istringstream input(payloads[i]);
                writer.addFile(names[i], input);
            }
        }
        struct Archive : Bsa::BSAFile
        {
            void openConstrained(const std::filesystem::path& file)
            {
                mFilepath = file;
                auto stream = Files::openConstrainedFileStream(file);
                readHeader(*stream);
                mIsLoaded = true;
            }
        } actual;
        actual.openConstrained(path);
        Bsa::BSAFile expected; expected.open(path);
        require(actual.getList().size() == 3 && actual.getList().size() == expected.getList().size(),
            "portable production BSA index count changed");
        for (unsigned i = 0; i != 3; ++i)
        {
            const auto& a = actual.getList()[i];
            const auto& b = expected.getList()[i];
            require(a.name() == b.name() && a.mOffset == b.mOffset && a.mFileSize == b.mFileSize
                && a.mHash.mLow == b.mHash.mLow && a.mHash.mHigh == b.mHash.mHigh,
                "portable production BSA header/offset/hash changed");
            const auto index = static_cast<unsigned>(std::find(names.begin(), names.end(), a.name()) - names.begin());
            require(index < names.size(), "portable BSA entry name changed");
            auto stream = actual.getFile(&a);
            std::string value(a.mFileSize, '\0'); stream->read(value.data(), a.mFileSize);
            require(value == payloads[index], "portable BSA constrained payload changed");
            require(stream->peek() == std::char_traits<char>::eof(), "portable BSA region escaped into another entry");
            stream->clear(); stream->seekg(0);
            require(stream->get() == std::char_traits<char>::to_int_type(payloads[index][0]), "portable BSA rewind failed");
        }
    }
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    try
    {
        const std::filesystem::path prefix(argv[1]);
        semantics(prefix); seekAndPutback(prefix); hashAndSequential(prefix); failures(prefix);
        portableArchive(prefix); boundedOwnership(prefix);
        Debug::HitchTrace::Capture::instance().finish();
        std::cout << "PASS production constrained stream differential/random/regions/EOF/hash/failures/cap/concurrency\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
