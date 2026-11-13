// libb3loader.so: the NativeActivity entry of the APK. The game library is either inside the APK (a developer build,
// android/build_bayonetta3.*) or built on the phone from the player's own dump by the in-app setup
// (<files>/game/libbayonetta3.so: an APK for players carries no game code). This finds it and hands the activity over.
#include <android/log.h>
#include <android/native_activity.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

#define TAG "b3loader"

typedef void (*on_create_fn)(ANativeActivity*, void*, size_t);

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity* activity, void* state,
                                                                     size_t size) {
    // A developer APK's own library first: it is newer than anything a past setup built.
    void* lib = dlopen("libbayonetta3.so", RTLD_NOW | RTLD_GLOBAL);
    char path[1024];
    if (!lib) {
        snprintf(path, sizeof path, "%s/game/libbayonetta3.so", activity->internalDataPath);
        if (access(path, R_OK) != 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "no game library: set the game up in the launcher (%s)", path);
            ANativeActivity_finish(activity);
            return;
        }
        lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    }
    if (!lib) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "cannot load the game: %s", dlerror());
        ANativeActivity_finish(activity);
        return;
    }
    on_create_fn on_create = (on_create_fn)dlsym(lib, "ANativeActivity_onCreate");
    if (!on_create || on_create == ANativeActivity_onCreate) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "the game library has no ANativeActivity_onCreate");
        ANativeActivity_finish(activity);
        return;
    }
    on_create(activity, state, size);
}
