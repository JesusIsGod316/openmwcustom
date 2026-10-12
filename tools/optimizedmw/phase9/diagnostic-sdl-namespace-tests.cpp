#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define SDL_MAIN_HANDLED
#include <windows.h>
#include <GL/gl.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "../../../components/rendercore/diagnosticnamespace.hpp"
#include "../../../components/sdlutil/sdlgraphicswindow.hpp"

#include <array>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

SDL_GLContext p9SdlCreatedContext();
HGLRC p9SdlCreatedNative();
HDC p9SdlCreatedDc();
DWORD p9SdlCreateThread();
DWORD p9SdlDestroyThread();
unsigned p9SdlCreateCalls();
unsigned p9SdlDestroyCalls();
unsigned p9SdlRealDestroyCalls();
unsigned p9SdlInjectedDestroyFailures();
bool p9SdlLastDestroyResult();
bool p9SdlLastDestroyInjected();
void p9SdlInjectNextDestroyFailure();
bool SDLCALL p9ObservedSdlDestroyContext(SDL_GLContext);

using namespace RenderCore::Temporal;
namespace
{
    constexpr unsigned Width = 64, Height = 64;
    unsigned checks = 0, draws = 0, handoffs = 0;
    const DWORD mainThread = GetCurrentThreadId();
    void require(bool value, const char* description)
    {
        ++checks;
        if (!value) throw std::runtime_error(description);
    }
    std::wstring desktopName(HANDLE desktop)
    {
        std::array<wchar_t, 256> result{};
        DWORD used = 0;
        require(GetUserObjectInformationW(desktop, UOI_NAME, result.data(), sizeof(result), &used),
            "Read native desktop identity");
        return result.data();
    }
    void privateDesktopRequired()
    {
        const auto own = desktopName(GetThreadDesktop(GetCurrentThreadId()));
        const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        require(input != nullptr, "Read active input desktop");
        const auto inputName = desktopName(input);
        CloseDesktop(input);
        require(own.starts_with(L"OptimizedMW_Benchmark_") && own != inputName,
            "Native fixture must start on the unswitched private benchmark desktop");
    }
    class ActualWindow final
    {
    public:
        ActualWindow()
        {
            require(GetCurrentThreadId() == mainThread, "Only main creates SDL objects");
            require(SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY)
                && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3)
                && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3)
                && SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1)
                && SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24)
                && SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8), "Request compatible fixture GL attributes");
            window = SDL_CreateWindow("OptimizedMW private native lifecycle", Width, Height,
                SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
            require(window != nullptr, "Create actual SDL OpenGL window on private desktop");
            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            traits->width = Width;
            traits->height = Height;
            traits->doubleBuffer = true;
            traits->inheritedWindowData = new SDLUtil::GraphicsWindowSDL2::WindowData(window);
            graphics = new SDLUtil::GraphicsWindowSDL2(traits.get(), SDLUtil::VSyncMode::Disabled);
            require(graphics->valid() && graphics->getState(), "Unchanged production graphics window initializes");
            require(graphics->realize() && graphics->makeCurrent(), "Production main-thread realize and current bind succeed");
            native = wglGetCurrentContext();
            dc = wglGetCurrentDC();
            context = SDL_GL_GetCurrentContext();
            osgContext = graphics->getState()->getContextID();
            require(native && dc && context, "Native current WGL and SDL identities observed independently");
            require(native == p9SdlCreatedNative() && dc == p9SdlCreatedDc()
                && context == p9SdlCreatedContext() && p9SdlCreateThread() == mainThread,
                "Production SDL creation witness binds actual WGL identity, not an opaque handle reinterpretation");
            authority = DiagnosticNamespace::issue(observation());
            require(authority.has_value(), "Actual native creation owner issues a unique diagnostic epoch");
        }
        ~ActualWindow() noexcept
        {
            // Failure unwind is not an accepted retirement receipt. The native
            // fixture owns this window; no user/game instance is touched.
            if (graphics) graphics->close(true);
            graphics = nullptr;
            if (window) SDL_DestroyWindow(window);
        }
        ActualWindow(const ActualWindow&) = delete;
        DiagnosticNativeObservation observation() const
        {
            return {reinterpret_cast<std::uintptr_t>(wglGetCurrentContext()), osgContext};
        }
        void mainBind()
        {
            require(GetCurrentThreadId() == mainThread && graphics->makeCurrent(), "Production SDL bind stays on main");
            require(wglGetCurrentContext() == native && wglGetCurrentDC() == dc,
                "Actual main current namespace matches issued owner");
        }
        void mainRelease()
        {
            require(GetCurrentThreadId() == mainThread && graphics->releaseContext(), "Production SDL release stays on main");
            require(!wglGetCurrentContext(), "Main is unbound before native worker handoff");
        }
        SDL_Window* window = nullptr;
        osg::ref_ptr<SDLUtil::GraphicsWindowSDL2> graphics;
        std::optional<DiagnosticNamespace> authority;
        HGLRC native = nullptr;
        HDC dc = nullptr;
        SDL_GLContext context = nullptr;
        unsigned osgContext = 0;
    };
    GLuint texture(std::array<unsigned char, 4> color)
    {
        std::vector<unsigned char> pixels(Width * Height * 4);
        for (unsigned i = 0; i < Width * Height; ++i)
            for (unsigned component = 0; component < 4; ++component) pixels[i * 4 + component] = color[component];
        GLuint result = 0;
        glGenTextures(1, &result);
        glBindTexture(GL_TEXTURE_2D, result);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Width, Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        require(result && glIsTexture(result) && glGetError() == GL_NO_ERROR, "Allocate exact owned native fixture texture");
        return result;
    }
    void ordinaryDraw(GLuint source, std::array<unsigned char, 4> expected)
    {
        require(wglGetCurrentContext() && glIsTexture(source), "Ordinary native draw has current source owner");
        glViewport(0, 0, Width, Height);
        glDrawBuffer(GL_BACK);
        glReadBuffer(GL_BACK);
        glDisable(GL_DITHER);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_LIGHTING);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(1.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, source);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.f, 0.f); glVertex2f(-1.f, -1.f);
        glTexCoord2f(1.f, 0.f); glVertex2f(1.f, -1.f);
        glTexCoord2f(0.f, 1.f); glVertex2f(-1.f, 1.f);
        glTexCoord2f(1.f, 1.f); glVertex2f(1.f, 1.f);
        glEnd();
        std::array<unsigned char, 4> actual{};
        glReadPixels(Width / 2, Height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
        require(actual == expected && glGetError() == GL_NO_ERROR, "Following ordinary fixture draw retains expected pixels");
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
        ++draws;
    }
    void drawScope(ActualWindow& window, GLuint source, std::array<unsigned char, 4> expected)
    {
        require(window.authority->observer().withExecution(window.observation(), [&](const auto& execution)
        {
            require(execution.current(window.observation()), "Scoped current proof uses fresh WGL observation on its callback thread");
            require(window.authority->observer().withExecution(window.observation(), [](const auto&) {})
                == DiagnosticNamespaceStatus::Busy, "Recursive native draw ownership is refused without waiting");
            ordinaryDraw(source, expected);
        }) == DiagnosticNamespaceStatus::Executed, "Admit actual current native draw scope");
    }
    void workerDraw(ActualWindow& window, GLuint source, std::array<unsigned char, 4> expected, bool holdForMain)
    {
        window.mainRelease();
        std::mutex mutex;
        std::condition_variable cv;
        bool held = false, inspected = false;
        bool mainUnboundWhileHeld = false;
        DiagnosticNamespaceStatus missingMainStatus = DiagnosticNamespaceStatus::Executed;
        std::exception_ptr failure;
        std::thread worker([&]
        {
            try
            {
                require(GetCurrentThreadId() != mainThread && !wglGetCurrentContext(), "Worker begins without a native context");
                require(wglMakeCurrent(window.dc, window.native) != FALSE, "Windows WGL moves the released native namespace to worker");
                require(wglGetCurrentContext() == window.native && wglGetCurrentDC() == window.dc,
                    "Worker owns exact SDL-created WGL namespace and borrowed device context");
                if (holdForMain)
                {
                    std::unique_lock lock(mutex);
                    held = true;
                    cv.notify_one();
                    cv.wait(lock, [&] { return inspected; });
                }
                drawScope(window, source, expected);
                require(wglMakeCurrent(nullptr, nullptr) != FALSE && !wglGetCurrentContext(),
                    "Worker explicitly releases current namespace before ending and joining");
            }
            catch (...) { failure = std::current_exception(); wglMakeCurrent(nullptr, nullptr); }
            if (holdForMain)
            {
                std::lock_guard lock(mutex);
                held = true;
                cv.notify_one();
            }
        });
        if (holdForMain)
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return held; });
            // Collect observations without throwing while the worker waits.
            // Assertions occur after release/join so fixture failure cannot
            // strand a joined-context handoff or invoke std::terminate.
            mainUnboundWhileHeld = !wglGetCurrentContext();
            missingMainStatus = window.authority->observer().withExecution(window.observation(), [](const auto&) {});
            inspected = true;
            lock.unlock();
            cv.notify_one();
        }
        worker.join();
        if (failure) std::rethrow_exception(failure);
        if (holdForMain)
        {
            require(mainUnboundWhileHeld, "Main never binds while worker owns the same namespace");
            require(missingMainStatus == DiagnosticNamespaceStatus::WrongContext,
                "Fresh main WGL observation refuses a noncurrent namespace");
        }
        require(!wglGetCurrentContext(), "Main remains unbound after worker joined");
        ++handoffs;
        window.mainBind();
    }
    void retire(ActualWindow& window, GLuint& source)
    {
        require(window.authority->beginClose() == DiagnosticNamespaceStatus::Closing,
            "Actual native owner freezes all diagnostic admission before close");
        unsigned nativeCalls = 0;
        require(window.authority->withNativeDestroy([&] { ++nativeCalls; return true; })
            == DiagnosticNamespaceStatus::PendingRetirement && !nativeCalls,
            "Pending owned native storage prevents platform destruction callback");
        require(window.authority->withRetirement(window.observation(), [&](const auto& execution)
        {
            require(execution.current(window.observation()), "Main retirement has actual current native namespace");
            glDeleteTextures(1, &source);
            require(!glIsTexture(source) && glGetError() == GL_NO_ERROR, "Real GL deletion retires the exact owned texture");
            source = 0;
            require(execution.setKnownResources(0, 0), "Credit logical retirement only after actual delete succeeds");
        }) == DiagnosticNamespaceStatus::Executed, "Run orderly current-context retirement on main");
    }
    DiagnosticNamespaceStatus closeThroughProduction(ActualWindow& window)
    {
        return window.authority->withNativeDestroy([&]
        {
            require(GetCurrentThreadId() == mainThread, "Actual SDL context destruction stays on main");
            const auto before = p9SdlDestroyCalls();
            window.graphics->close(true);
            require(p9SdlDestroyCalls() == before + 1 && p9SdlDestroyThread() == mainThread,
                "Unchanged production close made one observed SDL destroy operation");
            return p9SdlLastDestroyResult();
        });
    }
}

