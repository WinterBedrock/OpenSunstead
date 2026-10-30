#include "EditionFaker.h"
#include "../../Memory/SigManager.h"
#include "../../Core/Logger.h"
#include "../../SDK/Client/ClientInstance.h"
#include "../../Modules/Visual/Notifications.h"
#include <Windows.h>
#include <stdio.h>
#include <cstring>
#include <cstdint>

// ---------------------------------------------------------------------------
// EditionFaker (1.26.50.04) -- in-place CFG-call patch.
//
// ConnectionRequest::toJson reads DeviceOS / DefaultInputMode from the
// AppPlatform singleton through CFG-guarded virtuals:
//
//     mov rax, [rbp+0x118]        ; options holder
//     mov rcx, [rax+0x10]         ; AppPlatform ptr
//     mov rax, [rcx]              ; platform vtable
//     mov rax, [rax+SLOT]         ; getter fn  (0x98 = DeviceOS, 0x870 = Input)
//     call __guard_dispatch_icall_fptr   ; 6 bytes: FF 15 disp32
//     mov edx, eax                ; -> value into the JSON writer's edx
//     ...
//     call sub_14DF1EA60          ; int field writer (rcx=builder, edx=value)
//
// Patch: replace the 6-byte guard call with `mov eax, imm32` + NOP. The
// following `mov edx, eax` then carries the spoofed value into the writer.
// No allocation, no rel32, instructions before/after are untouched.
//
// The OLD 1.26.30 approach NOP-ed a 0x1C/0x23-byte block before the writer
// call and wrote `mov edx, imm32` at call-9. That layout no longer exists in
// 1.26.42.01 — the old signatures still matched a byte-shape, so the patch
// cut instructions in half and the game executed `add [rax],al` on the vtable
// function pointer -> CRASH on every world/server join. This rewrite targets
// the new (verified-in-IDA) call sites instead.
//
// Four sites are patched (each signature is UNIQUE in 1.26.50.04):
//   DeviceOS      modern @ sig+0x24   (full toJson builder)
//   InputMode     modern @ sig+0x21
//   DeviceOS      legacy @ sig+0x0E   (short pre-1.19-protocol builder)
//   InputMode     legacy @ sig+0x0E
// ---------------------------------------------------------------------------

static const size_t PATCH_LEN = 6; // FF 15 disp32 (guard dispatch call)

struct Site {
    uintptr_t sig;      // resolved signature address
    size_t    guardOff; // offset of the guard call from the match
    uint8_t   saved[6];
    bool      patched;
};

// Guard-call offset from the pattern match:
//   DeviceOS  modern: `BA FF FF FF FF` prefix (5) ... +0x24
//   InputMode modern: `31 D2` prefix (2) — 3 bytes shorter ... +0x21
//   both legacy: pattern starts at `48 8B 4? 10` ... +0x0E
static Site g_osSite{ 0, 0x24, {}, false };
static Site g_inSite{ 0, 0x21, {}, false };
static Site g_osLegacy{ 0, 0x0E, {}, false };
static Site g_inLegacy{ 0, 0x0E, {}, false };
static int  lastOsVal    = -1;
static int  lastInputVal = -1;

