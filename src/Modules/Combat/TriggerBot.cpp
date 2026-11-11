#include "TriggerBot.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/SDK.h"
#include "../Setting.h"
#include "../ModuleManager.h"
#include "../Misc/AntiBot.h"
#include "CombatSignal.h"
#include <vector>
#include <cmath>
#include <Windows.h>

using SDK::sehGetPos;
using SDK::sehGetActorList;
namespace GM = SDK::GameMode;

void TriggerBot::OnUpdate() {
    if (!IsEnabled()) return;

    SDK::GameContext ctx;
    if (!SDK::resolveContext(ctx, true)) return;
    SDK::Level* level = ctx.level;
    SDK::Actor* lp = ctx.lp;

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
    if (!target) return;

    // Optional range gate (the crosshair can register beyond melee reach).
    Vec3 myPos{}, tPos{};
    if (sehGetPos(lp, &myPos) && sehGetPos(target, &tPos)) {
        float range = GetRange();
        if (vecDistanceSq(tPos, myPos) > range * range) return;
    }

    void** vt = nullptr;
    void* gm = GM::resolve(lp, &vt);
    if (!gm || !vt) return;

    // Prime the HitResult as ENTITY (it already is) and swing + attack — the
    // exact Solstice order: player->swing(); gameMode->attack(actor).
    GM::prepEntityHit(level);

    Vec3 clickPos = hit.valid ? hit.pos : tPos;

    GM::swing(lp, GM::SwingSource::Attack);
    if (GM::attack(gm, vt, target, &clickPos)) {
        mLastAttack = now;
        if (curTick >= 0) mLastAttackTick = curTick;
        CombatSignal::registerHit(target);
    }
}
