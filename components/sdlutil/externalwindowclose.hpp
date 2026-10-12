#ifndef OPENMW_COMPONENTS_SDLUTIL_EXTERNALWINDOWCLOSE_H
#define OPENMW_COMPONENTS_SDLUTIL_EXTERNALWINDOWCLOSE_H

#include "sdlgraphicswindow.hpp"
#include <vector>
#include <stdexcept>
#include <utility>

namespace SDLUtil
{
    // Opt-in external SDL owner used by Engine's MSAA retry and final teardown.
    // This authority never zeros importer/fence debt or infers native death
    // from OSG State discard. All operations belong to the creation thread.
    class ExternalWindowCloseAuthority final
    {
    public:
        ExternalWindowCloseAuthority() = default;
        ExternalWindowCloseAuthority(const ExternalWindowCloseAuthority&) = delete;
        ExternalWindowCloseAuthority& operator=(const ExternalWindowCloseAuthority&) = delete;
        void attach(SDL_Window* window,GraphicsWindowSDL2* graphics)
        {
            if (!onOwner() || !window || !graphics || !graphics->diagnosticNamespaceRequested() || mPins.size()>=32)
                throw std::logic_error("Invalid external window close owner");
            const auto* traits=graphics->getTraits();
            const auto* data=traits?dynamic_cast<const GraphicsWindowSDL2::WindowData*>(traits->inheritedWindowData.get()):nullptr;
            if (!data || data->mWindow!=window) throw std::logic_error("External window does not match actual native creation");
            if (mJoined && !subsystemReleaseAllowed())
                throw std::logic_error("Cannot replace an unclosed native window");
            mPins.push_back({window,graphics});mJoined=false;
        }
        template<class Join>
        bool stopAdmissionsAndJoin(Join&& join)
        {
            if (!onOwner()) return false;
            for (const auto& pin:mPins) pin.graphics->requestDiagnosticClose();
            // Joined is published only after the actual owner callback returns.
            // An exception or wrong-thread call cannot grant release authority.
            mJoined=false;std::forward<Join>(join)();mJoined=true;return true;
        }
        bool closeJoined()
        {
            if (!onOwner() || !mJoined) return false;
            for (const auto& pin:mPins)
                if (!pin.graphics->diagnosticExternalWindowReleaseAllowed()) pin.graphics->close(true);
            return subsystemReleaseAllowed();
        }
        bool retryCloseJoined()
        {
            if (!onOwner() || !mJoined) return false;
            for (const auto& pin:mPins)
                if (!pin.graphics->diagnosticExternalWindowReleaseAllowed()) pin.graphics->retryDiagnosticNativeClose();
            return subsystemReleaseAllowed();
        }
        bool windowReleaseAllowed(SDL_Window* window) const noexcept
        {
            if (!onOwner() || !mJoined) return false;
            bool found=!window;
            for (const auto& pin:mPins)
                if (pin.window==window)
                {found=true;if (!pin.graphics->diagnosticExternalWindowReleaseAllowed()) return false;}
            return found;
        }
        bool destroyExternalWindow(SDL_Window*& window) const noexcept
        {
            if (!windowReleaseAllowed(window)) return false;
            if (window) SDL_DestroyWindow(window);
            window=nullptr;return true;
        }
        bool subsystemReleaseAllowed() const noexcept
        {
            if (!onOwner() || !mJoined) return false;
            for (const auto& pin:mPins) if (!pin.graphics->diagnosticExternalWindowReleaseAllowed()) return false;
            return true;
        }
        bool ownsContext(const osg::GraphicsContext* context) const noexcept
        {
            if (!onOwner()) return false;
            for (const auto& pin:mPins) if (pin.graphics.get()==context) return true;
            return false;
        }
        ~ExternalWindowCloseAuthority()
        {
            // Terminal SDK refusal is exceptional. Retain each actual native
            // authority through process exit rather than letting a destructor
            // erase it, then destroying its window/SDL subsystem underneath it.
            // ref() needs no emergency allocation; no free/death credit is
            // reported. The caller must log its refused teardown explicitly.
            for (const auto& pin:mPins)
                if (!pin.graphics->diagnosticExternalWindowReleaseAllowed()) pin.graphics->ref();
        }
    private:
        bool onOwner() const noexcept { return std::this_thread::get_id()==mOwner; }
        const std::thread::id mOwner=std::this_thread::get_id();
        struct Pin { SDL_Window* window;osg::ref_ptr<GraphicsWindowSDL2> graphics; };
        std::vector<Pin> mPins;
        bool mJoined=false;
    };
}
#endif
