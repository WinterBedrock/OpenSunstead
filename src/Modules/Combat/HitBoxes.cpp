#include "HitBoxes.h"
#include "../../Utils/MCSDK.h"
#include "../ModuleManager.h"
#include "../Misc/AntiBot.h"

static bool sehExpandAabb(AABBShapeComponent* aabbs, float w, float h) {
    if (!sdk_safe_ptr(aabbs, sizeof(AABBShapeComponent))) return false;
    __try {
        float cx = (aabbs->mMin.x + aabbs->mMax.x) * 0.5f;
        float cz = (aabbs->mMin.z + aabbs->mMax.z) * 0.5f;
        float feetY = aabbs->mMin.y;
        float hw = w * 0.5f;
        aabbs->mMin.x = cx - hw; aabbs->mMin.z = cz - hw;
        aabbs->mMax.x = cx + hw; aabbs->mMax.z = cz + hw;
        aabbs->mMax.y = feetY + h;
        aabbs->width = w; aabbs->height = h;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// SEH-guarded getAabbs — the Actor+0x220 component-pointer deref is the one
// per-actor engine read. Validated + guarded so a bad actor can't throw.
// AABBShapeComponent lives in the global namespace (Types.h), so no SDK:: prefix.
static AABBShapeComponent* sehGetAabbs(SDK::Actor* actor) {
    if (!actor || !sdk_safe_ptr(actor, 0x228)) return nullptr;
    __try { return actor->getAabbs(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Apply (w,h) to every actor matching the target filter's AABB width/height.
// SEH-wrapped per actor; returns the count of actors actually written. This is
// the only place engine component memory is touched.
static int applyHitbox(SDK::Level* level, SDK::Actor* lp, float w, float h, int targetMode) {
    int processed = 0;
    std::vector<SDK::Actor*> actors;
    if (!sehGetActorList(level, &actors)) return 0;

    AntiBot* antiBot = (targetMode == 2)
        ? static_cast<AntiBot*>(ModuleManager::GetInstance().GetModuleByName("AntiBot"))
        : nullptr;

    for (auto* actor : actors) {
        if (!actor || actor == lp) continue;
        if (targetMode >= 1 && !sehIsPlayer(actor)) continue;   // players-only modes
        if (targetMode == 2 && antiBot && antiBot->IsBot(actor)) continue;
        auto* aabbs = sehGetAabbs(actor);
        if (!aabbs) continue;
        if (sehExpandAabb(aabbs, w, h))
            ++processed;
    }
    return processed;
}

HitBoxes::HitBoxes() : Module("HitBoxes", "Expands hitboxes", Category::Combat) {
    AddSetting(new EnumSetting("Targets", "Whose hitboxes to modify", 0,
        { "All Entities", "Players Only", "Players + AntiBot" }));                       // [0]
    widthSetting = new NumberSetting("Width", "Expanded hitbox width", 2.0f, 0.6f, 5.0f, 0.1f);   // [1]
    heightSetting = new NumberSetting("Height", "Expanded hitbox height", 2.0f, 1.8f, 5.0f, 0.1f); // [2]
    AddSetting(widthSetting);
    AddSetting(heightSetting);
}

void HitBoxes::OnDisable() {
    // Restore each target's ORIGINAL base size. With "All Entities" enabled
    // the module wrote non-player boxes too, so the restore must run with the
    // same filter to touch the same set of actors. Snapshot the local
    // player's size as the vanilla reference (all players share it in the
    // common case; mobs get their engine-recomputed size back on their next
    // physics tick regardless).
    SDK::ClientInstance* ci = nullptr;
    SDK::Level* level = nullptr;
    SDK::Actor* lp = nullptr;
    if (!SDK::resolveContext(&ci, &level, &lp)) return;

    auto* lpAabbs = sehGetAabbs(lp);
    if (!lpAabbs) return;

    applyHitbox(level, lp, lpAabbs->width, lpAabbs->height, GetTargetMode());
}

void HitBoxes::OnTick() {
    if (!IsEnabled()) return;

    SDK::ClientInstance* ci = nullptr;
    SDK::Level* level = nullptr;
    SDK::Actor* lp = nullptr;
    if (!SDK::resolveContext(&ci, &level, &lp)) return;

    float w = widthSetting->mValue;
    float h = heightSetting->mValue;
    applyHitbox(level, lp, w, h, GetTargetMode());
}