static bool PatchSite(Site& s, int32_t value) {
    if (!s.sig) return false;
    uintptr_t call = s.sig + s.guardOff;

    // Sanity on FIRST patch only: the site must hold `FF 15 disp32` (guard
    // dispatch). If a future update shifts the layout, bail out instead of
    // corrupting whatever landed there. Once patched the bytes are OUR
    // `B8 imm32 90` — rewriting just the imm32 (setting changed) is fine and
    // must NOT fail this check.
    if (!s.patched) {
        __try {
            uint8_t* b = reinterpret_cast<uint8_t*>(call);
            if (b[0] != 0xFF || b[1] != 0x15) return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    DWORD oldProt;
    if (!VirtualProtect(reinterpret_cast<void*>(call), PATCH_LEN,
                        PAGE_EXECUTE_READWRITE, &oldProt))
        return false;

    if (!s.patched)
        memcpy(s.saved, reinterpret_cast<void*>(call), PATCH_LEN);  // save once

    // mov eax, imm32 (B8 xx xx xx xx) + NOP — the next `mov edx, eax` (or
    // `lea rcx,[rbp+..]; mov edx, eax`) forwards eax into the JSON writer.
    uint8_t p[6];
    p[0] = 0xB8;
    memcpy(p + 1, &value, 4);
    p[5] = 0x90;
    memcpy(reinterpret_cast<void*>(call), p, PATCH_LEN);

    VirtualProtect(reinterpret_cast<void*>(call), PATCH_LEN, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(call), PATCH_LEN);
    s.patched = true;
    return true;
}

static void RestoreSite(Site& s) {
    if (!s.sig || !s.patched) return;
    uintptr_t call = s.sig + s.guardOff;
    DWORD oldProt;
    if (VirtualProtect(reinterpret_cast<void*>(call), PATCH_LEN,
                       PAGE_EXECUTE_READWRITE, &oldProt)) {
        memcpy(reinterpret_cast<void*>(call), s.saved, PATCH_LEN);
        VirtualProtect(reinterpret_cast<void*>(call), PATCH_LEN, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(call), PATCH_LEN);
    }
    s.patched = false;
}

void EditionFaker::OnEnable() {
    ApplySpoof();
}

void EditionFaker::OnDisable() {
    RestoreOriginals();
}

// SEH probe: true when a local player exists (in-world). Standalone so the
// __try frame stays free of C++ objects.
static bool editionFakerInWorld() {
    __try {
        SDK::ClientInstance* ci = SDK::ClientInstance::get();
        return ci && ci->getLocalPlayer() != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void EditionFaker::OnTick() {
    if (!IsEnabled()) return;
    int curOs    = static_cast<EnumSetting*>(mSettings[0])->mValue;
    int curInput = static_cast<EnumSetting*>(mSettings[1])->mValue;
    if (curOs != lastOsVal || curInput != lastInputVal) {
        // DeviceOS / DefaultInputMode are only sent inside the ConnectionRequest
        // at LOGIN time — changing them while connected has no effect on the
        // current session, tell the player instead of leaving them guessing.
        if (editionFakerInWorld())
            Notifications::AddNotification("EditionFaker: rejoin the server to apply the new device OS");
        ApplySpoof();
    }
}

void EditionFaker::ApplySpoof() {
    auto& sig = SigManager::GetInstance();
    g_osSite.sig    = sig.ConnectionRequest_DeviceOS;
    g_inSite.sig    = sig.ConnectionRequest_DefaultInput;
    g_osLegacy.sig  = sig.ConnectionRequest_DeviceOS_Legacy;
    g_inLegacy.sig  = sig.ConnectionRequest_DefaultInput_Legacy;

    int osVal    = static_cast<EnumSetting*>(mSettings[0])->mValue;
    int inputVal = static_cast<EnumSetting*>(mSettings[1])->mValue;

    // normalize via SDK enums to ensure valid indices
    SDK::DeviceOS osEnum = SDK::DeviceOSFromIndex(osVal);
    SDK::InputMode inEnum = SDK::InputModeFromIndex(inputVal);
    int32_t osIndex = static_cast<int32_t>(osEnum);
    int32_t inIndex = static_cast<int32_t>(inEnum);

    if (!g_osSite.sig && !g_inSite.sig && !g_osLegacy.sig && !g_inLegacy.sig) {
        LOG_WARN("EditionFaker: no ConnectionRequest sites resolved; spoof skipped", "EditionFaker");
        return;
    }

    int ok = 0;
    if (PatchSite(g_osSite, osIndex)) ++ok;
    if (PatchSite(g_inSite, inIndex)) ++ok;
    if (PatchSite(g_osLegacy, osIndex)) ++ok;
    if (PatchSite(g_inLegacy, inIndex)) ++ok;

    if (ok > 0) {
        LOG_INFO("EditionFaker: patched " + std::to_string(ok) +
                     "/4 sites — DeviceOS -> " + SDK::DeviceOSToString(osEnum) +
                     " (" + std::to_string(osIndex) + "), Input -> " +
                     SDK::InputModeToString(inEnum) + " (" + std::to_string(inIndex) + ")",
                     "EditionFaker");
    } else {
        LOG_WARN("EditionFaker: all patch sites failed (VirtualProtect?)", "EditionFaker");
    }

    lastOsVal    = osVal;
    lastInputVal = inputVal;
}

void EditionFaker::RestoreOriginals() {
    RestoreSite(g_osSite);
    RestoreSite(g_inSite);
    RestoreSite(g_osLegacy);
    RestoreSite(g_inLegacy);
    lastOsVal    = -1;
    lastInputVal = -1;
    LOG_INFO("EditionFaker: restored original instructions", "EditionFaker");
}
