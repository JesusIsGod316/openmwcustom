// Native fixture only: compile the unchanged production GraphicsWindowSDL2
// implementation while observing its real native create/destroy results.
// This is not linked into OpenMW and is not a replacement window-owner model.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <SDL3/SDL.h>

namespace
{
    SDL_GLContext createdContext = nullptr;
    HGLRC createdNative = nullptr;
    HDC createdDc = nullptr;
    DWORD createThread = 0, destroyThread = 0;
    unsigned createCalls = 0, destroyCalls = 0, realDestroyCalls = 0, injectedDestroyFailures = 0;
    bool lastDestroyResult = false, lastDestroyInjected = false, injectNextDestroy = false;
}

SDL_GLContext SDLCALL p9ObservedSdlCreateContext(SDL_Window* window)
{
    const auto context = SDL_GL_CreateContext(window);
    ++createCalls;
    createdContext = context;
    createdNative = wglGetCurrentContext();
    createdDc = wglGetCurrentDC();
    createThread = GetCurrentThreadId();
    return context;
}
bool SDLCALL p9ObservedSdlDestroyContext(SDL_GLContext context)
{
    ++destroyCalls;
    destroyThread = GetCurrentThreadId();
    lastDestroyInjected = injectNextDestroy;
    if (injectNextDestroy)
    {
        injectNextDestroy = false;
        ++injectedDestroyFailures;
        lastDestroyResult = false;
        SDL_SetError("Controlled native fixture failure before real SDL context destruction");
        return false;
    }
    ++realDestroyCalls;
    lastDestroyResult = SDL_GL_DestroyContext(context);
    return lastDestroyResult;
}

SDL_GLContext p9SdlCreatedContext() { return createdContext; }
HGLRC p9SdlCreatedNative() { return createdNative; }
HDC p9SdlCreatedDc() { return createdDc; }
DWORD p9SdlCreateThread() { return createThread; }
DWORD p9SdlDestroyThread() { return destroyThread; }
unsigned p9SdlCreateCalls() { return createCalls; }
unsigned p9SdlDestroyCalls() { return destroyCalls; }
unsigned p9SdlRealDestroyCalls() { return realDestroyCalls; }
unsigned p9SdlInjectedDestroyFailures() { return injectedDestroyFailures; }
bool p9SdlLastDestroyResult() { return lastDestroyResult; }
bool p9SdlLastDestroyInjected() { return lastDestroyInjected; }
void p9SdlInjectNextDestroyFailure() { injectNextDestroy = true; }

// SDK declarations were included above, so the two macros affect only calls
// inside this unchanged production translation unit, not SDL's declarations.
#define SDL_GL_CreateContext p9ObservedSdlCreateContext
#define SDL_GL_DestroyContext p9ObservedSdlDestroyContext
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100) // Retained production setCursor has an unused parameter.
#endif
#include "../../../components/sdlutil/sdlgraphicswindow.cpp"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#undef SDL_GL_DestroyContext
#undef SDL_GL_CreateContext
