#include "ItemPhysics.h"
#include "../ModuleManager.h"
#include "../Setting.h"
#include "../../Core/Logger.h"
#include "../../Memory/SigManager.h"

#include <Windows.h>

namespace {

constexpr size_t kMovOffset      = 0x13;   // movaps xmm2, xmm7
constexpr size_t kCallRetOffset  = 0x1B;   // instruction after the call
const unsigned char kMovapsXmm2Xmm7[3] = { 0x0F, 0x28, 0xD7 };
const unsigned char kXorpsXmm2Xmm2[3]  = { 0x0F, 0x57, 0xD2 };

unsigned char* g_site = nullptr;   // the movaps, once verified
bool  g_patched = false;
bool  g_layFlat = false;

bool resolveSite() {
    if (g_site) return true;
    const uintptr_t anchor = SigManager::GetInstance().ItemActor_spinAngleMov;
    if (!anchor) return false;

    auto* p = reinterpret_cast<unsigned char*>(anchor + kMovOffset);
    if (memcmp(p, kMovapsXmm2Xmm7, sizeof(kMovapsXmm2Xmm7)) != 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_ERROR("ItemPhysics: spin-angle site did not match "
                      "(expected movaps xmm2,xmm7) — NoSpin/LayFlat disabled",
                      "ItemPhysics");
        }
        return false;
    }
    g_site = p;
    return true;
}

void writeBytes(const unsigned char* src, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(g_site, n, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(g_site, src, n);
    VirtualProtect(g_site, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_site, n);
}

void setPatched(bool on) {
    if (on == g_patched) return;
    if (!resolveSite()) return;
    writeBytes(on ? kXorpsXmm2Xmm2 : kMovapsXmm2Xmm7, 3);
    g_patched = on;
    LOG_INFO(on ? "NoSpin patch applied" : "NoSpin patch reverted", "ItemPhysics");
}

bool boolSetting(Module* m, const char* name, bool dflt) {
    for (Setting* s : m->mSettings)
        if (s && s->mName == name) return static_cast<BoolSetting*>(s)->mValue;
    return dflt;
}

} // namespace

void ItemPhysics::OnEnable()  { Apply(); }

void ItemPhysics::OnDisable() {
    setPatched(false);
    g_layFlat = false;
}

bool  ItemPhysics::LayFlatEnabled() { return g_layFlat; }

uintptr_t ItemPhysics::SpinCallReturnAddr() {
    const uintptr_t anchor = SigManager::GetInstance().ItemActor_spinAngleMov;
    return anchor ? anchor + kCallRetOffset : 0;
}

void ItemPhysics::Apply() {
    Module* m = ModuleManager::GetInstance().GetModuleByName("ItemPhysics");
    if (!m) return;
    const bool on = m->IsEnabled();

    const bool layFlat = on && boolSetting(m, "Lay Flat", true);
    const bool noSpin  = on && boolSetting(m, "No Spin", true) && !layFlat;

    setPatched(noSpin);
    g_layFlat = layFlat;
}

void ItemPhysics::UpdateSettingsVisibility() {
    Module* m = this;
    const bool layFlat = boolSetting(m, "Lay Flat", true);
    for (Setting* s : mSettings) {
        if (!s) continue;
        if (s->mName == "Height")  s->mVisible = layFlat;
        if (s->mName == "No Spin") s->mVisible = !layFlat;
    }
}

void ItemPhysics::OnUpdate() {
    Apply();
}
