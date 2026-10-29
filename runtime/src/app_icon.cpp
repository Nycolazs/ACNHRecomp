// The app's icon on its windows (computers): the taskbar's and the title bar's on Windows and Linux (X11), the Dock's
// on macOS for the game started by the launcher. Wayland takes it from the .desktop file named after the app id.
// From assets/acnhrecomp.png, as 128x128 RGBA (app_icon_rgba.inc).
#include <SDL3/SDL.h>

namespace nx::plat {

void set_window_icon(SDL_Window* window) {
    static const unsigned char kPixels[] = {
#include "app_icon_rgba.inc"
    };
    if (!window) return;
    SDL_Surface* s = SDL_CreateSurfaceFrom(128, 128, SDL_PIXELFORMAT_ABGR8888, const_cast<unsigned char*>(kPixels), 128 * 4);
    if (!s) return;
    SDL_SetWindowIcon(window, s);
    SDL_DestroySurface(s);
}

}  // namespace nx::plat
