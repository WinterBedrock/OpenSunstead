#pragma once
#include "../Module.h"

#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

// ---------------------------------------------------------------------------
// ArrayList — the list of enabled modules, Rise ModernInterface style.
//
// Three stages, so geometry is decided once per frame and the draw pass never
// recomputes anything:
//
//   collect()  visible modules -> measured, cased, animated Entry records
//   layout()   Entries         -> screen-space Slots (enter/exit motion applied)
//   paint()    one Slot        -> pixels
//
// Rows animate their vertical position as well as their entry, so toggling a
// module in the middle of the list makes the ones below it glide instead of
// teleporting. That target Y lives on the module (Module::arrayListY) because
// Entry/Slot are rebuilt from scratch every frame.
//
// The list is ALWAYS sorted by row width, mirroring the anchor: top corners
// stack the widest row first, bottom corners the narrowest first (so the
// longest row hugs the bottom edge the way it hugs the top edge at the top).
//
// Draw-order note: geometry goes to the FOREGROUND draw list while the backdrop
// blur goes to the BACKGROUND list. A D2D blur is a callback that paints an
// opaque blurred rect onto the backbuffer at replay time, so on one list it
// would erase geometry recorded before it; splitting the two means the blur can
// only ever land underneath.
//
// Setting *names* are load-bearing: ConfigManager persists by name, so renaming
// one silently resets it for every existing config.
// ---------------------------------------------------------------------------
class ArrayList : public Module {
public:
    ArrayList();

    void OnRender() override;
    void UpdateSettingsVisibility() override;

private:
    enum class Corner { TopLeft, TopRight, BottomLeft, BottomRight };
    enum class Casing { Keep, Lower, Upper };
    enum class Anim   { Slide, Fade, Scale, Swipe };

    // One visible module, measured and animated.
    struct Entry {
        Module*     mod = nullptr;
        std::string name;
        std::string mode;
        float       nameWidth = 0.f;
        float       modeWidth = 0.f;
        float       width = 0.f;   // full row width including padding
        float       anim = 0.f;    // 0..1 raw enable amount
    };

    // Screen-space box of a laid-out row.
    struct Slot {
        float left = 0.f, top = 0.f, right = 0.f, bottom = 0.f;
        float anim = 0.f;   // raw 0..1
        float ease = 0.f;   // eased 0..1 — what alphas and offsets use
    };

    // Everything derived once per frame and shared by layout and paint.
    struct Frame {
        float fontSize = 0.f;
        float padX = 0.f;        // Card Width — horizontal padding around the text
        float padY = 0.f;        // Card Height — vertical padding above/below the text
        float rowHeight = 0.f;
        float gap = 0.f;
        float glow = 0.f;        // bar-only glow strength (0 = off)
        float barW = 4.f, barHeight = 80.f, barR = 2.f, barPad = 8.f;
        Corner corner = Corner::TopRight;
        Anim   anim = Anim::Slide;
        bool   right = false;
        bool   bottom = false;
    };

    template <typename T, typename... Args>
    T* add(Args&&... args) {
        T* setting = new T(std::forward<Args>(args)...);
        AddSetting(setting);
        return setting;
    }

    // --- Setting handles (assigned in the ctor via add<>) --------------------
    EnumSetting*   mCorner        = nullptr;
    EnumSetting*   mCasing        = nullptr;

    EnumSetting*   mColorMode     = nullptr;
    ColorSetting*  mCustomColor   = nullptr;
    ColorSetting*  mCustomColor2  = nullptr;
    NumberSetting* mGradientSpan  = nullptr;

    BoolSetting*   mTextShadow    = nullptr;
    BoolSetting*   mShowMode      = nullptr;
    BoolSetting*   mModeDash      = nullptr;
    BoolSetting*   mModeThemed    = nullptr;
    ColorSetting*  mModeColor     = nullptr;

    BoolSetting*   mBackground    = nullptr;
    NumberSetting* mBgOpacity     = nullptr;
    NumberSetting* mBlur          = nullptr;
    BoolSetting*   mShadow        = nullptr;
    NumberSetting* mShadowOpacity = nullptr;
    // Glow lives on the sidebar bar only. A glow around the whole row sat on
    // top of the Shadow plate and read as a muddy halo, so the row-level bloom
    // is gone and this toggle now controls just the bar's own light.
    BoolSetting*   mGlow          = nullptr;
    NumberSetting* mGlowStrength  = nullptr;
    // The sidebar bar is one simple accent bar; the old Bar Style "themes"
    // (None / Bar / Pill / Line / Gradient) were removed entirely. What is
    // left are plain appearance knobs: thickness, height, corner rounding and
    // the padding between the text and the bar.
    NumberSetting* mBarWidth   = nullptr;
    NumberSetting* mBarHeight  = nullptr;
    NumberSetting* mBarRadius  = nullptr;
    NumberSetting* mBarPadding = nullptr;

    NumberSetting* mFontSize      = nullptr;
    // Width/height of the card AROUND the module text. The old "H Padding" /
    // "V Spacing" sliders were dead: buildFrame() overwrote both with the fixed
    // Rise geometry, so dragging them changed nothing. These are the same two
    // knobs, but they are the values buildFrame actually reads.
    NumberSetting* mCardWidth     = nullptr;
    NumberSetting* mCardHeight    = nullptr;
    NumberSetting* mOffsetX       = nullptr;
    NumberSetting* mOffsetY       = nullptr;

    EnumSetting*   mAnimMode      = nullptr;
    NumberSetting* mAnimSpeed     = nullptr;
    BoolSetting*   mMouseToggle   = nullptr;

    Corner corner()   const { return static_cast<Corner>(mCorner->mValue); }
    Casing casing()   const { return static_cast<Casing>(mCasing->mValue); }
    Anim   animMode() const { return static_cast<Anim>(mAnimMode->mValue); }

    Frame   buildFrame() const;
    ImColor colorAt(float index) const;
    std::string applyCasing(std::string text) const;

    std::vector<Entry> collect(const Frame& f, float dt) const;
    std::vector<Slot>  layout(const Frame& f, const std::vector<Entry>& entries,
                              float originX, float originY, float dt, ImVec4* boundsOut) const;

    void paintRowShadow(const Frame& f, const Slot& slot, ImDrawList* dl) const;
    void paintSurface(const Frame& f, const Slot& slot, float hover,
                      ImDrawList* dl) const;
    void paintText(const Frame& f, const Entry& entry, const Slot& slot, const ImColor& accent,
                   float gradientBase, ImDrawList* dl) const;
};
