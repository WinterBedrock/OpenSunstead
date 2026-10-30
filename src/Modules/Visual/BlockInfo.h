#pragma once
#include "../Module.h"
#include <string>

// aka waila
//
// Panel AND icon are both ImGui now (Present hook): the block's real texture is
// drawn with SDK::drawBlockTexture3D directly onto the plate, so it renders ON
// TOP of the blur/fill. That is what replaced the old workaround — the
// game-side ItemRenderer icon (drawn from the ScreenView::setupAndRender hook)
// had to live in its own slot OUTSIDE the panel with an outline around it,
// because the ImGui pass always painted over it. The icon now sits INSIDE the
// plate, left of the text column, and there is no second "plate" behind it.
//
// The look follows the client's counter plates (CounterStyle.h / FPS /
// Scaffold): backdrop blur, dim, drop shadow, rounding, a thin white hairline,
// crisp text (optional shadow), the coordinates in the live theme gradient and
// a scale-up appear / scale-down disappear animation.
class BlockInfo : public Module {
public:
    BlockInfo();
    void OnRender() override;

    // Legacy hook entry: the game-side ItemRenderer pass this module used to draw
    // the icon in. It stays a no-op — the icon is painted by OnRender now, with
    // the block's real texture.
    void DrawGameIcon(void* mcuirc, const std::string& screenName);

private:
    struct Target {
        void* block = nullptr;      // SDK::Block*
        std::string name;
        int x = 0, y = 0, z = 0;
        float dist = 0.f;
    };

    bool resolveTarget(Target& out);

    bool  showTexture() const;
    float iconPixels() const;       // icon edge length in screen pixels

    // Panel geometry for the icon that is painted INSIDE the plate: anchors the
    // icon to the panel's own left padding and vertically centres it against the
    // text block, so the icon and the panel can never disagree about the slot.
    struct Layout {
        float iconX = 0.f, iconY = 0.f, iconSize = 0.f;
        float panelX = 0.f, panelY = 0.f;
        bool  hasIcon = false;
    };
    Layout layoutFor(float panelWidth, float panelHeight) const;

    // Appear/disappear driver for the whole panel (0 = hidden, 1 = shown) — the
    // same scale-up + fade the counter plates use.
    float mPanelAnim = 0.f;

    // Last RESOLVED content. The panel keeps painting while it fades out, so the
    // text has to outlive the frame that resolved it; these are plain string
    // copies of already-read values, never a fresh read of game memory.
    std::string mLastTitle, mLastNs, mLastCoords, mLastDist, mLastIconName;
    bool mHaveContent = false;
};
