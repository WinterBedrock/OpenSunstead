#include "Hat.h"
#include "../../Utils/MCSDK.h"
#include "../../Render/FrameView.h"
#include "../../SDK/Actor/RenderPos.h"
#include "../Setting.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include <imgui.h>
#include "imgui_internal.h"
#include <algorithm>
#include <vector>
#include <Windows.h>
#include <cstdio>
#include <cmath>
#include <unordered_map>

namespace {

static constexpr float kHatOffset = -1.0f;

static constexpr float kHeadPivotBelow = 0.55f;

static bool sdk_safe_ptr(const void* p, size_t bytes) {
    if (!p) return false;
    return !IsBadReadPtr(p, bytes);
}

static double nowMsHiRes() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static Vec3 interpPos(SDK::Actor* a) {
    return SDK::interpSelfRenderPos(a);
}

static bool getHeadRef(SDK::Actor* a, Vec3* center, float* feetY, float* headY) {
    Vec3 pos = interpPos(a);
    AABB bb;
    __try { bb = a->getBoundingBox(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    float h = bb.higher.y - bb.lower.y;
    if (!(h > 0.1f && h < 6.0f)) h = 1.8f;
    center->x = pos.x;
    center->z = pos.z;
    center->y = pos.y;
    *feetY = pos.y;
    *headY = pos.y + h;
    return true;
}

static bool sehReadRot(SDK::Actor* a, Vec2* cur) {
    __try {
        ActorRotationComponent* rc = a->getRotation();
        if (!rc) return false;
        *cur = rc->rotation;
        return std::isfinite(cur->x) && std::isfinite(cur->y);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct RotSmooth { float pitch, yaw; double tMs; };
static Vec2 headRotFor(SDK::Actor* a) {
    Vec2 cur(0, 0);
    if (!sehReadRot(a, &cur)) return Vec2(0, 0);

    static std::unordered_map<void*, RotSmooth> sm;
    if (sm.size() > 512) sm.clear();
    RotSmooth& s = sm[(void*)a];
    const double now = nowMsHiRes();
    if (s.tMs == 0.0) {
        s = { cur.x, cur.y, now };
        return cur;
    }
    const float dtMs = (float)std::max(0.0, now - s.tMs);
    s.tMs = now;
    float dP = cur.x - s.pitch;
    float dY = cur.y - s.yaw;
    while (dY >  180.0f) dY -= 360.0f;
    while (dY < -180.0f) dY += 360.0f;
    if (std::fabs(dP) > 90.0f || std::fabs(dY) > 120.0f) {
        s.pitch = cur.x; s.yaw = cur.y;
        return cur;
    }
    const float k = 1.0f - expf(-dtMs / 65.0f);
    s.pitch += dP * k;
    s.yaw   += dY * k;
    while (s.yaw >  180.0f) s.yaw -= 360.0f;
    while (s.yaw < -180.0f) s.yaw += 360.0f;
    return Vec2(s.pitch, s.yaw);
}

struct HeadFrame { Vec3 axis, e1, e2; };

static HeadFrame headFrame(float pitchDeg, float yawDeg, bool tilt) {
    HeadFrame f;
    if (!tilt) {
        f.axis = Vec3(0, 1, 0);
        f.e1   = Vec3(1, 0, 0);
        f.e2   = Vec3(0, 0, 1);
        return f;
    }
    constexpr float D2R = 0.01745329251f;
    const float th = pitchDeg * D2R;
    const float yr = yawDeg * D2R;
    const float ct = cosf(th), st = sinf(th);
    const Vec3 fwd(-sinf(yr), 0.0f, cosf(yr));
    const Vec3 right(cosf(yr), 0.0f, sinf(yr));
    f.axis = Vec3(fwd.x * st, ct, fwd.z * st);
    f.e1   = right;
    f.e2   = Vec3(fwd.x * ct, -st, fwd.z * ct);
    return f;
}

static void addRingBloom(ImDrawList* dl, const std::vector<ImVec2>& pts,
                         const std::vector<ImColor>& cols, const std::vector<char>& ok,
                         float spread, float a) {
    const int n = (int)pts.size() - 1;
    if (!dl || n < 3 || spread <= 0.5f || a <= 0.004f) return;
    const int res = (int)ok.size() - 1;
    if (res < n) return;

    std::vector<ImVec2> nrm(n + 1);
    std::vector<char> deg(n + 1, 0);
    for (int i = 0; i <= n; ++i) {
        const int pv = (i == 0) ? n - 1 : i - 1;
        const int nx = (i == n) ? 1 : i + 1;
        const float dx = pts[nx].x - pts[pv].x;
        const float dy = pts[nx].y - pts[pv].y;
        const float len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-4f) { deg[i] = 1; nrm[i] = ImVec2(0.0f, 0.0f); }
        else nrm[i] = ImVec2(-dy / len, dx / len);
    }

    int nSeg = 0;
    for (int i = 0; i < n; ++i)
        if (ok[i] && ok[i + 1] && !deg[i] && !deg[i + 1]) ++nSeg;
    if (nSeg == 0) return;

    const int kBands = (int)std::clamp(spread / 2.0f, 8.0f, 22.0f);
    const float sigma = spread / 3.0f;
    const float kk = 1.0f / (sigma * 1.41421356f); // 1/(sigma*sqrt(2)) for erfc
    const int quads = nSeg * kBands * 2;
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;

    dl->PrimReserve(quads * 6, quads * 4);
    for (int i = 0; i < n; ++i) {
        const int j = i + 1;
        if (!ok[i] || !ok[j] || deg[i] || deg[j]) continue;
        const ImColor& ci = cols[i];
        const ImColor& cj = cols[j];
        for (int side = 0; side < 2; ++side) {
            const float sg = (side == 0) ? 1.0f : -1.0f;
            const float nix = nrm[i].x * sg, niy = nrm[i].y * sg;
            const float njx = nrm[j].x * sg, njy = nrm[j].y * sg;
            for (int b = 0; b < kBands; ++b) {
                const float d0 = spread * ((float)b / (float)kBands);
                const float d1 = spread * ((float)(b + 1) / (float)kBands);
                const float aIn  = a * erfcf(d0 * kk);
                const float aOut = a * erfcf(d1 * kk);
                const unsigned base = dl->_VtxCurrentIdx;
                dl->PrimWriteVtx(ImVec2(pts[i].x + nix * d0, pts[i].y + niy * d0), uv,
                                 ImGui::GetColorU32(ImVec4(ci.Value.x, ci.Value.y, ci.Value.z, aIn)));
                dl->PrimWriteVtx(ImVec2(pts[j].x + njx * d0, pts[j].y + njy * d0), uv,
                                 ImGui::GetColorU32(ImVec4(cj.Value.x, cj.Value.y, cj.Value.z, aIn)));
                dl->PrimWriteVtx(ImVec2(pts[j].x + njx * d1, pts[j].y + njy * d1), uv,
                                 ImGui::GetColorU32(ImVec4(cj.Value.x, cj.Value.y, cj.Value.z, aOut)));
                dl->PrimWriteVtx(ImVec2(pts[i].x + nix * d1, pts[i].y + niy * d1), uv,
                                 ImGui::GetColorU32(ImVec4(ci.Value.x, ci.Value.y, ci.Value.z, aOut)));
                dl->PrimWriteIdx((ImDrawIdx)(base));     dl->PrimWriteIdx((ImDrawIdx)(base + 1)); dl->PrimWriteIdx((ImDrawIdx)(base + 2));
                dl->PrimWriteIdx((ImDrawIdx)(base));     dl->PrimWriteIdx((ImDrawIdx)(base + 2)); dl->PrimWriteIdx((ImDrawIdx)(base + 3));
            }
        }
    }
}

} // namespace

void Hat::OnEnable() {}

void Hat::OnRender() {
    if (!IsEnabled()) return;

    if (SDK::ScreenView::currentScreenName() != "hud_screen") return;

    SDK::ClientInstance* ci = nullptr;
    SDK::Level* level = nullptr;
    SDK::Actor* lp = nullptr;
    if (!SDK::resolveContext(&ci, &level, &lp)) return;

    float mvp[16];
    Vec3 origin(0, 0, 0);
    if (!acquireCameraMVP(ci, mvp, origin)) return;

    std::vector<SDK::Actor*> actors;
    if (!sehGetActorList(level, &actors)) return;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 screen = io.DisplaySize;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    static int s_frame = -1;
    static int s_passInFrame = 0;
    static int s_lastFramePassCount = 1;
    static int s_drawnFrame = -1;
    int frame = ImGui::GetFrameCount();
    if (frame != s_frame) {
        if (s_frame != -1) s_lastFramePassCount = (s_passInFrame > 0 ? s_passInFrame : 1);
        s_frame = frame;
        s_passInFrame = 0;
    }
    ++s_passInFrame;
    if (s_passInFrame < s_lastFramePassCount) return;
    if (s_drawnFrame == frame) return;
    s_drawnFrame = frame;


    const bool  isHalo      = IsHalo();
    float radius     = static_cast<NumberSetting*>(mSettings[1])->mValue;
    float height     = static_cast<NumberSetting*>(mSettings[2])->mValue;
    int   segments   = (int)static_cast<NumberSetting*>(mSettings[3])->mValue;
    float thickness  = static_cast<NumberSetting*>(mSettings[4])->mValue;
    bool  renderOthers = static_cast<BoolSetting*>(mSettings[5])->mValue;
    ColorSetting* col = static_cast<ColorSetting*>(mSettings[6]);
    float fillA      = static_cast<NumberSetting*>(mSettings[7])->mValue;
    const bool tilt  = static_cast<BoolSetting*>(mSettings[8])->mValue;

    if (segments < 6) segments = 6;
    ImColor lineCol(col->mValue[0], col->mValue[1], col->mValue[2], col->mValue[3]);
    ImColor fillCol(col->mValue[0], col->mValue[1], col->mValue[2], fillA);
    ImU32 lineU32 = lineCol;
    ImU32 fillU32 = fillCol;

    std::vector<ImColor> pal;
    if (isHalo) {
        const float period = ColorUtils::getThemedPeriod();
        pal.resize(segments + 1);
        for (int s = 0; s <= segments; ++s)
            pal[s] = ColorUtils::getThemedColor((float)s * period / (float)segments);
    }
    const float breath = 0.88f + 0.12f * sinf((float)nowMsHiRes() * 0.0042f);

    for (SDK::Actor* a : actors) {
        if (!a || !sdk_safe_ptr(a, 0x230)) continue;
        if (a != lp && !renderOthers) continue;
        if (!sehIsPlayer(a)) continue;

        Vec3 center; float feetY, headY;
        if (!getHeadRef(a, &center, &feetY, &headY)) continue;

        const float baseY = headY + kHatOffset - 0.60f;
        const Vec3 anchor(center.x, baseY, center.z);

        const Vec2 rot = headRotFor(a); // { pitch, yaw } in degrees
        const HeadFrame hf = headFrame(rot.x, rot.y, tilt);

        const Vec3 pivotC(anchor.x, anchor.y - kHeadPivotBelow, anchor.z);
        const float ringLift = isHalo
            ? kHeadPivotBelow + 0.05f + height * 0.5f
            : kHeadPivotBelow;
        const float ringW[3] = { pivotC.x + hf.axis.x * ringLift,
                                 pivotC.y + hf.axis.y * ringLift,
                                 pivotC.z + hf.axis.z * ringLift };
        const float apexLift = kHeadPivotBelow + height;
        const float apexW[3] = { pivotC.x + hf.axis.x * apexLift,
                                 pivotC.y + hf.axis.y * apexLift,
                                 pivotC.z + hf.axis.z * apexLift };

        std::vector<ImVec2> ring;
        std::vector<char> ok;
        ring.reserve(segments + 1);
        ok.reserve(segments + 1);
        bool anyValid = false;
        for (int s = 0; s <= segments; ++s) {
            const float ang = (float)s / (float)segments * 6.28318530718f;
            const float ca = cosf(ang) * radius;
            const float sa = sinf(ang) * radius;
            const Vec3 pW(ringW[0] + hf.e1.x * ca + hf.e2.x * sa,
                          ringW[1] + hf.e1.y * ca + hf.e2.y * sa,
                          ringW[2] + hf.e1.z * ca + hf.e2.z * sa);
            ImVec2 sp;
            const bool vis = worldToScreen(pW, mvp, origin, screen, &sp);
            if (vis) anyValid = true;
            ring.push_back(sp);
            ok.push_back(vis ? 1 : 0);
        }
        if (!anyValid) continue;

        if (!isHalo) {
            ImVec2 apex;
            if (!worldToScreen(Vec3(apexW[0], apexW[1], apexW[2]), mvp, origin, screen, &apex)) continue;

            if (fillA > 0.001f) {
                for (int s = 0; s < segments; ++s) {
                    dl->AddTriangleFilled(apex, ring[s], ring[s + 1], fillU32);
                }
            }
            for (int s = 0; s < segments; ++s) {
                dl->AddLine(ring[s], ring[s + 1], lineU32, thickness);
                dl->AddLine(ring[s], apex, lineU32, thickness * 0.6f);
            }
        } else {
            float midX = 0.0f, midY = 0.0f; int cnt = 0;
            float maxR = 0.0f;
            for (int s = 0; s <= segments; ++s) {
                if (!ok[s]) continue;
                midX += ring[s].x; midY += ring[s].y; ++cnt;
            }
            if (cnt > 0) {
                midX /= (float)cnt; midY /= (float)cnt;
                for (int s = 0; s <= segments; ++s) {
                    if (!ok[s]) continue;
                    const float dx = ring[s].x - midX, dy = ring[s].y - midY;
                    const float r = sqrtf(dx * dx + dy * dy);
                    if (r > maxR) maxR = r;
                }
            }
            const float glowA = std::clamp(fillA * 1.8f, 0.0f, 1.0f) * breath;
            const float spread = std::clamp(maxR * 0.45f, 10.0f, 46.0f);
            addRingBloom(dl, ring, pal, ok, spread * 2.3f, glowA * 0.28f); // ambient haze
            addRingBloom(dl, ring, pal, ok, spread, glowA);                // tight bloom

            for (int s = 0; s < segments; ++s) {
                if (!ok[s] || !ok[s + 1]) continue;
                const ImColor& c = pal[s];
                dl->AddLine(ring[s], ring[s + 1],
                            ImGui::GetColorU32(ImVec4(c.Value.x, c.Value.y, c.Value.z, breath)),
                            thickness * 1.35f);
                const float t = 0.6f;
                dl->AddLine(ring[s], ring[s + 1],
                            ImGui::GetColorU32(ImVec4(
                                c.Value.x + (1.0f - c.Value.x) * t,
                                c.Value.y + (1.0f - c.Value.y) * t,
                                c.Value.z + (1.0f - c.Value.z) * t,
                                0.9f * breath)),
                            thickness * 0.8f + 0.6f);
            }
        }
    }
}
