// MenuUI.cpp - ImGui menu rendering and feature parsing.
//
#include "MenuUI.hpp"

#include <android/log.h>
#include <array>
#include <cctype>
#include <cstring>
#include <map>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "FeatureBus.hpp"
#include "Features.h"

#define LOG_TAG "ImGuiMenu"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace MenuUI {

// ===========================================================================
// Feature list (single source of truth: Features.h)
// ===========================================================================
const std::vector<const char*>& GetFeatures() {
    static std::vector<const char*> vec;
    if (vec.empty()) {
        for (int i = 0; i < Features::Count; i++) vec.push_back(Features::List[i]);
    }
    return vec;
}

// ===========================================================================
// Helpers
// ===========================================================================
static std::vector<std::string> Split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(delim, start);
        if (pos == std::string::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

static bool IsDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
    return true;
}

static std::string ReplaceFirst(const std::string& src, const std::string& old, const std::string& nw) {
    size_t pos = src.find(old);
    if (pos == std::string::npos) return src;
    std::string r = src;
    return r.replace(pos, old.size(), nw);
}

// ===========================================================================
// Feature parsing (mirrors the original Menu.java numbering/split rules)
// ===========================================================================
std::vector<Feature> ParseFeatures() {
    const auto& raw = GetFeatures();
    std::vector<Feature> out;
    int subFeat = 0;

    for (int i = 0; i < (int)raw.size(); i++) {
        std::string feature = raw[i];
        Feature f;

        // "_True" -> default on
        if (feature.find("_True") != std::string::npos) {
            f.defOn = true;
            feature = ReplaceFirst(feature, "_True", "");
        }

        // "CollapseAdd_" prefix -> item inside a collapse group
        if (feature.rfind("CollapseAdd_", 0) == 0) {
            f.inCollapse = true;
            feature = feature.substr(std::strlen("CollapseAdd_"));
        }

        // split
        std::vector<std::string> str = Split(feature, '_');
        if (str.empty()) continue;

        // Numbering: leading digits = explicit featNum, else i - subFeat
        if (IsDigits(str[0])) {
            f.featNum = std::stoi(str[0]);
            feature = feature.substr(str[0].size() + 1);
            str = Split(feature, '_');
            subFeat++;
        } else {
            f.featNum = i - subFeat;
        }

        if (str.empty()) continue;
        f.type = str[0];

        // Per-type argument parsing (same as the Menu.java switch).
        if (f.type == "Toggle" || f.type == "CheckBox" || f.type == "Button" || f.type == "InputText") {
            f.name = str.size() > 1 ? str[1] : "";
        } else if (f.type == "SeekBar") {
            f.name = str.size() > 1 ? str[1] : "";
            f.min  = str.size() > 2 ? std::stoi(str[2]) : 0;
            f.max  = str.size() > 3 ? std::stoi(str[3]) : 100;
        } else if (f.type == "ButtonOnOff") {
            f.name = str.size() > 1 ? str[1] : "";
        } else if (f.type == "Spinner") {
            f.name  = str.size() > 1 ? str[1] : "";
            f.extra = str.size() > 2 ? str[2] : "";
        } else if (f.type == "InputValue") {
            if (str.size() >= 3) { f.max = std::stoi(str[1]); f.name = str[2]; }
            else if (str.size() == 2) { f.max = 0; f.name = str[1]; }
        } else if (f.type == "InputLValue") {
            if (str.size() >= 3) { f.maxLong = std::stoll(str[1]); f.name = str[2]; }
            else if (str.size() == 2) { f.maxLong = 0; f.name = str[1]; }
        } else if (f.type == "RadioButton") {
            f.name  = str.size() > 1 ? str[1] : "";
            f.extra = str.size() > 2 ? str[2] : "";
        } else if (f.type == "Collapse") {
            f.name = str.size() > 1 ? str[1] : "";
        } else if (f.type == "ButtonLink") {
            f.name  = str.size() > 1 ? str[1] : "";
            f.extra = str.size() > 2 ? str[2] : "";
        } else if (f.type == "Category" || f.type == "RichTextView" || f.type == "RichWebView") {
            f.name = str.size() > 1 ? str[1] : "";
        }

        // Same as Menu.java: Category/Collapse/ButtonLink/RichText* don't count,
        // otherwise featNum drifts from the Java menu and Preferences/Changes()
        // mappings break.
        if (f.type == "Collapse" || f.type == "ButtonLink" || f.type == "Category" ||
            f.type == "RichTextView" || f.type == "RichWebView") {
            subFeat++;
        }

        out.push_back(f);
    }
    return out;
}

