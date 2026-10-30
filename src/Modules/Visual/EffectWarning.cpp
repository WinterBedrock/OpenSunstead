#include "EffectWarning.h"
#include "../Setting.h"
#include "Notifications.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Actor/MobEffects.h"
#include "../../SDK/Actor/MobEffect.h"
#include "../../SDK/Client/Chat.h"
#include <Windows.h>
#include <cstdio>
#include <string>

EffectWarning::EffectWarning()
    : Module("EffectWarning", "Warn when a status effect is about to expire", Category::Visual, 0) {
    // Seconds remaining at/below which we fire a warning.
    AddSetting(new NumberSetting("Seconds", "Warn this many seconds before expiry", 5.0f, 1.0f, 30.0f, 0.5f));
    AddSetting(new BoolSetting("Only Beneficial", "Ignore harmful effects (poison, wither, ...)", true));
    AddSetting(new BoolSetting("Notification", "Show a toast notification", true));
    AddSetting(new BoolSetting("Chat", "Also print to client chat", false));
    // Minimum gap between warnings for the same effect id (after refresh / re-apply).
    AddSetting(new NumberSetting("Cooldown", "Min seconds between warnings for the same effect", 3.0f, 0.5f, 30.0f, 0.5f));
    canBind = true;
}

float EffectWarning::GetWarnSeconds() const {
    return static_cast<const NumberSetting*>(mSettings[0])->mValue;
}

bool EffectWarning::GetOnlyBeneficial() const {
    return static_cast<const BoolSetting*>(mSettings[1])->mValue;
}

bool EffectWarning::GetNotifyToast() const {
    return static_cast<const BoolSetting*>(mSettings[2])->mValue;
}

bool EffectWarning::GetNotifyChat() const {
    return static_cast<const BoolSetting*>(mSettings[3])->mValue;
}

float EffectWarning::GetWarnCooldown() const {
    return static_cast<const NumberSetting*>(mSettings[4])->mValue;
}

void EffectWarning::OnEnable() {
    mLastWarnAt.clear();
    mWarnedThisWindow.clear();
}

void EffectWarning::OnDisable() {
    mLastWarnAt.clear();
    mWarnedThisWindow.clear();
}

static bool isHarmful(uint32_t id) {
    using ET = SDK::EffectType;
    switch (static_cast<ET>(id)) {
    case ET::Slowness:
    case ET::MiningFatigue:
    case ET::InstantDamage:
    case ET::Nausea:
    case ET::Blindness:
    case ET::Hunger:
    case ET::Weakness:
    case ET::Poison:
    case ET::Wither:
    case ET::Levitation:
    case ET::FatalPoison:
    case ET::BadOmen:
    case ET::Darkness:
    case ET::TrialOmen:
    case ET::WindCharged:
    case ET::Weaving:
    case ET::Oozing:
    case ET::Infested:
    case ET::RaidOmen:
        return true;
    default:
        return false;
    }
}

void EffectWarning::OnTick() {
    if (!IsEnabled()) return;

    const float warnSec = GetWarnSeconds();
    if (warnSec <= 0.f) return;
    const int warnTicks = static_cast<int>(warnSec * 20.f + 0.5f);

    std::vector<SDK::MobEffectData> effects = SDK::MobEffects::getLocal();
    if (effects.empty()) {
        mWarnedThisWindow.clear();
        return;
    }

    const double now = static_cast<double>(GetTickCount64()) * 0.001;
    const float cooldown = GetWarnCooldown();
    const bool onlyGood = GetOnlyBeneficial();
    const bool toast = GetNotifyToast();
    const bool chat = GetNotifyChat();
    if (!toast && !chat) return;

    // Track which effects are still active so we can clear warn state when they drop.
    std::unordered_set<uint32_t> active;
    active.reserve(effects.size());

    for (const auto& e : effects) {
        if (e.id == 0 || e.id > 36) continue;
        active.insert(e.id);

        if (e.infinite || e.durationTicks <= 0) {
            // Refreshed to infinite / permanent — allow a new warn later if it becomes finite.
            mWarnedThisWindow.erase(e.id);
            continue;
        }

        // Still far from expiry — clear one-shot flag so a later re-entry can warn again.
        if (e.durationTicks > warnTicks) {
            mWarnedThisWindow.erase(e.id);
            continue;
        }

        if (onlyGood && isHarmful(e.id)) continue;
        if (mWarnedThisWindow.count(e.id)) continue;

        auto it = mLastWarnAt.find(e.id);
        if (it != mLastWarnAt.end() && (now - it->second) < static_cast<double>(cooldown))
            continue;

        const char* name = SDK::effectTypeName(e.id);
        const int secLeft = e.durationTicks / 20;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s expiring in %ds", name, secLeft > 0 ? secLeft : 0);

        if (toast) Notifications::AddNotification(buf);
        if (chat) {
            char chatBuf[160];
            std::snprintf(chatBuf, sizeof(chatBuf),
                "§7[§eEffectWarning§7]§r §f%s §7expiring in §e%d§7s",
                name, secLeft > 0 ? secLeft : 0);
            SDK::Chat::displayClientMessage(chatBuf);
        }

        mLastWarnAt[e.id] = now;
        mWarnedThisWindow.insert(e.id);
    }

    // Drop warn state for effects that are no longer on the player.
    for (auto it = mWarnedThisWindow.begin(); it != mWarnedThisWindow.end(); ) {
        if (!active.count(*it)) it = mWarnedThisWindow.erase(it);
        else ++it;
    }
}
