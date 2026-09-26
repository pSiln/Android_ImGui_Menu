#include "Includes/obfuscate.h"
#include "Menu/Menu.hpp"
#include "Utils.hpp"
#include "ImGuiOverlay/NativeOverlay.hpp"

int RegisterMenu(JNIEnv *env) {
    JNINativeMethod methods[] = {
            {OBFUSCATE("Icon"),            OBFUSCATE(
                                                   "()Ljava/lang/String;"),                                                           reinterpret_cast<void *>(Icon)},
            {OBFUSCATE("IconWebViewData"), OBFUSCATE(
                                                   "()Ljava/lang/String;"),                                                           reinterpret_cast<void *>(IconWebViewData)},
            {OBFUSCATE("IsGameLibLoaded"), OBFUSCATE(
                                                   "()Z"),                                                                            reinterpret_cast<void *>(isGameLibLoaded)},
            {OBFUSCATE("Init"),            OBFUSCATE(
                                                   "(Landroid/content/Context;Landroid/widget/TextView;Landroid/widget/TextView;)V"), reinterpret_cast<void *>(Init)},
            {OBFUSCATE("SettingsList"),    OBFUSCATE(
                                                   "()[Ljava/lang/String;"),                                                          reinterpret_cast<void *>(SettingsList)},
            {OBFUSCATE("GetFeatureList"),  OBFUSCATE(
                                                   "()[Ljava/lang/String;"),                                                          reinterpret_cast<void *>(GetFeatureList)},
    };

    jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Menu"));
    if (!clazz || env->ExceptionCheck())
        return JNI_ERR;
    if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0])) != 0)
        return JNI_ERR;
    return JNI_OK;
}

int RegisterPreferences(JNIEnv *env) {
    JNINativeMethod methods[] = {
            {OBFUSCATE("Changes"), OBFUSCATE("(Landroid/content/Context;ILjava/lang/String;IJZLjava/lang/String;)V"), reinterpret_cast<void *>(Changes)},
    };
    jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Preferences"));
    if (!clazz || env->ExceptionCheck())
        return JNI_ERR;
    if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0])) != 0)
        return JNI_ERR;
    return JNI_OK;
}

int RegisterMain(JNIEnv *env) {
    JNINativeMethod methods[] = {
            {OBFUSCATE("CheckOverlayPermission"), OBFUSCATE("(Landroid/content/Context;)V"),
             reinterpret_cast<void *>(CheckOverlayPermission)},
    };
    jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Main"));
    if (!clazz || env->ExceptionCheck())
        return JNI_ERR;
    if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0])) != 0)
        return JNI_ERR;

    return JNI_OK;
}

// Generic template: never return JNI_ERR. Our dex classes may not exist in
// the target process; failed FindClass is normal -> log and clear.
static void TryRegister(JNIEnv *env, int (*fn)(JNIEnv *), const char *tag) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    int r = -1;
    try {
        r = fn(env);
    } catch (...) {
        r = -1;
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (r != 0) {
        LOGW(OBFUSCATE("Register %s skipped (class not found in this process?)"), tag);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
}

extern "C"
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = nullptr;
    if (vm == nullptr) return JNI_VERSION_1_6;
    if (vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
        // Never return ERR here: it would make loadLibrary throw and crash the app.
        return JNI_VERSION_1_6;
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    TryRegister(env, RegisterMenu, "Menu");
    TryRegister(env, RegisterPreferences, "Preferences");
    TryRegister(env, RegisterMain, "Main");
    if (env->ExceptionCheck()) env->ExceptionClear();

    // Auto-create the floating window for so-only injection (no dex needed).
    NativeOverlay::SetVM(vm);
    NativeOverlay::StartAuto();

    return JNI_VERSION_1_6;
}
