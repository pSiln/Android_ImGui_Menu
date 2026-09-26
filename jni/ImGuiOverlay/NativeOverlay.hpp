// NativeOverlay: fully-native floating window, no dex required.
#pragma once

#include <jni.h>

namespace NativeOverlay {

void SetVM(JavaVM* vm);
void StartAuto();

} // namespace NativeOverlay
