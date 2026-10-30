#include "StarveSprint.h"
#include "../../Memory/SigManager.h"
#include <cstring>

// testssss
//   FF C1                         inc ecx
//   F3 0F 10 05 ?? ?? ?? ??       movss xmm0, 6.0
//   41 0F 2E 40 ??                ucomiss xmm0, [r8+disp]  ; food
//   B8 01 00 00 00                mov eax, 1
//   0F 43 C1                      cmovnbe eax, ecx         ; fail if food < 6
//
// Patch: NOP the 3-byte cmovnbe so eax stays 1 regardless of hunger.

bool StarveSprint::writeBytes(uintptr_t addr, const void* data, size_t len) {
    if (!addr || !data || !len) return false;
    DWORD old = 0;
    if (!VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)addr, data, len);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
    VirtualProtect((void*)addr, len, old, &old);
    return true;
}

bool StarveSprint::sehReadN(uintptr_t addr, unsigned char* out, size_t n) {
    if (!addr || !out || !n) return false;
    __try {
        for (size_t i = 0; i < n; ++i) out[i] = *((unsigned char*)addr + i);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool StarveSprint::findAndApply() {
    mPatchAddr = 0;
    mOg.clear();
    mPatched = false;

    uintptr_t hit = SigManager::GetInstance().StarveSprint_foodGate;
    if (!hit) return false;

    //sig points at 'inc ecx' (FF C1). cmovnbe is at +20
    static constexpr size_t kCmovOff = 20;
    static constexpr size_t kCmovLen = 3;

    unsigned char window[24] = {};
    if (!sehReadN(hit, window, sizeof(window))) return false;
    // Sanity: ... B8 01 00 00 00 0F 43 C1
    if (window[15] != 0xB8 || window[16] != 0x01 ||
        window[20] != 0x0F || window[21] != 0x43 || window[22] != 0xC1)
        return false;

    mPatchAddr = hit + kCmovOff;
    mOg.assign(window + kCmovOff, window + kCmovOff + kCmovLen);
    unsigned char nops[3] = { 0x90, 0x90, 0x90 };
    if (!writeBytes(mPatchAddr, nops, kCmovLen)) {
        mPatchAddr = 0;
        mOg.clear();
        return false;
    }
    mPatched = true;
    return true;
}

void StarveSprint::restore() {
    if (mPatched && mPatchAddr && mOg.size() == 3)
        writeBytes(mPatchAddr, mOg.data(), mOg.size());
    mPatchAddr = 0;
    mOg.clear();
    mPatched = false;
}

void StarveSprint::reassert() {
    if (!mPatched || !mPatchAddr) return;
    unsigned char nops[3] = { 0x90, 0x90, 0x90 };
    writeBytes(mPatchAddr, nops, 3);
}

void StarveSprint::OnEnable() {
    findAndApply();
}

void StarveSprint::OnDisable() {
    restore();
}

void StarveSprint::OnUpdate() {
    if (!IsEnabled()) return;
    reassert();
}
