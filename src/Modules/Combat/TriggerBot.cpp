#include "TriggerBot.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/SDK.h"
#include "../Setting.h"
#include "../ModuleManager.h"
#include "../Misc/AntiBot.h"
#include "CombatSignal.h"
#include "../../Core/Logger.h"
#include <vector>
#include <cmath>
#include <Windows.h>

using SDK::sehGetPos;
using SDK::sehGetActorList;
namespace GM = SDK::GameMode;

static SDK::DbgThrottle g_tbDbg;

void TriggerBot::OnUpdate() {
    if (!IsEnabled()) return;
    const bool dbg = GetDebug();
    char msg[192];

    SDK::GameContext ctx;
    if (!SDK::resolveContext(ctx, true)) return;
    SDK::Level* level = ctx.level;
    SDK::Actor* lp = ctx.lp;

    // Solstice logic: look at the crosshair HitResult. Attack ONLY when it is an
    // ENTITY, resolving the exact actor by its entity id. This is what makes
    // Solstice's TriggerBot reliable (no geometric FOV guessing).
    GM::CrosshairHit hit{};
    if (!GM::readCrosshair(level, &hit)) return;
    if (hit.type != SDK::HitType::ENTITY) return; // not aiming at an entity
    const unsigned int hitId = hit.entityId;

    unsigned long long now = GetTickCount64();

    // Randomized APS throttle (Solstice: random between Min/Max each tick).
    float apsMin = GetAPSMin(), apsMax = GetAPSMax();
    if (apsMax < apsMin + 1.0f) apsMax = apsMin + 1.0f;
    float aps = SDK::randRange(apsMin, apsMax);
    if (aps < 1.0f) aps = 1.0f;
    unsigned long long delay = (unsigned long long)(1000.0f / aps);
    if (now - mLastAttack < delay) return;

    // One attack per game tick (clean PlayerAuthInput serialization).
    long long curTick = lp->getTicksExisted();
    if (curTick >= 0 && curTick == mLastAttackTick) return;

    std::vector<SDK::Actor*> actors;
    if (!sehGetActorList(level, &actors)) return;

    AntiBot* antiBot = static_cast<AntiBot*>(ModuleManager::GetInstance().GetModuleByName("AntiBot"));
    SDK::Actor* target = GM::actorFromEntityId(
        actors, lp, hitId,
        [antiBot](SDK::Actor* a) { return antiBot && antiBot->IsBot(a); });
    if (!target) {
        if (SDK::dbgFormat(g_tbDbg, dbg, 250, msg, sizeof(msg),
                           "hit entity id=%u not found in actor list", hitId))
            LOG_INFO(msg, "TriggerBot");
        return;
    }

    // Optional range gate (the crosshair can register beyond melee reach).
    Vec3 myPos{}, tPos{};
    if (sehGetPos(lp, &myPos) && sehGetPos(target, &tPos)) {
        float range = GetRange();
        if (vecDistanceSq(tPos, myPos) > range * range) {
            if (SDK::dbgFormat(g_tbDbg, dbg, 250, msg, sizeof(msg), "target out of range"))
                LOG_INFO(msg, "TriggerBot");
            return;
        }
    }

    void** vt = nullptr;
    void* gm = GM::resolve(lp, &vt);
    if (!gm || !vt) {
        if (SDK::dbgFormat(g_tbDbg, dbg, 250, msg, sizeof(msg), "GameMode resolve FAIL"))
            LOG_INFO(msg, "TriggerBot");
        return;
    }

    // Prime the HitResult as ENTITY (it already is) and swing + attack — the
    // exact Solstice order: player->swing(); gameMode->attack(actor).
    GM::prepEntityHit(level);

    // attack() needs the click position as a real third argument (the game copies
    // it into the outbound transaction). The crosshair HitResult already holds
    // the exact point the ray hit, so prefer it and fall back to the target pos.
    Vec3 clickPos = hit.valid ? hit.pos : tPos;

    GM::swing(lp, GM::SwingSource::Attack);
    if (GM::attack(gm, vt, target, &clickPos)) {
        mLastAttack = now;
        if (curTick >= 0) mLastAttackTick = curTick;
        CombatSignal::registerHit(target);
        if (SDK::dbgFormat(g_tbDbg, dbg, 250, msg, sizeof(msg),
                           "attacked id=%u aps=%.1f", hitId, aps))
            LOG_INFO(msg, "TriggerBot");
    }
}