// ===========================================================================
// JNI callbacks (Preferences.changeFeatureXxx: persist state + Changes())
// ===========================================================================
static JavaVM*  g_jvm      = nullptr;
static jobject  g_context  = nullptr;  // global ref
static jclass   g_prefs    = nullptr;  // global ref
static jmethodID g_changeBool   = nullptr;
static jmethodID g_changeInt    = nullptr;
static jmethodID g_changeLong   = nullptr;
static jmethodID g_changeString = nullptr;
static bool g_prefsChecked = false;

void SetJniEnv(JavaVM* vm, jobject contextGlobal) {
    g_jvm = vm;
    g_context = contextGlobal;
    // Re-check after a vm change (OverlayHost calls this on Init/Shutdown).
    g_prefsChecked = false;
    g_changeBool = g_changeInt = g_changeLong = g_changeString = nullptr;
}

static JNIEnv* GetEnv() {
    if (!g_jvm) return nullptr;
    JNIEnv* env = nullptr;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        g_jvm->AttachCurrentThread(&env, nullptr);
    }
    return env;
}

static void EnsurePreferencesMethods(JNIEnv* env) {
    if (g_prefsChecked || !env) return;
    g_prefsChecked = true; // look up once: this class never exists in so-only processes
    if (env->ExceptionCheck()) env->ExceptionClear();
    jclass local = env->FindClass("com/android/support/Preferences");
    // Always fails for so-only injection. The exception MUST be cleared, or the
    // next JNI call aborts (clicking a button/slider crashed; dragging was fine
    // because it never reaches CallChanges).
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!local) return;
    g_prefs = (jclass)env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    if (!g_prefs) return;

    g_changeBool   = env->GetStaticMethodID(g_prefs, "changeFeatureBool",   "(Ljava/lang/String;IZ)V");
    g_changeInt    = env->GetStaticMethodID(g_prefs, "changeFeatureInt",    "(Ljava/lang/String;II)V");
    g_changeLong   = env->GetStaticMethodID(g_prefs, "changeFeatureLong",   "(Ljava/lang/String;IJ)V");
    g_changeString = env->GetStaticMethodID(g_prefs, "changeFeatureString", "(Ljava/lang/String;ILjava/lang/String;)V");
    if (env->ExceptionCheck()) env->ExceptionClear(); // missing methods -> null, callers check
}

