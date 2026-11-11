#include "Swing.h"

#include "../../Core/Logger.h"
#include "../../Memory/SigManager.h"
#include "../../SDK/Client/ClientInstance.h"
#include "../../Hooks/HookEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <climits>

static constexpr ptrdiff_t kSwingProgress = 0x42C; // float, 0..1
static constexpr ptrdiff_t kSwingFlag     = 0x45D; // byte, 1 while swinging

namespace {

using NormalTickFn = void(__fastcall*)(void* self);
using SwingDurationFn = int(__fastcall*)(void* actor, bool alternate);
NormalTickFn oNormalTick = nullptr;
SwingDurationFn oSwingDuration = nullptr;

bool  g_hooked     = false;
bool  g_durHooked  = false;
bool  g_enabled    = false;
int   g_style      = 0;
float g_speed      = 1.0f;

uint8_t g_prevSwingFlag = 0;

float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

bool sehReadByte(const void* p, uint8_t* out) {
    __try {
        *out = *reinterpret_cast<const uint8_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float shape(int style, float t) {
    t = clampf(t, 0.0f, 1.0f);
    switch (style) {
    case 1:  // Smooth - smoothstep: slow off the rest pose, slow into the jab.
        return t * t * (3.0f - 2.0f * t);
    case 2:  // Snap - fast out, then the envelope carries the long return.
        return std::sqrt(t);
    case 3:  // Ease - hangs back, then comes through.
        return t * t;
    case 4:  // Whip - barely moves, then snaps to full extension at the end.
        return t * t * t;
    default: // Vanilla / None
        return t;
    }
}

int __fastcall hkSwingDuration(void* actor, bool alternate) {
    const int base = oSwingDuration ? oSwingDuration(actor, alternate) : 6;
    if (!g_enabled) return base;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !actor) return base;
    if (reinterpret_cast<void*>(ci->getLocalPlayer()) != actor) return base;

    const int scaled = static_cast<int>(std::lround(base / g_speed));
    return std::max(1, scaled);
}

void __fastcall hkNormalTick(void* self) {
    if (oNormalTick) oNormalTick(self);
    if (!g_enabled) return;

    auto* base = reinterpret_cast<unsigned char*>(self);
    auto* progress = reinterpret_cast<float*>(base + kSwingProgress);
    if (!sdk_safe_ptr(progress, sizeof(float))) return;

    uint8_t flag = 0;
    if (!sehReadByte(base + kSwingFlag, &flag)) return;

    float out;
    if (flag != 0) {
        out = (g_style == 5) ? 0.0f : shape(g_style, *progress);
        g_prevSwingFlag = 1;
    } else if (g_prevSwingFlag != 0) {
        // swing ended
        out = 1.0f;
        g_prevSwingFlag = 0;
    } else {
        out = 0.0f;
    }
    *progress = out;
}

} // namespace

Swing::Swing()
    : Module("Swing", "Custom first-person hand swing animations", Category::Visual, 0) {
    AddSetting(new EnumSetting("Style", "How the swing travels", 1,
        {"Vanilla", "Smooth", "Snap", "Ease", "Whip", "None"}));
    AddSetting(new NumberSetting("Speed", "How fast the arm swings", 1.0f, 0.2f, 3.0f, 0.05f));
    AddSetting(new BoolSetting("Custom Angle", "Override the swing angle", false));
    AddSetting(new NumberSetting("Angle", "Swing angle in degrees (vanilla is -20)", -20.0f, -180.0f, 180.0f, 1.0f));
    canBind = true;
}

int   Swing::Style()       const { return static_cast<EnumSetting*>(mSettings[0])->mValue; }
float Swing::Speed()       const { return static_cast<NumberSetting*>(mSettings[1])->mValue; }
bool  Swing::CustomAngle() const { return static_cast<BoolSetting*>(mSettings[2])->mValue; }
float Swing::Angle()       const { return static_cast<NumberSetting*>(mSettings[3])->mValue; }

void InstallDurationHook() {
    if (g_durHooked) return;

    const uintptr_t fn = SigManager::GetInstance().Mob_getCurrentSwingDuration;
    if (!fn) {
        LOG_WARN("getCurrentSwingDuration not found — swing speed left vanilla", "Swing");
        return;
    }

    if (!HookEngine::Create(reinterpret_cast<LPVOID>(fn), reinterpret_cast<LPVOID>(&hkSwingDuration),
                      reinterpret_cast<LPVOID*>(&oSwingDuration))) {
        LOG_ERROR("getCurrentSwingDuration hook failed", "Swing");
        return;
    }

    g_durHooked = true;
    LOG_INFO("getCurrentSwingDuration hooked", "Swing");
}

bool Swing::InstallHook() {
    InstallDurationHook();
    if (g_hooked) return true;

    const uintptr_t fn = SigManager::GetInstance().LocalPlayer_normalTick;
    if (!fn) {
        LOG_WARN("LocalPlayer::normalTick not found — swing styling disabled", "Swing");
        return false;
    }

    if (!HookEngine::Create(reinterpret_cast<LPVOID>(fn), reinterpret_cast<LPVOID>(&hkNormalTick),
                      reinterpret_cast<LPVOID*>(&oNormalTick))) {
        oNormalTick = nullptr;
        LOG_ERROR("normalTick hook failed", "Swing");
        return false;
    }

    g_hooked = true;
    LOG_INFO("normalTick hooked", "Swing");
    return true;
}

void InstallSwingRotateHook();

void Swing::InstallHooks() {
    InstallHook();
    InstallSwingRotateHook();
}

// ---------------------------------------------------------------------------
// Custom Angle — direct code patch.
//
// The swing rotation site (pinned byte-for-byte by the ItemInHand_swingAngleLoad
// signature) computes:  angle = progress * [-20.0] * deg2rad, then calls
// Matrix::rotateAxisAngle. The first multiply is `mulss xmm9, [rip+disp32]`
// (opcode bytes are part of the signature, only the displacement is wild).
// The -20.0 constant it points at is shared by 24 other code sites, so instead
// of touching the constant we rewrite the displacement to aim at a 4-byte
// writable slot we allocate near the game image. Vanilla keeps -20.0 in the
// slot; Custom Angle swaps in the user's angle. No hooks, no trampolines and no
// return-address matching — the previous detour-based approach silently
// depended on the shared Matrix::rotateAxisAngle hook matching a return site.
// ---------------------------------------------------------------------------
namespace {

constexpr ptrdiff_t kMulssAt  = 5;   // F3 44 0F 59 0D <disp32> begins at sig+5
constexpr ptrdiff_t kDispAt    = kMulssAt + 5;
constexpr ptrdiff_t kInstrEnd  = kDispAt + 4;

float*    g_angleSlot      = nullptr;  // writable 4-byte slot near the image
uintptr_t g_angleDispAt    = 0;        // VA of the disp32 inside the mulss
int32_t   g_angleOrigDisp  = 0;        // original value, restored on unload
bool      g_anglePatched   = false;

bool sehReadI32(const void* p, int32_t* out) {
    __try {
        *out = *reinterpret_cast<const int32_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Any committed page within +-2GB of `near' works; rel32 displacements must
// stay in int32 range. The slot is intentionally never freed: after the client
// unloads, the patched instruction still dereferences it, so it must outlive
// us and keep holding the vanilla value.
float* allocAngleSlotNear(uintptr_t nearVA) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    constexpr uint64_t kStep = 0x10000;
    for (uint64_t off = kStep; off < 0x70000000ULL; off += kStep) {
        for (int dir = 0; dir < 2; ++dir) {
            const uint64_t addr = dir ? (nearVA + off) : (nearVA - off);
            if (addr < 0x10000) continue;
            void* p = VirtualAlloc(reinterpret_cast<void*>(addr & ~(kStep - 1)),
                                   si.dwPageSize, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_READWRITE);
            if (p) return static_cast<float*>(p);
        }
    }
    return nullptr;
}

void applyAngleSlot(float degrees) {
    if (g_angleSlot) *g_angleSlot = degrees;  // aligned dword store: atomic
}

} // namespace

// vanilla sweeps -20 degrees
static constexpr float kVanillaSwingDegrees = -20.0f;

float Swing::CustomAngleDegrees() {
    return (g_enabled && CustomAngle()) ? Angle() : kVanillaSwingDegrees;
}

// swings rotate site patch
void InstallSwingRotateHook() {
    if (g_anglePatched) return;

    const uintptr_t match = SigManager::GetInstance().ItemInHand_swingAngleLoad;
    if (!match) {
        LOG_WARN("Swing rotation site not found — Custom Angle unavailable", "Swing");
        return;
    }

    const uintptr_t dispAt = match + kDispAt;
    int32_t orig = 0;
    if (!sehReadI32(reinterpret_cast<const void*>(dispAt), &orig)) {
        LOG_WARN("Swing rotation site unreadable — Custom Angle unavailable", "Swing");
        return;
    }

    float* slot = allocAngleSlotNear(match);
    if (!slot) {
        LOG_WARN("Could not allocate an angle slot near the game image", "Swing");
        return;
    }
    *slot = kVanillaSwingDegrees;

    const int64_t delta = reinterpret_cast<int64_t>(slot) -
                          static_cast<int64_t>(match + kInstrEnd);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        VirtualFree(slot, 0, MEM_RELEASE);
        LOG_WARN("Angle slot out of rel32 range — Custom Angle unavailable", "Swing");
        return;
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(dispAt), sizeof(int32_t),
                        PAGE_EXECUTE_READWRITE, &oldProt)) {
        VirtualFree(slot, 0, MEM_RELEASE);
        LOG_WARN("Swing site is not writable — Custom Angle unavailable", "Swing");
        return;
    }
    *reinterpret_cast<int32_t*>(dispAt) = static_cast<int32_t>(delta);
    VirtualProtect(reinterpret_cast<void*>(dispAt), sizeof(int32_t), oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(dispAt), sizeof(int32_t));

    g_angleSlot     = slot;
    g_angleDispAt   = dispAt;
    g_angleOrigDisp = orig;
    g_anglePatched  = true;
    LOG_INFO("Swing angle patch installed (slot at follows-sig displacement)", "Swing");
}

void Swing::RestoreAnglePatch() {
    if (!g_anglePatched) return;
    DWORD oldProt = 0;
    if (VirtualProtect(reinterpret_cast<void*>(g_angleDispAt), sizeof(int32_t),
                       PAGE_EXECUTE_READWRITE, &oldProt)) {
        *reinterpret_cast<int32_t*>(g_angleDispAt) = g_angleOrigDisp;
        VirtualProtect(reinterpret_cast<void*>(g_angleDispAt), sizeof(int32_t), oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(),
                              reinterpret_cast<void*>(g_angleDispAt), sizeof(int32_t));
    }
    // Keep g_angleSlot allocated and leave it at the vanilla value: the
    // process may outlive this DLL (eject). Only mark the code as restored.
    g_anglePatched = false;
}

void Swing::OnEnable() {
    g_enabled = true;
    g_prevSwingFlag = 0;
    InstallHook();
    InstallSwingRotateHook();
    if (g_anglePatched) applyAngleSlot(CustomAngleDegrees());
}

void Swing::OnDisable() {
    g_enabled = false;
    g_prevSwingFlag = 0;
    if (g_anglePatched) applyAngleSlot(kVanillaSwingDegrees);
}

void Swing::OnTick() {
    g_style = Style();
    g_speed = Speed();
    if (g_anglePatched) applyAngleSlot(CustomAngleDegrees());
}

void Swing::UpdateSettingsVisibility() {
    const bool none = Style() == 5;
    const bool custom = CustomAngle();
    for (Setting* s : mSettings) {
        if (!s) continue;
        if (s->mName == "Speed") s->mVisible = !none;
        if (s->mName == "Angle") s->mVisible = custom;
    }
}
