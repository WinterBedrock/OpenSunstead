#pragma once
#include <Windows.h>
#include <utility>
#include "../Module.h"

// ---------------------------------------------------------------------------
// ClickGUI — one centred glass panel instead of five draggable columns.
//
//   ┌──────────────┬───────────────────────────────────────┐
//   │  logo        │  Combat            [search]           │
//   │  ─ rail ─    │  ─────────────────────────────────    │
//   │  Combat      │  ┌ module card ─────────── [toggle] ┐ │
//   │  Movement    │  │   settings (animated reveal)     │ │
//   │  …           │  └─────────────────────────────────┘ │
//   │  Settings    │                                       │
//   └──────────────┴───────────────────────────────────────┘
//
// The panel is laid out at a fixed size and the open/close animation is pure
// alpha + a vertical slide. Nothing scales geometrically on purpose: the old
// build scaled font sizes toward zero while closing, which trips ImGui's
// `font_size > 0` assert in imgui_draw.cpp.
//
// This module also OWNS the client's appearance settings (Theme, Font, custom
// palette, §x code colour). They used to live on a separate settings-only
// "Interface" module that appeared as a card with no real toggle; they are read
// by name here instead — see ColorUtils::getThemedColor and RenderManager. The
// Settings tab is where they are edited.
//
// Panel chrome is recorded on the BACKGROUND draw list so ImGui widgets used
// for text entry (search, string editor) render on top of it; modals and
// tooltips go on the FOREGROUND list so they sit above everything.
// ---------------------------------------------------------------------------
class ClickGUI : public Module {
public:
    // Shape persisted by ConfigManager. Only the first entry is meaningful now —
    // it carries the panel's screen position. The old build stored one entry per
    // draggable category column, hence the name.
    struct CategoryPos {
        float x = 0.0f;
        float y = 0.0f;
        bool isExtended = true;
    };

    static std::vector<CategoryPos> GetCategoryPositions();
    static void SetCategoryPositions(const std::vector<CategoryPos>& positions);
    // 0 = modules, 1 = Settings, 2 = Configs, 3 = Themes, 4 = Changelog. Used by the
    // start-screen quick buttons.
    static void SetTab(int tab);
    // True while a keybind row is waiting for a key press. The WndProc must not
    // act on the menu's own bind key (or Escape) during that window, or rebinding
    // the menu to its current key would slam the panel shut and Escape could never
    // cancel a rebind.
    static bool IsBinding();
    // True while a StringSetting modal (profile name, ...) has an ImGui InputText
    // focused — the WndProc force-blocks keys then even with "Block Keys" OFF, so
    // typing a name cannot also walk the player.
    static bool IsStringEditing();

    // Live search — there is NO input field. While the panel is open on the
    // modules tab the WndProc feeds WM_KEYDOWN straight into the module filter
    // (translated via ToUnicode, so it works regardless of WM_CHAR delivery),
    // and the query text is drawn with a per-character appear animation.
    static bool WantsSearchKeys();                  // capture allowed right now
    static void FeedSearchKeyDown(unsigned int vk); // WM_KEYDOWN virtual key
    static void FeedSearchChar(unsigned int ch);    // raw codepoint (paste path)
    static bool EscapeClearsSearch();               // query non-empty -> clear, true

    ClickGUI();

    void OnEnable() override;
    void OnDisable() override;
    void OnRender() override;
    void OnTick() override;
    void DrawColumns();   // BETA Columns layout renderer
    void UpdateSettingsVisibility() override;

private:
    template <typename T, typename... Args>
    T* add(Args&&... args) {
        T* setting = new T(std::forward<Args>(args)...);
        AddSetting(setting);
        return setting;
    }

    // Setting names are load-bearing: ConfigManager persists by name. The
    // appearance block deliberately reuses the names the Interface module used,
    // so existing configs keep their theme after the merge.
    EnumSetting*   mTheme      = nullptr;
    EnumSetting*   mFont       = nullptr;
    BoolSetting*   mBoldFont   = nullptr;   // bold weight for the picked font
    EnumSetting*   mGradient   = nullptr;
    NumberSetting* mColorCount = nullptr;
    ColorSetting*  mColors[6]  = {};
    NumberSetting* mColorSpeed = nullptr;
    NumberSetting* mSaturation = nullptr;
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
