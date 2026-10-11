// Included inside SceneUtil::GpuIndirectPackets after its clip-contract helpers.
struct RetiredStorage
{
    std::array<GLuint, 4> buffers{};
    std::array<osg::GLsync, 2> fences{};
    bool uncertain = false;
    std::shared_ptr<Capacity> capacity;
    unsigned context = ~0u;
    void cleared() const
    {
        if (capacity && context < capacity->pendingContexts.size())
            capacity->pendingContexts[context].store(false, std::memory_order_release);
    }
};

class SyncRetirement final : public osg::GraphicsObjectManager
{
public:
#ifdef OPENMW_INDIRECT_PACKET_TEST_HOOKS
    inline static bool holdRetirement = false;
#endif
    explicit SyncRetirement(unsigned context) : osg::GraphicsObjectManager("OptimizedMW-IndirectStorage", context) {}
    void retire(RetiredStorage value)
    {
        if (!value.buffers[0]) return;
        if (value.capacity && value.capacity->charge) value.capacity->charge->pending();
        std::lock_guard lock(mMutex);
        if (!mAlive) { value.cleared(); return; } // Its GL namespace is already gone.
        for (auto& entry : mPending) if (!entry.buffers[0]) { entry = std::move(value); return; }
        std::terminate(); // <=1024 live tickets, one storage ticket per context.
    }
    bool alive() const { std::lock_guard lock(mMutex); return mAlive; }
    void flushDeletedGLObjects(double, double&) override { flushAllDeletedGLObjects(); }
    void flushAllDeletedGLObjects() override
    {
#ifdef OPENMW_INDIRECT_PACKET_TEST_HOOKS
        if (holdRetirement) return;
#endif
        std::lock_guard lock(mMutex);
        auto* ext = osg::get<osg::GLExtensions>(_contextID);
        if (!ext || !ext->glClientWaitSync || !ext->glDeleteSync || !ext->glDeleteBuffers) return;
        for (auto& entry : mPending) if (entry.buffers[0] && !entry.uncertain)
        {
            bool ready = true;
            for (auto fence : entry.fences) if (fence)
            {
                const auto result = ext->glClientWaitSync(fence, 0, 0);
                ready &= result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED;
            }
            if (!ready) continue;
            for (auto fence : entry.fences) if (fence) ext->glDeleteSync(fence);
            // Exact private names leave the application's delete queue before
            // admission capacity is released. Driver physical reuse is not
            // inferred; the shared ledger never gives immediate OS free credit.
            ext->glDeleteBuffers(static_cast<GLsizei>(entry.buffers.size()), entry.buffers.data());
            entry.cleared();
            entry = {};
        }
    }
    void deleteAllGLObjects() override { flushAllDeletedGLObjects(); }
    void discardAllGLObjects() override
    {
        std::lock_guard lock(mMutex);
        mAlive = false;
        for (auto& entry : mPending) { entry.cleared(); entry = {}; }
    }
private:
    mutable std::mutex mMutex;
    std::array<RetiredStorage, MaximumLiveOwners> mPending{};
    bool mAlive = true;
};

using GetIndexed = void (GL_APIENTRY*)(GLenum, GLuint, GLint*);
using GetIndexed64 = void (GL_APIENTRY*)(GLenum, GLuint, GLint64*);

inline float queryGlVersion()
{
#ifdef OPENMW_INDIRECT_PACKET_TEST_HOOKS
    ++Counters::actualVersionQueries;
#endif
    return osg::getGLVersionNumber();
}

inline void* resolveGpuFunction(const char* name)
{
#ifdef OPENMW_INDIRECT_PACKET_TEST_HOOKS
    ++Counters::actualFunctionResolutions;
#endif
    return osg::getGLExtensionFuncPtr(name);
}

struct Context
{
    unsigned id = ~0u, next = 0;
    bool failed = false;
    std::array<bool, 2> gpuCommands{};
    RetiredStorage storage;
    osg::ref_ptr<SyncRetirement> retirement;
    osg::ref_ptr<osg::GLExtensions> capabilityOwner;
    GetIndexed indexed = nullptr;
    GetIndexed64 indexed64 = nullptr;
    bool gpuFunctionsResolved = false;
};

