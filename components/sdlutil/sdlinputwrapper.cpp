#include "sdlinputwrapper.hpp"

#include <components/debug/debuglog.hpp>
#include <components/settings/values.hpp>

#include <osgViewer/Viewer>

namespace SDLUtil
{

    InputWrapper::InputWrapper(SDL_Window* window, osg::ref_ptr<osgViewer::Viewer> viewer, bool grab)
        : mSDLWindow(window)
        , mViewer(std::move(viewer))
        , mMouseListener(nullptr)
        , mSensorListener(nullptr)
        , mKeyboardListener(nullptr)
        , mWindowListener(nullptr)
        , mConListener(nullptr)
        , mWarpX(0)
        , mWarpY(0)
        , mWarpCompensate(false)
        , mWrapPointer(false)
        , mAllowGrab(grab)
        , mWantMouseVisible(false)
        , mWantGrab(false)
        , mWantRelative(false)
        , mGrabPointer(false)
        , mMouseRelative(false)
        , mFirstMouseMove(true)
        , mMouseZ(0)
        , mMouseX(0)
        , mMouseY(0)
        , mPendingWheelY(0.0)
        , mWindowHasFocus(true)
        , mMouseInWindow(true)
    {
        const SDL_WindowFlags flags = SDL_GetWindowFlags(mSDLWindow);
        mWindowHasFocus = (flags & SDL_WINDOW_INPUT_FOCUS);
        mMouseInWindow = (flags & SDL_WINDOW_MOUSE_FOCUS);
        _setWindowScale();
    }

    InputWrapper::~InputWrapper() = default;

    void InputWrapper::_setWindowScale()
    {
        const float density = SDL_GetWindowPixelDensity(mSDLWindow);
        const float scale = density > 0.f ? density : 1.f;
        mScaleX = scale;
        mScaleY = scale;
    }

    void InputWrapper::capture(bool windowEventsOnly)
    {
        mViewer->getEventQueue()->frame(0.f);

        SDL_PumpEvents();

        SDL_Event evt;

        if (windowEventsOnly)
        {
            // During loading, handle window events, discard button presses and mouse movement and keep others for later
            while (SDL_PeepEvents(&evt, 1, SDL_GETEVENT, SDL_EVENT_WINDOW_FIRST, SDL_EVENT_WINDOW_LAST) > 0)
                handleWindowEvent(evt);

            SDL_FlushEvent(SDL_EVENT_KEY_DOWN);
            SDL_FlushEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            SDL_FlushEvent(SDL_EVENT_MOUSE_BUTTON_DOWN);
            SDL_FlushEvent(SDL_EVENT_MOUSE_MOTION);
            SDL_FlushEvent(SDL_EVENT_MOUSE_WHEEL);

            return;
        }

        while (SDL_PollEvent(&evt))
        {
            if (evt.type >= SDL_EVENT_WINDOW_FIRST && evt.type <= SDL_EVENT_WINDOW_LAST)
            {
                handleWindowEvent(evt);
                continue;
            }

            switch (evt.type)
            {
                case SDL_EVENT_MOUSE_MOTION:
                    // Ignore this if it happened due to a warp
                    if (!_handleWarpMotion(evt.motion))
                    {
                        // If in relative mode, don't trigger events unless window has focus
                        if (!mWantRelative || mWindowHasFocus)
                            mMouseListener->mouseMoved(_packageMouseMotion(evt));

                        // Try to keep the mouse inside the window
                        if (mWindowHasFocus)
                            _wrapMousePointer(evt.motion);
                    }
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    mMouseListener->mouseMoved(_packageMouseMotion(evt));
                    mMouseListener->mouseWheelMoved(evt.wheel);
                    break;
                case SDL_EVENT_SENSOR_UPDATE:
                    mSensorListener->sensorUpdated(evt.sensor);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    mMouseListener->mousePressed(evt.button, evt.button.button);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    mMouseListener->mouseReleased(evt.button, evt.button.button);
                    break;
                case SDL_EVENT_KEY_DOWN:
                    mKeyboardListener->keyPressed(evt.key);

                    if (!isModifierHeld(SDL_KMOD_ALT) && evt.key.key >= SDLK_F1 && evt.key.key <= SDLK_F12)
                    {
                        mViewer->getEventQueue()->keyPress(
                            osgGA::GUIEventAdapter::KEY_F1 + (evt.key.key - SDLK_F1));
                    }

                    break;
                case SDL_EVENT_KEY_UP:
                    if (!evt.key.repeat)
                    {
                        mKeyboardListener->keyReleased(evt.key);

                        if (!isModifierHeld(SDL_KMOD_ALT) && evt.key.key >= SDLK_F1
                            && evt.key.key <= SDLK_F12)
                            mViewer->getEventQueue()->keyRelease(
                                osgGA::GUIEventAdapter::KEY_F1 + (evt.key.key - SDLK_F1));
                    }

                    break;
                case SDL_EVENT_TEXT_EDITING:
                    break;
                case SDL_EVENT_TEXT_INPUT:
                    mKeyboardListener->textInput(evt.text);
                    break;
                case SDL_EVENT_KEYMAP_CHANGED:
                    break;
                case SDL_EVENT_JOYSTICK_HAT_MOTION: // As we manage everything with GameController, don't even bother with these.
                case SDL_EVENT_JOYSTICK_AXIS_MOTION:
                case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
                case SDL_EVENT_JOYSTICK_BUTTON_UP:
                case SDL_EVENT_JOYSTICK_ADDED:
                case SDL_EVENT_JOYSTICK_REMOVED:
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (mConListener)
                        mConListener->controllerAdded(
                            1, evt.gdevice); // We only support one joystick, so give everything a generic deviceID
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (mConListener)
                        mConListener->controllerRemoved(evt.gdevice);
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (mConListener)
                        mConListener->buttonPressed(1, evt.gbutton);
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_UP:
                    if (mConListener)
                        mConListener->buttonReleased(1, evt.gbutton);
                    break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                    if (mConListener)
                        mConListener->axisMoved(1, evt.gaxis);
                    break;
                case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
                    // controller sensor data is received on demand
                    break;
                case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
                    mConListener->touchpadPressed(1, TouchEvent(evt.gtouchpad));
                    break;
                case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
                    mConListener->touchpadMoved(1, TouchEvent(evt.gtouchpad));
                    break;
                case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
                    mConListener->touchpadReleased(1, TouchEvent(evt.gtouchpad));
                    break;
                case SDL_EVENT_QUIT:
                    if (mWindowListener)
                        mWindowListener->windowClosed();
                    break;
                case SDL_EVENT_DISPLAY_ORIENTATION:
                    if (mSensorListener && evt.display.displayID == SDL_GetDisplayForWindow(mSDLWindow))
                        mSensorListener->displayOrientationChanged();
                    break;
                case SDL_EVENT_CLIPBOARD_UPDATE:
                    break; // We don't need this event, clipboard is retrieved on demand

                case SDL_EVENT_FINGER_DOWN:
                case SDL_EVENT_FINGER_UP:
                case SDL_EVENT_FINGER_MOTION:
                    // No use for touch & gesture events
                    break;

                case SDL_EVENT_WILL_ENTER_BACKGROUND:
                case SDL_EVENT_WILL_ENTER_FOREGROUND:
                case SDL_EVENT_DID_ENTER_BACKGROUND:
                case SDL_EVENT_DID_ENTER_FOREGROUND:
                    // We do not need background/foreground switch event for mobile devices so far
                    break;

                case SDL_EVENT_TERMINATING:
                    // There is nothing we can do here.
                    break;

                case SDL_EVENT_LOW_MEMORY:
                    Log(Debug::Warning) << "System reports that free RAM on device is running low. You may encounter "
                                           "an unexpected behaviour.";
                    break;

                default:
                    Log(Debug::Info) << "Unhandled SDL event of type 0x" << std::hex << evt.type;
                    break;
            }
        }
    }

