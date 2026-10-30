#pragma once
// SDK box-overlay primitives: canonical cube corner layout (bottom ring 0..3,
// top ring 4..7), face/edge index tables, corner and rect projection,
// convex-hull silhouette, face/edge drawing — pure geometry over an explicit
// (mn, mx) box. Policy (degenerate-box rejection, clamping, colors, thickness)
// stays with the caller.
#include "../Types.h"
#include <imgui.h>

namespace SDK {
namespace BoxOverlay {

// Faces of the canonical cube (bottom 0..3, top 4..7).
inline constexpr int kCubeFaces[6][4] = {
    {0, 1, 2, 3},   // bottom
    {4, 5, 6, 7},   // top
    {0, 1, 5, 4},   // -Z side
    {1, 2, 6, 5},   // +X side
    {2, 3, 7, 6},   // +Z side
    {3, 0, 4, 7},   // -X side
};

// The 12 cube edges: bottom ring, top ring, uprights.
inline constexpr int kCubeEdges[12][2] = {
    {0,1},{1,2},{2,3},{3,0},
    {4,5},{5,6},{6,7},{7,4},
    {0,4},{1,5},{2,6},{3,7},
};

// Canonical 8 corners of (mn, mx): bottom ring 0..3, top ring 4..7.
inline void cubeCorners(const Vec3& mn, const Vec3& mx, Vec3 out[8]) {
    out[0] = Vec3(mn.x, mn.y, mn.z); out[1] = Vec3(mx.x, mn.y, mn.z);
    out[2] = Vec3(mx.x, mn.y, mx.z); out[3] = Vec3(mn.x, mn.y, mx.z);
    out[4] = Vec3(mn.x, mx.y, mn.z); out[5] = Vec3(mx.x, mx.y, mn.z);
    out[6] = Vec3(mx.x, mx.y, mx.z); out[7] = Vec3(mn.x, mx.y, mx.z);
}

// Project all 8 corners. A corner behind the near plane is skipped
// (valid[i] = false) instead of rejecting the whole box, so close boxes keep
// their visible part. Returns the number of projectable corners.
inline int projectBoxCorners(const Vec3& mn, const Vec3& mx, const float* mvp,
                              const Vec3& origin, const ImVec2& screen,
                              ImVec2 pts[8], bool valid[8]) {
    Vec3 c[8];
    cubeCorners(mn, mx, c);
    int vis = 0;
    for (int i = 0; i < 8; ++i) {
        valid[i] = worldToScreen(c[i], mvp, origin, screen, &pts[i]);
        if (valid[i]) ++vis;
    }
    return vis;
}

// Screen-space bounding rect of the projected box. Raw min/max (no clamping,
// no degenerate policy — the caller applies its own). Returns the number of
// projectable corners; tl/br untouched when it is 0.
inline int projectBoxRect(const Vec3& mn, const Vec3& mx, const float* mvp,
                           const Vec3& origin, const ImVec2& screen,
                           ImVec2* tl, ImVec2* br) {
    ImVec2 p[8];
    bool v[8];
    const int vis = projectBoxCorners(mn, mx, mvp, origin, screen, p, v);
    if (vis == 0) return 0;
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (int i = 0; i < 8; ++i) {
        if (!v[i]) continue;
        if (p[i].x < minX) minX = p[i].x;
        if (p[i].y < minY) minY = p[i].y;
        if (p[i].x > maxX) maxX = p[i].x;
        if (p[i].y > maxY) maxY = p[i].y;
    }
    *tl = ImVec2(minX, minY);
    *br = ImVec2(maxX, maxY);
    return vis;
}

// Convex hull (Andrew's monotone chain, collinear points dropped) of up to 8
// projected points: the box silhouette. hull must hold 8+ points; returns
// the ordered-loop count (no duplicate end point).
inline int convexHull2D(const ImVec2* in, int n, ImVec2* hull) {
    if (n < 3) { for (int i = 0; i < n; ++i) hull[i] = in[i]; return n; }
    ImVec2 pts[8];
    int m = (n > 8) ? 8 : n;
    for (int i = 0; i < m; ++i) pts[i] = in[i];
    for (int i = 1; i < m; ++i) {
        ImVec2 key = pts[i]; int j = i - 1;
        while (j >= 0 && (pts[j].x > key.x || (pts[j].x == key.x && pts[j].y > key.y))) {
            pts[j + 1] = pts[j]; --j;
        }
        pts[j + 1] = key;
    }
    auto cross = [](const ImVec2& O, const ImVec2& A, const ImVec2& B) {
        return (A.x - O.x) * (B.y - O.y) - (A.y - O.y) * (B.x - O.x);
    };
    ImVec2 h[18]; int k = 0;
    for (int i = 0; i < m; ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    const int lower = k + 1;
    for (int i = m - 2; i >= 0; --i) {
        while (k >= lower && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    const int count = k - 1;
    for (int i = 0; i < count; ++i) hull[i] = h[i];
    return count < 0 ? 0 : count;
}

// Fill every fully-visible cube face with a translucent color.
inline void drawCubeFaces(ImDrawList* dl, const ImVec2 pts[8], const bool valid[8],
                          ImU32 fill) {
    if (!dl) return;
    for (int f = 0; f < 6; ++f) {
        const int* q = kCubeFaces[f];
        if (!(valid[q[0]] && valid[q[1]] && valid[q[2]] && valid[q[3]])) continue;
        const ImVec2 quad[4] = { pts[q[0]], pts[q[1]], pts[q[2]], pts[q[3]] };
        dl->AddConvexPolyFilled(quad, 4, fill);
    }
}

// Stroke every fully-visible cube edge.
inline void drawCubeEdges(ImDrawList* dl, const ImVec2 pts[8], const bool valid[8],
                          ImU32 color, float thickness) {
    if (!dl) return;
    for (int e = 0; e < 12; ++e) {
        const int a = kCubeEdges[e][0], b = kCubeEdges[e][1];
        if (!(valid[a] && valid[b])) continue;
        dl->AddLine(pts[a], pts[b], color, thickness);
    }
}

} // namespace BoxOverlay
} // namespace SDK
