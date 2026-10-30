#pragma once
#include "../Module.h"

class MotionBlur : public Module {
public:
    MotionBlur() : Module("MotionBlur", "Blends previous frames for a motion-blur trail", Category::Visual, 0) {
        // Intensity is the base opacity of the most recent past frame (0.01..1).
        AddSetting(new NumberSetting("Intensity", "How strongly past frames show through", 0.55f, 0.05f, 0.95f, 0.01f));
        // How many past frames to keep in the trail ring buffer.
        AddSetting(new NumberSetting("Frames", "How many past frames to accumulate", 8.0f, 1.0f, 20.0f, 1.0f));
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnRender() override;
};
