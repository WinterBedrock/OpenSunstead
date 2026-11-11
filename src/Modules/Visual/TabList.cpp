#include "TabList.h"
#include "../Setting.h"
#include "../../SDK/SDK.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/SkinUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"

#include <imgui.h>
#include <d3d11.h>
#include <Windows.h>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <cmath>

namespace {

// ---- Context + enumeration (mirrors NameTags' SEH-guarded helpers) ----------
// sehGetActorList / sehIsPlayer are consolidated in SDK::GameContext (ADL).


std::string normName(const std::string& in) {
    std::string o; o.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0xC2 && i + 1 < in.size() && (unsigned char)in[i + 1] == 0xA7) { i += 2; continue; }
        if (c == ' ' || c == '\t') continue;
        o.push_back((char)tolower(c));
    }
    return o;
}

// 1.26.62 (Latite-dumped): Actor+0xAD8 -> SerializedSkinRef* ->
// skinImpl = *ref -> SkinImage @ skinImpl+0x80. PlayerListEntry's skin
// (entry+0xA0, SerializedSkinRef inline) starts with the same skinImpl ptr.
SDK::mce::Image* sehImageFromSkinImpl(uintptr_t skinImpl) {
    if (!skinImpl) return nullptr;
    constexpr uintptr_t kSkinImage = ::Layout::Actor::SkinImageInRef;   // 0x80
    if (!sdk_safe_ptr((void*)skinImpl, kSkinImage + sizeof(SDK::mce::Image))) return nullptr;
    return reinterpret_cast<SDK::mce::Image*>(skinImpl + kSkinImage);
}

