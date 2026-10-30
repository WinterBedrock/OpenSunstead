#include "SuperHeroFX.h"
#include "../../Render/RenderManager.h"
#include "../../Utils/MCSDK.h"
#include "../../Render/FrameView.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../Setting.h"
#include <imgui.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <Windows.h>

// im a super hero, super hero, super hero
namespace {

const char* const kWords_[] = {
    "BOOM!", "POW!", "BAM!", "CRASH!", "WHAM!", "ZAP!",
    "KABOOM!", "THUD!", "SMACK!", "KER-POW!", "BIFF!", "OOF!",
};
constexpr int kWords_Count = sizeof(kWords_) / sizeof(kWords_[0]);

const ImColor kPalette_[] = {
    ImColor(255,   0,   0, 255),
    ImColor(255, 165,   0, 255),
    ImColor(255, 255,   0, 255),
    ImColor( 50, 220,  80, 255),
    ImColor( 50, 150, 255, 255),
    ImColor(180,  80, 255, 255),
    ImColor(255,  80, 180, 255),
    ImColor(255, 255, 255, 255),
};
constexpr int kPalette_Count = sizeof(kPalette_) / sizeof(kPalette_[0]);

inline float fxRand01() { return (float)std::rand() / (float)RAND_MAX; }
inline float fxRandRange(float a, float b) { return a + (b - a) * fxRand01(); }

} // namespace

// The body of the module's own Effect struct is declared in the header (it is
// used within the inline spawnFX template below) but the spawn logic itself
// lives here so the .cpp helper has access to the private Effect fields.
struct SuperHeroFXSpawnAccess {
    static SuperHeroFX::Effect make(float wx, float wy, float wz, float life,
                                    float rise, bool rainbow, int rainbowIdx, int word) {
        SuperHeroFX::Effect e;
        e.wx = wx; e.wy = wy; e.wz = wz;
        e.age = 0.0f;
        e.life = life;
        e.riseSpeed = rise;
        e.swayPhase = fxRandRange(0.0f, 6.2831f);
        e.word = word;
        e.color = rainbow ? kPalette_[rainbowIdx % kPalette_Count]
                          : kPalette_[std::rand() % kPalette_Count];
        return e;
    }
};

// Actor+0x19C (invulnerability/hurtEffects) read lives in this POD __try frame
// so OnRender stays C++ unwind-safe (C2712).
static int chamsReadHurtTime(SDK::Actor* a) {
    int hurt = 0;
    __try { hurt = *reinterpret_cast<int*>((uintptr_t)a + 0x19C); }
    __except (EXCEPTION_EXECUTE_HANDLER) { hurt = 0; }
    return hurt < 0 ? 0 : hurt;
}

