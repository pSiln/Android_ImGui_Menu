# Android Mod Menu — 通用注入模板（纯 so 自动悬浮窗 + ImGui）
#基于 https://github.com/LGLTeam/Android-Mod-Menu

不依赖 Unity / UE / 任何游戏引擎的 Android 通用注入模板。把 so 注入到任意 App 后，**不需要任何 dex**，
就会自动创建一个全屏透明悬浮窗，在里面跑一套自己的 EGL + ImGui 菜单；在游戏进程内拦截触摸，
菜单内的点击自己消费，菜单外的事件原样放行给游戏。

- 不 hook `eglSwapBuffers`，不碰游戏的渲染管线（绘制走独立 Surface + 自建 EGL 上下文）
- 渲染 / 菜单 / 触摸 全部在 native 层完成，任何引擎、任何 App 通用
- 同时保留传统改包路径（自带 dex 的 Java 菜单），两者共存时自动让路，不会建两个窗
- 内置 Dobby hook / patch 宏、KittyMemory、xDL 符号定位，开箱即可写功能

## 目录结构

```
app/src/main/
├── jni/
│   ├── Main.cpp                 # 模板入口：GetFeatureList / Changes() / hack_thread
│   ├── Android.mk Application.mk# 构建
│   ├── ImGuiOverlay/            # 核心：悬浮窗 + ImGui + 触摸
│   │   ├── NativeOverlay.cpp    #   纯 native 自动建窗（dlsym + WindowManager.addView）
│   │   ├── OverlayHost.cpp      #   JNI / C 导出共用的宿主
│   │   ├── OverlayRenderer.cpp  #   自建 EGL 上下文 + ImGui 渲染线程
│   │   ├── InputHook.cpp        #   进程内 hook libinput，拦截系统触摸
│   │   ├── MenuUI.cpp           #   ImGui 菜单渲染 / 功能解析
│   │   ├── Features.h           #   ★ 功能列表单一数据源（改这里）
│   │   ├── FeatureBus.hpp       #   菜单状态总线（开关值给 hack_thread 读）
│   │   └── BridgeAPI.cpp        #   ModMenu_* C 导出（注入器用 dlsym 调）
│   ├── Menu/                    # JNI_OnLoad、Java 菜单注册、权限检查
│   ├── Includes/                # Logger / Utils(xDL) / Macros(Dobby/PATCH 宏)
│   └── Dobby KittyMemory xDL imgui/  # 第三方库
├── java/com/android/support/    # 改包路径的 Java 层（Menu/Preferences/Launcher…）
├── libs/arm64-v8a/libMyLibName.so     # ndk-build 产物
```

## 构建

