#include "JumpCircles.h"
#include "../../Utils/MCSDK.h"
#include "../../Render/FrameView.h"
#include "../Setting.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include <imgui.h>
#include <vector>
#include <cstdlib>
#include <Windows.h>
#include <cmath>

namespace {

static bool sdk_safe_ptr(const void* p, size_t bytes) {
    if (!p) return false;
    return !IsBadReadPtr(p, bytes);
}

// Context resolution is SDK::resolveContext (SDK/Client/GameContext.h).

// mat4Mul / worldToScreen are consolidated in SDK/Math.h (mat4Multiply,
// worldToScreen; both global, resolved via ADL).

// Read the local player feet-ground position + a vertical-velocity proxy.
// Feet Y is the bounding-box floor (true ground contact); x/z from getPos.
// The vertical velocity is derived from the per-tick position delta
// (pos.y - posOld.y) — the same proven state fields ChinaHat interpolates —
// which is more reliable across builds than the raw velocity component.
static bool sehReadState(SDK::Actor* lp, Vec3* feet, float* velY) {
    __try {
        Vec3 pos = lp->getPos();
        Vec3 old = lp->getPosOld();
        AABB bb = lp->getBoundingBox();
        float footY = bb.lower.y;
        // Guard a bogus AABB: fall back to pos.y (treated as feet like ChinaHat).
        if (!(footY < pos.y + 0.5f) || footY < pos.y - 4.0f) footY = pos.y;
        feet->x = pos.x; feet->y = footY; feet->z = pos.z;
        *velY = pos.y - old.y;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// -------------------------------------------------------------------------
// Effect state (module-local, single local player).
// -------------------------------------------------------------------------
struct Ring {
    Vec3 pos;              // ground centre (world)
    float radius;          // current base radius
    unsigned long long start;
};
struct Particle {
    Vec3 pos;              // world position
    Vec3 vel;              // world velocity (blocks / frame-ish)
    float groundY;         // spawn ground level — particles rest HERE, never sink
    unsigned long long start;
};

static std::vector<Ring>     g_rings;
static std::vector<Particle> g_particles;
static bool  g_prevGrounded = true;
static float g_prevVelY     = 0.0f;

static float frand(float lo, float hi) {
    return lo + (hi - lo) * ((float)rand() / (float)RAND_MAX);
}

} // namespace

void JumpCircles::OnEnable() {
    g_rings.clear();
    g_particles.clear();
    g_prevGrounded = true;
    g_prevVelY = 0.0f;
}

void JumpCircles::OnDisable() {
    g_rings.clear();
    g_particles.clear();
}

void JumpCircles::OnRender() {
    if (!IsEnabled()) return;

    SDK::ClientInstance* ci = nullptr;
    SDK::Level* level = nullptr;
    SDK::Actor* lp = nullptr;
    if (!SDK::resolveContext(&ci, &level, &lp)) return;

    // Camera MVP via the shared delayed-transform acquisition (FrameView.h).
    float mvp[16];
    Vec3 origin(0, 0, 0);
    if (!acquireCameraMVP(ci, mvp, origin)) return;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 screen = io.DisplaySize;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // --- Multi-pass de-jitter (mirrors ESP / ChinaHat) ---------------------
    // OnRender runs several times per frame (once per active screen). Only the
    // final in-world pass carries a fresh view matrix + valid origin. Advance
    // simulation and draw at most once per frame, on the last pass.
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

    // --- Settings ---
    int   mode      = static_cast<EnumSetting*>(mSettings[0])->mValue;   // 0=Circle 1=Particles
    float speed     = static_cast<NumberSetting*>(mSettings[1])->mValue; // growth multiplier (1 = Rise pace)
    float lifetime  = static_cast<NumberSetting*>(mSettings[2])->mValue; // ms
    float opacity   = static_cast<NumberSetting*>(mSettings[3])->mValue;
    float thickness = static_cast<NumberSetting*>(mSettings[4])->mValue; // px
    int   pcount    = (int)static_cast<NumberSetting*>(mSettings[5])->mValue;
    bool  themed    = static_cast<BoolSetting*>(mSettings[6])->mValue;
    ColorSetting* col = static_cast<ColorSetting*>(mSettings[7]);
    if (lifetime < 1.0f) lifetime = 1.0f;
    if (thickness < 0.5f) thickness = 0.5f;

    auto baseColor = [&](float aMul) -> ImU32 {
        float r, g, b;
        if (themed) {
            ImColor tc = ColorUtils::getThemedColor(0);
            r = tc.Value.x; g = tc.Value.y; b = tc.Value.z;
        } else {
            r = col->mValue[0]; g = col->mValue[1]; b = col->mValue[2];
        }
        float a = opacity * aMul;
        if (a < 0.0f) a = 0.0f; if (a > 1.0f) a = 1.0f;
        return ImColor(r, g, b, a);
    };

    unsigned long long now = GetTickCount64();

    // --- Landing detection (transition airborne -> grounded) ---------------
    Vec3 feet; float velY = 0.0f;
    bool haveState = sehReadState(lp, &feet, &velY);
    if (haveState) {
        // Grounded when we are no longer moving downward appreciably and were
        // previously falling. velocity.y snaps toward ~0 on ground contact.
        bool grounded = velY > -0.05f;
        bool wasFalling = g_prevVelY < -0.12f;
        if (grounded && !g_prevGrounded && wasFalling) {
            if (mode == 0) {
                g_rings.push_back({ feet, 0.0f, now });   // Rise: ring grows from 0
            } else {
                int n = pcount;
                for (int i = 0; i < n; ++i) {
                    float ang = frand(0.0f, 6.28318530718f);
                    float spd = frand(0.4f, 1.0f) * (0.6f + speed * 2.0f);
                    Particle p;
                    p.pos = Vec3(feet.x, feet.y + 0.05f, feet.z);
                    p.vel = Vec3(cosf(ang) * spd * 0.06f,
                                 frand(0.10f, 0.20f),
                                 sinf(ang) * spd * 0.06f);
                    p.groundY = feet.y + 0.05f;
                    p.start = now;
                    g_particles.push_back(p);
                }
            }
        }
        g_prevGrounded = grounded;
        g_prevVelY = velY;
    }

    // -----------------------------------------------------------------------
    // CIRCLE MODE — Rise (6.9.5 JumpCircles):
    //   radius 0 -> 2 blocks at GROWTH_RATE 0.004/frame (≈0.24 blocks/s),
    //   ONE smooth closed ring (glLineLoop, 2px), the theme accent color
    //   flowing around the circumference, fading out as it expands
    //   (cumulative alpha loss integrates to ~1-(r/2)²). No concentric
    //   trail, no fat glow halo.
    // -----------------------------------------------------------------------
    const int segments = 96;
    static unsigned long long s_lastCircleNow = 0;
    const float dtMs = (float)(now - s_lastCircleNow);
    s_lastCircleNow = now;
    const float growthPerMs = 0.00024f * speed;   // 0.004 blocks/frame at 60fps
    for (size_t ri = 0; ri < g_rings.size(); ) {
        Ring& c = g_rings[ri];
        float ageMs = (float)(now - c.start);
        c.radius += growthPerMs * dtMs;
        const float kMaxRadius = 2.0f;
        if (c.radius >= kMaxRadius || ageMs > lifetime) {
            g_rings.erase(g_rings.begin() + ri);
            continue;
        }
        if (c.radius <= 0.01f) { ++ri; continue; }

        // Rise fade: slow at first, accelerating as the ring expands; the
        // lifetime acts as an early cut so the setting still controls it.
        const float radiusFrac = c.radius / kMaxRadius;         // 0 -> 1
        const float lifeFrac   = 1.0f - (ageMs / lifetime);     // 1 -> 0
        float fade = 1.0f - radiusFrac * radiusFrac;
        if (lifeFrac < fade) fade = lifeFrac;
        if (fade <= 0.0f) { ++ri; continue; }

        // Project the whole loop once, then stroke each visible segment with
        // its own accent color — the gradient flows around the ring.
        ImVec2 pts[segments];
        bool    vis[segments];
        int     visCount = 0;
        for (int j = 0; j < segments; ++j) {
            const float ang = (float)j / (float)segments * 6.28318530718f;
            Vec3 wp(c.pos.x + c.radius * cosf(ang),
                    c.pos.y,
                    c.pos.z + c.radius * sinf(ang));
            vis[j] = worldToScreen(wp, mvp, origin, screen, &pts[j]);
            if (vis[j]) ++visCount;
        }
        if (visCount >= 2) {
            for (int j = 0; j < segments; ++j) {
                const int k = (j + 1 == segments) ? 0 : j + 1;   // closed loop
                if (!vis[j] || !vis[k]) continue;
                ImU32 col;
                if (themed) {
                    ImColor tc = ColorUtils::getThemedColor(
                        (float)j / (float)segments * 3000.0f, now);
                    tc.Value.w = opacity * fade;
                    col = (ImU32)tc;
                } else {
                    col = baseColor(fade);
                }
                dl->AddLine(pts[j], pts[k], col, thickness);
            }
        }
        ++ri;
    }

    // -----------------------------------------------------------------------
    // PARTICLES MODE — burst that arcs up, then RESTS on the ground plane
    // (clamped to the spawn ground level so nothing sinks below the floor
    // and fades away into "the void").
    // -----------------------------------------------------------------------
    const float gravity = 0.012f;
    for (size_t pi = 0; pi < g_particles.size(); ) {
        Particle& p = g_particles[pi];
        float ageMs = (float)(now - p.start);
        if (ageMs > lifetime) { g_particles.erase(g_particles.begin() + pi); continue; }

        // Integrate (frame-rate independent enough for a short-lived effect).
        p.vel.y -= gravity;
        p.pos.x += p.vel.x;
        p.pos.y += p.vel.y;
        p.pos.z += p.vel.z;
        if (p.pos.y < p.groundY) {          // ground contact: rest + damp slide
            p.pos.y = p.groundY;
            p.vel.y = 0.0f;
            p.vel.x *= 0.80f;
            p.vel.z *= 0.80f;
        }

        float lifeFrac = 1.0f - (ageMs / lifetime);
        if (lifeFrac < 0.0f) lifeFrac = 0.0f;

        ImVec2 sp;
        if (worldToScreen(p.pos, mvp, origin, screen, &sp)) {
            float sz = 2.5f + 1.5f * lifeFrac;
            dl->AddCircleFilled(sp, sz, baseColor(lifeFrac), 8);
        }
        ++pi;
    }
}
