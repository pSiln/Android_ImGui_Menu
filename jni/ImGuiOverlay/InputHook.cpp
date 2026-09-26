// InputHook.cpp - in-process input interception via libinput consume hook.
//
// Hook point (verified on Android 16, auto-fallback to the older 4-arg form):
//   InputConsumer::consume(factory, batches, frameTime, outSeq, outEvent)
// Every swallowed event must be acknowledged or input ANR accumulates:
//   InputConsumer::sendFinishedSignal(seq, handled=true)
// We return -EAGAIN (WOULD_BLOCK) after swallowing, which
// NativeInputEventReceiver treats as "no event right now"; the looper keeps
// draining the fd on the next round, so game events are not blocked.
//
// Coordinates: prefer AMotionEvent_getRawX/Y (screen space, 1:1 with the
// fullscreen overlay), probed at runtime via dlsym; fall back to getX/getY
// plus the window offset.
//
#include "InputHook.hpp"

#include <android/input.h>
#include <android/log.h>
#include <atomic>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <string.h>

#include "Dobby/dobby.h"
#include "OverlayHost.hpp"

#define LOG_TAG "InputHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace InputHook {

namespace {

std::atomic<bool> g_started{false};
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_active{false};
std::atomic<bool> g_noFinishLogged{false};

// consume / finish signatures (probed at runtime; mangling varies by release)
using Consume5Fn = int32_t (*)(void*, void*, bool, int64_t, uint32_t*, void**);
using Consume4Fn = int32_t (*)(void*, void*, bool, int64_t, void**);
using FinishFn = int32_t (*)(void*, uint32_t, bool);
using GetSeqFn = uint32_t (*)(const void*);
using RawFn = float (*)(const AInputEvent*, size_t);

Consume5Fn g_orig5 = nullptr;
Consume4Fn g_orig4 = nullptr;
FinishFn g_finish = nullptr;
GetSeqFn g_getSeq = nullptr; // reads seq from the event for the old 4-arg form; null = pass through
RawFn g_rawX = nullptr;
RawFn g_rawY = nullptr;
// Gesture continuity: once a DOWN is swallowed, later events of the same
// pointer must keep going to the menu even if the finger leaves it, otherwise
// an UP delivered to the game leaves menu-side touch_down_ stuck. Mostly
// single-threaded (input looper); atomic for multi-channel cases.
std::atomic<int32_t> g_activePointer{-1};

float EvtX(const AInputEvent* ev, size_t idx) {
    if (g_rawX) return g_rawX(ev, idx);
    return AMotionEvent_getX(ev, idx) + AMotionEvent_getXOffset(ev);
}

float EvtY(const AInputEvent* ev, size_t idx) {
    if (g_rawY) return g_rawY(ev, idx);
    return AMotionEvent_getY(ev, idx) + AMotionEvent_getYOffset(ev);
}

// Menu hit + ackable -> Touch + finish and return true (caller returns
// WOULD_BLOCK). Anything else returns false and the event passes through.
bool HandleMotion(void* consumer, void* event, uint32_t seq) {
    const AInputEvent* ev = reinterpret_cast<const AInputEvent*>(event);
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return false;
    if (!OverlayHost::IsReady()) return false;

    int32_t action = AMotionEvent_getAction(ev);
    int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
    int touch = -1;
    switch (masked) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN: touch = 0; break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP: touch = 1; break;
        case AMOTION_EVENT_ACTION_MOVE: touch = 2; break;
        case AMOTION_EVENT_ACTION_CANCEL: touch = 3; break;
        default: return false; // hover etc. pass through
    }

