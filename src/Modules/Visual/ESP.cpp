#include "ESP.h"
#include "../../Core/Logger.h"
#include "../../Render/FrameView.h"
#include "../../Hooks/GameHooks.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/Actor/RenderPos.h"
#include "../../SDK/Render/BoxOverlay.h"
#include "../Setting.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include <imgui.h>
#include <algorithm>
#include <vector>
#include <Windows.h>
#include <cstdio>
#include <unordered_map>
#include <cmath>

namespace {

static bool projectActorBox(SDK::Actor* actor, const float* mvp, const Vec3& origin,
                            const ImVec2& screen, float expand, ImVec2* tl, ImVec2* br) {
    Vec3 pos;
    AABB bb;
    __try {
        pos = actor->getPos();
        bb = actor->getBoundingBox();           // true world-space lower/higher
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    Vec3 dim = bb.higher - bb.lower;
    if (!(dim.x > 0.05f && dim.x < 6.0f) || !(dim.y > 0.1f && dim.y < 6.0f)) {
        dim = Vec3(0.6f, 1.8f, 0.6f);
    }
    Vec3 rp = SDK::interpRenderPos(actor);
    Vec3 delta(rp.x - pos.x, rp.y - pos.y, rp.z - pos.z);
    Vec3 mn(bb.lower.x  + delta.x, bb.lower.y  + delta.y, bb.lower.z  + delta.z);
    Vec3 mx(bb.higher.x + delta.x, bb.higher.y + delta.y, bb.higher.z + delta.z);
    if (expand > 0.0f) {
        mn = Vec3(mn.x - expand, mn.y - expand, mn.z - expand);
        mx = Vec3(mx.x + expand, mx.y + expand, mx.z + expand);
    }

    // Projection itself is shared (BoxOverlay); the clamp + degenerate policy
    // below is this module's own (partially off-screen boxes keep drawing).
    ImVec2 rawTl, rawBr;
    if (SDK::BoxOverlay::projectBoxRect(mn, mx, mvp, origin, screen, &rawTl, &rawBr) == 0)
        return false;                            // entirely behind camera
    float minX = rawTl.x, minY = rawTl.y, maxX = rawBr.x, maxY = rawBr.y;
    // Clamp to screen so partially off-screen boxes still draw their visible part.
    if (maxX < 0.0f || minX > screen.x || maxY < 0.0f || minY > screen.y) return false;
    if (minX < 0.0f) minX = 0.0f;
    if (minY < 0.0f) minY = 0.0f;
    if (maxX > screen.x) maxX = screen.x;
    if (maxY > screen.y) maxY = screen.y;
    if (maxX - minX < 1.0f || maxY - minY < 1.0f) return false;  // degenerate
    *tl = ImVec2(minX, minY);
    *br = ImVec2(maxX, maxY);
    return true;
}


// Project all 8 AABB corners individually for a true 3D box (shared
// BoxOverlay primitive; the feet/AABB rebase recipe below stays local).
static int projectActorCorners(SDK::Actor* actor, const float* mvp, const Vec3& origin,
                               const ImVec2& screen, float expand,
                               ImVec2 pts[8], bool valid[8]) {
    Vec3 pos; AABB bb;
    __try { pos = actor->getPos(); bb = actor->getBoundingBox(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    Vec3 dim = bb.higher - bb.lower;
    if (!(dim.x > 0.05f && dim.x < 6.0f) || !(dim.y > 0.1f && dim.y < 6.0f)) dim = Vec3(0.6f, 1.8f, 0.6f);
    Vec3 rp = SDK::interpRenderPos(actor);   // push-time pair + served alpha (glides with the model)
    Vec3 delta(rp.x - pos.x, rp.y - pos.y, rp.z - pos.z);
    Vec3 mn(bb.lower.x  + delta.x, bb.lower.y  + delta.y, bb.lower.z  + delta.z);
    Vec3 mx(bb.higher.x + delta.x, bb.higher.y + delta.y, bb.higher.z + delta.z);
    if (expand > 0.0f) {
        mn = Vec3(mn.x - expand, mn.y - expand, mn.z - expand);
        mx = Vec3(mx.x + expand, mx.y + expand, mx.z + expand);
    }
    return SDK::BoxOverlay::projectBoxCorners(mn, mx, mvp, origin, screen, pts, valid);
}

// ---------------------------------------------------------------------------
static constexpr float kEspGradCycleFrac = 0.25f;

static float espGradSpan() {
    return ColorUtils::getThemedPeriod() * kEspGradCycleFrac;
}

// Theme colour at vertical position t along a box (0 = top edge, 1 = bottom),
// `span` being the index span from espGradSpan().
static ImColor espThemeAt(float t, float span, uint64_t ms) {
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    return ColorUtils::getThemedColor(t * span, ms);
}

static void tintEspGradient(ImDrawList* dl, int vStart, float yTop, float yBot,
                            float span, uint64_t ms, float alphaScale = 1.0f) {
    const int vEnd = dl->VtxBuffer.Size;
    if (vEnd <= vStart) return;
    const float h = (yBot - yTop) > 1.0f ? (yBot - yTop) : 1.0f;
    for (int i = vStart; i < vEnd; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const ImColor c = espThemeAt((v.pos.y - yTop) / h, span, ms);
        const float cov = (float)((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
        v.col = ImGui::ColorConvertFloat4ToU32(ImVec4(c.Value.x, c.Value.y, c.Value.z,
                                                      cov * alphaScale));
    }
}

static ImU32 hpColor(float frac, float alpha = 1.0f) {
    if (frac < 0.0f) frac = 0.0f; else if (frac > 1.0f) frac = 1.0f;
    // green (0.20,1.00,0.25) -> yellow (1.00,0.95,0.20) on [0.5..1],
    // yellow -> red (1.00,0.15,0.10) on [0..0.5].
    float r, g, b;
    if (frac >= 0.5f) {
        const float t = (frac - 0.5f) * 2.0f;         // 0 at half, 1 at full
        r = 1.00f - 0.80f * t;
        g = 0.95f + 0.05f * t;
        b = 0.20f + 0.05f * t;
    } else {
        const float t = frac * 2.0f;                  // 0 at death, 1 at half
        r = 1.00f;
        g = 0.15f + 0.80f * t;
        b = 0.10f + 0.10f * t;
    }
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, alpha));
}

static void espRectShadow(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                          float opacity, float spread) {
    if (opacity <= 0.005f || spread <= 0.5f) return;
    ImRenderUtils::fillShadowRectangle(ImVec4(tl.x, tl.y, br.x, br.y),
                                       ImColor(0, 0, 0), opacity, 0.0f,
                                       spread, 0.9f, dl);
}

static void drawFlatPlane(SDK::Actor* actor, ImDrawList* dl, const float* mvp,
                          const Vec3& origin, const ImVec2& screen, float scaleMul,
                          bool doFill, bool themeFill, ImU32 fillColor, float fillOp,
                          bool themeOutline, ImU32 color, float thickness,
                          float gradSpan, uint64_t nowMs) {
    Vec3 pos; AABB bb;
    float yawDeg = 0.0f;
    __try { pos = actor->getPos(); bb = actor->getBoundingBox(); yawDeg = actor->getRot().y; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    Vec3 dim = bb.higher - bb.lower;
    if (!(dim.x > 0.05f && dim.x < 6.0f) || !(dim.y > 0.1f && dim.y < 6.0f)) {
        dim = Vec3(0.6f, 1.8f, 0.6f);
    }
    Vec3 rp = SDK::interpRenderPos(actor);
    Vec3 delta(rp.x - pos.x, rp.y - pos.y, rp.z - pos.z);
    const float cx = (bb.lower.x + bb.higher.x) * 0.5f + delta.x;
    const float cz = (bb.lower.z + bb.higher.z) * 0.5f + delta.z;
    const float y0 = bb.lower.y + delta.y;
    const float y1 = bb.higher.y + delta.y;
    // Strip half-width: the hitbox footprint, times the Flat Scale multiplier.
    const float hw = std::clamp(dim.x * 0.5f * scaleMul, 0.05f, 3.0f);
    // Actor look direction (MC yaw: facing = (-sin, cos)). The strip runs
    // PERPENDICULAR to it — across the body, facing where they look.
    const float yaw = yawDeg * 0.017453292519943295f;
    const float lx = -std::sin(yaw), lz = std::cos(yaw);
    const float fx =  lz;   // horizontal axis = look rotated 90 degrees
    const float fz = -lx;

    ImVec2 p[4];
    const Vec3 quad[4] = {
        Vec3(cx - fx * hw, y0, cz - fz * hw),
        Vec3(cx + fx * hw, y0, cz + fz * hw),
        Vec3(cx + fx * hw, y1, cz + fz * hw),
        Vec3(cx - fx * hw, y1, cz - fz * hw),
    };
    for (int i = 0; i < 4; ++i)
        if (!worldToScreen(quad[i], mvp, origin, screen, &p[i])) return;

    float gyTop = p[0].y, gyBot = p[0].y;
    for (int i = 1; i < 4; ++i) {
        gyTop = std::min(gyTop, p[i].y);
        gyBot = std::max(gyBot, p[i].y);
    }

    // Outline-ESP look: translucent volume fill, then the black-backed
    // themed contour around the projected plane.
    if (doFill) {
        const int vFill = dl->VtxBuffer.Size;
        dl->AddConvexPolyFilled(p, 4, fillColor);
        if (themeFill) tintEspGradient(dl, vFill, gyTop, gyBot, gradSpan, nowMs, fillOp);
    }
    dl->AddPolyline(p, 4, IM_COL32(0, 0, 0, 217), ImDrawFlags_Closed, thickness + 1.5f);
    const int vLine = dl->VtxBuffer.Size;
    dl->AddPolyline(p, 4, color, ImDrawFlags_Closed, thickness);
    if (themeOutline) tintEspGradient(dl, vLine, gyTop, gyBot, gradSpan, nowMs);
}

// 2D mode: vertical filled bar hugging the box's left edge (Rise-style),
// with a black outline like the 2D-Corner brackets carry.
static void drawHpBar2D(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                         float frac, double time, float op) {
    const float h = br.y - tl.y;
    const float s = std::clamp(h / 140.0f, 0.25f, 1.0f);
    const float w = 4.0f * s;               // bar cross-section
    const float gap = 4.0f * s;             // breathing room from the box
    const float outlineT = std::max(1.2f * s, 0.75f);
    const float expand = std::max(1.0f * s, 0.5f);
    const ImVec2 bg(tl.x - gap - w, tl.y), bgEnd(tl.x - gap, br.y);
    // Low-HP flash: alpha breathes ~4 Hz and the bar almost disappears.
    float alpha = 1.0f;
    if (frac < 0.25f) alpha = 0.45f + 0.55f * (float)(0.5 + 0.5 * std::sin(time * 25.0));
    const float fillH = h * frac;
    const ImVec2 fillTop(bg.x, bgEnd.y - (fillH >= 0.5f ? fillH : 0.5f));
    // Track: dark plate with a black outline ring. `op` = HP Opacity setting.
    const int trackA = (int)(130.0f * op); if (trackA <= 0) return;
    dl->AddRectFilled(bg, bgEnd, IM_COL32(0, 0, 0, trackA), 0.0f);
    dl->AddRect(bg, bgEnd, IM_COL32(0, 0, 0, (int)(210.0f * op)), 0.0f, ImDrawFlags_None, outlineT);
    // Fill: colour core over a black expanded underlay (outline).
    if (fillH >= 0.5f) {
        dl->AddRectFilled(ImVec2(fillTop.x - expand, fillTop.y - expand),
                          ImVec2(bgEnd.x + expand, bgEnd.y + expand),
                          IM_COL32(0, 0, 0, (int)(210.0f * op)), 0.0f);
        dl->AddRectFilled(fillTop, bgEnd, hpColor(frac, alpha * op), 0.0f);
    }
}

static void drawHpRing(SDK::Actor* actor, ImDrawList* dl, const float* mvp,
                       const Vec3& origin, const ImVec2& screen, float frac,
                       double time, float op) {
    Vec3 pos = actor->getPos();
    AABB bb = actor->getBoundingBox();
    Vec3 dim = bb.higher - bb.lower;
    if (!(dim.x > 0.05f && dim.x < 6.0f) || !(dim.y > 0.1f && dim.y < 6.0f)) {
        bb.lower = Vec3(pos.x - 0.3f, pos.y, pos.z - 0.3f);
        bb.higher = Vec3(pos.x + 0.3f, pos.y + 1.8f, pos.z + 0.3f);
        dim = Vec3(0.6f, 1.8f, 0.6f);
    }
    Vec3 rp = SDK::interpRenderPos(actor);
    Vec3 delta(rp.x - pos.x, rp.y - pos.y, rp.z - pos.z);
    const float cx = (bb.lower.x + bb.higher.x) * 0.5f + delta.x;
    const float cy = bb.lower.y + delta.y + 0.05f;
    const float cz = (bb.lower.z + bb.higher.z) * 0.5f + delta.z;
    // Outer radius wraps the same footprint the ESP box is built from (half
    // the footprint diagonal + a hair), so the ring hugs the box edges.
    const float radius = 0.5f * std::sqrt(dim.x * dim.x + dim.z * dim.z) + 0.06f;
    const float bandW  = std::clamp(radius * 0.25f, 0.10f, 0.32f);  // fat band
    constexpr int kSegs = 64;

    // Low-HP flash (~4 Hz).
    float alpha = 1.0f;
    if (frac < 0.25f) alpha = 0.40f + 0.60f * (float)(0.5 + 0.5 * std::sin(time * 25.0));

    auto ringPt = [&](float a, float r, ImVec2* out) {
        return worldToScreen(Vec3(cx + std::cos(a) * r, cy, cz + std::sin(a) * r),
                             mvp, origin, screen, out);
    };
    // One flat annulus slice [a0..a1] between inner radius rIn and outer rOut.
    auto bandSeg = [&](float a0, float a1, float rIn, float rOut, ImU32 col) {
        ImVec2 o0, o1, i0, i1;
        if (!(ringPt(a0, rOut, &o0) && ringPt(a1, rOut, &o1) &&
              ringPt(a0, rIn,  &i0) && ringPt(a1, rIn,  &i1))) return;
        ImVec2 q[4] = { i0, o0, o1, i1 };
        dl->AddConvexPolyFilled(q, 4, col);
    };
    // A band swept over [0..sweep]: black outline underlay, then the fat core.
    // `op` (HP Opacity setting) multiplies every band alpha.
    if (op > 0.01f) {
        auto hpBand = [&](float sweep, ImU32 core) {
            const float pad = 0.03f;                 // outline margin, world units
            for (int i = 0; i < kSegs; ++i) {
                const float a0 = (float)i       / kSegs * 6.2831853f;
                float       a1 = (float)(i + 1) / kSegs * 6.2831853f;
                if (a0 >= sweep) break;
                if (a1 > sweep) a1 = sweep;
                bandSeg(a0, a1, radius - bandW - pad, radius + pad, IM_COL32(0, 0, 0, (int)(220.0f * op)));
                bandSeg(a0, a1, radius - bandW,       radius,       core);
            }
        };

        // Track: full dark circle so missing health reads as empty ring space.
        hpBand(6.2831853f, IM_COL32(0, 0, 0, (int)(120.0f * op)));
        // Health arc: sweep proportional to frac.
        hpBand(6.2831853f * frac, hpColor(frac, alpha * op));
    }
}

} // namespace

static int g_espFrame = 0;
static bool g_espResetCache = false;

void ESP::OnEnable() { g_espResetCache = true; }

void ESP::OnRender() {
    if (!IsEnabled()) return;
    // "Always show" off (default) -> only render on the in-world HUD screen.
    // When on, render on every screen as before.
    if (!static_cast<BoolSetting*>(mSettings[9])->mValue &&
        SDK::ScreenView::currentScreenName() != "hud_screen") return;
    bool dbg = (++g_espFrame % 120) == 0; // log roughly every 2s

    SDK::GameContext ctx;
    if (!SDK::resolveContext(ctx)) { if (dbg) LOG_WARN("resolveContext failed", "ESP"); return; }
    SDK::ClientInstance* ci = ctx.ci;
    SDK::Level* level = ctx.level;
    SDK::Actor* lp = ctx.lp;

    // View/Projection MVP + render origin (frameView snapshot + zoom applied).
    float mvp[16];
    Vec3 origin(0, 0, 0);
    if (!acquireCameraMVP(ci, mvp, origin)) { if (dbg) LOG_WARN("acquireCameraMVP failed", "ESP"); return; }

    bool originValid = (origin.x != 0.0f || origin.y != 0.0f || origin.z != 0.0f);
    if (!originValid) return;                 // skip stale passes entirely

    static int s_drawnFrame = -1;
    int frame = ImGui::GetFrameCount();
    if (g_espResetCache) { s_drawnFrame = -1; g_espResetCache = false; }
    if (s_drawnFrame == frame) return;
    s_drawnFrame = frame;

    std::vector<SDK::Actor*> actors;
    if (!sehGetActorList(level, &actors)) { if (dbg) LOG_WARN("getActorList failed", "ESP"); return; }

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 screen = io.DisplaySize;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();  // background list is the one this client flushes (same as Watermark)

    int   mode      = static_cast<EnumSetting*>(mSettings[0])->mValue;   // 0=2D, 1=2D Corner, 2=3D, 3=Outline, 4=2.5D
    int   colMode   = static_cast<EnumSetting*>(mSettings[1])->mValue;   // 0=Theme, 1=Custom
    float* ccol     = static_cast<ColorSetting*>(mSettings[2])->mValue;  // custom outline rgba
    float thickness = static_cast<NumberSetting*>(mSettings[3])->mValue;
    bool  doFill    = static_cast<BoolSetting*>(mSettings[4])->mValue;
    int   fillMode  = static_cast<EnumSetting*>(mSettings[5])->mValue;   // 0=Theme, 1=Custom
    float* fcol     = static_cast<ColorSetting*>(mSettings[6])->mValue;  // custom fill rgba
    float fillOp    = static_cast<NumberSetting*>(mSettings[7])->mValue;

    const uint64_t nowMs = ColorUtils::GetTimeMs();
    const float    gradSpan = espGradSpan();
    ImU32 outline = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.85f));

    int players = 0, projected = 0;
    ImVec2 dbgTl(0,0), dbgBr(0,0); bool dbgHave=false;
    bool showEntities = static_cast<BoolSetting*>(mSettings[8])->mValue;
    const bool renderSelf = static_cast<BoolSetting*>(mSettings[11])->mValue;
    const bool themeMode = static_cast<BoolSetting*>(mSettings[12])->mValue;
    // [13]/[14] HP indicator: vertical bar (2D) or ground ring (Ring).
    const bool hpShow = mSettings.size() > 13 && static_cast<BoolSetting*>(mSettings[13])->mValue;
    const int  hpType  = mSettings.size() > 14 ? static_cast<EnumSetting*>(mSettings[14])->mValue : 1;
    // [15] HP indicator opacity — multiplies every alpha the bar/ring emits.
    const float hpOpacity = mSettings.size() > 15
        ? std::clamp(static_cast<NumberSetting*>(mSettings[15])->mValue, 0.0f, 1.0f) : 1.0f;
    // [16] Flat: size multiplier on the player's projected height (the
    // billboard itself is anchored to the player in the world).
    const float flatScale = mSettings.size() > 16
        ? std::clamp(static_cast<NumberSetting*>(mSettings[16])->mValue, 0.5f, 2.5f) : 1.0f;
    // [17]-[19] Shadow: the gaussian chrome-style halo around the 2D box
    // lines, with its opacity/spread knobs.
    const bool espShadow = mSettings.size() > 17 &&
        static_cast<BoolSetting*>(mSettings[17])->mValue;
    const float shadowOpacity = mSettings.size() > 18
        ? std::clamp(static_cast<NumberSetting*>(mSettings[18])->mValue, 0.05f, 1.0f) : 0.35f;
    const float shadowSpread = mSettings.size() > 19
        ? std::clamp(static_cast<NumberSetting*>(mSettings[19])->mValue, 1.0f, 20.0f) : 9.0f;
    // [20]/[21] Expand: off = the original hitbox size; on = grow the boxes
    // outward by the [21] slider amount (world blocks).
    const bool expandOn = mSettings.size() > 20 &&
        static_cast<BoolSetting*>(mSettings[20])->mValue;
    const float espExpand = (expandOn && mSettings.size() > 21)
        ? std::clamp(static_cast<NumberSetting*>(mSettings[21])->mValue, 0.05f, 0.15f) : 0.0f;
    const double nowT = ImGui::GetTime();

    auto drawActorBox = [&](SDK::Actor* actor) {
        const bool themeOutline = (colMode == 0) || themeMode;
        const bool themeFill    = (fillMode == 0);
        ImU32 color = themeOutline
            ? IM_COL32_WHITE
            : ImGui::ColorConvertFloat4ToU32(ImVec4(ccol[0], ccol[1], ccol[2], ccol[3]));

        // Resolve the translucent fill colour once (Theme gradient or Custom).
        // Themed fills are tinted with alpha = Fill Opacity per vertex.
        ImU32 fillColor = 0;
        if (doFill) {
            fillColor = themeFill
                ? IM_COL32_WHITE
                : ImGui::ColorConvertFloat4ToU32(ImVec4(fcol[0], fcol[1], fcol[2], fillOp));
        }

        if (hpShow && hpOpacity > 0.0f) {
            auto hp = actor->getHealth();
            if (hp && *hp > 0.0f) {
                const float frac = std::clamp(*hp / 20.0f, 0.0f, 1.0f);
                if (hpType == 0) {
                    ImVec2 hpTl, hpBr;
                    // The 2D bar hugs the player's true projected box in every
                    // mode (Flat's crossed strips have no single 2D rect).
                    if (projectActorBox(actor, mvp, origin, screen, espExpand, &hpTl, &hpBr))
                        drawHpBar2D(dl, hpTl, hpBr, frac, nowT, hpOpacity);
                } else {
                    drawHpRing(actor, dl, mvp, origin, screen, frac, nowT, hpOpacity);
                }
            }
        }

        if (mode == 4) {
            drawFlatPlane(actor, dl, mvp, origin, screen, flatScale,
                          doFill, themeFill, fillColor, fillOp,
                          themeOutline, color, thickness, gradSpan, nowMs);
            ++projected;
            if (!dbgHave) { dbgTl = ImVec2(0, 0); dbgBr = screen; dbgHave = true; }
        } else if (mode == 0 || mode == 1) {
            ImVec2 tl, br;
            if (!projectActorBox(actor, mvp, origin, screen, espExpand, &tl, &br)) {
                return;
            }
            ++projected;
            if (!dbgHave) { dbgTl=tl; dbgBr=br; dbgHave=true; }

            if (mode == 0) {
                // Plain rectangle: 2D projects the true box.
                if (espShadow) espRectShadow(dl, tl, br, shadowOpacity, shadowSpread);
                if (doFill) {
                    // Themed fill: emit flat white, then tint the quad's vertices
                    // with the vertical gradient at Fill Opacity.
                    const int vFill = dl->VtxBuffer.Size;
                    dl->AddRectFilled(tl, br, fillColor, 0.0f);
                    if (themeFill) tintEspGradient(dl, vFill, tl.y, br.y, gradSpan, nowMs, fillOp);
                }
                dl->AddRect(tl, br, outline, 0.0f, 0, thickness + 1.5f); // flat black backing
                const int vLine = dl->VtxBuffer.Size;
                dl->AddRect(tl, br, color,   0.0f, 0, thickness);
                // Vertical theme gradient down the box: top edge = theme colour at
                // the current palette position, bottom edge = a quarter cycle later.
                if (themeOutline) tintEspGradient(dl, vLine, tl.y, br.y, gradSpan, nowMs);
            } else {
                const float x = tl.x, y = tl.y, z = br.x, w = br.y;
                const float bw = z - x, bh = w - y;
                const float ct  = std::clamp(thickness, 0.5f, 6.0f);
                auto armLen = [ct](float extent) {
                    const float half = extent * 0.5f;
                    return std::clamp(extent * 0.25f, std::min(ct * 2.0f, half), half);
                };
                const float ax  = armLen(bw);   // horizontal arm length
                const float ay  = armLen(bh);   // vertical arm length
                const float pad = 1.0f;         // black underlay margin on each side
                struct Arm { float x0, y0, x1, y1; };
                const Arm arms[8] = {
                    { x,      y,          x + ax, y + ct }, // top-left:    horizontal
                    { x,      y,          x + ct, y + ay }, // top-left:    vertical
                    { z - ax, y,          z,      y + ct }, // top-right:   horizontal
                    { z - ct, y,          z,      y + ay }, // top-right:   vertical
                    { x,      w - ct,     x + ax, w      }, // bottom-left: horizontal
                    { x,      w - ay,     x + ct, w      }, // bottom-left: vertical
                    { z - ax, w - ct,     z,      w      }, // bottom-right: horizontal
                    { z - ct, w - ay,     z,      w      }, // bottom-right: vertical
                };
                for (const Arm& a : arms) {
                    if (espShadow) {
                        ImRenderUtils::fillShadowRectangle(
                            ImVec4(a.x0 - pad, a.y0 - pad, a.x1 + pad, a.y1 + pad),
                            ImColor(0, 0, 0), shadowOpacity, 0.0f,
                            shadowSpread * 0.6f, 0.9f, dl);
                    }
                    // Underlay first, then its color arm — same rect geometry.
                    dl->AddRectFilled(ImVec2(a.x0 - pad, a.y0 - pad),
                                      ImVec2(a.x1 + pad, a.y1 + pad), outline, 0.0f);
                    const int vArm = dl->VtxBuffer.Size;
                    dl->AddRectFilled(ImVec2(a.x0, a.y0), ImVec2(a.x1, a.y1), color, 0.0f);
                    if (themeOutline) tintEspGradient(dl, vArm, y, w, gradSpan, nowMs);
                }
            }
        } else {
            // ---- 3D (mode 2) and Outline (mode 3): project all 8 cube
            //      corners. (Flat has its own crossed-strip branch above.) ----
            ImVec2 p[8]; bool vb[8];
            if (projectActorCorners(actor, mvp, origin, screen, espExpand, p, vb) < 2) return;
            ++projected;

            // Vertical extent of the projected box — the axis of the theme
            // gradient, the 3D equivalent of the 2D rect's tl.y .. br.y.
            float gyTop = 1e30f, gyBot = -1e30f;
            for (int i = 0; i < 8; ++i) {
                if (!vb[i]) continue;
                if (p[i].y < gyTop) gyTop = p[i].y;
                if (p[i].y > gyBot) gyBot = p[i].y;
            }

            ImVec2 hullPts[18]; int hullN = 0;
            if (mode == 3 || doFill) {
                ImVec2 valids[8]; int nv = 0;
                for (int i = 0; i < 8; ++i) if (vb[i]) valids[nv++] = p[i];
                hullN = SDK::BoxOverlay::convexHull2D(valids, nv, hullPts);
            }
            if (doFill) {
                if (mode == 3) {
                    if (hullN >= 3) {
                        const int vFill = dl->VtxBuffer.Size;
                        dl->AddConvexPolyFilled(hullPts, hullN, fillColor);
                        if (themeFill) tintEspGradient(dl, vFill, gyTop, gyBot, gradSpan, nowMs, fillOp);
                    }
                } else {
                    for (int f = 0; f < 6; ++f) {
                        const int* q = SDK::BoxOverlay::kCubeFaces[f];
                        if (!(vb[q[0]] && vb[q[1]] && vb[q[2]] && vb[q[3]])) continue;
                        ImVec2 quad[4] = { p[q[0]], p[q[1]], p[q[2]], p[q[3]] };
                        const int vFace = dl->VtxBuffer.Size;
                        dl->AddConvexPolyFilled(quad, 4, fillColor);
                        if (themeFill) tintEspGradient(dl, vFace, gyTop, gyBot, gradSpan, nowMs, fillOp);
                    }
                }
            }

            if (mode == 2) {
                auto cube = [&](ImU32 c, float th, bool themed){
                    for (int i = 0; i < 12; ++i) {
                        int a = SDK::BoxOverlay::kCubeEdges[i][0], b = SDK::BoxOverlay::kCubeEdges[i][1];
                        if (!(vb[a] && vb[b])) continue;
                        const int vEdge = dl->VtxBuffer.Size;
                        dl->AddLine(p[a], p[b], c, th);
                        if (themed) tintEspGradient(dl, vEdge, gyTop, gyBot, gradSpan, nowMs);
                    }
                };
                cube(outline, thickness + 1.5f, false);   // flat black backing
                cube(color,   thickness,      themeOutline);
            } else {
                if (hullN >= 2) {
                    dl->AddPolyline(hullPts, hullN, outline, ImDrawFlags_Closed, thickness + 1.5f);
                    const int vHull = dl->VtxBuffer.Size;
                    dl->AddPolyline(hullPts, hullN, color,   ImDrawFlags_Closed, thickness);
                    if (themeOutline) tintEspGradient(dl, vHull, gyTop, gyBot, gradSpan, nowMs);
                }
            }
        }

    };

    // World actors (local player always excluded here).
    for (SDK::Actor* actor : actors) {
        if (!actor || actor == lp) continue;
        // Players are always shown; non-player entities only when "Entities" is on.
        if (!sehIsPlayer(actor) && !(showEntities && sdk_safe_ptr(actor, 0x230))) continue;
        ++players;
        drawActorBox(actor);
    }

    if (renderSelf && lp && GameHooks::LastPerspective() != 0)
        drawActorBox(lp);

    if (dbg) LOG_INFO("ESP summary: actors=" + std::to_string(actors.size()) + " players=" + std::to_string(players) + " projected=" + std::to_string(projected) + " screen=" + std::to_string(screen.x) + "x" + std::to_string(screen.y) + " origin=" + std::to_string(origin.x) + "," + std::to_string(origin.y) + "," + std::to_string(origin.z), "ESP");
}

