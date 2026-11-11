#pragma once
#include <string>
#include <vector>
#include <utility>
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

    virtual void UpdateSettingsVisibility() {}
    virtual void OnTick() {}
    virtual void OnRender() {}
    virtual void OnKey(int key) {}
    virtual void OnPacketReceive(class SDK::Packet* packet, int packetId, bool& cancel) {}

    virtual void OnPacketSend(class SDK::Packet* packet, int packetId, bool& cancel) {}

    const std::string& GetName() const { return name; }
    const std::string& GetDescription() const { return description; }
    Category GetCategory() const { return category; }

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

    void EnsureVisibleSetting() {
        for (auto* s : mSettings)
            if (s && s->mName == "Visible" && s->mType == SettingType::Bool) return;
        const bool visibleByDefault = !(name == "ArrayList" || name == "ToggleSounds");
        mSettings.push_back(new BoolSetting("Visible", "Show this module in the ArrayList while enabled", visibleByDefault));
    }
    bool IsVisibleInArrayList() const {
        for (auto* s : mSettings)
            if (s && s->mName == "Visible" && s->mType == SettingType::Bool)
                return static_cast<BoolSetting*>(s)->mValue;
        return true;
    }


    float cAnim = 0.0f;

    float colNameSlide = 0.0f;
    float enableAnim = 0.0f;
    float hoverAnim = 0.0f;
    float arrayListAnim = 1.0f;
    float arrayListY = -1.0f;

    float searchAnim = 1.0f;
    bool showSettings = false;
    bool guiPinned = false;

    float guiPinAnim = 0.0f;

    float guiPinShow = 0.0f;
    

    float mPosX = 10.0f;
    float mPosY = 10.0f;
    float mWidth = 100.0f;
    float mHeight = 30.0f;
    float mDefaultPosX = 10.0f;
    float mDefaultPosY = 10.0f;
    void SnapshotDefaultPos() { mDefaultPosX = mPosX; mDefaultPosY = mPosY; }
    virtual void ResetPosition(float screenW, float screenH) {
        mPosX = mDefaultPosX; mPosY = mDefaultPosY;
        OnFirstLayout(screenW, screenH);
        mScale = 1.0f;
    }


    std::vector<Setting*> mSettings;
    virtual void OnFirstLayout(float screenW, float screenH) { (void)screenW; (void)screenH; }
    bool canBind = true;
    bool mHoldMode = false;
    bool mHoldKeyDown = false;
    bool mHoldForced = false;
    bool mHoldEngaged = false;
    bool IsHoldActive() const { return mHoldMode && mHoldKeyDown; }
    bool mKeybindToggles = true;
    float mScale = 1.0f;
    bool mIsHudModule = false;
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