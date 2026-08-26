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
# The insertion profiler previously placed a pointer to stack-local stats in
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
# Work-queue CSV telemetry used getNumActiveThreads() from worker threads.
# Upstream implements that by iterating mThreads; stop() clears that vector while
# workers are being joined, so worker-side reads are unsafe during shutdown.
# Keep upstream getNumActiveThreads() completely unchanged and maintain a V3-only
# atomic count only while OPENMW_V3_WORKQUEUE_FILE is enabled. Normal gameplay
# therefore pays no per-job atomic bookkeeping and keeps upstream semantics.
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

# Add-work telemetry runs as a WorkQueue member, so it can read the private
# atomic directly without walking mThreads.
replace_once(
    "components/sceneutil/workqueue.cpp",
    '''                << Debug::V3Diagnostics::csvQuote(typeName) << ',' << queueDepth << ',' << getNumActiveThreads() << ",0";''',
    '''                << Debug::V3Diagnostics::csvQuote(typeName) << ',' << queueDepth << ','
                << mV3ActiveThreads.load(std::memory_order_relaxed) << ",0";''',
)

# A trace-only capture should still know the work-item type. Active-worker
# bookkeeping is needed only for the separate workqueue CSV rows.
replace_once(
    "components/sceneutil/workqueue.cpp",
    '''            auto& writer = Debug::V3Diagnostics::workQueueWriter();
            const bool profile = writer.enabled();
            const std::uintptr_t itemId = reinterpret_cast<std::uintptr_t>(item.get());
            const std::string typeName = profile ? typeid(*item).name() : std::string();
            const auto start = profile ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
            Debug::V3Diagnostics::TraceScope trace("workqueue", typeName, std::to_string(itemId), 0.05);''',
    '''            auto& writer = Debug::V3Diagnostics::workQueueWriter();
            const bool profile = writer.enabled();
            const bool traceProfile = Debug::V3Diagnostics::traceWriter().enabled();
            if (profile)
                mWorkQueue->mV3ActiveThreads.fetch_add(1, std::memory_order_relaxed);
            const std::uintptr_t itemId = reinterpret_cast<std::uintptr_t>(item.get());
            const std::string typeName = (profile || traceProfile) ? typeid(*item).name() : std::string();
            const auto start = profile ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
            Debug::V3Diagnostics::TraceScope trace("workqueue", typeName, std::to_string(itemId), 0.05);''',
)
replace_once(
    "components/sceneutil/workqueue.cpp",
    '''                    << mWorkQueue->getNumActiveThreads() << ",0";''',
    '''                    << mWorkQueue->mV3ActiveThreads.load(std::memory_order_relaxed) << ",0";''',
)
replace_once(
    "components/sceneutil/workqueue.cpp",
    '''                    << mWorkQueue->getNumActiveThreads() << ',' << std::fixed << std::setprecision(3) << durationMs;''',
    '''                    << mWorkQueue->mV3ActiveThreads.load(std::memory_order_relaxed) << ',' << std::fixed
                    << std::setprecision(3) << durationMs;''',
)
replace_once(
    "components/sceneutil/workqueue.cpp",
    '''            }
            mActive = false;
        }
    }''',
    '''            }
            if (profile)
                mWorkQueue->mV3ActiveThreads.fetch_sub(1, std::memory_order_relaxed);
            mActive = false;
        }
    }''',
)


# ---------------------------------------------------------------------------
# Prepared-instance cache is experimental/off by default. Be stricter than the
# template pre-check: if instancing unexpectedly introduces update traversal,
# reject the clone instead of storing it for later activation.
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
# and this audit. The primary shader/render scripts now generate their safe
# forms directly; these checks make sure they stay that way.
# ---------------------------------------------------------------------------
checks = {
    "components/shader/shadermanager.cpp": [
        ("program->setName(v3ProgramName)", False,
         "diagnostic osg::Program mutation returned"),
        ("vertexShader->getName() + \" + \" + fragmentShader->getName()", False,
         "unguarded shader-pair dereference returned"),
        ("if (const osg::Shader* shader = getShader(i))", True,
         "null-safe relink shader enumeration missing"),
        ("csvQuote(v3ProgramDetail)", True,
         "shader relink detail output missing"),
    ],
    "apps/openmw/mwrender/pingpongcanvas.cpp": [
        ("setName(\"V3 PostFX", False,
         "diagnostic PostFX StateSet mutation returned"),
        ("v3PostFxWriter.writeLine(row.str())", True,
         "PostFX per-pass timing was lost"),
        ("node.mHandle ? node.mHandle->getName()", True,
         "PostFX technique label is no longer null-safe"),
    ],
    "apps/openmw/mwworld/scene.cpp": [
        ("V3InsertionAccumulatorScope insertionScope", True,
         "RAII insertion accumulator guard missing"),
        ("sV3InsertionAccumulator = previousStats", False,
         "manual dangling-prone insertion accumulator restore returned"),
    ],
    "components/sceneutil/workqueue.cpp": [
        ("mThreads.begin(), mThreads.end()", True,
         "upstream getNumActiveThreads implementation was unexpectedly replaced"),
        ("mV3ActiveThreads.load(std::memory_order_relaxed)", True,
         "V3 worker telemetry is missing its shutdown-safe active count"),
        ("mWorkQueue->getNumActiveThreads()", False,
         "worker telemetry still calls the shutdown-sensitive upstream counter"),
        ("if (profile)\n                mWorkQueue->mV3ActiveThreads.fetch_add", True,
         "V3 active-worker bookkeeping is not gated by workqueue profiling"),
        ("const bool traceProfile = Debug::V3Diagnostics::traceWriter().enabled();", True,
         "trace-only work-item typing missing"),
    ],
    "components/resource/scenemanager.cpp": [
        ("!prepared || prepared->getNumChildrenRequiringUpdateTraversal() != 0", True,
         "prepared clone post-validation missing"),
        ("if (!sceneTemplate || sceneTemplate->getNumChildrenRequiringUpdateTraversal() != 0)", True,
         "prepared template null/update guard missing"),
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
