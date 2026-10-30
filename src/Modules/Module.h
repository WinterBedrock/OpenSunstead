#pragma once
#include <string>
#include <vector>
#include "Setting.h"

namespace SDK { class Packet; }

enum class Category {
    Visual,
    Combat,
    Movement,
    Player,
    Misc
};

class Module {
public:
    Module(std::string name, std::string description, Category category, int keybind = 0)
        : name(std::move(name)), description(std::move(description)), category(category), keybind(keybind), enabled(false) {}

    virtual ~Module() = default;

    virtual void OnEnable() {}
    virtual void OnDisable() {}
    virtual void OnUpdate() {}
    // Called every frame by the ClickGUI for the module whose settings are being
    // drawn. Override to toggle Setting::mVisible without side effects.
    virtual void UpdateSettingsVisibility() {}
    virtual void OnTick() {}
    virtual void OnRender() {}
    virtual void OnKey(int key) {}
    virtual void OnPacketReceive(class SDK::Packet* packet, int packetId, bool& cancel) {}
    // Outgoing packets (LoopbackPacketSender::send). Modules own their spoof logic here.
    virtual void OnPacketSend(class SDK::Packet* packet, int packetId, bool& cancel) {}

    const std::string& GetName() const { return name; }
    const std::string& GetDescription() const { return description; }
    Category GetCategory() const { return category; }

    // Restore every setting to its construction default (the "Reset settings"
    // pill at the bottom of the module's flyout). Depends visibility is
    // refreshed right after, so conditional rows show/hide correctly.
    void ResetSettings() {
        for (Setting* s : mSettings)
            if (s) s->ResetToDefault();
        UpdateSettingsVisibility();
    }
    
    bool IsEnabled() const { return enabled; }
    void SetEnabled(bool enabled) {
        if (this->enabled != enabled) {
            this->enabled = enabled;
            if (enabled) OnEnable();
            else OnDisable();
        }
    }
    void Toggle() { SetEnabled(!enabled); }

    int GetKeybind() const { return keybind; }
    void SetKeybind(int key) { keybind = key; }

    // Universal per-module toggle controlling whether the module shows up in the
    // ArrayList while enabled. Injected ONCE for every module by ModuleManager
    // (deliberately NOT declared in each module's constructor) so it always sits
    // LAST in mSettings and never shifts existing index-based setting access.
    //
    // Both lookups check the TYPE as well as the name. A module that declares its
    // own setting called "Visible" of a different type would otherwise be found
    // first and static_cast to BoolSetting, so the ArrayList reads whatever the
    // first bytes of that object are as a bool — and the real toggle never gets
    // injected.
    void EnsureVisibleSetting() {
        for (auto* s : mSettings)
            if (s && s->mName == "Visible" && s->mType == SettingType::Bool) return;
        mSettings.push_back(new BoolSetting("Visible", "Show this module in the ArrayList while enabled", true));
    }
    bool IsVisibleInArrayList() const {
        for (auto* s : mSettings)
            if (s && s->mName == "Visible" && s->mType == SettingType::Bool)
                return static_cast<BoolSetting*>(s)->mValue;
        return true;
    }

    // UI Animation State
    float cAnim = 0.0f;
    // Columns GUI: 0..1 how far the module's name has slid from centre to the
    // left edge while its inline settings body is open.
    float colNameSlide = 0.0f;
    float enableAnim = 0.0f; // 0..1 cross-fade for enable/disable toggle
    float hoverAnim = 0.0f;  // 0..1 ClickGUI row hover highlight (fades out)
    float arrayListAnim = 1.0f; // ArrayList fade animation (1.0 = fully visible)
    // Animated ArrayList row top, in screen pixels. The list eases toward the
    // freshly computed stack position each frame so a module toggled in the
    // middle makes the rows below it glide rather than jump. Negative means
    // "not placed yet" — the first frame a row appears it snaps to its target
    // instead of sliding in from the top of the screen.
    float arrayListY = -1.0f;
    // ClickGUI search filter: 1 = matches / no filter, 0 = hidden. Animated so
    // non-matching modules collapse instead of popping out instantly.
    float searchAnim = 1.0f;
    bool showSettings = false;
    // ClickGUI pin: pinned modules float to the top of their list (both the
    // "All" overview and their own category) regardless of the sort/filter.
    // Toggled from the pin glyph that appears on the card header on hover.
    bool guiPinned = false;
    // Eased 0..1 mirror of guiPinned, so the glyph can pop/settle instead of
    // switching colour on one frame.
    float guiPinAnim = 0.0f;
    // Eased 0..1 "the pin is visible" amount: driven by card hover, latched to 1
    // while pinned. Drives the slide-in as well as the alpha.
    float guiPinShow = 0.0f;
    