    void InputWrapper::handleWindowEvent(const SDL_Event& evt)
    {
        switch (evt.type)
        {
            case SDL_EVENT_WINDOW_MOUSE_ENTER:
                mMouseInWindow = true;
                updateMouseSettings();
                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                mMouseInWindow = false;
                updateMouseSettings();
                break;
            case SDL_EVENT_WINDOW_MOVED:
                // I'm not sure what OSG is using the window position for, but I don't think it's needed,
                // so we ignore window moved events (improves window movement performance)
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                int w, h;
                SDL_GetWindowSizeInPixels(mSDLWindow, &w, &h);
                int x, y;
                SDL_GetWindowPosition(mSDLWindow, &x, &y);

                // Happens when you Alt-Tab out of game
                if (w == 0 && h == 0)
                    return;

                mViewer->getCamera()->getGraphicsContext()->resized(x, y, w, h);

                mViewer->getEventQueue()->windowResize(x, y, w, h);

                if (mWindowListener)
                    mWindowListener->windowResized(w, h);

                _setWindowScale();

                break;

            case SDL_EVENT_WINDOW_RESIZED:
                // This should also fire SIZE_CHANGED, so no need to handle
                break;

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                mWindowHasFocus = true;
                updateMouseSettings();
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                mWindowHasFocus = false;
                updateMouseSettings();
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                break;
            case SDL_EVENT_WINDOW_SHOWN:
            case SDL_EVENT_WINDOW_RESTORED:
                if (mWindowListener)
                    mWindowListener->windowVisibilityChange(true);
                break;
            case SDL_EVENT_WINDOW_HIDDEN:
            case SDL_EVENT_WINDOW_MINIMIZED:
                if (mWindowListener)
                    mWindowListener->windowVisibilityChange(false);
                break;
        }
    }

    bool InputWrapper::isModifierHeld(int mod)
    {
        return (SDL_GetModState() & mod) != 0;
    }

    bool InputWrapper::isKeyDown(SDL_Scancode key)
    {
        return (SDL_GetKeyboardState(nullptr)[key]) != 0;
    }

    /// \brief Moves the mouse to the specified point within the viewport
    void InputWrapper::warpMouse(int x, int y)
    {
        SDL_WarpMouseInWindow(mSDLWindow, x, y);
        mWarpCompensate = true;
        mWarpX = static_cast<Uint16>(x);
        mWarpY = static_cast<Uint16>(y);
    }