SDK::mce::Image* sehImageFromActor(SDK::Actor* actor) {
    if (!actor) return nullptr;
    __try {
        constexpr uintptr_t kSkinRef = ::Layout::Actor::SerializedSkinRef; // 0xAD8
        if (!sdk_safe_ptr(actor, kSkinRef + sizeof(void*))) return nullptr;
        uintptr_t ref = *reinterpret_cast<uintptr_t*>((uintptr_t)actor + kSkinRef);
        if (!ref || !sdk_safe_ptr((void*)ref, sizeof(void*))) return nullptr;
        uintptr_t skinImpl = *reinterpret_cast<uintptr_t*>(ref);
        if (!skinImpl) return nullptr;
        return sehImageFromSkinImpl(skinImpl);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// SEH-guarded PlayerList map access. Returns nullptr if not available.

// SEH-guarded name read from a PlayerListEntry.

// Context resolution is SDK::resolveContext (SDK/Client/GameContext.h).

std::vector<unsigned char> sehHeadRGBA(SDK::Actor* actor, int outSize) {
    SDK::mce::Image* img = sehImageFromActor(actor);
    if (!img) return {};
    return SkinUtils::extractHeadRGBA(*img, outSize);
}

// Head decode straight from the server-side player-list entry (works for ALL
// connected players, not just nearby actors — this is the Latite path).
// The row carries entry.skin.skinImpl (PlayerListEntry+0xA0 -> SerializedSkinRef).
std::vector<unsigned char> sehHeadRGBAFromEntry(void* skinImpl, int outSize) {
    SDK::mce::Image* img = sehImageFromSkinImpl((uintptr_t)skinImpl);
    if (!img) return {};
    return SkinUtils::extractHeadRGBA(*img, outSize);
}

} // namespace

void TabList::releaseHeads() {
    for (auto& kv : mHeadTex) {
        if (kv.second.first)
            SkinUtils::retireTexture(kv.second.first);
    }
    mHeadTex.clear();
}

void TabList::syncHoldMode() {
    mHoldMode = (static_cast<EnumSetting*>(mSettings[0])->mValue == 1);
}

void TabList::UpdateSettingsVisibility() {
    syncHoldMode();
}

void TabList::OnDisable() {
    releaseHeads();
}

void TabList::OnRender() {
    if (!ImGui::GetCurrentContext()) return;

    syncHoldMode();
    const bool doHeads     = static_cast<BoolSetting*>(mSettings[1])->mValue;
    const bool doBg        = static_cast<BoolSetting*>(mSettings[2])->mValue;
    const float blur       = static_cast<NumberSetting*>(mSettings[3])->mValue;
    const float opacity    = static_cast<NumberSetting*>(mSettings[4])->mValue;
    const float textSize   = static_cast<NumberSetting*>(mSettings[5])->mValue;
    const int   maxPlayers = (int)static_cast<NumberSetting*>(mSettings[6])->mValue;
    int   rowsPerCol       = (int)static_cast<NumberSetting*>(mSettings[7])->mValue;
    const bool  hlSelf     = static_cast<BoolSetting*>(mSettings[8])->mValue;
    const float uiRounding = static_cast<NumberSetting*>(mSettings[9])->mValue;
    if (rowsPerCol < 1) rowsPerCol = 1;

    const bool onHud = SDK::isGameplayInputActive();
    const bool shouldShow = onHud && IsEnabled();

    // Frame-rate independent easing (~12/s). Snap to the endpoints to avoid a
    // permanent tiny residual.
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
    float target = shouldShow ? 1.0f : 0.0f;
    mAnim += (target - mAnim) * (1.0f - expf(-12.0f * dt));
    if (mAnim > 0.999f) mAnim = 1.0f;
    if (mAnim < 0.001f) mAnim = 0.0f;

    // Fully closed: release head textures and stop. (Only release when truly
    // hidden so heads don't flicker during the fade-out.)
    if (mAnim <= 0.0f || !onHud) { if (mAnim <= 0.0f) releaseHeads(); return; }

    const float anim = mAnim;

    SDK::ClientInstance* ci; SDK::Level* level; SDK::Actor* lp;
    if (!SDK::resolveContext(&ci, &level, &lp)) return;

    // ---- Primary: use the server-side player list (ALL connected players) ----
    auto* playerList = SDK::sehGetPlayerList(level);

    struct Row { std::string name; SDK::Actor* actor; bool self; void* skinImpl; };
    std::vector<Row> rows;

    // Get the local player's name for self-detection.
    std::string localName;
    if (lp) localName = lp->getRawName();

    if (playerList && !playerList->empty()) {
        // Collect unique player names from the server-side player list.
        rows.reserve(playerList->size());
        for (auto& entry : *playerList) {
            std::string nm;
            if (!SDK::sehGetPlayerListName(&entry.second, &nm) || nm.empty()) continue;
            bool dup = false;
            for (auto& r : rows) if (r.name == nm) { dup = true; break; }
            if (dup) continue;
            rows.push_back({ nm, nullptr, nm == localName, entry.second.skin.skinImpl });
        }
    }

    // ---- Fallback: if no player list, use the actor list (nearby actors) ----
    if (rows.empty()) {
        std::vector<SDK::Actor*> actors;
        if (!sehGetActorList(level, &actors)) return;
        for (SDK::Actor* a : actors) {
            if (!a || !sehIsPlayer(a)) continue;
            std::string nm;
            if (!SDK::sehGetName(a, &nm) || nm.empty()) continue;
            bool dup = false;
            for (auto& r : rows) if (r.name == nm) { dup = true; break; }
            if (dup) continue;
            rows.push_back({ nm, a, a == lp, nullptr });
        }
    }

    // ---- Supplement: build name -> actor map for head extraction ----
    // Even when using the player list, we need actors to extract skin images.
    if (doHeads && playerList && !playerList->empty()) {
        std::vector<SDK::Actor*> actors;
        if (sehGetActorList(level, &actors)) {
            for (SDK::Actor* a : actors) {
                if (!a || !sehIsPlayer(a)) continue;
                std::string nm;
                if (!SDK::sehGetName(a, &nm) || nm.empty()) continue;
                // Match actor to row by NORMALISED name (formatting-insensitive).
                const std::string an = normName(nm);
                for (auto& r : rows) {
                    if (!r.actor && normName(r.name) == an) {
                        r.actor = a;
                        break;
                    }
                }
            }
        }
    }

    if (rows.empty()) return;

    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
        return _stricmp(x.name.c_str(), y.name.c_str()) < 0;
    });
    if ((int)rows.size() > maxPlayers) rows.resize(maxPlayers);

    // ---- Resolve / refresh head textures, prune departed players ------------
    const int frame = ImGui::GetFrameCount();
    if (doHeads) {
        for (auto& r : rows) {
            auto it = mHeadTex.find(r.name);
            if (it != mHeadTex.end()) { it->second.second = frame; continue; }
            std::vector<unsigned char> rgba;
            if (r.actor) rgba = sehHeadRGBA(r.actor, 32);
            if (rgba.empty()) rgba = sehHeadRGBAFromEntry(r.skinImpl, 32);
            if (rgba.empty()) continue; // skin not loaded yet; retry next frame
            ID3D11ShaderResourceView* srv = SkinUtils::createTextureRGBA(rgba.data(), 32, 32);
            if (srv) mHeadTex[r.name] = { (void*)srv, frame };
        }
    }
    // Release textures for players no longer present (or when heads disabled).
    for (auto it = mHeadTex.begin(); it != mHeadTex.end();) {
        if (!doHeads || it->second.second != frame) {
            if (it->second.first) SkinUtils::retireTexture(it->second.first);
            it = mHeadTex.erase(it);
        } else {
            ++it;
        }
    }

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float pad = 10.0f;
    const float rowH = textSize + 8.0f;
    const float headSize = doHeads ? (rowH - 6.0f) : 0.0f;
    const float headGap = doHeads ? 8.0f : 0.0f;

    const int total = (int)rows.size();
    const int numCols = (total + rowsPerCol - 1) / rowsPerCol; // ceil
    const int rowsInCol = (numCols > 1) ? rowsPerCol : total;  // tallest column

    std::string header = std::to_string(total) + " players";
    // Widest name determines a uniform column width so columns align cleanly.
    float maxNameW = 0.0f;
    for (auto& r : rows) {
        float w = ImRenderUtils::getTextWidth(&r.name, textSize);
        if (w > maxNameW) maxNameW = w;
    }
    const float colW = pad + headSize + headGap + maxNameW + pad;
    float panelW = colW * (float)numCols;
    // Keep the header readable even when there is only one narrow column.
    const float headerW = ImRenderUtils::getTextWidth(&header, textSize) + pad * 2.0f;
    if (panelW < headerW) panelW = headerW;

    const float headerH = rowH;
    const float panelH = pad + headerH + rowH * (float)rowsInCol + pad;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float x = (ds.x - panelW) * 0.5f;
    // Slide down from slightly above while opening (and back up while closing),
    // driven by the eased anim value.
    const float y = 36.0f - (1.0f - anim) * 14.0f;
    const ImVec4 panel(x, y, x + panelW, y + panelH);
    const float rounding = uiRounding;

    // Alpha helper: fold the open/close anim into any opacity so the whole panel
    // fades together.
    auto A = [anim](float a) { return a * anim; };

    if (doBg) {
        const float blurStrength = blur * anim;
        if (blurStrength > 0.05f) D2D::addBlurOptimized(dl, blurStrength, panel, rounding);
        ImRenderUtils::fillShadowRectangle(panel, ImColor(0, 0, 0), A(0.55f), rounding, 18.0f);
        ImRenderUtils::fillRectangle(panel, ImColor(16, 16, 20), A(opacity), rounding, dl);
    }

    // Header (centered over the whole panel, accent color).
    ImColor accent(46, 184, 242);
    float hx = x + (panelW - ImRenderUtils::getTextWidth(&header, textSize)) * 0.5f;
    ImRenderUtils::drawText(ImVec2(hx, y + pad), header, accent, textSize, A(1.0f), true, dl);

    const float gridTop = y + pad + headerH;
    for (int i = 0; i < total; ++i) {
        const Row& r = rows[i];
        const int col = i / rowsPerCol;
        const int rowInCol = i % rowsPerCol;
        const float cellX = x + colW * (float)col;
        const float cy = gridTop + rowH * (float)rowInCol;

        if (hlSelf && r.self) {
            ImRenderUtils::fillRectangle(ImVec4(cellX + 4.0f, cy - 1.0f, cellX + colW - 4.0f, cy + rowH - 1.0f),
                                         accent, A(0.18f), 4.0f, dl);
        }
        float tx = cellX + pad;
        if (doHeads) {
            auto it = mHeadTex.find(r.name);
            if (it != mHeadTex.end() && it->second.first) {
                float iy = cy + (rowH - headSize) * 0.5f;
                int ia = (int)(255.0f * anim);
                dl->AddImageRounded((ImTextureID)(uintptr_t)it->second.first,
                                    ImVec2(tx, iy), ImVec2(tx + headSize, iy + headSize),
                                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, ia), headSize * 0.18f);
            }
            tx += headSize + headGap;
        }
        ImColor txtCol = r.self ? accent : ImColor(235, 235, 240);
        ImRenderUtils::drawText(ImVec2(tx, cy + (rowH - textSize) * 0.5f), r.name, txtCol, textSize, A(1.0f), true, dl);
    }
}