    // HUD positions and dimensions
    float mPosX = 10.0f;
    float mPosY = 10.0f;
    float mWidth = 100.0f;
    float mHeight = 30.0f;
    // Construction-time position, snapshotted by ModuleManager::Initialize
    // (before the config overwrites mPosX/mPosY). This is what the HUD
    // editor "Reset Positions" restores — the old code preset every module
    // to Scaffold's (24,300) sentinel, so only Scaffold ever reset and the
    // button effectively just forced mScale = 1.0 on everything else.
    float mDefaultPosX = 10.0f;
    float mDefaultPosY = 10.0f;
    void SnapshotDefaultPos() { mDefaultPosX = mPosX; mDefaultPosY = mPosY; }
    // Restore the construction-time position, recompute screen-dependent
    // defaults (Scaffold/Player OnFirstLayout gates on the virgin pos) and
    // reset the scale.
    virtual void ResetPosition(float screenW, float screenH) {
        mPosX = mDefaultPosX; mPosY = mDefaultPosY;
        OnFirstLayout(screenW, screenH);
        mScale = 1.0f;
    }

    // Settings
    std::vector<Setting*> mSettings;
    virtual void OnFirstLayout(float screenW, float screenH) { (void)screenW; (void)screenH; }
    bool canBind = true;
    bool mHoldMode = false; // If true, module is active only while key is held
    // ---- Hold-bind runtime state (driven by ModuleManager::PollHoldBinds) ----
    // A hold bind no longer requires the module to be toggled on: while the
    // key is HELD the manager force-enables the module (and force-disables it
    // on release), so it reacts to the hold exactly like a toggle bind reacts
    // to its press, and it shows up in the ArrayList for precisely as long as
    // the key is down. The module's own logic keeps deciding what "active"
    // means on top of these flags.
    bool mHoldKeyDown = false; // live key state, refreshed by the poll
    bool mHoldForced = false;  // module is currently enabled BY the hold poll
    bool mHoldEngaged = false; // this key-down span is managed by the poll
    bool IsHoldActive() const { return mHoldMode && mHoldKeyDown; }
    // If false, the module keybind does NOT toggle enable/disable (the module
    // still uses GetKeybind() for its own action, e.g. PearlTrigger throw).
    // Prevents the freezy toggle+notification+sound on every action press.
    bool mKeybindToggles = true;
    float mScale = 1.0f;
    // If true, this module renders a moveable/scaleable HUD element and is
    // therefore managed by the HUD editor (draggable + scroll-to-scale).
    // Defaults to false; HUD-type modules opt in via their constructor.
    bool mIsHudModule = false;
    // Set to true by a module during its own OnRender when it handled this
    // frame's left click itself (a button, slider or other control was hit).
    // Cleared at the top of every render pass by ModuleManager::OnRender;
    // HudEditor reads it after the module loop so pressing e.g. the Player's
    // "next" button no longer also grabs the whole panel for a drag.
    bool mClickConsumed = false;

protected:
    void AddSetting(Setting* setting) {
        mSettings.push_back(setting);
    }

private:
    std::string name;
    std::string description;
    Category category;
    bool enabled;
    int keybind;
};