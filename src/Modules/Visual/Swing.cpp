#include "Swing.h"

#include "../../Core/Logger.h"
#include "../../Memory/SigManager.h"
#include "../../SDK/Client/ClientInstance.h"

#include "MinHook.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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

float g_angleScale = 1.0f;

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

    if (MH_CreateHook(reinterpret_cast<LPVOID>(fn), reinterpret_cast<LPVOID>(&hkSwingDuration),
                      reinterpret_cast<LPVOID*>(&oSwingDuration)) != MH_OK
        || MH_EnableHook(reinterpret_cast<LPVOID>(fn)) != MH_OK) {
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

    if (MH_CreateHook(reinterpret_cast<LPVOID>(fn), reinterpret_cast<LPVOID>(&hkNormalTick),
                      reinterpret_cast<LPVOID*>(&oNormalTick)) != MH_OK
        || MH_EnableHook(reinterpret_cast<LPVOID>(fn)) != MH_OK) {
        MH_RemoveHook(reinterpret_cast<LPVOID>(fn));
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

static constexpr ptrdiff_t kSwingCallAt    = 62;
static constexpr ptrdiff_t kSwingRotateRet = kSwingCallAt + 5;

bool sehReadU8(const void* p, uint8_t* out) {
    __try {
        *out = *reinterpret_cast<const uint8_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool sehReadI32(const void* p, int32_t* out) {
    __try {
        *out = *reinterpret_cast<const int32_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t Swing::SwingRotateReturn() {
    const uintptr_t match = SigManager::GetInstance().ItemInHand_swingAngleLoad;
    return match ? match + kSwingRotateRet : 0;
}

// The swing arc is one `call rel32` inside ItemInHandRenderer::render. Decode
// that call so the hook lands on the function the renderer actually uses the client's Matrix::rotateAxisAngle signature resolves a different helper
// on this build, and hooking it changes nothing here.
uintptr_t Swing::SwingRotateFn() {
    const uintptr_t match = SigManager::GetInstance().ItemInHand_swingAngleLoad;
    if (!match) return 0;
    const uintptr_t call = match + kSwingCallAt;
    uint8_t op = 0;
    if (!sehReadU8(reinterpret_cast<const void*>(call), &op) || op != 0xE8) return 0;
    int32_t rel = 0;
    if (!sehReadI32(reinterpret_cast<const void*>(call + 1), &rel)) return 0;
    return call + 5 + static_cast<intptr_t>(rel);
}

float Swing::SwingAngleScale() { return g_angleScale; }

// swings rotate call
void InstallSwingRotateHook() {}

// vanilla sweeps -20 degrees
static constexpr float kVanillaSwingDegrees = -20.0f;

void Swing::OnEnable() {
    g_enabled = true;
    g_prevSwingFlag = 0;
    InstallHook();
    InstallSwingRotateHook();
}

void Swing::OnDisable() {
    g_enabled = false;
    g_prevSwingFlag = 0;
    g_angleScale = 1.0f;
}

void Swing::OnTick() {
    g_style = Style();
    g_speed = Speed();
    g_angleScale = (g_enabled && CustomAngle()) ? (Angle() / kVanillaSwingDegrees) : 1.0f;
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