    size_t count = AMotionEvent_getPointerCount(ev);
    if (count == 0) return false;
    int32_t pi =
        (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    size_t idx = (pi >= 0 && (size_t)pi < count) ? (size_t)pi : 0;
    int32_t pid = AMotionEvent_getPointerId(ev, idx);

    // Gesture continuity: skip HitTest for a pointer already down on the
    // menu; clear state on up/cancel to avoid a stuck gesture.
    int32_t active = g_activePointer.load(std::memory_order_relaxed);
    bool continuing = (active >= 0);
    if (continuing && pid != active) {
        if (masked == AMOTION_EVENT_ACTION_MOVE) {
            // NDK exposes no findPointerIndex; search by pointer id
            int32_t fi = -1;
            for (size_t k = 0; k < count; k++) {
                if (AMotionEvent_getPointerId(ev, k) == active) {
                    fi = (int32_t)k;
                    break;
                }
            }
            if (fi < 0) return false;
            idx = (size_t)fi;
            pid = active;
        } else {
            // Second finger: menu is single-touch, pass it to the game
            return false;
        }
    }

    float x = EvtX(ev, idx);
    float y = EvtY(ev, idx);

    if (!continuing) {
        if (!OverlayHost::HitTest(x, y)) return false; // outside the menu
        // POINTER_DOWN while a menu gesture is active: pass the second finger
        if (masked == AMOTION_EVENT_ACTION_POINTER_DOWN && active >= 0) return false;
    }

    if (seq == 0 || !g_finish) {
        // Cannot ack -> pass through: better the game gets the tap than an ANR.
        if (!g_noFinishLogged.exchange(true)) {
            LOGW("menu hit but cannot finish seq=%u, passthrough", seq);
        }
        return false;
    }
    OverlayHost::Touch(touch, x, y);
    if (!continuing && (masked == AMOTION_EVENT_ACTION_DOWN ||
                        masked == AMOTION_EVENT_ACTION_POINTER_DOWN)) {
        g_activePointer.store(pid, std::memory_order_relaxed);
    } else if (continuing && (masked == AMOTION_EVENT_ACTION_UP ||
                              masked == AMOTION_EVENT_ACTION_CANCEL ||
                              masked == AMOTION_EVENT_ACTION_POINTER_UP)) {
        g_activePointer.store(-1, std::memory_order_relaxed);
    }
    g_finish(consumer, seq, true);
    return true;
}

int32_t HookedConsume5(void* thiz, void* factory, bool batches, int64_t frameTime,
                       uint32_t* outSeq, void** outEvent) {
    int32_t st = g_orig5(thiz, factory, batches, frameTime, outSeq, outEvent);
    if (st != 0 || !outEvent || !*outEvent) return st;
    if (!g_enabled.load(std::memory_order_relaxed)) return st;
    uint32_t seq = outSeq ? *outSeq : 0;
    if (HandleMotion(thiz, *outEvent, seq)) return -EAGAIN; // WOULD_BLOCK
    return st;
}

int32_t HookedConsume4(void* thiz, void* factory, bool batches, int64_t frameTime,
                       void** outEvent) {
    int32_t st = g_orig4(thiz, factory, batches, frameTime, outEvent);
    if (st != 0 || !outEvent || !*outEvent) return st;
    if (!g_enabled.load(std::memory_order_relaxed)) return st;
    uint32_t seq = g_getSeq ? g_getSeq(*outEvent) : 0;
    if (HandleMotion(thiz, *outEvent, seq)) return -EAGAIN; // WOULD_BLOCK
    return st;
}

bool TryHookAddr(void* addr, void* hook, void** orig, const char* tag);
bool TryHook(void* h, const char* sym, void* hook, void** orig, const char* tag) {
    void* f = dlsym(h, sym);
    if (!f) return false;
    return TryHookAddr(f, hook, orig, tag);
}

bool TryHookAddr(void* addr, void* hook, void** orig, const char* tag) {
    if (!addr) return false;
    // Same casts as Macros.h DobbyHookWrapper.
    if (DobbyHook(addr, (dobby_dummy_func_t)hook, (dobby_dummy_func_t*)orig) != 0) {
        LOGW("DobbyHook failed: %s", tag);
        return false;
    }
    g_active.store(true);
    LOGI("input hook installed (%s)", tag);
    return true;
}

#if defined(__aarch64__)
// ---- Runtime scan of libinput's export table (mangling/OEM-proof) ----
// Only matches the "7consumeE" name boundary to avoid internal helpers
// such as consumeBatch.
struct ScanResult {
    char consumeName[256] = {0};
    void* consumeAddr = nullptr;
    bool consumeIs5 = false; // 'Pj' (outSeq) present -> 5-arg, else 4-arg
    char finishName[256] = {0};
    void* finishAddr = nullptr;
    char seqName[256] = {0};
    void* seqAddr = nullptr;
    bool done = false;
};

static void ScanNote(ScanResult* out, const char* nm, void* ad) {
    bool inConsumer = strstr(nm, "InputConsumer") != nullptr;
    if (inConsumer && strstr(nm, "7consumeE")) {
        bool is5 = strstr(nm, "Pj") != nullptr;
        // Prefer the 5-arg form; upgrade if we only saw the 4-arg one.
        if (!out->consumeAddr || (is5 && !out->consumeIs5)) {
            strncpy(out->consumeName, nm, sizeof(out->consumeName) - 1);
            out->consumeAddr = ad;
            out->consumeIs5 = is5;
        }
        return;
    }
    if (inConsumer && strstr(nm, "inishedSignal")) {
        // Exact sendFinishedSignal preferred over other FinishedSignal symbols.
        bool exact = strstr(nm, "sendFinishedSignal") != nullptr;
        bool curExact = out->finishAddr &&
                        strstr(out->finishName, "sendFinishedSignal") != nullptr;
        if (!out->finishAddr || (exact && !curExact)) {
            strncpy(out->finishName, nm, sizeof(out->finishName) - 1);
            out->finishAddr = ad;
        }
        return;
    }
    if (strstr(nm, "10InputEvent") && strstr(nm, "getSequenceNumber") && !out->seqAddr) {
        strncpy(out->seqName, nm, sizeof(out->seqName) - 1);
        out->seqAddr = ad;
    }
}

static int ScanCb(struct dl_phdr_info* info, size_t, void* data) {
    ScanResult* out = (ScanResult*)data;
    if (out->done) return 1;
    if (!info->dlpi_name || !strstr(info->dlpi_name, "libinput.so")) return 0;
    uintptr_t base = (uintptr_t)info->dlpi_addr;
    // Sanity: confirm an ELF mapping.
    if (memcmp((void*)base, "\x7f"
                            "ELF",
               4) != 0)
        return 0;
    const Elf64_Dyn* dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = (const Elf64_Dyn*)(base + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return 0;
    const Elf64_Sym* symtab = nullptr;
    const char* strtab = nullptr;
    const uint32_t* hash = nullptr;
    const uint32_t* gnu = nullptr;
    for (const Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_SYMTAB) symtab = (const Elf64_Sym*)(base + d->d_un.d_ptr);
        else if (d->d_tag == DT_STRTAB) strtab = (const char*)(base + d->d_un.d_ptr);
        else if (d->d_tag == DT_HASH) hash = (const uint32_t*)(base + d->d_un.d_ptr);
        else if (d->d_tag == DT_GNU_HASH) gnu = (const uint32_t*)(base + d->d_un.d_ptr);
    }
    if (!symtab || !strtab || (!hash && !gnu)) return 0;
    size_t n = 0;
    if (hash) {
        n = hash[1]; // nchain
    } else {
        uint32_t nbuckets = gnu[0], symoffset = gnu[1], bloomSize = gnu[2];
        if (nbuckets > 4096 || symoffset > 100000) return 0;
        uintptr_t bloom = (uintptr_t)(gnu + 4);
        uintptr_t buckets = bloom + (uintptr_t)bloomSize * 8; // 64-bit bloom
        uintptr_t chain = buckets + (uintptr_t)nbuckets * 4;
        size_t maxSym = symoffset;
        for (uint32_t b = 0; b < nbuckets; b++) {
            uint32_t idx = *(const uint32_t*)(buckets + (uintptr_t)b * 4);
            if (idx < symoffset) continue;
            for (int k = 0; k < 8192; k++) {
                if (idx - symoffset > 100000) break; // corrupt-table guard
                if (idx > maxSym) maxSym = idx;
                uint32_t h2 = *(const uint32_t*)(chain + (uintptr_t)(idx - symoffset) * 4);
                if (h2 & 1) break;
                idx++;
            }
        }
        n = maxSym + 1;
        if (n == 0 || n > 200000) return 0;
    }
    for (size_t i = 0; i < n; i++) {
        const Elf64_Sym* s = &symtab[i];
        if (s->st_shndx == SHN_UNDEF) continue;
        if (ELF64_ST_TYPE(s->st_info) != STT_FUNC) continue;
        if (s->st_name == 0) continue;
        ScanNote(out, strtab + s->st_name, (void*)(base + s->st_value));
    }
    out->done = true;
    return 1;
}

static void ScanAndHook() {
    ScanResult sr;
    dl_iterate_phdr(ScanCb, &sr);
    if (sr.finishName[0] && !g_finish) {
        g_finish = (FinishFn)sr.finishAddr;
        LOGI("finish resolved by scan: %s", sr.finishName);
    }
    if (sr.seqName[0] && !g_getSeq) {
        g_getSeq = (GetSeqFn)sr.seqAddr;
        LOGI("getSeq resolved by scan: %s", sr.seqName);
    }
    if (!sr.consumeAddr) {
        LOGE("scan: no InputConsumer::consume in libinput exports");
        return;
    }
    LOGI("scan found consume: %s", sr.consumeName);
    if (sr.consumeIs5) {
        TryHookAddr(sr.consumeAddr, (void*)HookedConsume5, (void**)&g_orig5, "consume5(scan)");
    } else {
        TryHookAddr(sr.consumeAddr, (void*)HookedConsume4, (void**)&g_orig4, "consume4(scan)");
    }
}
#else
static void ScanAndHook() {
    LOGW("scan skipped (non-arm64)");
}
#endif

} // namespace

