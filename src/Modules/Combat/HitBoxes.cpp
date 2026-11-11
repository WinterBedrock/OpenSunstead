#include "HitBoxes.h"
#include "../../Utils/MCSDK.h"
#include "../ModuleManager.h"
#include "../Misc/AntiBot.h"

#include <unordered_set>
#include <vector>

namespace {

AABBShapeComponent* getAabb(SDK::Actor* actor) {
    if (!actor || !sdk_safe_ptr(actor, Offsets().Actor_mAABB + sizeof(void*))) return nullptr;
    __try { return actor->getAabbs(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void restoreAabb(SDK::Actor* actor, const AABBShapeComponent& original) {
    if (!actor || !sdk_safe_ptr(actor, Offsets().Actor_mAABB + sizeof(void*))) return;
    auto* aabb = getAabb(actor);
    if (!aabb) return;
    __try { *aabb = original; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void expandAabb(AABBShapeComponent* aabb, float width, float height) {
    if (!sdk_safe_ptr(aabb, sizeof(AABBShapeComponent))) return;
    __try {
        const float centerX = (aabb->mMin.x + aabb->mMax.x) * 0.5f;
        const float centerZ = (aabb->mMin.z + aabb->mMax.z) * 0.5f;
        aabb->mMin.x = centerX - width * 0.5f;
        aabb->mMax.x = centerX + width * 0.5f;
        aabb->mMin.z = centerZ - width * 0.5f;
        aabb->mMax.z = centerZ + width * 0.5f;
        aabb->mMax.y = aabb->mMin.y + height;
        aabb->width = width;
        aabb->height = height;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

}

HitBoxes::HitBoxes() : Module("HitBoxes", "Expands hitboxes", Category::Combat) {
    AddSetting(new EnumSetting("Targets", "Whose hitboxes to modify", 0,
        { "All Entities", "Players Only", "Players + AntiBot" }));
    widthSetting = new NumberSetting("Width", "Expanded hitbox width", 2.0f, 0.6f, 5.0f, 0.1f);
    heightSetting = new NumberSetting("Height", "Expanded hitbox height", 2.0f, 1.8f, 5.0f, 0.1f);
    AddSetting(widthSetting);
    AddSetting(heightSetting);
}

void HitBoxes::OnTick() {
    SDK::GameContext context;
    if (!SDK::resolveContext(context)) {
        trackedLevel = nullptr;
        originalBoxes.clear();
        return;
    }
    if (trackedLevel != context.level) {
        trackedLevel = context.level;
        originalBoxes.clear();
    }

    std::vector<SDK::Actor*> actors;
    if (!SDK::sehGetActorList(context.level, &actors)) return;

    std::unordered_set<SDK::Actor*> present;
    present.reserve(actors.size());
    for (SDK::Actor* actor : actors)
        if (actor && sdk_safe_ptr(actor, Offsets().Actor_mAABB + sizeof(void*))) present.insert(actor);

    for (auto it = originalBoxes.begin(); it != originalBoxes.end();) {
        if (!present.contains(it->first)) it = originalBoxes.erase(it);
        else ++it;
    }

    auto* antiBot = static_cast<AntiBot*>(ModuleManager::GetInstance().GetModuleByName("AntiBot"));
    const int targets = GetTargetMode();
    for (SDK::Actor* actor : present) {
        if (actor == context.lp) continue;
        const bool selected = targets == 0 || (SDK::sehIsPlayer(actor) &&
            (targets != 2 || !antiBot || !antiBot->IsBot(actor)));

        auto* aabb = getAabb(actor);
        if (!aabb) continue;
        auto original = originalBoxes.find(actor);
        if (!selected) {
            if (original != originalBoxes.end()) {
                restoreAabb(actor, original->second);
                originalBoxes.erase(original);
            }
            continue;
        }
        if (original == originalBoxes.end())
            original = originalBoxes.emplace(actor, *aabb).first;
        expandAabb(aabb, widthSetting->mValue, heightSetting->mValue);
    }
}

void HitBoxes::OnDisable() {
    SDK::GameContext context;
    if (!SDK::resolveContext(context)) {
        originalBoxes.clear();
        trackedLevel = nullptr;
        return;
    }

    std::vector<SDK::Actor*> actors;
    if (SDK::sehGetActorList(context.level, &actors)) {
        for (SDK::Actor* actor : actors) {
            const auto it = originalBoxes.find(actor);
            if (it != originalBoxes.end()) restoreAabb(actor, it->second);
        }
    }
    originalBoxes.clear();
    trackedLevel = nullptr;
}
