import os
import re
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.16 match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.16 patched {rel} ({count} match(es))")


def write_new(rel, content):
    path = ROOT / rel
    if path.exists():
        raise RuntimeError(f"{rel}: V3.16 expected file to be absent before backport")
    path.write_text(content, encoding="utf-8", newline="\n")
    print(f"V3.16 added {rel}")


# -----------------------------------------------------------------------------
# 1) Official OpenMW sound-head cache backport (upstream 9ec49cfb4709cbfd...)
# -----------------------------------------------------------------------------
# Keep the feature default-off so Mode86 remains an exact V3.15 Mode84 control.
# V3.16 modes explicitly opt into larger RAM-backed cache budgets.

HEAD_CACHE_HPP = r'''#ifndef GAME_SOUND_HEADCACHE_H
#define GAME_SOUND_HEADCACHE_H

#include <cstddef>
#include <functional>
#include <ios>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <components/files/istreamptr.hpp>
#include <components/vfs/pathutil.hpp>

namespace VFS
{
    class Manager;
}

namespace MWSound
{
    struct HeadBuffer
    {
        VFS::Path::Normalized mName;
        std::vector<char> mHead;
        std::vector<char> mSuffix;
        std::streamoff mSuffixStart;
        std::streamoff mFileSize;

        HeadBuffer(VFS::Path::Normalized&& name, std::vector<char>&& head, std::vector<char>&& suffix,
            std::streamoff suffixStart, std::streamoff fileSize)
            : mName(std::move(name))
            , mHead(std::move(head))
            , mSuffix(std::move(suffix))
            , mSuffixStart(suffixStart)
            , mFileSize(fileSize)
        {
        }
    };

    Files::IStreamPtr makeHeadStream(std::shared_ptr<const HeadBuffer>&& buffer, const VFS::Manager& vfs);
    Files::IStreamPtr makeRecordingStream(Files::IStreamPtr&& impl);

    class HeadCache
    {
    public:
        explicit HeadCache(const VFS::Manager& vfs, std::size_t maxBytes);

        std::shared_ptr<const HeadBuffer> lookup(VFS::Path::NormalizedView name);
        void insert(VFS::Path::NormalizedView name, const std::istream& stream);

    private:
        using LruIt = std::list<std::shared_ptr<const HeadBuffer>>::iterator;

        void insert(VFS::Path::NormalizedView name, std::vector<char>&& head, std::vector<char>&& suffix,
            std::streamoff suffixStart, std::streamoff fileSize);

        const VFS::Manager& mVfs;
        const std::size_t mMaxBytes;
        std::mutex mMutex;
        std::list<std::shared_ptr<const HeadBuffer>> mLru;
        std::unordered_map<VFS::Path::Normalized, LruIt, VFS::Path::Hash, std::equal_to<>> mEntries;
        std::size_t mBytes = 0;
    };
}

#endif
'''

