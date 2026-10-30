#include "AntiImmobile.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Client/ClientInstance.h"
#include "../../SDK/Client/Chat.h"
#include "../../Core/Logger.h"
#include <cstdio>
#include <string>

extern uintptr_t g_Actor_setImmobile;
extern uintptr_t g_entt_assure_actorDataFlag;
// The generic entt assure — this is what actually drives setStatusFlag's working
// tryGet<> path, so it, not the specialized ActorDataFlag assure, decides whether
// the Noai clear can run at all.
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
    mNoaiClears = 0;
    mTagClears = 0;
    mWarned = false;
    mReported = false;
    mStateLogged = false;
    mTagAssureFn = sdk_enttAssureFromSetImmobile();

    // Two independent mechanisms, either of which is enough to be useful:
    //   * the Noai bit, reachable through the generic g_entt_assure, and
    //   * the cached tag component, dropped via Actor::setImmobile.
    // Only bail when BOTH are unreachable. The old test looked at
    // g_entt_assure_actorDataFlag, which is merely the fallback accessor, so a
    // miss there disabled the module even though the Noai write would have worked.
    if (!g_entt_assure && !g_Actor_setImmobile) {
        LOG_WARN("neither ActorData assure nor Actor::setImmobile resolved — module has no effect",
                 "AntiImmobile");
        if (notify())
            SDK::Chat::displayClientMessage("§c[AntiImmobile] §fUnsupported build: immobile state unreachable");
        mWarned = true;
        return;
    }

    char buf[192];
    std::snprintf(buf, sizeof(buf),
        "ON assure=0x%llX assureActorData=0x%llX setImmobile=0x%llX tagAssure=0x%llX",
        (unsigned long long)g_entt_assure,
        (unsigned long long)g_entt_assure_actorDataFlag,
        (unsigned long long)g_Actor_setImmobile,
        (unsigned long long)mTagAssureFn);
    LOG_INFO(buf, "AntiImmobile");
    if (notify())
        SDK::Chat::displayClientMessage("§a[AntiImmobile] §fActive — Noai will be kept cleared");
}

void AntiImmobile::OnDisable() {
    // Nothing to restore: we never set the flag, and the server is free to
    // re-apply it whenever it wants. Stopping simply lets the next freeze stick.
    LOG_INFO("OFF noaiClears=" + std::to_string(mNoaiClears) +
             " tagClears=" + std::to_string(mTagClears), "AntiImmobile");
}

void AntiImmobile::OnUpdate() {
    if (!IsEnabled()) return;
    // GAME thread. ModuleManager dispatches OnUpdate from hkUpdatePlayer
    // (UpdatePlayerFromCameraSystemUtil::_updatePlayer) — see ModuleManager.h —
    // so this, not OnTick, is where the freeze is applied by the SetActorData
    // handler and where the movement integrator reads it back. Both the in-place
    // Noai bit flip and the entt structural remove of the cached tag belong here;
    // doing the tag remove from the render thread (as the previous build did)
    // raced entt's sparse-set bookkeeping and landed a frame late.
    clearNow(true);
}

void AntiImmobile::OnTick() {
    // RENDER thread in this codebase (dispatched from LevelRenderer::renderLevel).
    // Only the idempotent Noai bit write runs here, as a cheap safety net for the
    // frames between game ticks. No entt structural changes off the game thread.
    if (!IsEnabled()) return;
    clearNow(false);
}

void AntiImmobile::clearNow(bool allowTagClear) {
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp || !sdk_safe_ptr(lp, 0x20)) return;

    // Observe before clearing (diagnostics only — the clears below do NOT depend
    // on these reads succeeding).
    const int hadNoai = lp->getStatusFlag(ActorFlags::Noai);
    const int hadTag  = lp->hasImmobile(mTagAssureFn);

    // (1) Noai bit — the predicate's primary gate: sub_141ED7610 returns "immobile"
    // as soon as `flagsWord0 & 0x10000` is set. Idempotent in-place write.
    //
    // NOT gated on g_entt_assure_actorDataFlag. setStatusFlag's working path is
    // tryGet<ActorDataFlagComponentRaw> driven by the GENERIC g_entt_assure (which
    // resolves reliably); the specialized assure is only used by the raw-walk
    // fallback. Gating the call on it meant that whenever that one signature
    // missed, the module skipped a write that would have succeeded — the main
    // reason it "did nothing".
    lp->setStatusFlag(ActorFlags::Noai, false);

    // (2) The ACTUAL freeze on most servers: the ActorIsImmobileFlagComponent
    // tag. The movement integrator (sub_14CC50C20) treats "has this tag" as
    // immobile REGARDLESS of the Noai bit, which is why noai=0 and you were
    // still frozen. Call the game's own setter unconditionally — it removes the
    // component only if present and is SEH-guarded, so gating on a possibly
    // unreadable `hadTag` (which was the old bug: hadTag==-1 skipped the clear)
    // is both unnecessary and wrong.
    if (allowTagClear && clearTag() && g_Actor_setImmobile) {
        lp->setImmobile(false);
        if (hadTag == 1) ++mTagClears;
    }

    if (hadNoai == 1) ++mNoaiClears;

    if ((hadNoai == 1 || hadTag == 1) && !mReported) {
        mReported = true;
        LOG_INFO("cleared immobile state (server had frozen you)", "AntiImmobile");
        if (notify())
            SDK::Chat::displayClientMessage("§a[AntiImmobile] §fFreeze cleared");
    }

    // One-shot state report so "nothing happens" stays diagnosable from the log.
    if (!mStateLogged) {
        mStateLogged = true;
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            "first tick: noai=%d cachedTag=%d assureData=%d setImmobile=%d "
            "(1=set 0=clear -1=unreadable) — tag is the real freeze",
            hadNoai, hadTag, g_entt_assure_actorDataFlag ? 1 : 0,
            g_Actor_setImmobile ? 1 : 0);
        LOG_INFO(buf, "AntiImmobile");
        if (!g_Actor_setImmobile)
            LOG_WARN("Actor::setImmobile unresolved — cannot drop the immobile tag, the freeze will stick",
                     "AntiImmobile");
    }
}
