#include "AntiImmobile.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Client/ClientInstance.h"
#include "../../SDK/Client/Chat.h"
#include "../../Core/Logger.h"
#include <string>

extern uintptr_t g_Actor_setImmobile;
extern uintptr_t g_entt_assure_actorDataFlag;
extern uintptr_t g_entt_assure;

AntiImmobile::AntiImmobile()
    : Module("AntiImmobile", "Clears the client-side immobile state so you can always move",
             Category::Movement, 0) {
    AddSetting(new BoolSetting("Notify", "Chat message the first time a freeze is cleared", true));
    AddSetting(new BoolSetting("Clear Cached Tag", "Also drop the game's cached immobile tag component", true));
    canBind = true;
}

bool AntiImmobile::notify() const {
    return static_cast<const BoolSetting*>(mSettings[0])->mValue;
}

bool AntiImmobile::clearTag() const {
    return static_cast<const BoolSetting*>(mSettings[1])->mValue;
}

void AntiImmobile::OnEnable() {
    mReported = false;
    mTagAssureFn = sdk_enttAssureFromSetImmobile();

    if (!g_entt_assure && !g_Actor_setImmobile) {
        LOG_WARN("neither ActorData assure nor Actor::setImmobile resolved — module has no effect",
                 "AntiImmobile");
        if (notify())
            SDK::Chat::displayClientMessage("§c[AntiImmobile] §fUnsupported build: immobile state unreachable");
        return;
    }

    if (notify())
        SDK::Chat::displayClientMessage("§a[AntiImmobile] §fActive — Noai will be kept cleared");
}

void AntiImmobile::OnUpdate() {
    if (!IsEnabled()) return;
    clearNow(true);
}

void AntiImmobile::OnTick() {
    if (!IsEnabled()) return;
    clearNow(false);
}

void AntiImmobile::clearNow(bool allowTagClear) {
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp || !sdk_safe_ptr(lp, 0x20)) return;

    const int hadNoai = lp->getStatusFlag(ActorFlags::Noai);
    const int hadTag  = lp->hasImmobile(mTagAssureFn);

    lp->setStatusFlag(ActorFlags::Noai, false);

    if (allowTagClear && clearTag() && g_Actor_setImmobile)
        lp->setImmobile(false);

    if ((hadNoai == 1 || hadTag == 1) && !mReported) {
        mReported = true;
        if (notify())
            SDK::Chat::displayClientMessage("§a[AntiImmobile] §fFreeze cleared");
    }
}
