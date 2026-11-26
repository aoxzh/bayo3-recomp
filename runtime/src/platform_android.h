// Shared between the Android platform files (platform_android*.cpp).
#pragma once

#include <android/input.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <atomic>
#include <cstdint>

namespace nx::plat {

ANativeWindow* android_window();                   // platform_android.cpp: the current window, or null
int32_t on_input(android_app*, AInputEvent* e);    // platform_android_input.cpp: the glue's input callback
void release_input();                              // platform_android_input.cpp: no button, stick or finger held
void open_menu();                                  // platform_android.cpp: hold the game, show GameActivity's menu
extern std::atomic<int32_t> g_pad_device;          // platform_android_rumble.cpp: the controller that last sent input
void android_rumble_tick(android_app* app);        // platform_android_rumble.cpp: the game's rumble to that controller

}  // namespace nx::plat
