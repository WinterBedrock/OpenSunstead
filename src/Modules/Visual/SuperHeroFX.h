#pragma once
#include "../Module.h"
#include <imgui.h>
#include <string>
#include <vector>

// :DD
// its cool ig
class SuperHeroFX : public Module {
public:
    SuperHeroFX() : Module("SuperHeroFX", "Comic-style hit / proximity FX in world space",
                            Category::Visual, 0) {
        AddSetting(new BoolSetting("On Damage", "Spawn FX when an actor takes damage", true));                          // [0]
        AddSetting(new BoolSetting("On Proximity", "Occasionally spawn FX while an actor is near", false));              // [1]
        AddSetting(new BoolSetting("Random Idle", "Random ticks spawn FX around the local player", false));              // [2]
        AddSetting(new NumberSetting("Radius", "Trigger radius (blocks)", 24.0f, 2.0f, 64.0f, 1.0f));                     // [3]
        AddSetting(new NumberSetting("Duration", "Effect lifetime (seconds)", 1.4f, 0.4f, 4.0f, 0.1f));                  // [4]
        AddSetting(new NumberSetting("Rise Speed", "World-space units the text rises over the lifetime", 1.2f, 0.0f, 4.0f, 0.1f)); // [5]
        AddSetting(new NumberSetting("Font Size", "Rendered font size (px) at full opacity", 40.0f, 16.0f, 96.0f, 2.0f)); // [6]
        AddSetting(new BoolSetting("Rainbow Color", "Cycle the palette over time instead of random colors", false));     // [7]
        AddSetting(new NumberSetting("Idle Interval", "Mean seconds between random idle bursts", 4.0f, 0.5f, 30.0f, 0.5f)); // [8]
    }

    void OnRender() override;
    void OnDisable() override { mFX.clear(); mActorSeen.clear(); mNextIdleMs = 0; }

    struct Effect {
        float wx, wy, wz;
        float age;
        float life;
        float riseSpeed;
        float swayPhase;
        int   word;
        ImColor color;
    };
    struct ActorSeen {
        void* actor;
        unsigned long long lastMs;
        int lastHurt;
    };
    std::vector<Effect> mFX;
    std::vector<ActorSeen> mActorSeen;
    unsigned long long mNextIdleMs = 0;
    unsigned long long mLastProximityMs = 0;
};
