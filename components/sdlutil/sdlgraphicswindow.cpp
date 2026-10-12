#include "sdlgraphicswindow.hpp"

#include <SDL3/SDL.h>
#if defined(_WIN32)
#pragma push_macro("NOGDI")
#undef NOGDI
#include <windows.h>
#include <wingdi.h>
#pragma pop_macro("NOGDI")
#endif

#ifdef OPENMW_GL4ES_MANUAL_INIT
#include "gl4esinit.h"
#endif

namespace SDLUtil
{

    GraphicsWindowSDL2::~GraphicsWindowSDL2()
    {
        close(true);
    }

    GraphicsWindowSDL2::GraphicsWindowSDL2(osg::GraphicsContext::Traits* traits, VSyncMode vsyncMode,
        bool diagnosticNamespaceRequested)
        : mWindow(nullptr)
        , mContext(nullptr)
        , mValid(false)
        , mRealized(false)
        , mOwnsWindow(false)
        , mVSyncMode(vsyncMode)
        , mDiagnosticRequested(diagnosticNamespaceRequested)
    {
        _traits = traits;

        init();
        if (GraphicsWindowSDL2::valid())
        {
            setState(new osg::State);
            getState()->setGraphicsContext(this);

            if (_traits.valid() && _traits->sharedContext.valid())
            {
                getState()->setContextID(_traits->sharedContext->getState()->getContextID());
                incrementContextIDUsageCount(getState()->getContextID());
            }
            else
            {
                getState()->setContextID(osg::GraphicsContext::createNewContextID());
            }
            if (mDiagnosticRequested)
            {
                mDiagnosticOsgContext = getState()->getContextID();
                mDiagnosticIssuedState = getState();
                // Saved by this actual native creation, while SDL had made it
                // current. OSG's association is assigned only after init.
                try
                {
                    auto issued = SDL_IsMainThread() ? RenderCore::Temporal::DiagnosticNamespace::issue(
                        {mDiagnosticCreationNative, mDiagnosticOsgContext}) : std::nullopt;
                    if (issued)
                    {
                        mDiagnosticNamespace = std::make_unique<RenderCore::Temporal::DiagnosticNamespace>(std::move(*issued));
                        mDiagnosticCloseState.store(DiagnosticCloseState::Open);
                    }
                    else mDiagnosticCloseState.store(DiagnosticCloseState::Unsupported);
                }
                catch (...) { mDiagnosticCloseState.store(DiagnosticCloseState::Unsupported); }
            }
        }
    }

    bool GraphicsWindowSDL2::setWindowDecorationImplementation(bool flag)
    {
        if (!mWindow)
            return false;

        SDL_SetWindowBordered(mWindow, flag);
        return true;
    }

    bool GraphicsWindowSDL2::setWindowRectangleImplementation(int x, int y, int width, int height)
    {
        if (!mWindow)
            return false;

        const float density = SDL_GetWindowPixelDensity(mWindow);
        const float scale = density > 0.f ? density : 1.f;

        SDL_SetWindowPosition(mWindow, x, y);
        SDL_SetWindowSize(mWindow, static_cast<int>(static_cast<float>(width) / scale + 0.5f),
            static_cast<int>(static_cast<float>(height) / scale + 0.5f));
        return true;
    }

    void GraphicsWindowSDL2::setWindowName(const std::string& name)
    {
        if (!mWindow)
            return;

        SDL_SetWindowTitle(mWindow, name.c_str());
        _traits->windowName = name;
    }

    void GraphicsWindowSDL2::setCursor(MouseCursor mouseCursor)
    {
        _traits->useCursor = false;
    }

