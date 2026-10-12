#ifndef OPENMW_COMPONENTS_SDLUTIL_SDLGRAPHICSWINDOW_H
#define OPENMW_COMPONENTS_SDLUTIL_SDLGRAPHICSWINDOW_H

#include <SDL3/SDL.h>

#include <osgViewer/GraphicsWindow>

#include "vsyncmode.hpp"
#include <components/rendercore/diagnosticnamespace.hpp>

namespace SDLUtil
{

    class GraphicsWindowSDL2 : public osgViewer::GraphicsWindow
    {
        SDL_Window* mWindow;
        SDL_GLContext mContext;

        bool mValid;
        bool mRealized;
        bool mOwnsWindow;
        VSyncMode mVSyncMode;
        const bool mDiagnosticRequested;
        const std::thread::id mDiagnosticNativeOwnerThread = std::this_thread::get_id();
        std::uintptr_t mDiagnosticCreationNative = 0;
        unsigned mDiagnosticOsgContext = 0;
        osg::observer_ptr<osg::State> mDiagnosticIssuedState;
        std::unique_ptr<RenderCore::Temporal::DiagnosticNamespace> mDiagnosticNamespace;

        void init();

        virtual ~GraphicsWindowSDL2();

    public:
        enum class DiagnosticCloseState { NotRequested, Open, Closing, DestroyFailed, Destroyed, Unsupported };
        GraphicsWindowSDL2(osg::GraphicsContext::Traits* traits, VSyncMode vsyncMode,
            bool diagnosticNamespaceRequested = false);
        std::optional<RenderCore::Temporal::DiagnosticNamespaceObserver> diagnosticNamespaceObserver() const noexcept;
        RenderCore::Temporal::DiagnosticNativeObservation diagnosticCurrentObservation() const noexcept;
        RenderCore::Temporal::DiagnosticNamespaceSnapshot diagnosticNamespaceSnapshot() const noexcept;
        RenderCore::Temporal::DiagnosticNamespaceStatus requestDiagnosticClose() noexcept;
        // Main native owner only, after its render workers have stopped/joined.
        // These operate independently of OSG State, which close() may discard.
        bool makeDiagnosticCurrentForRetirement() noexcept;
        template <class Consumer>
        RenderCore::Temporal::DiagnosticNamespaceStatus retireDiagnosticResources(Consumer&& consumer)
        {
            if (!mDiagnosticNamespace || std::this_thread::get_id() != mDiagnosticNativeOwnerThread)
                return RenderCore::Temporal::DiagnosticNamespaceStatus::WrongThread;
            return mDiagnosticNamespace->withRetirement(diagnosticRetirementObservation(), std::forward<Consumer>(consumer));
        }
        bool retryDiagnosticNativeClose() noexcept;
        DiagnosticCloseState diagnosticCloseState() const noexcept { return mDiagnosticCloseState.load(); }
        bool diagnosticExternalWindowReleaseAllowed() const noexcept
        { return !mDiagnosticRequested || diagnosticCloseState() == DiagnosticCloseState::Destroyed; }
        bool diagnosticNamespaceRequested() const noexcept { return mDiagnosticRequested; }

        bool isSameKindAs(const Object* object) const override
        {
            return dynamic_cast<const GraphicsWindowSDL2*>(object) != nullptr;
        }
        const char* libraryName() const override { return "osgViewer"; }
        const char* className() const override { return "GraphicsWindowSDL2"; }

        bool valid() const override { return mValid; }

        /** Realise the GraphicsContext.*/
        bool realizeImplementation() override;

        /** Return true if the graphics context has been realised and is ready to use.*/
        bool isRealizedImplementation() const override { return mRealized; }

        /** Close the graphics context.*/
        void closeImplementation() override;

        /** Make this graphics context current.*/
        bool makeCurrentImplementation() override;

        /** Release the graphics context.*/
        bool releaseContextImplementation() override;

        /** Swap the front and back buffers.*/
        void swapBuffersImplementation() override;

        /** Set sync-to-vblank. */
        void setSyncToVBlank(bool on) override;
        void setSyncToVBlank(VSyncMode mode);

        /** Set Window decoration.*/
        bool setWindowDecorationImplementation(bool flag) override;

        /** Raise specified window */
        void raiseWindow() override;

        /** Set the window's position and size.*/
        bool setWindowRectangleImplementation(int x, int y, int width, int height) override;

        /** Set the name of the window */
        void setWindowName(const std::string& name) override;

        /** Set mouse cursor to a specific shape.*/
        void setCursor(MouseCursor cursor) override;

        /** Get focus.*/
        void grabFocus() override {}

        /** Get focus on if the pointer is in this window.*/
        void grabFocusIfPointerInWindow() override {}

        /** WindowData is used to pass in the SDL2 window handle attached to the GraphicsContext::Traits structure. */
        struct WindowData : public osg::Referenced
        {
            WindowData(SDL_Window* window)
                : mWindow(window)
            {
            }

            SDL_Window* mWindow;
        };

    private:
        RenderCore::Temporal::DiagnosticNativeObservation diagnosticRetirementObservation() const noexcept;
        std::atomic<DiagnosticCloseState> mDiagnosticCloseState{DiagnosticCloseState::NotRequested};
        void setSwapInterval(VSyncMode mode);
    };

}

#endif /* OSGGRAPHICSWINDOW_H */
