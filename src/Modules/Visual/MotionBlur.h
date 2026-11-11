#pragma once
#include "../Module.h"

class MotionBlur : public Module {
public:
    MotionBlur() : Module("MotionBlur", "Blends previous frames for a motion-blur trail", Category::Visual, 0) {
        AddSetting(new NumberSetting("Intensity", "How strongly past frames show through", 0.55f, 0.05f, 0.95f, 0.01f));
        AddSetting(new NumberSetting("Frames", "How many past frames to accumulate", 8.0f, 1.0f, 20.0f, 1.0f));
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnRender() override;
};
