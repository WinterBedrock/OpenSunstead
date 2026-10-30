#pragma once
#include "../Module.h"
#include "../../SDK/Types.h"
#include <imgui.h>
#include <map>

class Aura : public Module {
public:
    Aura() : Module("Aura", "Attacks all players within range", Category::Combat, 0) {
        AddSetting(new NumberSetting("Range", "Attack radius in blocks", 4.0f, 2.0f, 8.0f, 0.1f));
        AddSetting(new NumberSetting("APS", "Attacks per second (hits/sec)", 10.0f, 1.0f, 20.0f, 1.0f));
        AddSetting(new BoolSetting("Debug", "Print attack-pipeline diagnostics to the console", false));
        AddSetting(new EnumSetting("Rotate", "Rotation behaviour", 1, { "None", "Normal", "Flick" }));
        AddSetting(new NumberSetting("Smooth", "Rotation smoothing", 1.0f, 0.05f, 1.0f, 0.05f)); // [4]
        AddSetting(new EnumSetting("Switch", "Auto weapon switch", 0, { "None", "Full", "OnAttack" }));             // [5]
        AddSetting(new EnumSetting("Weapon", "Which weapons to auto-switch to", 0, { "Swords", "Swords+Axes" }));   // [6]
        AddSetting(new EnumSetting("Target Visual", "Visual on the current target", 0,
                                   { "None", "Corner", "Rings" })); // [7]
        AddSetting(new BoolSetting("Raycast", "Only attack targets your view ray reaches (attack the obstructing player instead)", false)); // [8]
        // Randomize APS: re-draw the attack interval from [Min..Max] on every
        // hit — a perfectly constant CPS is itself a heuristic (Prax/Solstice).
        AddSetting(new BoolSetting("Randomize APS", "Randomize the attack interval per hit", true));               // [9]
        AddSetting(new NumberSetting("APS Min", "Minimum randomized attacks per second", 8.0f, 1.0f, 20.0f, 0.5f)); // [10]
        AddSetting(new NumberSetting("APS Max", "Maximum randomized attacks per second", 12.0f, 1.0f, 20.0f, 0.5f)); // [11]
        // Hurt time: vanilla invulnerability frames last 10 ticks (500 ms) —
        // hitting the same target faster than any player could is detectable
        // and wastes the swings (Prax "Hurt Time").
        AddSetting(new BoolSetting("Hurt Time", "Respect the target's invulnerability frames", false));            // [12]
        AddSetting(new NumberSetting("Hurt Time Ms", "Minimum milliseconds between two hits on the same target", 500.0f, 0.0f, 1000.0f, 25.0f)); // [13]
        // Interact: send the Interact(left-click) packet right before the
        // attack transaction, like a real client's input pipeline does
        // (Prax/Solstice "Interact").
        AddSetting(new BoolSetting("Interact", "Send an Interact packet before each attack", false));                // [14]
    }

    void OnUpdate() override;   // runs on GAME tick (see ModuleManager::OnUpdate)
    void OnRender() override;   // target visual (Target Visual setting)
    void OnDisable() override;  // clears the silent-rotation channel
private:
    // Shared zoom-out exit replay (target lost / module toggled off).
    void renderExitZoom();
public:

