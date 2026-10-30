#include "BlockInfo.h"
#include "../../SDK/Client/GameIcon.h"
#include "../Setting.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Client/ScreenView.h"
#include "../../SDK/Client/ClientInstance.h"
#include "../../SDK/Client/BlockTextures.h"
#include "../../SDK/World/World.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <imgui.h>

// aka WAILA. path: Level::getHitResult() -> HitType::BLOCK -> BlockSource::getBlock(pos) -> BlockLegacy::getName()

BlockInfo::BlockInfo()
    : Module("BlockInfo", "Shows block under crosshair", Category::Visual, 0) {
    AddSetting(new ColorSetting("Text Color", "Title text color", 1.f, 1.f, 1.f, 1.f));                              // [0]
    AddSetting(new ColorSetting("Detail Color", "Namespace / distance text color", 0.55f, 0.75f, 1.f, 1.f));         // [1]
    AddSetting(new BoolSetting("Show Namespace", "Show minecraft: prefix / namespace", true));                        // [2]
    AddSetting(new BoolSetting("Show Coordinates", "Show block XYZ", true));                                          // [3]
    AddSetting(new BoolSetting("Show Distance", "Show distance to hit", false));                                      // [4]
    // Plate style — the same four knobs (and the same names) the counter HUD
    // modules expose, so every plate in the client reads the same. See
    // CounterStyle.h / FPS.
    AddSetting(new NumberSetting("Background Dim", "Plate fill opacity (0..1)", 0.50f, 0.f, 1.f, 0.02f));             // [5]
    AddSetting(new NumberSetting("Background Blur", "Backdrop blur strength behind the plate", 4.f, 0.f, 20.f, 0.5f));// [6]
    AddSetting(new NumberSetting("Background Rounding", "Plate corner radius (px)", 6.f, 0.f, 20.f, 1.f));            // [7]
    // Icon: the block's real texture (SDK/Client/BlockTextures) drawn INSIDE the
    // plate — the settings above keep their original indices, so everything
    // added from here on is appended.
    AddSetting(new BoolSetting("Show Texture", "Draw the block's real texture inside the plate, left of the text", true)); // [8]
    AddSetting(new NumberSetting("Texture Size", "Block icon size in pixels", 34.f, 14.f, 72.f, 1.f));                 // [9]
    AddSetting(new BoolSetting("Text Shadow", "Draw a drop shadow behind the panel glyphs (off = crisp text)", false)); // [10]
    AddSetting(new NumberSetting("Shadow Opacity", "Opacity of the plate's drop shadow (0..1)", 0.35f, 0.f, 1.f, 0.02f)); // [11]
    AddSetting(new BoolSetting("Gradient Coordinates", "Draw the block coordinates in the live theme gradient", true)); // [12]
    mIsHudModule = true;
    mPosX = 10.f;
    mPosY = 80.f;
    canBind = true;
}

bool BlockInfo::showTexture() const {
    return static_cast<const BoolSetting*>(mSettings[8])->mValue;
}

float BlockInfo::iconPixels() const {
    return static_cast<const NumberSetting*>(mSettings[9])->mValue * mScale;
}

bool BlockInfo::resolveTarget(Target& out) {
    out = Target{};

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, 0x200)) return false;

    SDK::Minecraft* mc = ci->getMinecraft();
    if (!mc) return false;
    SDK::Level* level = mc->getLevel();
    if (!level || !sdk_safe_ptr(level, sizeof(void*))) return false;

    SDK::HitResult* hit = level->getHitResult();
    if (!hit || !sdk_safe_ptr(hit, sizeof(SDK::HitResult))) return false;
    if (hit->mType != SDK::HitType::BLOCK) return false;

    out.x = hit->mBlockPos[0];
    out.y = hit->mBlockPos[1];
    out.z = hit->mBlockPos[2];

    const float dx = hit->mPos.x - hit->mStartPos.x;
    const float dy = hit->mPos.y - hit->mStartPos.y;
    const float dz = hit->mPos.z - hit->mStartPos.z;
    out.dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    SDK::BlockSource* region = ci->getBlockSource();
    if (!region) return false;

    SDK::Block* block = region->getBlock(SDK::BlockPos(out.x, out.y, out.z));
    if (!block) return false;
    SDK::BlockLegacy* leg = block->getLegacy();
    if (!leg) return false;

    out.name = leg->getName();
    if (out.name.empty()) return false;
    out.block = block;
    SDK::GameIcon::noteBlock(out.name, block);   // lets the real icon be captured
    return true;
}

