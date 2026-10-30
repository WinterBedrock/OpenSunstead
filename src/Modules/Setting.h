#pragma once
#include <string>
#include <vector>

enum class SettingType {
    Bool,
    Number,
    Enum,
    Color,
    String
};

class Setting {
public:
    std::string mName;
    std::string mDescription;
    SettingType mType;

    // Animation states for UI
    float sliderEase = 0.0f;
    float boolScale = 0.0f;
    float enumSlide = 0.0f;
    float enumSelAnim = 0.0f;  // animated selected-option index (sliding highlight)
    float knobPulse = 0.0f;    // slider knob grab/change pulse (0..1)
    bool isDragging = false;
    bool enumExtended = false;
    bool mVisible = true;

    Setting(std::string name, std::string desc, SettingType type)
        : mName(std::move(name)), mDescription(std::move(desc)), mType(type) {}

    virtual ~Setting() = default;
    // Restore the construction default (the flyout's "Reset settings" pill).
    virtual void ResetToDefault() {}
};

class BoolSetting : public Setting {
public:
    bool mValue;
    bool mDefault;
    BoolSetting(std::string name, std::string desc, bool val)
        : Setting(std::move(name), std::move(desc), SettingType::Bool), mValue(val), mDefault(val) {}
    void ResetToDefault() override { mValue = mDefault; }
};

class NumberSetting : public Setting {
public:
    float mValue, mMin, mMax, mStep;
    float mDefault;
    NumberSetting(std::string name, std::string desc, float val, float min, float max, float step)
        : Setting(std::move(name), std::move(desc), SettingType::Number), mValue(val), mMin(min), mMax(max), mStep(step), mDefault(val) {}
    void ResetToDefault() override { mValue = mDefault; }
};

class EnumSetting : public Setting {
public:
    int mValue;
    int mDefault;
    std::vector<std::string> mValues;
    EnumSetting(std::string name, std::string desc, int val, std::vector<std::string> values)
        : Setting(std::move(name), std::move(desc), SettingType::Enum), mValue(val), mDefault(val), mValues(std::move(values)) {}
    void ResetToDefault() override { mValue = mDefault; }
};

class ColorSetting : public Setting {
public:
    float mValue[4];
    float mDefault[4];
    ColorSetting(std::string name, std::string desc, float r, float g, float b, float a)
        : Setting(std::move(name), std::move(desc), SettingType::Color) {
        mValue[0] = r; mValue[1] = g; mValue[2] = b; mValue[3] = a;
        mDefault[0] = r; mDefault[1] = g; mDefault[2] = b; mDefault[3] = a;
    }
    void ResetToDefault() override {
        for (int i = 0; i < 4; ++i) mValue[i] = mDefault[i];
    }
};
class StringSetting : public Setting {
public:
    std::string mValue;
    std::string mDefault;
    StringSetting(std::string name, std::string desc, std::string val)
        : Setting(std::move(name), std::move(desc), SettingType::String), mValue(std::move(val)), mDefault(mValue) {}
    void ResetToDefault() override { mValue = mDefault; }
};
