#pragma once
#include "../Module.h"
#include <string>
#include <deque>

// Notification kind — drives the tile icon: ToggleOn = check, ToggleOff =
// cross, everything else = the plain themed tile.
enum class NotifType { Info = 0, ToggleOn, ToggleOff };

// Rise NotificationComponent (6.9.5) — faithful port:
//   * a QUEUE (max 5, oldest evicted) with ONE card visible at a time;
//   * entry: center-pivot zoom 0 -> 1, EASE_OUT_EXPO 500ms; exit: zoom
//     1 -> 1.1 + fade;
//   * card: dark rounded plate (radius 10) + a 20x20 rounded-6 white icon
//     tile, BOLD WHITE title + LIGHT WHITE description (Rise's own text
//     colours are white/grey — the accent-tinted title read badly), auto
//     width (min 140);
//   * HUD-editor element: grab it on the pause/chat screens to move it,
//     scroll to scale; an EXAMPLE card renders while there is nothing to
//     show there (TargetHUD-style) so the position is always grabbable.
class Notifications : public Module {
public:
    Notifications() : Module("Notifications", "Displays notifications on screen", Category::Visual, 0) {
        canBind = false;
        mIsHudModule = true;   // HUD-editor: drag on pause/chat, scroll to scale
        // (0,0) = "never positioned" sentinel — OnRender replaces it with the
        // default band anchor (just BELOW the Watermark; Module's own (10,10)
        // default made the card spawn exactly ON the Watermark).
        mPosX = 0.f;
        mPosY = 0.f;
        // Style preset: "Stead" = the classic Rise look driven by every knob
        // below; "Nova" = a self-contained modern preset (fixed bottom-right,
        // slide animations, frosted backdrop, countdown strip). NOVA is the
        // DEFAULT; the Stead knobs are hidden and not applied while Nova is
        // active. Nova's own knobs sit at the end of the list.
        AddSetting(new EnumSetting("Style", "Notification style preset", 1, { "Stead", "Nova" }));
        AddSetting(new NumberSetting("Scale", "Overall scale of the notification card", 1.0f, 0.5f, 2.5f, 0.05f));
        AddSetting(new NumberSetting("Duration", "How long each notification stays visible (seconds)", 3.0f, 0.5f, 30.0f, 0.5f));
        AddSetting(new NumberSetting("Opacity", "Background plate opacity", 0.85f, 0.05f, 1.0f, 0.01f));
        AddSetting(new NumberSetting("Dim", "Backdrop dim strength behind the cards", 0.25f, 0.0f, 1.0f, 0.05f));
        AddSetting(new NumberSetting("Blur", "Backdrop blur strength behind the cards", 3.0f, 0.0f, 20.0f, 0.5f));
        AddSetting(new NumberSetting("Rounding", "Card corner rounding", 10.0f, 0.0f, 24.0f, 1.0f));
        AddSetting(new BoolSetting("Shadow", "Soft shadow behind the cards", false));
        AddSetting(new NumberSetting("Shadow Strength", "How strong the shadow is", 0.5f, 0.05f, 1.0f, 0.05f));
        // Nova-only knobs (appended AFTER the Stead block: settings are read by
        // name in OnRender and persisted by name, so Stead is untouched).
        AddSetting(new NumberSetting("Nova Blur", "Nova: backdrop blur strength behind the cards", 6.0f, 0.0f, 20.0f, 0.5f));
        AddSetting(new NumberSetting("Nova Dim", "Nova: backdrop dim strength behind the cards", 0.35f, 0.0f, 1.0f, 0.05f));
        AddSetting(new BoolSetting("Nova Limit", "Nova: cap how many notifications are on screen at once", false));
        AddSetting(new NumberSetting("Nova Max", "Nova: maximum simultaneous notifications", 3.0f, 1.0f, 10.0f, 1.0f));
        UpdateSettingsVisibility();
    }

    void OnRender() override;
    void UpdateSettingsVisibility() override {
        // Everything is looked up by NAME + TYPE (never by index: the list is
        // appended to over time and an index drift would silently toggle the
        // wrong knob's visibility). Nova owns its whole look, so the Stead
        // knobs hide while it is selected; the Nova knobs are the mirror
        // image, and Nova Max only shows while Nova Limit is on.
        Setting* styleS = nullptr;
        Setting* shadowS = nullptr;
        Setting* limitS = nullptr;
        for (Setting* s : mSettings) {
            if (!s) continue;
            if      (s->mName == "Style"       && s->mType == SettingType::Enum) styleS  = s;
            else if (s->mName == "Shadow"      && s->mType == SettingType::Bool) shadowS = s;
            else if (s->mName == "Nova Limit"  && s->mType == SettingType::Bool) limitS  = s;
        }
        const bool nova     = styleS  && static_cast<EnumSetting*>(styleS)->mValue == 1;
        const bool shadowOn = !shadowS || static_cast<BoolSetting*>(shadowS)->mValue;
        const bool limitOn  = limitS && static_cast<BoolSetting*>(limitS)->mValue;
        for (Setting* s : mSettings) {
            if (!s) continue;
            const std::string& n = s->mName;
            // Stead-only knobs: Opacity, Dim, Blur, Rounding, Shadow,
            // Shadow Strength (plus this module's Scale/Duration/Style stay
            // visible in both — they are shared).
            const bool steadOnly = (n == "Opacity" || n == "Rounding" ||
                                    n == "Dim" || n == "Blur");
            if (steadOnly) s->mVisible = !nova;
            else if (n == "Shadow") s->mVisible = !nova;
            else if (n == "Shadow Strength") s->mVisible = !nova && shadowOn;
            else if (n == "Nova Blur" || n == "Nova Dim" || n == "Nova Limit")
                s->mVisible = nova;
            else if (n == "Nova Max") s->mVisible = nova && limitOn;
        }
    }

    static void AddNotification(const std::string& text, NotifType type = NotifType::Info);
    // Rise-style two-line notification: bold title + a lighter description
    // underneath (NotificationComponent.getFirst/Second).
    static void AddNotification(const std::string& title, const std::string& description,
                                NotifType type = NotifType::Info);

private:
    struct Item {
        std::string title;
        std::string description;
        NotifType type = NotifType::Info;
    };
    static std::deque<Item>& queue();   // pending (max 5, oldest evicted)

    // The card currently on screen + its animation state.
    struct Current {
        Item item;
        unsigned long long startMs = 0;   // when the card started showing
        float anim = 0.f;                // eased zoom value (entry 0->1, exit 1->1.1)
        bool exiting = false;
        bool valid = false;
    };
    static Current& current();
};
