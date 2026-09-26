// Hook/patch helper macros built on Dobby + KittyMemory (offset or symbol).
// thanks to shmoo and joeyjurjens for the original wrappers.
#ifndef ANDROID_MOD_MENU_MACROS_H
#define ANDROID_MOD_MENU_MACROS_H

#include "KittyMemory/MemoryPatch.hpp"
#include "KittyMemory/KittyInclude.hpp"
#include "KittyMemory/Deps/Keystone/includes/keystone.h"
#include "Dobby/dobby.h"

#if defined(__aarch64__)
int MP_ASM = 1;
#else
int MP_ASM = 0;
#endif

#define HOOK(lib, off_sym, ptr, orig) DobbyHookWrapper(lib, OBFUSCATE(off_sym), (void*)(ptr), (void**)&(orig))
#define HOOK_NO_ORIG(lib, off_sym, ptr) DobbyHookWrapper(lib, OBFUSCATE(off_sym), (void*)(ptr), nullptr)

void DobbyHookWrapper(const char *lib, const char *relative, void* hook_function, void** original_function) {
    if (lib == nullptr || relative == nullptr || hook_function == nullptr) return;
    if (!isLibraryLoaded(lib)) {
        LOGW(OBFUSCATE("HOOK skipped, %s not loaded yet (%s)"), lib, relative);
        return;
    }
    void *abs = getAbsoluteAddress(lib, relative);
    if (abs == nullptr) {
        LOGE(OBFUSCATE("HOOK FAILED (null addr): %s"), relative);
        return;
    }
    int res = -1;
    if (original_function != nullptr) {
        res = DobbyHook(abs, (dobby_dummy_func_t)hook_function, (dobby_dummy_func_t*)original_function);
    } else {
        res = DobbyHook(abs, (dobby_dummy_func_t)hook_function, nullptr);
    }
    if (res < 0) LOGE(OBFUSCATE("HOOK FAILED: %s"), relative);
}

#define INST(lib, off_sym, name, boolean) DobbyInstrumentWrapper(lib, OBFUSCATE(off_sym), OBFUSCATE(name), boolean)

std::map<void*, const char*> detecting_functions;
void Detector(void *address, DobbyRegisterContext *ctx) {
    if (detecting_functions.count(address)) LOGW(OBFUSCATE("%s executed"), detecting_functions[address]);
}

void DobbyInstrumentWrapper(const char *lib, const char *relative, const char *name, bool apply) {
    if (lib == nullptr || relative == nullptr) return;
    if (!isLibraryLoaded(lib)) return;
    void *abs = getAbsoluteAddress(lib, relative);
    if (abs == nullptr) return;
    if (detecting_functions.count(abs)) {
        if (!apply) {
            if (DobbyDestroy(abs) == 0) {
                LOGI(OBFUSCATE("INST killed: %s"), detecting_functions[abs]);
                detecting_functions.erase(abs);
            } else {
                LOGE(OBFUSCATE("INST kill failed: %s"), detecting_functions[abs]);
            }
        }
    } else {
        if (apply) {
            if (DobbyInstrument(abs, (dobby_instrument_callback_t)(Detector)) == 0) {
                LOGI(OBFUSCATE("INST run: %s"), name);
                detecting_functions[abs] = name;
            } else {
                LOGE(OBFUSCATE("INST run failed: %s"), name);
            }
        }
    }
}

struct DobbyPatchInfo {
    void* address{};
    std::vector<uint8_t> original_bytes;
    std::vector<uint8_t> patch_bytes;
    bool applied{};
};