HEAD_CACHE_CPP = r'''#include "headcache.hpp"

#include <algorithm>
#include <cstring>
#include <istream>
#include <limits>
#include <stdexcept>

#include <components/files/streamwithbuffer.hpp>
#include <components/vfs/manager.hpp>

namespace MWSound
{
    namespace
    {
        constexpr std::streamoff sMaxHeadBytes = 256 * 1024;

        class RecordingBuf final : public std::streambuf
        {
        public:
            explicit RecordingBuf(Files::IStreamPtr inner)
                : mInner(std::move(inner))
            {
            }

            std::streamoff prefixEnd() const { return mPrefixEnd; }
            std::streamoff suffixStart() const { return mSuffixStart; }

        protected:
            std::streamsize xsgetn(char* s, std::streamsize n) override
            {
                mInner->clear();
                mInner->read(s, n);
                const std::streamsize got = mInner->gcount();
                const std::streamoff begin = mPos;
                mPos += got;
                if (mFileSize > 0 && begin >= mFileSize / 2)
                    mSuffixStart = std::min(mSuffixStart, begin);
                else
                    mPrefixEnd = std::max(mPrefixEnd, mPos);
                return got;
            }

            std::streampos seekoff(std::streamoff off, std::ios_base::seekdir dir, std::ios_base::openmode) override
            {
                mInner->clear();
                mInner->seekg(off, dir);
                if (!*mInner)
                    return std::streampos(std::streamoff(-1));
                mPos = mInner->tellg();
                if (dir == std::ios_base::end)
                    mFileSize = std::max(mFileSize, mPos - off);
                setg(nullptr, nullptr, nullptr);
                return mPos;
            }

            std::streampos seekpos(std::streampos pos, std::ios_base::openmode mode) override
            {
                return seekoff(static_cast<std::streamoff>(pos), std::ios_base::beg, mode);
            }

        private:
            Files::IStreamPtr mInner;
            std::streamoff mPos = 0;
            std::streamoff mPrefixEnd = 0;
            std::streamoff mSuffixStart = std::numeric_limits<std::streamoff>::max();
            std::streamoff mFileSize = 0;
        };

        class HeadBuf final : public std::streambuf
        {
        public:
            HeadBuf(std::shared_ptr<const HeadBuffer>&& buffer, const VFS::Manager& vfs)
                : mBuffer(std::move(buffer))
                , mVfs(vfs)
            {
            }

        protected:
            std::streamsize xsgetn(char* s, std::streamsize n) override
            {
                std::streamsize done = 0;
                while (done < n && mPos < mBuffer->mFileSize)
                {
                    const std::streamoff headSize = static_cast<std::streamoff>(mBuffer->mHead.size());
                    if (mPos < headSize)
                    {
                        const std::streamsize take = std::min<std::streamsize>(n - done, headSize - mPos);
                        std::memcpy(s + done, mBuffer->mHead.data() + mPos, take);
                        done += take;
                        mPos += take;
                        continue;
                    }
                    if (!mBuffer->mSuffix.empty() && mPos >= mBuffer->mSuffixStart)
                    {
                        const std::streamoff inSuffix = mPos - mBuffer->mSuffixStart;
                        const std::streamsize take = std::min<std::streamsize>(
                            n - done, static_cast<std::streamoff>(mBuffer->mSuffix.size()) - inSuffix);
                        if (take <= 0)
                            break;
                        std::memcpy(s + done, mBuffer->mSuffix.data() + inSuffix, take);
                        done += take;
                        mPos += take;
                        continue;
                    }
                    if (mInner == nullptr)
                        mInner = mVfs.get(mBuffer->mName);
                    mInner->clear();
                    mInner->seekg(mPos);
                    const std::streamsize want = !mBuffer->mSuffix.empty() && mPos < mBuffer->mSuffixStart
                        ? std::min<std::streamsize>(n - done, mBuffer->mSuffixStart - mPos)
                        : n - done;
                    mInner->read(s + done, want);
                    const std::streamsize got = mInner->gcount();
                    if (got <= 0)
                        break;
                    done += got;
                    mPos += got;
                }
                return done;
            }

            std::streampos seekoff(std::streamoff off, std::ios_base::seekdir dir, std::ios_base::openmode) override
            {
                std::streamoff base = 0;
                if (dir == std::ios_base::cur)
                    base = mPos;
                else if (dir == std::ios_base::end)
                    base = mBuffer->mFileSize;
                const std::streamoff target = base + off;
                if (target < 0 || target > mBuffer->mFileSize)
                    return std::streampos(std::streamoff(-1));
                mPos = target;
                setg(nullptr, nullptr, nullptr);
                return mPos;
            }

            std::streampos seekpos(std::streampos pos, std::ios_base::openmode mode) override
            {
                return seekoff(static_cast<std::streamoff>(pos), std::ios_base::beg, mode);
            }

        private:
            std::shared_ptr<const HeadBuffer> mBuffer;
            const VFS::Manager& mVfs;
            Files::IStreamPtr mInner;
            std::streamoff mPos = 0;
        };
    }

    Files::IStreamPtr makeHeadStream(std::shared_ptr<const HeadBuffer>&& buffer, const VFS::Manager& vfs)
    {
        return std::make_unique<Files::StreamWithBuffer<HeadBuf>>(std::make_unique<HeadBuf>(std::move(buffer), vfs));
    }

    Files::IStreamPtr makeRecordingStream(Files::IStreamPtr&& impl)
    {
        return std::make_unique<Files::StreamWithBuffer<RecordingBuf>>(std::make_unique<RecordingBuf>(std::move(impl)));
    }

    HeadCache::HeadCache(const VFS::Manager& vfs, std::size_t maxBytes)
        : mVfs(vfs)
        , mMaxBytes(maxBytes)
    {
    }

    std::shared_ptr<const HeadBuffer> HeadCache::lookup(VFS::Path::NormalizedView name)
    {
        const std::lock_guard lock(mMutex);
        const auto it = mEntries.find(name);
        if (it == mEntries.end())
            return nullptr;
        mLru.splice(mLru.begin(), mLru, it->second);
        return *it->second;
    }

    void HeadCache::insert(VFS::Path::NormalizedView name, const std::istream& stream)
    {
        const auto* const recording = dynamic_cast<const RecordingBuf*>(stream.rdbuf());
        if (recording == nullptr)
            throw std::invalid_argument("HeadCache::insert: stream is not from makeRecordingStream");
        const std::streamoff prefixEnd = recording->prefixEnd();
        if (prefixEnd <= 0 || prefixEnd > sMaxHeadBytes)
            return;

        Files::IStreamPtr fresh = mVfs.get(name);
        fresh->seekg(0, std::ios_base::end);
        const std::streamoff fileSize = fresh->tellg();
        if (fileSize <= 0)
            return;
        std::vector<char> head(static_cast<std::size_t>(std::min(prefixEnd, fileSize)));
        fresh->seekg(0);
        fresh->read(head.data(), head.size());
        if (fresh->gcount() != static_cast<std::streamsize>(head.size()))
            return;

        std::vector<char> suffix;
        std::streamoff suffixStart = fileSize;
        if (recording->suffixStart() < fileSize)
        {
            suffixStart = recording->suffixStart();
            if (fileSize - suffixStart > sMaxHeadBytes)
                return;
            suffix.resize(static_cast<std::size_t>(fileSize - suffixStart));
            fresh->clear();
            fresh->seekg(suffixStart);
            fresh->read(suffix.data(), suffix.size());
            if (fresh->gcount() != static_cast<std::streamsize>(suffix.size()))
                return;
        }

        insert(name, std::move(head), std::move(suffix), suffixStart, fileSize);
    }

    void HeadCache::insert(VFS::Path::NormalizedView name, std::vector<char>&& head, std::vector<char>&& suffix,
        std::streamoff suffixStart, std::streamoff fileSize)
    {
        const std::size_t bytes = head.size() + suffix.size();
        if (bytes > mMaxBytes)
            return;
        const std::lock_guard lock(mMutex);
        if (mEntries.contains(name))
            return;
        while (!mLru.empty() && mBytes + bytes > mMaxBytes)
        {
            const HeadBuffer& evicted = *mLru.back();
            mBytes -= evicted.mHead.size() + evicted.mSuffix.size();
            mEntries.erase(evicted.mName);
            mLru.pop_back();
        }
        const LruIt lruIt = mLru.insert(mLru.begin(),
            std::make_shared<const HeadBuffer>(
                VFS::Path::Normalized(name), std::move(head), std::move(suffix), suffixStart, fileSize));
        mEntries.emplace((*lruIt)->mName, lruIt);
        mBytes += bytes;
    }
}
'''

