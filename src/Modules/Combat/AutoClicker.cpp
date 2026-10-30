#include "AutoClicker.h"
#include "../../SDK/SDK.h"
#include "../../Memory/SigManager.h"
#include "../ModuleManager.h"
#include "../Misc/AntiBot.h"
#include "CombatSignal.h"
#include "../../Core/Logger.h"
#include <vector>
#include <Windows.h>

namespace GM = SDK::GameMode;

namespace {

SDK::DbgThrottle g_dbg;

} // namespace

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

    const bool dbg = GetDebug();
    char msg[192];

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

    // CPS gate. The randomized branch now draws from the shared RNG; it used to
    // be `GetTickCount64() % (max-min)`, which is not random and moved in step
    // with the very clock it was compared against.
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
        // REAL right click: inject RMB DOWN now, schedule the UP ~30ms later so
        // the game's per-frame input poll is guaranteed to see the press. The
        // game's own input path then performs the use exactly like a physical
        // click. Not tied to the game tick — the CPS gate spaces the clicks.
        if (mRightDownSent) return;   // previous press still in flight
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        SendInput(1, &in, sizeof(INPUT));
        mRightDownSent = true;
        mRightUpAtMs = now + 30;
        if (SDK::dbgFormat(g_dbg, dbg, 250, msg, sizeof(msg), "right click cps=%.1f", cps))
            LOG_INFO(msg, "AutoClicker");
        return;
    }

    if (!wantLeft) return;

    SDK::GameContext ctx;
    if (!SDK::resolveContext(ctx, true)) return;
    SDK::Actor* lp = ctx.lp;
    SDK::Level* level = ctx.level;

    void** vt = nullptr;
    void* gm = GM::resolve(lp, &vt);
    if (!gm || !vt) {
        if (SDK::dbgFormat(g_dbg, dbg, 250, msg, sizeof(msg), "GameMode resolve FAIL"))
            LOG_INFO(msg, "AutoClicker");
        return;
    }

    const long long curTick = lp->getTicksExisted();
    if (curTick >= 0 && curTick == mLastLeftTick) return;

    // What is under the crosshair right now? This is the same HitResult the game
    // itself consults when you physically click, so "would this click have hit
    // something?" is answered by the game, not by our own geometry guess.
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
                if (SDK::dbgFormat(g_dbg, dbg, 250, msg, sizeof(msg),
                                   "attack id=%u cps=%.1f", hit.entityId, cps))
                    LOG_INFO(msg, "AutoClicker");
            }
            return;
        }
        // Crosshair said ENTITY but the actor is filtered out or gone — fall
        // through to a plain swing rather than dropping the click entirely.
    }

    if (!GetSwingAtAir()) return;

    // Swing with nothing to hit: vanilla's missed swing. The arm-swing animation
    // goes out on its own; the MISSED_SWING AuthInput flag is added to the next
    // outgoing PlayerAuthInput in OnPacketSend below, which is what tells the
    // server this was a swing rather than a hit.
    if (GM::swing(lp, GM::SwingSource::Attack)) {
        if (curTick >= 0) mLastLeftTick = curTick;
        mPendingMissedSwing = true;
        if (SDK::dbgFormat(g_dbg, dbg, 250, msg, sizeof(msg),
                           "swing air (hit=%d) cps=%.1f", (int)hit.type, cps))
            LOG_INFO(msg, "AutoClicker");
    }
}

// Tag the outgoing PlayerAuthInput with MISSED_SWING for a swing that connected
// with nothing. Vanilla sets this bit on the tick you click at air, and servers
// use it to tell a whiff apart from a hit that they rejected.
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
