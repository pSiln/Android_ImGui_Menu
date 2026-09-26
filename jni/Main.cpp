// Main.cpp - generic template entry: feature list, Changes() callback,
// and a poll thread that turns menu state into patches.
// No Unity/Il2Cpp dependency, no eglSwapBuffers hook: drawing lives on its
// own overlay surface + EGL context (see ImGuiOverlay/OverlayRenderer).
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <jni.h>
#include <unistd.h>
#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.hpp"
#include "Menu/Menu.hpp"
#include "Menu/Jni.hpp"
#include "Includes/Macros.h"
#include "ImGuiOverlay/Features.h"
#include "ImGuiOverlay/FeatureBus.hpp"

int scoreMul = 1, coinsMul = 1;

// Target library to patch/hook. Empty ("") = don't wait for any so,
// hack_thread is ready immediately (menu-only scenario).
#define targetLibName OBFUSCATE("libtarget.so")

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;

    int Total_Feature = Features::Count;
    jclass strCls = env->FindClass(OBFUSCATE("java/lang/String"));
    if (strCls == nullptr || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return nullptr;
    }
    ret = (jobjectArray)
            env->NewObjectArray(Total_Feature, strCls,
                                env->NewStringUTF(""));

    for (int i = 0; i < Total_Feature; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(Features::List[i]));

    return (ret);
}

bool btnPressed = false;

void Changes(JNIEnv *env, jclass clazz, jobject obj, jint featNum, jstring featName, jint value, jlong Lvalue, jboolean boolean, jstring text) {
    // featName may be null; GetStringUTFChars(nullptr) crashes.
    std::string name;
    const char *cName = nullptr;
    if (featName != nullptr) {
        cName = env->GetStringUTFChars(featName, nullptr);
        if (cName) name = cName;
    }
    if (cName) env->ReleaseStringUTFChars(featName, cName);
    if (env->ExceptionCheck()) env->ExceptionClear();

    // Guard: only PATCH/HOOK once the target so is loaded, otherwise
    // getAbsoluteAddress returns null and Dobby crashes.
    if (featNum >= 0 && !isLibraryLoaded(targetLibName)) {
        LOGW(OBFUSCATE("Changes(%d) ignored, %s not loaded yet"), featNum, (const char *)targetLibName);
        return;
    }

    switch (featNum) {
        case 0:
            // Example toggle. Replace "0x123456" with your real offset:
            // PATCH_SWITCH(targetLibName, "0x123456", "C0 03 5F D6", boolean);
            LOGI(OBFUSCATE("Toggle No death: %d"), boolean);
            break;
        case 1:
            btnPressed = true;
            LOGI(OBFUSCATE("Button pressed: %s"), name.c_str());
            break;
        case 2:
            scoreMul = value;
            break;
        case 3:
            coinsMul = value;
            break;
        default:
            LOGI(OBFUSCATE("Changes feat=%d val=%d bool=%d"), featNum, value, boolean);
            break;
    }
}

// Generic hack thread: wait for target so -> poll FeatureBus -> apply patch.
// Hooks nothing inside the game, needs no dex, works with plain dlopen injection.
// The floating window is created elsewhere: dex path via Launcher's
// ImGuiOverlayView, so-only path via the ModMenu_* exports (BridgeAPI.cpp).
void hack_thread() {
    const char *lib = (const char *)targetLibName;

    int waited = 0;
    if (lib != nullptr && lib[0] != '\0') {
        // Wait at most 60s so a process without the so doesn't sleep forever.
        while (!isLibraryLoaded(lib) && waited < 60) {
            sleep(1);
            waited++;
        }
        if (!isLibraryLoaded(lib)) {
            // Keep polling: the so may load later; the menu still works.
            LOGW(OBFUSCATE("Target lib %s not found after %ds, keep polling"), lib, waited);
        } else {
            LOGI(OBFUSCATE("Target lib loaded: %s @ 0x%lx"), lib, (unsigned long)getLibraryAddress(lib));
        }
    }

    // FeatureBus polling: menu toggles become patches here.
    // featNum mapping is documented at the top of Features.h.
    bool lastNoDeath = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        try {
            if (lib == nullptr || lib[0] == '\0' || !isLibraryLoaded(lib)) {
                continue;
            }

            // Example feat 0 Toggle: patch only on state change.
            bool noDeath = FeatureBus::GetBool(0);
            if (noDeath != lastNoDeath) {
                lastNoDeath = noDeath;
                // PATCH_SWITCH(lib, "0x123456", "C0 03 5F D6", noDeath);
                LOGI(OBFUSCATE("No death -> %d"), noDeath);
            }

            // Example feat 1 Button: consume accumulated clicks.
            int clicks = FeatureBus::ConsumeButton(1);
            if (clicks > 0) {
                btnPressed = true;
                LOGI(OBFUSCATE("Button x%d"), clicks);
            }

            // Example feat 2/3 SeekBar: sync value when set.
            if (FeatureBus::Has(2)) scoreMul = FeatureBus::GetInt(2);
            if (FeatureBus::Has(3)) coinsMul = FeatureBus::GetInt(3);
        } catch (...) {
        }
    }
}

__attribute__((constructor))
void lib_main() {
    // Constructor only spawns a thread; no heavy/dangerous work here.
    try {
        std::thread(hack_thread).detach();
    } catch (...) {
        LOGE(OBFUSCATE("hack_thread spawn failed"));
    }
}
