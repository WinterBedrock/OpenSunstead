#pragma once
#include "../Module.h"

class NoClip : public Module {
public:
    NoClip() : Module("NoClip", "Walk through blocks (WASD to move, SPACE/SHIFT for up/down)", Category::Movement, 0) {
        AddSetting(new NumberSetting("Horizontal", "Horizontal movement speed", 1.0f, 0.2f, 11.0f, 0.1f)); // [0]
        AddSetting(new NumberSetting("Vertical", "Vertical movement speed", 0.5f, 0.2f, 5.0f, 0.1f));      // [1]
        AddSetting(new NumberSetting("Glide", "Passive vertical drift while no vertical key is held", 0.0f, -0.75f, 0.75f, 0.01f)); // [2]
        canBind = true;
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;

    float GetHSpeed() const { return static_cast<NumberSetting*>(mSettings[0])->mValue; }
    float GetVSpeed() const { return static_cast<NumberSetting*>(mSettings[1])->mValue; }
    float GetGlide() const { return static_cast<NumberSetting*>(mSettings[2])->mValue; }

private:
    // The AABB we flattened, so disable can restore exactly what we broke.
    // 1.8f is the vanilla player height and what the game itself rebuilds.
    float mSavedHeight = 1.8f;
    bool mCollapsed = false;
};
