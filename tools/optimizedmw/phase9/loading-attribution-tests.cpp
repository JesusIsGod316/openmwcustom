#include <components/debug/hitchtrace.hpp>
#include <components/files/constrainedfilestream.hpp>
#include <components/files/hash.hpp>

#include <array>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace
{
    void require(bool value, const char* reason)
    {
        if (!value) throw std::runtime_error(reason);
    }

    class FailingBuffer : public std::stringbuf
    {
    public:
        FailingBuffer() : std::stringbuf(std::string(8192, 'a')) {}
    protected:
        std::streamsize xsgetn(char* out, std::streamsize count) override
        {
            if (mReads++) throw std::runtime_error("fixture read failure");
            return std::stringbuf::xsgetn(out, count);
        }
    private:
        unsigned mReads = 0;
    };

    void hashFixtures(const std::filesystem::path& prefix)
    {
        struct Case { std::size_t size; std::array<std::uint64_t, 2> hash; };
        constexpr Case cases[] = {
            {0, {0, 0}}, {1, {9607679276477937801ull, 16624257681780017498ull}},
            {128, {15287858148353394424ull, 16818615825966581310ull}},
            {1000, {11018119256083894017ull, 6631144854802791578ull}},
            {4096, {11972283295181039100ull, 16027670129106775155ull}},
            {4097, {16717956291025443060ull, 12856404199748778153ull}},
            {5000, {15775925571142117787ull, 10322955217889622896ull}}
        };
        for (const auto& c : cases)
        {
            // A nonzero starting position must survive hashing unchanged.
            std::istringstream stream(std::string("prefix") + std::string(c.size, 'a'));
            stream.seekg(6);
            stream.exceptions(std::ios::failbit | std::ios::badbit);
            require(Files::getHash("logical/hash-" + std::to_string(c.size) + ".nif", stream) == c.hash,
                "block-dependent hash changed");
            require(stream.tellg() == 6, "hash did not restore stream position");
            require(stream.exceptions() == (std::ios::failbit | std::ios::badbit), "exception mask changed");
        }
        const auto physical = std::filesystem::path(prefix.string() + "-physical,asset.nif");
        {
            std::ofstream file(physical, std::ios::binary);
            file << std::string(17, 'p') << std::string(4097, 'a') << "tail";
        }
        {
            auto stream = Files::openConstrainedFileStream(physical, 17, 4097);
            require(Files::getHash("meshes/logical-archive-entry.nif", *stream) == cases[5].hash,
                "constrained stream hash changed");
            require(stream->tellg() == 0, "constrained hash did not rewind");
            std::string parsed(4097, '\0');
            stream->read(parsed.data(), static_cast<std::streamsize>(parsed.size()));
            require(parsed == std::string(4097, 'a'), "constrained region changed after hashing");
            require(stream->peek() == std::char_traits<char>::eof(), "constrained read escaped region");
        }
        FailingBuffer buffer;
        std::istream failure(&buffer);
        try
        {
            Files::getHash("logical/read-failure.nif", failure);
            throw std::runtime_error("failed hash unexpectedly completed");
        }
        catch (const std::runtime_error& error)
        {
            require(std::string(error.what()).find("logical/read-failure.nif") != std::string::npos,
                "hash exception lost filename");
        }
        try
        {
            auto missing = Files::openConstrainedFileStream(prefix.string() + "-missing.nif");
            throw std::runtime_error("missing file unexpectedly opened");
        }
        catch (const std::runtime_error& error)
        {
            require(std::string(error.what()).find("-missing.nif") != std::string::npos,
                "file-open failure changed");
        }
    }

    void crossThreadDependency()
    {
        using namespace Debug::HitchTrace;
        Scope request("fixture.terrain_request");
        request.retain();
        Scope create("fixture.terrain_create");
        create.retain();
        create.setLink(request.id());
        const auto job = create.id();
        std::mutex mutex;
        std::condition_variable ready;
        bool done = false;
        Scope enqueue("fixture.terrain_enqueue");
        enqueue.retain();
        enqueue.setLink(job);
        std::thread worker([&] {
            Scope work("fixture.terrain_work", CpuTiming::Scoped);
            work.retain();
            work.setLink(job);
            { Scope prepare("fixture.terrain_prepare"); prepare.retain(); prepare.setLink(job); }
            Scope publish("fixture.terrain_publish");
            publish.retain();
            publish.setLink(job);
            {
                const std::lock_guard lock(mutex);
                done = true;
            }
            ready.notify_one();
        });
        {
            Scope callerWait("fixture.terrain_wait", CpuTiming::Scoped);
            callerWait.retain();
            callerWait.setLink(job);
            std::unique_lock lock(mutex);
            ready.wait(lock, [&] { return done; });
        }
        worker.join();
    }
}

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    const std::filesystem::path prefix(argv[1]);
    const bool armed = std::string_view(argv[2]) == "armed";
    const std::string output = prefix.string() + ".csv";
#ifdef _WIN32
    _putenv_s("OPENMW_P9_HITCH_SPANS_FILE", armed ? output.c_str() : "0");
#else
    setenv("OPENMW_P9_HITCH_SPANS_FILE", armed ? output.c_str() : "0", 1);
#endif
    try
    {
        hashFixtures(prefix);
        crossThreadDependency();
        Debug::HitchTrace::Capture::instance().finish();
        std::cout << "PASS original hash semantics, constrained regions, failures and dependency fixture\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