std::map<std::string, DobbyPatchInfo> pExpress;
void DobbyPatchWrapper(const char *libName, const char *relative, std::string data, bool apply) {
    if (libName == nullptr || relative == nullptr) return;
    if (!isLibraryLoaded(libName)) {
        LOGW(OBFUSCATE("PATCH skipped, %s not loaded yet (%s)"), libName, relative);
        return;
    }
    std::string key = relative;
    auto it = pExpress.find(key);
    void* abs = nullptr;

    if (it != pExpress.end()) {
        abs = it->second.address;
    } else {
        abs = getAbsoluteAddress(libName, relative);
        if (!abs) {
            LOGE(OBFUSCATE("Failed to get absolute address for %s"), relative);
            return;
        }

        DobbyPatchInfo info;
        info.address = abs;
        info.applied = false;

        std::string asm_data = data;
        if (KittyUtils::String::ValidateHex(data)) {
            size_t patch_size = data.length() / 2;
            info.patch_bytes.resize(patch_size);
            KittyUtils::dataFromHex(data, info.patch_bytes.data());
        } else {
            ks_engine *ks = nullptr;
            ks_err err = (MP_ASM == 1) ? ks_open(KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN, &ks)
                                       : ks_open(KS_ARCH_ARM, KS_MODE_LITTLE_ENDIAN, &ks);
            if (err != KS_ERR_OK) {
                KITTY_LOGE(OBFUSCATE("ks_open failed: %s"), ks_strerror(err));
                return;
            }

            unsigned char *insn_bytes = nullptr;
            size_t insn_size = 0, insn_count = 0;
            if (ks_asm(ks, asm_data.c_str(), 0, &insn_bytes, &insn_size, &insn_count) == 0 &&
                insn_bytes != nullptr && insn_size > 0) {
                info.patch_bytes.resize(insn_size);
                memcpy(info.patch_bytes.data(), insn_bytes, insn_size);
            } else {
                KITTY_LOGE(OBFUSCATE("ks_asm failed for %s"), relative);
                if (insn_bytes) ks_free(insn_bytes);
                ks_close(ks);
                return;
            }
            if (insn_bytes) ks_free(insn_bytes);
            ks_close(ks);
        }

        info.original_bytes.resize(info.patch_bytes.size());
        memcpy(info.original_bytes.data(), info.address, info.patch_bytes.size());
        pExpress[key] = info;
    }

    DobbyPatchInfo& info = pExpress[key];

    if (apply) {
        if (!info.applied) {
            int res = DobbyCodePatch(info.address, info.patch_bytes.data(), info.patch_bytes.size());
            if (res == 0) {
                info.applied = true;
            } else {
                LOGE(OBFUSCATE("Failed to create patch: %s, error: %d"), relative, res);
            }
        }
    } else {
        if (info.applied) {
            int res = DobbyCodePatch(info.address, info.original_bytes.data(), info.original_bytes.size());
            if (res == 0) {
                info.applied = false;
            } else {
                LOGE(OBFUSCATE("Failed to restore patch: %s, error: %d"), relative, res);
            }
        }
    }
}

#define PATCH(lib, off_sym, hex_asm) DobbyPatchWrapper(lib, OBFUSCATE(off_sym), OBFUSCATE(hex_asm), true)
#define RESTORE(lib, off_sym) DobbyPatchWrapper(lib, OBFUSCATE(off_sym), "", false)
#define PATCH_SWITCH(lib, off_sym, hex_asm, boolean) DobbyPatchWrapper(lib, OBFUSCATE(off_sym), OBFUSCATE(hex_asm), boolean)

void PatchRelativeOffset(const char *libName, const char *rootOffset, const char *addOffset, std::string data, bool apply) {
    DobbyPatchWrapper(libName, getRelativeAddress(libName, rootOffset, addOffset).c_str(), std::move(data), apply);
}

#define rPATCH(lib, root_off_sym, add_off, hex_asm) PatchRelativeOffset(lib, OBFUSCATE(root_off_sym), OBFUSCATE(add_off), OBFUSCATE(hex_asm), true)
#define rRESTORE(lib, root_off_sym, add_off) PatchRelativeOffset(lib, OBFUSCATE(root_off_sym), OBFUSCATE(add_off), "", false)
#define rPATCH_SWITCH(lib, root_off_sym, add_off, hex_asm, boolean) PatchRelativeOffset(lib, OBFUSCATE(root_off_sym), OBFUSCATE(add_off), OBFUSCATE(hex_asm), boolean)

// Dynamic asm patches: assemble at runtime via KittyUtils::String::Fmt.
#define dPATCH(lib, off_sym, asms, ...) DobbyPatchWrapper(lib, OBFUSCATE(off_sym), KittyUtils::String::Fmt(OBFUSCATE(asms), __VA_ARGS__), true)
#define dRESTORE(lib, off_sym) DobbyPatchWrapper(lib, OBFUSCATE(off_sym), "", false)
#define dPATCH_SWITCH(boolean, lib, off_sym, asms, ...) DobbyPatchWrapper(lib, OBFUSCATE(off_sym), KittyUtils::String::Fmt(OBFUSCATE(asms), __VA_ARGS__), boolean)
#define drPATCH_SWITCH(boolean, lib, root_off_sym, add_off, asms, ...) PatchRelativeOffset(lib, OBFUSCATE(root_off_sym), OBFUSCATE(add_off), KittyUtils::String::Fmt(OBFUSCATE(asms), __VA_ARGS__), boolean)

#endif //ANDROID_MOD_MENU_MACROS_H
