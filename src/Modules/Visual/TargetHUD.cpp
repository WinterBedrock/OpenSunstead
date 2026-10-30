#include "TargetHUD.h"
#include "../../Utils/MCSDK.h"
#include "../../Render/FrameView.h"
#include "../../SDK/Render/BoxOverlay.h"
#include "../Setting.h"
#include "../ModuleManager.h"
#include "../../Render/HudEditor.h"
#include "Chat.h"
#include "../Misc/AntiBot.h"
#include "../Combat/Aura.h"
#include "../Combat/CombatSignal.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include "../../Utils/MiscUtils/SkinUtils.hpp"
#include <d3d11.h>
#include <vector>
#include <algorithm>
#include <imgui.h>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <Windows.h>

// ---------------------------------------------------------------------------
// All native reads are SEH-guarded (POD-only frames) so a recycled/garbage
// actor can never fault the render thread. Mirrors the Aura/NameTags helpers.
// ---------------------------------------------------------------------------
namespace {

// Context resolution is SDK::resolveContext (SDK/Client/GameContext.h).

// sehGetActorList / sehIsPlayer / sehGetPos are consolidated in
// SDK::GameContext and resolved via ADL on SDK::Actor.

// Player::getSkin() lives at Player+0xAB0 -> ref -> wrapper -> SkinImage @wrapper+0xA0
SDK::mce::Image* sehImageFromActor(SDK::Actor* actor) {
    if (!actor) return nullptr;
    __try {
        if (!sdk_safe_ptr(actor, 0xAB0 + sizeof(void*))) return nullptr;
        uintptr_t ref = *reinterpret_cast<uintptr_t*>((uintptr_t)actor + 0xAB0);
        if (!ref || !sdk_safe_ptr((void*)ref, sizeof(void*))) return nullptr;
        uintptr_t wrapper = *reinterpret_cast<uintptr_t*>(ref);
        if (!wrapper || !sdk_safe_ptr((void*)wrapper, 0xA0 + sizeof(void*))) return nullptr;
        return reinterpret_cast<SDK::mce::Image*>(wrapper + 0xA0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Head decode: the image pointer is resolved under SEH; the decode itself runs
// outside the __try because extractHeadRGBA returns a std::vector by value
// (C2712 forbids C++ object unwinding inside a __try frame).
std::vector<unsigned char> sehHeadRGBA(SDK::Actor* actor, int outSize) {
    SDK::mce::Image* img = sehImageFromActor(actor);
    if (!img) return {};
    return SkinUtils::extractHeadRGBA(*img, outSize);
}

// getHealth() validates all pointers internally; the optional<float> it returns
// must live outside a __try frame, so no SEH wrapper is needed here.

// Project an actor's AABB to a screen-space rect (tl/br). Returns false when
// the player is entirely behind the camera or the projection is degenerate —
// the caller then falls back to the HUD-editor position. Box recipe stays
// local (sehGetActorBox); the projection loop is the shared BoxOverlay one.
bool projectActorScreenRect(SDK::Actor* actor, const float* mvp, const Vec3& origin,
                            const ImVec2& screen, ImVec2* tl, ImVec2* br) {
    Vec3 mn, mx;
    if (!sehGetActorBox(actor, &mn, &mx)) return false;
    if (SDK::BoxOverlay::projectBoxRect(mn, mx, mvp, origin, screen, tl, br) == 0)
        return false;
    if (br->x - tl->x < 1.0f || br->y - tl->y < 1.0f) return false;
    return true;
}

// SEH POD helper: reads Actor::invulnerableTime (the Mob::hurtEffects timer,
// Actor+0x19C per Latite's SDK — their JS getHurtTime returns this field).
// It advances whenever the actor actually takes damage, which is how the
// head hit-animation detects real landed hits instead of raw clicks.
static bool sehReadHurtTime(SDK::Actor* actor, int* out) {
    __try {
        if (!actor || !sdk_safe_ptr(actor, 0x1A0)) return false;
        *out = *reinterpret_cast<int*>((uintptr_t)actor + 0x19C);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

TargetHUD::~TargetHUD() {
    if (mHeadTex) { SkinUtils::retireTexture(mHeadTex); mHeadTex = nullptr; }
}

void TargetHUD::UpdateSettingsVisibility() {
    // Range only matters when we are free to pick any nearby player.
    if (mSettings.size() > 17) {
        bool onlyAura = static_cast<BoolSetting*>(mSettings[17])->mValue;
        mSettings[0]->mVisible = !onlyAura;
    }
}

void TargetHUD::OnRender() {
    if (!IsEnabled()) return;
    if (!HudEditor::HudVisible()) return;

    NumberSetting* rangeS   = static_cast<NumberSetting*>(mSettings[0]);
    BoolSetting*   showHpS  = static_cast<BoolSetting*>(mSettings[1]);
    BoolSetting*   showDstS = static_cast<BoolSetting*>(mSettings[2]);
    BoolSetting*   antiBotS = static_cast<BoolSetting*>(mSettings[3]);
    NumberSetting* blurS    = static_cast<NumberSetting*>(mSettings[4]);
    NumberSetting* dimS     = static_cast<NumberSetting*>(mSettings[5]);
    NumberSetting* roundS   = static_cast<NumberSetting*>(mSettings[6]);
    ColorSetting*  accentS  = static_cast<ColorSetting*>(mSettings[7]);
    // [8] Show Head is read further down.
    EnumSetting*   hpStyleS = static_cast<EnumSetting*>(mSettings[9]);
    BoolSetting*   hpNumS   = static_cast<BoolSetting*>(mSettings[10]);
    BoolSetting*   borderS  = static_cast<BoolSetting*>(mSettings[11]);
    BoolSetting*   hitAnimS = static_cast<BoolSetting*>(mSettings[12]);
    NumberSetting* barHS    = static_cast<NumberSetting*>(mSettings[13]);
    BoolSetting*   customAcS= static_cast<BoolSetting*>(mSettings[14]);
    NumberSetting* minWS    = static_cast<NumberSetting*>(mSettings[15]);
    EnumSetting*   posTypeS = static_cast<EnumSetting*>(mSettings[16]);
    BoolSetting*   onlyAuraS= static_cast<BoolSetting*>(mSettings[17]);

    // Accent color: theme by default, or the user's Accent Color setting when
    // "Custom Accent" is on. Used for the border pulse, glow and (optionally)
    // the health bar.
    auto accentColor = [&](float hueShift) -> ImColor {
        if (customAcS->mValue)
            return ImColor(accentS->mValue[0], accentS->mValue[1], accentS->mValue[2], accentS->mValue[3]);
        return ColorUtils::getThemedColor(hueShift);
    };

    // Rise-style HUD behaviour: on the chat/pause surfaces (INCLUDING our own
    // custom chat menu — it keeps the hud_screen but opens a UI surface) a
    // TargetHUD with no target nearby shows OUR OWN player instead, easing in
    // with the normal appear animation; when the surface closes it lingers ~1s
    // and then eases away. The HUD editor mode is gone — this replaces its
    // placeholder.
    const std::string& screenName = SDK::ScreenView::currentScreenName();
    const bool menuScreen = (screenName == "chat_screen" || screenName == "pause_screen" ||
                             (screenName == "hud_screen" && ::Chat::IsOpen()));
    static float menuHold = 0.0f;   // seconds of self-view left after leaving the menu
    {
        float dt0 = ImGui::GetIO().DeltaTime;
        if (dt0 <= 0.0f) dt0 = 1.0f / 60.0f;
        if (menuScreen) menuHold = 1.0f;
        else if (menuHold > 0.0f) menuHold -= dt0;
    }
    const bool selfShow = menuHold > 0.0f;

    // ---- Resolve the target ---------------------------------------------
    // Only On Aura (default): require Aura enabled and pick the same way Aura
    // does (nearest player inside Aura's range). Off: original nearest-in-Range.
    std::string targetName;
    float targetHealth = -1.0f;
    float targetDist   = 0.0f;
    bool  haveTarget   = false;
    SDK::Actor* targetActor = nullptr;

    SDK::ClientInstance* ci = nullptr; SDK::Level* level = nullptr; SDK::Actor* lp = nullptr;
    if (SDK::resolveContext(&ci, &level, &lp)) {
        bool onlyOnAura = onlyAuraS->mValue;
        Aura* aura = static_cast<Aura*>(ModuleManager::GetInstance().GetModuleByName("Aura"));
        bool auraOk = aura && aura->IsEnabled();

        // When Only On Aura is on and Aura is off / missing, never show a
        // target (editor placeholder still works via editorOpen below).
        bool canSearch = !onlyOnAura || auraOk;

        if (canSearch) {
            Vec3 myPos{};
            if (sehGetPos(lp, &myPos)) {
                float range = onlyOnAura ? aura->GetRange() : rangeS->mValue;
                float best2 = range * range;

                AntiBot* antiBot = nullptr;
                if (antiBotS->mValue)
                    antiBot = static_cast<AntiBot*>(ModuleManager::GetInstance().GetModuleByName("AntiBot"));

                std::vector<SDK::Actor*> actors;
                if (sehGetActorList(level, &actors)) {
                    SDK::Actor* nearest = nullptr;
                    for (SDK::Actor* a : actors) {
                        if (!a || a == lp) continue;
                        if (!sehIsPlayer(a)) continue;
                        if (antiBot && antiBot->IsBot(a)) continue;
                        Vec3 p{};
                        if (!sehGetPos(a, &p)) continue;
                        float dx = p.x - myPos.x, dy = p.y - myPos.y, dz = p.z - myPos.z;
                        float d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 < best2) { best2 = d2; nearest = a; }
                    }
                    if (nearest) {
                        haveTarget   = true;
                        targetActor  = nearest;
                        targetName   = nearest->getRawName();
                        targetHealth = SDK::sehGetHealth(nearest);
                        targetDist   = std::sqrt(best2);
                    }
                }
            }
        }
    }

    // No real target in the menu (or its 1s linger): show OUR OWN player, the
    // Rise HUD-editor behaviour. The fade then eases in/out with the panel's
    // normal zoom animation instead of popping.
    if (!haveTarget && selfShow && lp) {
        haveTarget   = true;
        targetActor  = lp;
        targetName   = lp->getRawName();
        targetHealth = SDK::sehGetHealth(lp);
        targetDist   = 0.0f;
    }

    // Only bail entirely once the panel is fully faded out. The panel drives
    // its fade toward 0 while easing away, so the target loss is always
    // animated; below the threshold there is nothing left to draw.
    if (!haveTarget && mFade < 0.01f) {
        mFade = 0.0f;
        if (mHeadTex) { SkinUtils::retireTexture(mHeadTex); mHeadTex = nullptr; mHeadName.clear(); }
        return;
    }

    // ---- Linger: hold the panel for a second after the target is lost -----
    // The fade-out animation only starts once the linger expires, so the
    // panel visibly hangs on the last target instead of snapping away.
    if (haveTarget) mLingerMs = GetTickCount64();
    const bool linger = !haveTarget && mLingerMs != 0 &&
                        (GetTickCount64() - mLingerMs) < 1000ull;

    // ---- Smooth show/hide fade (eased both directions) -------------------
    float fadeTarget = (haveTarget || linger) ? 1.0f : 0.0f;
    mFade += (fadeTarget - mFade) * 0.18f;

    // Keep the last real target name so the fade-out has content to show.
    if (haveTarget && !targetName.empty()) mLastName = targetName;
    if (!haveTarget) targetName = mLastName;

    // ---- Player head texture (decode once per target; retry while loading) --
    if (static_cast<BoolSetting*>(mSettings[8])->mValue && haveTarget && targetActor) {
        if (targetName != mHeadName) {
            if (mHeadTex) { SkinUtils::retireTexture(mHeadTex); mHeadTex = nullptr; }
            mHeadName = targetName;
            mHeadTryCd = 0;
        }
        if (!mHeadTex) {
            if (mHeadTryCd <= 0) {
                mHeadTryCd = 15; // retry cadence until the server skin streams in
                std::vector<unsigned char> rgba = sehHeadRGBA(targetActor, 64);
                if (!rgba.empty()) mHeadTex = SkinUtils::createTextureRGBA(rgba.data(), 64, 64);
            } else { --mHeadTryCd; }
        }
    }

    // ---- Hit feedback: driven by the TARGET's hurt EFFECT, not our clicks.
    // Actor+0x19C (invulnerableTime / Mob::hurtEffects — Latite's getHurtTime)
    // holds the remaining invulnerability while the hurt state runs. CRITICAL:
    // that timer DECREMENTS every tick (10, 9, 8, ...) — firing on "value
    // changed" re-pulsed the animation for the WHOLE decay (the "100 hit
    // animations per hurt"). The only real event is the transition from NOT
    // hurting to hurting; invulnerability gates damage, so every new hit
    // passes through 0 first and re-arms the edge. One pulse per one hit.
    if (haveTarget && targetActor) {
        if (mSeenTarget != (void*)targetActor) {
            mSeenTarget = (void*)targetActor;
            mWasHurting = false;
            mHurtSinceMs = 0;
        }
        int hurt = 0;
        if (sehReadHurtTime(targetActor, &hurt)) {
            if (hurt < 0) hurt = 0;
            const bool hurting = hurt > 0;
            const unsigned long long now = GetTickCount64();
            if (hurting && !mWasHurting && hitAnimS->mValue) {
                mHitPulse = 1.0f;
                mHurtSinceMs = now;
            }
            // Stale-offset guard: on a build where +0x19C does not decay to 0
            // (wrong field, or a server that keeps invulnerability pinned), the
            // rising edge would fire once and then never again. If the run has
            // been "hurting" for over 1.5 s, re-arm the edge so held combos
            // still pulse at a sane rate instead of freezing after one hit.
            if (hurting && mWasHurting && mHurtSinceMs != 0 &&
                now - mHurtSinceMs > 1500ull && now - mStallPulseMs > 1500ull) {
                if (hitAnimS->mValue) mHitPulse = 1.0f;
                mStallPulseMs = now;
            }
            if (!hurting) { mHurtSinceMs = 0; mStallPulseMs = 0; }
            mWasHurting = hurting;
        }
    }
    mHitPulse += (0.0f - mHitPulse) * 0.10f; // decay toward 0
    if (mHitPulse < 0.001f) mHitPulse = 0.0f;

    if (mFade < 0.01f) return;

    if (haveTarget) {
        if (targetHealth < 0.0f) targetHealth = 20.0f;
        // The readouts ease (never snap) toward the live values. During a
        // target-switch transition they lerp faster so the new target's HP /
        // distance settle inside the ~250 ms slide instead of trailing it.
        const float lerp = (mSwitchAge < 0.25f) ? 0.35f : 0.25f;
        mDispHealth += (targetHealth - mDispHealth) * lerp;
        mDispDist   += (targetDist   - mDispDist)   * lerp;
    }

    if (targetName.empty()) targetName = "Player";

    // ---- Target-switch detection (animated swap) --------------------------
    // An identity change restarts the transition clock: the incoming name
    // slides in + fades in over ~250 ms (see the name draw at the bottom),
    // and the health/dist readouts keep easing from the previous target's
    // values instead of snapping, so the whole card morphs to the new target.
    // First ever frame (mShownName empty) snaps — nothing to animate from.
    {
        float sdt = ImGui::GetIO().DeltaTime;
        if (sdt > 0.0f && sdt < 0.1f) mSwitchAge += sdt;
        if (haveTarget && targetName != mShownName) {
            if (!mShownName.empty()) mSwitchAge = 0.0f; // start the transition
            mShownName = targetName;
        }
    }

    // ---- Layout ----------------------------------------------------------
    float scale     = mScale;
    float pad       = 10.0f * scale;
    float nameSize  = 19.0f * scale;
    float infoSize  = 15.0f * scale;
    float barH      = barHS->mValue * scale;
    float rounding  = roundS->mValue * scale;

    bool showHp  = showHpS->mValue;
    bool showDst = showDstS->mValue;

    bool  reserveHead = static_cast<BoolSetting*>(mSettings[8])->mValue;
    // Bigger head that hugs the LEFT EDGE of the panel — only a thin sliver of
    // padding separates it from the border (the old 32px head floating a full
    // pad inside read as lost in the middle).
    float headSize    = reserveHead ? 42.0f * scale : 0.0f;
    float headInset   = reserveHead ? pad * 0.30f : 0.0f;
    float headGap     = reserveHead ? pad * 0.8f : 0.0f;

    float nameW = ImRenderUtils::getTextWidth(&targetName, nameSize);
    float textW = nameW;
    float width = headInset + headSize + headGap + textW + pad * 1.5f;
    if (width < minWS->mValue * scale) width = minWS->mValue * scale;

    float height = pad + nameSize + pad * 0.4f;
    if (showHp)  height += barH + 4.0f * scale;
    if (showDst) height += infoSize + 2.0f * scale;
    height += pad * 0.6f;
    if (reserveHead && height < headSize + pad * 1.0f) height = headSize + pad * 1.0f;

    mWidth  = width;   // base hitbox for the HUD editor (unaffected by anim)
    mHeight = height;

    // ---- Panel top-left: HudEditor fixed pos, or spring-follow the player ---
    // mPosX/mPosY always stay as the HUD-editor coordinates (drag/save). The
    // actual draw origin (drawX/drawY) may live on the player when Position Type
    // is "Player", with a smooth spring + blend back when they go behind us.
    float drawX = mPosX;
    float drawY = mPosY;
    const int posType = posTypeS->mValue; // 0 = HudEditor, 1 = Player
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    if (dt > 0.05f) dt = 0.05f;

    if (posType == 1 && !selfShow) {
        float desiredX = mPosX;
        float desiredY = mPosY;
        bool  onPlayer = false;

        if (haveTarget && targetActor && ci) {
            float mvp[16];
            Vec3 origin{};
            if (acquireCameraMVP(ci, mvp, origin)) {
                ImVec2 screen = ImGui::GetIO().DisplaySize;
                ImVec2 tl, br;
                if (projectActorScreenRect(targetActor, mvp, origin, screen, &tl, &br)) {
                    // Right of the player, slightly overlapping the body so the
                    // panel feels attached rather than floating next to them.
                    desiredX = br.x - width * 0.18f;
                    desiredY = (tl.y + br.y) * 0.5f - height * 0.5f;
                    // Keep a little margin from the screen edges.
                    if (desiredX < 4.0f) desiredX = 4.0f;
                    if (desiredY < 4.0f) desiredY = 4.0f;
                    if (desiredX + width  > screen.x - 4.0f) desiredX = screen.x - width  - 4.0f;
                    if (desiredY + height > screen.y - 4.0f) desiredY = screen.y - height - 4.0f;
                    onPlayer = true;
                }
            }
        }

        // Soft blend flag: eases toward 1 while on-player, back to 0 when the
        // target is behind / lost so we glide to the HUD-editor fallback.
        float blendTarget = onPlayer ? 1.0f : 0.0f;
        mOnPlayerBlend += (blendTarget - mOnPlayerBlend) * (1.0f - std::exp(-10.0f * dt));
        if (mOnPlayerBlend < 0.001f) mOnPlayerBlend = 0.0f;
        if (mOnPlayerBlend > 0.999f) mOnPlayerBlend = 1.0f;

        // Desired position blends player attach <-> editor pos.
        float goalX = mPosX + (desiredX - mPosX) * mOnPlayerBlend;
        float goalY = mPosY + (desiredY - mPosY) * mOnPlayerBlend;

        if (!mFollowInit) {
            mFollowX = goalX; mFollowY = goalY;
            mFollowVelX = mFollowVelY = 0.0f;
            mFollowInit = true;
        }

        // Spring-damper follow: slightly lags behind movement, settles with a
        // soft ease (no hard snap when the player strafes or jumps).
        const float stiffness = 90.0f;
        const float damping   = 16.0f;
        float ax = stiffness * (goalX - mFollowX) - damping * mFollowVelX;
        float ay = stiffness * (goalY - mFollowY) - damping * mFollowVelY;
        mFollowVelX += ax * dt;
        mFollowVelY += ay * dt;
        mFollowX += mFollowVelX * dt;
        mFollowY += mFollowVelY * dt;

        drawX = mFollowX;
        drawY = mFollowY;
    } else {
        // HudEditor mode (or editor open): pin to mPosX/mPosY and reset the
        // spring so the next Player-mode entry starts from the editor pos.
        mFollowInit = false;
        mFollowVelX = mFollowVelY = 0.0f;
        mOnPlayerBlend = 0.0f;
        drawX = mPosX;
        drawY = mPosY;
    }

    // Show/hide animation: a ZOOM (scale 0.86 -> 1.0 about the panel center) as
    // it fades in, unwound as it fades out, plus a subtle slide and the per-hit
    // scale "pop". The drawing rect carries all of this; the editor hitbox
    // (mWidth/mHeight above) stays at the base size so dragging is stable.
    float az    = 0.86f + 0.14f * mFade;
    float slide = -(1.0f - mFade) * 14.0f * scale;
    // Extra motion juice in Player mode: tiny stretch driven by follow velocity.
    float followSpeed = (posType == 1)
        ? std::sqrt(mFollowVelX * mFollowVelX + mFollowVelY * mFollowVelY) : 0.0f;
    float stretchAmt = followSpeed * 0.0008f;
    if (stretchAmt > 0.05f) stretchAmt = 0.05f;
    float stretch = 1.0f + (followSpeed > 0.0f ? stretchAmt : 0.0f);
    float pop   = mHitPulse * 5.0f * scale;
    float dWidth  = width  * az * stretch;
    float dHeight = height * az;
    float ccx = drawX + width  * 0.5f + slide;
    float ccy = drawY + height * 0.5f;
    ImVec4 rect(ccx - dWidth * 0.5f - pop, ccy - dHeight * 0.5f - pop,
                ccx + dWidth * 0.5f + pop, ccy + dHeight * 0.5f + pop);
    // FOREGROUND list: the backdrop blur below goes on the BACKGROUND list, and
    // ImGui always replays background before foreground, so this ordering is what
    // guarantees the blur lands underneath the panel and its particles instead of
    // painting over them.
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    // Zoom-scaled draw metrics so text/bars/head grow with the panel.
    float dpad      = pad      * az;
    float dNameSize = nameSize * az;
    float dInfoSize = infoSize * az;
    float dBarH     = barH     * az;
    float dRound    = rounding * az;
    float dHead     = headSize * az;
    float dHeadInset = headInset * az;
    float dGap      = headGap  * az;

    int   alpha = (int)(255.0f * mFade);
    float dim   = dimS->mValue * mFade;
    ImColor themed = accentColor(0.0f);

    // ---- Backdrop blur, emitted FIRST and onto a different list -------------
    // A D2D blur is a draw callback that paints an OPAQUE blurred rect straight
    // onto the backbuffer at replay time — it covers pixels, it does not blend
    // with them. So it must never be recorded after anything it should sit
    // behind. Two rules keep that true here:
    //   * it is emitted before this panel's own particles and plate, and
    //   * it goes on the BACKGROUND list while the panel draws on the FOREGROUND
    //     list, so it also cannot erase Watermark/TabList/Coordinates, which all
    //     draw to the background list earlier in the module order.
    // Blur strength is scaled by mFade so the frosted backdrop eases out WITH
    // the panel instead of staying full-strength until mFade snaps under the
    // cull threshold (that snap was the "lingering blur" artifact on fade-out).
    if (mFade >= 0.01f) {
        bool guiOpen = false;
        if (Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI"))
            guiOpen = gui->IsEnabled();
        const float blurStrength = blurS->mValue * mFade;
        if (blurStrength > 0.05f && !guiOpen) {
            D2D::addBlurOptimized(ImGui::GetBackgroundDrawList(), blurStrength,
                                  std::optional<ImVec4>(rect), dRound);
        }
    }

    // (The old hit-particle burst is gone — the hit feedback is the head
    // animation alone: a sideways tip + red flush on the target's head.)

    // If the panel itself is invisible we only needed the layout pass.
    if (mFade < 0.01f) return;

    // ---- Background: dim plate + hit glow + border -----------------------
    // Flat Rise-style plate: no pseudo-3D bevels (the old white-top/black-bottom
    // overlays read as fake depth), just the dim fill, the shadow and an
    // optional border. The blur that used to live here now runs at the top of
    // this function, before any geometry.
    ImRenderUtils::fillShadowRectangle(rect, ImColor(0, 0, 0), 0.4f * mFade, dRound, 10.0f * scale);
    // Themed glow flash that pulses outward on each landed hit.
    if (mHitPulse > 0.01f)
        ImRenderUtils::fillShadowRectangle(rect, themed, mHitPulse * 0.6f * mFade, dRound, 16.0f * scale);
    ImRenderUtils::fillRectangle(rect, ImColor(12, 12, 15, (int)(255 * dim)), 1.0f, dRound);
    // Border lerps white -> accent color as the hit pulse rises (optional).
    if (borderS->mValue) {
        ImColor borderCol = ColorUtils::LerpImColor(ImColor(1.0f, 1.0f, 1.0f, 1.0f), themed, mHitPulse);
        borderCol.Value.w = (0.10f + mHitPulse * 0.75f) * mFade;
        drawList->AddRect(ImVec2(rect.x, rect.y), ImVec2(rect.z, rect.w), borderCol, dRound, 0, 1.0f + mHitPulse * 1.5f);
    }

    // ---- Player head (left, hugging the panel edge) ----------------------
    if (reserveHead) {
        float hx = rect.x + dHeadInset;
        float hy = rect.y + ((rect.w - rect.y) - dHead) * 0.5f; // vertically centered
        ImVec4 hrect(hx, hy, hx + dHead, hy + dHead);
        // Hit feedback lives ON the head icon: on a landed hit (the target's
        // hurt timer rising) the head TIPS SIDEWAYS a few degrees around its
        // center and flushes red, then springs back — no particles, no panel
        // pop. The rotation is baked into the vertex positions below.
        const float tiltDeg = mHitPulse * 14.0f;                 // lean angle at impact
        const float tilt = tiltDeg * 0.017453293f;               // radians
        const float cs = std::cos(tilt), sn = std::sin(tilt);
        const float hcx = (hrect.x + hrect.z) * 0.5f;
        const float hcy = (hrect.y + hrect.w) * 0.5f;
        const float e = dHead * 0.5f;
        // White -> red as the hit pulse rises, so the face flushes on damage.
        ImColor headTint = ColorUtils::LerpImColor(
            ImColor(1.0f, 1.0f, 1.0f, 1.0f), ImColor(1.0f, 0.32f, 0.32f, 1.0f), mHitPulse);
        headTint.Value.w = mFade;
        if (mHeadTex) {
            if (std::fabs(tiltDeg) < 0.5f) {
                // Rounded head at rest — a proper circular-corner cut (the
                // same look NameTags' head icons have).
                drawList->AddImageRounded((ImTextureID)mHeadTex,
                                          ImVec2(hcx - e, hcy - e), ImVec2(hcx + e, hcy + e),
                                          ImVec2(0, 0), ImVec2(1, 1), headTint,
                                          dHead * 0.22f);
            } else {
                // THE HIT-ANIMATION FIX, ROUND 2: the first attempt built the
                // rotated rounded-rect with a hand-rolled PrimReserve vertex
                // block. The vertices were provably correct, but the head still
                // vanished for every tilted frame on live builds — the manual
                // path relies on assumptions about the draw-list state that the
                // rest of this pipeline (D2D callbacks, command splitting)
                // evidently breaks. So the tilt path now uses ONLY the exact
                // primitives the WORKING at-rest path uses inside
                // ImDrawList::AddImageRounded: PushTexture -> Path* ->
                // PathFillConvex -> ShadeVertsLinearUV -> PopTexture. The only
                // difference from AddImageRounded is that the path points are
                // rotated around the head centre before PathFillConvex sees
                // them, and the UVs are remapped against the unrotated box so
                // the face does not visibly spin inside its frame at <=14 deg.
                const float r = std::min(dHead * 0.22f, e * 0.9f);   // corner radius
                const float rx = e - r, ry = e - r;                  // arc centres offset
                const float cAng[4][2] = { {  rx, -ry }, {  rx,  ry }, { -rx,  ry }, { -rx, -ry } };
                const float a0[4] = { -1.5707963f, 0.0f, 1.5707963f, 3.1415926f };
                drawList->PushTextureID((ImTextureID)mHeadTex);
                const int vtxStart = drawList->VtxBuffer.Size;
                drawList->PathClear();
                for (int k = 0; k < 4; ++k) {
                    for (int s = 0; s <= 4; ++s) {
                        const float a = a0[k] + 1.5707963f * ((float)s / 4.0f);
                        const float lx = cAng[k][0] + r * std::cos(a);
                        const float ly = cAng[k][1] + r * std::sin(a);
                        const float wx = lx * cs - ly * sn;
                        const float wy = lx * sn + ly * cs;
                        drawList->PathLineTo(ImVec2(hcx + wx, hcy + wy));
                    }
                }
                drawList->PathFillConvex(ImGui::GetColorU32(headTint.Value));
                const int vtxEnd = drawList->VtxBuffer.Size;
                // Map UV 0..1 over the UNROTATED head box (same contract as
                // AddImageRounded) — a <=14 deg lean shears the UVs by at most
                // a couple of pixels, invisible on a Minecraft face.
                ImGui::ShadeVertsLinearUV(drawList, vtxStart, vtxEnd,
                                          ImVec2(hcx - e, hcy - e), ImVec2(hcx + e, hcy + e),
                                          ImVec2(0, 0), ImVec2(1, 1), true);
                drawList->PopTextureID();
            }
        } else {
            // Placeholder plate while the skin streams in (same spot, no tilt).
            ImRenderUtils::fillRectangle(ImVec4(hcx - e, hcy - e, hcx + e, hcy + e),
                                         ImColor(30, 30, 36, (int)(160 * mFade)), 1.0f, dRound * 0.6f);
        }
        drawList->AddRect(ImVec2(hcx - e - 1.5f, hcy - e - 1.5f), ImVec2(hcx + e + 1.5f, hcy + e + 1.5f),
            ImColor(255, 255, 255, (int)(45 * mFade)), dRound * 0.6f, 0, 1.0f);
    }

    // ---- Content ---------------------------------------------------------
    float panelW = rect.z - rect.x;
    float cx = rect.x + dHeadInset + dHead + dGap;
    float cy = rect.y + dpad * 0.6f;

    // Target-switch animation: for ~250 ms after the target identity changed,
    // the incoming name slides in from the right and fades in with an
    // easeOutCubic. The panel rect never moves, so the layout stays stable —
    // only the name's x offset and alpha are animated. Health/dist values
    // animate through mDispHealth/mDispDist (lerped above, never snapped).
    float swT = mSwitchAge / 0.25f;
    if (swT < 0.0f) swT = 0.0f;
    if (swT > 1.0f) swT = 1.0f;
    const float swEase    = 1.0f - (1.0f - swT) * (1.0f - swT) * (1.0f - swT);
    const float nameSlide = (1.0f - swEase) * 12.0f * scale;
    const int   nameAlpha = (int)(alpha * (0.25f + 0.75f * swEase));
    ImRenderUtils::drawText(ImVec2(cx + nameSlide, cy), targetName,
                            ImColor(255, 255, 255, nameAlpha), dNameSize, 1.0f, true);
    cy += dNameSize + dpad * 0.4f;

    if (showHp) {
        // While the target is live the panel eases toward the fresh read. When
        // the target is lost (linger / fade-out) the last real values are kept
        // frozen — never reset to 20 HP / 0 m — until a NEW target is acquired.
        float hp = mDispHealth;
        float frac = hp / 20.0f;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;

        // Rise-style bar: a fully rounded PILL with the theme gradient as the
        // fill. The optional numeric HP sits to the RIGHT of the bar (never
        // overlapping it), and the bar shrinks to make room.
        float numW = 0.0f;
        std::string hs;
        if (hpNumS->mValue) {
            char hb[24];
            snprintf(hb, sizeof(hb), "%.0f", hp);
            hs = hb;
            numW = ImRenderUtils::getTextWidth(&hs, dInfoSize) + 6.0f * scale;
        }
        float barW = (rect.z - dpad) - cx - numW;
        if (barW < 10.0f * scale) barW = 10.0f * scale;
        ImVec4 barBg(cx, cy, cx + barW, cy + dBarH);
        ImRenderUtils::fillRectangle(barBg, ImColor(0, 0, 0, (int)(140 * mFade)), 1.0f, dBarH * 0.5f);

        // 3D volume pass over any filled pill: a lit top fading out, a dark
        // bottom fading in (two vertical rounded gradients), plus a crisp
        // glossy top line — the bar reads as a shaded cylinder, not a flat
        // stripe.
        auto shadeBar = [&](const ImVec4& pill) {
            if (pill.z - pill.x < 1.0f) return;
            ImRenderUtils::fillGradientRounded(drawList, pill,
                ImColor(255, 255, 255, (int)(75 * mFade)), ImColor(255, 255, 255, 0),
                dBarH * 0.5f, 0, /*vertical=*/true);
            ImRenderUtils::fillGradientRounded(drawList, pill,
                ImColor(0, 0, 0, 0), ImColor(0, 0, 0, (int)(95 * mFade)),
                dBarH * 0.5f, 0, /*vertical=*/true);
            drawList->AddLine(ImVec2(pill.x + dBarH * 0.35f, pill.y + 1.0f),
                              ImVec2(pill.z - dBarH * 0.35f, pill.y + 1.0f),
                              ImColor(255, 255, 255, (int)(65 * mFade)), 1.0f);
        };

        int hpStyle = hpStyleS->mValue;
        const bool customAccent = customAcS->mValue;
        if (customAccent) {
            ImColor hpCol = themed;
            hpCol.Value.w = mFade;
            ImVec4 pill(cx, cy, cx + barW * frac, cy + dBarH);
            ImRenderUtils::fillRectangle(pill, hpCol, 1.0f, dBarH * 0.5f);
            shadeBar(pill);
        } else if (hpStyle == 1) {
            // HP heat: green -> yellow -> red by remaining health, rounded pill.
            ImColor hpCol = ColorUtils::LerpImColor(ImColor(0.90f, 0.20f, 0.20f, 1.0f),
                                                    ImColor(0.25f, 0.90f, 0.35f, 1.0f), frac);
            hpCol.Value.w = mFade;
            ImVec4 pill(cx, cy, cx + barW * frac, cy + dBarH);
            ImRenderUtils::fillRectangle(pill, hpCol, 1.0f, dBarH * 0.5f);
            shadeBar(pill);
        } else if (hpStyle == 2) {
            // Segmented pips, each rounded.
            const int segs = 10;
            float gap = 2.0f * scale;
            float segW = (barW - gap * (segs - 1)) / segs;
            for (int i = 0; i < segs; ++i) {
                float sx = cx + i * (segW + gap);
                float segFrac = frac * segs - i;
                if (segFrac <= 0.0f) continue;
                if (segFrac > 1.0f) segFrac = 1.0f;
                ImColor sc = accentColor((float)i / segs * 60.0f);
                sc.Value.w = mFade;
                ImVec4 segR(sx, cy, sx + segW * segFrac, cy + dBarH);
                ImRenderUtils::fillRectangle(segR, sc, 1.0f, dBarH * 0.35f);
            }
        } else {
            // Theme Gradient (default): the theme's live gradient along the
            // filled part, rounded like a pill — then the 3D volume pass.
            ImColor c0 = ColorUtils::getThemedColor(0.0f);
            ImColor c1 = ColorUtils::getThemedColor(60.0f);
            c0.Value.w = mFade; c1.Value.w = mFade;
            ImVec4 pill(cx, cy, cx + barW * frac, cy + dBarH);
            ImRenderUtils::fillGradientRounded(drawList,
                pill, c0, c1, dBarH * 0.5f, 0, /*vertical=*/false);
            shadeBar(pill);
        }

        // Numeric HP beside the bar — no overlap, vertically centered on it.
        if (hpNumS->mValue) {
            ImRenderUtils::drawText(ImVec2(cx + barW + 6.0f * scale, cy + (dBarH - dInfoSize) * 0.5f),
                                    hs, ImColor(255, 255, 255, alpha), dInfoSize, 1.0f, true);
        }
        cy += dBarH + 4.0f * scale;
    }

    if (showDst) {
        char buf[48];
        if (hpNumS->mValue) {
            // HP is already shown on the bar, so keep the info line to distance.
            snprintf(buf, sizeof(buf), "%.1fm", mDispDist);
        } else {
            snprintf(buf, sizeof(buf), "HP %.1f   %.1fm", mDispHealth, mDispDist);
        }
        std::string info(buf);
        ImRenderUtils::drawText(ImVec2(cx, cy), info, ImColor(200, 200, 205, alpha), dInfoSize, 1.0f, true);
    }
}
