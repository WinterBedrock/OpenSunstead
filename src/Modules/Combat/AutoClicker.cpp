#include "AutoClicker.h"
#include "../../SDK/SDK.h"
#include "../../Memory/SigManager.h"
#include "../ModuleManager.h"
#include "../Misc/AntiBot.h"
#include "CombatSignal.h"
#include <vector>
#include <Windows.h>

namespace GM = SDK::GameMode;

void AutoClicker::UpdateSettingsVisibility() {
    const bool rnd = GetRandomize();
    mSettings[3]->mVisible = !rnd;   // CPS
    mSettings[4]->mVisible = rnd;    // CPS Min
    mSettings[5]->mVisible = rnd;    // CPS Max
}

void AutoClicker::OnDisable() {
    // A synthetic right press must never outlive the module.
    if (mRightDownSent) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        SendInput(1, &in, sizeof(INPUT));
        mRightDownSent = false;
    }
}void AutoClicker::OnTick() {
    if (!IsEnabled()) return;

    // Finish an in-flight synthetic right press even when a gate below bails
    // out, so the button can never stick down.
    if (mRightDownSent && SDK::nowMs() >= mRightUpAtMs) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        SendInput(1, &in, sizeof(INPUT));
        mRightDownSent = false;
    }

    // Never act while alt-tabbed or while a menu/inventory/ClickGUI is up. The
    // real-click Right mode especially must only ever click the in-game HUD.
    if (!SDK::isGameWindowFocused()) return;
    if (!SDK::isHudScreen()) return;
    Module* cg = ModuleManager::GetInstance().GetModuleByName("ClickGUI");
    if (cg && cg->IsEnabled()) return;

    const int mode = GetClickMode();
    const bool hold = GetHold();
    const bool lmbHeld = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool rmbHeld = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

    const bool wantLeft  = (mode == 0) && (!hold || lmbHeld);
    const bool wantRight = (mode == 1) && (!hold || rmbHeld);
    if (!wantLeft && !wantRight) return;

    const uint64_t now = SDK::nowMs();
    if (now < mNextClickMs) return;

    float cps;
    if (GetRandomize()) {
        float mn = GetCpsMin(), mx = GetCpsMax();
        if (mx < mn) { const float t = mn; mn = mx; mx = t; }
        cps = SDK::randRange(mn, mx);
    } else {
        cps = GetCps();
    }
    if (cps < 1.0f) cps = 1.0f;
    mNextClickMs = now + (uint64_t)(1000.0f / cps);

    if (wantRight) {
        if (mRightDownSent) return;   // previous press still in flight
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        SendInput(1, &in, sizeof(INPUT));
        mRightDownSent = true;
        mRightUpAtMs = now + 30;
        return;
    }

    if (!wantLeft) return;

    SDK::GameContext ctx;
    if (!SDK::resolveContext(ctx, true)) return;
    SDK::Actor* lp = ctx.lp;
    SDK::Level* level = ctx.level;

    void** vt = nullptr;
    void* gm = GM::resolve(lp, &vt);
    if (!gm || !vt) return;

    const long long curTick = lp->getTicksExisted();
    if (curTick >= 0 && curTick == mLastLeftTick) return;

    GM::CrosshairHit hit{};
    const bool haveHit = GM::readCrosshair(level, &hit);
    const bool onEntity = haveHit && hit.type == SDK::HitType::ENTITY;

    if (onEntity && GetAttackEntities()) {
        std::vector<SDK::Actor*> actors;
        if (!SDK::sehGetActorList(level, &actors)) return;

        AntiBot* antiBot = static_cast<AntiBot*>(
            ModuleManager::GetInstance().GetModuleByName("AntiBot"));
        SDK::Actor* target = GM::actorFromEntityId(
            actors, lp, hit.entityId,
            [antiBot](SDK::Actor* a) { return antiBot && antiBot->IsBot(a); });

        if (target) {
            // Swing first, then attack: the order the game uses, and the order
            // that makes the server register a hit rather than just a look.
            Vec3 clickPos = hit.pos;
            GM::prepEntityHit(level);
            GM::swing(lp, GM::SwingSource::Attack);
            if (GM::attack(gm, vt, target, &clickPos)) {
                if (curTick >= 0) mLastLeftTick = curTick;
                CombatSignal::registerHit(target);
            }
            return;
        }
        // Crosshair said ENTITY but the actor is filtered out or gone — fall
        // through to a plain swing rather than dropping the click entirely.
    }

    if (!GetSwingAtAir()) return;

    if (GM::swing(lp, GM::SwingSource::Attack)) {
        if (curTick >= 0) mLastLeftTick = curTick;
        mPendingMissedSwing = true;
    }
}

void AutoClicker::OnPacketSend(SDK::Packet* packet, int packetId, bool& /*cancel*/) {
    if (!IsEnabled() || !packet) return;
    if (packetId != static_cast<int>(SDK::PacketID::PlayerAuthInput)) return;
    if (!mPendingMissedSwing) return;
    mPendingMissedSwing = false;
    __try {
        reinterpret_cast<SDK::PlayerAuthInputPacket*>(packet)
            ->addInput(SDK::AuthInputAction::MISSED_SWING);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
