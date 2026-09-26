// NativeOverlay.cpp - fully-native auto floating window (no dex needed).
//
// Flow: dlopen -> JNI_OnLoad stores vm -> thread waits for the app ->
// ActivityThread.currentApplication() for a Context ->
// new SurfaceView + WindowManager.addView (TYPE_APPLICATION_OVERLAY, fullscreen
// transparent) -> wait until the Surface is valid -> OverlayHost::Init starts
// the EGL + ImGui render thread.
//
// - Uses only android.* system classes, so it works without our dex.
// - Window flags keep it display-only: NOT_FOCUSABLE|NOT_TOUCHABLE|
//   LAYOUT_IN_SCREEN|LAYOUT_INSET_DECOR. Touch is captured in-process instead.
// - Skipped automatically when our dex is present (repackaged path).
#include "NativeOverlay.hpp"

#include <android/log.h>
#include <atomic>
#include <chrono>
#include <dlfcn.h>
#include <thread>

#include "OverlayHost.hpp"
#include "InputHook.hpp"

#define LOG_TAG "NativeOverlay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace NativeOverlay {

static JavaVM* g_vm = nullptr;
static std::atomic<bool> g_started{false};

void SetVM(JavaVM* vm) {
    if (vm) g_vm = vm;
}

// Clear any pending exception and check for null.
static bool Bad(JNIEnv* env, jobject o) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    return o == nullptr;
}

// Args for the surface-waiter thread (holder/ctx are global refs, cross-thread safe).
struct SurfaceWaitArgs {
    JavaVM* vm = nullptr;
    jobject holder = nullptr;
    jobject ctx = nullptr;
};