// Single entry: dispatch by type to the matching Preferences method.
static void CallChanges(int type, int featNum, const std::string& name, int value, long lvalue, bool on, const std::string& text) {
    if (!g_jvm || !g_context) return;
    JNIEnv* env = GetEnv();
    if (!env) return;
    if (env->ExceptionCheck()) env->ExceptionClear();
    EnsurePreferencesMethods(env);
    if (!g_prefs) return;

    jstring jname = env->NewStringUTF(name.c_str());
    switch (type) {
        case 0: // bool
            if (g_changeBool) env->CallStaticVoidMethod(g_prefs, g_changeBool, jname, (jint)featNum, (jboolean)on);
            break;
        case 1: // int
            if (g_changeInt) env->CallStaticVoidMethod(g_prefs, g_changeInt, jname, (jint)featNum, (jint)value);
            break;
        case 2: // long
            if (g_changeLong) env->CallStaticVoidMethod(g_prefs, g_changeLong, jname, (jint)featNum, (jlong)lvalue);
            break;
        case 3: { // string
            if (g_changeString) {
                jstring jtext = env->NewStringUTF(text.c_str());
                env->CallStaticVoidMethod(g_prefs, g_changeString, jname, (jint)featNum, jtext);
                env->DeleteLocalRef(jtext);
            }
            break;
        }
        default:
            break;
    }
    env->DeleteLocalRef(jname);
    // Never let Java exceptions leave the render thread.
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// ===========================================================================
// State
// ===========================================================================
struct FeatureState {
    bool b = false;
    int  i = 0;
    long l = 0;
    std::string s;
};
static std::map<int, FeatureState> g_state;

static FeatureState& GetState(const Feature& f) {
    auto it = g_state.find(f.featNum);
    if (it == g_state.end()) {
        FeatureState st;
        st.b = f.defOn;
        if (f.type == "SeekBar") st.i = f.min;
        g_state[f.featNum] = st;
    }
    return g_state[f.featNum];
}

// ===========================================================================
// Fonts
// ===========================================================================
void InitFont() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefault();

    // Try Chinese-capable fonts by priority; fall back to the default font.
    const char* font_paths[] = {
            "/system/fonts/NotoSansCJK-Regular.ttc",
            "/system/fonts/NotoSansCJKsc-Regular.otf",
            "/system/fonts/DroidSansFallback.ttf",
            "/system/fonts/Roboto-Regular.ttf",
    };
    for (const char* path : font_paths) {
        ImFont* font = io.Fonts->AddFontFromFileTTF(path, 18.0f, nullptr,
                io.Fonts->GetGlyphRangesChineseFull());
        if (font) {
            io.FontDefault = font;
            LOGI("Loaded font: %s", path);
            return;
        }
    }
    LOGI("No custom font, using default");
}

// ===========================================================================
// Widgets
// ===========================================================================
static void RenderFeature(const Feature& f) {
    FeatureState& st = GetState(f);

    if (f.type == "Toggle" || f.type == "CheckBox") {
        bool v = st.b;
        if (ImGui::Checkbox(f.name.c_str(), &v)) {
            st.b = v;
            CallChanges(0, f.featNum, f.name, 0, 0, st.b, "");
            FeatureBus::SetBool(f.featNum, st.b);
        }
    } else if (f.type == "SeekBar") {
        int v = st.i;
        if (ImGui::SliderInt(f.name.c_str(), &v, f.min, f.max, "%d")) {
            if (v < f.min) v = f.min;
            st.i = v;
            CallChanges(1, f.featNum, f.name, v, 0, false, "");
            FeatureBus::SetInt(f.featNum, v);
        }
    } else if (f.type == "Button") {
        if (ImGui::Button(f.name.c_str(), ImVec2(-1, 0))) {
            CallChanges(1, f.featNum, f.name, 0, 0, false, "");
            FeatureBus::PushButton(f.featNum);
        }
    } else if (f.type == "ButtonOnOff") {
        ImVec4 col = st.b ? ImVec4(0.13f, 0.55f, 0.13f, 1.0f)
                          : ImVec4(0.55f, 0.15f, 0.15f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        if (ImGui::Button(f.name.c_str(), ImVec2(-1, 0))) {
            st.b = !st.b;
            CallChanges(0, f.featNum, f.name, 0, 0, st.b, "");
            FeatureBus::SetBool(f.featNum, st.b);
        }
        ImGui::PopStyleColor();
    } else if (f.type == "Spinner") {
        // Split comma-separated options.
        static std::map<int, std::vector<std::string>> g_items_cache;
        auto& items = g_items_cache[f.featNum];
        if (items.empty()) {
            for (const auto& it : Split(f.extra, ',')) items.push_back(it);
            if (items.empty()) items.push_back("Item");
        }
        // ImGui needs a const char* array.
        static std::map<int, std::vector<std::string>> g_item_pool;
        auto& pool = g_item_pool[f.featNum];
        pool = items;
        std::vector<const char*> cstrs;
        for (auto& p : pool) cstrs.push_back(p.c_str());
        int v = st.i;
        if (ImGui::Combo(f.name.c_str(), &v, cstrs.data(), (int)cstrs.size())) {
            st.i = v;
            CallChanges(1, f.featNum, items[v], v, 0, false, "");
            FeatureBus::SetInt(f.featNum, v);
        }
    } else if (f.type == "InputValue") {
        int v = st.i;
        if (ImGui::InputInt(f.name.c_str(), &v)) {
            if (f.max > 0 && v > f.max) v = f.max;
            st.i = v;
            CallChanges(1, f.featNum, f.name, v, 0, false, "");
            FeatureBus::SetInt(f.featNum, v);
        }
    } else if (f.type == "InputLValue") {
        long v = st.l;
        if (ImGui::InputScalar(f.name.c_str(), ImGuiDataType_S64, &v)) {
            if (f.maxLong > 0 && v > f.maxLong) v = f.maxLong;
            st.l = v;
            CallChanges(2, f.featNum, f.name, 0, v, false, "");
            FeatureBus::SetLong(f.featNum, (long long)v);
        }
    } else if (f.type == "InputText") {
        static std::map<int, std::array<char, 256>> g_text_buf;
        auto& buf = g_text_buf[f.featNum];
        if (ImGui::InputText(f.name.c_str(), buf.data(), buf.size())) {
            st.s = buf.data();
            CallChanges(3, f.featNum, f.name, 0, 0, false, st.s);
            FeatureBus::SetString(f.featNum, st.s);
        }
    } else if (f.type == "RadioButton") {
        auto options = Split(f.extra, ',');
        int idx = 0;
        for (const auto& opt : options) {
            // PushID to avoid duplicate labels.
            ImGui::PushID(f.featNum * 100 + idx);
            if (ImGui::RadioButton(opt.c_str(), &st.i, idx)) {
                CallChanges(1, f.featNum, f.name, idx, 0, false, "");
                FeatureBus::SetInt(f.featNum, idx);
            }
            ImGui::PopID();
            idx++;
        }
    } else if (f.type == "Category") {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.51f, 0.79f, 0.99f, 1.0f));
        ImGui::Separator();
        ImGui::TextUnformatted(f.name.c_str());
        ImGui::PopStyleColor();
    } else if (f.type == "Collapse") {
        ImGui::Separator();
        ImGui::CollapsingHeader(f.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        // following CollapseAdd_ items render into this section
    } else if (f.type == "ButtonLink") {
        if (ImGui::Button(f.name.c_str(), ImVec2(-1, 0))) {
            LOGI("ButtonLink: %s", f.extra.c_str());
            // Open links from the Java layer via an Intent.
        }
    } else if (f.type == "RichTextView" || f.type == "RichWebView") {
        // ImGui has no rich-text/web widget; render as plain text.
        ImGui::TextWrapped("%s", f.name.c_str());
    }
}

