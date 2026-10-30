#pragma once
#include <string>

struct ImDrawList;
struct ID3D11Resource;

// Real game icons, captured from Minecraft's own ItemRenderer (its UI pass
// lands UNDER our ImGui overlay, so we capture instead of drawing in place):
//  1. UI thread, in ScreenView::setupAndRender (hud_screen): fillRectangle
//     (vt[15]) paints a black and a white cell; renderGuiItemNew draws the
//     block's ItemStack on both.
//  2. Present hook, before ImGui: both cells are copied out of the back buffer
//     into a staging texture and read back.
//  3. Difference matting: a = 1 - (white - black), rgb = black / a yields the
//     icon with its real anti-aliased alpha; uploaded once as an RGBA SRV and
//     cached by block name.
// The scratch cells appear for a frame or two per NEW block type only; later
// draws are cached ImGui images ON TOP of HUD plates and blur.
namespace SDK {
namespace GameIcon {

// Game/render thread: remember which live Block* belongs to a block name so a
// capture can be scheduled. Cheap; call every frame.
void noteBlock(const std::string& name, void* block);

// ImGui (Present) thread: draw the captured icon for `name`. Returns false
// (and schedules a capture when a Block* is known) when not ready yet, so
// callers fall back this frame.
bool draw(ImDrawList* dl, float x, float y, float size, const std::string& name, float alpha);

// UI thread hook (ScreenView::setupAndRender, after the original call).
void onUiRender(void* mcuirc, const std::string& screenName);

// Present hook, before ImGui renders. `backBuffer` is the D3D11 resource the
// game rendered into this frame (wrapped resource on DX12).
void onPresent(ID3D11Resource* backBuffer, bool isDX12);

// Drop every cached texture (resize / uninject / world change).
void clear();

} // namespace GameIcon
} // namespace SDK
