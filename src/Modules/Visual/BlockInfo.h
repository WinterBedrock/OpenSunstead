#pragma once
#include "../Module.h"
#include <string>

class BlockInfo : public Module {
public:
    BlockInfo();
    void OnRender() override;

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
    float iconPixels() const;

    struct Layout {
        float iconX = 0.f, iconY = 0.f, iconSize = 0.f;
        float panelX = 0.f, panelY = 0.f;
        bool  hasIcon = false;
    };
    Layout layoutFor(float panelWidth, float panelHeight) const;

    float mPanelAnim = 0.f;

    std::string mLastTitle, mLastNs, mLastCoords, mLastDist, mLastIconName;
    bool mHaveContent = false;
};
