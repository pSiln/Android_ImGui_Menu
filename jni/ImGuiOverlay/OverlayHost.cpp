// OverlayHost.cpp - overlay host shared by JNI and C-export paths.
#include "OverlayHost.hpp"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <mutex>

#include "MenuUI.hpp"
#include "OverlayRenderer.hpp"

#define LOG_TAG "ImGuiHost"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace OverlayHost {

static JavaVM* g_jvm = nullptr;
static ANativeWindow* g_window = nullptr;
static jobject g_ctxGlobal = nullptr;
static std::mutex g_lock;

static JNIEnv* TlEnv(JavaVM* vm, bool* attached) {
    if (!vm) return nullptr;
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        if (attached) *attached = false;
        return env;
    }
    if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        if (attached) *attached = true;
        return env;
    }
    return nullptr;
}

static void ClearCtxLocked(JNIEnv* env) {
    if (g_ctxGlobal && env) {
        env->DeleteGlobalRef(g_ctxGlobal);
    }
    g_ctxGlobal = nullptr;
    MenuUI::SetJniEnv(nullptr, nullptr);
}

bool Init(JavaVM* vm, jobject surface, jobject ctx) {
    if (!vm || !surface) {
        LOGE("Init: null vm/surface");
        return false;
    }
    std::lock_guard<std::mutex> lock(g_lock);

    bool attached = false;
    JNIEnv* env = TlEnv(vm, &attached);
    if (!env) {
        LOGE("Init: no JNIEnv");
        return false;
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    g_jvm = vm;

    ANativeWindow* nw = ANativeWindow_fromSurface(env, surface);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!nw) {
        LOGE("ANativeWindow_fromSurface failed");
        return false;
    }

    if (g_window == nw) {
        ANativeWindow_release(nw);
        return true;
    }
    if (g_window) {
        OverlayRenderer::Get().Shutdown();
        ANativeWindow_release(g_window);
        g_window = nullptr;
        ClearCtxLocked(env);
    }

    // ctx may be null: no Preferences class in pure-so processes; menu state
    // still flows into FeatureBus for hack_thread / ModMenu_Get*.
    if (ctx) {
        jobject cg = env->NewGlobalRef(ctx);
        if (env->ExceptionCheck()) env->ExceptionClear();
        ClearCtxLocked(env);
        g_ctxGlobal = cg;
        if (cg) MenuUI::SetJniEnv(g_jvm, g_ctxGlobal);
    } else {
        ClearCtxLocked(env);
    }

    g_window = nw;
    if (!OverlayRenderer::Get().Init(g_window)) {
        LOGE("OverlayRenderer Init failed");
        ANativeWindow_release(g_window);
        g_window = nullptr;
        ClearCtxLocked(env);
        return false;
    }
    OverlayRenderer::Get().StartRenderThread();
    LOGI("OverlayHost Init done");
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_lock);
    OverlayRenderer::Get().Shutdown();
    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }
    if (g_jvm) {
        bool attached = false;
        JNIEnv* env = TlEnv(g_jvm, &attached);
        if (env) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            ClearCtxLocked(env);
        } else {
            g_ctxGlobal = nullptr;
            MenuUI::SetJniEnv(nullptr, nullptr);
        }
    }
    LOGI("OverlayHost Shutdown done");
}

bool IsReady() {
    return OverlayRenderer::Get().IsReady();
}

void Touch(int action, float x, float y) {
    if (!IsReady()) return;
    OverlayRenderer::Get().OnTouchEvent(action, x, y);
}

void Scroll(float delta_y) {
    if (!IsReady()) return;
    OverlayRenderer::Get().OnScroll(delta_y);
}

bool HitTest(float x, float y) {
    if (!IsReady()) return false; // pass through before first frame
    return MenuUI::IsInsideMenu(x, y);
}

} // namespace OverlayHost
