#include "BlockESP.h"
#include "../../Core/Logger.h"
#include "../../Render/FrameView.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/World/BlockScan.h"
#include "../../SDK/Render/BoxOverlay.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../Setting.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <chrono>
#include <mutex>
#include <unordered_set>
#include <string>

// Block classification, colours, block-box projection and the exposed-to-air
// filter now live in the SDK (SDK::BlockScan) — shared with Xray, so a new
// ore or renamed block is fixed once for every block overlay.
using namespace SDK::BlockScan;

namespace {

static bool kindEnabled(const BlockESP* self, int kind) {
    switch (kind) {
        case Kind_Diamond:  return static_cast<BoolSetting*>(self->mSettings[3])->mValue;
        case Kind_Emerald:  return static_cast<BoolSetting*>(self->mSettings[4])->mValue;
        case Kind_Gold:     return static_cast<BoolSetting*>(self->mSettings[5])->mValue;
        case Kind_Iron:     return static_cast<BoolSetting*>(self->mSettings[6])->mValue;
        case Kind_Coal:     return static_cast<BoolSetting*>(self->mSettings[7])->mValue;
        case Kind_Redstone: return static_cast<BoolSetting*>(self->mSettings[8])->mValue;
        case Kind_Lapis:    return static_cast<BoolSetting*>(self->mSettings[9])->mValue;
        case Kind_Portal:   return static_cast<BoolSetting*>(self->mSettings[10])->mValue;
        case Kind_Chest:    return static_cast<BoolSetting*>(self->mSettings[11])->mValue;
        case Kind_Spawner:  return static_cast<BoolSetting*>(self->mSettings[12])->mValue;
        case Kind_Debris:   return static_cast<BoolSetting*>(self->mSettings[18])->mValue;
        case Kind_Shulker:  return static_cast<BoolSetting*>(self->mSettings[20])->mValue;
        default: return false;
    }
}

static bool readActorPos(SDK::Actor* actor, Vec3* out) {
    if (!actor || !out) return false;
    __try {
        *out = actor->getPos();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static uint64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static bool debugEnabled(const BlockESP* self) {
    return self && self->mSettings.size() > 15 && static_cast<BoolSetting*>(self->mSettings[15])->mValue;
}

static void debugLogThrottled(const std::string& msg) {
    static uint64_t last = 0;
    uint64_t now = nowMs();
    if (now - last < 1000) return;
    last = now;
    LOG_INFO(msg, "BlockESP");
}

// 8-point silhouette hull now lives in the shared SDK (BoxOverlay); the
// arbitrary-size cluster hull below stays local (different purpose).


// Convex hull (Andrew's monotone chain, same approach as the per-cube hull in
// Xray.cpp's Outline mode) over an ARBITRARY point set: all projected corners
// of every block in one kind-group -> a single silhouette polygon around the
// whole cluster (classic "block cluster outline" ESP look). Sorts a copy of
// pts and fills hull (ordered closed loop, no duplicate end point). Returns
// the hull point count. File-local + distinctly named (ESP.cpp/Xray.cpp have
// their own copies) so no ODR clash.
static int blockEspGroupHull(std::vector<ImVec2>& pts, std::vector<ImVec2>& hull) {
    hull.clear();
    if (pts.size() < 3) return 0;
    std::sort(pts.begin(), pts.end(), [](const ImVec2& A, const ImVec2& B) {
        return A.x < B.x || (A.x == B.x && A.y < B.y);
    });
    auto cross = [](const ImVec2& O, const ImVec2& A, const ImVec2& B) {
        return (A.x - O.x) * (B.y - O.y) - (A.y - O.y) * (B.x - O.x);
    };
    for (const ImVec2& q : pts) {   // lower hull
        while (hull.size() >= 2 && cross(hull[hull.size() - 2], hull[hull.size() - 1], q) <= 0) hull.pop_back();
        hull.push_back(q);
    }
    size_t lower = hull.size() + 1;
    for (int i = (int)pts.size() - 2; i >= 0; --i) {   // upper hull
        while (hull.size() >= lower && cross(hull[hull.size() - 2], hull[hull.size() - 1], pts[i]) <= 0) hull.pop_back();
        hull.push_back(pts[i]);
    }
    if (!hull.empty()) hull.pop_back();   // last point == first
    return (int)hull.size();
}

// Convex hull over a plain array (the cluster-wide fill shape). Same
// monotone-chain as blockEspGroupHull above but taking raw points.
static int blockEspGroupHull2D(ImVec2* pts, int n, ImVec2* hull) {
    if (n < 3) return 0;
    std::sort(pts, pts + n, [](const ImVec2& A, const ImVec2& B) {
        return A.x < B.x || (A.x == B.x && A.y < B.y);
    });
    auto cross = [](const ImVec2& O, const ImVec2& A, const ImVec2& B) {
        return (A.x - O.x) * (B.y - O.y) - (A.y - O.y) * (B.x - O.x);
    };
    int k = 0;
    for (int i = 0; i < n; ++i) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0) --k;
        hull[k++] = pts[i];
    }
    const int lower = k + 1;
    for (int i = n - 2; i >= 0; --i) {
        while (k >= lower && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0) --k;
        hull[k++] = pts[i];
    }
    if (k > 0) --k;
    return k;
}

} // namespace

