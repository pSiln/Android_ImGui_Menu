// JNI_Bridge.cpp - Java <-> overlay bridge (dex path, static registration).
// JNI_OnLoad lives in Setup.cpp; logic is shared with BridgeAPI.cpp via OverlayHost.
#include <jni.h>
#include <android/log.h>

#include "OverlayHost.hpp"

#define LOG_TAG "ImGuiJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern "C" JNIEXPORT void JNICALL
Java_com_android_support_ImGuiOverlayView_nativeInitOverlay(JNIEnv* env, jclass, jobject surface, jobject context) {
    if (!env || !surface) return;
    if (env->ExceptionCheck()) env->ExceptionClear();
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK || !vm) return;
    OverlayHost::Init(vm, surface, context);
    LOGI("nativeInitOverlay done");
}

extern "C" JNIEXPORT void JNICALL
Java_com_android_support_ImGuiOverlayView_nativeDestroyOverlay(JNIEnv*, jclass) {
    OverlayHost::Shutdown();
}

extern "C" JNIEXPORT void JNICALL
Java_com_android_support_ImGuiOverlayView_nativeOnTouch(JNIEnv*, jclass, jint action, jfloat x, jfloat y) {
    OverlayHost::Touch((int)action, (float)x, (float)y);
}

extern "C" JNIEXPORT void JNICALL
Java_com_android_support_ImGuiOverlayView_nativeOnScroll(JNIEnv*, jclass, jfloat deltaY) {
    OverlayHost::Scroll((float)deltaY);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_android_support_ImGuiOverlayView_nativeIsInsideMenu(JNIEnv*, jclass, jfloat x, jfloat y) {
    return OverlayHost::HitTest((float)x, (float)y) ? JNI_TRUE : JNI_FALSE;
}
