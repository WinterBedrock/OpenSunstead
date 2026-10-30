#pragma once
#include "../Module.h"

class CameraClip : public Module {
public:
    CameraClip() : Module("CameraClip", "Third-person camera passes through blocks", Category::Visual, 0) {}

    void OnEnable() override;
    void OnDisable() override;
    static void RestorePatch();
};