class Primitive final : public osg::DrawElementsUInt
{
public:
    Primitive() : osg::DrawElementsUInt(GL_TRIANGLES) {}
    Primitive(const Primitive& copy, const osg::CopyOp& op = osg::CopyOp::SHALLOW_COPY)
        : osg::DrawElementsUInt(copy, op), mBase(copy.mBase), mBounds(copy.mBounds), mLeaf(copy.mLeaf)
        , mCompute(copy.mCompute), mCapacity(copy.mCapacity), mOriginal(copy.mOriginal)
        , mReuseCommands(copy.mReuseCommands), mReuseCapabilities(copy.mReuseCapabilities)
        , mOpportunityFilter(copy.mOpportunityFilter) {}
    META_Object(SceneUtil, Primitive)

    Primitive(std::vector<unsigned>&& indices, std::vector<osg::DrawElementsIndirectCommand>&& commands,
        osg::Vec4Array* bounds, const osg::StateSet* leaf, bool gpu, std::shared_ptr<Capacity> capacity,
        const osg::Geometry::PrimitiveSetList& original)
        : osg::DrawElementsUInt(GL_TRIANGLES, indices.begin(), indices.end()), mBase(std::move(commands))
        , mBounds(bounds), mLeaf(leaf), mGpu(gpu), mCapacity(std::move(capacity)), mProduced(true), mOriginal(original)
        , mReuseCommands(reuseIndirectCommands.load(std::memory_order_relaxed))
        , mReuseCapabilities(reuseContextCapabilities.load(std::memory_order_relaxed))
        , mOpportunityFilter(filterVisibilityOpportunity.load(std::memory_order_relaxed))
    {
        for (unsigned i = 0; i + 2 < mBounds->size(); i += 3)
        {
            const auto& lo = (*mBounds)[i]; const auto& hi = (*mBounds)[i + 1];
            mWholeBounds.expandBy(osg::Vec3f(lo.x(), lo.y(), lo.z()));
            mWholeBounds.expandBy(osg::Vec3f(hi.x(), hi.y(), hi.z()));
        }
        mCompute = new osg::Program;
        mCompute->addShader(new osg::Shader(osg::Shader::COMPUTE, computeSource()));
    }

    static bool supported(osg::State& state, bool reuseCapabilities = false)
    {
        auto* ext = state.get<osg::GLExtensions>();
        if (reuseCapabilities && !ext) return false;
        const bool mdi = (reuseCapabilities && ext ? ext->glVersion : queryGlVersion()) >= 4.3f
            || osg::isGLExtensionSupported(state.getContextID(), "GL_ARB_multi_draw_indirect");
        const bool sync = (reuseCapabilities && ext ? ext->glVersion : queryGlVersion()) >= 3.2f
            || osg::isGLExtensionSupported(state.getContextID(), "GL_ARB_sync");
        return mdi && sync && ext && ext->glMultiDrawElementsIndirect && ext->glFenceSync
            && ext->glClientWaitSync && ext->glDeleteSync && ext->glGenBuffers && ext->glDeleteBuffers
            && ext->glBindBuffer && ext->glBufferData && ext->glBufferSubData;
    }

#ifdef OPENMW_INDIRECT_PACKET_TEST_HOOKS
    inline static bool forceBusy = false;
    inline static bool forceCpuCommands = false;
    inline static bool forceMissingGpuFunctions = false;
    void dropCapabilityCertificate() const
    {
        std::lock_guard lock(mContextMutex);
        for (auto& context : mContexts)
        { context.capabilityOwner = nullptr; context.gpuFunctionsResolved = false; }
    }
    std::vector<osg::DrawElementsIndirectCommand> readLastCommands(osg::State& state) const
    {
        std::vector<osg::DrawElementsIndirectCommand> output;
        const auto found = std::find_if(mContexts.begin(), mContexts.end(),
            [&](const Context& value) { return value.id == state.getContextID(); });
        if (found == mContexts.end() || found->failed) return output;
        auto* ext = state.get<osg::GLExtensions>();
        if (!ext || !ext->glGetBufferSubData) return output;
        GLint previous = 0; glGetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING, &previous);
        ext->glBindBuffer(GL_DRAW_INDIRECT_BUFFER, found->storage.buffers[2 + (found->next + 1) % 2]);
        output.resize(mBase.size());
        ext->glGetBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0,
            static_cast<osg::GLsizeiptr>(output.size() * sizeof(osg::DrawElementsIndirectCommand)), output.data());
        ext->glBindBuffer(GL_DRAW_INDIRECT_BUFFER, static_cast<GLuint>(previous));
        return output;
    }
