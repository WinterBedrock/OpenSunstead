#include "NameTags.h"
#include "../../Core/Logger.h"
#include "../../Render/FrameView.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/Actor/RenderPos.h"
#include "../Setting.h"
#include "../ModuleManager.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include <optional>
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/SkinUtils.hpp"
#include <imgui.h>
#include <d3d11.h>
#include <vector>
#include <string>
#include <Windows.h>
#include <cstdio>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

namespace {

// ---- Robust context + enumeration (mirrors ESP/Tracers/HitBoxes helpers) ----
// sehGetActorList / sehIsPlayer are consolidated in SDK::GameContext (ADL).

// Context resolution is SDK::resolveContext (SDK/Client/GameContext.h).

// worldToScreen is consolidated in SDK/Math.h (global, resolved via ADL).
// Camera MVP/origin acquisition is shared via acquireCameraMVP (FrameView.h).

// SEH-isolated reads. The __try frames below must not create any C++ objects
// with non-trivial destructors, or MSVC raises C2712.
// pos + bounding box in one guarded read: SDK::sehGetPosBox (GameContext.h).

// No __try: getRawName() is already SEH-guarded via actor_vtable_getname_seh,
// and the temporary std::string it returns cannot live inside a __try frame.
// Name + health reads are SDK::sehGetName / SDK::sehGetHealth (GameContext.h).

// Per-actor horizontal speed (blocks/sec), low-pass filtered for a steady read.
static float computeBps(SDK::Actor* a, const Vec3& pos) {
    struct S { Vec3 last; float bps; uint64_t tMs; };
    static std::unordered_map<void*, S> s_map;
    uint64_t now = GetTickCount64();
    auto it = s_map.find(a);
    if (it == s_map.end()) { s_map[a] = { pos, 0.0f, now }; return 0.0f; }
    S& s = it->second;
    float dtMs = (float)(now - s.tMs);
    if (dtMs < 1.0f) return s.bps;       // too soon, reuse last
    float dx = pos.x - s.last.x, dz = pos.z - s.last.z;
    float dist = sqrtf(dx*dx + dz*dz);
    float inst = dist / (dtMs / 1000.0f);
    if (inst > 80.0f) inst = s.bps;      // teleport guard
    s.bps = s.bps + (inst - s.bps) * 0.25f;
    s.last = pos; s.tMs = now;
    return s.bps;
}

static bool ntIsSneaking(SDK::Actor* a) {
    if (!a) return false;
    __try { return a->getStatusFlag(ActorFlags::Sneaking) == 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Latite-proven actor skin path (same as TabList/TargetHUD). Player+0xAB0 ->
// SerializedSkinRef -> wrapper+0xA0 = SkinImage. Avoids g_Actor_mSerializedSkin.
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

// Image pointer under SEH; decode outside (__try forbids std::vector return).
std::vector<unsigned char> sehHeadRGBA(SDK::Actor* actor, int outSize) {
    SDK::mce::Image* img = sehImageFromActor(actor);
    if (!img) return {};
    return SkinUtils::extractHeadRGBA(*img, outSize);
}

} // namespace

static int g_ntFrame = 0;

// --- Selective vanilla-tag suppression -------------------------------------
// Actors the overlay drew a custom label for THIS ImGui frame. The
// renderNametag detour (GameHooks) skips the vanilla tag for exactly these
// actors instead of zeroing the whole batch, so untagged mobs and players
// keep their regular floating nick.
static std::mutex g_tagMutex;
static std::unordered_set<void*> g_tagActors;

bool NameTags::ShouldHideActor(void* actor, bool* fallbackAll) {
    // fallbackAll is deliberately unused now: the hide decision is stable
    // frame-to-frame (see the double-buffered swap in OnRender), so there is
    // no "overlay drew nothing this frame" window that would need a
    // hide-everything fallback. The old fallback is what hid ALL nametags
    // whenever the overlay skipped a frame.
    if (fallbackAll) *fallbackAll = false;
    Module* m = ModuleManager::GetInstance().GetModuleByName("NameTags");
    if (!m || !m->IsEnabled()) return false;
    // [17] Hide Original
    if (m->mSettings.size() <= 17) return false;
    if (!static_cast<BoolSetting*>(m->mSettings[17])->mValue) return false;
    std::lock_guard<std::mutex> lock(g_tagMutex);
    return g_tagActors.count(actor) != 0;
}

bool NameTags::ShouldHideOriginal() {
    Module* m = ModuleManager::GetInstance().GetModuleByName("NameTags");
    if (!m || !m->IsEnabled()) return false;
    // [17] Hide Original
    if (m->mSettings.size() <= 17) return false;
    return static_cast<BoolSetting*>(m->mSettings[17])->mValue;
}

// SRVs queued for Release AFTER ImGui has finished the current frame's Present
// (see flushDeferredHeadReleases). Mid-frame Release of textures still referenced
// by ImDrawList commands freezes the overlay permanently.
static std::vector<void*> g_deferredHeadSrv;

static void deferHeadRelease(void* srv) {
    if (srv) g_deferredHeadSrv.push_back(srv);
}

void NameTags::flushDeferredHeadReleases() {
    for (void* p : g_deferredHeadSrv) {
        if (p) SkinUtils::retireTexture(p);
    }
    g_deferredHeadSrv.clear();
}

void NameTags::clearHeads() {
    for (auto& kv : mHeadTex) deferHeadRelease(kv.second.first);
    mHeadTex.clear();
}

NameTags::~NameTags() {
    // Destructor can free immediately (no live frame).
    for (auto& kv : mHeadTex) {
        if (kv.second.first) SkinUtils::retireTexture(kv.second.first);
    }
    mHeadTex.clear();
    flushDeferredHeadReleases();
}

void NameTags::OnEnable()  { /* overlay-only; original-tag hiding is handled by the renderNametag hook via ShouldHideOriginal() */ }
void NameTags::OnDisable() {
    clearHeads(); /* deferred Release */
    { std::lock_guard<std::mutex> lock(g_tagMutex); g_tagActors.clear(); }
}

void NameTags::OnRender() {
    if (!IsEnabled()) return;
    // "Always show" off (default) -> only render on the in-world HUD screen.
    if (!static_cast<BoolSetting*>(mSettings[18])->mValue &&
        SDK::ScreenView::currentScreenName() != "hud_screen") return;
    bool dbg = (++g_ntFrame % 120) == 0;

    SDK::ClientInstance* ci = nullptr; SDK::Level* level = nullptr; SDK::Actor* lp = nullptr;
    if (!SDK::resolveContext(&ci, &level, &lp)) { if (dbg) LOG_WARN("resolveContext failed", "NameTags"); return; }

    // Camera MVP + origin via the shared Present-time acquisition (live
    // GameRenderer matrices, snapshot fallback, zoom scale) — same projection
    // ESP/Tracers use, so tags and boxes never disagree.
    float mvp[16];
    Vec3 origin(0, 0, 0);
    if (!acquireCameraMVP(ci, mvp, origin)) return;
    bool originValid = (origin.x != 0.0f || origin.y != 0.0f || origin.z != 0.0f);
    if (!originValid) return;

    // Draw at most ONCE per ImGui frame (rendering runs once per Present; the
    // old pass-count deferral could drop frames and make tags stutter).
    static int s_drawnFrame = -1;
    int frame = ImGui::GetFrameCount();
    if (s_drawnFrame == frame) return;
    s_drawnFrame = frame;
    // DOUBLE-BUFFERED tag set: the frame's actor set is built locally and
    // swapped in only when COMPLETE. The game's nametag pass runs on its own
    // cadence and used to read the set mid-rebuild (empty -> the old
    // hide-all fallback fired) or on frames the overlay skipped - that
    // alternation was the rapid visible/not-visible flicker. The last
    // COMPLETE frame's set now stays valid until the next one replaces it.

    std::vector<SDK::Actor*> actors;
    if (!sehGetActorList(level, &actors)) return;

    // This frame's tag set (double-buffered - swapped into g_tagActors only
    // after the loop completes; see the comment at the top of OnRender).
    std::unordered_set<void*> frameTags;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 screen = io.DisplaySize;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    bool  renderLocal = static_cast<BoolSetting*>(mSettings[0])->mValue;
    // Custom self tag (appended block, see the header).
    const bool customSelf  = mSettings.size() > 21 && static_cast<BoolSetting*>(mSettings[21])->mValue;
    std::string selfName   = (mSettings.size() > 22) ? static_cast<StringSetting*>(mSettings[22])->mValue : "";
    const bool showRealSelf= mSettings.size() > 23 ? static_cast<BoolSetting*>(mSettings[23])->mValue : true;
    bool  distScale   = static_cast<BoolSetting*>(mSettings[1])->mValue;
    float fontSizeCfg = static_cast<NumberSetting*>(mSettings[2])->mValue;
    float scaleMult   = static_cast<NumberSetting*>(mSettings[3])->mValue;
    float minScale    = static_cast<NumberSetting*>(mSettings[4])->mValue;
    int   colMode     = static_cast<EnumSetting*>(mSettings[5])->mValue;
    float* ccol       = static_cast<ColorSetting*>(mSettings[6])->mValue;
    bool  doBg        = static_cast<BoolSetting*>(mSettings[7])->mValue;
    float darkening   = static_cast<NumberSetting*>(mSettings[8])->mValue;
    float rounding    = static_cast<NumberSetting*>(mSettings[9])->mValue;
    float blur        = static_cast<NumberSetting*>(mSettings[10])->mValue;
    bool  doShadow    = static_cast<BoolSetting*>(mSettings[11])->mValue;
    float shadowStr   = static_cast<NumberSetting*>(mSettings[12])->mValue;
    float shadowOff   = static_cast<NumberSetting*>(mSettings[13])->mValue;
    bool  showHealth  = static_cast<BoolSetting*>(mSettings[14])->mValue;
    bool  showBps     = static_cast<BoolSetting*>(mSettings[15])->mValue;
    bool  showEntities= static_cast<BoolSetting*>(mSettings[16])->mValue;
    bool  renderHeads = static_cast<BoolSetting*>(mSettings[19])->mValue;
    bool  sep         = static_cast<BoolSetting*>(mSettings[20])->mValue;

    uint64_t nowMs = GetTickCount64();
    int drawn = 0;
    std::unordered_set<std::string> seenHeads;

    if (!renderHeads && !mHeadTex.empty()) clearHeads();

    for (SDK::Actor* actor : actors) {
        if (!actor) continue;
        bool isLocal = (actor == lp);
        if (isLocal && !renderLocal) continue;
        bool player = sehIsPlayer(actor);
        if (!player && !(showEntities && sdk_safe_ptr(actor, 0x230))) continue;
        // NOTE: a previous skin-validity gate (checking getSkin()->mId/mSkinImage)
        // silently filtered EVERY player on this build because the skin offsets
        // were wrong -> no names rendered. Names must show regardless of skin
        // load state, so the gate is gone (labels are cheap + skin-independent).

        Vec3 pos; AABB bb;
        if (!SDK::sehGetPosBox(actor, &pos, &bb)) continue;
        Vec3 head;
        {
            // Anchor just above the model top, centred, rebased onto the
            // interpolated render position — the SAME path for the local
            // player and everyone else. The old fixed self offset rode the
            // third-person render position and floated the tag far too high.
            Vec3 dim = bb.higher - bb.lower;
            if (!(dim.y > 0.1f && dim.y < 6.0f)) dim = Vec3(0.6f, 1.8f, 0.6f);
            float topY = bb.higher.y;
            if (!(topY > pos.y) || topY > pos.y + 6.0f) topY = pos.y + dim.y;
            // Sneak: the synced AABB can lag behind the crouch — clamp the
            // anchor down to the crouched height so the tag DROPS with the
            // head instead of riding up at the standing height.
            if (ntIsSneaking(actor)) {
                const float boxH = topY - bb.lower.y;
                if (boxH > 1.55f) topY = bb.lower.y + 1.42f;
            }
            head = Vec3((bb.lower.x + bb.higher.x) * 0.5f, topY + 0.45f, (bb.lower.z + bb.higher.z) * 0.5f);
            const Vec3 rp = SDK::interpRenderPos(actor);
            if (rp.x == 0.f && rp.y == 0.f && rp.z == 0.f) continue;
            head = Vec3(head.x + (rp.x - pos.x), head.y + (rp.y - pos.y), head.z + (rp.z - pos.z));
        }

        ImVec2 sp;
        if (!worldToScreen(head, mvp, origin, screen, &sp)) continue;
        if (sp.x < -200.0f || sp.x > screen.x + 200.0f || sp.y < -200.0f || sp.y > screen.y + 200.0f) continue;

        // ---- Build the label text ----
        std::string rawName;
        if (!SDK::sehGetName(actor, &rawName) || rawName.empty()) rawName = player ? "Player" : "Entity";
        // The DRAWN name: your own tag carries the custom self name (plus the
        // real nick in brackets when enabled). The head texture still keys on
        // the REAL name below — the skin belongs to the real player.
        std::string drawName = rawName;
        if (isLocal && customSelf && !selfName.empty()) {
            drawName = selfName;
            if (showRealSelf) drawName += " (" + rawName + ")";
        }

        // Stats (HP / BPS) as WHOLE numbers. Each stat is its own segment so
        // the GRAY separators between them (and between the name and the
        // first stat) never take the HP/BPS colour: "Name - 18 - 5" with the
        // dashes gray, the numbers in the stat colour.
        std::string hpText, bpsText;
        {
            if (showHealth) {
                float hp = SDK::sehGetHealth(actor);
                if (hp >= 0.0f) {
                    char b[24]; snprintf(b, sizeof(b), "%d", (int)lroundf(hp));
                    hpText = b;
                }
            }
            if (showBps && player) {
                float bps = computeBps(actor, pos);
                char b[24]; snprintf(b, sizeof(b), "%d", (int)lroundf(bps));
                bpsText = b;
            }
        }
        const bool hasStats = !hpText.empty() || !bpsText.empty();
        const char* sepStr = sep ? " - " : "  ";

        // ---- Font size: fixed or distance-scaled ----
        float fontSize = fontSizeCfg;
        if (distScale) {
            float dx = head.x - origin.x, dy = head.y - origin.y, dz = head.z - origin.z;
            float distance = sqrtf(dx*dx + dy*dy + dz*dz) + 2.5f;
            if (distance < 0.01f) distance = 0.01f;
            fontSize = (1.0f / distance) * 100.0f * scaleMult;
            if (fontSize < minScale) fontSize = minScale;
            if (fontSize > 48.0f) fontSize = 48.0f;
        }
        if (fontSize < 1.0f) fontSize = 1.0f;

        // ---- Color ----
        // The nick is WHITE (theme mode) or the custom color - never a dark
        // theme shade; the stats suffix carries the theme accent color.
        ImColor nameCol, statCol;
        if (colMode == 0) {
            nameCol = ImColor(255, 255, 255, 255);
            statCol = ColorUtils::getThemedColor(0.0f, nowMs);
        } else {
            nameCol = ImColor(ccol[0], ccol[1], ccol[2], ccol[3]);
            statCol = nameCol;
        }

        const std::string sepS(sepStr);
        float nameW  = ImRenderUtils::getTextWidth(&drawName, fontSize);
        float sepW   = ImRenderUtils::getTextWidth(&sepS, fontSize);
        float hpW    = hpText.empty() ? 0.0f : ImRenderUtils::getTextWidth(&hpText, fontSize);
        float bpsW   = bpsText.empty() ? 0.0f : ImRenderUtils::getTextWidth(&bpsText, fontSize);
        float textW  = nameW;
        if (hasStats) textW += sepW + hpW;
        if (!hpText.empty() && !bpsText.empty()) textW += sepW;
        if (!bpsText.empty()) textW += bpsW;
        float textH  = fontSize;
        float pad    = fontSize * 0.28f;

        // Head icon (left of nick), TargetHUD/TabList style. Only for players —
        // entities rarely have a usable skin path.
        float headSize = 0.0f;
        float headGap  = 0.0f;
        void* headTex  = nullptr;
        if (renderHeads && player) {
            headSize = fontSize * 1.15f;
            if (headSize < 10.0f) headSize = 10.0f;
            headGap = pad * 0.7f;
            seenHeads.insert(rawName);
            auto it = mHeadTex.find(rawName);
            if (it != mHeadTex.end()) {
                it->second.second = frame;
                headTex = it->second.first;
            } else {
                std::vector<unsigned char> rgba = sehHeadRGBA(actor, 32);
                if (!rgba.empty()) {
                    ID3D11ShaderResourceView* srv = SkinUtils::createTextureRGBA(rgba.data(), 32, 32);
                    if (srv) {
                        mHeadTex[rawName] = { (void*)srv, frame };
                        headTex = srv;
                    }
                }
            }
        }

        // Content width includes optional head; plate stays centred on the player.
        float contentW = headSize + headGap + textW;
        float contentH = (headSize > textH) ? headSize : textH;
        ImVec2 contentOrigin(sp.x - contentW * 0.5f, sp.y - contentH);

        if (doBg) {
            ImVec2 mn(contentOrigin.x - pad, contentOrigin.y - pad * 0.6f);
            ImVec2 mx(contentOrigin.x + contentW + pad, contentOrigin.y + contentH + pad * 0.6f);
            ImVec4 r(mn.x, mn.y, mx.x, mx.y);
            if (blur > 0.1f)
                D2D::addBlurOptimized(dl, blur * 1.6f, std::optional<ImVec4>(r), rounding);
            if (doShadow && shadowStr > 0.001f) {
                ImRenderUtils::fillShadowRectangle(r, ImColor(0.0f, 0.0f, 0.0f, 1.0f),
                                                   shadowStr, rounding,
                                                   6.0f + shadowOff * 3.0f, 0.6f);
            }
            float a = darkening; if (a < 0.0f) a = 0.0f; if (a > 1.0f) a = 1.0f;
            ImRenderUtils::fillRectangle(r, ImColor(0.0f, 0.0f, 0.0f, 1.0f), a, rounding, dl);
        }

        float cursorX = contentOrigin.x;
        float midY = contentOrigin.y + contentH * 0.5f;

        if (headSize > 0.0f) {
            float hx = cursorX;
            float hy = midY - headSize * 0.5f;
            float hr = headSize * 0.22f;
            if (headTex) {
                dl->AddImageRounded((ImTextureID)headTex,
                    ImVec2(hx, hy), ImVec2(hx + headSize, hy + headSize),
                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255), hr);
            } else {
                // Placeholder while the server skin streams in.
                ImRenderUtils::fillRectangle(
                    ImVec4(hx, hy, hx + headSize, hy + headSize),
                    ImColor(30, 30, 36, 160), 1.0f, hr, dl);
            }
            dl->AddRect(ImVec2(hx, hy), ImVec2(hx + headSize, hy + headSize),
                        IM_COL32(255, 255, 255, 45), hr, 0, 1.0f);
            cursorX += headSize + headGap;
        }

        ImVec2 textPos(cursorX, midY - textH * 0.5f);
        bool textShadow = (!doBg && doShadow);
        ImRenderUtils::drawText(textPos, drawName, nameCol, fontSize, 1.0f, textShadow, dl);
        // Segmented stats: gray separator between the name and the first
        // stat and between the stats themselves; the stat values keep the
        // stat colour. (One fused string made the separators inherit the
        // HP/BPS colour and left no gap after the nick.)
        {
            const ImColor sepCol(150, 150, 150, 230);
            float sx = cursorX + nameW;
            if (hasStats) {
                ImRenderUtils::drawText(ImVec2(sx, textPos.y), sepStr, sepCol,
                                        fontSize, 1.0f, textShadow, dl);
                sx += sepW;
            }
            if (!hpText.empty()) {
                ImRenderUtils::drawText(ImVec2(sx, textPos.y), hpText, statCol,
                                        fontSize, 1.0f, textShadow, dl);
                sx += hpW;
            }
            if (!hpText.empty() && !bpsText.empty()) {
                ImRenderUtils::drawText(ImVec2(sx, textPos.y), sepStr, sepCol,
                                        fontSize, 1.0f, textShadow, dl);
                sx += sepW;
            }
            if (!bpsText.empty()) {
                ImRenderUtils::drawText(ImVec2(sx, textPos.y), bpsText, statCol,
                                        fontSize, 1.0f, textShadow, dl);
            }
        }
        frameTags.insert((void*)actor);
        ++drawn;
    }

    // Swap the COMPLETED frame set in atomically. The game's nametag pass
    // keeps seeing the previous complete set until this moment - never a
    // half-built or empty one.
    { std::lock_guard<std::mutex> lock(g_tagMutex); g_tagActors.swap(frameTags); }

    // Defer release of head textures for players no longer visible this frame.
    if (renderHeads) {
        for (auto it = mHeadTex.begin(); it != mHeadTex.end();) {
            if (seenHeads.find(it->first) == seenHeads.end()) {
                deferHeadRelease(it->second.first);
                it = mHeadTex.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (dbg) LOG_INFO("NameTags summary: actors=" + std::to_string(actors.size()) + " drawn=" + std::to_string(drawn) + " screen=" + std::to_string(screen.x) + "x" + std::to_string(screen.y), "NameTags");
}
