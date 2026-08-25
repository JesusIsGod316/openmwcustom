from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel: str, old: str, new: str) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"prepared-fastpath safety patched {rel}")


# OFF should be nearly upstream-cost-free. Use an atomic enable snapshot so the
# normal getInstance path never takes the prepared-pool mutex while disabled.
replace_once(
    "components/resource/scenemanager.hpp",
    '''#include <array>
#include <cstddef>''',
    '''#include <atomic>
#include <array>
#include <cstddef>''',
)
replace_once(
    "components/resource/scenemanager.hpp",
    '''        mutable std::mutex mPreparedInstanceMutex;
        std::map<VFS::Path::Normalized, std::deque<osg::ref_ptr<osg::Node>>, std::less<>> mPreparedInstances;''',
    '''        std::atomic_bool mPreparedInstanceEnabled{ false };
        mutable std::mutex mPreparedInstanceMutex;
        std::map<VFS::Path::Normalized, std::deque<osg::ref_ptr<osg::Node>>, std::less<>> mPreparedInstances;''',
)

replace_once(
    "components/resource/scenemanager.cpp",
    '''        {
            std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
            if (mPreparedInstanceLimit != 0)
            {
                const auto found = mPreparedInstances.find(path);
                if (found != mPreparedInstances.end() && !found->second.empty())
                {
                    osg::ref_ptr<osg::Node> prepared = std::move(found->second.front());
                    found->second.pop_front();
                    --mPreparedInstanceCount;
                    ++mPreparedInstanceHits;
                    if (found->second.empty())
                        mPreparedInstances.erase(found);
                    return prepared;
                }
                ++mPreparedInstanceMisses;
            }
        }
        return getInstance(getTemplate(path));''',
    '''        if (mPreparedInstanceEnabled.load(std::memory_order_acquire))
        {
            std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
            if (mPreparedInstanceLimit != 0)
            {
                const auto found = mPreparedInstances.find(path);
                if (found != mPreparedInstances.end() && !found->second.empty())
                {
                    osg::ref_ptr<osg::Node> prepared = std::move(found->second.front());
                    found->second.pop_front();
                    --mPreparedInstanceCount;
                    ++mPreparedInstanceHits;
                    if (found->second.empty())
                        mPreparedInstances.erase(found);
                    return prepared;
                }
                ++mPreparedInstanceMisses;
            }
        }
        return getInstance(getTemplate(path));''',
)

replace_once(
    "components/resource/scenemanager.cpp",
    '''        if (mPreparedInstanceLimit != limit)
            ++mPreparedInstanceGeneration;
        mPreparedInstanceLimit = limit;
        while (mPreparedInstanceCount > mPreparedInstanceLimit && !mPreparedInstances.empty())''',
    '''        if (mPreparedInstanceLimit != limit)
            ++mPreparedInstanceGeneration;
        mPreparedInstanceLimit = limit;
        mPreparedInstanceEnabled.store(limit != 0, std::memory_order_release);
        while (mPreparedInstanceCount > mPreparedInstanceLimit && !mPreparedInstances.empty())''',
)

replace_once(
    "components/resource/scenemanager.cpp",
    '''    bool SceneManager::prepareInstance(VFS::Path::NormalizedView path)
    {
        std::size_t generation = 0;''',
    '''    bool SceneManager::prepareInstance(VFS::Path::NormalizedView path)
    {
        if (!mPreparedInstanceEnabled.load(std::memory_order_acquire))
            return false;

        std::size_t generation = 0;''',
)

# Clearing the pool invalidates in-flight work through the generation token;
# the configured enable state remains unchanged because clearCache is not a
# settings change.

header = (ROOT / "components/resource/scenemanager.hpp").read_text(encoding="utf-8")
source = (ROOT / "components/resource/scenemanager.cpp").read_text(encoding="utf-8")
required = [
    (header, "std::atomic_bool mPreparedInstanceEnabled{ false }", "atomic enable snapshot missing"),
    (source, "if (mPreparedInstanceEnabled.load(std::memory_order_acquire))", "getInstance fast gate missing"),
    (source, "mPreparedInstanceEnabled.store(limit != 0, std::memory_order_release);", "enable snapshot update missing"),
    (source, "if (!mPreparedInstanceEnabled.load(std::memory_order_acquire))", "prepareInstance fast gate missing"),
]
problems = [message for text, needle, message in required if needle not in text]
if problems:
    raise RuntimeError("Prepared-instance fast-path safety preflight failed:\n" + "\n".join(problems))

print("V3 prepared-instance disabled fast path completed successfully.")
