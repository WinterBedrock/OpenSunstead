#pragma once
#include "../Module.h"

class Reach : public Module {
public:
    Reach() : Module("Reach", "Increases block interaction and entity attack range", Category::Combat, 0) {
        AddSetting(new NumberSetting("Distance", "Interact range in blocks", 5.0f, 3.0f, 10.0f, 0.1f));
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnRender() override;
    float GetReachDistance() const;
};