BlockInfo::Layout BlockInfo::layoutFor(float panelWidth, float panelHeight) const {
    Layout l;
    l.hasIcon = showTexture();
    l.panelX = mPosX;
    l.panelY = mPosY;
    (void)panelWidth;
    if (!l.hasIcon) return l;
    // The icon lives INSIDE the plate now (its own dark pad slot outside the
    // panel is gone), so it hangs off the panel's own left padding and is
    // centred against the text block it sits next to.
    l.iconSize = iconPixels();
    // Same 12px left inset the text column uses (padX in OnRender), so the cube
    // lines up with the plate's own padding.
    const float padX = 12.f * (mScale > 0.01f ? mScale : 1.f);
    l.iconX = mPosX + padX;
    l.iconY = mPosY + (panelHeight - l.iconSize) * 0.5f;
    return l;
}

void BlockInfo::DrawGameIcon(void* mcuirc, const std::string& screenName) {
    // The icon is now drawn by the ImGui pass with the block's REAL texture
    // (see OnRender) — it lands on top of the HUD plate/blur, which the
    // game-side ItemRenderer pass could not do (the overlay painted over it).
    (void)mcuirc; (void)screenName;
}


static std::string prettyBlockName(const std::string& raw, bool showNs) {
    if (raw.empty()) return "unknown";
    std::string id = raw;
    // Drop minecraft: unless user wants full id
    if (!showNs) {
        size_t colon = id.find(':');
        if (colon != std::string::npos) id = id.substr(colon + 1);
    }
    // title-case underscores
    std::string out;
    out.reserve(id.size());
    bool cap = true;
    for (char c : id) {
        if (c == '_' || c == ':') {
            out.push_back(c == ':' ? ':' : ' ');
            cap = true;
            continue;
        }
        if (cap && c >= 'a' && c <= 'z') c = (char)(c - 32);
        else if (!cap && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out.push_back(c);
        cap = false;
    }
    return out;
}

// Ease-out-back (the counter plates' spring): overshoots ~10% past 1 before
// settling, which is what makes the panel POP into place instead of sliding.
static float biEaseOutBack(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    const float u = t - 1.f;
    return 1.f + c3 * u * u * u + c1 * u * u;
}

void BlockInfo::OnRender() {
    if (!IsEnabled()) return;
    if (SDK::ScreenView::currentScreenName() != "hud_screen") return;

    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.f || dt > 0.25f) dt = 1.f / 60.f;

    Target t;
    const bool hasTarget = resolveTarget(t);

    // ---- appear / disappear driver -----------------------------------------
    // Same shape as the block counter and the OreMiner bar: the panel scales up
    // from 0.88 with an ease-out-back pop when a block comes under the crosshair
    // and eases back down to 0.92 while its alpha falls to zero, so it retracts
    // instead of blinking out of existence.
    mPanelAnim += ((hasTarget ? 1.f : 0.f) - mPanelAnim) * std::min(1.f, dt * 14.f);
    if (mPanelAnim < 0.004f) {
        mPanelAnim = 0.f;
        // Nothing under the crosshair: zero footprint so the HUD editor has no
        // phantom box to drag for a panel that is not on screen.
        mWidth = 0.f;
        mHeight = 0.f;
        return;
    }
    const float a = mPanelAnim;

    auto* textCol = static_cast<ColorSetting*>(mSettings[0]);
    auto* detCol  = static_cast<ColorSetting*>(mSettings[1]);
    bool showNs       = static_cast<BoolSetting*>(mSettings[2])->mValue;
    bool showCoord    = static_cast<BoolSetting*>(mSettings[3])->mValue;
    bool showDist     = static_cast<BoolSetting*>(mSettings[4])->mValue;
    float dim         = static_cast<NumberSetting*>(mSettings[5])->mValue;
    float blur        = static_cast<NumberSetting*>(mSettings[6])->mValue;
    float rounding    = static_cast<NumberSetting*>(mSettings[7])->mValue * mScale;
    const bool  doShadow   = static_cast<BoolSetting*>(mSettings[10])->mValue;
    const float shadowA    = static_cast<NumberSetting*>(mSettings[11])->mValue;
    const bool  gradCoords = static_cast<BoolSetting*>(mSettings[12])->mValue;

    // The content strings are rebuilt ONLY on a frame that resolved a block: the
    // fade-out paints the block that was under the crosshair from the copies
    // kept in the module (nothing is re-read once the target is already gone).
    if (hasTarget) {
        const std::string& rawName = t.name;
        mLastTitle = prettyBlockName(rawName, false);
        mLastIconName = rawName;
        mLastNs = showNs ? rawName : std::string();
        if (showCoord) {
            char cbuf[64];
            std::snprintf(cbuf, sizeof(cbuf), "%d, %d, %d", t.x, t.y, t.z);
            mLastCoords = cbuf;
        } else {
            mLastCoords.clear();
        }
        if (showDist) {
            char dbuf[32];
            std::snprintf(dbuf, sizeof(dbuf), "%.1fm", t.dist);
            mLastDist = dbuf;
        } else {
            mLastDist.clear();
        }
        mHaveContent = true;
    }
    if (!mHaveContent) return;

    const float scale = mScale > 0.01f ? mScale : 1.f;
    const float titleSize = 20.f * scale;
    const float detSize = 15.f * scale;
    const float padX = 12.f * scale;
    const float padY = 10.f * scale;
    const float iconGap = 8.f * scale;
    const float lineGap = 2.f * scale;
    const float iconSize = showTexture() ? iconPixels() : 0.f;

    // Detail stack, in draw order: namespace, coordinates (the "value" line —
    // the one that carries the theme gradient) and the distance.
    std::string lines[3];
    int lineCount = 0;
    int gradLine = -1;
    if (!mLastNs.empty())     lines[lineCount++] = mLastNs;
    if (!mLastCoords.empty()) { gradLine = lineCount; lines[lineCount++] = mLastCoords; }
    if (!mLastDist.empty())   lines[lineCount++] = mLastDist;

    const float titleW = ImRenderUtils::getTextWidth(&mLastTitle, titleSize);
    float detW = 0.f;
    for (int i = 0; i < lineCount; ++i) {
        const float w = ImRenderUtils::getTextWidth(&lines[i], detSize);
        if (w > detW) detW = w;
    }
    const float textW = titleW > detW ? titleW : detW;
    const float textH = lineCount > 0
        ? (titleSize + 4.f * scale + detSize * (float)lineCount + lineGap * (float)(lineCount - 1))
        : titleSize;

    // The plate grows to hold the icon, which now sits INSIDE it (there is no
    // second background slot behind the texture any more).
    const float contentW = iconSize > 0.f ? (iconSize + iconGap + textW) : textW;
    float width = padX * 2.f + contentW;
    if (width < 140.f * scale) width = 140.f * scale;
    float height = padY * 2.f + (iconSize > textH ? iconSize : textH);

    const Layout l = layoutFor(width, height);
    const bool hasIcon = l.hasIcon && iconSize > 0.f;

    // Footprint = the plate itself, exactly what the user sees.
    mWidth = width;
    mHeight = height;

    // ---- appear / disappear: scale up with the pop, down while fading -------
    const float zoom = hasTarget
        ? 0.88f + 0.12f * biEaseOutBack(a)
        : 0.92f + 0.08f * (a * a * (3.f - 2.f * a));   // smoothstep down

    // Center-pivot zoom: the plate, the icon and every glyph are mapped through
    // these, so nothing crawls out of the plate while it animates.
    const float pivotX = l.panelX + width * 0.5f;
    const float pivotY = l.panelY + height * 0.5f;
    auto zx = [&](float v) { return pivotX + (v - pivotX) * zoom; };
    auto zy = [&](float v) { return pivotY + (v - pivotY) * zoom; };

    const ImVec4 rect(zx(l.panelX), zy(l.panelY), zx(l.panelX + width), zy(l.panelY + height));
    float r = rounding * zoom;
    if (r < 0.f) r = 0.f;

    auto* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    // ---- frosted plate — the same plate CounterStyle.h paints for FPS/Ping ---
    // Blur ramps in with the alpha, so a full-strength blur patch can never
    // flash over the world while the plate is still ~invisible.
    const float blurAmt = blur * std::min(1.f, a * 3.f);
    if (blurAmt > 0.01f)
        D2D::addBlurOptimized(dl, blurAmt, std::optional<ImVec4>(rect), r);

    if (shadowA > 0.01f)
        ImRenderUtils::fillShadowRectangle(rect, ImColor(0, 0, 0), shadowA * a, r, 12.f * scale, 0.6f, dl);
    ImRenderUtils::fillRectangle(rect, ImColor(12, 12, 16, 255), dim * a, r, dl);
    // Thin white hairline. The coloured outline the panel used to carry is not
    // part of the counter language — the theme colour lives in the value text.
    dl->AddRect(ImVec2(rect.x, rect.y), ImVec2(rect.z, rect.w),
                ImColor(255, 255, 255, (int)(25 * a)), r, 0, 1.0f);

    // ---- icon: the block's REAL texture, painted INSIDE the plate -----------
    // drawBlockTexture3D lands on this ImGui list and therefore on top of the
    // blur/fill; layoutFor already centred it against the plate height.
    const float iconPx = iconSize * zoom;
    if (hasIcon && !mLastIconName.empty() && iconPx > 2.f)
        SDK::drawBlockTexture3D(dl, zx(l.iconX), zy(l.iconY), iconPx, mLastIconName.c_str(), a);

    // ---- text column: title, then the detail stack --------------------------
    const float fsT = titleSize * zoom;
    const float fsD = detSize * zoom;
    const float textX = hasIcon ? zx(l.iconX + iconSize + iconGap) : zx(l.panelX + padX);
    const uint64_t nowMs = ColorUtils::GetTimeMs();
    const ImColor tcol(textCol->mValue[0], textCol->mValue[1], textCol->mValue[2], textCol->mValue[3]);
    const ImColor dcol(detCol->mValue[0], detCol->mValue[1], detCol->mValue[2], detCol->mValue[3]);

    // Vertically centred text block, so a tall cube and two text lines share the
    // same optical middle.
    float ty = zy(l.panelY + (height - textH) * 0.5f);
    ImRenderUtils::drawText(ImVec2(textX, ty), mLastTitle, tcol, fsT, a, doShadow, dl);
    for (int i = 0; i < lineCount; ++i) {
        ty += (i == 0 ? fsT + 4.f * scale * zoom : fsD + lineGap * zoom);
        if (i == gradLine && gradCoords) {
            // Per-glyph theme colour, the same sweep the counters give their
            // value (FPS's "Gradient Numbers").
            float gx = textX;
            float idx = 0.f;
            for (char c : lines[i]) {
                const std::string glyph(1, c);
                ImRenderUtils::drawText(ImVec2(gx, ty), glyph,
                                        ColorUtils::getThemedColor(60.f + idx, nowMs), fsD, a, doShadow, dl);
                gx += ImRenderUtils::getTextWidth(&glyph, fsD);
                idx += 72.f;
            }
        } else {
            ImRenderUtils::drawText(ImVec2(textX, ty), lines[i], dcol, fsD, a, doShadow, dl);
        }
    }
}