// ===========================================================================
// Draw
// ===========================================================================

// Cached menu rect for IsInsideMenu hit-testing (updated each frame).
static ImVec2 g_menu_pos  = ImVec2(0, 0);
static ImVec2 g_menu_size = ImVec2(0, 0);

void Draw() {
    static std::vector<Feature> features;
    static bool parsed = false;
    if (!parsed) {
        features = ParseFeatures();
        parsed = true;
    }

    // The overlay is a fixed-size window; the menu starts at its top-left.
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.10f, 0.15f, 0.95f));

    if (ImGui::Begin("Mod Menu", nullptr,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        for (auto& f : features) {
            RenderFeature(f);
        }
        // Record the menu rect for touch hit-testing.
        g_menu_pos  = ImGui::GetWindowPos();
        g_menu_size = ImGui::GetWindowSize();
    }
    ImGui::End();

    // Extra drawing (ESP/boxes/rays) goes here via the background draw list:
    //   ImDrawList* bg = ImGui::GetBackgroundDrawList();
    //   bg->AddRect(ImVec2(100,100), ImVec2(200,300), IM_COL32(255,60,60,220));
    // Read game memory only in a background thread and draw from that cache;
    // never read memory or call game functions on the render thread.

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

bool IsInsideMenu(float x, float y) {
    if (g_menu_size.x <= 0.f || g_menu_size.y <= 0.f) return false;
    return x >= g_menu_pos.x && x <= g_menu_pos.x + g_menu_size.x &&
           y >= g_menu_pos.y && y <= g_menu_pos.y + g_menu_size.y;
}

} // namespace MenuUI