#endif

    void draw(osg::State& state, bool useVbo) const override
    {
        auto fallback = [&] {
            ++Counters::fallbacks;
            // Original primitive boundaries and BOs are retained. In particular,
            // fallback never turns a custom gl_PrimitiveID shader into one merged draw.
            for (const auto& primitive : mOriginal) primitive->draw(state, useVbo);
        };
        if (!mProduced || !useVbo || state.getContextID() >= 4) { fallback(); return; }
        const auto* consumer = state.getLastAppliedProgramObject();
        const osg::Program* program = consumer ? consumer->getProgram() : nullptr;
        osg::Matrixf projection(state.getProjectionMatrix());
        bool projectionKnown = false, projectionChecked = false;
        const auto resolveProjection = [&] {
            if (!projectionChecked)
            {
                projectionChecked = true; projectionKnown = linearObjectProjection(program);
                if (program && projectionKnown)
                {
                    const auto* applied = appliedProjection(state, mLeaf);
                    projectionKnown = applied && applied->get(projection);
                }
            }
            return projectionKnown;
        };
        if (mGpu && mOpportunityFilter && resolveProjection()
#ifdef OPENMW_INDIRECT_PACKET_TEST_HOOKS
            && !forceCpuCommands
#endif
            && wholeBoundInside(mWholeBounds, osg::Matrixf(state.getModelViewMatrix()) * projection))
        {
            // Keep all original primitive boundaries and any previously allocated
            // private generations/fences. No private GL allocation or submission
            // is needed when every command must remain visible.
            for (const auto& primitive : mOriginal) primitive->draw(state, useVbo);
            ++Counters::visibilityOriginalDraws; ++Counters::visibilityDispatchesAvoided;
            return;
        }
        if (!supported(state, mReuseCapabilities)) { fallback(); return; }
        std::unique_lock lock(mContextMutex, std::try_to_lock);
        if (!lock.owns_lock()) { fallback(); return; }
#ifdef OPENMW_INDIRECT_PACKET_TEST_HOOKS
        if (forceBusy) { ++Counters::busySlots; fallback(); return; }
#endif
        auto* ext = state.get<osg::GLExtensions>();
        auto found = std::find_if(mContexts.begin(), mContexts.end(),
            [&](const Context& value) { return value.id == state.getContextID(); });
        if (found == mContexts.end())
        {
            if (!sample || !Resource::SpeculativeBudget::newOwnerWorkUsable(sample(sampleOwner),
                Resource::SpeculativePriority::Background)) { ++Counters::admissionDeclines; fallback(); return; }
            found = std::find_if(mContexts.begin(), mContexts.end(), [](const Context& value) { return value.id == ~0u; });
            if (found == mContexts.end() || !initialize(*found, state)) { fallback(); return; }
        }
        auto* retirement = state.get<SyncRetirement>();
        if (found->failed || found->retirement.get() != retirement || !retirement->alive()) { fallback(); return; }
        int selected = -1;
        for (unsigned n = 0; n < 2; ++n)
        {
            const unsigned slot = (found->next + n) % 2;
            auto& fence = found->storage.fences[slot];
            if (fence)
            {
                const auto result = ext->glClientWaitSync(fence, 0, 0);
                if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) continue;
                ext->glDeleteSync(fence); fence = nullptr;
            }
            selected = static_cast<int>(slot); found->next = (slot + 1) % 2; break;
        }
        if (selected < 0) { ++Counters::busySlots; fallback(); return; }

        // Use raw private names temporarily and restore actual names exactly.
        // No OSG buffer/program cache is edited by this path.
        GLint oldElement = 0, oldIndirect = 0;
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING_ARB, &oldElement);
        glGetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING, &oldIndirect);
        ext->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB, found->storage.buffers[0]);
        ext->glBindBuffer(GL_DRAW_INDIRECT_BUFFER, found->storage.buffers[2 + selected]);
        bool gpu = mGpu && (mReuseCapabilities ? ext->glVersion : queryGlVersion()) >= 4.3f
            && ext->glDispatchCompute && ext->glMemoryBarrier
            && ext->glBindBufferRange && ext->glBindBufferBase && ext->glUniform1ui && ext->glUniformMatrix4fv;
#ifdef OPENMW_INDIRECT_PACKET_TEST_HOOKS
        if (forceCpuCommands) gpu = false;
