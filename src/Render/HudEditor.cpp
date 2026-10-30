#include "HudEditor.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "RenderManager.h"
#include "../Modules/ModuleManager.h"
#include "../Modules/Visual/Chat.h"
#include "../SDK/Client/ScreenView.h"
#include "../Utils/MiscUtils/ColorUtils.hpp"
#include "../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../Utils/MiscUtils/D2D.hpp"

namespace HudEditor {
namespace {

Module* g_dragging = nullptr;
ImVec2 g_dragOffset{ 0.0f, 0.0f };

// The drag screens are exactly the ones where the game has already released
// the cursor. On hud_screen the cursor is grabbed for camera control — EXCEPT
// while the custom Chat menu is open (its WndProc branch blocks the keys from
// the game and releases the cursor), which makes it a drag surface too.
bool onEditableScreen() {
    const std::string& sn = SDK::ScreenView::currentScreenName();
    if (sn == "pause_screen" || sn == "chat_screen") return true;
    return sn == "hud_screen" && ::Chat::IsOpen();
}

std::vector<Module*> hudModules() {
    std::vector<Module*> out;
    for (Module* mod : ModuleManager::GetInstance().GetModules()) {
        if (!mod || !mod->mIsHudModule || !mod->IsEnabled()) continue;
        // Bounds come from the element's own last draw. Some report nothing when
        // they have nothing to show (the ArrayList with no enabled modules zeroes
        // its size), and a zero-area box is neither visible nor grabbable.
        if (mod->mWidth < 4.0f || mod->mHeight < 4.0f) continue;
        out.push_back(mod);
    }
    return out;
}

// Reset-Positions button state. The button is shown while a drag is active (or
// within its linger window) and clicked applies an OnFirstLayout pass over
// every HUD module at its module-provided default.
struct ResetButton {
    float anim = 0.0f;              // 0..1 visibility
    unsigned long long lingerUntil = 0; // ms; button stays while dragging +3s after
    bool wasDragging = false;

    // Reset every HUD module to its module-provided default position.
    // Each module restores its own construction-time position and recomputes
    // screen-dependent defaults (Scaffold's "near center", Player's bottom
    // center); mScale is reset to 1.0.
    static void resetPositions() {
        ImVec2 screen = ImGui::GetIO().DisplaySize;
        for (Module* mod : ModuleManager::GetInstance().GetModules()) {
            if (!mod || !mod->mIsHudModule) continue;
            mod->ResetPosition(screen.x, screen.y);
        }
    }

    void frame(bool draggingNow) {
        ImGuiIO& io = ImGui::GetIO();
        const float dt = (io.DeltaTime > 0.0f && io.DeltaTime < 0.25f) ? io.DeltaTime : 1.0f / 60.0f;
        unsigned long long now = GetTickCount64();
        if (draggingNow) {
            lingerUntil = now + 3000;
            wasDragging = true;
        } else if (wasDragging) {
            wasDragging = false;
            lingerUntil = now + 3000;
        }
        bool wantShow = draggingNow || (now < lingerUntil);
        anim += ((wantShow ? 1.0f : 0.0f) - anim) * std::min(1.0f, dt * 8.0f);
        if (anim < 0.004f) { anim = 0.0f; return; }

        // ---- draw the button -------------------------------------------------
        const std::string label = "Reset Positions";
        const float fontSize = 14.0f;
        const float padX = 12.0f, padY = 8.0f;
        float w = ImRenderUtils::getTextWidth(&label, fontSize) + padX * 2.0f;
        float h = fontSize + padY * 2.0f;
        ImVec2 screen = io.DisplaySize;
        // Slide down from the top (fade + slide-in), centred horizontally.
        const float slide = (1.0f - anim) * 28.0f;
        ImVec2 pos(screen.x * 0.5f - w * 0.5f, 20.0f - slide);
        ImVec4 rect(pos.x, pos.y, pos.x + w, pos.y + h);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float a = anim;

        // Backdrop: project D2D blur + fill shadow + plate (rounded corners).
        D2D::addBlurOptimized(dl, 4.0f * a, std::optional<ImVec4>(rect), 8.0f);
        ImRenderUtils::fillShadowRectangle(rect, ImColor(0, 0, 0), 0.30f * a, 8.0f, 10.0f, 0.5f, dl);
        ImRenderUtils::fillRectangle(rect, ImColor(12, 12, 15, 255), 0.42f * a, 8.0f, dl);
        dl->AddRect(ImVec2(rect.x, rect.y), ImVec2(rect.z, rect.w),
                    ImColor(255, 255, 255, (int)(30.0f * a)), 8.0f, 0, 1.0f);
        ImRenderUtils::drawText(ImVec2(rect.x + padX, rect.y + padY), label,
                                ImColor(255, 255, 255, 255), fontSize, a, true, dl);

        // Click: a simple point-in-rect hit-test on mouseup.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            ImRenderUtils::isMouseOver(rect)) {
            resetPositions();
        }
    }
};
static ResetButton g_resetBtn;

} // namespace

