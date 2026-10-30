#pragma once
#include "../Module.h"
#include <Windows.h>

// Jesus — walk on water by detouring WaterBlockLegacy::getCollisionShapeForCamera
// to return a solid unit AABB while enabled. No .rdata vtable scan (that froze).
class Jesus : public Module {
public:
    Jesus() : Module("Jesus", "Lets you walk on water", Category::Movement, 0) {
        AddSetting(new BoolSetting("Allow Shift", "Hold shift to sink down", true)); // [0]
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;

    static bool ShouldSolid();

private:
    bool GetAllowShift() const { return static_cast<BoolSetting*>(mSettings[0])->mValue; }

    static bool s_solid;
    bool mHooked = false;

    bool installHook();
    void removeHook();
};