write_new("apps/openmw/mwsound/headcache.hpp", HEAD_CACHE_HPP)
write_new("apps/openmw/mwsound/headcache.cpp", HEAD_CACHE_CPP)

replace_exact(
    "apps/openmw/CMakeLists.txt",
    "    soundmanagerimp openaloutput ffmpegdecoder sound soundbuffer sounddecoder soundoutput\n",
    "    soundmanagerimp openaloutput ffmpegdecoder headcache sound soundbuffer sounddecoder soundoutput\n",
)

replace_exact(
    "components/settings/sanitizerimpl.hpp",
    "    std::unique_ptr<Sanitizer<int>> makeClampSanitizerInt(int min, int max);\n",
    "    std::unique_ptr<Sanitizer<int>> makeClampSanitizerInt(int min, int max);\n\n"
    "    std::unique_ptr<Sanitizer<std::size_t>> makeClampSanitizerSize(std::size_t min, std::size_t max);\n",
)

replace_exact(
    "components/settings/sanitizerimpl.cpp",
    '''    std::unique_ptr<Sanitizer<int>> makeClampSanitizerInt(int min, int max)
    {
        return std::make_unique<Clamp<int>>(min, max);
    }

    std::unique_ptr<Sanitizer<float>> makeClampStrictMaxSanitizerFloat(float min, float max)''',
    '''    std::unique_ptr<Sanitizer<int>> makeClampSanitizerInt(int min, int max)
    {
        return std::make_unique<Clamp<int>>(min, max);
    }

    std::unique_ptr<Sanitizer<std::size_t>> makeClampSanitizerSize(std::size_t min, std::size_t max)
    {
        return std::make_unique<Clamp<std::size_t>>(min, max);
    }

    std::unique_ptr<Sanitizer<float>> makeClampStrictMaxSanitizerFloat(float min, float max)''',
)