#endif
        if (gpu && !resolveProjection())
        { gpu = false; ++Counters::unknownShaders; }
        GetIndexed indexed = nullptr; GetIndexed64 indexed64 = nullptr;
        if (!mReuseCapabilities || gpu)
        {
            if (mReuseCapabilities && found->capabilityOwner.get() == ext && found->gpuFunctionsResolved)
            {
                indexed = found->indexed; indexed64 = found->indexed64;
                Counters::gpuResolutionsAvoided.fetch_add(2, std::memory_order_relaxed);
            }
            else
            {
                indexed = reinterpret_cast<GetIndexed>(resolveGpuFunction("glGetIntegeri_v"));
                indexed64 = reinterpret_cast<GetIndexed64>(resolveGpuFunction("glGetInteger64i_v"));
#ifdef OPENMW_INDIRECT_PACKET_TEST_HOOKS
                if (forceMissingGpuFunctions) { indexed = nullptr; indexed64 = nullptr; }
#endif
                if (mReuseCapabilities)
                {
                    found->capabilityOwner = ext; found->indexed = indexed; found->indexed64 = indexed64;
                    found->gpuFunctionsResolved = true; ++Counters::capabilitySnapshots;
                }
            }
        }
        gpu = gpu && indexed && indexed64;
        if (gpu)
        {
            GLint feedback = GL_FALSE, paused = GL_FALSE, actualProgram = 0;
            glGetIntegerv(GL_TRANSFORM_FEEDBACK_ACTIVE, &feedback);
            glGetIntegerv(GL_TRANSFORM_FEEDBACK_PAUSED, &paused);
            glGetIntegerv(GL_CURRENT_PROGRAM, &actualProgram);
            if ((feedback && !paused) || glIsEnabled(GL_RASTERIZER_DISCARD)
                || static_cast<GLuint>(actualProgram) != (consumer ? consumer->getHandle() : 0)) gpu = false;
        }
        if (gpu)
        {
            mCompute->compileGLObjects(state);
            auto* compute = mCompute->getPCP(state);
            gpu = compute && compute->isLinked() && !compute->needsLink();
            if (gpu)
            {
                GLint generic = 0, actualProgram = 0;
                std::array<GLint, 2> binding{}; std::array<GLint64, 2> start{}, size{};
                glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &generic);
                glGetIntegerv(GL_CURRENT_PROGRAM, &actualProgram);
                for (unsigned i = 0; i < 2; ++i)
                {
                    indexed(GL_SHADER_STORAGE_BUFFER_BINDING, i, &binding[i]);
                    indexed64(GL_SHADER_STORAGE_BUFFER_START, i, &start[i]);
                    indexed64(GL_SHADER_STORAGE_BUFFER_SIZE, i, &size[i]);
                }
                ext->glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, found->storage.buffers[1], 0, mBounds->getTotalDataSize());
                ext->glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, found->storage.buffers[2 + selected], 0,
                    static_cast<osg::GLsizeiptr>(mBase.size() * sizeof(osg::DrawElementsIndirectCommand)));
                ext->glUseProgram(compute->getHandle());
                const osg::Matrixf mvp = osg::Matrixf(state.getModelViewMatrix()) * projection;
                ext->glUniformMatrix4fv(compute->getUniformLocation("mvp"), 1, GL_FALSE, mvp.ptr());
                ext->glUniform1ui(compute->getUniformLocation("rows"), static_cast<GLuint>(mBase.size()));
                ext->glDispatchCompute(static_cast<GLuint>((mBase.size() + 31) / 32), 1, 1);
                ext->glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
                found->gpuCommands[selected] = true;
                ext->glUseProgram(static_cast<GLuint>(actualProgram));
                for (unsigned i = 0; i < 2; ++i)
                    if (binding[i] && size[i] > 0) ext->glBindBufferRange(GL_SHADER_STORAGE_BUFFER, i,
                        static_cast<GLuint>(binding[i]), static_cast<osg::GLintptr>(start[i]), static_cast<osg::GLsizeiptr>(size[i]));
                    else ext->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, static_cast<GLuint>(binding[i]));
                ext->glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(generic));
                ++Counters::dispatches;
            }
        }
        if (!gpu)
        {
            // initialize() already populated each private slot with mBase.
            // Only a GPU visibility write can change those immutable commands.
            // Fence selection above still proves that the slot is available.
            if (!mReuseCommands || found->gpuCommands[selected])
            {
                const auto bytes = mBase.size() * sizeof(osg::DrawElementsIndirectCommand);
                ext->glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, static_cast<osg::GLsizeiptr>(bytes), mBase.data());
                found->gpuCommands[selected] = false;
                ++Counters::commandUploads;
                Counters::commandUploadBytes.fetch_add(bytes, std::memory_order_relaxed);
            }
            else ++Counters::commandUploadsAvoided;
        }
        ext->glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr,
            static_cast<GLsizei>(mBase.size()), sizeof(osg::DrawElementsIndirectCommand));
        found->storage.fences[selected] = ext->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!found->storage.fences[selected]) { found->failed = true; found->storage.uncertain = true; }
        ext->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB, static_cast<GLuint>(oldElement));
        ext->glBindBuffer(GL_DRAW_INDIRECT_BUFFER, static_cast<GLuint>(oldIndirect));
        ++Counters::draws; Counters::commands.fetch_add(mBase.size(), std::memory_order_relaxed);
        if (mReuseCapabilities) ++Counters::capabilityDraws;
    }

    void releaseGLObjects(osg::State* state = nullptr) const override
    {
        std::lock_guard lock(mContextMutex);
        for (auto& context : mContexts)
            if (!state || state->getContextID() == context.id) retire(context);
        if (mCompute) mCompute->releaseGLObjects(state);
        // The control primitive BOs belong to the original graph and are never
        // released by this private aggregate.
    }
