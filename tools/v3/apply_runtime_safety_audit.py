from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel: str, old: str, new: str) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"runtime-safety audit patched {rel}")


# ---------------------------------------------------------------------------
# 1) PostFX labels were diagnostic-only object mutations. They are not needed
# by the current CPU pass telemetry (which uses the technique handle directly),
# so remove them completely. This restores upstream setPasses behaviour and
# eliminates an entire startup-time pointer/lifetime surface.
# ---------------------------------------------------------------------------
replace_once(
    "apps/openmw/mwrender/pingpongcanvas.cpp",
    '''    void PingPongCanvas::setPasses(Fx::DispatchArray&& passes)
    {
        mPasses = std::move(passes);
        for (auto& node : mPasses)
        {
            if (!node.mHandle)
                continue;
            const std::string techniqueName = node.mHandle->getName();
            if (node.mRootStateSet)
                node.mRootStateSet->setName("V3 PostFX " + techniqueName);
            for (std::size_t i = 0; i < node.mPasses.size(); ++i)
            {
                if (node.mPasses[i].mStateSet)
                    node.mPasses[i].mStateSet->setName(
                        "V3 PostFX " + techniqueName + " pass " + std::to_string(i));
            }
        }
    }''',
    '''    void PingPongCanvas::setPasses(Fx::DispatchArray&& passes)
    {
        mPasses = std::move(passes);
    }''',
)


# ---------------------------------------------------------------------------
# 2) Program names only exist to make OPENMW_V3_RENDER_FILE useful. Do not
# mutate osg::Program objects at all in normal gameplay. When profiling is
# enabled, tolerate vertex-only, fragment-only, and two-stage programs.
# ---------------------------------------------------------------------------
replace_once(
    "components/shader/shadermanager.cpp",
    '''            std::string v3ProgramName;
            if (vertexShader)
                v3ProgramName = vertexShader->getName();
            if (fragmentShader)
            {
                if (!v3ProgramName.empty())
                    v3ProgramName += " + ";
                v3ProgramName += fragmentShader->getName();
            }
            if (!v3ProgramName.empty())
                program->setName(v3ProgramName);''',
    '''            if (Debug::V3Diagnostics::renderWriter().enabled())
            {
                std::string v3ProgramName;
                if (vertexShader)
                    v3ProgramName = vertexShader->getName();
                if (fragmentShader)
                {
                    if (!v3ProgramName.empty())
                        v3ProgramName += " + ";
                    v3ProgramName += fragmentShader->getName();
                }
                if (!v3ProgramName.empty())
                    program->setName(v3ProgramName);
            }''',
)


# ---------------------------------------------------------------------------
# 3) The insertion profiler previously placed a pointer to stack-local stats in
# thread_local storage and restored it manually. Any exception before the
# restore would leave a dangling pointer. Use RAII so nested insertion and
# exceptional exits always restore the previous pointer.
# ---------------------------------------------------------------------------
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''    thread_local V3InsertionAccumulator* sV3InsertionAccumulator = nullptr;

    void addObject''',
    '''    thread_local V3InsertionAccumulator* sV3InsertionAccumulator = nullptr;

    class V3InsertionAccumulatorScope
    {
    public:
        explicit V3InsertionAccumulatorScope(V3InsertionAccumulator* current)
            : mPrevious(sV3InsertionAccumulator)
        {
            sV3InsertionAccumulator = current;
        }

        ~V3InsertionAccumulatorScope() { sV3InsertionAccumulator = mPrevious; }

        V3InsertionAccumulatorScope(const V3InsertionAccumulatorScope&) = delete;
        V3InsertionAccumulatorScope& operator=(const V3InsertionAccumulatorScope&) = delete;

    private:
        V3InsertionAccumulator* mPrevious = nullptr;
    };

    void addObject''',
)
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        V3InsertionAccumulator insertionStats;
        V3InsertionAccumulator* const previousStats = sV3InsertionAccumulator;
        if (insertionWriter.enabled())
            sV3InsertionAccumulator = &insertionStats;
''',
    '''        V3InsertionAccumulator insertionStats;
        V3InsertionAccumulatorScope insertionScope(insertionWriter.enabled() ? &insertionStats : nullptr);
''',
)
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''
        sV3InsertionAccumulator = previousStats;
        if (insertionWriter.enabled())
''',
    '''
        if (insertionWriter.enabled())