bool HudVisible() {
    // HUD elements live on the HUD screen and stay visible on the chat/pause
    // screens, where they can be grabbed and repositioned directly.
    const std::string& sn = SDK::ScreenView::currentScreenName();
    return sn == "hud_screen" || sn == "pause_screen" || sn == "chat_screen";
}

void OnRender() {
    if (!onEditableScreen()) {
        g_dragging = nullptr;
        return;
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool lmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    const std::vector<Module*> mods = hudModules();

    // Grab detection — no hover chrome: the old hover frames read as leftover
    // editor clutter; an element simply follows the cursor once grabbed (with
    // a tiny name tag so you see what you are moving).
    Module* hovered = nullptr;
    if (!g_dragging) {
        for (Module* mod : mods) {
            const ImVec4 rect(mod->mPosX, mod->mPosY,
                              mod->mPosX + mod->mWidth, mod->mPosY + mod->mHeight);
            if (ImRenderUtils::isMouseOver(rect)) { hovered = mod; break; }
        }
    }

    // A module that owns this frame's left click (the Player's prev/pause/next
    // buttons, its progress bar) has already consumed it — grabbing the panel
    // here would yank it out from under the button press.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !hovered->mClickConsumed) {
        g_dragging = hovered;
        g_dragOffset = ImVec2(mouse.x - hovered->mPosX, mouse.y - hovered->mPosY);
    }

    bool draggingNow = false;
    if (g_dragging) {
        Module* mod = g_dragging;
        const ImVec4 rect(mod->mPosX, mod->mPosY,
                          mod->mPosX + mod->mWidth, mod->mPosY + mod->mHeight);
        const std::string& name = mod->GetName();
        const float ly = (rect.y - 16.0f > 4.0f) ? (rect.y - 16.0f) : (rect.w + 4.0f);
        ImRenderUtils::drawText(ImVec2(rect.x + 1.0f, ly), name,
                                ImColor(0.925f, 0.937f, 0.969f, 1.0f), 12.0f, 0.9f, true, dl);

        if (!lmbDown) {
            g_dragging = nullptr;
        } else {
            draggingNow = true;
            // Animated drag: the element eases toward the cursor (and into the
            // edge/centre snaps) instead of hard-teleporting — repositioning
            // feels like moving a physical card, not dragging a debug rect.
            float nx = mouse.x - g_dragOffset.x;
            float ny = mouse.y - g_dragOffset.y;
            const float snap = 10.0f;
            if (std::fabs(nx) < snap) nx = 0.0f;
            else if (std::fabs(nx + mod->mWidth - screen.x) < snap) nx = screen.x - mod->mWidth;
            if (std::fabs(ny) < snap) ny = 0.0f;
            else if (std::fabs(ny + mod->mHeight - screen.y) < snap) ny = screen.y - mod->mHeight;
            nx = std::clamp(nx, 0.0f, std::max(0.0f, screen.x - mod->mWidth));
            ny = std::clamp(ny, 0.0f, std::max(0.0f, screen.y - mod->mHeight));
            const float dt = ImGui::GetIO().DeltaTime;
            const float k = std::clamp(dt * 26.0f, 0.0f, 1.0f);   // snappy but visible
            mod->mPosX += (nx - mod->mPosX) * k;
            mod->mPosY += (ny - mod->mPosY) * k;
        }
    }

    // Reset-Positions button: driven off draggingNow (or its 3 s linger).
    g_resetBtn.frame(draggingNow);

    // Scroll over a hovered element to resize it (kept from the editor) —
    // EXCEPT the Chat panel: its size is owned by the module's settings and
    // the wheel scrolls the message history there.
    // Range is [0.5, 6.0]: the old 2.5 ceiling sat right on Watermark's 2.4
    // default, so the watermark started AT the max and could only shrink.
    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f && hovered->GetName() != "Chat")
            hovered->mScale = std::clamp(hovered->mScale + wheel * 0.05f, 0.5f, 6.0f);
    }
}

} // namespace HudEditor
