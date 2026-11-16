// Shared between the Android platform files (platform_android*.cpp).
#pragma once

#include <android/input.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

namespace nx::plat {

ANativeWindow* android_window();                   // platform_android.cpp: the current window, or null
int32_t on_input(android_app*, AInputEvent* e);    // platform_android_input.cpp: the glue's input callback
// platform_android_input.cpp: the touches the settings screen took that ended, counted; whether one is on it now.
uint64_t ui_taps();
bool ui_finger_down();

}  // namespace nx::plat