    void GraphicsWindowSDL2::init()
    {
        if (mDiagnosticRequested && mContext)
            return; // Never overwrite a retained native handle after failed close.
        if (mValid)
            return;

        if (!_traits.valid())
            return;

        WindowData* inheritedWindowData = dynamic_cast<WindowData*>(_traits->inheritedWindowData.get());
        mWindow = inheritedWindowData ? inheritedWindowData->mWindow : nullptr;

        mOwnsWindow = (mWindow == nullptr);
        if (mOwnsWindow)
        {
            OSG_FATAL << "Error: No SDL window provided." << std::endl;
            return;
        }

        // SDL will change the current context when it creates a new one, so we
        // have to get the current one to be able to restore it afterward.
        SDL_Window* oldWin = SDL_GL_GetCurrentWindow();
        SDL_GLContext oldCtx = SDL_GL_GetCurrentContext();

#if defined(ANDROID) || defined(OPENMW_GL4ES_MANUAL_INIT)
        int major = 1;
        int minor = 1;
        char* ver = getenv("OPENMW_GLES_VERSION");

        if (ver && strcmp(ver, "2") == 0)
        {
            major = 2;
            minor = 0;
        }
        else if (ver && strcmp(ver, "3") == 0)
        {
            major = 3;
            minor = 2;
        }

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
#endif

        mContext = SDL_GL_CreateContext(mWindow);
        if (!mContext)
        {
            OSG_FATAL << "Error: Unable to create OpenGL graphics context: " << SDL_GetError() << std::endl;
            return;
        }
        if (mDiagnosticRequested)
        {
#if defined(_WIN32)
            mDiagnosticCreationNative = reinterpret_cast<std::uintptr_t>(wglGetCurrentContext());
#endif
        }

#ifdef OPENMW_GL4ES_MANUAL_INIT
        openmw_gl4es_init(mWindow);
#endif

        setSwapInterval(mVSyncMode);

        // Update traits with what we've actually been given
        // Use intermediate to avoid signed/unsigned mismatch
        int intermediateLocation;
        SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &intermediateLocation);
        _traits->red = intermediateLocation;
        SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &intermediateLocation);
        _traits->green = intermediateLocation;
        SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &intermediateLocation);
        _traits->blue = intermediateLocation;
        SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &intermediateLocation);
        _traits->alpha = intermediateLocation;
        SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &intermediateLocation);
        _traits->depth = intermediateLocation;
        SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &intermediateLocation);
        _traits->stencil = intermediateLocation;

        SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &intermediateLocation);
        _traits->doubleBuffer = intermediateLocation;
        SDL_GL_GetAttribute(SDL_GL_MULTISAMPLEBUFFERS, &intermediateLocation);
        _traits->sampleBuffers = intermediateLocation;
        SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &intermediateLocation);
        _traits->samples = intermediateLocation;

        SDL_GL_MakeCurrent(oldWin, oldCtx);

        mValid = true;

        getEventQueue()->syncWindowRectangleWithGraphicsContext();
    }

    bool GraphicsWindowSDL2::realizeImplementation()
    {
        if (mDiagnosticRequested && (diagnosticCloseState() == DiagnosticCloseState::Closing
            || diagnosticCloseState() == DiagnosticCloseState::DestroyFailed
            || diagnosticCloseState() == DiagnosticCloseState::Destroyed)) return false;
        if (mRealized)
        {
            OSG_NOTICE << "GraphicsWindowSDL2::realizeImplementation() Already realized" << std::endl;
            return true;
        }

        if (!mValid)
            init();
        if (!mValid)
            return false;

        SDL_ShowWindow(mWindow);

        getEventQueue()->syncWindowRectangleWithGraphicsContext();

        mRealized = true;

        return true;
    }

    bool GraphicsWindowSDL2::makeCurrentImplementation()
    {
        if (!mRealized)
        {
            OSG_WARN << "Warning: GraphicsWindow not realized, cannot do makeCurrent." << std::endl;
            return false;
        }

        return SDL_GL_MakeCurrent(mWindow, mContext);
    }

    bool GraphicsWindowSDL2::releaseContextImplementation()
    {
        if (!mRealized)
        {
            OSG_WARN << "Warning: GraphicsWindow not realized, cannot do releaseContext." << std::endl;
            return false;
        }

        return SDL_GL_MakeCurrent(nullptr, nullptr);
    }

    void GraphicsWindowSDL2::closeImplementation()
    {
        if (mDiagnosticRequested)
        {
            using Status = RenderCore::Temporal::DiagnosticNamespaceStatus;
            requestDiagnosticClose();
            // OSG close has stopped its graphics worker. Do not infer that an
            // arbitrary caller has done the same; the caller must join any
            // other native owner before using this main-owner close service.
            if (std::this_thread::get_id() != mDiagnosticNativeOwnerThread) return;
            mValid = false;
            mRealized = false;
            if (mContext)
            {
                const auto destroy = [&]() { return SDL_GL_DestroyContext(mContext); };
                const auto result = mDiagnosticNamespace ? mDiagnosticNamespace->withNativeDestroy(destroy)
                    : (destroy() ? Status::Destroyed : Status::NativeDestroyFailed);
                if (result != Status::Destroyed)
                {
                    mDiagnosticCloseState.store(result == Status::NativeDestroyFailed
                        ? DiagnosticCloseState::DestroyFailed : DiagnosticCloseState::Closing);
                    return; // Retain handle AND window; no native success/free credit.
                }
                mContext = nullptr;
            }
            mDiagnosticCloseState.store(DiagnosticCloseState::Destroyed);
            if (mWindow && mOwnsWindow) SDL_DestroyWindow(mWindow);
            mWindow = nullptr;
            return;
        }
        // Ordinary control behavior remains exactly the existing close path.
        if (mContext)
            SDL_GL_DestroyContext(mContext);
        mContext = nullptr;

        if (mWindow && mOwnsWindow)
            SDL_DestroyWindow(mWindow);
        mWindow = nullptr;

        mValid = false;
        mRealized = false;
    }

    std::optional<RenderCore::Temporal::DiagnosticNamespaceObserver>
    GraphicsWindowSDL2::diagnosticNamespaceObserver() const noexcept
    {
        if (!mDiagnosticNamespace) return std::nullopt;
        return mDiagnosticNamespace->observer();
    }
    RenderCore::Temporal::DiagnosticNativeObservation GraphicsWindowSDL2::diagnosticCurrentObservation() const noexcept
    {
        // Draw observations require the current live OSG association, not a
        // remembered context ID from a discarded or replaced State.
        const auto* state = getState();
        if (!mDiagnosticNamespace || !state || state != mDiagnosticIssuedState.get() || state->getGraphicsContext() != this
            || state->getContextID() != mDiagnosticOsgContext) return {};
#if defined(_WIN32)
        return {reinterpret_cast<std::uintptr_t>(wglGetCurrentContext()), state->getContextID()};
#else
        return {};
#endif
    }
    RenderCore::Temporal::DiagnosticNativeObservation GraphicsWindowSDL2::diagnosticRetirementObservation() const noexcept
    {
        // Only the still-retained native creation owner may retire after OSG
        // close discarded State. This cached association is a closed-owner
        // cleanup identity, never a fresh draw/evaluation observation.
        const auto phase = diagnosticCloseState();
        if (std::this_thread::get_id() != mDiagnosticNativeOwnerThread || !mDiagnosticNamespace || !mContext || !mWindow
            || (phase != DiagnosticCloseState::Closing && phase != DiagnosticCloseState::DestroyFailed)) return {};
        if (const auto* state = getState(); state && (state != mDiagnosticIssuedState.get() || state->getGraphicsContext() != this
            || state->getContextID() != mDiagnosticOsgContext)) return {};
#if defined(_WIN32)
        return {reinterpret_cast<std::uintptr_t>(wglGetCurrentContext()), mDiagnosticOsgContext};
#else
        return {};
#endif
    }
    RenderCore::Temporal::DiagnosticNamespaceSnapshot GraphicsWindowSDL2::diagnosticNamespaceSnapshot() const noexcept
    {
        return mDiagnosticNamespace ? mDiagnosticNamespace->snapshot()
            : RenderCore::Temporal::DiagnosticNamespaceSnapshot{};
    }
    RenderCore::Temporal::DiagnosticNamespaceStatus GraphicsWindowSDL2::requestDiagnosticClose() noexcept
    {
        using Status = RenderCore::Temporal::DiagnosticNamespaceStatus;
        if (!mDiagnosticRequested) return Status::Expired;
        if (diagnosticCloseState() == DiagnosticCloseState::Destroyed) return Status::Destroyed;
        mDiagnosticCloseState.store(DiagnosticCloseState::Closing);
        return mDiagnosticNamespace ? mDiagnosticNamespace->beginClose() : Status::Closing;
    }
    bool GraphicsWindowSDL2::makeDiagnosticCurrentForRetirement() noexcept
    {
        const auto phase = diagnosticCloseState();
        if (!mDiagnosticRequested || std::this_thread::get_id() != mDiagnosticNativeOwnerThread
            || !mDiagnosticNamespace || !mContext || !mWindow
            || (phase != DiagnosticCloseState::Closing && phase != DiagnosticCloseState::DestroyFailed)) return false;
        if (!SDL_GL_MakeCurrent(mWindow, mContext)) return false;
        const auto observed = diagnosticRetirementObservation();
        const auto issued = mDiagnosticNamespace->snapshot();
        return observed.nativeContext == issued.nativeContext && observed.osgContext == issued.osgContext;
    }
    bool GraphicsWindowSDL2::retryDiagnosticNativeClose() noexcept
    {
        if (!mDiagnosticRequested || std::this_thread::get_id() != mDiagnosticNativeOwnerThread) return false;
        closeImplementation();
        return diagnosticCloseState() == DiagnosticCloseState::Destroyed;
    }

    void GraphicsWindowSDL2::swapBuffersImplementation()
    {
        if (!mRealized)
            return;

        SDL_GL_SwapWindow(mWindow);
    }

    void GraphicsWindowSDL2::setSyncToVBlank(bool on)
    {
        throw std::runtime_error(
            "setSyncToVBlank with bool argument is not supported. Use the VSyncMode argument instead.");
    }

    void GraphicsWindowSDL2::setSyncToVBlank(VSyncMode mode)
    {
        SDL_Window* oldWin = SDL_GL_GetCurrentWindow();
        SDL_GLContext oldCtx = SDL_GL_GetCurrentContext();

        SDL_GL_MakeCurrent(mWindow, mContext);

        setSwapInterval(mode);

        SDL_GL_MakeCurrent(oldWin, oldCtx);
    }

    void GraphicsWindowSDL2::setSwapInterval(VSyncMode mode)
    {
        mVSyncMode = mode;

        if (mode == VSyncMode::Adaptive)
        {
            if (!SDL_GL_SetSwapInterval(-1))
            {
                OSG_NOTICE << "Adaptive vsync unsupported" << std::endl;
                setSwapInterval(VSyncMode::Enabled);
            }
        }
        else if (mode == VSyncMode::Enabled)
        {
            if (!SDL_GL_SetSwapInterval(1))
            {
                OSG_NOTICE << "Vertical synchronization unsupported, disabling" << std::endl;
                setSwapInterval(VSyncMode::Disabled);
            }
        }
        else
        {
            SDL_GL_SetSwapInterval(0);
        }
    }

    void GraphicsWindowSDL2::raiseWindow()
    {
        SDL_RaiseWindow(mWindow);
    }

}
