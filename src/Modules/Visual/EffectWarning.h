#pragma once
#include "../Module.h"
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

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

    std::unordered_map<uint32_t, double> mLastWarnAt;
    std::unordered_set<uint32_t> mWarnedThisWindow;
};