// Polls holder.getSurface().isValid(), then hands it to OverlayHost.
// Must be a separate thread: ViewRoot traversal is driven by Looper.loop() on
// the window-creating thread, which must never block waiting (otherwise the
// surface never becomes valid).
static void SurfaceWaitMain(SurfaceWaitArgs* a) {
    JavaVM* wvm = a->vm;
    JNIEnv* wenv = nullptr;
    if (wvm->AttachCurrentThread(&wenv, nullptr) != JNI_OK || !wenv) {
        LOGE("waiter attach failed");
        delete a; // rare path; a couple of leaked global refs die with the process
        return;
    }
    jobject holder = a->holder;
    jobject wctx = a->ctx;
    delete a;

    jclass holderClass = wenv->FindClass("android/view/SurfaceHolder");
    jclass surfaceClass = wenv->FindClass("android/view/Surface");
    jmethodID getSurface = (holderClass && !wenv->ExceptionCheck())
        ? wenv->GetMethodID(holderClass, "getSurface", "()Landroid/view/Surface;") : nullptr;
    jmethodID isValid = (surfaceClass && !wenv->ExceptionCheck())
        ? wenv->GetMethodID(surfaceClass, "isValid", "()Z") : nullptr;
    if (wenv->ExceptionCheck()) wenv->ExceptionClear();
    if (!getSurface || !isValid) {
        LOGE("waiter: Surface methods not found");
        if (wctx) wenv->DeleteGlobalRef(wctx);
        wvm->DetachCurrentThread();
        return;
    }

    jobject surface = nullptr;
    for (int i = 0; i < 200; i++) { // ~10s; usually valid within 1-2 frames
        jobject s = wenv->CallObjectMethod(holder, getSurface);
        if (wenv->ExceptionCheck()) { wenv->ExceptionClear(); s = nullptr; }
        if (s) {
            jboolean ok = wenv->CallBooleanMethod(s, isValid);
            if (wenv->ExceptionCheck()) { wenv->ExceptionClear(); ok = JNI_FALSE; }
            if (ok) { surface = s; break; }
            wenv->DeleteLocalRef(s);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!surface) {
        LOGE("surface never valid");
    } else if (!OverlayHost::Init(wvm, surface, wctx)) {
        LOGE("OverlayHost Init failed");
    } else {
        LOGI("overlay rendering started");
        // The window itself is NOT_TOUCHABLE, so we intercept system input
        // inside the game process: consume hits over the menu, pass everything
        // else. Failure here does not affect rendering.
        InputHook::Start();
    }
    if (surface) wenv->DeleteLocalRef(surface);
    if (wctx) wenv->DeleteGlobalRef(wctx); // Init took its own global ref
    // holder global ref is kept on purpose; freed with the process
    wvm->DetachCurrentThread();
}

static void AutoMain() {
    JavaVM* vm = g_vm;
    if (!vm) {
        // Fallback for injectors that map the lib without JNI_OnLoad:
        // JNI_GetCreatedJavaVMs lives in libart and is not link-time visible,
        // so resolve it at runtime with dlsym.
        void* hArt = dlopen("libart.so", RTLD_NOW);
        if (hArt) {
            typedef jint (*GetVMsFn)(JavaVM**, jsize, jsize*);
            GetVMsFn fn = (GetVMsFn)dlsym(hArt, "JNI_GetCreatedJavaVMs");
            if (fn) {
                JavaVM* buf[1] = {nullptr};
                jsize n = 0;
                if (fn(buf, 1, &n) == JNI_OK && n >= 1 && buf[0]) {
                    vm = buf[0];
                    g_vm = vm;
                }
            }
            dlclose(hArt);
        }
        if (!vm) {
            LOGE("no JavaVM, auto overlay aborted");
            return;
        }
    }

    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGE("AttachCurrentThread failed");
        return;
    }

    // Dex present? Then the repackaged Launcher owns the window; step aside.
    {
        jclass mine = env->FindClass("com/android/support/ImGuiOverlayView");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (mine != nullptr) {
            LOGI("dex overlay present, skip native window");
            vm->DetachCurrentThread();
            return;
        }
    }

    // Wait for the app (currentApplication is null before Application exists).
    jclass atClass = env->FindClass("android/app/ActivityThread");
    if (Bad(env, atClass)) { vm->DetachCurrentThread(); return; }
    jmethodID curApp = env->GetStaticMethodID(atClass, "currentApplication", "()Landroid/app/Application;");
    if (Bad(env, (jobject)(uintptr_t)curApp)) { vm->DetachCurrentThread(); return; }

    jobject app = nullptr;
    for (int i = 0; i < 240; i++) { // up to ~120s
        app = env->CallStaticObjectMethod(atClass, curApp);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (app) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!app) {
        LOGE("currentApplication never ready");
        vm->DetachCurrentThread();
        return;
    }

    // Build the window with the application context.
    jclass appClass = env->GetObjectClass(app);
    jmethodID getAppCtx = env->GetMethodID(appClass, "getApplicationContext", "()Landroid/content/Context;");
    jobject appCtx = nullptr;
    if (getAppCtx && !env->ExceptionCheck()) {
        appCtx = env->CallObjectMethod(app, getAppCtx);
        if (env->ExceptionCheck()) { env->ExceptionClear(); appCtx = nullptr; }
    } else {
        env->ExceptionClear();
    }
    jobject ctx = appCtx ? appCtx : app;

    // Looper.prepare: ViewRoot needs a Looper on this thread.
    jclass looperClass = env->FindClass("android/os/Looper");
    if (Bad(env, looperClass)) { vm->DetachCurrentThread(); return; }
    jmethodID looperPrepare = env->GetStaticMethodID(looperClass, "prepare", "()V");
    jmethodID looperLoop = env->GetStaticMethodID(looperClass, "loop", "()V");
    if (!looperPrepare || !looperLoop || env->ExceptionCheck()) {
        env->ExceptionClear();
        vm->DetachCurrentThread();
        return;
    }
    env->CallStaticVoidMethod(looperClass, looperPrepare);
    if (env->ExceptionCheck()) env->ExceptionClear();

    jclass ctxClass = env->FindClass("android/content/Context");
    if (Bad(env, ctxClass)) { vm->DetachCurrentThread(); return; }
    jmethodID getService = env->GetMethodID(ctxClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    if (!getService || env->ExceptionCheck()) {
        env->ExceptionClear();
        vm->DetachCurrentThread();
        return;
    }
    jstring winSvc = env->NewStringUTF("window");
    jobject wm = env->CallObjectMethod(ctx, getService, winSvc);
    env->DeleteLocalRef(winSvc);
    if (Bad(env, wm)) { vm->DetachCurrentThread(); return; }

    // new SurfaceView(ctx)
    jclass svClass = env->FindClass("android/view/SurfaceView");
    if (Bad(env, svClass)) { vm->DetachCurrentThread(); return; }
    jmethodID svInit = env->GetMethodID(svClass, "<init>", "(Landroid/content/Context;)V");
    jmethodID setBg = env->GetMethodID(svClass, "setBackgroundColor", "(I)V");
    jmethodID setTop = env->GetMethodID(svClass, "setZOrderOnTop", "(Z)V");
    jmethodID getHolder = env->GetMethodID(svClass, "getHolder", "()Landroid/view/SurfaceHolder;");
    if (!svInit || !setBg || !setTop || !getHolder || env->ExceptionCheck()) {
        env->ExceptionClear();
        vm->DetachCurrentThread();
        return;
    }
    jobject sv = env->NewObject(svClass, svInit, ctx);
    if (Bad(env, sv)) { vm->DetachCurrentThread(); return; }
    env->CallVoidMethod(sv, setBg, (jint)0);
    env->CallVoidMethod(sv, setTop, JNI_TRUE);
    if (env->ExceptionCheck()) env->ExceptionClear();

    jobject holder = env->CallObjectMethod(sv, getHolder);
    if (Bad(env, holder)) { vm->DetachCurrentThread(); return; }
    jclass holderClass = env->FindClass("android/view/SurfaceHolder");
    if (Bad(env, holderClass)) { vm->DetachCurrentThread(); return; }
    jmethodID setFormat = env->GetMethodID(holderClass, "setFormat", "(I)V");
    jmethodID getSurface = env->GetMethodID(holderClass, "getSurface", "()Landroid/view/Surface;");
    if (!setFormat || !getSurface || env->ExceptionCheck()) {
        env->ExceptionClear();
        vm->DetachCurrentThread();
        return;
    }
    env->CallVoidMethod(holder, setFormat, (jint)-2); // PixelFormat.TRANSPARENT
    if (env->ExceptionCheck()) env->ExceptionClear();
    jobject holderGlobal = env->NewGlobalRef(holder);
    if (!holderGlobal || env->ExceptionCheck()) {
        env->ExceptionClear();
        vm->DetachCurrentThread();
        return;
    }

    // Fullscreen transparent overlay params; addView must run before the surface exists.
    int sdk = 26;
    {
        jclass verClass = env->FindClass("android/os/Build$VERSION");
        if (verClass && !env->ExceptionCheck()) {
            jfieldID sdkField = env->GetStaticFieldID(verClass, "SDK_INT", "I");
            if (sdkField && !env->ExceptionCheck()) {
                sdk = (int)env->GetStaticIntField(verClass, sdkField);
                if (env->ExceptionCheck()) { env->ExceptionClear(); sdk = 26; }
            } else {
                env->ExceptionClear();
            }
        } else {
            env->ExceptionClear();
        }
    }
    const int type = (sdk >= 26) ? 2038 /* TYPE_APPLICATION_OVERLAY */ : 2002 /* TYPE_PHONE */;
    // NOT_FOCUSABLE(8) | NOT_TOUCHABLE(16) | LAYOUT_IN_SCREEN(256) | LAYOUT_INSET_DECOR(512)
    const int flags = 8 | 16 | 256 | 512;

    jclass lpClass = env->FindClass("android/view/WindowManager$LayoutParams");
    if (Bad(env, lpClass)) { vm->DetachCurrentThread(); return; }
    jmethodID lpInit = env->GetMethodID(lpClass, "<init>", "(IIIII)V");
    if (!lpInit || env->ExceptionCheck()) { env->ExceptionClear(); vm->DetachCurrentThread(); return; }
    jobject lp = env->NewObject(lpClass, lpInit, (jint)-1, (jint)-1, (jint)type, (jint)flags, (jint)-2);
    if (Bad(env, lp)) { vm->DetachCurrentThread(); return; }
    jfieldID cutout = env->GetFieldID(lpClass, "layoutInDisplayCutoutMode", "I");
    if (cutout && !env->ExceptionCheck()) {
        env->SetIntField(lp, cutout, (jint)1);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    jclass wmClass = env->FindClass("android/view/WindowManager");
    if (Bad(env, wmClass)) { vm->DetachCurrentThread(); return; }
    jmethodID addView = env->GetMethodID(wmClass, "addView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
    if (!addView || env->ExceptionCheck()) { env->ExceptionClear(); vm->DetachCurrentThread(); return; }
    env->CallVoidMethod(wm, addView, sv, lp);
    if (env->ExceptionCheck()) {
        // Most common cause: missing overlay permission. Log and move on.
        env->ExceptionClear();
        LOGW("addView failed (overlay permission?)");
        env->DeleteGlobalRef(holderGlobal);
        vm->DetachCurrentThread();
        return;
    }
    LOGI("native overlay window added");

    // Never block here: the first ViewRoot traversal is driven by
    // Looper.loop() below, while the waiter thread polls surface validity.
    // ctx must be a global ref to cross threads (local refs cannot).
    jobject ctxGlobal = nullptr;
    if (ctx) {
        ctxGlobal = env->NewGlobalRef(ctx);
        if (env->ExceptionCheck() || !ctxGlobal) {
            env->ExceptionClear();
            ctxGlobal = nullptr;
            LOGW("ctx global ref failed, continue without ctx");
        }
    }
    SurfaceWaitArgs* args = nullptr;
    try {
        args = new SurfaceWaitArgs{vm, holderGlobal, ctxGlobal};
    } catch (...) {
        args = nullptr;
    }
    if (!args) {
        LOGE("oom, auto overlay aborted");
        if (ctxGlobal) env->DeleteGlobalRef(ctxGlobal);
        env->DeleteGlobalRef(holderGlobal);
        vm->DetachCurrentThread();
        return;
    }
    try {
        std::thread(SurfaceWaitMain, args).detach();
    } catch (...) {
        LOGE("waiter spawn failed");
        delete args;
        if (ctxGlobal) env->DeleteGlobalRef(ctxGlobal);
        env->DeleteGlobalRef(holderGlobal);
        vm->DetachCurrentThread();
        return;
    }

    // Permanent Looper for the SurfaceView's ViewRoot; this thread never returns.
    env->CallStaticVoidMethod(looperClass, looperLoop);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void StartAuto() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    try {
        std::thread(AutoMain).detach();
    } catch (...) {
        g_started.store(false);
        LOGE("StartAuto: thread spawn failed");
    }
}

} // namespace NativeOverlay
