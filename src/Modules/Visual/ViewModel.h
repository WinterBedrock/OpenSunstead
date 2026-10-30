#pragma once
#include "../Module.h"
#include <atomic>

class ViewModel : public Module {
public:
    ViewModel() : Module("ViewModel", "Move, rotate and scale the first-person hand", Category::Visual, 0) {
        AddSetting(new NumberSetting("Pos X", "Moves the hand left/right", 0.0f, -4.0f, 4.0f, 0.05f));
        AddSetting(new NumberSetting("Pos Y", "Moves the hand up/down", 0.0f, -4.0f, 4.0f, 0.05f));
        AddSetting(new NumberSetting("Pos Z", "Moves the hand closer/further", 0.0f, -4.0f, 4.0f, 0.05f));
        AddSetting(new NumberSetting("Rot X", "Rotate around X (pitch), degrees", 0.0f, -180.0f, 180.0f, 1.0f));
        AddSetting(new NumberSetting("Rot Y", "Rotate around Y (yaw), degrees", 0.0f, -180.0f, 180.0f, 1.0f));
        AddSetting(new NumberSetting("Rot Z", "Rotate around Z (roll), degrees", 0.0f, -180.0f, 180.0f, 1.0f));
        AddSetting(new NumberSetting("Scale X", "Hand scale on X", 1.0f, 0.1f, 3.0f, 0.05f));
        AddSetting(new NumberSetting("Scale Y", "Hand scale on Y", 1.0f, 0.1f, 3.0f, 0.05f));
        AddSetting(new NumberSetting("Scale Z", "Hand scale on Z", 1.0f, 0.1f, 3.0f, 0.05f));
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override { RefreshParams(); }
    
    struct Params {
        float px = 0, py = 0, pz = 0;
        float rx = 0, ry = 0, rz = 0;
        float sx = 1, sy = 1, sz = 1;
    };
    static const Params& GetParams() { return g_params; }
    static bool IsActive() { return g_active; }

private:
    void RefreshParams();

    static std::atomic<bool> g_active;
    static Params g_params;          // plain struct, written on tick thread only
};
