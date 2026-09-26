// OverlayRenderer: ImGui overlay on its own EGL context (OpenGL ES 2.0).
#pragma once

#include <android/native_window.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <atomic>
#include <mutex>
#include <thread>

class OverlayRenderer {
public:
    static OverlayRenderer& Get();

    bool Init(ANativeWindow* window);
    void Shutdown();

    bool IsReady() const { return ready_.load(); }
    int Width() const { return width_; }
    int Height() const { return height_; }

    void StartRenderThread();
    void StopRenderThread();

    void OnTouchEvent(int action, float x, float y);
    void OnScroll(float delta_y);

private:
    OverlayRenderer() = default;
    ~OverlayRenderer() = default;
    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    void RenderLoop();
    bool CreateEGL();
    void DestroyEGL();
    bool RecreateSurface();

    ANativeWindow* window_ = nullptr;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig  config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;

    int width_ = 0;
    int height_ = 0;

    std::atomic<bool> ready_{false};
    std::atomic<bool> running_{false};
    std::thread render_thread_;

    mutable std::mutex egl_mutex_;

    std::mutex input_mutex_;
    float touch_x_ = 0.f;
    float touch_y_ = 0.f;
    float last_touch_y_ = 0.f;
    bool  touch_down_ = false;
    float scroll_delta_ = 0.f;
    float scroll_accum_ = 0.f;
};
