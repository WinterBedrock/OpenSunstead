#pragma once
#include "../Module.h"
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

// Warns when a local-player potion/status effect is about to expire.
// Reads active effects via SDK::MobEffects::getLocal() (MobEffectsComponent).
class EffectWarning : public Module {
public:
    EffectWarning();

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;
    void OnRender() override {}

private:
    float GetWarnSeconds() const;
    bool  GetOnlyBeneficial() const;
    bool  GetNotifyChat() const;
    bool  GetNotifyToast() const;
    float GetWarnCooldown() const;

    // effect id -> last time we warned (seconds, GetTickCount64-based)
    std::unordered_map<uint32_t, double> mLastWarnAt;
    // effects already warned for current remaining window (reset when refreshed)
    std::unordered_set<uint32_t> mWarnedThisWindow;
};
