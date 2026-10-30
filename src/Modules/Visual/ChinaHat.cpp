#include "ChinaHat.h"
#include "../../Utils/MCSDK.h"
#include "../../Render/FrameView.h"
#include "../../SDK/Actor/RenderPos.h"
#include "../Setting.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include <imgui.h>
#include <vector>
#include <Windows.h>
#include <cstdio>
#include <cmath>
#include <unordered_map>

namespace {

// Vertical offset of the hat relative to the head, in blocks.
// This used to be the user-facing "Offset" NumberSetting; it has been removed
// from the module's settings list. The value is pinned to the slider's old
// MINIMUM (-1) - anything above that floats the hat too high above the head.
static constexpr float kHatOffset = -1.0f;

static bool sdk_safe_ptr(const void* p, size_t bytes) {
    if (!p) return false;
    return !IsBadReadPtr(p, bytes);
}

// sehGetActorList / sehIsPlayer are consolidated in SDK::GameContext (ADL).

// Context resolution is SDK::resolveContext (SDK/Client/GameContext.h).

// mat4Mul / worldToScreen are consolidated in SDK/Math.h (mat4Multiply,
// worldToScreen; both global, resolved via ADL).


// High-resolution "now" in milliseconds. GetTickCount64 only has ~15ms
// granularity, so on a sharp point like the hat apex the reconstructed tick
// alpha snapped in coarse 15ms steps -> visible shudder every few frames even
// though the (large) ESP boxes hid the same error. QPC gives sub-millisecond
// alpha, which is the single biggest smoothness win here.
static double nowMsHiRes() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static Vec3 interpPos(SDK::Actor* a) {
    // Shared tick-aligned interpolation (SDK::interpRenderPos) — the SAME
    // curve/alpha every other world-space overlay (ESP/NameTags) uses, which
    // rides the camera queue's SERVED push (delay 1). The previous private
    // wall-clock reconstruction rebuilt the alpha from the LIVE clock: with
    // the delay-1 serving that is one frame newer than the alpha the drawn
    // model used — the residual ChinaHat jitter the user saw while
    // everything else was already glued.
    // The hat is a SELF overlay: the local player's anchor comes from the
    // SERVED push (pair + alpha of the frame the camera came from) —
    // interpRenderPos would read the live pair, one tick fresher than the
    // model the presented frame drew, which was the above-head wobble.
    return SDK::interpSelfRenderPos(a);
}

// Returns the world-space head reference: interpolated x/z and the top of the
// actor's bounding box for y.
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

} // namespace

void ChinaHat::OnEnable() {}

void ChinaHat::OnRender() {
    if (!IsEnabled()) return;

    // Only draw on the in-world HUD screen. This is the key de-jitter fix and the
    // exact guard ESP uses: OnRender fires once per active screen (play_screen,
    // hud_screen, ...), but ONLY the hud_screen pass carries a FRESH view matrix
    // + valid render origin. Drawing on the other passes reuses a stale matrix,
    // which is what made the hat shudder while moving/rotating the camera. ESP
    // never jittered precisely because it has this guard and ChinaHat did not.
    if (SDK::ScreenView::currentScreenName() != "hud_screen") return;

    SDK::ClientInstance* ci = nullptr;
    SDK::Level* level = nullptr;
    SDK::Actor* lp = nullptr;
    if (!SDK::resolveContext(&ci, &level, &lp)) return;

    // Camera MVP via the shared delayed-transform acquisition (FrameView.h) —
    // the hat stays glued to heads while the camera moves.
    float mvp[16];
    Vec3 origin(0, 0, 0);
    if (!acquireCameraMVP(ci, mvp, origin)) return;

    std::vector<SDK::Actor*> actors;
    if (!sehGetActorList(level, &actors)) return;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 screen = io.DisplaySize;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // --- Multi-pass de-jitter (mirrors ESP) ---------------------------------
    // OnRender runs several times per frame (once per active screen). Only the
    // in-world pass has a fresh view matrix + valid origin; other passes carry a
    // stale matrix which makes the hat shudder when rotating the camera. Draw
    // only on the final pass of each frame, at most once per frame.
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
    if (s_passInFrame < s_lastFramePassCount) return; // wait for the last pass
    if (s_drawnFrame == frame) return;                // at most once per frame
    s_drawnFrame = frame;


    // --- Settings ---
    // The "Offset" NumberSetting was removed from the list, so every index that
    // followed it is shifted down by one (settings are read positionally):
    //   0 Radius | 1 Height | 2 Segments | 3 Thickness
    //   4 Render Others | 5 Color | 6 Fill Opacity
    // (Module::EnsureVisibleSetting appends "Visible" LAST, after these.)
    float radius     = static_cast<NumberSetting*>(mSettings[0])->mValue;
    float height     = static_cast<NumberSetting*>(mSettings[1])->mValue;
    int   segments   = (int)static_cast<NumberSetting*>(mSettings[2])->mValue;
    float thickness  = static_cast<NumberSetting*>(mSettings[3])->mValue;
    bool  renderOthers = static_cast<BoolSetting*>(mSettings[4])->mValue;
    ColorSetting* col = static_cast<ColorSetting*>(mSettings[5]);
    float fillA      = static_cast<NumberSetting*>(mSettings[6])->mValue;

    if (segments < 6) segments = 6;
    ImColor lineCol(col->mValue[0], col->mValue[1], col->mValue[2], col->mValue[3]);
    ImColor fillCol(col->mValue[0], col->mValue[1], col->mValue[2], fillA);
    ImU32 lineU32 = lineCol;
    ImU32 fillU32 = fillCol;

    for (SDK::Actor* a : actors) {
        if (!a || !sdk_safe_ptr(a, 0x230)) continue;
        if (a != lp && !renderOthers) continue;
        if (!sehIsPlayer(a)) continue;

        Vec3 center; float feetY, headY;
        if (!getHeadRef(a, &center, &feetY, &headY)) continue;

        // Hat sits on the head. Pull the base down ~0.45 blocks from the
        // bounding-box top so it rests on the head instead of floating above.
        float baseY = headY + kHatOffset - 0.45f;
        // Apex of the cone, above the base.
        Vec3 apexW(center.x, baseY + height, center.z);
        ImVec2 apex;
        if (!worldToScreen(apexW, mvp, origin, screen, &apex)) continue;

        // Project the base circle at head level.
        std::vector<ImVec2> ring;
        ring.reserve(segments + 1);
        bool anyValid = false;
        for (int s = 0; s <= segments; ++s) {
            float ang = (float)s / (float)segments * 6.28318530718f;
            Vec3 pW(center.x + cosf(ang) * radius,
                    baseY,
                    center.z + sinf(ang) * radius);
            ImVec2 sp;
            if (worldToScreen(pW, mvp, origin, screen, &sp)) { anyValid = true; }
            ring.push_back(sp);
        }
        if (!anyValid) continue;

        // Filled cone (triangle fan from apex to each base edge).
        if (fillA > 0.001f) {
            for (int s = 0; s < segments; ++s) {
                dl->AddTriangleFilled(apex, ring[s], ring[s + 1], fillU32);
            }
        }
        // Cone outline: base ring + slant lines to apex.
        for (int s = 0; s < segments; ++s) {
            dl->AddLine(ring[s], ring[s + 1], lineU32, thickness);
            dl->AddLine(ring[s], apex, lineU32, thickness * 0.6f);
        }
    }
}