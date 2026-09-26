// BridgeAPI.cpp - C exports for so-only injection (dlsym-friendly).
// Built with -fvisibility=hidden, so each export is marked default visibility.
//
// Typical call from the injector:
//   int (*init)(JavaVM*, jobject, jobject) = dlsym(h, "ModMenu_InitOverlay");
//   init(vm, surface, appContext);
#include <jni.h>

#include "FeatureBus.hpp"
#include "NativeOverlay.hpp"
#include "InputHook.hpp"
#include "OverlayHost.hpp"

#define MODMENU_API __attribute__((visibility("default")))

extern "C" {

// Start overlay rendering + render thread.
// vm: JavaVM*; surface: android.view.Surface jobject;
// ctx: app/activity jobject, may be null (null -> FeatureBus only, no Preferences callback).
// Returns 1 on success, 0/-1 on failure. Never throws, never crashes the host.
MODMENU_API int ModMenu_InitOverlay(JavaVM* vm, jobject surface, jobject ctx) {
    try {
        bool ok = OverlayHost::Init(vm, surface, ctx);
        if (ok) InputHook::Start();
        return ok ? 1 : 0;
    } catch (...) {
        return -1;
    }
}

// Touch master switch: 1 = on (default), 0 = off (all input passes to the game).
MODMENU_API void ModMenu_SetTouchEnabled(int enabled) {
    try {
        InputHook::SetEnabled(enabled != 0);
    } catch (...) {
    }
}

// Trigger auto window creation. Normally unnecessary (JNI_OnLoad already
// starts it); call once if the injector skipped JNI_OnLoad. Idempotent.
MODMENU_API void ModMenu_AutoOverlay() {
    try {
        NativeOverlay::StartAuto();
    } catch (...) {
    }
}

MODMENU_API void ModMenu_ShutdownOverlay() {
    try {
        OverlayHost::Shutdown();
    } catch (...) {
    }
}

// action: 0=DOWN 1=UP 2=MOVE 3=CANCEL; x/y in overlay-window pixels.
MODMENU_API void ModMenu_Touch(int action, float x, float y) {
    try {
        OverlayHost::Touch(action, x, y);
    } catch (...) {
    }
}

// 1 = point is inside the menu (consume), 0 = outside (pass through to game).
MODMENU_API int ModMenu_HitTest(float x, float y) {
    try {
        return OverlayHost::HitTest(x, y) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

MODMENU_API int ModMenu_IsReady() {
    try {
        return OverlayHost::IsReady() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

// ---- FeatureBus readers; featNum matches the list in Features.h ----
MODMENU_API int ModMenu_GetBool(int feat) {
    try {
        return FeatureBus::GetBool(feat) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

MODMENU_API int ModMenu_GetInt(int feat) {
    try {
        return FeatureBus::GetInt(feat);
    } catch (...) {
        return 0;
    }
}

MODMENU_API long long ModMenu_GetLong(int feat) {
    try {
        return FeatureBus::GetLong(feat);
    } catch (...) {
        return 0;
    }
}

// out is caller-allocated; returns copied length, or -1 when unset.
MODMENU_API int ModMenu_GetString(int feat, char* out, int outLen) {
    try {
        return FeatureBus::GetString(feat, out, outLen);
    } catch (...) {
        return -1;
    }
}

// Returns accumulated Button clicks and resets the counter to 0.
MODMENU_API int ModMenu_ConsumeButton(int feat) {
    try {
        return FeatureBus::ConsumeButton(feat);
    } catch (...) {
        return 0;
    }
}

} // extern "C"
