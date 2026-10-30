#include "Tracers.h"
#include "../../Render/FrameView.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/Render/BoxOverlay.h"
#include "../Setting.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include <imgui.h>
#include <vector>
#include <Windows.h>
#include <unordered_map>
#include <cmath>
#include <algorithm>

namespace {

// ---- Robust context + enumeration (mirrors HitBoxes' SEH-guarded helpers) ----
// sehGetActorList / sehIsPlayer are consolidated in SDK::GameContext (ADL).

// Context resolution is SDK::resolveContext (SDK/Client/GameContext.h).

// Column-major 4x4 multiply C = A * B  (glm/game layout: m[col*4 + row]).
// mat4Mul / worldToScreen are consolidated in SDK/Math.h (mat4Multiply,
// worldToScreen; both global, resolved via ADL).

// Project one player's AABB to a 2D screen rect. Returns false if not visible.
static bool projectActorBox(SDK::Actor* actor, const float* mvp, const Vec3& origin,
                            const ImVec2& screen, ImVec2* tl, ImVec2* br) {
    // Build the world-space box the Latite/Hitboxes way: take the box DIMENSIONS
    // from the AABB component but POSITION it from the reliable state-vector
    // position (getPos). The raw AABB min/max can be stale or in a shifted
    // frame, which made boxes land off the actual player. Rebasing onto getPos
    // guarantees the box sits where the player actually is.
    Vec3 feet;
    AABB bb;
    __try {
        feet = actor->getPos();
        bb = actor->getBoundingBox();           // raw lower/higher (for size)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    Vec3 dim = bb.higher - bb.lower;            // width/height/depth
    // Sanity: fall back to a default player size if the AABB looks bogus.
    if (!(dim.x > 0.05f && dim.x < 6.0f) || !(dim.y > 0.1f && dim.y < 6.0f)) {
        dim = Vec3(0.6f, 1.8f, 0.6f);
    }
    // Recentre the box on the feet position (feet = bottom-centre in world).
// getPos() is at EYE height, not the feet вЂ” using it as the box bottom made the
    // box float ABOVE the player. bb.lower.y is the true world-space foot level,
    // so anchor vertically there while keeping the reliable horizontal centre
    // from getPos().x/z.
    float footY = bb.lower.y;
    if (!(footY < feet.y + 0.5f) || footY < feet.y - 4.0f) footY = feet.y - dim.y; // guard odd AABB
    Vec3 mn(feet.x - dim.x * 0.5f, footY,         feet.z - dim.z * 0.5f);
    Vec3 mx(feet.x + dim.x * 0.5f, footY + dim.y, feet.z + dim.z * 0.5f);

    // Projection loop is shared (BoxOverlay); the clamp + degenerate policy
    // below is this module's own.
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


// Project to screen even for points BEHIND the camera. For behind points we
// mirror the clip coords and clamp the result to the screen border, so a tracer
// still points toward off-screen / behind-you players. Always returns a point.
static void worldToScreenBehind(const Vec3& world, const float* mvp, const Vec3& origin,
                                const ImVec2& screen, ImVec2* out) {
    float px = world.x - origin.x, py = world.y - origin.y, pz = world.z - origin.z;
    float cx = mvp[0]*px + mvp[4]*py + mvp[8]*pz  + mvp[12];
    float cy = mvp[1]*px + mvp[5]*py + mvp[9]*pz  + mvp[13];
    float cw = mvp[3]*px + mvp[7]*py + mvp[11]*pz + mvp[15];
    bool behind = (cw < 0.1f);
    if (!behind) {
        float inv = 1.0f / cw;
        float sx = (cx * inv + 1.0f) * 0.5f * screen.x;
        float sy = (1.0f - cy * inv) * 0.5f * screen.y;
        if (sx < 0) sx = 0; if (sx > screen.x) sx = screen.x;
        if (sy < 0) sy = 0; if (sy > screen.y) sy = screen.y;
        out->x = sx; out->y = sy;
        return;
    }
    // BEHIND the camera: vertical projection is meaningless (it inverts), so by
    // convention a player behind you resolves to the BOTTOM of the screen. Only
    // the horizontal (left/right) component is kept, mirrored because they're
    // behind you. So a player directly behind -> bottom-centre (points down).
    // Behind the camera the perspective divide flips left<->right (negative cw).
    // Negate ONLY cw so the ratio cx/cw is mirrored back to the correct side;
    // negating both cx and cw (as before) cancelled out and left it reversed.
    cw = -cw; if (cw < 0.0001f) cw = 0.0001f;
    float sx = (cx / cw + 1.0f) * 0.5f * screen.x;
    if (sx < 0) sx = 0; if (sx > screen.x) sx = screen.x;
    out->x = sx;
    out->y = screen.y - 1.0f;   // bottom edge
}

} // namespace



// SEH cannot live in a function that also has C++ objects with destructors
// (C2712). Isolate the protected reads in their own helper.
// pos + bounding box in one guarded read: SDK::sehGetPosBox (GameContext.h).

void Tracers::OnRender() {
    if (!IsEnabled()) return;
    // "Always show" off (default) -> only render on the in-world HUD screen.
    // When on, render on every screen as before.
    if (!static_cast<BoolSetting*>(mSettings[6])->mValue &&
        SDK::ScreenView::currentScreenName() != "hud_screen") return;

    SDK::ClientInstance* ci = nullptr; SDK::Level* level = nullptr; SDK::Actor* lp = nullptr;
    if (!SDK::resolveContext(&ci, &level, &lp)) return;

    // Camera MVP via the shared delayed-transform acquisition (FrameView.h).
    float mvp[16];
    Vec3 origin(0, 0, 0);
    if (!acquireCameraMVP(ci, mvp, origin)) return;

    // Same valid-frame cache trick as ESP (matrices/origin only valid in-world).
    static float s_mvp[16] = {0}; static Vec3 s_origin(0, 0, 0); static bool s_have = false;
    bool ov = (origin.x != 0.0f || origin.y != 0.0f || origin.z != 0.0f);
    if (ov) { memcpy(s_mvp, mvp, sizeof(mvp)); s_origin = origin; s_have = true; }
    else if (s_have) { memcpy(mvp, s_mvp, sizeof(mvp)); origin = s_origin; }

    std::vector<SDK::Actor*> actors;
    if (!sehGetActorList(level, &actors)) return;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 screen = io.DisplaySize;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    int   mode      = static_cast<EnumSetting*>(mSettings[0])->mValue;   // 0 lines, 1 arrows
    int   anchor    = static_cast<EnumSetting*>(mSettings[1])->mValue;   // 0 top,1 center,2 bottom
    int   colMode   = static_cast<EnumSetting*>(mSettings[2])->mValue;   // 0 theme,1 custom
    float* ccol     = static_cast<ColorSetting*>(mSettings[3])->mValue;
    float thickness = static_cast<NumberSetting*>(mSettings[4])->mValue;
    bool  behindOn  = static_cast<BoolSetting*>(mSettings[5])->mValue;
    bool  showMobs  = static_cast<BoolSetting*>(mSettings[7])->mValue;   // [6] is Always show
    uint64_t nowMs  = GetTickCount64();

    // Arrows mode draws small triangles in a ring around the screen centre,
    // each pointing at its target — an off-screen radar, not lines.
    const ImVec2 center2(screen.x * 0.5f, screen.y * 0.5f);
    const float ringR = std::min(screen.x, screen.y) * 0.16f;
    const float tri = 9.0f;               // arrow size

    // Lines mode anchor.
    const ImVec2 from(screen.x * 0.5f,
                      anchor == 0 ? 0.0f : (anchor == 1 ? screen.y * 0.5f : screen.y));

    for (SDK::Actor* a : actors) {
        if (!a || a == lp) continue;
        // Players always; mobs/animals only with the Mobs option on.
        if (!sehIsPlayer(a) && !(showMobs && sdk_safe_ptr(a, 0x230))) continue;

        Vec3 posNow; AABB bb;
        if (!SDK::sehGetPosBox(a, &posNow, &bb)) continue;

        // Endpoint: the position the presented frame's model was drawn at —
        // push-time pair + served push's alpha (same instant as the served
        // camera). The old scheme read the raw sim pos (steps at tick rate)
        // and ran an exponential smoother over it — the smoother LAGGED the
        // model, so while a player moved the line end visibly trailed and
        // snapped (the "teleporting" jitter).
        Vec3 rp = SDK::interpRenderPos(a);
        if (rp.x == 0.0f && rp.y == 0.0f && rp.z == 0.0f) continue;
        Vec3 dim = bb.higher - bb.lower;
        if (!(dim.y > 0.1f && dim.y < 6.0f)) dim = Vec3(0.6f, 1.8f, 0.6f);
        // Vertical anchor: foot level from the AABB, rebased onto the render
        // position's Y (getPos carries EYE height, not feet).
        float footY = bb.lower.y + (rp.y - posNow.y);
        if (footY > rp.y + 0.5f || footY < rp.y - 4.0f) footY = rp.y - dim.y;
        Vec3 center(rp.x, footY + dim.y * 0.5f, rp.z);

        ImU32 color;
        if (colMode == 0) color = (ImU32)ColorUtils::getThemedColor(0.0f, nowMs);
        else color = ImGui::ColorConvertFloat4ToU32(ImVec4(ccol[0], ccol[1], ccol[2], ccol[3]));

        if (mode == 1) {
            // Arrow: direction = centre -> the target's projected point (for
            // behind/off-screen players the direction uses the camera-space
            // vector directly, so the ring always points truthfully).
            float ang = 0.0f;
            bool haveAng = false;
            ImVec2 to;
            if (behindOn) {
                worldToScreenBehind(center, mvp, origin, screen, &to);
                float dx = to.x - center2.x;
                float dy = to.y - center2.y;
                float len = std::sqrt(dx * dx + dy * dy);
                if (len > 0.5f) { ang = std::atan2(dy, dx); haveAng = true; }
            } else {
                ImVec2 to2;
                if (worldToScreen(center, mvp, origin, screen, &to2)) {
                    float dx = to2.x - center2.x;
                    float dy = to2.y - center2.y;
                    float len = std::sqrt(dx * dx + dy * dy);
                    if (len > 0.5f) { ang = std::atan2(dy, dx); haveAng = true; }
                }
            }
            if (!haveAng) continue;
            const float ca = std::cos(ang), sa = std::sin(ang);
            const ImVec2 tip(center2.x + ca * (ringR + tri), center2.y + sa * (ringR + tri));
            const ImVec2 l(center2.x + ca * (ringR - tri * 0.5f) - sa * tri * 0.6f,
                            center2.y + sa * (ringR - tri * 0.5f) + ca * tri * 0.6f);
            const ImVec2 r(center2.x + ca * (ringR - tri * 0.5f) + sa * tri * 0.6f,
                            center2.y + sa * (ringR - tri * 0.5f) - ca * tri * 0.6f);
            dl->AddTriangleFilled(tip, l, r, color);
            continue;
        }

        ImVec2 to;
        if (behindOn) {
            worldToScreenBehind(center, mvp, origin, screen, &to); // always draws (edge-clamps behind)
        } else {
            if (!worldToScreen(center, mvp, origin, screen, &to)) continue;
        }

        dl->AddLine(from, to, color, thickness);
    }
}