protected:
    ~Primitive() override
    { for (auto& context : mContexts) retire(context); }
private:
    bool initialize(Context& context, osg::State& state) const
    {
        // A release does not free a backing generation. The same ticket may
        // allocate its replacement only after exact previous buffer retirement.
        if (!mCapacity || mCapacity->pendingContexts[state.getContextID()].load(std::memory_order_acquire))
        { ++Counters::busySlots; return false; }
        osg::ref_ptr<SyncRetirement> retirement;
        try { retirement = state.get<SyncRetirement>(); }
        catch (const std::bad_alloc&) { return false; }
        if (!retirement->alive()) return false;
        auto* ext = state.get<osg::GLExtensions>();
        GLint previous = 0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING_ARB, &previous);
        context.id = state.getContextID(); context.retirement = retirement;
        context.gpuCommands = {};
        context.storage.capacity = mCapacity;
        context.storage.context = state.getContextID();
        ext->glGenBuffers(static_cast<GLsizei>(context.storage.buffers.size()), context.storage.buffers.data());
        for (unsigned i = 0; i < context.storage.buffers.size(); ++i)
        {
            ext->glBindBuffer(GL_ARRAY_BUFFER_ARB, context.storage.buffers[i]);
            const void* data = i == 0 ? getDataPointer() : i == 1 ? mBounds->getDataPointer() : mBase.data();
            const auto bytes = i == 0 ? getTotalDataSize() : i == 1 ? mBounds->getTotalDataSize()
                : static_cast<unsigned>(mBase.size() * sizeof(osg::DrawElementsIndirectCommand));
            ext->glBufferData(GL_ARRAY_BUFFER_ARB, bytes, data, i < 2 ? GL_STATIC_DRAW : GL_STREAM_DRAW);
        }
        ext->glBindBuffer(GL_ARRAY_BUFFER_ARB, static_cast<GLuint>(previous));
        if (glGetError() != GL_NO_ERROR || std::find(context.storage.buffers.begin(), context.storage.buffers.end(), 0)
            != context.storage.buffers.end())
        {
            // No indirect/compute use has been submitted. Delete these optional
            // failed names on their exact current owner and keep this view control.
            ext->glDeleteBuffers(static_cast<GLsizei>(context.storage.buffers.size()), context.storage.buffers.data());
            context.storage.buffers = {}; context.failed = true; return false;
        }
        return true;
    }
    static void retire(Context& context)
    {
        if (context.storage.buffers[0] && context.storage.capacity && context.storage.context < 4)
            context.storage.capacity->pendingContexts[context.storage.context].store(true, std::memory_order_release);
        if (context.retirement) context.retirement->retire(std::move(context.storage));
        context.storage = {}; context.retirement = nullptr; context.id = ~0u; context.next = 0; context.failed = false;
        context.capabilityOwner = nullptr; context.indexed = nullptr; context.indexed64 = nullptr;
        context.gpuFunctionsResolved = false;
    }
    const std::vector<osg::DrawElementsIndirectCommand> mBase;
    osg::ref_ptr<const osg::Vec4Array> mBounds;
    osg::ref_ptr<const osg::StateSet> mLeaf;
    osg::ref_ptr<osg::Program> mCompute;
    bool mGpu = false;
    std::shared_ptr<Capacity> mCapacity;
    bool mProduced = false;
    const osg::Geometry::PrimitiveSetList mOriginal;
    bool mReuseCommands = false;
    bool mReuseCapabilities = false, mOpportunityFilter = false;
    osg::BoundingBoxf mWholeBounds;
    mutable std::array<Context, 4> mContexts;
    mutable std::mutex mContextMutex;
};
