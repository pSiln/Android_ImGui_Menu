#include "Jni.hpp"
#include <list>
#include <vector>
#include <cstring>
#include <string>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <dlfcn.h>
#include "Includes/obfuscate.h"
#include "Menu/Jni.hpp"
#include "Includes/Logger.h"

void Dialog(JNIEnv *env, jobject context, const char *title, const char *message, const char *openBtn, const char *closeBtn, int sec, const char *url) {
    jclass dialogHelperClass = env->FindClass(OBFUSCATE("com/android/support/DialogHelper"));
    jmethodID showMethod = env->GetStaticMethodID(dialogHelperClass, OBFUSCATE("showDialogWithLink"),
                                                  OBFUSCATE("(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;)V"));

    jstring jTitle = env->NewStringUTF(title);
    jstring jMessage = env->NewStringUTF(message);
    jstring jOpen = env->NewStringUTF(openBtn);
    jstring jClose = env->NewStringUTF(closeBtn);
    jint jSec = sec;
    jstring jUrl = env->NewStringUTF(url);

    env->CallStaticVoidMethod(dialogHelperClass, showMethod, context, jTitle, jMessage, jOpen, jClose, jSec, jUrl);

    env->DeleteLocalRef(jTitle);
    env->DeleteLocalRef(jMessage);
    env->DeleteLocalRef(jOpen);
    env->DeleteLocalRef(jClose);
    env->DeleteLocalRef(jUrl);
}

