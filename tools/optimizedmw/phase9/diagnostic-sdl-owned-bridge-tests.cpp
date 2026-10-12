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
#include "../../../components/sdlutil/sdlgraphicswindow.hpp"
#include "../../../components/sdlutil/externalwindowclose.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

unsigned p9SdlDestroyCalls();
bool p9SdlLastDestroyResult();
bool p9SdlLastDestroyInjected();
void p9SdlInjectNextDestroyFailure();
using namespace RenderCore::Temporal;
namespace
{
    std::atomic_uint checks{0};
    void require(bool value, const char* reason) { ++checks; if (!value) throw std::runtime_error(reason); }
    std::wstring desktopName(HANDLE desktop)
    {
        std::array<wchar_t,256> text{};
        DWORD bytes = 0;
        require(GetUserObjectInformationW(desktop,UOI_NAME,text.data(),sizeof(text),&bytes) != FALSE,
            "Read actual desktop identity");
        return text.data();
    }
}
int main()
{
    SDL_Window* window = nullptr;
    try
    {
        const auto ownDesktop = desktopName(GetThreadDesktop(GetCurrentThreadId()));
        const auto input = OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
        require(input != nullptr,"Read input desktop");
        const auto inputDesktop = desktopName(input);
        CloseDesktop(input);
        require(ownDesktop.starts_with(L"OptimizedMW_Benchmark_") && ownDesktop != inputDesktop,
            "Owned native bridge fixture runs only on an unswitched private desktop");
        SDL_SetMainReady();
        require(SDL_Init(SDL_INIT_VIDEO),"Initialize video only, without audio");
        require(SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_COMPATIBILITY)
            && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3)
            && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3),"Set native GL compatibility attributes");
        window = SDL_CreateWindow("Private SDL-owned namespace bridge",64,64,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
        require(window != nullptr,"Create actual SDL window");
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->width=64;traits->height=64;
        traits->inheritedWindowData = new SDLUtil::GraphicsWindowSDL2::WindowData(window);
        osg::ref_ptr<SDLUtil::GraphicsWindowSDL2> graphics = new SDLUtil::GraphicsWindowSDL2(
            traits,SDLUtil::VSyncMode::Disabled,true);
        require(graphics->valid() && graphics->realize() && graphics->makeCurrent(),"Actual opt-in production window is current on main");
        const auto observer = graphics->diagnosticNamespaceObserver();
        const auto issued = graphics->diagnosticNamespaceSnapshot();
        require(observer && issued.epoch && issued.nativeContext == reinterpret_cast<std::uintptr_t>(wglGetCurrentContext())
            && issued.osgContext == graphics->getState()->getContextID(),"Production SDL owner issued actual native/OSG association");
        require(observer->withExecution(graphics->diagnosticCurrentObservation(),[&](const auto& scope)
        {
            require(scope.current(graphics->diagnosticCurrentObservation()),"Fresh draw observation is current and scoped");
        }) == DiagnosticNamespaceStatus::Executed,"Use production-issued observer without fixture reissuance");
        osg::ref_ptr<osg::State> originalState = graphics->getState();
        const auto originalId = originalState->getContextID();
        originalState->setContextID(originalId+1);
        require(!graphics->diagnosticCurrentObservation().nativeContext,"Fresh observation refuses changed OSG association");
        originalState->setContextID(originalId);
        osg::ref_ptr<osg::State> replacementState = new osg::State;
        replacementState->setContextID(originalId);
        replacementState->setGraphicsContext(graphics);
        graphics->setState(replacementState);
        require(!graphics->diagnosticCurrentObservation().nativeContext,"Same context ID cannot refresh a replaced OSG State");
        graphics->setState(originalState);
        const HGLRC native = wglGetCurrentContext();
        const HDC dc = wglGetCurrentDC();
        GLuint texture = 0;
        glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
        const std::array<unsigned char,4> green{0,255,0,255};
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,green.data());
        require(texture && glIsTexture(texture) && glGetError()==GL_NO_ERROR,"Allocate one exact owned native source");
        observer->withExecution(graphics->diagnosticCurrentObservation(),[&](const auto& scope){scope.setKnownResources(4,1);});
        require(graphics->releaseContext() && !wglGetCurrentContext(),"Main releases before WGL worker ownership");
        std::mutex mutex;
        std::condition_variable cv;
        bool held=false,release=false,mainRefused=false,mainRetained=false;
        std::exception_ptr failure;
        const auto destroyBefore = p9SdlDestroyCalls();
        std::thread draw([&]
        {
            try
            {
                require(wglMakeCurrent(dc,native)!=FALSE,"Worker binds SDL-created namespace using WGL only");
                require(observer->withExecution(graphics->diagnosticCurrentObservation(),[&](const auto& scope)
                {
                    require(scope.current(graphics->diagnosticCurrentObservation()),"Production-issued lifetime permits current worker handoff");
                    std::array<unsigned char,4> pixels{};
                    glBindTexture(GL_TEXTURE_2D,texture);glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
                    require(pixels==green && glGetError()==GL_NO_ERROR,"Worker sees exact original source pixels");
                    std::unique_lock lock(mutex);held=true;cv.notify_one();cv.wait(lock,[&]{return release;});
                })==DiagnosticNamespaceStatus::Executed,"Worker entered real owner execution");
            }
            catch(...){failure=std::current_exception();}
            wglMakeCurrent(nullptr,nullptr);
            std::lock_guard lock(mutex);held=true;cv.notify_one();
        });
        {
            std::unique_lock lock(mutex);cv.wait(lock,[&]{return held;});
            // No throwing assertion until the worker has been released/joined.
            graphics->closeImplementation();
            mainRefused = p9SdlDestroyCalls()==destroyBefore;
            mainRetained = !graphics->diagnosticExternalWindowReleaseAllowed();
            release=true;lock.unlock();cv.notify_one();
        }
        draw.join();if(failure)std::rethrow_exception(failure);
        require(mainRefused && mainRetained,"Active owner prevents actual native destruction and external window release");
        require(!graphics->realize(),"Closed admission cannot recreate over a retained native handle");
        require(graphics->makeDiagnosticCurrentForRetirement(),"Main rebinds retained actual namespace for cleanup");
        require(graphics->retireDiagnosticResources([&](const auto& scope)
        {
            glDeleteTextures(1,&texture);
            require(!glIsTexture(texture) && glGetError()==GL_NO_ERROR,"Current owner retires exact native source");
            require(scope.setKnownResources(0,0),"Credit logical retirement only after real delete");
        })==DiagnosticNamespaceStatus::Executed,"Explicit closed-owner retirement succeeds");
        p9SdlInjectNextDestroyFailure();
        graphics->close(true);
        require(p9SdlLastDestroyInjected() && !p9SdlLastDestroyResult()
            && graphics->diagnosticCloseState()==SDLUtil::GraphicsWindowSDL2::DiagnosticCloseState::DestroyFailed,
            "Synthetic SDK false leaves explicit failed native close authority");
        require(!graphics->getState() && !graphics->diagnosticCurrentObservation().nativeContext
            && !graphics->diagnosticExternalWindowReleaseAllowed(),"Discarded OSG State is not fresh draw provenance or external-window release permission");
        require(graphics->makeDiagnosticCurrentForRetirement(),"Closed native owner survives OSG State removal without refreshing draw association");
        require(graphics->retireDiagnosticResources([](const auto&){})==DiagnosticNamespaceStatus::Executed,
            "Cached original association is allowed only for explicit closed-owner retirement");
        require(graphics->retryDiagnosticNativeClose() && !p9SdlLastDestroyInjected(),"Actual SDK retry completes retained native close");
        require(graphics->diagnosticNamespaceSnapshot().phase==DiagnosticNamespacePhase::Destroyed
            && graphics->diagnosticExternalWindowReleaseAllowed() && !wglGetCurrentContext(),"Only confirmed actual death releases external-window permission");
        const auto finalCalls=p9SdlDestroyCalls();
        require(graphics->retryDiagnosticNativeClose() && p9SdlDestroyCalls()==finalCalls,"Duplicate close never invokes native destruction twice");
        graphics=nullptr;SDL_DestroyWindow(window);window=nullptr;

        // Exercise the same external-owner service called by Engine, including
        // OSG State discard, an actual native texture debt and SDK false retry.
        SDLUtil::ExternalWindowCloseAuthority external;
        window=SDL_CreateWindow("Private engine external close authority",64,64,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
        require(window!=nullptr,"Create actual second external SDL window");
        const auto windowId=SDL_GetWindowID(window);
        traits->inheritedWindowData=new SDLUtil::GraphicsWindowSDL2::WindowData(window);
        graphics=new SDLUtil::GraphicsWindowSDL2(traits,SDLUtil::VSyncMode::Disabled,true);
        external.attach(window,graphics);
        require(external.ownsContext(graphics) && graphics->realize() && graphics->makeCurrent(),
            "Engine external owner pins the actual opt-in SDL creation");
        require(!external.destroyExternalWindow(window) && window && !external.subsystemReleaseAllowed(),
            "External window/subsystem release is denied before actual owner join");
        std::atomic_bool wrongCalled{false};bool wrongStatus=true;
        std::thread wrong([&] {wrongStatus=external.stopAdmissionsAndJoin([&]{wrongCalled=true;});});wrong.join();
        require(!wrongStatus && !wrongCalled,"Wrong-thread caller cannot manufacture an owner join receipt");
        GLuint pendingTexture=0;glGenTextures(1,&pendingTexture);glBindTexture(GL_TEXTURE_2D,pendingTexture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,green.data());
        const auto ownedObserver=graphics->diagnosticNamespaceObserver();
        require(pendingTexture && glIsTexture(pendingTexture) && ownedObserver
            && ownedObserver->withExecution(graphics->diagnosticCurrentObservation(),
                [](const auto& scope){scope.setKnownResources(4,0);})==DiagnosticNamespaceStatus::Executed,
            "Actual importer-like texture allocation is declared before close");
        bool joinFailed=false;
        try {external.stopAdmissionsAndJoin([]{throw std::runtime_error("native join injection");});}
        catch(const std::runtime_error&) {joinFailed=true;}
        require(joinFailed && !external.closeJoined() && !external.destroyExternalWindow(window),
            "Failed actual join callback grants no native/window shutdown permission");
        unsigned joins=0;
        require(external.stopAdmissionsAndJoin([&]{++joins;}) && joins==1,
            "External owner invokes and completes its actual join callback before close");
        const auto pendingDestroyBefore=p9SdlDestroyCalls();
        require(!external.closeJoined() && p9SdlDestroyCalls()==pendingDestroyBefore
            && !graphics->getState() && window && SDL_GetWindowFromID(windowId)==window,
            "Actual resource debt survives OSG discard and prevents native/external window destruction");
        require(graphics->makeDiagnosticCurrentForRetirement()
            && graphics->retireDiagnosticResources([&](const auto& scope)
            {glDeleteTextures(1,&pendingTexture);require(!glIsTexture(pendingTexture),"Actual pending texture deleted before credit");scope.setKnownResources(0,0);})
                ==DiagnosticNamespaceStatus::Executed,
            "Retained original native owner retires resources independently of discarded OSG State");
        p9SdlInjectNextDestroyFailure();
        require(!external.retryCloseJoined() && !external.destroyExternalWindow(window)
            && !external.subsystemReleaseAllowed() && window && SDL_GetWindowFromID(windowId)==window,
            "SDK false retains the real external window and subsystem authority");
        require(external.retryCloseJoined() && external.subsystemReleaseAllowed(),
            "Explicit finite actual native retry qualifies SDL subsystem release");
        require(external.destroyExternalWindow(window) && !window && !SDL_GetWindowFromID(windowId),
            "Only confirmed native death allows actual Engine external window destruction");
        graphics=nullptr;
        std::cout<<"PASS production SDL-owned lifetime bridge: "<<checks.load()<<" assertions; real namespace/current/closed-owner cleanup only\n";
        std::cout<<"LIMITATIONS: synthetic false destroy/join throw are fault injection. Actual Engine service and native owners exercised; no game import/NGX/jitter/dense motion/performance proof.\n";
        SDL_Quit();return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr<<"FAIL production SDL-owned bridge: "<<error.what()<<"; SDL="<<SDL_GetError()<<'\n';
        // A failed assertion may leave real importer/native-close debt. Do not
        // manufacture cleanup by destroying its external window/SDL subsystem.
        // This finite fixture exits; the OS reclaims its failed process.
        return 1;
    }
}