replace_exact(
    "components/settings/categories/sound.hpp",
    '''        SettingValue<int> mBufferCacheMin{ mIndex, "Sound", "buffer cache min", makeMaxSanitizerInt(1) };
        SettingValue<int> mBufferCacheMax{ mIndex, "Sound", "buffer cache max", makeMaxSanitizerInt(1) };''',
    '''        SettingValue<int> mBufferCacheMin{ mIndex, "Sound", "buffer cache min", makeMaxSanitizerInt(1) };
        SettingValue<int> mBufferCacheMax{ mIndex, "Sound", "buffer cache max", makeMaxSanitizerInt(1) };
        SettingValue<std::size_t> mHeadCacheSize{ mIndex, "Sound", "head cache size", makeClampSanitizerSize(0, 4095) };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''# Maximum size to use for the sound buffer cache, in MB. The cache can use up
# to this much memory until old buffers get purged.
buffer cache max = 64

# Specifies whether to enable HRTF processing.''',
    '''# Maximum size to use for the sound buffer cache, in MB. The cache can use up
# to this much memory until old buffers get purged.
buffer cache max = 64

# V3.16: size of the streamed sound head cache, in MB. Keeps the beginning/end
# ranges FFmpeg needs to initialize recently started music/voice streams in RAM.
# 0 disables the cache so the V3.15 control remains exact.
head cache size = 0

# Specifies whether to enable HRTF processing.''',
)

replace_exact(
    "apps/openmw/mwsound/ffmpegdecoder.hpp",
    '''namespace MWSound
{
    struct AVIOContextDeleter''',
    '''namespace MWSound
{
    class HeadCache;

    struct AVIOContextDeleter''',
)

replace_exact(
    "apps/openmw/mwsound/ffmpegdecoder.hpp",
    '''    public:
        explicit FFmpegDecoder(const VFS::Manager* vfs);

        virtual ~FFmpegDecoder();

        friend class SoundManager;
    };''',
    '''    public:
        explicit FFmpegDecoder(const VFS::Manager* vfs, HeadCache* headCache);

        virtual ~FFmpegDecoder();

        friend class SoundManager;

    private:
        HeadCache* mHeadCache;
    };''',
)

replace_exact(
    "apps/openmw/mwsound/ffmpegdecoder.cpp",
    '''#include <libavutil/channel_layout.h>
#endif

namespace MWSound''',
    '''#include <libavutil/channel_layout.h>
#endif

#include "headcache.hpp"

namespace MWSound''',
)

replace_exact(
    "apps/openmw/mwsound/ffmpegdecoder.cpp",
    '''    void FFmpegDecoder::open(VFS::Path::NormalizedView fname)
    {
        close();
        mDataStream = mResourceMgr->get(fname);

        AVIOContextPtr ioCtx;''',
    '''    void FFmpegDecoder::open(VFS::Path::NormalizedView fname)
    {
        close();
        bool cached = false;
        if (mHeadCache != nullptr)
        {
            if (std::shared_ptr<const HeadBuffer> buffer = mHeadCache->lookup(fname))
            {
                mDataStream = makeHeadStream(std::move(buffer), *mResourceMgr);
                cached = true;
            }
            else
                mDataStream = makeRecordingStream(mResourceMgr->get(fname));
        }
        else
            mDataStream = mResourceMgr->get(fname);

        AVIOContextPtr ioCtx;''',
)

replace_exact(
    "apps/openmw/mwsound/ffmpegdecoder.cpp",
    '''        if (!opened && !openContext(fname.value().data(), nullptr, false, ioCtx, formatCtxPtr, stream))
            throw std::runtime_error("Failed to open input");

        const AVCodec* codec''',
    '''        if (!opened && !openContext(fname.value().data(), nullptr, false, ioCtx, formatCtxPtr, stream))
            throw std::runtime_error("Failed to open input");

        if (mHeadCache != nullptr && !cached)
            mHeadCache->insert(fname, *mDataStream);

        const AVCodec* codec''',
)

