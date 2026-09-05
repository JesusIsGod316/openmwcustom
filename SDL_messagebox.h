#pragma once

// V4 CP1B temporary include-path compatibility bridge.
// SDL3 installs public headers under SDL3/, while a few inherited OpenMW
// translation units still include the SDL2-era bare header name.
#include <SDL3/SDL_messagebox.h>
