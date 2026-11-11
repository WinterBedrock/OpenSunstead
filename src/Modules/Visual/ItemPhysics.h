#pragma once
#include "../Module.h"

class ItemPhysics : public Module {
public:
    ItemPhysics()
        : Module("ItemPhysics", "Makes dropped items lie flat on the ground",
                 Category::Visual, 0) {
        AddSetting(new BoolSetting("No Spin",
            "Freeze the idle rotation of dropped items", true));
        AddSetting(new BoolSetting("Lay Flat",
            "Tip items onto their side so they lie on the ground", true));
        canBind = true;
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnUpdate() override;
    void UpdateSettingsVisibility() override;

    static void Apply();

    static bool  LayFlatEnabled();
    static uintptr_t SpinCallReturnAddr();
};
