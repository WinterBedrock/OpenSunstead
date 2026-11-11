#include "ColorCodeHook.h"

#include "../Memory/SigManager.h"
#include "../Core/Logger.h"
#include "../Utils/MiscUtils/ColorUtils.hpp"
#include "../Modules/ModuleManager.h"
#include "../Modules/Setting.h"
#include "HookEngine.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace {

// mce::Color as the game stores it in the code table at .data:0x151533BB0.
struct GameColor {
    float r, g, b, a;
};

using GetColorFromCodeFn = const GameColor* (__fastcall*)(const void* code);

GetColorFromCodeFn oGetColorFromCode = nullptr;
bool  g_hookInstalled = false;
bool  g_gatePatched   = false;
BYTE  g_savedGateByte = 0;
uintptr_t g_gateByteAddr = 0;

constexpr char kSectionHi = '\xC2';
constexpr char kSectionLo = '\xA7';

constexpr char kLetterTheme  = 'z';
constexpr char kLetterAccent = 'y';
constexpr char kLetterCustom = 'x';

// Rendered forms handed back to callers. Index matches ColorCodeHook::Code.
constexpr const char* kCodeStrings[] = { "§z", "§y", "§x" };

// MSVC std::string: SSO buffer (or heap pointer) at +0x00, size at +0x10,
// capacity at +0x18. Capacity >= 0x10 means the data is out of line.
bool readShortString(const void* str, char* out, size_t outCap, size_t* lenOut) {
    if (!str || !out || outCap == 0) return false;
    __try {
        const auto base = reinterpret_cast<uintptr_t>(str);
        const size_t len = *reinterpret_cast<const size_t*>(base + 0x10);
        const size_t cap = *reinterpret_cast<const size_t*>(base + 0x18);
        if (len == 0 || len >= outCap) return false;
        const char* data = (cap >= 0x10)
            ? *reinterpret_cast<const char* const*>(base)
            : reinterpret_cast<const char*>(base);
        if (!data) return false;
        memcpy(out, data, len);
        *lenOut = len;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

ImColor customCodeColor() {
    Module* iface = ModuleManager::GetInstance().GetModuleByName("ClickGUI");
    if (iface) {
        for (Setting* s : iface->mSettings) {
            if (s && s->mName == "Code Color") {
                const float* c = static_cast<ColorSetting*>(s)->mValue;
                return ImColor(c[0], c[1], c[2], 1.f);
            }
        }
    }
    return ImColor(1.f, 1.f, 1.f, 1.f);
}

// Non-SEH so the std::vector work inside getThemedColor is legal here.
const GameColor* resolveCustomColor(ColorCodeHook::Code which) {
    // One slot per thread: the caller only reads it before the next call on the
    // same thread, and the game lays text out from both the render and UI paths.
    thread_local GameColor col{ 1.f, 1.f, 1.f, 1.f };

    thread_local uint64_t s_frameMs = 0;
    thread_local float    s_sweep   = 0.f;
    const uint64_t now = ColorUtils::GetTimeMs();
    if (now != s_frameMs) {
        s_frameMs = now;
        s_sweep   = 0.f;
    }

    ImColor out(1.f, 1.f, 1.f, 1.f);
    switch (which) {
    case ColorCodeHook::Code::Theme:
        out = ColorUtils::getThemedColor(s_sweep, now);
        s_sweep += 6.0f; // per-run step, matches drawGradientText's stride
        break;
    case ColorCodeHook::Code::Accent:
        out = ColorUtils::getThemedColor(0.f, now);
        break;
    case ColorCodeHook::Code::Custom:
        out = customCodeColor();
        break;
    }

    col.r = out.Value.x;
    col.g = out.Value.y;
    col.b = out.Value.z;
    col.a = 1.f;
    return &col;
}

int classifyCode(const void* code) {
    char buf[8];
    size_t len = 0;
    if (!readShortString(code, buf, sizeof(buf), &len)) return -1;
    if (len != 3 || buf[0] != kSectionHi || buf[1] != kSectionLo) return -1;
    switch (buf[2]) {
    case kLetterTheme:  return (int)ColorCodeHook::Code::Theme;
    case kLetterAccent: return (int)ColorCodeHook::Code::Accent;
    case kLetterCustom: return (int)ColorCodeHook::Code::Custom;
    default: return -1;
    }
}

const GameColor* callOriginal(const void* code) {
    if (!oGetColorFromCode) return nullptr;
    __try {
        return oGetColorFromCode(code);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

const GameColor* __fastcall hkGetColorFromCode(const void* code) {
    const int which = classifyCode(code);
    if (which >= 0) return resolveCustomColor((ColorCodeHook::Code)which);
    return callOriginal(code);
}

// ---------------------------------------------------------------------------
// Content verification for the getColorFromCode signature.
//
// This exists because a stale signature does not fail loudly. On the build that
// crashed, the pattern matched 0xBD0 bytes past the real function and landed on
// the pair-constructor the colour map is BUILT with — a function whose first
// argument is an out-parameter, not a std::string. Hooking that and answering
// some calls with our own pointer left the caller holding an unconstructed
// object, which is what took the process down at world join.
//
// Rather than trusting the byte pattern, prove the resolved function is the
// right one by its data: the real getColorFromCode is the only function that
// references the 29-entry mce::Color table, whose first two entries are
// §0 = {0,0,0,1} (black) and §1 = {0,0,0.667,1} (dark blue). Walk the function's
// first bytes for a RIP-relative `lea` and check the target against those
// values. This is build-independent — the table contents are the vanilla colours
// and do not move with codegen.
// ---------------------------------------------------------------------------
bool looksLikeColorTable(const void* p) {
    if (!p) return false;
    __try {
        const float* f = reinterpret_cast<const float*>(p);
        // `near` is a legacy MSVC keyword macro, hence the name.
        const auto eq = [](float a, float b) { return (a - b) < 0.002f && (b - a) < 0.002f; };
        // §0 black, then §1 dark blue. Alpha is 1.0 on both.
        return eq(f[0], 0.f) && eq(f[1], 0.f) && eq(f[2], 0.f)     && eq(f[3], 1.f)
            && eq(f[4], 0.f) && eq(f[5], 0.f) && eq(f[6], 0.6667f) && eq(f[7], 1.f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool verifyGetColorFromCode(uintptr_t fn) {
    if (!fn) return false;
    // 0x400 covers the thread-safe-static block that seeds the map, which is
    // where every table reference lives.
    constexpr size_t kScan = 0x400;
    __try {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(fn);
        for (size_t i = 0; i + 7 < kScan; ++i) {
            // 48 8D /r with a RIP-relative operand: lea r64, [rip+disp32].
            // ModRM 0x05/0x0D/0x15/0x1D = rax/rcx/rdx/rbx, mod=00 rm=101.
            if (p[i] != 0x48 || p[i + 1] != 0x8D) continue;
            const unsigned char modrm = p[i + 2];
            if ((modrm & 0xC7) != 0x05) continue;
            const int32_t disp = *reinterpret_cast<const int32_t*>(p + i + 3);
            const void* target = reinterpret_cast<const void*>(fn + i + 7 + (intptr_t)disp);
            if (looksLikeColorTable(target)) return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


bool writeGateByte(BYTE value, BYTE* previous) {
    if (!g_gateByteAddr) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(g_gateByteAddr), 1,
                        PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    if (previous) *previous = *reinterpret_cast<BYTE*>(g_gateByteAddr);
    *reinterpret_cast<BYTE*>(g_gateByteAddr) = value;
    VirtualProtect(reinterpret_cast<void*>(g_gateByteAddr), 1, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_gateByteAddr), 1);
    return true;
}

} // namespace

void ColorCodeHook::Initialize() {
    auto& sigs = SigManager::GetInstance();

    // Half one: let 'z' through the processor's letter gate at all.
    // `cmp dl, 16h` -> `cmp dl, 19h`, i.e. 'a'..'w' becomes 'a'..'z'.
    if (sigs.TextProcessor_codeRangeGate) {
        g_gateByteAddr = sigs.TextProcessor_codeRangeGate + 5;
        BYTE before = 0;
        if (writeGateByte(0x19, &before) && before == 0x16) {
            g_savedGateByte = before;
            g_gatePatched = true;
            LOG_INFO("§-code letter gate widened to 'a'..'z' @ " +
                         std::to_string(g_gateByteAddr), "ColorCodeHook");
        } else {
            // Either the write failed or the immediate was not what we reversed;
            // put the original back rather than leaving a half-patch behind.
            if (before != 0) writeGateByte(before, nullptr);
            g_gateByteAddr = 0;
            LOG_WARN("§-code letter gate immediate was 0x" + std::to_string((int)before) +
                         ", expected 0x16 — not patching", "ColorCodeHook");
        }
    } else {
        LOG_WARN("TextProcessor::codeRangeGate not found; custom codes stay inert", "ColorCodeHook");
    }

    if (sigs.TextColor_getColorFromCode &&
        !verifyGetColorFromCode(sigs.TextColor_getColorFromCode)) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "getColorFromCode @0x%llX rejected; custom colour codes disabled",
            (unsigned long long)sigs.TextColor_getColorFromCode);
        LOG_WARN(buf, "ColorCodeHook");
    } else if (sigs.TextColor_getColorFromCode) {
        auto target = reinterpret_cast<LPVOID>(sigs.TextColor_getColorFromCode);
        if (HookEngine::Create(target, reinterpret_cast<LPVOID>(&hkGetColorFromCode),
                          reinterpret_cast<LPVOID*>(&oGetColorFromCode))) {
            g_hookInstalled = true;
            LOG_INFO("ColorCodeHook: §z gate hooked (theme gradient), "
                     "§y (theme accent), §x (Settings > Code Color) are live",
                     "ColorCodeHook");
        } else {
            LOG_ERROR("FAILED to hook the colour-code function", "ColorCodeHook");
        }
    } else {
        LOG_WARN("colour-code function not found; custom codes stay white", "ColorCodeHook");
    }
}

void ColorCodeHook::Shutdown() {
    if (g_gatePatched) {
        writeGateByte(g_savedGateByte, nullptr);
        g_gatePatched = false;
    }
}

bool ColorCodeHook::IsAvailable() {
    return g_hookInstalled && g_gatePatched;
}

const char* ColorCodeHook::CodeString(Code c) {
    if (!IsAvailable()) return "§f"; // white — always safe to emit
    const int i = static_cast<int>(c);
    if (i < 0 || i >= (int)(sizeof(kCodeStrings) / sizeof(kCodeStrings[0]))) return "§f";
    return kCodeStrings[i];
}

const char* ColorCodeHook::ThemeCode() {
    return CodeString(Code::Theme);
}