''',
)


# ---------------------------------------------------------------------------
# 4) V3 work-queue telemetry called getNumActiveThreads() from worker threads.
# Upstream implements that by walking mThreads, while stop() clears mThreads
# outside the queue mutex in order to join workers safely. That makes a
# profiling-time read race possible during shutdown. Maintain an atomic active
# count instead; this is safe to read from main or worker threads and preserves
# the public method's meaning.
# ---------------------------------------------------------------------------
replace_once(
    "components/sceneutil/workqueue.hpp",
    '''        std::vector<std::unique_ptr<WorkThread>> mThreads;
    };''',
    '''        std::vector<std::unique_ptr<WorkThread>> mThreads;
        std::atomic_size_t mV3ActiveThreads{ 0 };

        friend class WorkThread;
    };''',
)
replace_once(
    "components/sceneutil/workqueue.cpp",
    '''    size_t WorkQueue::getNumActiveThreads() const
    {
        return std::accumulate(
            mThreads.begin(), mThreads.end(), 0u, [](auto r, const auto& t) { return r + t->isActive(); });
    }''',
    '''    size_t WorkQueue::getNumActiveThreads() const
    {
        return mV3ActiveThreads.load(std::memory_order_relaxed);
    }''',
)
replace_once(
    "components/sceneutil/workqueue.cpp",
    '''            mActive = true;

            auto& writer = Debug::V3Diagnostics::workQueueWriter();''',
    '''            mActive = true;
            mWorkQueue->mV3ActiveThreads.fetch_add(1, std::memory_order_relaxed);

            auto& writer = Debug::V3Diagnostics::workQueueWriter();''',
)
replace_once(
    "components/sceneutil/workqueue.cpp",
    '''            }
            mActive = false;
        }
    }''',
    '''            }
            mActive = false;
            mWorkQueue->mV3ActiveThreads.fetch_sub(1, std::memory_order_relaxed);
        }
    }''',
)


# ---------------------------------------------------------------------------
# 5) Prepared-instance cache is experimental/off by default. Be stricter than
# the template pre-check: if instancing unexpectedly introduces update
# traversal, reject the clone instead of storing it for later activation.
# ---------------------------------------------------------------------------
replace_once(
    "components/resource/scenemanager.cpp",
    '''        osg::ref_ptr<osg::Node> prepared = getInstance(sceneTemplate.get());
        if (!prepared)
            return false;

        std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);''',
    '''        osg::ref_ptr<osg::Node> prepared = getInstance(sceneTemplate.get());
        if (!prepared || prepared->getNumChildrenRequiringUpdateTraversal() != 0)
        {
            std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);
            ++mPreparedInstanceRejected;
            return false;
        }

        std::lock_guard<std::mutex> lock(mPreparedInstanceMutex);''',
)


# ---------------------------------------------------------------------------
# Semantic preflight for the exact classes of runtime bug found in smoke test
# and this audit. These checks run on the fully generated C++ before CMake.
# ---------------------------------------------------------------------------
checks = {
    "components/shader/shadermanager.cpp": [
        ("program->setName(vertexShader->getName()", False,
         "unguarded shader-program naming dereference returned"),
        ("if (vertexShader)", True, "vertex shader null guard missing"),
        ("if (fragmentShader)", True, "fragment shader null guard missing"),
    ],
    "apps/openmw/mwrender/pingpongcanvas.cpp": [
        ("mRootStateSet->setName(\"V3 PostFX", False,
         "diagnostic PostFX StateSet mutation returned"),
        ("mStateSet->setName(\"V3 PostFX", False,
         "diagnostic PostFX pass StateSet mutation returned"),
    ],
    "apps/openmw/mwworld/scene.cpp": [
        ("V3InsertionAccumulatorScope insertionScope", True,
         "RAII insertion accumulator guard missing"),
        ("sV3InsertionAccumulator = previousStats", False,
         "manual dangling-prone insertion accumulator restore returned"),
    ],
    "components/sceneutil/workqueue.cpp": [
        ("return mV3ActiveThreads.load(std::memory_order_relaxed);", True,
         "atomic work-queue active counter missing"),
        ("mThreads.begin(), mThreads.end()", False,
         "worker telemetry can still iterate the mutable thread vector"),
    ],
    "components/resource/scenemanager.cpp": [
        ("!prepared || prepared->getNumChildrenRequiringUpdateTraversal() != 0", True,
         "prepared clone post-validation missing"),
    ],
}

problems = []
for rel, rules in checks.items():
    text = (ROOT / rel).read_text(encoding="utf-8")
    for needle, required, message in rules:
        present = needle in text
        if present != required:
            problems.append(f"{rel}: {message}")

if problems:
    raise RuntimeError("V3 runtime-safety preflight failed:\n" + "\n".join(problems))

print("V3 runtime-safety audit completed successfully.")
