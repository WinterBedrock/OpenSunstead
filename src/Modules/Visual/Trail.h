#pragma once
#include "../Module.h"
#include <imgui.h>
#include <vector>

class Trail : public Module {
public:
    Trail() : Module("Trail", "Show a fading path of where an actor walked", Category::Visual, 0) {
        AddSetting(new EnumSetting("Type", "Type of trail to display", 0,
                                   { "Line", "Circle" }));                                                                             // [0]
        AddSetting(new NumberSetting("Max Points", "Buffer points per actor", 200.0f, 20.0f, 2000.0f, 20.0f));                        // [1]
        AddSetting(new NumberSetting("Fade Time", "Lifetime of one point (seconds)", 6.0f, 1.0f, 60.0f, 1.0f));                      // [2]
        AddSetting(new NumberSetting("Delay", "Sampling delay between points (ms)", 60.0f, 5.0f, 1000.0f, 5.0f));                   // [3]
        AddSetting(new NumberSetting("Size", "Circle radius (pixels) / Line width", 20.0f, 1.0f, 100.0f, 1.0f));                 // [4]
        AddSetting(new BoolSetting("RGB", "Rainbow color cycle", true));                                                              // [5]
        AddSetting(new BoolSetting("Others", "Also follow other players", false));                                                    // [6]
        AddSetting(new BoolSetting("Self", "Follow the local player", true));        //[7]
        AddSetting(new NumberSetting("Scale In", "Circle pop-in time", 0.80f, 0.0f, 1.0f, 0.05f)); // [8]
        AddSetting(new NumberSetting("Circle Gap", "Delay between new circles in Circle mode (ms)", 450.0f, 5.0f, 1000.0f, 5.0f)); // [9]
        AddSetting(new BoolSetting("Always show", "Render on non-HUD screens too", false));                                         // [10]
        AddSetting(new BoolSetting("Hide in 1st Person", "Render only from 2nd/3rd person cameras", false));                         // [11]
    }
    void UpdateSettingsVisibility() override {
        if (mSettings.size() > 9 && mSettings[9])
            mSettings[9]->mVisible = (static_cast<EnumSetting*>(mSettings[0])->mValue == 1);
    }
    void OnTick() override;
    void OnRender() override;
    void OnDisable() override { mPoints.clear(); }
    void OnPacketReceive(class SDK::Packet* packet, int packetId, bool& cancel) override;

    struct Point {
        void* entity;
        unsigned long long timeMs;
        float x, y, z;
    };
    struct ActorPoints {
        void* entity;
        std::vector<Point> points;
    };
    std::vector<ActorPoints> mPoints;   // one entry per actor
};
