#include "Anchor.h"
#include "../../Memory/SigManager.h"
#include <cstring>

// MoveRequest edge clamp.
// Patch forces edge flag = 1 and always enters clamp (no PlayerAction sneak).

bool Anchor::writeBytes(uintptr_t addr, const void* data, size_t len) {
    if (!addr || !data || !len) return false;
    DWORD old = 0;
    if (!VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)addr, data, len);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
    VirtualProtect((void*)addr, len, old, &old);
    return true;
}

bool Anchor::sehReadN(uintptr_t addr, unsigned char* out, size_t n) {
    if (!addr || !out || !n) return false;
    __try {
        for (size_t i = 0; i < n; ++i) out[i] = *((unsigned char*)addr + i);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Anchor::findAndApplyPatches() {
    mPrimary = {};
    mAlt = {};
    mPatched = false;

    uintptr_t primary = SigManager::GetInstance().SneakMovementSystem_tickSneakMovementSystem;
    if (primary) {
        unsigned char og[13] = {};
        if (sehReadN(primary, og, sizeof(og)) &&
            og[0] == 0x83 && og[1] == 0xE1 && og[2] == 0x01 &&
            og[3] == 0x88 && og[4] == 0x48 &&
            og[6] == 0x75 && og[8] == 0xE9) {
            const unsigned char disp = og[5];
            unsigned char patch[13] = {
                0xB1, 0x01,
                0x90,
                0x88, 0x48, disp,
                0xEB, og[7],
                0x90, 0x90, 0x90, 0x90, 0x90
            };
            mPrimary.addr = primary;
            mPrimary.og.assign(og, og + sizeof(og));
            mPrimary.patch.assign(patch, patch + sizeof(patch));
        }
    }

    uintptr_t alt = SigManager::GetInstance().SneakMovementSystem_edgeClampAlt;
    if (alt) {
        unsigned char og[13] = {};
        if (sehReadN(alt, og, sizeof(og)) &&
            og[0] == 0x44 && og[1] == 0x88 && og[2] == 0x50 &&
            og[4] == 0x48 && og[5] == 0x85 && og[6] == 0xC9 &&
            og[7] == 0x0F && og[8] == 0x84) {
            const unsigned char disp = og[3];
            unsigned char patch[13] = {
                0xC6, 0x40, disp, 0x01,
                0x48, 0x85, 0xC9,
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90
            };
            mAlt.addr = alt;
            mAlt.og.assign(og, og + sizeof(og));
            mAlt.patch.assign(patch, patch + sizeof(patch));
        }
    }

    if (!mPrimary.addr && !mAlt.addr) return false;

    bool ok = true;
    if (mPrimary.addr)
        ok = writeBytes(mPrimary.addr, mPrimary.patch.data(), mPrimary.patch.size()) && ok;
    if (mAlt.addr)
        ok = writeBytes(mAlt.addr, mAlt.patch.data(), mAlt.patch.size()) && ok;
    mPatched = ok;
    return mPatched;
}

void Anchor::restorePatches() {
    if (mPrimary.addr && !mPrimary.og.empty())
        writeBytes(mPrimary.addr, mPrimary.og.data(), mPrimary.og.size());
    if (mAlt.addr && !mAlt.og.empty())
        writeBytes(mAlt.addr, mAlt.og.data(), mAlt.og.size());
    mPrimary = {};
    mAlt = {};
    mPatched = false;
}

void Anchor::reassertPatches() {
    if (!mPatched) return;
    if (mPrimary.addr && !mPrimary.patch.empty())
        writeBytes(mPrimary.addr, mPrimary.patch.data(), mPrimary.patch.size());
    if (mAlt.addr && !mAlt.patch.empty())
        writeBytes(mAlt.addr, mAlt.patch.data(), mAlt.patch.size());
}

void Anchor::OnEnable() {
    findAndApplyPatches();
}

void Anchor::OnDisable() {
    restorePatches();
}

void Anchor::OnUpdate() {
    if (!IsEnabled()) return;
    reassertPatches();
}
