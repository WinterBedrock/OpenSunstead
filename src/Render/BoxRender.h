#pragma once
// Sunstead SDK — world-space box drawing.
//
// FreeCam, OreMiner, BlockESP, Baritone and ESP each spelled out the same
// 8-corner array plus a static edges[12][2] table and the same projection loop.
// One implementation here; callers pass colours and thickness.

#include "../SDK/Types.h"
#include "FrameView.h"
#include <imgui.h>

namespace SDK {

// Project the eight corners of [mn,mx]. `pts` receives pixel positions and `ok`
// which of them landed in front of the camera. Returns the number projected.
inline int projectBoxCorners(const Vec3& mn, const Vec3& mx, const float* mvp,
                             const Vec3& origin, const ImVec2& screen,
                             ImVec2 pts[8], bool ok[8]) {
    const Vec3 corners[8] = {
        {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z},
        {mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z},
        {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z},
        {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z},
    };
    int valid = 0;
    for (int i = 0; i < 8; ++i) {
        ok[i] = worldToScreen(corners[i], mvp, origin, screen, &pts[i]);
        if (ok[i]) ++valid;
    }
    return valid;
}

// Bottom face 0-1-2-3, top face 4-5-6-7, verticals 0-4 1-5 2-6 3-7.
inline const int (*boxEdges())[2] {
    static const int e[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7},
    };
    return e;
}

// Wireframe box, optionally with a filled bottom face. `fill` of 0 skips it.
// Draws nothing when fewer than two corners project (a box entirely behind the
// camera would otherwise streak lines across the screen).
inline void drawBoxWireframe(ImDrawList* dl, const Vec3& mn, const Vec3& mx,
                             const float* mvp, const Vec3& origin,
                             const ImVec2& screen, ImU32 col,
                             float thickness = 1.5f, ImU32 fill = 0) {
    if (!dl) return;
    ImVec2 pts[8];
    bool ok[8];
    if (projectBoxCorners(mn, mx, mvp, origin, screen, pts, ok) < 2) return;

    auto edges = boxEdges();
    for (int i = 0; i < 12; ++i) {
        const int a = edges[i][0], b = edges[i][1];
        if (ok[a] && ok[b]) dl->AddLine(pts[a], pts[b], col, thickness);
    }
    if (fill && ok[0] && ok[1] && ok[2] && ok[3]) {
        ImVec2 quad[4] = { pts[0], pts[1], pts[2], pts[3] };
        dl->AddConvexPolyFilled(quad, 4, fill);
    }
}

} // namespace SDK
