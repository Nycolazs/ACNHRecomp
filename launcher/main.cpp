// The launcher's window and frame loop (launcher.h): SDL3 + Dear ImGui with the SDL_Renderer backend.
//
// The look is the settings screen's (ui/theme.h), laid out in design pixels for 1280x720 times the window's scale.
// On high-density displays the fonts are rasterized at the real pixel size and drawn scaled down, so text stays
// sharp. The loop sleeps in SDL_WaitEventTimeout while nothing happens.
//
// Tests: NX_LAUNCHER_SIZE=WxH (window size), NX_LAUNCHER_LANG=<code>, NX_LAUNCHER_PAGE=<page> (pages.cpp),
// NX_LAUNCHER_SHOT=<file.bmp> (saves the window after NX_LAUNCHER_SHOT_AFTER seconds, default 1, and exits),
// NX_LAUNCHER_INI=<file> (another launcher.ini), NX_LAUNCHER_SCRIPT=<script> (runs in place of setup_acnh.py),
// NX_LAUNCHER_SCROLL=<0..1> (the page scrolled that far), NX_LAUNCHER_SEARCH=keys|base|update|dlc|ptbr (that card
// of the files page searched).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

#include "launcher.h"
#include "ui/i18n.h"
#include "ui/theme.h"

using namespace launcher;

int main(int argc, char** argv) {
    (void)argc, (void)argv;
    SDL_SetAppMetadata("ACNHRecomp", NX_VERSION_STRING, "io.github.acnhrecomp");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    settings().load(launcher_ini_path());
    const char* lang_env = std::getenv("NX_LAUNCHER_LANG");
    nx::ui::ui_set_language(lang_env ? lang_env : settings().get("launcher.language", "auto"));

    const char* shot = std::getenv("NX_LAUNCHER_SHOT");
    double shot_after = std::getenv("NX_LAUNCHER_SHOT_AFTER") ? std::atof(std::getenv("NX_LAUNCHER_SHOT_AFTER")) : 1.0;
    int w = 1180, h = 760;
    if (const char* s = std::getenv("NX_LAUNCHER_SIZE")) {
        std::sscanf(s, "%dx%d", &w, &h);
    } else if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
        // Most of a small screen, never more than the design size on a big one.
        float fit = std::min(1.0f, std::min(m->w * 0.85f / w, m->h * 0.85f / h));
        w = int(w * fit), h = int(h * fit);
    }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (shot ? SDL_WINDOW_HIDDEN : 0);
    SDL_Window* window = SDL_CreateWindow("ACNHRecomp", w, h, flags);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    nx::plat::set_window_icon(window);
    SDL_SetWindowMinimumSize(window, 360, 480);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // launcher.ini keeps what matters
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavMoveSetMousePos = false;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    pages_init(window);

    float font_scale = 0;
    std::string font_lang;
    nx::ui::ThemeFonts fonts;
    const auto t0 = std::chrono::steady_clock::now();
    bool run = true;
    int settle = 0;  // frames still drawn at full rate after an event (hover, popups opening)
    while (run) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, pages_busy() || shot || settle > 0 ? 16 : 250)) {
            settle = 8;
            do {
                ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) pages_close_requested();
                if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) pages_drop(e.drop.data, e.drop.x, e.drop.y);
                if (e.type == SDL_EVENT_DROP_BEGIN || e.type == SDL_EVENT_DROP_POSITION) pages_drag(true, e.drop.x, e.drop.y);
                if (e.type == SDL_EVENT_DROP_COMPLETE || e.type == SDL_EVENT_MOUSE_MOTION) pages_drag(false, 0, 0);
            } while (SDL_PollEvent(&e));
        } else if (settle > 0) {
            settle--;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) continue;
        int ww = 0, wh = 0;
        SDL_GetWindowSize(window, &ww, &wh);
        float density = std::max(1.0f, SDL_GetWindowPixelDensity(window));
        float scale = nx::ui::theme_scale(float(ww), float(wh));
        if (std::fabs(scale * density - font_scale) > 0.04f || font_lang != nx::ui::ui_language()) {
            ImGui_ImplSDLRenderer3_DestroyFontsTexture();  // recreated by the next NewFrame
            fonts = nx::ui::theme_build_fonts(scale * density, "");
            io.FontGlobalScale = 1.0f / density;
            nx::ui::theme_apply_style(scale);
            font_scale = scale * density, font_lang = nx::ui::ui_language();
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::PushFont(fonts.body);
        run = pages_draw(scale, fonts.title);
        ImGui::PopFont();
        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 255, 249, 232, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (shot && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > shot_after) {
            if (SDL_Surface* s = SDL_RenderReadPixels(renderer, nullptr)) {
                if (!SDL_SaveBMP(s, shot)) std::fprintf(stderr, "SDL_SaveBMP: %s\n", SDL_GetError());
                SDL_DestroySurface(s);
            }
            run = false;
        }
        SDL_RenderPresent(renderer);
    }

    setup_cancel();  // never leave the setup running without its window
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