    /// \brief Locks the pointer to the window
    void InputWrapper::setGrabPointer(bool grab)
    {
        mWantGrab = grab;
        updateMouseSettings();
    }

    /// \brief Set the mouse to relative positioning. Doesn't move the cursor
    ///        and disables mouse acceleration.
    void InputWrapper::setMouseRelative(bool relative)
    {
        mWantRelative = relative;
        updateMouseSettings();
    }

    void InputWrapper::setMouseVisible(bool visible)
    {
        mWantMouseVisible = visible;
        updateMouseSettings();
    }

    void InputWrapper::updateMouseSettings()
    {
        mGrabPointer = mWantGrab && mMouseInWindow && mWindowHasFocus;
        SDL_SetWindowMouseGrab(mSDLWindow, mGrabPointer && mAllowGrab);

        if (mWantMouseVisible || !mWindowHasFocus)
            SDL_ShowCursor();
        else
            SDL_HideCursor();

        bool relative = mWantRelative && mMouseInWindow && mWindowHasFocus;
        if (mMouseRelative == relative)
            return;

        mMouseRelative = relative;

        mWrapPointer = false;

        // eep, wrap the pointer manually if the input driver doesn't support
        // relative positioning natively
        // also use wrapping if no-grab was specified in options (SDL_SetRelativeMouseMode
        // appears to eat the mouse cursor when pausing in a debugger)
        const bool success = mAllowGrab && SDL_SetWindowRelativeMouseMode(mSDLWindow, relative);
        if (relative && !success)
            mWrapPointer = true;

        // now remove all mouse events using the old setting from the queue
        SDL_PumpEvents();
        SDL_FlushEvent(SDL_EVENT_MOUSE_MOTION);
    }

    /// \brief Internal method for ignoring relative motions as a side effect
    ///        of warpMouse()
    bool InputWrapper::_handleWarpMotion(const SDL_MouseMotionEvent& evt)
    {
        if (!mWarpCompensate)
            return false;

        // this was a warp event, signal the caller to eat it.
        if (evt.x == mWarpX && evt.y == mWarpY)
        {
            mWarpCompensate = false;
            return true;
        }

        return false;
    }

    /// \brief Wrap the mouse to the viewport
    void InputWrapper::_wrapMousePointer(const SDL_MouseMotionEvent& evt)
    {
        // don't wrap if we don't want relative movements, support relative
        // movements natively, or aren't grabbing anyways
        if (!mMouseRelative || !mWrapPointer || !mGrabPointer)
            return;

        int width = 0;
        int height = 0;

        SDL_GetWindowSize(mSDLWindow, &width, &height);

        const int fudgeFactorX = width / 4;
        const int fudgeFactorY = height / 4;

        // warp the mouse if it's about to go outside the window
        if (evt.x - fudgeFactorX < 0 || evt.x + fudgeFactorX > width || evt.y - fudgeFactorY < 0
            || evt.y + fudgeFactorY > height)
        {
            warpMouse(width / 2, height / 2);
        }
    }

    /// \brief Package mouse and mousewheel motions into a single event
    MouseMotionEvent InputWrapper::_packageMouseMotion(const SDL_Event& evt)
    {
        MouseMotionEvent packEvt = {};
        packEvt.x = mMouseX * mScaleX;
        packEvt.y = mMouseY * mScaleY;
        packEvt.z = mMouseZ;

        if (evt.type == SDL_EVENT_MOUSE_MOTION)
        {
            packEvt.x = mMouseX = static_cast<Sint32>(evt.motion.x * mScaleX);
            packEvt.y = mMouseY = static_cast<Sint32>(evt.motion.y * mScaleY);
            packEvt.xrel = static_cast<Sint32>(evt.motion.xrel * mScaleX);
            packEvt.yrel = static_cast<Sint32>(evt.motion.yrel * mScaleY);
            packEvt.type = SDL_EVENT_MOUSE_MOTION;
            if (mFirstMouseMove)
            {
                // first event should be treated as non-relative, since there's no point of reference
                // SDL then (incorrectly) uses (0,0) as point of reference, on Linux at least...
                packEvt.xrel = packEvt.yrel = 0;
                mFirstMouseMove = false;
            }
        }
        else if (evt.type == SDL_EVENT_MOUSE_WHEEL)
        {
            const double preciseY = evt.wheel.y;

            mPendingWheelY += preciseY * 120.0;
            const int zrel = static_cast<int>(mPendingWheelY);
            mPendingWheelY -= zrel;

            mMouseZ += zrel;
            packEvt.zrel = zrel;
            packEvt.z = mMouseZ;
            packEvt.type = SDL_EVENT_MOUSE_WHEEL;

            packEvt.x = static_cast<Sint32>(evt.wheel.mouse_x * mScaleX);
            packEvt.y = static_cast<Sint32>(evt.wheel.mouse_y * mScaleY);
        }
        else
        {
            throw std::runtime_error("Tried to package non-motion event!");
        }

        return packEvt;
    }
}
