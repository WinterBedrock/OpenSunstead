#include "Switcher.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/SDK.h"
#include "../Setting.h"
#include "../ModuleManager.h"
#include "../../Core/Logger.h"
#include <string>
#include <Windows.h>

SUNSTEAD_DEFINE_DBG(swDbg, "Switcher", 300)

// Weapon-quality scoring
static int switcherWeaponScore(const std::string& n, bool swordOnly) {
    const bool isSword = n.find("sword") != std::string::npos;
    const bool isAxe   = n.find("axe")   != std::string::npos;
    if (swordOnly && !isSword) return -1;
    if (!isSword && !isAxe)    return -1;
    int tier = 1;
    if      (n.find("netherite") != std::string::npos) tier = 6;
    else if (n.find("diamond")   != std::string::npos) tier = 5;
    else if (n.find("iron")      != std::string::npos) tier = 4;
    else if (n.find("stone")     != std::string::npos) tier = 3;
    else if (n.find("golden") != std::string::npos || n.find("gold") != std::string::npos) tier = 2;
    return tier * 2 + (isSword ? 1 : 0);
}

static int bestWeaponSlot(SDK::Actor* lp, bool swordOnly) {
    SDK::Container* c = SDK::Inv::container(lp);
    if (!c) return -1;
    int best = -1, bestScore = -1;
    for (int slot = 0; slot < 9; ++slot) {
        std::string n = SDK::Inv::lowerNameAt(c, slot);
        if (n.empty()) continue;
        int s = switcherWeaponScore(n, swordOnly);
        if (s > bestScore) { bestScore = s; best = slot; }
    }
    return best;
}

Switcher::Switcher()
    : Module("Switcher", "Swaps to the best weapon the moment you click so the server sees the sword, then restores your slot",
             Category::Combat, 0) {
    AddSetting(new EnumSetting("Weapons", "Which weapons count as the swap target", 0,
                               { "Swords", "Swords+Axes" }));
    AddSetting(new NumberSetting("Restore Delay", "ms after releasing the button before your slot comes back", 500.0f, 0.0f, 3000.0f, 50.0f));
    AddSetting(new BoolSetting("Debug", "Print switch diagnostics to the console", false));
}

void Switcher::OnEnable() {
    mLmbDown = false;
    mActive = false;
    mRestoreSlot = -1;
    mLastAttackMs = 0;
}

void Switcher::OnDisable() {
    SDK::GameContext ctx;
    if (SDK::resolveContext(ctx, false) && ctx.lp) RestoreNow(ctx.lp);
    mLmbDown = false;
    mActive = false;
    mRestoreSlot = -1;
}

void Switcher::Equip(SDK::Actor* lp) {
    const int best = bestWeaponSlot(lp, GetWeaponMode() == 0);
    if (best < 0) return;
    const int cur = SDK::Inv::selectedSlot(lp);
    if (cur == best) return;   // already the weapon - nothing to do
    mRestoreSlot = cur;
    SDK::Inv::setSelectedSlot(lp, best);   // client write + MobEquipmentPacket
    mActive = true;
    swDbg(GetDebug(), "equip: %d -> %d", cur, best);
}

void Switcher::RestoreNow(SDK::Actor* lp) {
    if (!mActive || !lp) return;
    const int cur = SDK::Inv::selectedSlot(lp);
    if (mRestoreSlot >= 0 && mRestoreSlot <= 8 && cur != mRestoreSlot)
        SDK::Inv::setSelectedSlot(lp, mRestoreSlot);
    swDbg(GetDebug(), "restore: %d (held %d)", mRestoreSlot, cur);
    mActive = false;
    mRestoreSlot = -1;
}

void Switcher::OnLocalAttack(SDK::Actor* lp) {
    if (!IsEnabled() || !lp) return;
    mLastAttackMs = GetTickCount64();
    if (mActive) return;
    Equip(lp);
}

void Switcher::OnUpdate() {
    if (!IsEnabled()) return;

    const bool inHud = (SDK::ScreenView::currentScreenName() == "hud_screen");
    Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI");
    const bool guiOpen = (gui && gui->IsEnabled());
    const bool lmb = inHud && !guiOpen && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    const unsigned long long now = GetTickCount64();

    // Fresh click: swap to the weapon IMMEDIATELY, so the MobEquipmentPacket
    // is long gone by the time the attack transaction is serialized.
    if (lmb && !mLmbDown) {
        SDK::GameContext ctx;
        if (SDK::resolveContext(ctx, false) && ctx.lp) {
            mLastAttackMs = now;
            Equip(ctx.lp);
        }
    }
    if (lmb && mActive) mLastAttackMs = now;   // holding the button keeps it out
    mLmbDown = lmb;

    // Button released and no hit for Restore Delay ms -> give the slot back.
    // While the user scrolls manually mid-fight we stay out of the way: the
    // restore only fires when the held slot is still the weapon we equipped.
    if (mActive && !lmb && now - mLastAttackMs >= (unsigned long long)GetHoldMs()) {
        SDK::GameContext ctx;
        if (SDK::resolveContext(ctx, false) && ctx.lp) RestoreNow(ctx.lp);
        else { mActive = false; mRestoreSlot = -1; }
    }
}
