#include "ItemPhysics.h"
#include "../ModuleManager.h"
#include "../Setting.h"
#include "../../Core/Logger.h"
#include "../../Memory/SigManager.h"

#include <Windows.h>

// ---------------------------------------------------------------------------
// The spin is removed by neutralising the angle argument in place.
//
// From the anchor the signature resolves (the `mulss xmm7, deg2rad`), the
// instruction stream is:
//
//   +0x00  F3 0F 59 3D ....   mulss  xmm7, [rip+deg2rad]
//   +0x08  48 8D 4D ??        lea    rcx, [rbp+out]
//   +0x0C  4C 8D 4D ??        lea    r9,  [rbp+axis]
//   +0x10  4C 89 FA           mov    rdx, r15
//   +0x13  0F 28 D7           movaps xmm2, xmm7      <-- the angle argument
//   +0x16  E8 ....            call   Matrix_rotateAxisAngle
//   +0x1B                     <-- return address of that call
//
// `xorps xmm2, xmm2` is also three bytes (0F 57 D2), so the angle becomes zero
// with no length change, no trampoline and no relocation.
//
// Why patch rather than hook: the angle is a register-passed argument computed
// inline. There is no function boundary to intercept — a detour would need a
// 5-byte landing site and would clobber the very instruction that matters.
// ---------------------------------------------------------------------------
namespace {

// Offsets from the resolved anchor, and the bytes involved.
constexpr size_t kMovOffset      = 0x13;   // movaps xmm2, xmm7
constexpr size_t kCallRetOffset  = 0x1B;   // instruction after the call
const unsigned char kMovapsXmm2Xmm7[3] = { 0x0F, 0x28, 0xD7 };
const unsigned char kXorpsXmm2Xmm2[3]  = { 0x0F, 0x57, 0xD2 };

unsigned char* g_site = nullptr;   // the movaps, once verified
bool  g_patched = false;
bool  g_noBob   = false;
bool  g_layFlat = false;
float g_height  = 0.0f;

// Resolve and verify the patch site. Refuses unless the bytes are exactly the
// movaps expected, so a shifted layout in a future build turns the feature off
// instead of corrupting an unrelated instruction.
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

float numSetting(Module* m, const char* name, float dflt) {
    for (Setting* s : m->mSettings)
        if (s && s->mName == name) return static_cast<NumberSetting*>(s)->mValue;
    return dflt;
}

} // namespace

void ItemPhysics::OnEnable()  { Apply(); }

void ItemPhysics::OnDisable() {
    setPatched(false);
    g_noBob = g_layFlat = false;
    g_height = 0.0f;
}

bool  ItemPhysics::NoBobEnabled()   { return g_noBob; }
bool  ItemPhysics::LayFlatEnabled() { return g_layFlat; }
float ItemPhysics::HeightOffset()   { return g_height; }

uintptr_t ItemPhysics::SpinCallReturnAddr() {
    const uintptr_t anchor = SigManager::GetInstance().ItemActor_spinAngleMov;
    return anchor ? anchor + kCallRetOffset : 0;
}

void ItemPhysics::Apply() {
    Module* m = ModuleManager::GetInstance().GetModuleByName("ItemPhysics");
    if (!m) return;
    const bool on = m->IsEnabled();

    // Lay Flat needs the spin patch OFF: it works by substituting the axis and
    // angle of that same rotate call, so zeroing the angle first would leave
    // nothing to substitute. No Spin is redundant while Lay Flat is on anyway —
    // the substituted angle is constant, so the item does not turn either way.
    const bool layFlat = on && boolSetting(m, "Lay Flat", true);
    const bool noSpin  = on && boolSetting(m, "No Spin", true) && !layFlat;

    setPatched(noSpin);
    g_layFlat = layFlat;
    g_noBob   = on && boolSetting(m, "No Bob", true);
    g_height  = layFlat ? numSetting(m, "Height", 0.22f) : 0.0f;
}

void ItemPhysics::UpdateSettingsVisibility() {
    Module* m = this;
    const bool layFlat = boolSetting(m, "Lay Flat", true);
    for (Setting* s : mSettings) {
        if (!s) continue;
        // Height only means anything while the item is being laid down, and
        // No Spin is subsumed by Lay Flat (see Apply).
        if (s->mName == "Height")  s->mVisible = layFlat;
        if (s->mName == "No Spin") s->mVisible = !layFlat;
    }
}

void ItemPhysics::OnUpdate() {
    // Settings can change without a module toggle (config load, clicking a
    // sub-setting), so the desired state is reconciled here rather than only on
    // enable. Apply and setPatched are both no-ops when nothing changed.
    Apply();
}
