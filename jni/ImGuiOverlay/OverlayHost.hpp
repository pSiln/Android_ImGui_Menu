// OverlayHost: EGL host shared by the JNI and C-export paths.
#pragma once

#include <jni.h>

namespace OverlayHost {

bool Init(JavaVM* vm, jobject surface, jobject ctx);
void Shutdown();
bool IsReady();

void Touch(int action, float x, float y);
void Scroll(float delta_y);
bool HitTest(float x, float y);

} // namespace OverlayHost