replace_exact(
    "apps/openmw/mwsound/ffmpegdecoder.cpp",
    "    FFmpegDecoder::FFmpegDecoder(const VFS::Manager* vfs)\n",
    "    FFmpegDecoder::FFmpegDecoder(const VFS::Manager* vfs, HeadCache* headCache)\n",
)

replace_exact(
    "apps/openmw/mwsound/ffmpegdecoder.cpp",
    '''        , mDataBuf(nullptr)
        , mFrameData(nullptr)
        , mDataBufLen(0)
    {''',
    '''        , mDataBuf(nullptr)
        , mFrameData(nullptr)
        , mDataBufLen(0)
        , mHeadCache(headCache)
    {''',
)

replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.hpp",
    '''    class SoundBase;
    class Sound;
    class Stream;
''',
    '''    class SoundBase;
    class Sound;
    class Stream;
    class HeadCache;
''',
)

replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.hpp",
    '''        const VFS::Manager* mVFS;

        std::unique_ptr<SoundOutput> mOutput;''',
    '''        const VFS::Manager* mVFS;

        std::unique_ptr<HeadCache> mHeadCache;

        std::unique_ptr<SoundOutput> mOutput;''',
)

replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.hpp",
    '''    protected:
        DecoderPtr getDecoder();
        friend class OpenALOutput;''',
    '''    protected:
        DecoderPtr getDecoder();
        DecoderPtr getStreamDecoder();
        friend class OpenALOutput;''',
)

replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.cpp",
    '''#include "constants.hpp"
#include "ffmpegdecoder.hpp"
#include "openaloutput.hpp"''',
    '''#include "constants.hpp"
#include "ffmpegdecoder.hpp"
#include "headcache.hpp"
#include "openaloutput.hpp"''',
)

replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.cpp",
    '''            return volume;
        }
    }

    // For combining PlayMode and Type flags''',
    '''            return volume;
        }

        std::unique_ptr<HeadCache> makeHeadCache(const VFS::Manager& vfs)
        {
            const std::size_t sizeMb = Settings::sound().mHeadCacheSize;
            if (sizeMb == 0)
                return nullptr;
            return std::make_unique<HeadCache>(vfs, sizeMb * 1024 * 1024);
        }
    }

    // For combining PlayMode and Type flags''',
)

replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.cpp",
    '''    SoundManager::SoundManager(const VFS::Manager* vfs, bool useSound)
        : mVFS(vfs)
        , mOutput(std::make_unique<OpenALOutput>(*this))''',
    '''    SoundManager::SoundManager(const VFS::Manager* vfs, bool useSound)
        : mVFS(vfs)
        , mHeadCache(makeHeadCache(*vfs))
        , mOutput(std::make_unique<OpenALOutput>(*this))''',
)

replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.cpp",
    '''    DecoderPtr SoundManager::getDecoder()
    {
        return std::make_shared<FFmpegDecoder>(mVFS);
    }

    DecoderPtr SoundManager::loadVoice''',
    '''    DecoderPtr SoundManager::getDecoder()
    {
        return std::make_shared<FFmpegDecoder>(mVFS, nullptr);
    }

    DecoderPtr SoundManager::getStreamDecoder()
    {
        return std::make_shared<FFmpegDecoder>(mVFS, mHeadCache.get());
    }

    DecoderPtr SoundManager::loadVoice''',
)

replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.cpp",
    "            DecoderPtr decoder = getDecoder();\n            decoder->open(Misc::ResourceHelpers::correctSoundPath(voicefile, *decoder->mResourceMgr));",
    "            DecoderPtr decoder = getStreamDecoder();\n            decoder->open(Misc::ResourceHelpers::correctSoundPath(voicefile, *decoder->mResourceMgr));",
)

replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.cpp",
    '''        Log(Debug::Info) << "Playing \\\"" << filename << "\\\"";

        DecoderPtr decoder = getDecoder();''',
    '''        Log(Debug::Info) << "Playing \\\"" << filename << "\\\"";

        DecoderPtr decoder = getStreamDecoder();''',
)

