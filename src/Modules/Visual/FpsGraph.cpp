#include "FpsGraph.h"
#include "../../Render/HudEditor.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include "../../Utils/MCSDK.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <cstdio>

void FpsGraph::OnRender() {
    if (!IsEnabled()) return;
    // Also visible while the HUD editor is open (it runs on the pause/chat screens).
    if (!HudEditor::HudVisible()) return;

    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
    float now = (float)ImGui::GetTime();
    float inst = ImGui::GetIO().Framerate;
    if (inst < 1.0f) inst = 1.0f;

    // Smooth readout a bit so the graph isn't pure noise.
    if (mSmoothFps <= 0.0f) mSmoothFps = inst;
    mSmoothFps += (inst - mSmoothFps) * std::min(1.0f, dt * 8.0f);

    mSamples.push_back({ now, mSmoothFps });
    const float hist = GetHistory() > 0.5f ? GetHistory() : 4.0f;
    while (!mSamples.empty() && (now - mSamples.front().t) > hist)
        mSamples.pop_front();

    float scale = mScale > 0.01f ? mScale : 1.0f;
    float width  = 160.0f * scale;
    float height = 56.0f * scale;
    mWidth = width;
    mHeight = height;

    ImVec4 rect(mPosX, mPosY, mPosX + width, mPosY + height);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float rounding = GetRounding() * scale;
    float dim = GetDim();
    float blur = GetBlur();

    if (blur > 0.01f)
        D2D::addBlurOptimized(dl, blur, std::optional<ImVec4>(rect), rounding);
    ImRenderUtils::fillShadowRectangle(rect, ImColor(0, 0, 0), 0.45f, rounding, 10.0f * scale);
    ImRenderUtils::fillRectangle(rect, ImColor(12, 12, 15, (int)(255 * dim)), 1.0f, rounding);
    dl->AddRect(ImVec2(rect.x, rect.y), ImVec2(rect.z, rect.w), ImColor(255, 255, 255, 28), rounding, 0, 1.0f);

    // Graph area with padding.
    float pad = 8.0f * scale;
    float topExtra = GetShowValue() ? 14.0f * scale : 0.0f;
    ImVec2 g0(rect.x + pad, rect.y + pad + topExtra);
    ImVec2 g1(rect.z - pad, rect.w - pad);
    float gw = g1.x - g0.x;
    float gh = g1.y - g0.y;
    if (gw < 4.0f || gh < 4.0f) return;

    // Range.
    float mn = 1e9f, mx = 0.0f;
    for (const auto& s : mSamples) {
        if (s.fps < mn) mn = s.fps;
        if (s.fps > mx) mx = s.fps;
    }
    if (mx - mn < 5.0f) { // keep some vertical room
        float mid = (mn + mx) * 0.5f;
        mn = mid - 2.5f;
        mx = mid + 2.5f;
    }
    if (mn < 0.0f) mn = 0.0f;

    // Soft fill under the line + line itself.
    if (mSamples.size() >= 2) {
        ImColor accent = ColorUtils::getThemedColor(0.0f);
        ImColor fillTop = accent; fillTop.Value.w = 0.35f;
        ImColor fillBot = accent; fillBot.Value.w = 0.02f;
        ImColor lineCol = accent; lineCol.Value.w = 0.95f;

        // Build polyline in graph space.
        ImVector<ImVec2> pts;
        pts.reserve((int)mSamples.size());
        float t0 = mSamples.front().t;
        float t1 = mSamples.back().t;
        float tSpan = (t1 - t0) > 0.001f ? (t1 - t0) : 1.0f;
        for (const auto& s : mSamples) {
            float nx = (s.t - t0) / tSpan;
            float ny = (s.fps - mn) / (mx - mn);
            if (ny < 0.0f) ny = 0.0f;
            if (ny > 1.0f) ny = 1.0f;
            pts.push_back(ImVec2(g0.x + nx * gw, g1.y - ny * gh));
        }

        // Filled area: triangle strip from line down to baseline.
        for (int i = 0; i + 1 < pts.Size; ++i) {
            ImVec2 a = pts[i];
            ImVec2 b = pts[i + 1];
            ImVec2 aB(a.x, g1.y);
            ImVec2 bB(b.x, g1.y);
            dl->AddTriangleFilled(a, b, bB, ImGui::GetColorU32(ImVec4(fillTop.Value.x, fillTop.Value.y, fillTop.Value.z, fillTop.Value.w * 0.55f)));
            dl->AddTriangleFilled(a, bB, aB, ImGui::GetColorU32(ImVec4(fillBot.Value.x, fillBot.Value.y, fillBot.Value.z, fillBot.Value.w + 0.08f)));
        }

        dl->AddPolyline(pts.Data, pts.Size, ImGui::GetColorU32(ImVec4(lineCol.Value.x, lineCol.Value.y, lineCol.Value.z, lineCol.Value.w)), 0, 1.8f * scale);
    }

    if (GetShowValue()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f FPS", mSmoothFps);
        std::string text = buf;
        float fs = 13.0f * scale;
        ImRenderUtils::drawText(ImVec2(rect.x + pad, rect.y + 4.0f * scale), text,
            ColorUtils::getThemedColor(0.0f), fs, 1.0f, true);
    }
}