int main()
{
    try
    {
        privateDesktopRequired();
        SDL_SetMainReady();
        require(SDL_Init(SDL_INIT_VIDEO), "Initialize SDL video only, without audio");
        std::optional<DiagnosticNamespaceObserver> oldObserver;
        DiagnosticNamespaceSnapshot oldIdentity;
        GLuint oldTexture = 0;
        {
            ActualWindow window;
            const auto vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
            const auto renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            const auto version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
            require(vendor && renderer && version, "Observe actual native renderer identity");
            std::cout << "NATIVE vendor=" << vendor << " renderer=" << renderer << " version=" << version << '\n';
            const std::array<unsigned char, 4> green{0, 255, 0, 255};
            GLuint source = texture(green);
            oldTexture = source;
            oldObserver = window.authority->observer();
            oldIdentity = window.authority->snapshot();
            window.authority->observer().withExecution(window.observation(), [&](const auto& execution)
            {
                require(execution.setKnownResources(Width * Height * 4, 1), "Charge actual requested fixture storage");
            });
            drawScope(window, source, green);
            workerDraw(window, source, green, true);
            drawScope(window, source, green);
            workerDraw(window, source, green, false);
            drawScope(window, source, green);
            window.mainRelease();
            SetLastError(0);
            const auto failedBind = wglMakeCurrent(nullptr, window.native);
            const auto bindError = GetLastError();
            std::cout << "NATIVE failed_null_dc_bind_result=" << failedBind << " error=" << bindError << '\n';
            require(!failedBind && !wglGetCurrentContext(), "Real invalid-DC WGL bind fails and caller remains unbound");
            require(window.authority->observer().withExecution(window.observation(), [](const auto&) {})
                == DiagnosticNamespaceStatus::WrongContext, "Failed native bind cannot issue an execution lease");
            window.mainBind();
            drawScope(window, source, green);
            retire(window, source);
            require(closeThroughProduction(window) == DiagnosticNamespaceStatus::Destroyed
                && !p9SdlLastDestroyInjected(), "Real SDL destroy result, not void wrapper completion, confirms death");
            require(!wglGetCurrentContext() && !SDL_GL_GetCurrentContext() && !window.graphics->valid(),
                "Successful actual production close removes the current native namespace");
            const auto destroyCount = p9SdlDestroyCalls();
            require(window.authority->withNativeDestroy([&] { window.graphics->close(true); return true; })
                == DiagnosticNamespaceStatus::Destroyed && p9SdlDestroyCalls() == destroyCount,
                "Duplicate diagnostic release does not call the native API again");
        }
        {
            ActualWindow replacement;
            const std::array<unsigned char, 4> blue{0, 0, 255, 255};
            GLuint source = texture(blue);
            const auto next = replacement.authority->snapshot();
            require(next.epoch != oldIdentity.epoch, "Actual native recreation gets a fresh issuer epoch");
            std::cout << "NATIVE reuse_native=" << (next.nativeContext == oldIdentity.nativeContext)
                      << " reuse_osg=" << (next.osgContext == oldIdentity.osgContext)
                      << " reuse_texture_name=" << (source == oldTexture)
                      << " old_epoch=" << oldIdentity.epoch << " new_epoch=" << next.epoch << '\n';
            const auto stale = oldObserver->withExecution(replacement.observation(), [](const auto&) {});
            require(stale == DiagnosticNamespaceStatus::Expired || stale == DiagnosticNamespaceStatus::Destroyed
                || stale == DiagnosticNamespaceStatus::WrongContext, "Old actual lifetime observer cannot refresh into recreated handles");
            replacement.authority->observer().withExecution(replacement.observation(), [&](const auto& execution)
            {
                execution.setKnownResources(Width * Height * 4, 1);
            });
            drawScope(replacement, source, blue);
            retire(replacement, source);
            require(closeThroughProduction(replacement) == DiagnosticNamespaceStatus::Destroyed,
                "Recreated actual namespace closes independently");
        }
        {
            ActualWindow fault;
            const auto actualContext = fault.context;
            const auto actualNative = fault.native;
            GLuint source = texture({255, 255, 0, 255});
            fault.authority->observer().withExecution(fault.observation(), [&](const auto& execution)
            {
                execution.setKnownResources(Width * Height * 4, 1);
            });
            retire(fault, source);
            p9SdlInjectNextDestroyFailure();
            require(closeThroughProduction(fault) == DiagnosticNamespaceStatus::NativeDestroyFailed
                && p9SdlLastDestroyInjected(), "Synthetic pre-call false destruction gets no native-death credit");
            require(fault.authority->snapshot().phase == DiagnosticNamespacePhase::Closing
                && fault.authority->snapshot().destructionFailures == 1, "False destruction leaves diagnostic admission closed");
            require(!fault.graphics->valid(), "False native result still invalidated the unchanged production class");
            // OSG close releases current before closeImplementation. Test the
            // externally retained real creation handle on main rather than
            // assuming a failed destroy left it current.
            require(SDL_GL_MakeCurrent(fault.window, actualContext)
                && wglGetCurrentContext() == actualNative && SDL_GL_GetCurrentContext() == actualContext,
                "Known limitation: unchanged class discarded its internal handle despite a still-live real native context");
            require(fault.authority->observer().withExecution(fault.observation(), [](const auto&) {})
                == DiagnosticNamespaceStatus::Closing, "Live orphaned native handle cannot reopen diagnostic admission");
            require(fault.authority->withNativeDestroy([&]
            {
                require(GetCurrentThreadId() == mainThread, "Synthetic-fault cleanup stays on main");
                return p9ObservedSdlDestroyContext(actualContext);
            }) == DiagnosticNamespaceStatus::Destroyed && !p9SdlLastDestroyInjected(),
                "Fixture-retained creation handle receives actual SDK cleanup after synthetic failure");
            require(!wglGetCurrentContext() && !SDL_GL_GetCurrentContext(), "No real orphaned context remains after fixture recovery");
        }
        std::cout << "PASS Windows native namespace lifecycle: " << checks << " assertions, " << draws
                  << " ordinary fixture draws, " << handoffs << " joined WGL worker handoffs, "
                  << p9SdlCreateCalls() << " actual SDL context creates, " << p9SdlRealDestroyCalls()
                  << " actual SDL destroy calls, " << p9SdlInjectedDestroyFailures() << " synthetic pre-call destroy failures\n";
        std::cout << "LIMITATIONS: SDL calls were main-thread-only; WGL worker migration is Windows-only. "
                  << "The unchanged class loses its handle on synthetic destroy false; this is not production failure recovery. "
                  << "No game import, temporal producer, NGX, jitter, dense motion or performance result.\n";
        SDL_Quit();
        return 0;
    }
    catch (const std::exception& failure)
    {
        std::cerr << "FAIL native namespace fixture after " << checks << " assertions: " << failure.what()
                  << "; SDL=" << SDL_GetError() << '\n';
        SDL_Quit();
        return 1;
    }
}
