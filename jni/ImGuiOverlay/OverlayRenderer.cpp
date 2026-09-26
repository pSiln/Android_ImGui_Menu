// OverlayRenderer.cpp - ImGui overlay rendering on its own EGL context.
//
#include "OverlayRenderer.hpp"

#include <android/log.h>
#include <chrono>
#include <thread>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "MenuUI.hpp"

#define LOG_TAG "ImGuiOverlay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

OverlayRenderer& OverlayRenderer::Get() {
    static OverlayRenderer instance;
    return instance;
}

static const char* EglErrorStr(EGLint e) {
    switch (e) {
        case EGL_SUCCESS: return "SUCCESS";
        case EGL_BAD_DISPLAY: return "BAD_DISPLAY";
        case EGL_BAD_SURFACE: return "BAD_SURFACE";
        case EGL_BAD_CONTEXT: return "BAD_CONTEXT";
        case EGL_BAD_ALLOC: return "BAD_ALLOC";
        case EGL_BAD_MATCH: return "BAD_MATCH";
        case EGL_BAD_CONFIG: return "BAD_CONFIG";
        case EGL_BAD_ATTRIBUTE: return "BAD_ATTRIBUTE";
        case EGL_BAD_PARAMETER: return "BAD_PARAMETER";
        case EGL_NOT_INITIALIZED: return "NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "BAD_ACCESS";
        case EGL_BAD_NATIVE_WINDOW: return "BAD_NATIVE_WINDOW";
        case EGL_BAD_CURRENT_SURFACE: return "BAD_CURRENT_SURFACE";
        default: return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// EGL init / teardown (config fallbacks: some devices lack an RGBA8888
// window config)
// ---------------------------------------------------------------------------
bool OverlayRenderer::CreateEGL() {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed: %s", EglErrorStr(eglGetError()));
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(display_, &major, &minor)) {
        LOGE("eglInitialize failed: %s", EglErrorStr(eglGetError()));
        display_ = EGL_NO_DISPLAY;
        return false;
    }

    // Candidate configs: RGBA8888 transparent first, then RGB565 (opaque
    // fallback), then whatever eglChooseConfig accepts.
    const EGLint* candidates[] = {
        (const EGLint[]){ EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                          EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                          EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                          EGL_ALPHA_SIZE, 8, EGL_NONE },
        (const EGLint[]){ EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                          EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                          EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5,
                          EGL_NONE },
        nullptr,
    };

    EGLint num_configs = 0;
    bool picked = false;
    for (int i = 0; candidates[i] != nullptr && !picked; i++) {
        if (eglChooseConfig(display_, candidates[i], &config_, 1, &num_configs) && num_configs >= 1) {
            picked = true;
            LOGI("eglChooseConfig ok (level %d)", i);
        } else {
            EGLint e = eglGetError();
            LOGW("eglChooseConfig level %d failed: %s (0x%x)", i, EglErrorStr(e), e);
        }
    }

    // Last resort: some drivers reject every attribute list, so enumerate all
    // configs and score them (window + ES2 required, alpha preferred).
    if (!picked) {
        EGLint total = 0;
        if (eglGetConfigs(display_, nullptr, 0, &total) && total > 0) {
            std::vector<EGLConfig> all((size_t)total);
            EGLint got = 0;
            if (eglGetConfigs(display_, all.data(), total, &got) && got > 0) {
                int best = -1, bestScore = -1000000;
                for (int i = 0; i < got; i++) {
                    EGLint st = 0, rt = 0, r = 0, g = 0, b = 0, a = 0, d = 0;
                    eglGetConfigAttrib(display_, all[i], EGL_SURFACE_TYPE, &st);
                    eglGetConfigAttrib(display_, all[i], EGL_RENDERABLE_TYPE, &rt);
                    if (!(st & EGL_WINDOW_BIT)) continue;
                    if (!(rt & EGL_OPENGL_ES2_BIT)) continue;
                    eglGetConfigAttrib(display_, all[i], EGL_RED_SIZE, &r);
                    eglGetConfigAttrib(display_, all[i], EGL_GREEN_SIZE, &g);
                    eglGetConfigAttrib(display_, all[i], EGL_BLUE_SIZE, &b);
                    eglGetConfigAttrib(display_, all[i], EGL_ALPHA_SIZE, &a);
                    eglGetConfigAttrib(display_, all[i], EGL_DEPTH_SIZE, &d);
                    int score = r + g + b + a;
                    if (a >= 8) score += 500;
                    else if (a > 0) score += 100;
                    if (d >= 16) score += 50;
                    if (score > bestScore) { bestScore = score; best = i; }
                }
                if (best >= 0) {
                    config_ = all[best];
                    picked = true;
                    LOGI("egl fallback picked %d/%d (score %d)", best, got, bestScore);
                } else {
                    LOGW("egl fallback: no ES2 window config in %d configs", got);
                }
            }
        }
    }
    if (!picked) {
        EGLint e = eglGetError();
        LOGE("eglChooseConfig all failed: %s (0x%x)", EglErrorStr(e), e);
        return false;
    }

    // Critical for transparency: match the Surface buffer format to the EGL
    // config, otherwise the overlay shows a black background on some devices.
    {
        EGLint vis = 0;
        if (eglGetConfigAttrib(display_, config_, EGL_NATIVE_VISUAL_ID, &vis) && vis != 0) {
            ANativeWindow_setBuffersGeometry(window_, 0, 0, vis);
        }
    }

    const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attribs);
    if (context_ == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: %s", EglErrorStr(eglGetError()));
        return false;
    }

    if (!RecreateSurface()) {
        return false;
    }

    EGLint w = 0, h = 0;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &w);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &h);
    width_ = (int)w;
    height_ = (int)h;
    if (width_ <= 0 || height_ <= 0) {
        LOGE("bad surface size %dx%d", width_, height_);
        return false;
    }
    LOGI("EGL surface size: %dx%d (EGL %d.%d)", width_, height_, major, minor);

    // ImGui context is created here once and destroyed in DestroyEGL.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)width_, (float)height_);
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    MenuUI::InitFont();

    if (!ImGui_ImplOpenGL3_Init("#version 100")) {
        LOGE("ImGui_ImplOpenGL3_Init failed");
        ImGui::DestroyContext();
        return false;
    }

    return true;
}

