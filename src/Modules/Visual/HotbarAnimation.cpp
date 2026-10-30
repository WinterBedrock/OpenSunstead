#include "HotbarAnimation.h"

#include "../../Core/Logger.h"
#include "../../Memory/OffsetManager.h"
#include "../../Memory/SigManager.h"
#include "../../Modules/ModuleManager.h"
#include "../../SDK/Client/ScreenView.h"

#include "MinHook.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <windows.h>


namespace {

int   g_ease = 1;
float g_speed = 20.0f;
Setting* s_ease = nullptr;
Setting* s_speed = nullptr;

void pullSettings() {
    if (!s_ease || !s_speed) {
        Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI");
        if (!gui) return;
        for (Setting* s : gui->mSettings) {
            if (!s) continue;
            if (s->mName == "Hotbar Animation") s_ease = s;
            else if (s->mName == "Hotbar Speed") s_speed = s;
        }
    }
    if (s_ease && s_ease->mType == SettingType::Enum)
        g_ease = static_cast<EnumSetting*>(s_ease)->mValue;
    if (s_speed && s_speed->mType == SettingType::Number)
        g_speed = static_cast<NumberSetting*>(s_speed)->mValue;
}

struct Inst {
    SDK::UIControl* c;
    float x;
    float y;
    unsigned char flags;
};
Inst g_inst[16];
int  g_instCount = 0;
int  g_selIdx = -1;

// glide state
bool     g_havePos = false;
float    g_curX = 0.0f;
float    g_step = 0.0f;
double   g_lastQpc = 0.0;
bool     g_activeLogged = false;

float    g_outX = 0.0f;
bool     g_armed = false;

double nowSec() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

bool sehReadName(SDK::UIControl* c, int nameOff, char* out, size_t outCap) {
    __try {
        if (!c || nameOff <= 0 || !out || outCap < 2) return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(c) + nameOff;
        const unsigned long long len = *reinterpret_cast<const unsigned long long*>(base + 0x10);
        const unsigned long long cap  = *reinterpret_cast<const unsigned long long*>(base + 0x18);
        if (len == 0 || len > 128 || cap < len - 1 || cap > 4096) return false;
        const char* data = (cap >= 16) ? *reinterpret_cast<const char* const*>(base)
                                       : reinterpret_cast<const char*>(base);
        if (!data) return false;
        size_t n = static_cast<size_t>(len);
        if (n > outCap - 1) n = outCap - 1;
        for (size_t i = 0; i < n; ++i) out[i] = data[i];
        out[n] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool sehReadPos(SDK::UIControl* c, int posOff, float* outX, float* outY) {
    __try {
        if (!c || posOff <= 0) return false;
        const float* p = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(c) + posOff);
        const float x = p[0];
        const float y = p[1];
        if (!(std::fabs(x) < 100000.0f) || !(std::fabs(y) < 100000.0f)) return false;
        *outX = x;
        *outY = y;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

unsigned char sehReadFlags(SDK::UIControl* c, int flagsOff) {
    __try {
        if (!c || flagsOff <= 0) return 0;
        return *reinterpret_cast<const unsigned char*>(reinterpret_cast<uintptr_t>(c) + flagsOff);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct WalkCtx { int nameOff, posOff, flagsOff; };

void walkVisit(SDK::UIControl* c, void* ud) {
    WalkCtx* wc = static_cast<WalkCtx*>(ud);
    if (g_instCount >= 16) return;
    char nm[48];
    if (!sehReadName(c, wc->nameOff, nm, sizeof(nm))) return;
    if (std::memcmp(nm, "hotbar_slot_selected", 19) != 0) return;
    float x = 0, y = 0;
    if (!sehReadPos(c, wc->posOff, &x, &y)) return;
    Inst& it = g_inst[g_instCount++];
    it.c = c;
    it.x = x;
    it.y = y;
    it.flags = sehReadFlags(c, wc->flagsOff);
}

using DrawImage_t = void(__fastcall*)(void*, void*, float*, float*, float*, float*, unsigned char);
DrawImage_t oDrawImage = nullptr;

void __fastcall hkUIContextDrawImage(void* ctx, void* control, float* pos, float* size,
                                        float* uvPos, float* uvSize, unsigned char flag) {
    if (g_armed && pos && size) {
        if (std::fabs(size[0] - size[1]) < 1.0f && size[0] > 21.0f && size[0] < 36.0f) {
            for (int i = 0; i < g_instCount; ++i) {
                if (std::fabs(pos[0] - g_inst[i].x) < 2.0f &&
                    std::fabs(pos[1] - g_inst[i].y) < 2.0f) {
                    pos[0] = g_outX;
                    break;
                }
            }
        }
    }
    oDrawImage(ctx, control, pos, size, uvPos, uvSize, flag);
}

bool installOne(uintptr_t addr, void* detour, void** original, const char* name) {
    static std::once_flag once;
    std::call_once(once, []() { MH_Initialize(); });
    if (!addr) {
        LOG_WARN(std::string(name) + ": signature unresolved", "HotbarAnimation");
        return false;
    }
    MH_STATUS cr = MH_CreateHook(reinterpret_cast<LPVOID>(addr), detour, original);
    if (cr != MH_OK) {
        if (cr == MH_ERROR_ALREADY_CREATED && original && *original) {
            MH_EnableHook(reinterpret_cast<LPVOID>(addr));
            return true;
        }
        LOG_ERROR(std::string(name) + ": MH_CreateHook failed (" + std::to_string((int)cr) + ")", "HotbarAnimation");
        return false;
    }
    MH_STATUS er = MH_EnableHook(reinterpret_cast<LPVOID>(addr));
    if (er != MH_OK && er != MH_ERROR_ENABLED) {
        LOG_ERROR(std::string(name) + ": MH_EnableHook failed (" + std::to_string((int)er) + ")", "HotbarAnimation");
        return false;
    }
    return true;
}

} // namespace

void HotbarAnimation::OnScreenFrame(SDK::UIControl* root) {
    g_armed = false;
    if (!root) return;
    pullSettings();
    // "Hotbar Animation" == Off (enum index 3): no glide at all — the vanilla
    // selection box jumps instantly. Early-out keeps the drawImage hook inert.
    if (g_ease == 3) return;

    g_instCount = 0;
    WalkCtx wc = { Offsets().UI_Name, Offsets().UI_Position, Offsets().UI_Flags };
    root->forEachDescendantRaw(&walkVisit, &wc);
    if (g_instCount == 0) {
        g_havePos = false;
        g_selIdx = -1;
        return;
    }

    int bitSet = 0, bitClear = 0;
    for (int i = 0; i < g_instCount; ++i)
        (g_inst[i].flags & 0x10) ? ++bitSet : ++bitClear;
    int sel = -1;
    if (g_instCount == 1) {
        sel = 0;
    } else {
        const unsigned char minorityBit = (bitSet <= bitClear) ? 0x10 : 0x00;
        for (int i = 0; i < g_instCount; ++i)
            if ((g_inst[i].flags & 0x10) == minorityBit) { sel = i; break; }
    }
    if (sel < 0) sel = (g_selIdx >= 0 && g_selIdx < g_instCount) ? g_selIdx : 0;
    g_selIdx = sel;
    const float trueX = g_inst[sel].x;

    static float s_lastTrueX = 0.0f;
    if (g_havePos && std::fabs(trueX - s_lastTrueX) >= 4.0f) {
        const float d = std::fabs(trueX - s_lastTrueX);
        if (d <= 200.0f && (g_step <= 0.0f || d < g_step)) g_step = d;
    }
    s_lastTrueX = trueX;

    if (!g_havePos) {
        g_curX = trueX;
        g_havePos = true;
        g_lastQpc = nowSec();
        if (!g_activeLogged) {
            g_activeLogged = true;
            LOG_INFO("hotbar selection glide active (" + std::to_string(g_instCount) +
                     " cells)", "HotbarAnimation");
        }
        g_armed = true;
        g_outX = g_curX;
        return;
    }

    const double now = nowSec();
    float dt = static_cast<float>(now - g_lastQpc);
    g_lastQpc = now;
    if (dt < 0.0f)  dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;

    const float jumpLimit = 8.75f * (g_step > 0.0f ? g_step : 20.0f);
    if (std::fabs(trueX - g_curX) > jumpLimit) {
        g_curX = trueX;
    } else {
        float alpha = 1.0f - std::exp(-dt * g_speed);
        if (g_ease == 1)      alpha = alpha * alpha * (3.0f - 2.0f * alpha);
        else if (g_ease == 2) alpha = std::sqrt(alpha);
        g_curX += (trueX - g_curX) * alpha;
        if (std::fabs(trueX - g_curX) < 0.05f) g_curX = trueX;
    }

    g_armed = true;
    g_outX = g_curX;
}

void HotbarAnimation::InstallHooks() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    auto& sig = SigManager::GetInstance();
    if (installOne(sig.UIContext_drawImage, &hkUIContextDrawImage,
                   reinterpret_cast<void**>(&oDrawImage), "UIContext::drawImage")) {
        LOG_INFO("hotbar drawImage glide patch installed", "HotbarAnimation");
    } else {
        LOG_WARN("hotbar glide not installed (drawImage sig failed)", "HotbarAnimation");
    }
}
