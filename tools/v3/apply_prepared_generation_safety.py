from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel: str, old: str, new: str) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"prepared-generation safety patched {rel}")


# A preload worker releases mPreparedInstanceMutex while loading/cloning. If a
# world/cache clear happens in that interval, the completed old clone must not
# be inserted into the newly cleared pool. A generation token makes the final
# insertion conditional on the same cache generation still being active.
replace_once(
    "components/resource/scenemanager.hpp",
    '''        std::size_t mPreparedInstanceLimit = 0;
        std::size_t mPreparedInstanceCount = 0;
        std::size_t mPreparedInstanceAdded = 0;''',
    '''        std::size_t mPreparedInstanceLimit = 0;
        std::size_t mPreparedInstanceCount = 0;
        std::size_t mPreparedInstanceGeneration = 0;
        std::size_t mPreparedInstanceAdded = 0;''',
)

replace_once(
    "components/resource/scenemanager.cpp",
    '''    void SceneManager::setPreparedInstanceCacheLimit(std::size_t limit)
    {
        std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
        mPreparedInstanceLimit = limit;''',
    '''    void SceneManager::setPreparedInstanceCacheLimit(std::size_t limit)
    {
        std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
        if (mPreparedInstanceLimit != limit)
            ++mPreparedInstanceGeneration;
        mPreparedInstanceLimit = limit;''',
)

replace_once(
    "components/resource/scenemanager.cpp",
    '''    bool SceneManager::prepareInstance(VFS::Path::NormalizedView path)
    {
        {
            std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
            if (mPreparedInstanceLimit == 0 || mPreparedInstanceCount >= mPreparedInstanceLimit)
                return false;
        }

        osg::ref_ptr<const osg::Node> sceneTemplate = getTemplate(path);''',
    '''    bool SceneManager::prepareInstance(VFS::Path::NormalizedView path)
    {
        std::size_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
            if (mPreparedInstanceLimit == 0 || mPreparedInstanceCount >= mPreparedInstanceLimit)
                return false;
            generation = mPreparedInstanceGeneration;
        }

        osg::ref_ptr<const osg::Node> sceneTemplate = getTemplate(path);''',
)

replace_once(
    "components/resource/scenemanager.cpp",
    '''        std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
        if (mPreparedInstanceLimit == 0 || mPreparedInstanceCount >= mPreparedInstanceLimit)
            return false;
        mPreparedInstances[VFS::Path::Normalized(path)].push_back(std::move(prepared));''',
    '''        std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
        if (generation != mPreparedInstanceGeneration || mPreparedInstanceLimit == 0
            || mPreparedInstanceCount >= mPreparedInstanceLimit)
            return false;
        mPreparedInstances[VFS::Path::Normalized(path)].push_back(std::move(prepared));''',
)

replace_once(
    "components/resource/scenemanager.cpp",
    '''        {
            std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
            mPreparedInstances.clear();
            mPreparedInstanceCount = 0;
        }
        ResourceManager::clearCache();''',
    '''        {
            std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
            ++mPreparedInstanceGeneration;
            mPreparedInstances.clear();
            mPreparedInstanceCount = 0;
        }
        ResourceManager::clearCache();''',
)

# Semantic preflight on the fully generated source.
header = (ROOT / "components/resource/scenemanager.hpp").read_text(encoding="utf-8")
source = (ROOT / "components/resource/scenemanager.cpp").read_text(encoding="utf-8")
required = [
    (header, "mPreparedInstanceGeneration = 0", "generation member missing"),
    (source, "generation = mPreparedInstanceGeneration", "prepare token capture missing"),
    (source, "generation != mPreparedInstanceGeneration", "stale prepare rejection missing"),
    (source, "++mPreparedInstanceGeneration;\n            mPreparedInstances.clear();",
     "clearCache generation invalidation missing"),
]
problems = [message for text, needle, message in required if needle not in text]
if problems:
    raise RuntimeError("Prepared-instance generation safety preflight failed:\n" + "\n".join(problems))

print("V3 prepared-instance generation safety pass completed successfully.")