bool OverlayRenderer::RecreateSurface() {
    if (display_ == EGL_NO_DISPLAY || config_ == nullptr || window_ == nullptr) return false;
    // Release the old surface first (recreation path).
    if (surface_ != EGL_NO_SURFACE) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
    }
    surface_ = eglCreateWindowSurface(display_, config_, window_, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed: %s", EglErrorStr(eglGetError()));
        return false;
    }
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        LOGE("eglMakeCurrent failed: %s", EglErrorStr(eglGetError()));
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
        return false;
    }
    return true;
}

void OverlayRenderer::DestroyEGL() {
    // Caller must hold egl_mutex_.
    if (display_ != EGL_NO_DISPLAY) {
        // ImGui is bound to the context: shut it down before unbinding EGL.
        ImGui_ImplOpenGL3_Shutdown();
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();

        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        if (surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }
    config_ = nullptr;
    window_ = nullptr;
    width_ = height_ = 0;
    ready_.store(false);
}

bool OverlayRenderer::Init(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(egl_mutex_);
    if (window == nullptr) {
        LOGE("Init: null window");
        return false;
    }
    if (ready_.load() && window_ == window) {
        return true; // duplicate surfaceCreated, idempotent
    }
    if (ready_.load() && window_ != window) {
        // Rotation / surface recreation: swap only the surface if possible.
        window_ = window;
        if (RecreateSurface()) {
            EGLint w = 0, h = 0;
            eglQuerySurface(display_, surface_, EGL_WIDTH, &w);
            eglQuerySurface(display_, surface_, EGL_HEIGHT, &h);
            if (w > 0 && h > 0) {
                width_ = w; height_ = h;
                ImGui::GetIO().DisplaySize = ImVec2((float)w, (float)h);
                LOGI("surface recreated %dx%d", width_, height_);
                return true;
            }
        }
        LOGE("surface recreate failed, full reinit");
        DestroyEGL();
    }
    window_ = window;
    if (!CreateEGL()) {
        LOGE("CreateEGL failed");
        DestroyEGL();
        return false;
    }
    ready_.store(true);
    LOGI("OverlayRenderer initialized");
    return true;
}

void OverlayRenderer::Shutdown() {
    StopRenderThread(); // stop the thread first so RenderLoop stops touching EGL/ImGui
    std::lock_guard<std::mutex> lock(egl_mutex_);
    DestroyEGL();
}

// ---------------------------------------------------------------------------
// Render thread: operates only on our own EGL surface; never touches the
// game's EGL and never hooks eglSwapBuffers.
// ---------------------------------------------------------------------------
void OverlayRenderer::RenderLoop() {
    LOGI("Render thread started");
    constexpr auto frame_duration = std::chrono::milliseconds(16);
    int frame = 0;

    while (running_.load()) {
        if (!ready_.load()) {
            std::this_thread::sleep_for(frame_duration);
            continue;
        }

        // Guard against a dead surface (app backgrounded / window removed).
        {
            std::lock_guard<std::mutex> lock(egl_mutex_);
            if (display_ == EGL_NO_DISPLAY || surface_ == EGL_NO_SURFACE ||
                context_ == EGL_NO_CONTEXT) {
                std::this_thread::sleep_for(frame_duration);
                continue;
            }
            // Keep the context current (some ROMs drop it on background).
            if (eglGetCurrentContext() != context_) {
                if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
                    LOGE("eglMakeCurrent in loop failed: %s", EglErrorStr(eglGetError()));
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }
            }
        }

        if (!ImGui::GetCurrentContext()) {
            std::this_thread::sleep_for(frame_duration);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(input_mutex_);
            ImGuiIO& io = ImGui::GetIO();
            if (width_ > 0 && height_ > 0) io.DisplaySize = ImVec2((float)width_, (float)height_);
            io.AddMousePosEvent(touch_x_, touch_y_);
            io.AddMouseButtonEvent(0, touch_down_);
            if (scroll_delta_ != 0.f) {
                io.AddMouseWheelEvent(0.f, scroll_delta_);
                scroll_delta_ = 0.f;
            }
            if (scroll_accum_ != 0.f) {
                if (!ImGui::IsAnyItemActive() && !io.WantTextInput) {
                    io.AddMouseWheelEvent(0.f, scroll_accum_ / 40.0f);
                }
                scroll_accum_ = 0.f;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        MenuUI::Draw();

        ImGui::Render();

        glViewport(0, 0, width_, height_);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        EGLBoolean ok;
        {
            // Serialize swap against recreation to avoid surfaceDestroyed races.
            std::lock_guard<std::mutex> lock(egl_mutex_);
            if (!running_.load() || !ready_.load()) break;
            ok = eglSwapBuffers(display_, surface_);
        }
        if (!ok) {
            EGLint err = eglGetError();
            if (err == EGL_BAD_SURFACE || err == EGL_BAD_ALLOC || err == EGL_BAD_DISPLAY) {
                LOGE("eglSwapBuffers bad surface (%s), wait for new surface", EglErrorStr(err));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }
        }

        // Periodically sync the real size (rotation / split screen).
        if ((++frame % 60) == 0) {
            std::lock_guard<std::mutex> lock(egl_mutex_);
            if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) {
                EGLint w = 0, h = 0;
                eglQuerySurface(display_, surface_, EGL_WIDTH, &w);
                eglQuerySurface(display_, surface_, EGL_HEIGHT, &h);
                if (w > 0 && h > 0 && (w != width_ || h != height_)) {
                    width_ = w; height_ = h;
                    LOGI("surface resized %dx%d", width_, height_);
                }
            }
        }

        std::this_thread::sleep_for(frame_duration);
    }
    LOGI("Render thread stopped");
}

void OverlayRenderer::StartRenderThread() {
    if (running_.load()) return;
    running_.store(true);
    try {
        render_thread_ = std::thread(&OverlayRenderer::RenderLoop, this);
    } catch (...) {
        LOGE("create render thread failed");
        running_.store(false);
    }
}

void OverlayRenderer::StopRenderThread() {
    if (!running_.load()) return;
    running_.store(false);
    // egl_mutex_ is only held briefly around swap; join cannot deadlock.
    if (render_thread_.joinable()) {
        try { render_thread_.join(); } catch (...) {}
    }
}

// ---------------------------------------------------------------------------
// Touch input
// ---------------------------------------------------------------------------
void OverlayRenderer::OnTouchEvent(int action, float x, float y) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    switch (action) {
        case 0: // DOWN
            touch_x_ = x;
            touch_y_ = y;
            last_touch_y_ = y;
            touch_down_ = true;
            break;
        case 1: // UP
        case 3: // CANCEL
            touch_down_ = false;
            break;
        case 2: // MOVE
            touch_x_ = x;
            touch_y_ = y;
            scroll_accum_ += (y - last_touch_y_);
            last_touch_y_ = y;
            break;
        default:
            break;
    }
}

void OverlayRenderer::OnScroll(float delta_y) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    scroll_delta_ += delta_y;
}
