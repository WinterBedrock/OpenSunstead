#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <string>
#include <cmath>

namespace ImRenderUtils {
    // Visual size correction for fonts with tall metrics (Mojangles, Comfortaa)
    // — set by RenderManager each frame based on the pushed GUI font. The
    // compiled ImGui 1.92 rasterizes glyphs at the requested size, so a font
    // whose ascent/em is large simply reads oversized at the same nominal px.
    // Every text helper here multiplies the requested size by this factor, so
    // all GUI text shrinks uniformly when such a font is selected.
    inline float g_visualFontScale = 1.0f;

    inline void fillRectangle(ImVec4 rect, ImColor color, float alpha, float radius = 0.0f, ImDrawList* drawList = nullptr, int flags = 0) {
        if (!drawList) drawList = ImGui::GetBackgroundDrawList();
        color.Value.w *= alpha;
        drawList->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.z, rect.w), color, radius, flags);
    }

    // Rounded rectangle filled with a linear gradient, anti-aliasing intact.
    //
    // AddRectFilledMultiColor emits four plain vertices and cannot round its
    // corners, and PushClipRect is a RECTANGULAR scissor — so clipping a
    // multi-colour quad to a rounded plate still paints the very corner pixels
    // the rounding carved out. That is the "square under the round corner"
    // artefact.
    //
    // Instead the shape is emitted once as an ordinary rounded rect in white and
    // its vertices are tinted in place. ImGui's anti-aliased fill emits the outer
    // fringe with alpha 0, so SCALING each vertex's alpha (rather than replacing
    // it) keeps the silhouette smooth.
    inline void fillGradientRounded(ImDrawList* dl, const ImVec4& rect,
                                    const ImVec4& c0, const ImVec4& c1,
                                    float rounding, int flags = 0, bool vertical = true) {
        if (!dl) return;
        if (rect.z - rect.x < 0.5f || rect.w - rect.y < 0.5f) return;

        const int start = dl->VtxBuffer.Size;
        dl->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.z, rect.w),
                          IM_COL32_WHITE, rounding, flags);
        const int end = dl->VtxBuffer.Size;
        if (end <= start) return;

        const float g0 = vertical ? rect.y : rect.x;
        const float g1 = vertical ? rect.w : rect.z;
        const float span = (g1 - g0) != 0.0f ? (g1 - g0) : 1.0f;

        for (int i = start; i < end; ++i) {
            ImDrawVert& v = dl->VtxBuffer[i];
            const float p = vertical ? v.pos.y : v.pos.x;
            const float t = std::clamp((p - g0) / span, 0.0f, 1.0f);
            const float fringe = (float)((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
            const ImVec4 c(c0.x + (c1.x - c0.x) * t,
                           c0.y + (c1.y - c0.y) * t,
                           c0.z + (c1.z - c0.z) * t,
                           (c0.w + (c1.w - c0.w) * t) * fringe);
            v.col = ImGui::GetColorU32(c);
        }
    }

    inline void addBlur(ImVec4 rect, float strength, float radius) {
        // Backdrop dimming is fully handled by the Backdrop Dim setting to prevent double-dimming.
    }
    inline float getTextWidth(const std::string* text, float size) {
        size *= g_visualFontScale;
        if (size < 1.0f || !text) return 0.0f; // ImGui GetRoundedFontSize() rounds sub-1px to 0 -> assert
        ImFont* font = ImGui::GetFont();
        if (!font) return 0.0f;
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text->c_str()).x;
    }

    inline void drawText(ImVec2 pos, const std::string& text, ImColor color, float size, float alpha, bool shadow, ImDrawList* targetDl = nullptr) {
        size *= g_visualFontScale;
        ImDrawList* _dl = targetDl ? targetDl : ImGui::GetBackgroundDrawList();
        // ImGui (imgui_draw.cpp) asserts font_size > 0; the ClickGUI close
        // animation scales text size toward 0, so skip drawing in that window.
        if (!_dl || size < 1.0f || alpha <= 0.0f) return; // sub-1px rounds to 0 in ImGui -> font assert
        ImFont* font = ImGui::GetFont();
        if (!font) return;
        color.Value.w *= alpha;
        if (shadow) {
            // Shadow must follow the final text alpha — using the raw `alpha`
            // param left a full-opacity black ghost when callers put fade in
            // the ImColor (TargetHUD fade-out).
            int sa = (int)(255.0f * color.Value.w);
            if (sa > 0)
                _dl->AddText(font, size, ImVec2(pos.x + 1, pos.y + 1), ImColor(0, 0, 0, sa), text.c_str());
        }
        if (color.Value.w > 0.001f)
            _dl->AddText(font, size, pos, color, text.c_str());
    }

    template<typename F>
    inline void drawGradientText(ImVec2 pos, const std::string& text, float size, float alpha, bool shadow, F colorFunc, ImDrawList* targetDl = nullptr) {
        size *= g_visualFontScale;
        ImDrawList* _dl = targetDl ? targetDl : ImGui::GetBackgroundDrawList();
        if (size < 1.0f || alpha <= 0.0f) return; // sub-1px rounds to 0 in ImGui -> font assert
        float x = pos.x;
        float index = 0.0f;
        for (char c : text) {
            std::string s(1, c);
            float w = getTextWidth(&s, size);
            ImColor color = colorFunc(index);
            color.Value.w *= alpha;
            if (shadow) {
                int sa = (int)(255.0f * color.Value.w);
                if (sa > 0)
                    _dl->AddText(ImGui::GetFont(), size, ImVec2(x + 1, pos.y + 1), ImColor(0, 0, 0, sa), s.c_str());
            }
            if (color.Value.w > 0.001f)
                _dl->AddText(ImGui::GetFont(), size, ImVec2(x, pos.y), color, s.c_str());
            x += w;
            index += 6.0f; // step size for gradient
        }
    }
    inline float getDeltaTime() {
        return ImGui::GetIO().DeltaTime;
    }
    inline ImVec2 getMousePos() {
        return ImGui::GetIO().MousePos;
    }
    inline bool isMouseOver(ImVec4 rect) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        return mp.x >= rect.x && mp.y >= rect.y && mp.x <= rect.z && mp.y <= rect.w;
    }
    inline ImVec2 getScreenSize() {
        return ImGui::GetIO().DisplaySize;
    }
    inline int buildRoundedRectPath(const ImVec4& rect, float r, ImVec2* pts, ImVec2* nrm, int maxPts);
    inline void addShadowPenumbra(ImDrawList* dl, const ImVec2* pts, const ImVec2* nrm, int n,
                                  float thickness, float a, const ImColor& color);

    inline void fillShadowRectSoft(ImVec4 rect, ImColor color, float alpha, float thickness, float radius = 0.0f, ImDrawList* targetDl = nullptr, ImDrawFlags flags = 0) {
        if (alpha <= 0.0f || thickness <= 0.0f) return;
        if (rect.z - rect.x < 0.5f || rect.w - rect.y < 0.5f) return;
        ImDrawList* dl = targetDl ? targetDl : ImGui::GetBackgroundDrawList();
        if (!dl) return;
        const float a = std::clamp(alpha, 0.0f, 1.0f);
        const ImU32 col = ImGui::GetColorU32(ImVec4(color.Value.x, color.Value.y, color.Value.z, a));
        dl->AddShadowRect(ImVec2(rect.x, rect.y), ImVec2(rect.z, rect.w), col, thickness, ImVec2(0.0f, 0.0f), flags, radius);
    }

    inline void fillShadowRectCutout(ImVec4 rect, ImColor color, float alpha, float thickness, float radius = 0.0f, ImDrawList* targetDl = nullptr) {
        if (alpha <= 0.0f || thickness <= 0.0f) return;
        if (rect.z - rect.x < 0.5f || rect.w - rect.y < 0.5f) return;
        ImDrawList* dl = targetDl ? targetDl : ImGui::GetBackgroundDrawList();
        if (!dl) return;
        ImVec2 pts[64], nrm[64];
        const int n = buildRoundedRectPath(rect, radius, pts, nrm, 64);
        addShadowPenumbra(dl, pts, nrm, n, thickness, std::clamp(alpha, 0.0f, 1.0f), color);
    }

    inline int buildRoundedRectPath(const ImVec4& rect, float r, ImVec2* pts, ImVec2* nrm, int maxPts) {
        const float w = rect.z - rect.x, h = rect.w - rect.y;
        if (w <= 0.5f || h <= 0.5f) return 0;
        r = std::clamp(r, 0.0f, std::min(w, h) * 0.5f);
        const int arc = r > 0.5f ? 6 : 0;   // arc samples per corner
        int n = 0;
        auto push = [&](float x, float y, float nx, float ny) {
            if (n >= maxPts) return;
            pts[n] = ImVec2(x, y); nrm[n] = ImVec2(nx, ny); ++n;
        };
        // Top edge: left -> right, normal (0,-1).
        for (int i = 0; i <= 2; ++i)
            push(rect.x + r + (w - r * 2.0f) * (i / 2.0f), rect.y, 0.f, -1.f);
        // Top-right corner arc (center rect.z-r, rect.y+r), -90..0 deg.
        for (int i = 1; i <= arc; ++i) {
            const float a = (-1.5707963f) + 1.5707963f * (i / (float)arc);
            push(rect.z - r + std::cos(a) * r, rect.y + r + std::sin(a) * r, std::cos(a), std::sin(a));
        }
        // Right edge, normal (1,0).
        for (int i = 1; i <= 2; ++i)
            push(rect.z, rect.y + r + (h - r * 2.0f) * (i / 2.0f), 1.f, 0.f);
        // Bottom-right arc, 0..90.
        for (int i = 1; i <= arc; ++i) {
            const float a = 1.5707963f * (i / (float)arc);
            push(rect.z - r + std::cos(a) * r, rect.w - r + std::sin(a) * r, std::cos(a), std::sin(a));
        }
        // Bottom edge, right -> left, normal (0,1).
        for (int i = 1; i <= 2; ++i)
            push(rect.z - r - (w - r * 2.0f) * (i / 2.0f), rect.w, 0.f, 1.f);
        // Bottom-left arc, 90..180.
        for (int i = 1; i <= arc; ++i) {
            const float a = 1.5707963f + 1.5707963f * (i / (float)arc);
            push(rect.x + r + std::cos(a) * r, rect.w - r + std::sin(a) * r, std::cos(a), std::sin(a));
        }
        // Left edge, normal (-1,0).
        for (int i = 1; i <= 2; ++i)
            push(rect.x, rect.w - r - (h - r * 2.0f) * (i / 2.0f), -1.f, 0.f);
        // Top-left arc, 180..270.
        for (int i = 1; i <= arc; ++i) {
            const float a = 3.14159265f + 1.5707963f * (i / (float)arc);
            push(rect.x + r + std::cos(a) * r, rect.y + r + std::sin(a) * r, std::cos(a), std::sin(a));
        }
        return n;
    }

    inline void addShadowPenumbra(ImDrawList* dl, const ImVec2* pts, const ImVec2* nrm, int n,
                                  float thickness, float a, const ImColor& color) {
        if (!dl || n < 3 || thickness <= 0.5f || a <= 0.003f) return;
        // ~2px per band: dense enough that the piecewise-linear curve reads
        // as one smooth blur, and capped so huge spreads stay bounded.
        const int kBands = (int)std::clamp(thickness / 2.0f, 12.0f, 32.0f);
        const float at = std::clamp(a, 0.0f, 1.0f);
        // sigma = spread/3 so the profile is ~0 where the caller asked it to stop.
        const float sigma = thickness / 3.0f;
        const float k = 1.0f / (sigma * 1.41421356f); // 1/(sigma*sqrt(2)) for erfc
        const ImVec2 uv = dl->_Data->TexUvWhitePixel;
        dl->PrimReserve(n * 6 * kBands, n * 4 * kBands);
        unsigned int base = dl->_VtxCurrentIdx;
        for (int b = 0; b < kBands; ++b) {
            const float d0 = thickness * ((float)b / (float)kBands);
            const float d1 = thickness * ((float)(b + 1) / (float)kBands);
            const float aIn  = at * std::erfc(d0 * k);
            const float aOut = at * std::erfc(d1 * k);
            const ImU32 colIn  = ImGui::GetColorU32(ImVec4(color.Value.x, color.Value.y, color.Value.z,
                                                          std::clamp(aIn, 0.0f, 1.0f)));
            const ImU32 colOut = ImGui::GetColorU32(ImVec4(color.Value.x, color.Value.y, color.Value.z,
                                                          std::clamp(aOut, 0.0f, 1.0f)));
            for (int i = 0; i < n; ++i) {
                const int j = (i + 1) % n;
                const unsigned int v = base + 4u * (unsigned int)(b * n + i);
                dl->PrimWriteVtx(ImVec2(pts[i].x + nrm[i].x * d0, pts[i].y + nrm[i].y * d0), uv, colIn);
                dl->PrimWriteVtx(ImVec2(pts[j].x + nrm[j].x * d0, pts[j].y + nrm[j].y * d0), uv, colIn);
                dl->PrimWriteVtx(ImVec2(pts[j].x + nrm[j].x * d1, pts[j].y + nrm[j].y * d1), uv, colOut);
                dl->PrimWriteVtx(ImVec2(pts[i].x + nrm[i].x * d1, pts[i].y + nrm[i].y * d1), uv, colOut);
                dl->PrimWriteIdx((ImDrawIdx)(v));     dl->PrimWriteIdx((ImDrawIdx)(v + 1)); dl->PrimWriteIdx((ImDrawIdx)(v + 2));
                dl->PrimWriteIdx((ImDrawIdx)(v));     dl->PrimWriteIdx((ImDrawIdx)(v + 2)); dl->PrimWriteIdx((ImDrawIdx)(v + 3));
            }
        }
    }

    inline void fillShadowRectangle(ImVec4 rect, ImColor color, float alpha, float radius, float shadowThickness = 15.0f, float density = 0.6f, ImDrawList* targetDl = nullptr) {
        if (alpha <= 0.0f || shadowThickness <= 0.0f) return;
        ImDrawList* dl = targetDl ? targetDl : ImGui::GetBackgroundDrawList();
        if (!dl) return;
        ImVec2 pts[64], nrm[64];
        const int n = buildRoundedRectPath(rect, radius, pts, nrm, 64);
        addShadowPenumbra(dl, pts, nrm, n, shadowThickness,
                          alpha * std::clamp(density, 0.0f, 1.0f), color);
    }

    inline void fillShadowCircle(ImVec2 center, float radius, ImColor color, float alpha, int num_segments = 12, float shadowThickness = 15.0f, float density = 0.6f) {
        if (alpha <= 0.0f) return;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (!dl) return;
        const float a = std::clamp(alpha, 0.0f, 1.0f);
        const int segs = num_segments > 0 ? num_segments : 0; // 0 = auto-tessellate

        dl->AddCircleFilled(center, radius, ImGui::GetColorU32(ImVec4(color.Value.x, color.Value.y, color.Value.z, a)), segs);
        if (shadowThickness <= 0.0f) return;

        ImVec2 pts[64], nrm[64];
        const int n = buildRoundedRectPath(
            ImVec4(center.x - radius, center.y - radius, center.x + radius, center.y + radius),
            radius, pts, nrm, 64);
        addShadowPenumbra(dl, pts, nrm, n, shadowThickness, a * std::clamp(density, 0.0f, 1.0f), color);
    }

    inline void drawCheckMark(ImVec2 pos, float scale, ImColor color, float alpha) {
        color.Value.w *= alpha;
        ImGui::GetForegroundDrawList()->AddLine(ImVec2(pos.x, pos.y + 2.0f * scale), ImVec2(pos.x + 2.0f * scale, pos.y + 4.0f * scale), color, 2.0f * scale);
        ImGui::GetForegroundDrawList()->AddLine(ImVec2(pos.x + 2.0f * scale, pos.y + 4.0f * scale), ImVec2(pos.x + 7.0f * scale, pos.y - 2.0f * scale), color, 2.0f * scale);
    }

    inline void drawColorPicker(float* color, ImVec4 canvas_pos) {
        static float hue = 0.0f;
        static float saturation = 0.0f;
        static float value = 0.0f;

        // Color to HSV
        ImGui::ColorConvertRGBtoHSV(color[0], color[1], color[2], hue, saturation, value);

        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
        float hue_picker_thickness = 10.0f;

        ImVec2 sv_picker_pos = ImVec2(canvas_pos.x, canvas_pos.y);
        ImVec2 hue_picker_pos = ImVec2(canvas_pos.x, canvas_pos.y + canvas_pos.w + 6.0f);

        // Draw SV Square with a single multicolor rectangle (14000x faster & crash-proof!)
        ImU32 colTL = ImColor::HSV(hue, 0.0f, 1.0f);
        ImU32 colTR = ImColor::HSV(hue, 1.0f, 1.0f);
        ImU32 colBL = ImColor::HSV(hue, 0.0f, 0.0f);
        ImU32 colBR = ImColor::HSV(hue, 1.0f, 0.0f);
        draw_list->AddRectFilledMultiColor(
            sv_picker_pos,
            ImVec2(sv_picker_pos.x + canvas_pos.z, sv_picker_pos.y + canvas_pos.w),
            colTL,
            colTR,
            colBR,
            colBL
        );

        // Draw cursor circle
        ImVec2 sv_cursor_pos = ImVec2(
            sv_picker_pos.x + saturation * canvas_pos.z,
            sv_picker_pos.y + (1.0f - value) * canvas_pos.w
        );
        draw_list->AddCircle(sv_cursor_pos, 4.0f, IM_COL32(255, 255, 255, 255), 12, 1.5f);

        // Draw Hue Bar
        for (int i = 0; i < canvas_pos.z; i++) {
            float h = (float)i / canvas_pos.z;
            ImU32 col = ImColor::HSV(h, 1.0f, 1.0f);
            draw_list->AddRectFilled(
                ImVec2(hue_picker_pos.x + i, hue_picker_pos.y),
                ImVec2(hue_picker_pos.x + i + 1, hue_picker_pos.y + hue_picker_thickness),
                col
            );
        }

        // Draw hue line
        ImVec2 hue_cursor_pos = ImVec2(hue_picker_pos.x + hue * canvas_pos.z, hue_picker_pos.y);
        draw_list->AddLine(
            ImVec2(hue_cursor_pos.x, hue_cursor_pos.y),
            ImVec2(hue_cursor_pos.x, hue_cursor_pos.y + hue_picker_thickness),
            IM_COL32(255, 255, 255, 255),
            1.5f
        );

        // Handle Mouse Input with drag-capture
        ImGuiIO& io = ImGui::GetIO();
        static int dragMode = 0; // 0=none, 1=SV square, 2=hue bar
        bool overSV  = isMouseOver(ImVec4(sv_picker_pos.x, sv_picker_pos.y, sv_picker_pos.x + canvas_pos.z, sv_picker_pos.y + canvas_pos.w));
        bool overHue = isMouseOver(ImVec4(hue_picker_pos.x, hue_picker_pos.y, hue_picker_pos.x + canvas_pos.z, hue_picker_pos.y + hue_picker_thickness));

        if (ImGui::IsMouseClicked(0)) {
            if (overSV)       dragMode = 1;
            else if (overHue) dragMode = 2;
        }
        if (!ImGui::IsMouseDown(0)) dragMode = 0;

        if (dragMode == 1) {
            float sx = (io.MousePos.x - sv_picker_pos.x) / canvas_pos.z;
            float sy = (io.MousePos.y - sv_picker_pos.y) / canvas_pos.w;
            saturation = sx < 0.0f ? 0.0f : (sx > 1.0f ? 1.0f : sx);
            value      = 1.0f - (sy < 0.0f ? 0.0f : (sy > 1.0f ? 1.0f : sy));
            float r, g, b;
            ImGui::ColorConvertHSVtoRGB(hue, saturation, value, r, g, b);
            color[0] = r; color[1] = g; color[2] = b;
        } else if (dragMode == 2) {
            float hx = (io.MousePos.x - hue_picker_pos.x) / canvas_pos.z;
            hue = hx < 0.0f ? 0.0f : (hx > 1.0f ? 1.0f : hx);
            float r, g, b;
            ImGui::ColorConvertHSVtoRGB(hue, saturation, value, r, g, b);
            color[0] = r; color[1] = g; color[2] = b;
        }
    }

    // Soft radial glow
    inline void addGlowCircle(ImDrawList* dl, ImVec2 center, float radius, ImColor col, float a) {
        if (!dl || !dl->_Data || a <= 0.0f || radius <= 1.0f) return;
        const int seg = 28;
        ImU32 inner = ImGui::GetColorU32(ImVec4(col.Value.x, col.Value.y, col.Value.z, a));
        ImU32 outer = ImGui::GetColorU32(ImVec4(col.Value.x, col.Value.y, col.Value.z, 0.0f));
        ImVec2 uv = dl->_Data->TexUvWhitePixel;
        dl->PrimReserve(seg * 3, seg + 2);
        unsigned int base = dl->_VtxCurrentIdx;
        dl->PrimWriteVtx(center, uv, inner);
        for (int i = 0; i <= seg; i++) {
            float ang = ((float)i / (float)seg) * 6.2831853f;
            dl->PrimWriteVtx(ImVec2(center.x + cosf(ang) * radius, center.y + sinf(ang) * radius), uv, outer);
        }
        for (int i = 0; i < seg; i++) {
            dl->PrimWriteIdx((ImDrawIdx)base);
            dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
            dl->PrimWriteIdx((ImDrawIdx)(base + 2 + i));
        }
    }

    // Soft glow that hugs gradient text
    template <typename F>
    inline void drawGlowText(ImVec2 pos, const std::string& text, float size, float alpha,
                             F colorFunc, float radius = 16.0f, float glowAlpha = 0.55f, ImDrawList* targetDl = nullptr) {
        size *= g_visualFontScale;
        if (alpha <= 0.0f || text.empty()) return;
        ImDrawList* dl = targetDl ? targetDl : ImGui::GetBackgroundDrawList();
        float w = getTextWidth(&text, size);
        float cy = pos.y + size * 0.5f;
        float step = radius * 0.45f; if (step < 2.0f) step = 2.0f;
        float gi = 0.0f;
        const float x0 = pos.x - radius * 0.3f;
        const float x1 = pos.x + w + radius * 0.3f;
        const float mid = (x0 + x1) * 0.5f;
        float halfSpan = (x1 - x0) * 0.5f; if (halfSpan < 1.0f) halfSpan = 1.0f;
        for (float x = x0; x <= x1; x += step) {
            const float u = (x - mid) / halfSpan;          // -1..1 along the text
            const float bell = 1.0f - u * u;               // 1 at centre, 0 at ends
            addGlowCircle(dl, ImVec2(x, cy), radius * (0.60f + 0.40f * bell), colorFunc(gi),
                          glowAlpha * alpha * (0.55f + 0.45f * bell));
            gi += step;
        }
        drawGradientText(pos, text, size, alpha, false, colorFunc, targetDl);
    }


}