void SuperHeroFX::OnRender() {
    if (!IsEnabled()) return;
    ImGuiIO& io = ImGui::GetIO();
    const float dt = (io.DeltaTime > 0.0f && io.DeltaTime < 0.25f) ? io.DeltaTime : 1.0f / 60.0f;

    const bool onDamage  = static_cast<BoolSetting*>(mSettings[0])->mValue;
    const bool onProx    = static_cast<BoolSetting*>(mSettings[1])->mValue;
    const bool onIdle    = static_cast<BoolSetting*>(mSettings[2])->mValue;
    const float radius   = static_cast<NumberSetting*>(mSettings[3])->mValue;
    const float life     = static_cast<NumberSetting*>(mSettings[4])->mValue;
    const float rise     = static_cast<NumberSetting*>(mSettings[5])->mValue;
    const float fontSize = static_cast<NumberSetting*>(mSettings[6])->mValue;
    const bool rainbow   = static_cast<BoolSetting*>(mSettings[7])->mValue;
    const float idleInt  = static_cast<NumberSetting*>(mSettings[8])->mValue;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    SDK::Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
    SDK::Level* level = nullptr;
    if (ci) {
        SDK::Minecraft* mc = ci->getMinecraft();
        if (mc) level = mc->getLevel();
    }
    Vec3 myPos{};
    bool haveMyPos = lp && SDK::sehGetPos(lp, &myPos);

    // ---- spawn triggers -----------------------------------------------------
    if (haveMyPos) {
        unsigned long long now = GetTickCount64();
        if ((onDamage || onProx) && level) {
            std::vector<SDK::Actor*> actors;
            if (SDK::sehGetActorList(level, &actors)) {
                static int rainbowIdx = 0;
                for (SDK::Actor* a : actors) {
                    if (!a || a == lp) continue;
                    Vec3 p{};
                    if (!SDK::sehGetPos(a, &p)) continue;
                    float dx = p.x - myPos.x, dy = p.y - myPos.y, dz = p.z - myPos.z;
                    float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 > radius * radius) continue;

                    // Hurt edge (rising edge only; one pulse per hit, no
                    // re-trigger during invulnerability decay). The Actor+0x19C
                    // read lives in chamsReadHurtTime below so OnRender stays C++
                    // unwind-safe (C2712).
                    if (onDamage) {
                        int hurt = chamsReadHurtTime(a);
                        bool prevHurting = false;
                        bool recorded = false;
                        for (auto& s : mActorSeen) {
                            if (s.actor == (void*)a) { prevHurting = (s.lastHurt > 0); s.lastHurt = hurt; recorded = true; break; }
                        }
                        if (!recorded) mActorSeen.push_back({ (void*)a, now, hurt });
                        if (hurt > 0 && !prevHurting) {
                            // Spawn within the actor's own AABB bounds (typical
                            // player 0.6x1.8x0.6): x/z ±0.7, y 0..1.8. No
                            // upward rise — text stays near the actor.
                            float cx = p.x + fxRandRange(-0.7f, 0.7f);
                            float cy = p.y + fxRandRange(0.0f, 1.8f);
                            float cz = p.z + fxRandRange(-0.7f, 0.7f);
                            mFX.push_back(SuperHeroFXSpawnAccess::make(cx, cy, cz, life, 0.0f,
                                              rainbow, rainbowIdx++,
                                              std::rand() % kWords_Count));
                        }
                    }
                    if (onProx) {
                        if (now - mLastProximityMs > 800) {
                            mLastProximityMs = now;
                            static int proxRain = 0;
                            // Spawn stays inside a tight ring around the actor
                            // (no up/down offset beyond the actor's Y extent).
                            mFX.push_back(SuperHeroFXSpawnAccess::make(
                                p.x + fxRandRange(-1.0f, 1.0f), p.y + fxRandRange(0.0f, 1.8f),
                                p.z + fxRandRange(-1.0f, 1.0f), life, 0.0f, rainbow, proxRain++,
                                std::rand() % kWords_Count));
                        }
                    }
                }
            }
        }
        // Random idle: chance-burst around the LOCAL player on a rolling timer.
        if (onIdle) {
            if (mNextIdleMs == 0)
                mNextIdleMs = now + (unsigned long long)(idleInt * 1000.0f * fxRandRange(0.5f, 1.5f));
            if (now >= mNextIdleMs) {
                mNextIdleMs = now + (unsigned long long)(idleInt * 1000.0f * fxRandRange(0.5f, 1.5f));
                static int idleRain = 0;
                int bursts = 1 + (int)(fxRand01() * 2.99f);
                for (int i = 0; i < bursts; ++i) {
                    float a = fxRandRange(0.0f, 6.2831f);
                    float r = fxRandRange(0.3f, 1.5f);
                    mFX.push_back(SuperHeroFXSpawnAccess::make(
                        myPos.x + std::cos(a) * r, myPos.y + fxRandRange(0.0f, 1.8f),
                        myPos.z + std::sin(a) * r, life, 0.0f, rainbow, idleRain++,
                        std::rand() % kWords_Count));
                }
            }
        }
    }

    // ---- advance + render ---------------------------------------------------
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float mvp[16];
    Vec3 origin(0, 0, 0);
    bool haveMV = ci && acquireCameraMVP(ci, mvp, origin);
    ImVec2 screen = io.DisplaySize;
    for (size_t i = 0; i < mFX.size();) {
        Effect& e = mFX[i];
        e.age += dt;
        if (e.age >= e.life) { mFX.erase(mFX.begin() + i); continue; }
        const float t = e.age / e.life;
        // No upward rise — the effect lives NEAR the actor's AABB (spawned
        // within the pre-set randRange around the actor center + small
        // constant on sway only). The slight sway on x/z keeps the effect
        // from stacking when several fire at once.
        e.wx += std::sin(e.age * 3.0f + e.swayPhase) * 0.005f;
        e.wz += std::cos(e.age * 2.6f + e.swayPhase) * 0.005f;
        if (haveMV) {
            ImVec2 s;
            if (worldToScreen(Vec3(e.wx, e.wy, e.wz), mvp, origin, screen, &s)) {
                float a = (t < 0.10f) ? (t / 0.10f)
                      : (t > 0.50f) ? (1.0f - (t - 0.50f) / 0.50f)
                      : 1.0f;
                float scale = 0.75f + std::min(t * 2.0f, 1.0f) * 0.25f;
                ImFont* font = RenderManager::GetInstance().bangersFont;
                if (!font) font = ImGui::GetFont();
                const std::string lbl(kWords_[e.word]);
                const float fs = fontSize * scale * mScale;
                ImVec2 centerAnchor(s.x, s.y - fs * 0.5f);
                // Push the Bangers comic font for the FX labels (deployed by
                // AssetDeployer). ImRenderUtils::drawText feeds off the CURRENT
                // font, so we push before the call and reset after.
                ImGui::PushFont(font, fs);
                float w = ImRenderUtils::getTextWidth(&lbl, fs);
                ImRenderUtils::drawText(ImVec2(centerAnchor.x - w * 0.5f, centerAnchor.y), lbl,
                                        e.color, fs, a, true, dl);
                ImGui::PopFont();
            }
        }
        ++i;
    }
}
