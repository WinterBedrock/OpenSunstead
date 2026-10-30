#include "MouseKeystrokes.h"
#include "../../Render/HudEditor.h"
#include "../../Utils/MCSDK.h"
#include "../Setting.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>

void MouseKeystrokes::OnEnable() {
    mDeltaX = 0.0f;
    mDeltaY = 0.0f;
    mCursorX = 0.0f;
    mCursorY = 0.0f;
    mHasPrev = false;
    trails.clear();
}

void MouseKeystrokes::OnDisable() {
    trails.clear();
    mHasPrev = false;
}

void MouseKeystrokes::OnRender() {
    if (!IsEnabled()) return;
    // Also visible while the HUD editor is open (it runs on the pause/chat screens).
    if (!HudEditor::HudVisible()) return;

    // ---- Get local player + rotation component ----
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, 0x200)) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp || !sdk_safe_ptr(lp, 0x230)) return;

    // SEH-guarded rotation read (actor pointers can be transient).
    Vec2 curRot;
    __try {
        ActorRotationComponent* rot = lp->getRotation();
        if (!rot) return;
        curRot = rot->rotation; // x = pitch, y = yaw
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    // ---- Compute rotation delta from previous frame ----
    if (mHasPrev) {
        float deltaYaw   = curRot.y - mPrevRot.y;
        float deltaPitch = curRot.x - mPrevRot.x;

        // Wrap deltaYaw to [-180, 180] so crossing the -180/180 boundary
        // doesn't produce a ~360 degree jump.
        if (deltaYaw > 180.0f)  deltaYaw -= 360.0f;
        else if (deltaYaw < -180.0f) deltaYaw += 360.0f;

        // Scale the rotation delta to cursor movement (sensitivity multiplier).
        // Lower value = less sensitive / slower cursor.
        float sensitivity = 1.0f;
        mDeltaX += deltaYaw   * sensitivity;
        mDeltaY += deltaPitch * sensitivity;
    }
    mPrevRot = curRot;
    mHasPrev = true;

    // ---- Decay (smooth return to centre) ----
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    float decayRate = std::min(dt * 6.0f, 0.8f);
    mDeltaX *= (1.0f - decayRate);
    mDeltaY *= (1.0f - decayRate);

    // ---- Clamp to prevent runaway values ----
    const float maxMovement = 200.0f;
    mDeltaX = std::clamp(mDeltaX, -maxMovement, maxMovement);
    mDeltaY = std::clamp(mDeltaY, -maxMovement, maxMovement);

    // ---- Settings ----
    // Combine the module's own "Scale" setting with mScale (adjusted by the HUD
    // editor scroll wheel) so the panel can be resized from the HUD editor.
    float scale     = static_cast<NumberSetting*>(mSettings[0])->mValue * mScale;
    float rounding  = static_cast<NumberSetting*>(mSettings[1])->mValue;
    bool  doGlow    = static_cast<BoolSetting*>(mSettings[2])->mValue;
    bool  doBorder  = static_cast<BoolSetting*>(mSettings[3])->mValue;
    bool  doShadow  = static_cast<BoolSetting*>(mSettings[4])->mValue;
    float* bgCol    = static_cast<ColorSetting*>(mSettings[5])->mValue;
    float* curCol   = static_cast<ColorSetting*>(mSettings[6])->mValue;
    float* glowCol  = static_cast<ColorSetting*>(mSettings[7])->mValue;
    float cursorSizeRatio = static_cast<NumberSetting*>(mSettings[8])->mValue;
    float borderThickness  = static_cast<NumberSetting*>(mSettings[9])->mValue;

    // ---- Panel geometry ----
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    float panelSize = 80.0f * scale;
    float panelRounding = rounding * scale;
    float cursorRadius = panelSize * cursorSizeRatio;

    // Default to centre of screen; HUD editor overrides mPosX/mPosY.
    float panelX = mPosX;
    float panelY = mPosY;
    if (panelX < 1.0f && panelY < 1.0f) {
        panelX = (screen.x - panelSize) * 0.5f;
        panelY = (screen.y - panelSize) * 0.5f;
    }

    // Set mWidth/mHeight so the HUD editor can detect this module for dragging.
    mWidth = panelSize;
    mHeight = panelSize;

    // ---- Smoothed cursor offset within the panel ----
    float targetX = mDeltaX * scale;
    float targetY = mDeltaY * scale;
    float lerpFactor = std::min(dt * 12.0f, 1.0f);
    mCursorX += (targetX - mCursorX) * lerpFactor;
    mCursorY += (targetY - mCursorY) * lerpFactor;

    // Cursor position (clamped within panel bounds).
    float half = panelSize * 0.5f;
    float cx = panelX + half + mCursorX;
    float cy = panelY + half + mCursorY;
    cx = std::clamp(cx, panelX + cursorRadius, panelX + panelSize - cursorRadius);
    cy = std::clamp(cy, panelY + cursorRadius, panelY + panelSize - cursorRadius);

    // ---- Add current position to trail ----
    trails.insert(trails.begin(), {cx, cy, 1.0f});
    // Cap trail length to avoid unbounded growth.
    if (trails.size() > 40) trails.resize(40);

    // ---- Draw ----
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 pMin(panelX, panelY);
    ImVec2 pMax(panelX + panelSize, panelY + panelSize);

    // Drop shadow beneath the panel.
    if (doShadow)
        ImRenderUtils::fillShadowRectangle(
            ImVec4(panelX, panelY, panelX + panelSize, panelY + panelSize),
            ImColor(0, 0, 0), 0.35f, panelRounding, 10.0f);

    // Outer glow (coloured).
    if (doGlow)
        ImRenderUtils::fillShadowRectangle(
            ImVec4(panelX, panelY, panelX + panelSize, panelY + panelSize),
            ImColor((int)(glowCol[0] * 255), (int)(glowCol[1] * 255), (int)(glowCol[2] * 255)),
            glowCol[3] * 0.5f, panelRounding, 15.0f);

    // Panel background.
    ImU32 bgU32 = ImGui::GetColorU32(ImVec4(bgCol[0], bgCol[1], bgCol[2], bgCol[3]));
    dl->AddRectFilled(pMin, pMax, bgU32, panelRounding);

    // Border outline.
    if (doBorder)
        dl->AddRect(pMin, pMax,
                    ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.4f)),
                    panelRounding, 0, borderThickness * scale);

    // ---- Cursor + fading trail ----
    // Start with full cursor alpha, multiply by 0.9 for each older trail point
    // (same fading approach as FlarialSRC's Mousestrokes).
    float trailAlpha = curCol[3];
    for (const auto& t : trails) {
        ImU32 col = ImGui::GetColorU32(
            ImVec4(curCol[0], curCol[1], curCol[2], trailAlpha));
        dl->AddCircleFilled(ImVec2(t.x, t.y), cursorRadius, col, 16);
        trailAlpha *= 0.9f;
        if (trailAlpha < 0.01f) break;
    }
}