void BlockESP::resetScan(const SDK::BlockPos& center, int radius) {
    mCenter = center;
    mRadius = radius;
    mCursor = 0;
    // Restart the in-progress pass, but DELIBERATELY keep mFoundBlocks intact:
    // the last completed set keeps rendering until the new pass finishes and
    // swaps in. Clearing it here would blank every box for a few ticks on every
    // recenter (movement/teleport) -> flicker. mScanBuffer + cache are
    // tick-thread only, so no lock is needed for them.
    mScanBuffer.clear();
    mLegacyKindCache.clear();
}

void BlockESP::OnEnable() {
    std::lock_guard<std::mutex> lock(mBlocksMutex);
    mFoundBlocks.clear();
    mScanBuffer.clear();
    mLegacyKindCache.clear();
    mCursor = 0;
}

void BlockESP::OnDisable() {
    std::lock_guard<std::mutex> lock(mBlocksMutex);
    mFoundBlocks.clear();
    mScanBuffer.clear();
    mLegacyKindCache.clear();
    mCursor = 0;
}

void BlockESP::OnTick() {
    if (!IsEnabled()) return;
    bool dbg = debugEnabled(this);
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, 0x200)) {
        if (dbg) debugLogThrottled("tick: ClientInstance invalid");
        return;
    }
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp || !sdk_safe_ptr(lp, 0x230)) {
        if (dbg) debugLogThrottled("tick: LocalPlayer invalid");
        return;
    }
    SDK::BlockSource* src = ci->getBlockSource();
    if (!src) {
        if (dbg) debugLogThrottled("tick: BlockSource null");
        return;
    }

    Vec3 pos;
    if (!readActorPos(lp, &pos)) {
        if (dbg) debugLogThrottled("tick: readActorPos failed");
        return;
    }
    int radius = (int)static_cast<NumberSetting*>(mSettings[1])->mValue;
    if (radius < 4) radius = 4;
    SDK::BlockPos center((int)std::floor(pos.x), (int)std::floor(pos.y), (int)std::floor(pos.z));
    if (radius != mRadius ||
        std::abs(center.x - mCenter.x) > 8 || std::abs(center.y - mCenter.y) > 8 || std::abs(center.z - mCenter.z) > 8) {
        resetScan(center, radius);
    }

    int side = radius * 2 + 1;
    int total = side * side * side;
    int budget = (int)static_cast<NumberSetting*>(mSettings[2])->mValue;
    if (budget < 256) budget = 256;
    bool onlyExposed = static_cast<BoolSetting*>(mSettings[13])->mValue;
    int r2 = radius * radius;
    int scanned = 0, gotBlock = 0, gotLegacy = 0, classified = 0, enabled = 0, exposedRejected = 0, written = 0;

    // Progressive, double-buffered scan. We accumulate every match of the
    // current pass into mScanBuffer (tick-thread only, no lock). Only when the
    // pass completes do we atomically swap it into mFoundBlocks under the lock.
    // The render thread therefore always sees a COMPLETE result set and never a
    // half-refilled one -> the boxes stay solid instead of flickering as the
    // budget-limited scan walks the volume across multiple ticks.
    for (int i = 0; i < budget && total > 0; ++i) {
        if (mCursor == 0) mScanBuffer.clear();   // begin a fresh pass
        int cur = mCursor++;
        bool passDone = (mCursor >= total);

        int lx = cur % side;
        int ly = (cur / side) % side;
        int lz = cur / (side * side);
        int dx = lx - radius, dy = ly - radius, dz = lz - radius;
        if (dx * dx + dy * dy + dz * dz <= r2) {
            ++scanned;
            SDK::BlockPos bp(center.x + dx, center.y + dy, center.z + dz);
            SDK::Block* block = src->getBlock(bp);
            if (block) ++gotBlock;
            SDK::BlockLegacy* legacy = block ? block->getLegacy() : nullptr;
            if (legacy) {
                ++gotLegacy;
                int kind = Kind_None;
                auto it = mLegacyKindCache.find(legacy);
                if (it != mLegacyKindCache.end()) {
                    kind = it->second;
                } else {
                    kind = SDK::BlockScan::classify(legacy);
                    mLegacyKindCache[legacy] = kind;
                }
                if (kind != Kind_None) ++classified;

                bool isEnabledKind = kindEnabled(this, kind);
                if (isEnabledKind) ++enabled;
                bool exposedOk = !onlyExposed || SDK::BlockScan::exposedToAir(src, bp);
                if (isEnabledKind && !exposedOk) ++exposedRejected;
                if (isEnabledKind && exposedOk) {
                    mScanBuffer[bp] = { bp, SDK::BlockScan::colorFor(kind), kind };
                    ++written;
                }
            }
        }

        if (passDone) {
            // Publish the completed pass in one shot.
            std::lock_guard<std::mutex> lock(mBlocksMutex);
            mFoundBlocks.swap(mScanBuffer);
            mCursor = 0;
            break;
        }
    }

    if (dbg) {
        size_t found = 0, cache = 0;
        {
            std::lock_guard<std::mutex> lock(mBlocksMutex);
            found = mFoundBlocks.size();
            cache = mLegacyKindCache.size();
        }
        debugLogThrottled("tick: center=" + std::to_string(center.x) + "," + std::to_string(center.y) + "," + std::to_string(center.z) +
            " radius=" + std::to_string(radius) +
            " scanned=" + std::to_string(scanned) +
            " block=" + std::to_string(gotBlock) +
            " legacy=" + std::to_string(gotLegacy) +
            " classified=" + std::to_string(classified) +
            " enabled=" + std::to_string(enabled) +
            " exposedReject=" + std::to_string(exposedRejected) +
            " written=" + std::to_string(written) +
            " found=" + std::to_string(found) +
            " cache=" + std::to_string(cache));
    }
}