void Toast(JNIEnv *env, jobject thiz, const char *text, int length) {
    if (thiz == nullptr || text == nullptr) return;
    if (env->ExceptionCheck()) env->ExceptionClear();
    jstring jstr = env->NewStringUTF(text);
    if (jstr == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jclass toast = env->FindClass(OBFUSCATE("android/widget/Toast"));
    if (toast == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jmethodID methodMakeText = env->GetStaticMethodID(toast, OBFUSCATE("makeText"), OBFUSCATE("(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;"));
    if (methodMakeText == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jobject toastobj = env->CallStaticObjectMethod(toast, methodMakeText, thiz, jstr, length);
    if (toastobj == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jmethodID methodShow = env->GetMethodID(toast, OBFUSCATE("show"), OBFUSCATE("()V"));
    if (methodShow == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    env->CallVoidMethod(toastobj, methodShow);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// Keep the first letter lowercase: capital caused a crash (upstream quirk).
void setText(JNIEnv *env, jobject obj, const char* text){
    if (obj == nullptr || text == nullptr) return;
    if (env->ExceptionCheck()) env->ExceptionClear();
    // Html.fromHtml("...") -> TextView.setText(Spanned)
    jclass html = (*env).FindClass(OBFUSCATE("android/text/Html"));
    if (html == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jmethodID fromHtml = (*env).GetStaticMethodID(html, OBFUSCATE("fromHtml"), OBFUSCATE("(Ljava/lang/String;)Landroid/text/Spanned;"));
    if (fromHtml == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }

    jclass textView = (*env).FindClass(OBFUSCATE("android/widget/TextView"));
    if (textView == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jmethodID setText = (*env).GetMethodID(textView, OBFUSCATE("setText"), OBFUSCATE("(Ljava/lang/CharSequence;)V"));
    if (setText == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }

    jstring jstr = (*env).NewStringUTF(text);
    if (jstr == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jobject spanned = (*env).CallStaticObjectMethod(html, fromHtml, jstr);
    if (spanned == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    (*env).CallVoidMethod(obj, setText, spanned);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void startService(JNIEnv *env, jobject ctx){
    // Target process may not register the Launcher service; never crash it.
    if (env->ExceptionCheck()) env->ExceptionClear();
    jclass native_context = env->GetObjectClass(ctx);
    if (native_context == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        LOGE(OBFUSCATE("startService: GetObjectClass failed"));
        return;
    }
    jclass intentClass = env->FindClass(OBFUSCATE("android/content/Intent"));
    jclass actionString = env->FindClass(OBFUSCATE("com/android/support/Launcher"));
    if (intentClass == nullptr || actionString == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        LOGW(OBFUSCATE("startService: Launcher service not in this process, skip"));
        return;
    }
    jmethodID newIntent = env->GetMethodID(intentClass, OBFUSCATE("<init>"), OBFUSCATE("(Landroid/content/Context;Ljava/lang/Class;)V"));
    if (newIntent == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jobject intent = env->NewObject(intentClass, newIntent, ctx, actionString);
    if (intent == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jmethodID startServiceMethodId = env->GetMethodID(native_context, OBFUSCATE("startService"), OBFUSCATE("(Landroid/content/Intent;)Landroid/content/ComponentName;"));
    if (startServiceMethodId == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    env->CallObjectMethod(ctx, startServiceMethodId, intent);
    // Missing service / background-start limits: swallow and log.
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        LOGW(OBFUSCATE("startService call failed, ignored"));
    }
}

int get_api_sdk(JNIEnv* env) {
    jclass build_version_class = env->FindClass(OBFUSCATE("android/os/Build$VERSION"));
    if (build_version_class == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return 0;
    }
    jfieldID sdk_int_field = env->GetStaticFieldID(build_version_class, OBFUSCATE("SDK_INT"), OBFUSCATE("I"));
    if (sdk_int_field == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return 0;
    }
    jint v = env->GetStaticIntField(build_version_class, sdk_int_field);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return 0;
    }
    return (int)v;
}

void startActivityPermisson(JNIEnv *env, jobject ctx){
    // Jump to the system overlay-permission page; any failure just clears and returns.
    if (ctx == nullptr) return;
    if (env->ExceptionCheck()) env->ExceptionClear();
    jclass native_context = env->GetObjectClass(ctx);
    if (native_context == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jmethodID startActivity = env->GetMethodID(native_context, OBFUSCATE("startActivity"),OBFUSCATE("(Landroid/content/Intent;)V"));
    jmethodID pack = env->GetMethodID(native_context, OBFUSCATE("getPackageName"),OBFUSCATE("()Ljava/lang/String;"));
    if (startActivity == nullptr || pack == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jstring packageName = static_cast<jstring>(env->CallObjectMethod(ctx, pack));
    if (packageName == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }

    const char *pkg = env->GetStringUTFChars(packageName, 0);
    if (pkg == nullptr) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }

    std::stringstream strpkg;
    strpkg << OBFUSCATE("package:");
    strpkg << pkg;
    std::string pakg = strpkg.str();
    env->ReleaseStringUTFChars(packageName, pkg);

    jclass Uri = env->FindClass(OBFUSCATE("android/net/Uri"));
    jclass intentclass = env->FindClass(OBFUSCATE("android/content/Intent"));
    if (Uri == nullptr || intentclass == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jmethodID Parce = env->GetStaticMethodID(Uri, OBFUSCATE("parse"), OBFUSCATE("(Ljava/lang/String;)Landroid/net/Uri;"));
    jmethodID newIntent = env->GetMethodID(intentclass, OBFUSCATE("<init>"), OBFUSCATE("(Ljava/lang/String;Landroid/net/Uri;)V"));
    if (Parce == nullptr || newIntent == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jstring uriStr = env->NewStringUTF(pakg.c_str());
    jstring actionStr = env->NewStringUTF(OBFUSCATE("android.settings.action.MANAGE_OVERLAY_PERMISSION"));
    if (uriStr == nullptr || actionStr == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jobject UriMethod = env->CallStaticObjectMethod(Uri, Parce, uriStr);
    if (UriMethod == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jobject intent = env->NewObject(intentclass, newIntent, actionStr, UriMethod);
    if (intent == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }

    env->CallVoidMethod(ctx, startActivity, intent);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// jclass param required: this backs a static Java method.
void CheckOverlayPermission(JNIEnv *env, jclass thiz, jobject ctx){
    //If overlay permission option is greyed out, make sure to add android.permission.SYSTEM_ALERT_WINDOW in manifest

    LOGI(OBFUSCATE("Check overlay permission"));

    // Never kill the host process (the original template called exit(0) after 5s,
    // which looked like a crash right after the game logo). Just toast + open settings.
    if (env->ExceptionCheck()) env->ExceptionClear();
    int sdkVer = get_api_sdk(env);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        startService(env, ctx);
        return;
    }
    if (sdkVer >= 23) {
        jclass Settings = env->FindClass(OBFUSCATE("android/provider/Settings"));
        if (Settings == nullptr || env->ExceptionCheck()) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            startService(env, ctx);
            return;
        }
        jmethodID canDraw = env->GetStaticMethodID(Settings, OBFUSCATE("canDrawOverlays"), OBFUSCATE("(Landroid/content/Context;)Z"));
        if (canDraw == nullptr || env->ExceptionCheck()) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            startService(env, ctx);
            return;
        }
        jboolean allowed = env->CallStaticBooleanMethod(Settings, canDraw, ctx);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            startService(env, ctx);
            return;
        }
        if (!allowed){
            Toast(env, ctx, OBFUSCATE("Overlay permission is required in order to show mod menu."), 1);
            if (env->ExceptionCheck()) env->ExceptionClear();
            startActivityPermisson(env, ctx);
            if (env->ExceptionCheck()) env->ExceptionClear();
            // Don't exit or loop; keep starting the menu service. The window shows
            // once the user grants the permission.
        }
    }

    LOGI(OBFUSCATE("Start service"));
    startService(env, ctx);
    if (env->ExceptionCheck()) env->ExceptionClear();
}