#pragma once
#include "../Module.h"
#include <string>
#include <unordered_map>

namespace SDK { class Actor; }

// tags!
class NameTags : public Module {
public:
    NameTags() : Module("NameTags", "Draws name labels above players", Category::Visual, 0) {
        AddSetting(new BoolSetting("Render Local", "Also draw your own nametag", true));
        AddSetting(new BoolSetting("Distance Scale", "Scale the font by distance", true));
        AddSetting(new NumberSetting("Font Size", "Text size when distance scaling is off", 18.0f, 6.0f, 48.0f, 0.5f));
        AddSetting(new NumberSetting("Scale Mult", "Distance-scale strength", 1.0f, 0.1f, 5.0f, 0.01f));
        AddSetting(new NumberSetting("Min Scale", "Smallest font when far away", 12.0f, 4.0f, 40.0f, 0.5f));
        AddSetting(new EnumSetting("Color", "Text color source", 0, { "Theme", "Custom" }));
        AddSetting(new ColorSetting("Custom Color", "Custom text color", 1.0f, 1.0f, 1.0f, 1.0f));
        AddSetting(new BoolSetting("Background", "Draw a backing rectangle", true));
        AddSetting(new NumberSetting("Darkening", "Opacity of the backing plate", 0.5f, 0.0f, 1.0f, 0.01f));
        AddSetting(new NumberSetting("Rounding", "Corner radius of the backing plate", 5.0f, 0.0f, 20.0f, 0.5f));
        AddSetting(new NumberSetting("Blur", "Soft glow/blur strength around the plate", 0.0f, 0.0f, 10.0f, 0.1f));
        AddSetting(new BoolSetting("Shadow", "Draw a drop shadow under the plate", false));
        AddSetting(new NumberSetting("Shadow Strength", "Opacity of the plate shadow", 0.3f, 0.0f, 1.0f, 0.01f));
        AddSetting(new NumberSetting("Shadow Offset", "Shadow distance in pixels", 0.4f, 0.0f, 6.0f, 0.1f));
        AddSetting(new BoolSetting("Health", "Append the player's health", false));
        AddSetting(new BoolSetting("BPS", "Append the player's BPS", false));
        AddSetting(new BoolSetting("Entities", "Also label non-player entities", false));
        AddSetting(new BoolSetting("Hide Original", "Hide the game's built-in nametag", true));
        AddSetting(new BoolSetting("Always show", "Render on every screen, not just the HUD", false));
        AddSetting(new BoolSetting("Render Heads", "Draw the player's head left of the name", true));
        AddSetting(new BoolSetting("Separator", "Separate stats", true));
        // ---- Custom self tag (appended LAST: indexes above must not move) ----
        // While "Render Local" is on, your own tag can carry a custom name,
        // optionally followed by the real nick in brackets: "fake (real)".
        AddSetting(new BoolSetting("Custom Self Name", "Show your own tag under a custom name", false));   // [21]
        AddSetting(new StringSetting("Self Name", "The custom name for your own tag", "sunstead"));        // [22]
        AddSetting(new BoolSetting("Show Real Name", "Append the real nick in brackets: fake (real)", true)); // [23]
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnRender() override;
    ~NameTags() override;
    void UpdateSettingsVisibility() override {
        // The custom-self block only exists while the own tag is drawn at all.
        if (mSettings.size() > 23) {
            const bool local = static_cast<BoolSetting*>(mSettings[0])->mValue;
            const bool custom = static_cast<BoolSetting*>(mSettings[21])->mValue;
            mSettings[21]->mVisible = local;
            mSettings[22]->mVisible = local && custom;
            mSettings[23]->mVisible = local && custom;
        }
    }

    static bool ShouldHideActor(void* actor, bool* fallbackAll = nullptr);

    static bool ShouldHideOriginal();

    static void flushDeferredHeadReleases();

private:
    std::unordered_map<std::string, std::pair<void*, int>> mHeadTex;
    void clearHeads();
};