    float GetRange()   const { return static_cast<NumberSetting*>(mSettings[0])->mValue; }
    // Attacks-per-second slider drives the internal burst throttle. Delay is
    // derived: delayMs = 1000 / APS (0 APS would divide-by-zero, so guard it).
    float GetAPS()     const { return static_cast<NumberSetting*>(mSettings[1])->mValue; }
    float GetDelayMs() const { float aps = GetAPS(); return aps > 0.0f ? (1000.0f / aps) : 0.0f; }
    bool  GetDebug()   const { return static_cast<BoolSetting*>(mSettings[2])->mValue; }
    int   GetRotateMode() const { return static_cast<EnumSetting*>(mSettings[3])->mValue; } // 0=None 1=Normal 2=Flick
    float GetSmooth()     const { return static_cast<NumberSetting*>(mSettings[4])->mValue; }
    int   GetSwitchMode() const { return static_cast<EnumSetting*>(mSettings[5])->mValue; } // 0=None 1=Full 2=OnAttack
    int   GetWeaponMode() const { return static_cast<EnumSetting*>(mSettings[6])->mValue; } // 0=Swords 1=Swords+Axes
    int   GetTargetVisual() const { return static_cast<EnumSetting*>(mSettings[7])->mValue; } // 0=None 1=Corner 2=Rings
    // ---- Bypass settings (indices appended after the visual) --------------
    bool  GetRaycast()     const { return mSettings.size() > 8  && static_cast<BoolSetting*>(mSettings[8])->mValue; }
    bool  GetRandomizeAPS() const { return mSettings.size() > 9  && static_cast<BoolSetting*>(mSettings[9])->mValue; }
    float GetAPSMin()      const { return mSettings.size() > 10 ? static_cast<NumberSetting*>(mSettings[10])->mValue : 8.0f; }
    float GetAPSMax()      const { return mSettings.size() > 11 ? static_cast<NumberSetting*>(mSettings[11])->mValue : 12.0f; }
    bool  GetHurtTime()    const { return mSettings.size() > 12 && static_cast<BoolSetting*>(mSettings[12])->mValue; }
    float GetHurtTimeMs()  const { return mSettings.size() > 13 ? static_cast<NumberSetting*>(mSettings[13])->mValue : 500.0f; }
    bool  GetInteract()    const { return mSettings.size() > 14 && static_cast<BoolSetting*>(mSettings[14])->mValue; }

private:
    unsigned long long mLastBurst = 0;
    unsigned long long mLastDebug = 0;
    // Randomized-APS interval for the CURRENT hit (re-drawn after every swing).
    unsigned long long mNextAttackAfterMs = 0;
    // Last game tick (Actor::ticksExisted) on which we swung+attacked. OnUpdate
    // runs on the camera/render update (per frame), so this hard-gates us to at
    // most ONE attack per real server tick regardless of framerate — preventing
    // multiple ItemUseOnEntity transactions landing in one PlayerAuthInput flush
    // (the "Failed to decode batch packet / malformed packet" kick).
    long long mLastAttackTick = -1;

    // Full weapon-switch state: remember the player's original hotbar slot so
    // we can restore it when the target is lost or Aura is disabled.
    bool mDidFullSwitch = false;
    int  mPrevSlot = -1;

    // Hurt-time bookkeeping: last successful attack (ms) per target entity id.
    std::map<unsigned int, unsigned long long> mLastHitPerTarget;

    // ---- Target-visual animation state (Corner / Rings) -------------------
    // mVisAlpha  — eased 0..1: fades IN when a target is acquired, OUT when it
    //              is lost (the visual keeps drawing from its cached geometry
    //              while fading).
    // mVisTarget — the actor the visual currently tracks; on a CHANGE the
    //              morph below blends the visual from the old anchor to the
    //              new one instead of teleporting.
    // mMorphT    — 0..1 switch-morph progress (0.22 s, eased in OnRender).
    float  mVisAlpha = 0.0f;
    void*  mVisTarget = nullptr;
    float  mMorphT = 1.0f;
    int    mVisMode = 0;              // visual mode the cached state belongs to
    // Previous target's anchors for the morph (screen-space for Corner,
    // world-space for Rings).
    ImVec2 mMorphFrom = ImVec2(0, 0);
    float  mMorphFromR = 0.0f;
    Vec3   mMorphFromWp{};
    Vec3   mLastWorldRp{};            // rings: last world anchor (morph source)
    // Cached last-drawn geometry for the fade-out redraw (no fresh projection
    // is available once the target is gone).
    ImVec2 mLastCenter = ImVec2(0, 0);
    float  mLastR = 0.0f;
    float  mLastTh = 0.0f;
    ImVec2 mRingTop[65];
    ImVec2 mRingBot[65];
    bool   mRingVis[65] = {};
    int    mRingSeg = 0;
    // Rings: screen-space centre of the cached ring (the zoom-out exit scales
    // the cached points toward it).
    ImVec2 mRingCenter = ImVec2(0, 0);
};