# -----------------------------------------------------------------------------
# 2) V3.16 runtime modes. Freeze promoted Mode84 renderer/paging architecture.
# -----------------------------------------------------------------------------
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

old_menu = "Write-Host ' 85 = V3.15 aggressive packet/governor candidate'"
new_menu = """Write-Host ' 85 = V3.15 aggressive packet/governor candidate'
Write-Host ' 86 = V3.16 exact V3.15 Mode84 control'
Write-Host ' 87 = V3.16 Mode84 + 64MB streamed-audio head cache'
Write-Host ' 88 = V3.16 balanced general-play hitch candidate'
Write-Host ' 89 = V3.16 aggressive general-play hitch candidate'"""
if text.count(old_menu) != 1:
    raise RuntimeError("V3.16 launcher menu anchor mismatch")
text = text.replace(old_menu, new_menu, 1)

text, n = re.subn(
    r"do \{ \$choice = Read-Host 'Enter 1 through 85' \} until \(\$choice -in @\(([^\n]+)\)\)",
    lambda m: "do { $choice = Read-Host 'Enter 1 through 89' } until ($choice -in @(" + m.group(1)
    + ",'86','87','88','89'))",
    text,
    count=1,
)
if n != 1:
    raise RuntimeError("V3.16 launcher choice-range anchor mismatch")

# Mode86 copies Mode84 assignments exactly. 87 only adds the cache. 88/89 keep
# the same renderer/paging stack while raising the sound-head RAM budget.
mode84 = next(line for line in text.splitlines() if line.lstrip().startswith("'84'"))
body84 = mode84[mode84.index("{") + 1 : mode84.rindex("}")].strip()
insert_anchor = mode84 + "\n"
new_modes = (
    mode84 + "\n"
    + "        '86' { " + body84.replace("$Experiment = 'v315-balanced-full'", "$Experiment = 'v316-mode84-control'") + " }\n"
    + "        '87' { " + body84.replace("$Experiment = 'v315-balanced-full'", "$Experiment = 'v316-audio64'")
    + "; $V316HeadCacheSize = '64' }\n"
    + "        '88' { " + body84.replace("$Experiment = 'v315-balanced-full'", "$Experiment = 'v316-balanced-hitch'")
    + "; $V316HeadCacheSize = '64' }\n"
    + "        '89' { " + body84.replace("$Experiment = 'v315-balanced-full'", "$Experiment = 'v316-aggressive-hitch'")
    + "; $V316HeadCacheSize = '128' }\n"
)
if text.count(insert_anchor) != 1:
    raise RuntimeError("V3.16 Mode84 switch anchor mismatch")
text = text.replace(insert_anchor, new_modes, 1)

# Default is zero so Mode86 is exact control.
default_anchor = "$V315AdaptiveCompileGovernor = '0'\n$RendererProfiling"
if text.count(default_anchor) != 1:
    raise RuntimeError("V3.16 launcher defaults anchor mismatch")
text = text.replace(
    default_anchor,
    "$V315AdaptiveCompileGovernor = '0'\n$V316HeadCacheSize = '0'\n$RendererProfiling",
    1,
)

# Add sound cache setting to the settings mutation block near the V3.15 controls.
setting_anchor = "Set-IniValue $SettingsPath 'V3' 'v3.15 adaptive compile governor' $V315AdaptiveCompileGovernor"
if text.count(setting_anchor) != 1:
    raise RuntimeError("V3.16 launcher setting anchor mismatch")
text = text.replace(
    setting_anchor,
    setting_anchor + "\n        Set-IniValue $SettingsPath 'Sound' 'head cache size' $V316HeadCacheSize",
    1,
)

launcher.write_text(text, encoding="utf-8", newline="\n")
print("V3.16 launcher modes 86-89 added")

# Strong markers for artifact identity and preflight policy checks.
marker = ROOT / "V3.16-HITCH-LAYER.txt"
marker.write_text(
    "\n".join(
        [
            "V3.16 general-play hitch suppression",
            "upstream_audio_head_cache=9ec49cfb4709cbfd8f14e97f5b9a558b71b8184f",
            "mode86=v3.15-mode84-control",
            "mode87=audio-head-cache-64mb",
            "mode88=balanced-hitch-foundation",
            "mode89=aggressive-hitch-foundation",
            "",
        ]
    ),
    encoding="utf-8",
    newline="\n",
)

print("V3.16 general-play hitch layer applied")
