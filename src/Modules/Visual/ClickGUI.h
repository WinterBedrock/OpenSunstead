#pragma once
#include <Windows.h>
#include <utility>
#include "../Module.h"

class ClickGUI : public Module {
public:
    struct CategoryPos {
        float x = 0.0f;
        float y = 0.0f;
        bool isExtended = true;
    };

    static std::vector<CategoryPos> GetCategoryPositions();
    static void SetCategoryPositions(const std::vector<CategoryPos>& positions);
    static void SetTab(int tab);
    static bool IsBinding();
    static bool IsStringEditing();

    static bool WantsSearchKeys();                  // capture allowed right now
    static void FeedSearchKeyDown(unsigned int vk); // WM_KEYDOWN virtual key
    static void FeedSearchChar(unsigned int ch);    // raw codepoint (paste path)
    static bool EscapeClearsSearch();               // query non-empty -> clear, true

    ClickGUI();

    void OnEnable() override;
    void OnDisable() override;
    void OnRender() override;
    void OnTick() override;
    void DrawColumns();
    void UpdateSettingsVisibility() override;

private:
    template <typename T, typename... Args>
    T* add(Args&&... args) {
        T* setting = new T(std::forward<Args>(args)...);
        AddSetting(setting);
        return setting;
    }

    EnumSetting*   mTheme      = nullptr;
    EnumSetting*   mFont       = nullptr;
    BoolSetting*   mBoldFont   = nullptr;   // bold weight for the picked font
    EnumSetting*   mGradient   = nullptr;
    NumberSetting* mColorCount = nullptr;
    ColorSetting*  mColors[6]  = {};
    NumberSetting* mColorSpeed = nullptr;
    NumberSetting* mSaturation = nullptr;
    NumberSetting* mThemeStrength = nullptr;
    ColorSetting*  mCodeColor  = nullptr;

    EnumSetting*   mAnim       = nullptr;
    NumberSetting* mEaseSpeed  = nullptr;
    NumberSetting* mBlur       = nullptr;
    NumberSetting* mDim        = nullptr;
    NumberSetting* mOpacity    = nullptr;
    NumberSetting* mRounding   = nullptr;
    NumberSetting* mRowHeight  = nullptr;
    NumberSetting* mUiScale    = nullptr;
    NumberSetting* mGlow       = nullptr;
    BoolSetting*   mShadow     = nullptr;
    NumberSetting* mShadowStrength = nullptr;
    BoolSetting*   mTextShadow = nullptr;
    EnumSetting*   mCasing     = nullptr;
    EnumSetting*   mSort       = nullptr;
    BoolSetting*   mTooltips   = nullptr;
    BoolSetting*   mMenuButtons = nullptr;
    BoolSetting*   mBlockKeys  = nullptr;
    EnumSetting*   mHotbarAnim  = nullptr;   // hotbar selection glide easing (HotbarAnimation service)
    NumberSetting* mHotbarSpeed = nullptr;   // hotbar selection glide speed
    EnumSetting*   mLayout    = nullptr;   // 0 = Panel, 1 = Columns (beta)
    EnumSetting*   mColStyle  = nullptr;   // Columns look: 0 = Machined, 1 = Clean
};