void Start() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    try {
        void* h = dlopen("libinput.so", RTLD_NOW);
        if (!h) {
            LOGE("libinput.so not found, touch disabled");
            return;
        }
        // Swallowed events must be acked or input ANRs; without the symbol we
        // install nothing and keep display-only mode.
        g_finish = (FinishFn)dlsym(h, "_ZN7android13InputConsumer18sendFinishedSignalEjb");
        if (!g_finish) {
            LOGE("sendFinishedSignal missing, touch disabled (display only)");
            return;
        }
        // For the old signature only (the new one uses outSeq).
        g_getSeq = (GetSeqFn)dlsym(h, "_ZNK7android10InputEvent17getSequenceNumberEv");
        // Raw screen coordinates (newer API, probed at runtime).
        void* ha = dlopen("libandroid.so", RTLD_NOW);
        if (ha) {
            g_rawX = (RawFn)dlsym(ha, "AMotionEvent_getRawX");
            g_rawY = (RawFn)dlsym(ha, "AMotionEvent_getRawY");
            // Symbol addresses stay valid for the process lifetime; don't dlclose.
        }

        // Android 16 (nsecs_t == long) / long long variants, plus fully
        // qualified spellings to survive compiler substitution differences.
        const char* c5[] = {
            "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",
            "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPjPPNS_10InputEventE",
            "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPN7android10InputEventE",
            "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPjPPN7android10InputEventE",
            nullptr,
        };
        for (int i = 0; c5[i]; i++) {
            if (TryHook(h, c5[i], (void*)HookedConsume5, (void**)&g_orig5, "consume5")) return;
        }
        // Older 4-arg fallbacks.
        const char* c4[] = {
            "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPPNS_10InputEventE",
            "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPPNS_10InputEventE",
            nullptr,
        };
        for (int i = 0; c4[i]; i++) {
            if (TryHook(h, c4[i], (void*)HookedConsume4, (void**)&g_orig4, "consume4")) return;
        }
        // All hardcoded candidates missed: take the real name from the export table.
        LOGW("candidate symbols missed, scanning libinput exports...");
        ScanAndHook();
        if (g_active.load()) return;
        LOGE("input hook: no consume symbol found, touch disabled (display only)");
    } catch (...) {
        LOGE("input hook: exception during install");
    }
}

void SetEnabled(bool enabled) {
    g_enabled.store(enabled);
}

bool IsActive() {
    return g_active.load();
}

} // namespace InputHook