```

手动 `ndk-build -C app/src/main/jni`。
## 用法

### 1. 纯 so 注入（推荐，无需 dex）

用任意注入器把 `libMyLibName.so` dlopen 进目标进程即可。`JNI_OnLoad` 会自动：
存 JavaVM → 起线程等 Application 就绪 → `WindowManager.addView` 建全屏透明 SurfaceView →
等 Surface 合法 → 起 EGL + ImGui 渲染线程 → 装触摸 hook。全程只用 `android.*` 系统类。

注入器侧也可以手动控制（`dlsym`）：

```c
int  (*init)(JavaVM*, jobject, jobject) = dlsym(h, "ModMenu_InitOverlay");
void (*setTouch)(int)                    = dlsym(h, "ModMenu_SetTouchEnabled");
int  (*ready)()                          = dlsym(h, "ModMenu_IsReady");
init(vm, surface, appContext);   // 1 = ok；ctx 可传 null（只用 FeatureBus）
```

### 2. 改包路径（自带 dex）

把 so 打进 APK，`Main.Start(context)` 拉起 Java 菜单 + `ImGuiOverlayView` 悬浮层。
此时 `NativeOverlay` 检测到 `com.android/support/ImGuiOverlayView` 存在会自动跳过，避免重复建窗。

### 悬浮窗权限

`addView` 失败通常是没给悬浮窗权限（只打日志，游戏不会崩）：

```sh
appops set <游戏包名> SYSTEM_ALERT_WINDOW allow
```

## ModMenu_* 导出一览（BridgeAPI.cpp）

| 导出 | 说明 |
| --- | --- |
| `ModMenu_InitOverlay(vm, surface, ctx)` | 初始化悬浮绘制 + 渲染线程，返回 1 成功 |
| `ModMenu_AutoOverlay()` | 手动触发自动建窗（幂等，通常不用调） |
| `ModMenu_ShutdownOverlay()` | 销毁 |
| `ModMenu_Touch(action, x, y)` | 外部注入触摸（0=DOWN 1=UP 2=MOVE 3=CANCEL） |
| `ModMenu_HitTest(x, y)` | 触摸点是否在菜单内（1 消费 / 0 放行） |
| `ModMenu_SetTouchEnabled(en)` | 触摸总开关（0 = 全部放行给游戏） |
| `ModMenu_IsReady()` | 渲染是否就绪 |
| `ModMenu_GetBool/GetInt/GetLong/GetString(feat[, out, len])` | 读菜单状态 |
| `ModMenu_ConsumeButton(feat)` | 取走按钮点击次数并清零 |

## 写功能（两步）

1. 在 `jni/ImGuiOverlay/Features.h` 里加一行，例如 `"SeekBar_Score multiplier_1_100"`。
   编号规则与原 Java 菜单一致：首段纯数字 = 显式 `featNum`，`_True` = 默认开，
   `CollapseAdd_` = 折叠组内项，`Category/Collapse/ButtonLink/RichText*` 不占编号。
2. 在 `jni/Main.cpp` 的 `Changes()`（改包路径）和 `hack_thread()`（轮询 FeatureBus）
   里按 `featNum` 写逻辑，hook/patch 用 `Macros.h` 的 `HOOK/PATCH/PATCH_SWITCH`：

```cpp
#define targetLibName OBFUSCATE("libil2cpp.so")
PATCH_SWITCH(targetLibName, "0x123456", "C0 03 5F D6", on);   // ret
HOOK(targetLibName, "0x123456", my_hook, my_orig);
```

渲染线程只画缓存数据；读游戏内存请放后台线程，别在 `MenuUI::Draw()` 里直接读。

## 触摸是怎么通的（InputHook.cpp）

悬浮窗本身是 `NOT_TOUCHABLE` 的（不挡游戏操作），所以在游戏进程内 hook
`android::InputConsumer::consume`：

- 命中菜单 → 转发给 ImGui + `sendFinishedSignal(seq, true)` 回执 → 返回 `-EAGAIN`（WOULD_BLOCK）
- 未命中 → 原样放行，游戏完全无感
- 手势连续性：DOWN 被吞后同一根手指后续事件不再 HitTest，避免 UP 漏给游戏导致菜单卡住
- 符号解析：先试硬编码候选（Android 16 是 5 参签名，老版本 4 参），全 miss 再扫
  `libinput.so` 导出表拿真名（`dl_iterate_phdr` + DT_SYMTAB/DT_GNU_HASH），防 OEM 改签名
- 拿不到 `sendFinishedSignal` 就整个不装 hook，**只显示不吞输入**（宁可不能点也不能 ANR）

## 日志 / 排障

先打印悬浮窗权限状态，再吐历史缓冲里的命中行（含崩溃栈），最后实时跟随。

常见问题：

| 现象 | 原因 / 处理 |
| --- | --- |
| 有窗口但点不动 | `sendFinishedSignal` 没解析到（日志有 `touch disabled`），触摸降级为纯显示 |
| 完全没窗口 | 悬浮窗权限没给（`addView failed (overlay permission?)`），见上面 appops 命令 |
| 窗口是黑底不透明 | EGL config 没带 alpha，日志看 `egl fallback picked`；一般重装即可 |
| `Target lib not found` | `targetLibName` 填错或目标 so 未加载，线程会继续轮询，不崩 |

## 兼容性

- ABI：`arm64-v8a`（NDK r29 起官方移除 32 位 ARM target；老 NDK 可自行加回 v7a）
- minSdk 21，实测 Android 16；低版本自动降级探测符号
- 第三方库：[Dear ImGui](https://github.com/ocornut/imgui)、[Dobby](https://github.com/jmpews/Dobby)、
  KittyMemory、[xDL](https://github.com/cleverwhen/xdl)（原模板思路来自 LGLTeam / MJ 等开源项目）

## 免责

仅供学习与授权测试使用。
