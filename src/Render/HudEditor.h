#pragma once

// ---------------------------------------------------------------------------
// HUD layout — direct drag, no editor mode.
//
// The old full-screen HUD editor (open button, overlay chrome, Done button)
// is gone. What remains is the Rise-style behaviour it existed for:
//   * HUD elements render on the HUD screen AND on the chat/pause screens,
//   * on chat/pause you simply grab an element with the mouse and drag it,
//     scroll over one to scale it, and that is the whole editing experience.
//
// On hud_screen the game holds the cursor, so nothing is grabbable there —
// positions are still loaded/saved with the config (posX/posY/scale).
// State is file-static; the render pass is single-threaded.
// ---------------------------------------------------------------------------
namespace HudEditor {

// True when a HUD element should draw this frame: the HUD screen plus the
// chat/pause screens, where the elements are visible and draggable.
bool HudVisible();

// Hover outlines + drag/scale handling for HUD elements on the chat/pause
// screens. Called once per frame from RenderManager.
void OnRender();

} // namespace HudEditor