void BlockESP::OnRender() {
    if (!IsEnabled()) return;
    bool dbg = debugEnabled(this);
    if (!static_cast<BoolSetting*>(mSettings[14])->mValue &&
        SDK::ScreenView::currentScreenName() != "hud_screen") {
        if (dbg) debugLogThrottled("render: skipped screen=" + SDK::ScreenView::currentScreenName());
        return;
    }
    std::vector<FoundBlock> blocks;
    {
        std::lock_guard<std::mutex> lock(mBlocksMutex);
        if (mFoundBlocks.empty()) {
            if (dbg) debugLogThrottled("render: found list empty");
            return;
        }
        blocks.reserve(mFoundBlocks.size());
        for (const auto& kv : mFoundBlocks) blocks.push_back(kv.second);
    }

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, 0x200)) {
        if (dbg) debugLogThrottled("render: ClientInstance invalid");
        return;
    }

    // Camera MVP via the shared delayed-transform acquisition (FrameView.h):
    // the classic pipeline matches the matrix to the frame actually on
    // screen, so boxes stop sliding off blocks while the camera moves.
    float mvp[16];
    Vec3 origin(0, 0, 0);
    if (!acquireCameraMVP(ci, mvp, origin)) {
        if (dbg) debugLogThrottled("render: camera MVP unavailable");
        return;
    }

    int frame = ImGui::GetFrameCount();
    static int s_drawnFrame = -1;
    if (s_drawnFrame == frame) return;
    s_drawnFrame = frame;

    int type = static_cast<EnumSetting*>(mSettings[0])->mValue;   // 0=2D 1=3D 2=Outline
    bool doFill = static_cast<BoolSetting*>(mSettings[16])->mValue;
    float fillA = static_cast<NumberSetting*>(mSettings[17])->mValue;
    bool mergeMode = mSettings.size() > 19 && static_cast<BoolSetting*>(mSettings[19])->mValue;
    if (fillA < 0.0f) fillA = 0.0f; if (fillA > 1.0f) fillA = 1.0f;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    // Cube topology from the shared BoxOverlay tables (same canonical corner
    // layout projectBlockBox emits).


    int projected = 0;
    if (mergeMode) {
        // ---- Merge: face-adjacent same-kind blocks become ONE shape ------
        // BFS the found set for face-connected clusters of the same kind;
        // every corner the cluster projects becomes a single 2D rect (2D
        // mode) or one hull contour (3D / Outline) — an ore vein reads as one
        // blob instead of a box per block.
        static const int kDirs[6][3] = {
            { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
        };
        std::unordered_map<SDK::BlockPos, const FoundBlock*, SDK::BlockPosHash> lookup;
        for (const FoundBlock& fb : blocks) lookup.emplace(fb.pos, &fb);
        std::unordered_set<SDK::BlockPos, SDK::BlockPosHash> visited;
        std::vector<ImVec2> pts, hull;
        for (const auto& kv : lookup) {
            if (visited.count(kv.first)) continue;
            const FoundBlock* seed = kv.second;
            std::vector<SDK::BlockPos> stack{ kv.first };
            visited.insert(kv.first);
            std::vector<const FoundBlock*> cluster;
            while (!stack.empty() && cluster.size() < 512) {
                const SDK::BlockPos cur = stack.back(); stack.pop_back();
                const FoundBlock* fb = lookup[cur];
                if (!fb) continue;
                cluster.push_back(fb);
                for (int d = 0; d < 6; ++d) {
                    const SDK::BlockPos nb(cur.x + kDirs[d][0], cur.y + kDirs[d][1],
                                           cur.z + kDirs[d][2]);
                    if (visited.count(nb)) continue;
                    auto it = lookup.find(nb);
                    if (it == lookup.end() || it->second->kind != seed->kind) continue;
                    visited.insert(nb);
                    stack.push_back(nb);
                }
            }
            pts.clear();
            for (const FoundBlock* fb : cluster) {
                ImVec2 p[8]; bool v[8];
                if (SDK::BlockScan::projectBlockBox(fb->pos, mvp, origin, screen, p, v) < 2)
                    continue;
                for (int i = 0; i < 8; ++i) if (v[i]) pts.push_back(p[i]);
            }
            if (pts.size() < 3) continue;
            ++projected;
            const ImColor col = seed->color;
            const ImColor fill(col.Value.x, col.Value.y, col.Value.z, fillA);
            if (type == 0) {
                float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
                for (const ImVec2& q : pts) {
                    minX = std::min(minX, q.x); maxX = std::max(maxX, q.x);
                    minY = std::min(minY, q.y); maxY = std::max(maxY, q.y);
                }
                if (maxX <= minX || maxY <= minY) continue;
                ImVec2 mn(minX, minY), mx(maxX, maxY);
                if (doFill) dl->AddRectFilled(mn, mx, fill);
                dl->AddRect(mn, mx, ImColor(0, 0, 0, 220), 0.0f, 0, 3.0f);
                dl->AddRect(mn, mx, col, 0.0f, 0, 1.5f);
            } else if (type == 1) {
                // 3D + Merge: the TRUE union wireframe. A voxel-union edge is
                // drawn iff the union SURFACE folds along it. With a = the
                // neighbour across the edge's first adjacent face, b = across
                // the second, c = the diagonal neighbour, the edge is
                // suppressed only when the surface runs STRAIGHT through it
                // (exactly one of a/b present: the two coplanar side faces
                // fuse seamlessly across the seam — the line Merge exists to
                // remove) or when it is fully interior (a, b and c all
                // present). The old "both adjacent faces shared" test never
                // removed the plain seam between two side-by-side blocks and
                // wrongly ate the concave L-corner edges, so Merge visibly did
                // nothing in 3D.
                // Per-edge face directions, matching kCubeEdges order (the
                // two faces each edge belongs to; see kCubeFaces).
                static const int kEdgeDirs[12][2][3] = {
                    { { 0, -1, 0 }, { 0, 0, -1 } },  // {0,1}
                    { { 0, -1, 0 }, { 1, 0, 0 } },   // {1,2}
                    { { 0, -1, 0 }, { 0, 0, 1 } },   // {2,3}
                    { { 0, -1, 0 }, { -1, 0, 0 } },  // {3,0}
                    { { 0, 1, 0 },  { 0, 0, -1 } },  // {4,5}
                    { { 0, 1, 0 },  { 1, 0, 0 } },   // {5,6}
                    { { 0, 1, 0 },  { 0, 0, 1 } },   // {6,7}
                    { { 0, 1, 0 },  { -1, 0, 0 } },  // {7,4}
                    { { -1, 0, 0 }, { 0, 0, -1 } },  // {0,4}
                    { { 1, 0, 0 },  { 0, 0, -1 } },  // {1,5}
                    { { 1, 0, 0 },  { 0, 0, 1 } },   // {2,6}
                    { { -1, 0, 0 }, { 0, 0, 1 } },   // {3,7}
                };
                // kCubeFaces order: bottom, top, -Z, +X, +Z, -X.
                static const int kFaceDir[6][3] = {
                    { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 },
                    { 1, 0, 0 }, { 0, 0, 1 }, { -1, 0, 0 }
                };
                for (const FoundBlock* fb : cluster) {
                    ImVec2 p[8]; bool v[8];
                    if (SDK::BlockScan::projectBlockBox(fb->pos, mvp, origin, screen, p, v) < 2)
                        continue;
                    const SDK::BlockPos& P = fb->pos;
                    // Same-KIND cluster membership: a neighbour block of a
                    // different kind is a separate cluster and must not eat
                    // this cluster's edges or faces.
                    auto nb = [&](int dx, int dy, int dz) {
                        const SDK::BlockPos q(P.x + dx, P.y + dy, P.z + dz);
                        auto it = lookup.find(q);
                        return it != lookup.end() && it->second->kind == seed->kind;
                    };
                    // Fill: only the non-shared face quads, so the union's
                    // interior is not double-painted.
                    if (doFill) {
                        for (int fi = 0; fi < 6; ++fi) {
                            if (nb(kFaceDir[fi][0], kFaceDir[fi][1], kFaceDir[fi][2])) continue;
                            const int* q = SDK::BoxOverlay::kCubeFaces[fi];
                            if (!(v[q[0]] && v[q[1]] && v[q[2]] && v[q[3]])) continue;
                            const ImVec2 quad[4] = { p[q[0]], p[q[1]], p[q[2]], p[q[3]] };
                            dl->AddConvexPolyFilled(quad, 4, fill);
                        }
                    }
                    for (int ei = 0; ei < 12; ++ei) {
                        const int ca = SDK::BoxOverlay::kCubeEdges[ei][0];
                        const int cb = SDK::BoxOverlay::kCubeEdges[ei][1];
                        if (!(v[ca] && v[cb])) continue;
                        const int* d1 = kEdgeDirs[ei][0];
                        const int* d2 = kEdgeDirs[ei][1];
                        const bool a = nb(d1[0], d1[1], d1[2]);
                        const bool b = nb(d2[0], d2[1], d2[2]);
                        const bool c = nb(d1[0] + d2[0], d1[1] + d2[1], d1[2] + d2[2]);
                        const bool smooth = (!a && b && !c) || (a && !b && !c) || (a && b && c);
                        if (smooth) continue;
                        dl->AddLine(p[ca], p[cb], ImColor(0, 0, 0, 220), 3.0f);
                        dl->AddLine(p[ca], p[cb], col, 1.5f);
                    }
                }
            } else {
                // Outline + Merge: the TRUE union silhouette. Every block
                // strokes its own flat hull EXCEPT the edges whose midpoint
                // falls inside a NEIGHBOURING block's hull — the junction
                // lines vanish, so touching blocks fuse along their shared
                // edge, concave notches (the L) stay notched, and a lone
                // block keeps the plain Outline look. No one-big-hull
                // diagonal bridging, no 3D cage.
                struct SHull { ImVec2 pts[18]; int n; };
                std::vector<SHull> hulls;
                for (const FoundBlock* fb : cluster) {
                    ImVec2 p[8]; bool v[8];
                    if (SDK::BlockScan::projectBlockBox(fb->pos, mvp, origin, screen, p, v) < 2)
                        continue;
                    SHull h;
                    ImVec2 valids[8]; int nv = 0;
                    for (int i = 0; i < 8; ++i) if (v[i]) valids[nv++] = p[i];
                    h.n = (nv >= 3) ? SDK::BoxOverlay::convexHull2D(valids, nv, h.pts) : 0;
                    if (h.n >= 3) hulls.push_back(h);
                }
                if (!hulls.empty()) {
                    // Fill FIRST, and as ONE shape: the convex hull over EVERY
                    // projected corner of the whole cluster. The per-block hull
                    // fills used before still showed artifacts no matter what:
                    // adjacent hulls OVERLAP in screen space at oblique angles
                    // (a block's hull spills past the true union), so the
                    // translucent fills stacked into darker patches with
                    // visible boundaries, and their shared edges double-blended
                    // even with AA off. A single cluster-wide hull has no
                    // internal boundaries AT ALL — one polygon, one uniform
                    // alpha, zero seams. The strokes on top still trace the
                    // true union outline (concave notches stay visible as
                    // unfilled corners, matching the regular ESP's Outline
                    // fill philosophy).
                    if (doFill) {
                        ImVec2 allPts[8 * 64];
                        int nAll = 0;
                        for (const FoundBlock* fb : cluster) {
                            ImVec2 p[8]; bool v[8];
                            if (SDK::BlockScan::projectBlockBox(fb->pos, mvp, origin, screen, p, v) < 2)
                                continue;
                            for (int i = 0; i < 8 && nAll < 8 * 64; ++i)
                                if (v[i]) allPts[nAll++] = p[i];
                        }
                        if (nAll >= 3) {
                            ImVec2 clusterHull[18];
                            int hn = blockEspGroupHull2D(allPts, nAll, clusterHull);
                            if (hn >= 3)
                                dl->AddConvexPolyFilled(clusterHull, hn, fill);
                        }
                    }
                    // Point in a convex polygon; boundary counts as inside
                    // (a junction edge lies ON the neighbour's hull edge).
                    auto insideHull = [](const SHull& h, const ImVec2& p) {
                        int sign = 0;
                        for (int e = 0; e < h.n; ++e) {
                            const ImVec2& a = h.pts[e];
                            const ImVec2& b = h.pts[(e + 1) % h.n];
                            const float cr = (b.x - a.x) * (p.y - a.y) -
                                             (b.y - a.y) * (p.x - a.x);
                            const float eps = 0.5f;
                            if (cr > eps)       { if (sign < 0) return false; sign = 1; }
                            else if (cr < -eps) { if (sign > 0) return false; sign = -1; }
                        }
                        return true;
                    };
                    auto insideAnyOther = [&](const ImVec2& p, int skip) {
                        for (int i = 0; i < (int)hulls.size(); ++i) {
                            if (i == skip) continue;
                            if (insideHull(hulls[i], p)) return true;
                        }
                        return false;
                    };
                    for (int i = 0; i < (int)hulls.size(); ++i) {
                        for (int e = 0; e < hulls[i].n; ++e) {
                            const ImVec2 a = hulls[i].pts[e];
                            const ImVec2 b = hulls[i].pts[(e + 1) % hulls[i].n];
                            const ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                            if (insideAnyOther(mid, i)) continue;   // interior junction
                            dl->AddLine(a, b, ImColor(0, 0, 0, 220), 3.0f);
                            dl->AddLine(a, b, col, 1.5f);
                        }
                    }
                }
            }
        }
    } else for (const FoundBlock& fb : blocks) {
        ImVec2 p[8]; bool v[8];
        if (SDK::BlockScan::projectBlockBox(fb.pos, mvp, origin, screen, p, v) < 2) continue;
        ++projected;
        ImColor col = fb.color;
        ImColor fill(col.Value.x, col.Value.y, col.Value.z, fillA);

        if (type == 0) {
            // 2D: screen-space bounding rectangle over the projected corners.
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            for (int i = 0; i < 8; ++i) {
                if (!v[i]) continue;
                if (p[i].x < minX) minX = p[i].x;
                if (p[i].y < minY) minY = p[i].y;
                if (p[i].x > maxX) maxX = p[i].x;
                if (p[i].y > maxY) maxY = p[i].y;
            }
            if (maxX <= minX || maxY <= minY) continue;
            ImVec2 mn(minX, minY), mx(maxX, maxY);
            if (doFill) dl->AddRectFilled(mn, mx, fill);
            dl->AddRect(mn, mx, ImColor(0, 0, 0, 220), 0.0f, 0, 3.0f);
            dl->AddRect(mn, mx, col, 0.0f, 0, 1.5f);
        } else {
            // 3D box (type 1) or Outline (type 2). Fill only applies to 3D.
            if (type == 1 && doFill) {
                for (int fi = 0; fi < 6; ++fi) {
                    const int* f = SDK::BoxOverlay::kCubeFaces[fi];
                    if (!(v[f[0]] && v[f[1]] && v[f[2]] && v[f[3]])) continue;
                    ImVec2 q[4] = { p[f[0]], p[f[1]], p[f[2]], p[f[3]] };
                    dl->AddConvexPolyFilled(q, 4, fill);
                }
            }
            if (type == 1) {
                // 3D: full 12-edge cube wireframe (black underlay + colour).
                for (int ei = 0; ei < 12; ++ei) {
                    const int a = SDK::BoxOverlay::kCubeEdges[ei][0];
                    const int b = SDK::BoxOverlay::kCubeEdges[ei][1];
                    if (!(v[a] && v[b])) continue;
                    dl->AddLine(p[a], p[b], ImColor(0, 0, 0, 220), 3.0f);
                    dl->AddLine(p[a], p[b], col, 1.5f);
                }
            } else {
                // Outline: silhouette only -> convex hull of the 8 projected
                // corners, drawn as one closed contour (no interior edges
                // crossing the block, no duplicated 12-edge cube wireframe).
                ImVec2 valids[8];
                int nv = 0;
                for (int i = 0; i < 8; ++i) if (v[i]) valids[nv++] = p[i];
                if (nv >= 3) {
                    ImVec2 hull[18];
                    int hn = SDK::BoxOverlay::convexHull2D(valids, nv, hull);
                    if (hn >= 3) {
                        if (doFill) dl->AddConvexPolyFilled(hull, hn, fill);
                        dl->AddPolyline(hull, hn, ImColor(0, 0, 0, 220), ImDrawFlags_Closed, 3.0f);
                        dl->AddPolyline(hull, hn, col, ImDrawFlags_Closed, 1.5f);
                    }
                }
            }
        }
    }
    if (dbg) {
        debugLogThrottled("render: blocks=" + std::to_string(blocks.size()) +
            " projected=" + std::to_string(projected) +
            " screen=" + std::to_string((int)screen.x) + "x" + std::to_string((int)screen.y) +
            " origin=" + std::to_string(origin.x) + "," + std::to_string(origin.y) + "," + std::to_string(origin.z) +
            " frameView=" + std::to_string(g_frameView.valid ? 1 : 0) +
            " screenName=" + SDK::ScreenView::currentScreenName());
    }
}
