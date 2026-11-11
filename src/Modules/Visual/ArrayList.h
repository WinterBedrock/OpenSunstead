#pragma once
#include "../Module.h"

#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

class ArrayList : public Module {
public:
    ArrayList();

    void OnRender() override;
    void UpdateSettingsVisibility() override;

private:
    enum class Corner { TopLeft, TopRight, BottomLeft, BottomRight };
    enum class Casing { Keep, Lower };
    enum class Anim   { Slide, Fade, Scale, Swipe };

    struct Entry {
        Module*     mod = nullptr;
        std::string name;
        std::string mode;
        float       nameWidth = 0.f;
        float       modeWidth = 0.f;
        float       width = 0.f;
        float       anim = 0.f;
    };

    struct Slot {
        float left = 0.f, top = 0.f, right = 0.f, bottom = 0.f;
        float anim = 0.f;
        float ease = 0.f;
    };

    struct Frame {
        float fontSize = 0.f;
        float padX = 0.f;
        float padY = 0.f;
        float rowHeight = 0.f;
        float gap = 0.f;
        float glow = 0.f;
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
    BoolSetting*   mGlow          = nullptr;
    NumberSetting* mGlowStrength  = nullptr;
    NumberSetting* mBarWidth   = nullptr;
    NumberSetting* mBarHeight  = nullptr;
    NumberSetting* mBarRadius  = nullptr;
    NumberSetting* mBarPadding = nullptr;

    NumberSetting* mFontSize      = nullptr;
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
