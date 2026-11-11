#include "ClickGUI.h"

#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "../ModuleManager.h"
#include "../Misc/ToggleSounds.h"
#include "Notifications.h"
#include "../../Core/Config.h"
#include "../../Core/ConfigManager.h"
#include "../../Core/Logger.h"
#include "../../Core/Sunstead.h"
#include "../../Render/RenderManager.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/KeyboardUtils.hpp"
#include "../../Utils/MiscUtils/MathUtils.hpp"

namespace {

constexpr float kPanelW = 960.0f;
constexpr float kPanelH = 690.0f;
constexpr float kRailW  = 164.0f;
constexpr float kHeadH  = 52.0f;
constexpr float kPad    = 14.0f;


constexpr ImVec4 kPanel  { 23.0f  / 255.0f, 26.0f  / 255.0f, 33.0f  / 255.0f, 1.0f };
constexpr ImVec4 kTextHi { 1.000f, 1.000f, 1.000f, 1.0f };
constexpr ImVec4 kTextMid{ 0.700f, 0.700f, 0.720f, 1.0f };
constexpr ImVec4 kTextLo { 0.450f, 0.450f, 0.470f, 1.0f };

const char* const kCategoryNames[] = { "Visual", "Combat", "Movement", "Player", "Misc" };
constexpr int kCategoryCount = 5;

const char* const kRailGlyphs[] = {
    "\xEF\x82\xA8", "\xEF\x80\xAD", "\xEF\x82\xAB", "\xEF\x82\x95", "\xEF\x80\xAC", "\xEF\x82\xA6"
};


float  g_open      = 0.0f;
float  g_ease      = 0.0f;
float  g_panelX    = 0.0f;   // Unscaled, persisted top-left of the panel.
float  g_panelY    = 0.0f;
bool   g_placed    = false;  // false until a position has been chosen/loaded
bool   g_dragging  = false;
float  g_dragDX    = 0.0f;
float  g_dragDY    = 0.0f;

int    g_tab       = 0;      // 0 = modules, 1 = settings, 2 = configs, 3 = themes, 4 = changelog
int    g_category  = -1;      // active rail entry within the modules tab (-1 = All)
float  g_railEase  = 1.0f;   // animated rail selection index
float  g_tabFade   = 1.0f;   // content cross-fade after a tab/category switch

float  g_scroll       = 0.0f;
float  g_scrollTarget = 0.0f;

// Rail entries after the categories, in order. Index in the rail's animation
// space is kCategoryCount + position here, which is also g_tab - 1.
const char* const kExtraNames[]  = { "Settings", "Profiles", "Themes", "Changelog" };
const char* const kExtraGlyphs[] = { "", "2", "", "" };
constexpr int kExtraCount = 4;

Module* g_binding   = nullptr;  // module awaiting a keybind press
int*    g_keyTarget = nullptr;  // loose int (e.g. the eject key) awaiting a press
void*   g_slider    = nullptr;  // NumberSetting being dragged
void*   g_strEdit   = nullptr;  // StringSetting whose modal is open
bool    g_strFocus  = false;

char   g_search[128] = "";

std::string g_tip;
ImVec2      g_tipPos;
bool        g_tipShow = false;

// Eased hover amount per addressable element. Rebuilt geometry every frame
// cannot carry state, so it is keyed by a stable string id.
std::unordered_map<std::string, float> g_hover;

float hoverOf(const std::string& key, bool hovered, float dt) {
    float& v = g_hover[key];
    const float speed = std::clamp(dt * (hovered ? 16.0f : 9.0f), 0.0f, 1.0f);
    v += ((hovered ? 1.0f : 0.0f) - v) * speed;
    if (v < 0.002f) v = 0.0f;
    if (v > 0.998f) v = 1.0f;
    return v;
}

float easeOutCubic(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

float easeOutExpo(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return 1.0f - std::pow(2.0f, -9.0f * t);
}

float easeOutBack(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    constexpr float c1 = 1.70158f, c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

ImVec4 withAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

ImU32 col32(const ImVec4& c, float a) {
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a));
}

ImColor mix(const ImColor& a, const ImColor& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return ImColor(a.Value.x + (b.Value.x - a.Value.x) * t,
                   a.Value.y + (b.Value.y - a.Value.y) * t,
                   a.Value.z + (b.Value.z - a.Value.z) * t,
                   a.Value.w + (b.Value.w - a.Value.w) * t);
}

struct UiTransform {
    bool enabled = false;
    ImVec2 pivot{ 0.0f, 0.0f };
    ImVec2 slide{ 0.0f, 0.0f };
    float zoom = 1.0f;
    float fade = 1.0f;

    ImVec2 toScreen(const ImVec2& p) const {
        return enabled
            ? ImVec2(pivot.x + (p.x - pivot.x) * zoom + slide.x,
                     pivot.y + (p.y - pivot.y) * zoom + slide.y)
            : p;
    }

    ImVec2 toLogical(const ImVec2& p) const {
        if (!enabled) return p;
        const float invZoom = 1.0f / std::max(0.01f, zoom);
        return ImVec2(pivot.x + (p.x - slide.x - pivot.x) * invZoom,
                      pivot.y + (p.y - slide.y - pivot.y) * invZoom);
    }

    ImVec4 toScreen(const ImVec4& r) const {
        const ImVec2 p0 = toScreen(ImVec2(r.x, r.y));
        const ImVec2 p1 = toScreen(ImVec2(r.z, r.w));
        return ImVec4(p0.x, p0.y, p1.x, p1.y);
    }
};

UiTransform g_uiTransform;

ImVec2 guiMousePos() {
    return g_uiTransform.toLogical(ImRenderUtils::getMousePos());
}

bool over(const ImVec4& r) {
    const ImVec2 p = guiMousePos();
    return p.x >= r.x && p.y >= r.y && p.x <= r.z && p.y <= r.w;
}

struct UiDrawRange {
    ImDrawList* dl = nullptr;
    int firstVertex = 0;
    int firstElement = 0;
    int firstCommand = 0;

    explicit UiDrawRange(ImDrawList* drawList) : dl(drawList) {
        if (!dl) return;
        firstVertex = dl->VtxBuffer.Size;
        for (const ImDrawCmd& cmd : dl->CmdBuffer) firstElement += cmd.ElemCount;
        firstCommand = dl->CmdBuffer.Size;
    }

    void finish(float fade = 1.0f) const {
        if (!dl) return;
        const UiTransform& tr = g_uiTransform;
        for (int i = firstVertex; i < dl->VtxBuffer.Size; ++i) {
            ImDrawVert& v = dl->VtxBuffer[i];
            v.pos = tr.toScreen(v.pos);
            const ImU32 oldAlpha = (v.col >> IM_COL32_A_SHIFT) & 0xFFu;
            const ImU32 newAlpha = static_cast<ImU32>(
                static_cast<float>(oldAlpha) * std::clamp(fade, 0.0f, 1.0f) + 0.5f);
            v.col = (v.col & ~(0xFFu << IM_COL32_A_SHIFT)) |
                    (newAlpha << IM_COL32_A_SHIFT);
        }

        // Clipping stays in logical coordinates while drawing, then every
        // submitted command receives the same transform as its vertices.
        int element = 0;
        for (int i = 0; i < dl->CmdBuffer.Size; ++i) {
            ImDrawCmd& cmd = dl->CmdBuffer[i];
            const int end = element + cmd.ElemCount;
            // ImGui can append vertices to an empty preceding command. Never
            // change the clip of a command that already contained geometry.
            const bool mergedEmptyCommand = element >= firstElement && end > firstElement;
            if (i >= firstCommand || mergedEmptyCommand) {
                cmd.ClipRect = tr.toScreen(cmd.ClipRect);
                if (cmd.UserCallback == D2D::blurCallback ||
                    cmd.UserCallback == D2D::blurCallbackOptimized) {
                    auto* blur = static_cast<BlurCallbackData*>(cmd.UserCallbackData);
                    if (blur) {
                        if (blur->clipRect) blur->clipRect = tr.toScreen(*blur->clipRect);
                        blur->rounding *= tr.enabled ? tr.zoom : 1.0f;
                        blur->strength *= std::clamp(fade, 0.0f, 1.0f);
                    }
                }
            }
            element = end;
        }
    }
};

// Native ImGui widgets read the raw cursor, so give them the same logical space
// as the custom widgets and suppress their mouse capture during the close fade.
struct ScopedGuiPointer {
    ImGuiIO& io;
    ImVec2 savedPos;
    bool savedDown[IM_ARRAYSIZE(io.MouseDown)];
    bool savedClicked[IM_ARRAYSIZE(io.MouseClicked)];

    explicit ScopedGuiPointer(bool live) : io(ImGui::GetIO()), savedPos(io.MousePos) {
        for (int i = 0; i < IM_ARRAYSIZE(io.MouseDown); ++i) {
            savedDown[i] = io.MouseDown[i];
            savedClicked[i] = io.MouseClicked[i];
        }
        io.MousePos = live ? guiMousePos() : ImVec2(-FLT_MAX, -FLT_MAX);
        if (!live) {
            for (int i = 0; i < IM_ARRAYSIZE(io.MouseDown); ++i) {
                io.MouseDown[i] = false;
                io.MouseClicked[i] = false;
            }
        }
    }

    ~ScopedGuiPointer() {
        io.MousePos = savedPos;
        for (int i = 0; i < IM_ARRAYSIZE(io.MouseDown); ++i) {
            io.MouseDown[i] = savedDown[i];
            io.MouseClicked[i] = savedClicked[i];
        }
    }
};

// Rounded fill with an explicit alpha multiplier, on a caller-chosen list.
void fill(ImDrawList* dl, const ImVec4& r, const ImVec4& c, float a, float round,
          ImDrawFlags flags = 0) {
    if (a <= 0.002f || r.z - r.x < 0.5f || r.w - r.y < 0.5f) return;
    dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.z, r.w), col32(c, a), round, flags);
}

void stroke(ImDrawList* dl, const ImVec4& r, const ImVec4& c, float a, float round,
            float thickness = 1.0f, ImDrawFlags flags = 0) {
    if (a <= 0.002f || r.z - r.x < 0.5f || r.w - r.y < 0.5f) return;
    dl->AddRect(ImVec2(r.x, r.y), ImVec2(r.z, r.w), col32(c, a), round, flags, thickness);
}

void gradientV(ImDrawList* dl, const ImVec4& r, const ImVec4& top, const ImVec4& bottom,
               float aTop, float aBottom, float round, ImDrawFlags flags = 0) {
    ImRenderUtils::fillGradientRounded(dl, r, ImVec4(top.x, top.y, top.z, aTop),
                                       ImVec4(bottom.x, bottom.y, bottom.z, aBottom),
                                       round, flags, /*vertical=*/true);
}

// Horizontal two-stop gradient, same rationale as gradientV.
void gradientH(ImDrawList* dl, const ImVec4& r, const ImColor& left, const ImColor& right,
               float a, float round, ImDrawFlags flags = 0) {
    ImRenderUtils::fillGradientRounded(
        dl, r,
        ImVec4(left.Value.x, left.Value.y, left.Value.z, left.Value.w * a),
        ImVec4(right.Value.x, right.Value.y, right.Value.z, right.Value.w * a),
        round, flags, /*vertical=*/false);
}

void shadeDiag(ImDrawList* dl, const ImVec4& r, const ImVec4& c0, const ImVec4& c1,
               float round, ImDrawFlags flags = 0) {
    if (r.z - r.x < 0.5f || r.w - r.y < 0.5f) return;
    if (c0.w <= 0.002f && c1.w <= 0.002f) return;
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.z, r.w), IM_COL32_WHITE, round, flags);
    const int v1 = dl->VtxBuffer.Size;
    const float dx = r.z - r.x, dy = r.w - r.y;
    const float len2 = std::max(1.0e-3f, dx * dx + dy * dy);
    for (int i = v0; i < v1; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const float t = std::clamp(((v.pos.x - r.x) * dx + (v.pos.y - r.y) * dy) / len2,
                                   0.0f, 1.0f);
        const float keepA = static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
        v.col = ImGui::GetColorU32(ImVec4(c0.x + (c1.x - c0.x) * t,
                                          c0.y + (c1.y - c0.y) * t,
                                          c0.z + (c1.z - c0.z) * t,
                                          (c0.w + (c1.w - c0.w) * t) * keepA));
    }
}

void shadeDiagWipe(ImDrawList* dl, const ImVec4& r, const ImVec4& c0, const ImVec4& c1,
                   float wipe, float round, ImDrawFlags flags = 0) {
    if (r.z - r.x < 0.5f || r.w - r.y < 0.5f) return;
    if (c0.w <= 0.002f && c1.w <= 0.002f) return;
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.z, r.w), IM_COL32_WHITE, round, flags);
    const int v1 = dl->VtxBuffer.Size;
    const float dx = r.z - r.x, dy = r.w - r.y;
    const float len2 = std::max(1.0e-3f, dx * dx + dy * dy);
    const float edge = std::clamp(wipe, 0.0f, 1.0f) * 1.30f - 0.15f;
    for (int i = v0; i < v1; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const float t = std::clamp(((v.pos.x - r.x) * dx + (v.pos.y - r.y) * dy) / len2,
                                   0.0f, 1.0f);
        const float k = std::clamp((edge - t) / 0.15f, 0.0f, 1.0f);
        const float keepA = static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
        v.col = ImGui::GetColorU32(ImVec4(c0.x + (c1.x - c0.x) * t,
                                          c0.y + (c1.y - c0.y) * t,
                                          c0.z + (c1.z - c0.z) * t,
                                          (c0.w + (c1.w - c0.w) * t) * k * keepA));
    }
}

void shadeThemedSheet(ImDrawList* dl, const ImVec4& r, const ImVec4& space,
                      float lit, float sA, float span, uint64_t nowMs,
                      float round = 0.0f, ImDrawFlags flags = 0,
                      bool fullWash = false, const ImColor* lut = nullptr, int lutN = 0) {
    if (sA <= 0.004f || r.z - r.x < 0.5f || r.w - r.y < 0.5f) return;
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.z, r.w), IM_COL32_WHITE, round, flags);
    const int v1 = dl->VtxBuffer.Size;

    const float rdx = r.z - r.x, rdy = r.w - r.y;
    const float rlen2 = std::max(1.0e-3f, rdx * rdx + rdy * rdy);
    const float edge = std::clamp(lit, 0.0f, 1.0f) * 1.30f - 0.15f;

    const float sdx = space.z - space.x, sdy = space.w - space.y;
    const float slen2 = std::max(1.0e-3f, sdx * sdx + sdy * sdy);

    // Slow shared clocks.
    const float clockMs = static_cast<float>(nowMs % 4000000);
    // Diagonal drift of the whole pattern, in palette cycles (~16s per cycle).
    const float drift = clockMs * 0.00006f;

    const float sheenRate = 0.00010f;
    const float sheenSpan = 1.4f;
    const float sheenP = std::fmod(clockMs * sheenRate, 1.0f) * sheenSpan - 0.2f;
    const float sheenW = 0.10f;

    // Palette sampler: the per-frame LUT when present, else a live read.
    const auto samplePal = [&](float t) -> ImColor {
        t -= std::floor(t);
        if (lut && lutN > 1) {
            const float x = t * static_cast<float>(lutN);
            const int i0 = static_cast<int>(x) % lutN;
            const int i1 = (i0 + 1) % lutN;
            return mix(lut[i0], lut[i1], x - std::floor(x));
        }
        return ColorUtils::getThemedColor(t * span, nowMs);
    };

    for (int i = v0; i < v1; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const float rt = std::clamp(((v.pos.x - r.x) * rdx + (v.pos.y - r.y) * rdy) / rlen2,
                                    0.0f, 1.0f);
        const float st = std::clamp(((v.pos.x - space.x) * sdx + (v.pos.y - space.y) * sdy) / slen2,
                                    0.0f, 1.0f);
        const float k = std::clamp((edge - rt) / 0.15f, 0.0f, 1.0f);
        if (k <= 0.0f) { v.col = 0; continue; }
        const float keepA = static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;

        const float u = st * 2.0f;
        const ImColor cA = samplePal(u + drift);
        const ImColor cB = samplePal(u - drift * 0.8f + 0.37f);
        const float wv = 0.5f - 0.5f * std::cos(6.2831853f * (st * 1.5f - drift * 1.7f));
        const ImColor cc = mix(cA, cB, 0.20f + 0.50f * wv);

        const float dBand = std::fabs(st - sheenP);
        const float s01 = std::clamp(1.0f - dBand / sheenW, 0.0f, 1.0f);
        const float boost = s01 * s01 * (fullWash ? 0.60f : 0.45f);
        const float p2 = 1.0f - sheenP;
        const float dBand2 = std::fabs(st - p2);
        const float s02 = std::clamp(1.0f - dBand2 / (sheenW * 2.4f), 0.0f, 1.0f);
        const float boost2 = s02 * s02 * 0.22f;

        const float lift = boost + boost2 * 0.5f;
        const float cr = cc.Value.x + (1.0f - cc.Value.x) * lift;
        const float cg = cc.Value.y + (1.0f - cc.Value.y) * lift;
        const float cb = cc.Value.z + (1.0f - cc.Value.z) * lift;
        const float al = (fullWash
            ? 1.0f
            : std::min(1.0f, 0.88f - 0.26f * st)) * sA * k * keepA;
        v.col = ImGui::GetColorU32(ImVec4(cr, cg, cb, al));
    }
}

ImFont* bodyFont() {
    auto& rm = RenderManager::GetInstance();
    return rm.defaultFont ? rm.defaultFont : ImGui::GetFont();
}

ImFont* titleFont() {
    auto& rm = RenderManager::GetInstance();
    int idx = 0; bool bold = true;   // headers default to the bold weight
    if (Module* iface = ModuleManager::GetInstance().GetModuleByName("ClickGUI")) {
        for (Setting* s : iface->mSettings) {
            if (!s) continue;
            if (s->mName == "Font") idx = static_cast<EnumSetting*>(s)->mValue;
            else if (s->mName == "Bold Font") bold = static_cast<BoolSetting*>(s)->mValue;
        }
    }
    ImFont* f = rm.pickFont(idx, bold);
    return f ? f : ImGui::GetFont();
}

// Text with an optional 1px drop shadow. Both the shadow and the glyphs bail out
// below 1px because ImGui rounds sub-pixel font sizes to zero and then asserts.
void text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, const std::string& s,
          const ImVec4& c, float a, bool shadow) {
    size *= ImRenderUtils::g_visualFontScale;   // tall-metric GUI font trim
    if (!dl || size < 1.0f || a <= 0.003f || s.empty()) return;
    if (!font) font = ImGui::GetFont();
    if (!font) return;
    const float fa = std::clamp(c.w * a, 0.0f, 1.0f);
    if (fa <= 0.003f) return;
    if (shadow) {
        dl->AddText(font, size, ImVec2(pos.x + 1.0f, pos.y + 1.0f),
                    ImGui::GetColorU32(ImVec4(0, 0, 0, fa * 0.65f)), s.c_str());
    }
    dl->AddText(font, size, pos, ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, fa)), s.c_str());
}

float measure(ImFont* font, float size, const std::string& s) {
    size *= ImRenderUtils::g_visualFontScale;   // tall-metric GUI font trim
    if (!font) font = ImGui::GetFont();
    if (!font || size < 1.0f || s.empty()) return 0.0f;
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, s.c_str()).x;
}

// Truncate to fit `maxW`, appending an ellipsis when anything was dropped.
std::string elide(ImFont* font, float size, std::string s, float maxW) {
    if (measure(font, size, s) <= maxW) return s;
    while (s.size() > 1 && measure(font, size, s + "...") > maxW) s.pop_back();
    return s + "...";
}

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The first enum setting doubles as a module's mode label — the same convention
// the ArrayList uses.
std::string modeOf(Module* mod) {
    for (Setting* s : mod->mSettings) {
        if (!s || s->mType != SettingType::Enum) continue;
        auto* e = static_cast<EnumSetting*>(s);
        if (e->mValue >= 0 && e->mValue < static_cast<int>(e->mValues.size()))
            return e->mValues[e->mValue];
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Draw context: everything the widget helpers need, resolved once per frame.
// ---------------------------------------------------------------------------
namespace {

struct Ctx {
    ImDrawList* dl = nullptr;
    float   a = 1.0f;          // local alpha; the window fade is applied by UiDrawRange
    float   dt = 0.0f;
    bool    live = false;      // accept input this frame
    bool    lmb = false;       // left click edge
    bool    rmb = false;       // right click edge
    ImColor accent;
    float   scale = 1.0f;
    bool    textShadow = true;
    float   surface = 1.0f;    // Opacity setting
    float   round = 12.0f;
    float   rowH = 34.0f;
    float   glow = 1.0f;
    bool    shadow = true;
    float   shadowK = 1.0f;   // multiplier for the Shadow Strength setting
    bool    tooltips = true;
    ImFont* font = nullptr;
    ImFont* bold = nullptr;

    ImVec4 ink    { 0.925f, 0.937f, 0.969f, 1.0f };
    ImVec4 inkMid { 0.588f, 0.620f, 0.690f, 1.0f };
    ImVec4 inkLo  { 0.400f, 0.427f, 0.494f, 1.0f };
    ImVec4 tint   { 1.000f, 1.000f, 1.000f, 1.0f };
    ImVec4 hair   { 1.000f, 1.000f, 1.000f, 1.0f };
    ImVec4 plate  { 0.000f, 0.000f, 0.000f, 1.0f };
    ImVec4 knob   { 1.000f, 1.000f, 1.000f, 1.0f };
    float  plateA = 0.28f;         // how dark the dropdown recess is
    bool   light  = false;         // light surface: text goes on top of fills

    float    gradSpan = 900.0f;
    uint64_t gradMs   = 0;
    const ImColor* gradLut   = nullptr;
    int            gradLutN  = 0;
    const ImVec4*  gradSpace = nullptr;
    float bloomK   = 1.0f;
    float bloomMid = 0.0f;
    // Columns pill slider (rounded trough + sheet fill + knob) instead of the
    // Panel's hairline underline. Never set by the Panel.
    bool  colSlider    = false;
    float wellWipe     = 1.0f;
    bool  cornerReveal = false;
    bool  noWells      = false;
    bool  columnsClean = false;
    bool  columnsBody  = false;
};

void captureTip(const Ctx& c, const ImVec4& r, const std::string& desc) {
    if (!c.live || !c.tooltips || desc.empty() || !over(r)) return;
    g_tip = desc;
    const ImVec2 mp = guiMousePos();
    g_tipPos = ImVec2(mp.x + 16.0f * c.scale, mp.y + 18.0f * c.scale);
    g_tipShow = true;
}

// Track-and-knob switch. `on` is the eased 0..1 state so the knob glides and the
// track cross-fades instead of snapping.
void drawToggle(const Ctx& c, const ImVec4& r, float on, float hover) {
    const float h = r.w - r.y;
    const float rad = h * 0.5f;
    fill(c.dl, r, c.tint, (0.07f + 0.05f * hover) * c.a, rad);
    if (on > 0.004f) {
        const ImColor lo = mix(c.accent, ImColor(1.0f, 1.0f, 1.0f, 1.0f), 0.28f);
        gradientH(c.dl, r, c.accent, lo, on * c.a, rad);
    }
    stroke(c.dl, r, c.tint, (0.10f + 0.10f * hover) * c.a, rad, 1.0f);

    const float knobR = rad - 3.0f * c.scale;
    const float cx = MathUtils::lerp(r.x + rad, r.z - rad, std::clamp(on, 0.0f, 1.0f));
    const float cy = (r.y + r.w) * 0.5f;
    if (on > 0.02f && c.glow > 0.01f)
        ImRenderUtils::addGlowCircle(c.dl, ImVec2(cx, cy), knobR * 3.4f, c.accent,
                                     0.30f * c.glow * on * c.a);
    const ImVec4 kc(c.knob.x, c.knob.y, c.knob.z, (0.92f + 0.08f * hover) * c.a);
  c.dl->AddCircleFilled(ImVec2(cx, cy), std::max(1.0f, knobR),
                          ImGui::GetColorU32(kc), 20);
  stroke(c.dl, ImVec4(cx - knobR, cy - knobR, cx + knobR, cy + knobR), c.hair,
         0.22f * c.a * (1.0f - 0.6f * std::clamp(on, 0.0f, 1.0f)), rad, 1.0f);
}

} // namespace

// ---------------------------------------------------------------------------
// Setting rows. Animation is advanced before measuring so layout and drawing use
// the same expanded height.
// ---------------------------------------------------------------------------
namespace {

void tickAnims(Module* mod, float dt, bool bodyOpen) {
    const float openTarget = mod->showSettings ? 1.0f : 0.0f;
    const float openSpeed  = std::clamp(dt * (mod->showSettings ? 12.0f : 15.0f), 0.0f, 1.0f);
    mod->cAnim += (openTarget - mod->cAnim) * openSpeed;
    if (mod->cAnim < 0.002f) mod->cAnim = 0.0f;
    if (mod->cAnim > 0.998f) mod->cAnim = 1.0f;

    const float enTarget = mod->IsEnabled() ? 1.0f : 0.0f;
    const float enSpeed  = std::clamp(dt * (mod->IsEnabled() ? 15.0f : 18.0f), 0.0f, 1.0f);
    mod->enableAnim += (enTarget - mod->enableAnim) * enSpeed;
    if (mod->enableAnim < 0.002f) mod->enableAnim = 0.0f;
    if (mod->enableAnim > 0.998f) mod->enableAnim = 1.0f;

    for (Setting* s : mod->mSettings) {
        if (!s) continue;
        const bool wantOpen = s->enumExtended && bodyOpen;
        const float t = wantOpen ? 1.0f : 0.0f;
        s->enumSlide += (t - s->enumSlide) * std::clamp(dt * (wantOpen ? 16.0f : 19.0f), 0.0f, 1.0f);
        if (s->enumSlide < 0.002f) s->enumSlide = 0.0f;
        if (s->enumSlide > 0.998f) s->enumSlide = 1.0f;

        if (s->mType == SettingType::Bool) {
            const float bt = static_cast<BoolSetting*>(s)->mValue ? 1.0f : 0.0f;
            s->boolScale += (bt - s->boolScale) * std::clamp(dt * 16.0f, 0.0f, 1.0f);
        } else if (s->mType == SettingType::Enum) {
            const float target = static_cast<float>(static_cast<EnumSetting*>(s)->mValue);
            s->enumSelAnim += (target - s->enumSelAnim) * std::clamp(dt * 15.0f, 0.0f, 1.0f);
        }
    }
}

float optionHeight(const Ctx& c) { return c.rowH * 0.80f; }
float pickerHeight(const Ctx& c) { return 128.0f * c.scale; }

void drawGuiColorPicker(const Ctx& c, float* color, const ImVec4& rect) {
    const ScopedGuiPointer pointer(c.live);
    ImRenderUtils::drawColorPicker(color, rect);
}

float settingHeight(const Ctx& c, Setting* s) {
    float h = c.rowH;
    if (s->mType == SettingType::Enum) {
        const auto* e = static_cast<EnumSetting*>(s);
        // easeOutBack so the dropdown springs open; drawEnumRow/drawColorRow
        // MUST use the same curve or measured and drawn heights disagree.
        h += static_cast<float>(e->mValues.size()) * optionHeight(c) *
             easeOutBack(std::clamp(s->enumSlide, 0.0f, 1.0f));
    } else if (s->mType == SettingType::Color) {
        h += pickerHeight(c) * easeOutBack(std::clamp(s->enumSlide, 0.0f, 1.0f));
    }
    return h;
}

void drawBoolRow(const Ctx& c, BoolSetting* s, const ImVec4& row) {
    const std::string key = "b" + s->mName + std::to_string(reinterpret_cast<uintptr_t>(s));
    const bool hov = c.live && over(row);
    const float h = hoverOf(key, hov, c.dt);
    fill(c.dl, row, c.tint, 0.035f * h * c.a, 7.0f * c.scale);
    if (hov && c.lmb) s->mValue = !s->mValue;

    const float fs = (c.columnsBody ? 15.5f : 14.5f) * c.scale;
    const float labelX = c.columnsBody ? (c.columnsClean ? 0.0f : 8.0f) * c.scale : 12.0f * c.scale;
    text(c.dl, c.font, fs, ImVec2(row.x + labelX, (row.y + row.w - fs) * 0.5f),
         s->mName, s->mValue ? c.ink : c.inkMid, c.a, c.textShadow);

    const float tw = 36.0f * c.scale, th = 19.0f * c.scale;
    const ImVec4 sw(row.z - 12.0f * c.scale - tw, (row.y + row.w - th) * 0.5f,
                    row.z - 12.0f * c.scale, (row.y + row.w + th) * 0.5f);
    drawToggle(c, sw, easeOutCubic(std::clamp(s->boolScale, 0.0f, 1.0f)), h);
}

void drawNumberRow(const Ctx& c, NumberSetting* s, const ImVec4& row) {
    const std::string key = "n" + std::to_string(reinterpret_cast<uintptr_t>(s));
    const bool hov = c.live && over(row);
    const float h = hoverOf(key, hov, c.dt);
    fill(c.dl, row, c.tint, 0.035f * h * c.a, 7.0f * c.scale);

    const float fs = (c.columnsBody ? 15.5f : 14.5f) * c.scale;
    const float padX = 12.0f * c.scale;
    const float labelX = c.columnsBody ? (c.columnsClean ? 0.0f : 8.0f) * c.scale : padX;
    const float ty = (row.y + row.w - fs) * 0.5f;
    text(c.dl, c.font, fs, ImVec2(row.x + labelX, ty), s->mName,
         c.inkMid, c.a, c.textShadow);

    char buf[32];
    const bool whole = (std::floor(s->mStep) == s->mStep && s->mStep >= 1.0f);
    snprintf(buf, sizeof(buf), whole ? "%.0f" : "%.2f", s->mValue);
    const std::string val(buf);
    const float vw = measure(c.font, fs, val);
    text(c.dl, c.bold, fs, ImVec2(row.z - padX - vw, ty), val,
         c.ink, c.a, c.textShadow);

    const float range = (s->mMax - s->mMin) > 0.0f ? (s->mMax - s->mMin) : 1.0f;
    const float trackY = c.colSlider ? row.w - 8.0f * c.scale : row.w - 7.5f * c.scale;
    const float trackH = c.colSlider ? 6.0f * c.scale : 3.5f * c.scale;
    // The trough starts under the label's left line so the two share an edge.
    const ImVec4 track(row.x + labelX, trackY, row.z - padX, trackY + trackH);
    const float trackW = std::max(1.0f, track.z - track.x);

    if (hov && ImGui::IsMouseClicked(0)) g_slider = s;
    if (!ImGui::IsMouseDown(0) && g_slider == s) g_slider = nullptr;
    if (g_slider == s && ImGui::IsMouseDown(0) && c.live) {
        float v = (guiMousePos().x - track.x) / trackW * range + s->mMin;
        if (s->mStep > 0.0f) v = std::round(v / s->mStep) * s->mStep;
        s->mValue = std::clamp(v, s->mMin, s->mMax);
    }

    const float want = (s->mValue - s->mMin) / range * trackW;
    const float chase = (g_slider == s) ? 30.0f : 16.0f;
    s->sliderEase = MathUtils::animate(want, s->sliderEase, std::clamp(c.dt * chase, 0.0f, 1.0f));

    if (c.colSlider) {
        fill(c.dl, track, c.tint, 0.10f * c.a, trackH * 0.5f);
        if (s->sliderEase > 0.5f) {
            const ImVec4 filledR(track.x, track.y, track.x + s->sliderEase, track.w);
            shadeThemedSheet(c.dl, filledR, c.gradSpace ? *c.gradSpace : track,
                             1.0f, 0.9f * c.a, c.gradSpan, c.gradMs,
                             std::min(trackH * 0.5f, s->sliderEase * 0.5f), 0,
                             false, c.gradLut, c.gradLutN);
            const bool dragging = (g_slider == s);
            const float kr = (3.4f + 1.6f * std::clamp(h + (dragging ? 0.6f : 0.0f), 0.0f, 1.0f)) * c.scale;
            const ImVec2 kp(track.x + s->sliderEase, (track.y + track.w) * 0.5f);
            // A dark halo keeps the bright knob readable over the gradient.
            c.dl->AddCircleFilled(kp, kr + 1.4f * c.scale,
                                  ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.30f * c.a)), 20);
            c.dl->AddCircleFilled(kp, kr,
                                  ImGui::GetColorU32(ImVec4(c.knob.x, c.knob.y, c.knob.z, c.a)), 20);
        }
        return;
    }

    // Panel look: the track hugs the row's bottom edge as a slim underline.
    fill(c.dl, track, c.tint, 0.10f * c.a, 2.0f * c.scale);
    if (s->sliderEase > 0.5f) {
        const ImVec4 filled(track.x, track.y, track.x + s->sliderEase, track.w);
        gradientH(c.dl, filled, mix(c.accent, ImColor(1.0f, 1.0f, 1.0f, 1.0f), 0.35f), c.accent,
                  c.a, 2.0f * c.scale);
        const ImVec2 knob(track.x + s->sliderEase, (track.y + track.w) * 0.5f);
        const float kr = (4.0f + 1.5f * h) * c.scale;
        if (c.glow > 0.01f)
            ImRenderUtils::addGlowCircle(c.dl, knob, kr * 3.0f, c.accent, 0.35f * c.glow * c.a);
        c.dl->AddCircleFilled(knob, kr, ImGui::GetColorU32(ImVec4(c.knob.x, c.knob.y, c.knob.z, c.a)), 16);
        c.dl->AddCircle(knob, kr, ImGui::GetColorU32(ImVec4(c.hair.x, c.hair.y, c.hair.z, 0.28f * c.a)), 16, 1.0f);
    }
}

void drawEnumRow(const Ctx& c, EnumSetting* s, const ImVec4& row) {
    const float base = row.y + c.rowH;
    const ImVec4 head(row.x, row.y, row.z, base);
    const std::string key = "e" + std::to_string(reinterpret_cast<uintptr_t>(s));
    const bool hov = c.live && over(head);
    const float h = hoverOf(key, hov, c.dt);
    fill(c.dl, head, c.tint, 0.035f * h * c.a, 7.0f * c.scale);

    const float fs = (c.columnsBody ? 15.5f : 14.5f) * c.scale;
    const float padX = 12.0f * c.scale;
    const float labelX = c.columnsBody ? (c.columnsClean ? 0.0f : 8.0f) * c.scale : padX;
    text(c.dl, c.font, fs, ImVec2(head.x + labelX, (head.y + head.w - fs) * 0.5f), s->mName,
         c.inkMid, c.a, c.textShadow);

    const std::string cur = (s->mValue >= 0 && s->mValue < static_cast<int>(s->mValues.size()))
                              ? s->mValues[s->mValue] : std::string();
    const float cw = measure(c.bold, fs, cur);
    const float chevW = 12.0f * c.scale;
    const ImVec4 pill(head.z - padX - cw - chevW - 16.0f * c.scale, head.y + 5.0f * c.scale,
                      head.z - padX, head.w - 5.0f * c.scale);
    fill(c.dl, pill, c.tint, (0.05f + 0.04f * h) * c.a, (pill.w - pill.y) * 0.5f);
    text(c.dl, c.bold, fs, ImVec2(pill.x + 8.0f * c.scale, (pill.y + pill.w - fs) * 0.5f), cur,
         c.ink, c.a, c.textShadow);

    // Chevron rotates with the reveal, so the row states which way it will go.
    {
        const float t = easeOutCubic(s->enumSlide);
        const float cx = pill.z - 10.0f * c.scale;
        const float cy = (pill.y + pill.w) * 0.5f;
        const float w = 4.0f * c.scale, d = 2.5f * c.scale * (1.0f - 2.0f * t);
        const ImU32 col = ImGui::GetColorU32(ImVec4(c.inkMid.x, c.inkMid.y, c.inkMid.z, c.a));
        c.dl->AddLine(ImVec2(cx - w, cy - d), ImVec2(cx, cy + d), col, 1.6f * c.scale);
        c.dl->AddLine(ImVec2(cx, cy + d), ImVec2(cx + w, cy - d), col, 1.6f * c.scale);
    }

    if (hov && c.lmb) s->enumExtended = !s->enumExtended;
    if (hov && c.rmb && !s->mValues.empty())
        s->mValue = (s->mValue + 1) % static_cast<int>(s->mValues.size());

    const float reveal = easeOutBack(std::clamp(s->enumSlide, 0.0f, 1.0f));
    if (reveal <= 0.002f) return;

    const float optH = optionHeight(c);
    const int   n = static_cast<int>(s->mValues.size());
    const float visible = n * optH * reveal;
    const float listA = std::clamp(s->enumSlide * 2.2f, 0.0f, 1.0f);
    const float clipPad = 48.0f * c.scale;
    c.dl->PushClipRect(ImVec2(row.x - clipPad, base), ImVec2(row.z + clipPad, base + visible), true);

    if (!c.columnsBody)
        fill(c.dl, ImVec4(row.x + 8.0f * c.scale, base, row.z - 8.0f * c.scale, base + visible),
             c.plate, c.plateA * c.a, 7.0f * c.scale,
             ImDrawFlags_RoundCornersBottom);

    const float optFs = (c.columnsBody ? 15.5f : 13.5f) * c.scale;
    const float optInset = (c.columnsBody ? 14.0f : 10.0f) * c.scale;
    for (int i = 0; i < n; ++i) {
        const ImVec4 o(row.x + optInset, base + i * optH,
                       row.z - optInset, base + (i + 1) * optH);
        if (o.y >= base + visible) break;

        const float sel = 1.0f - std::clamp(std::fabs(static_cast<float>(i) - s->enumSelAnim), 0.0f, 1.0f);
        const bool  oHov = c.live && reveal > 0.6f && over(o);
        const float oh = hoverOf(key + "o" + std::to_string(i), oHov, c.dt);
        // Picking an option collapses the list — leaving it open after a choice
        // means every following click lands on options instead of the next row.
        if (oHov && c.lmb) { s->mValue = i; s->enumExtended = false; }

        if (sel > 0.004f || oh > 0.004f) {
            const ImVec4 hi(o.x, o.y + 1.0f, o.z, o.w - 1.0f);
            fill(c.dl, hi, ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f),
                 (0.14f * sel + 0.06f * oh) * c.a * listA, 6.0f * c.scale);
        }
        if (sel > 0.004f) {
            const ImVec4 bar(o.x, o.y + 5.0f * c.scale, o.x + 2.5f * c.scale, o.w - 5.0f * c.scale);
            fill(c.dl, bar, ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f),
                 sel * c.a * listA, 1.5f * c.scale);
        }
        const ImVec4 tc(c.inkLo.x + (c.ink.x - c.inkLo.x) * sel,
                        c.inkLo.y + (c.ink.y - c.inkLo.y) * sel,
                        c.inkLo.z + (c.ink.z - c.inkLo.z) * sel, 1.0f);
        text(c.dl, c.font, optFs,
             ImVec2(o.x + (c.columnsBody ? 6.0f : 14.0f) * c.scale,
                    (o.y + o.w - optFs) * 0.5f),
             s->mValues[i], tc, c.a * listA, c.textShadow);
    }
    c.dl->PopClipRect();
}

void drawColorRow(const Ctx& c, ColorSetting* s, const ImVec4& row) {
    const float base = row.y + c.rowH;
    const ImVec4 head(row.x, row.y, row.z, base);
    const std::string key = "c" + std::to_string(reinterpret_cast<uintptr_t>(s));
    const bool hov = c.live && over(head);
    const float h = hoverOf(key, hov, c.dt);
    fill(c.dl, head, c.tint, 0.035f * h * c.a, 7.0f * c.scale);

    const float fs = (c.columnsBody ? 15.5f : 14.5f) * c.scale;
    const float padX = 12.0f * c.scale;
    const float labelX = c.columnsBody ? (c.columnsClean ? 0.0f : 8.0f) * c.scale : padX;
    text(c.dl, c.font, fs, ImVec2(head.x + labelX, (head.y + head.w - fs) * 0.5f), s->mName,
         c.inkMid, c.a, c.textShadow);

    const float sz = 18.0f * c.scale;
    const ImVec4 swatch(head.z - padX - sz, (head.y + head.w - sz) * 0.5f,
                        head.z - padX, (head.y + head.w + sz) * 0.5f);
    fill(c.dl, swatch, ImVec4(s->mValue[0], s->mValue[1], s->mValue[2], 1.0f), c.a, 5.0f * c.scale);
    stroke(c.dl, swatch, c.tint, 0.22f * c.a, 5.0f * c.scale, 1.0f);

    if (hov && (c.lmb || c.rmb)) s->enumExtended = !s->enumExtended;

    const float reveal = easeOutBack(std::clamp(s->enumSlide, 0.0f, 1.0f));
    if (reveal <= 0.002f) return;

    const float ph = pickerHeight(c) * reveal;
    const float clipPad = 48.0f * c.scale;
    c.dl->PushClipRect(ImVec2(row.x - clipPad, base), ImVec2(row.z + clipPad, base + ph), true);
    const float cw = (row.z - row.x) - 24.0f * c.scale;
    const float chh = pickerHeight(c) - 34.0f * c.scale;
    drawGuiColorPicker(c, s->mValue,
        ImVec4(row.x + 12.0f * c.scale, base + 4.0f * c.scale, cw, chh));
    c.dl->PopClipRect();
}

void drawStringRow(const Ctx& c, StringSetting* s, const ImVec4& row) {
    const std::string key = "s" + std::to_string(reinterpret_cast<uintptr_t>(s));
    const bool active = (g_strEdit == static_cast<void*>(s));
    const bool hov = c.live && over(row);
    const float h = hoverOf(key, hov, c.dt);
    fill(c.dl, row, c.tint, 0.035f * h * c.a, 7.0f * c.scale);

    const float fs = (c.columnsBody ? 15.5f : 14.5f) * c.scale;
    const float padX = 12.0f * c.scale;
    const float labelX = c.columnsBody ? (c.columnsClean ? 0.0f : 8.0f) * c.scale : padX;
    text(c.dl, c.font, fs, ImVec2(row.x + labelX, (row.y + row.w - fs) * 0.5f), s->mName,
         c.inkMid, c.a, c.textShadow);

    const float maxW = (row.z - row.x) * 0.40f;
    const std::string shown = elide(c.font, 13.0f * c.scale,
                                    s->mValue.empty() ? std::string("click to edit") : s->mValue, maxW);
    const float tw = measure(c.font, 13.0f * c.scale, shown);
    const ImVec4 pill(row.z - padX - tw - 16.0f * c.scale, row.y + 5.0f * c.scale,
                      row.z - padX, row.w - 5.0f * c.scale);
    fill(c.dl, pill, c.tint, (active ? 0.12f : 0.05f + 0.04f * h) * c.a,
         (pill.w - pill.y) * 0.5f);
    text(c.dl, c.font, 13.0f * c.scale,
         ImVec2(pill.x + 8.0f * c.scale, (pill.y + pill.w - 13.0f * c.scale) * 0.5f), shown,
         s->mValue.empty() ? c.inkLo : c.ink, c.a, false);

    if (hov && (c.lmb || c.rmb)) {
        g_strEdit = active ? nullptr : static_cast<void*>(s);
        g_strFocus = false;
    }
}

void drawKeybindRow(const Ctx& c, Module* mod, const ImVec4& row, const char* label = "Keybind") {
    const std::string key = "k" + mod->GetName();
    const bool hov = c.live && over(row);
    const float h = hoverOf(key, hov, c.dt);
    fill(c.dl, row, c.tint, 0.035f * h * c.a, 7.0f * c.scale);
    if (hov && c.lmb) g_binding = mod;

    const float fs = (c.columnsBody ? 15.5f : 14.5f) * c.scale;
    const float padX = 12.0f * c.scale;
    const float labelX = c.columnsBody ? (c.columnsClean ? 0.0f : 8.0f) * c.scale : padX;
    text(c.dl, c.font, fs, ImVec2(row.x + labelX, (row.y + row.w - fs) * 0.5f),
         std::string(label), c.inkMid, c.a, c.textShadow);

    const bool waiting = (g_binding == mod);
    const std::string name = waiting ? "press a key..." : KeyboardUtils::GetKeyName(mod->GetKeybind());
    const float tw = measure(c.bold, fs, name);
    const ImVec4 pill(row.z - padX - tw - 16.0f * c.scale, row.y + 5.0f * c.scale,
                      row.z - padX, row.w - 5.0f * c.scale);
    if (waiting) {
        fill(c.dl, pill, ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f),
             0.22f * c.a, (pill.w - pill.y) * 0.5f);
    } else {
        fill(c.dl, pill, c.tint, (0.05f + 0.04f * h) * c.a, (pill.w - pill.y) * 0.5f);
    }
    text(c.dl, c.bold, fs, ImVec2(pill.x + 8.0f * c.scale, (pill.y + pill.w - fs) * 0.5f), name,
         waiting ? ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f) : c.ink,
         c.a, c.textShadow);
}

void drawKeyRow(const Ctx& c, const ImVec4& row, const char* label, int* target,
                const char* prefix, const std::string& desc) {
    const std::string key = "key" + std::string(label);
    const bool hov = c.live && over(row);
    const float h = hoverOf(key, hov, c.dt);
    fill(c.dl, row, c.tint, 0.035f * h * c.a, 7.0f * c.scale);
    captureTip(c, row, desc);
    if (hov && c.lmb) g_keyTarget = target;

    const float fs = (c.columnsBody ? 15.5f : 14.5f) * c.scale;
    const float padX = 12.0f * c.scale;
    const float labelX = c.columnsBody ? (c.columnsClean ? 0.0f : 8.0f) * c.scale : padX;
    text(c.dl, c.font, fs, ImVec2(row.x + labelX, (row.y + row.w - fs) * 0.5f),
         std::string(label), c.inkMid, c.a, c.textShadow);

    const bool waiting = (g_keyTarget == target);
    const std::string name = waiting
        ? std::string("press a key...")
        : std::string(prefix ? prefix : "") + KeyboardUtils::GetKeyName(*target);
    const float tw = measure(c.bold, fs, name);
    const ImVec4 pill(row.z - padX - tw - 16.0f * c.scale, row.y + 5.0f * c.scale,
                      row.z - padX, row.w - 5.0f * c.scale);
    const ImVec4 accentV(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f);
    if (waiting) fill(c.dl, pill, accentV, 0.22f * c.a, (pill.w - pill.y) * 0.5f);
    else fill(c.dl, pill, c.tint, (0.05f + 0.04f * h) * c.a, (pill.w - pill.y) * 0.5f);
    text(c.dl, c.bold, fs, ImVec2(pill.x + 8.0f * c.scale, (pill.y + pill.w - fs) * 0.5f), name,
         waiting ? accentV : c.ink, c.a, c.textShadow);
}

void drawSetting(const Ctx& c, Setting* s, const ImVec4& row) {
    switch (s->mType) {
    case SettingType::Bool:   drawBoolRow(c, static_cast<BoolSetting*>(s), row); break;
    case SettingType::Number: drawNumberRow(c, static_cast<NumberSetting*>(s), row); break;
    case SettingType::Enum:   drawEnumRow(c, static_cast<EnumSetting*>(s), row); break;
    case SettingType::Color:  drawColorRow(c, static_cast<ColorSetting*>(s), row); break;
    case SettingType::String: drawStringRow(c, static_cast<StringSetting*>(s), row); break;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Module cards keep their header height while the settings body expands inline.
// ---------------------------------------------------------------------------
namespace {

float cardHeaderHeight(const Ctx& c) { return 40.0f * c.scale; }

void drawResetPill(const Ctx& c, Module* owner, const ImVec4& row, bool left) {
    const float bh = row.w - row.y;
    const float ph = std::min(22.0f * c.scale, bh - 2.0f * c.scale);
    if (ph < 6.0f) return;
    const float pw = 104.0f * c.scale;
    const float bx = left ? row.x + 2.0f * c.scale
                          : (row.x + row.z) * 0.5f - pw * 0.5f;
    const ImVec4 btn(bx, row.y + (bh - ph) * 0.5f,
                     bx + pw, row.y + (bh + ph) * 0.5f);
    const bool hov = c.live && over(btn);
    const float h = hoverOf("rst" + owner->GetName(), hov, c.dt);
    fill(c.dl, btn, ImColor(30, 33, 42, 255), 0.85f * c.a, 4.0f * c.scale);
    if (h > 0.004f)
        fill(c.dl, btn, ImVec4(1, 1, 1, 1), 0.05f * h * c.a, 4.0f * c.scale);
    stroke(c.dl, btn, ImVec4(1, 1, 1, 1), (0.08f + 0.14f * h) * c.a, 4.0f * c.scale, 1.0f);
    const float fs = 12.0f * c.scale;
    const std::string label = "Reset settings";
    const float lw = measure(c.font, fs, label);
    text(c.dl, c.font, fs, ImVec2(bx + (pw - lw) * 0.5f, row.y + (bh - fs) * 0.5f),
         label, hov ? c.ink : c.inkLo, c.a, false);
    captureTip(c, btn, "Reset every setting of this module to its default");
    if (hov && c.lmb) owner->ResetSettings();
}

float cardBodyHeight(const Ctx& c, Module* mod, float cardWidth) {
    (void)cardWidth;
    float h = 0.0f;
    for (Setting* s : mod->mSettings) {
        if (!s || !s->mVisible) continue;
        h += settingHeight(c, s);
    }
    if (mod->canBind) h += c.rowH;
    if (h > 0.0f) h += 8.0f * c.scale; // bottom breathing room
    // The reset pill sits under the (optional) keybind row.
    if (h > 0.0f) h += 24.0f * c.scale + 4.0f * c.scale;
    return h;
}

float cardHeight(const Ctx& c, Module* mod, float cardWidth) {
    return cardHeaderHeight(c) +
           cardBodyHeight(c, mod, cardWidth) * easeOutBack(std::clamp(mod->cAnim, 0.0f, 1.0f));
}

void drawCard(const Ctx& c, Module* mod, const ImVec4& box, const std::string& casedName) {
    const float headH = cardHeaderHeight(c);
    const ImVec4 head(box.x, box.y, box.z, box.y + headH);
    const float en = easeOutCubic(std::clamp(mod->enableAnim, 0.0f, 1.0f));

    const std::string key = "card" + mod->GetName();
    const bool hovHead = c.live && over(head);
    const float h = hoverOf(key, hovHead, c.dt);
    const float cardRound = 6.0f * c.scale;

    // Hover darkens the card without changing its geometry.
    fill(c.dl, box, ImVec4(0, 0, 0, 1), 0.16f * c.surface * c.a, cardRound);
    if (h > 0.004f)
        fill(c.dl, box, ImVec4(0, 0, 0, 1), 0.10f * h * c.a, cardRound);

    // Pin glyph. Revealed by hovering ANYWHERE on the card (latched visible
    // while pinned), sliding in from the right as it appears.
    const bool hovCard = c.live && over(box);
    const float pinShowTarget = (mod->guiPinned || hovCard) ? 1.0f : 0.0f;
    mod->guiPinShow += (pinShowTarget - mod->guiPinShow) *
                       std::clamp(c.dt * (pinShowTarget > 0.5f ? 15.0f : 10.0f), 0.0f, 1.0f);
    if (mod->guiPinShow < 0.002f) mod->guiPinShow = 0.0f;
    if (mod->guiPinShow > 0.998f) mod->guiPinShow = 1.0f;

    const float pinTarget = mod->guiPinned ? 1.0f : 0.0f;
    mod->guiPinAnim += (pinTarget - mod->guiPinAnim) *
                       std::clamp(c.dt * (mod->guiPinned ? 17.0f : 13.0f), 0.0f, 1.0f);
    if (mod->guiPinAnim < 0.002f) mod->guiPinAnim = 0.0f;
    if (mod->guiPinAnim > 0.998f) mod->guiPinAnim = 1.0f;

    const float pinS = 14.0f * c.scale;
    const float pinHome = box.z - 12.0f * c.scale - pinS;
    const float pinShow = easeOutCubic(mod->guiPinShow);
    const float pinX = pinHome + (1.0f - pinShow) * 10.0f * c.scale;
    const ImVec4 pinRect(pinHome - 4.0f * c.scale, head.y,
                         pinHome + pinS + 4.0f * c.scale, head.w);
    const bool pinClicked = hovHead && c.live && c.lmb && over(pinRect);

    if (hovHead) {
        if (pinClicked) {
            mod->guiPinned = !mod->guiPinned;
        } else if (c.lmb) {
            mod->Toggle();
            if (Module* ts = ModuleManager::GetInstance().GetModuleByName("ToggleSounds"))
                if (ts->IsEnabled()) ToggleSounds::PlayToggleSound(mod->IsEnabled());

            Notifications::AddNotification(
                mod->GetName(),
                mod->IsEnabled() ? NotifType::ToggleOn : NotifType::ToggleOff);
        }
        if (c.rmb) mod->showSettings = !mod->showSettings;
    }

    // Enabled names switch to the current accent.
    const float nameSize = 15.0f * c.scale;
    const float nameX = box.x + 12.0f * c.scale;
    const float nameY = head.y + 6.0f * c.scale;
    const ImVec4 nameCol = en > 0.5f
        ? ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f)
        : ImVec4(1, 1, 1, 1);
    text(c.dl, c.bold, nameSize, ImVec2(nameX, nameY), casedName, nameCol, c.a, c.textShadow);

    const std::string mode = modeOf(mod);
    float descX = nameX;
    if (!mode.empty()) {
        const float ms = 11.5f * c.scale;
        const float nw = measure(c.bold, nameSize, casedName);
        text(c.dl, c.font, ms, ImVec2(nameX + nw + 8.0f * c.scale, nameY + 3.0f * c.scale),
             mode, kTextLo, c.a * 0.9f, false);
    }
    if (!mod->GetDescription().empty()) {
        const float ds = 11.5f * c.scale;
        // Keep the description clear of the pin/chevron zone on the right.
        const float descMaxW = (box.z - 12.0f * c.scale - pinS - 16.0f * c.scale) - descX;
        const std::string line = elide(c.font, ds, mod->GetDescription(), descMaxW);
        text(c.dl, c.font, ds, ImVec2(descX, nameY + nameSize + 1.0f * c.scale), line,
             kTextLo, 0.8f * c.a, false);
    }

    // Chevron (settings affordance) at the right, before the pin.
    const float cardW = box.z - box.x;
    const bool hasBody = cardBodyHeight(c, mod, cardW) > 0.5f;
    if (hasBody) {
        const float t = easeOutCubic(mod->cAnim);
        const float cx2 = pinHome - 18.0f * c.scale;
        const float cy2 = head.y + headH * 0.5f;
        const float w = 4.0f * c.scale, d = 2.5f * c.scale * (1.0f - 2.0f * t);
        const ImU32 chev = ImGui::GetColorU32(
            ImVec4(kTextLo.x, kTextLo.y, kTextLo.z, (0.6f + 0.4f * h) * c.a));
        c.dl->AddLine(ImVec2(cx2 - w, cy2 - d), ImVec2(cx2, cy2 + d), chev, 1.6f * c.scale);
        c.dl->AddLine(ImVec2(cx2, cy2 + d), ImVec2(cx2 + w, cy2 - d), chev, 1.6f * c.scale);
    }
    if (mod->guiPinShow > 0.004f) {
        ImFont* icons = RenderManager::GetInstance().iconFont
                          ? RenderManager::GetInstance().iconFont
                          : c.font;
        const float pinHover = hoverOf(key + "pin", c.live && over(pinRect), c.dt);
        const float pa = easeOutCubic(mod->guiPinAnim);
        const float pinA = pinShow * (0.42f + 0.30f * pinHover + 0.28f * pa) * c.a;
        if (pinA > 0.01f) {
            const ImVec4 dim(kTextLo.x, kTextLo.y, kTextLo.z, 1.0f);
            const ImU32 pinCol = ImGui::GetColorU32(
                ImVec4(dim.x + (c.accent.Value.x - dim.x) * pa,
                       dim.y + (c.accent.Value.y - dim.y) * pa,
                       dim.z + (c.accent.Value.z - dim.z) * pa, pinA));
            const float lift = pa * 1.5f * c.scale;
            c.dl->AddText(icons, pinS,
                          ImVec2(pinX, head.y + (headH - pinS) * 0.5f - 1.0f - lift),
                          pinCol, "j");
        }
    }

    if (mod->cAnim <= 0.002f || !hasBody) return;

    const float openT = std::clamp(mod->cAnim, 0.0f, 1.0f);
    const float bodyH = cardBodyHeight(c, mod, cardW) * easeOutBack(openT);
    const float contentA = std::clamp(openT * 2.2f, 0.0f, 1.0f);
    const float drop = (1.0f - easeOutCubic(std::clamp(openT * 1.5f, 0.0f, 1.0f))) *
                       7.0f * c.scale;
    const ImVec4 body(box.x, head.w, box.z, head.w + bodyH);
    fill(c.dl, body, ImVec4(0.0f, 0.0f, 0.0f, 1.0f), 0.22f * c.surface * c.a * contentA,
         cardRound, ImDrawFlags_RoundCornersBottom);
    c.dl->AddLine(ImVec2(body.x + 10.0f * c.scale, body.y),
                  ImVec2(body.z - 10.0f * c.scale, body.y),
                  ImGui::GetColorU32(ImVec4(1, 1, 1, 0.05f * c.a * contentA)), 1.0f);

    Ctx bc = c;
    bc.a = c.a * contentA;
    if (openT < 0.85f) { bc.live = false; bc.lmb = false; bc.rmb = false; }

    c.dl->PushClipRect(ImVec2(body.x, body.y), ImVec2(body.z, body.w), true);
    float y = body.y + 4.0f * c.scale + drop;
    const float inset = 8.0f * c.scale;

    for (Setting* s : mod->mSettings) {
        if (!s || !s->mVisible) continue;
        const float sh = settingHeight(c, s);
        const ImVec4 row(body.x + inset, y, body.z - inset, y + sh);
        if (row.y < body.w && row.w > body.y) {
            captureTip(bc, ImVec4(row.x, row.y, row.z, row.y + c.rowH), s->mDescription);
            drawSetting(bc, s, row);
        }
        y += sh;
    }
    if (mod->canBind) {
        const ImVec4 row(body.x + inset, y, body.z - inset, y + c.rowH);
        if (row.y < body.w) drawKeybindRow(bc, mod, row);
        y += c.rowH;
    }
    // Reset-to-defaults pill at the very bottom of the body.
    {
        const ImVec4 row(body.x + inset, y, body.z - inset, y + 24.0f * c.scale);
        if (row.y + 24.0f * c.scale < body.w + 6.0f * c.scale) drawResetPill(bc, mod, row, false);
    }
    c.dl->PopClipRect();
}

} // namespace

// ---------------------------------------------------------------------------
// Panel chrome: rail, header, search field.
// ---------------------------------------------------------------------------
namespace {

void drawPanelShell(const Ctx& c, const ImVec4& panel, const ImVec4& rail) {
    (void)rail;
    if (c.a <= 0.004f) return;

    if (c.shadow && c.shadowK > 0.001f) {
        ImRenderUtils::fillShadowRectangle(panel, ImColor(0.0f, 0.0f, 0.0f, 1.0f),
                                           0.55f * c.shadowK * c.a, c.round, 20.0f * c.scale, 0.85f, c.dl);
    }
    fill(c.dl, panel, kPanel, 0.985f * c.surface * c.a, c.round);
    stroke(c.dl, panel, ImVec4(1, 1, 1, 1), 0.055f * c.a, c.round, 1.0f);

    // Sidebar separator: a single 1px line where the rail ends.
    c.dl->AddLine(ImVec2(rail.z, panel.y + c.round * 0.5f),
                  ImVec2(rail.z, panel.w - c.round * 0.5f),
                  ImGui::GetColorU32(ImVec4(1, 1, 1, 0.05f * c.a)), 1.0f);
}

// Rail entry: plain text, soft flat hover fill, accent text while active.
// Returns the hover amount so the caller can react.
float drawRailItem(const Ctx& c, const ImVec4& r, const std::string& label,
                   const char* glyph, bool active, const std::string& key) {
    const bool hov = c.live && over(r);
    const float h = hoverOf(key, hov, c.dt);

    if (h > 0.004f && !active)
        fill(c.dl, r, ImVec4(1, 1, 1, 1), 0.04f * h * c.a, 7.0f * c.scale);
    if (active)
        fill(c.dl, r, ImVec4(1, 1, 1, 1), 0.055f * c.a, 7.0f * c.scale);

    // Category titles render LARGER than the module names below them (module
    // rows draw at 16-18px) - a header smaller than its content read as noise.
    const float fs = 19.0f * c.scale;
    ImFont* icons = RenderManager::GetInstance().iconFont;
    float tx = r.x + 14.0f * c.scale + 2.0f * h * c.scale;
    if (icons && glyph && glyph[0]) {
        const float gs = 17.0f * c.scale;
        const ImVec4 gc = active ? ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f)
                                 : kTextLo;
        c.dl->AddText(icons, gs, ImVec2(tx, (r.y + r.w - gs) * 0.5f),
                      ImGui::GetColorU32(ImVec4(gc.x, gc.y, gc.z,
                                                (active ? 1.0f : 0.6f + 0.4f * h) * c.a)),
                      glyph);
        tx += gs + 9.0f * c.scale;
    }
    const ImVec4 tc = active
        ? ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f)
        : ImVec4(kTextMid.x, kTextMid.y, kTextMid.z, 0.72f + 0.28f * h);
    text(c.dl, active ? c.bold : c.font, fs, ImVec2(tx, (r.y + r.w - fs) * 0.5f), label, tc,
         c.a, c.textShadow);
    return h;
}

} // namespace


struct SearchReveal { float t; };   // 0..1 appear progress, one per code point
static std::vector<SearchReveal> g_searchReveal;

// WantsSearchKeys is defined further down, after the Columns state block: the
// gate needs g_colTab, because g_tab is stale while Columns is on screen.

void ClickGUI::FeedSearchChar(unsigned int ch) {
    if (!WantsSearchKeys()) return;
    if (ch == 0x08) {                              // backspace: pop one code point
        size_t len = std::strlen(g_search);
        if (len == 0) return;
        // Step back over UTF-8 continuation bytes (10xxxxxx) so multi-byte
        // glyphs (cyrillic, emoji) delete whole characters, not halves.
        --len;
        while (len > 0 && ((unsigned char)g_search[len] & 0xC0) == 0x80) --len;
        g_search[len] = '\0';
    } else if (ch >= 0x20 && ch != 0x7F) {         // printable
        const size_t len = std::strlen(g_search);
        if (len + 4 < sizeof(g_search)) {
            // Encode as UTF-8 so the font atlas can actually draw the glyph.
            if (ch < 0x80) {
                g_search[len] = (char)ch; g_search[len + 1] = '\0';
            } else if (ch < 0x800) {
                g_search[len]     = (char)(0xC0 | (ch >> 6));
                g_search[len + 1] = (char)(0x80 | (ch & 0x3F));
                g_search[len + 2] = '\0';
            } else {
                g_search[len]     = (char)(0xE0 | (ch >> 12));
                g_search[len + 1] = (char)(0x80 | ((ch >> 6) & 0x3F));
                g_search[len + 2] = (char)(0x80 | (ch & 0x3F));
                g_search[len + 3] = '\0';
            }
            g_searchReveal.push_back({ 0.0f });

            static bool s_logged = false;
            if (!s_logged) { s_logged = true; LOG_INFO("ClickGUI live search: input flows", "ClickGUI"); }
        }
    }
}

void ClickGUI::FeedSearchKeyDown(unsigned int vk) {
    if (!WantsSearchKeys()) return;

    if (vk == VK_BACK) { FeedSearchChar(0x08); return; }

    if (vk == 'V' && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
        // Ctrl+V paste. OpenClipboard can fail (another window owns it) — bail.
        if (!OpenClipboard(nullptr)) return;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
                for (const wchar_t* p = w; *p; ++p)
                    FeedSearchChar(*p < 0x10000u ? (unsigned)*p : '?');
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        return;
    }

    // Only keys that PRODUCE text. Modifiers/F-keys/arrows must not pollute
    // the query — otherwise menu navigation would type garbage.
    const bool text =
        (vk >= '0' && vk <= 'Z') ||                      // digits + letters (incl. OEM gap is checked below)
        vk == VK_SPACE ||
        (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) ||
        (vk >= 0xBA && vk <= 0xC0) ||                    // ;=,-./`
        (vk >= 0xDB && vk <= 0xDF);                      // [\]'
    if (!text) return;

    BYTE keys[256] = {};
    if (!GetKeyboardState(keys)) return;
    wchar_t out[8] = {};
    const int n = ToUnicode((UINT)vk, 0, keys, out, 8, 0);
    for (int i = 0; i < n; ++i) FeedSearchChar((unsigned int)out[i]);
}

bool ClickGUI::EscapeClearsSearch() {
    if (g_search[0] == '\0') return false;
    g_search[0] = '\0';
    return true;
}

namespace {

void drawSearchText(const Ctx& c, const ImVec4& content, float dt, float sizeMul = 1.0f) {
    const char* q = g_search;
    const size_t len = std::strlen(q);
    const float fs = 18.0f * c.scale * sizeMul;

    // Empty query: a centered hint, same slot the typed query occupies.
    if (len == 0) {
        const std::string hint = "Type anything to search...";
        const float hw = measure(c.font, fs, hint);
        const float hx = (content.x + content.z) * 0.5f - hw * 0.5f;
        const float hy = content.y + 2.0f * c.scale;
        text(c.dl, c.font, fs, ImVec2(hx, hy), hint, kTextLo, 0.85f * c.a, false);
        return;
    }

    if (g_searchReveal.size() < len) g_searchReveal.resize(len, { 1.0f });
    else if (g_searchReveal.size() > len) g_searchReveal.resize(len);

    // Measure the full string before easing its centered anchor.
    const std::string full(q);
    const float totalW = measure(c.font, fs, full);

    // Ease the anchor instead of moving the text abruptly after each character.
    static float s_anchorX = -1.0f;
    const float targetX = (content.x + content.z) * 0.5f - totalW * 0.5f;
    if (s_anchorX < 0.0f) s_anchorX = targetX;
    s_anchorX += (targetX - s_anchorX) * std::clamp(dt * 12.0f, 0.0f, 1.0f);
    if (std::fabs(targetX - s_anchorX) < 0.25f) s_anchorX = targetX;

    const float textY = content.y + 2.0f * c.scale;
    const float revealSpeed = std::clamp(dt * 9.0f, 0.0f, 1.0f);
    float x = s_anchorX;

    c.dl->PushClipRect(ImVec2(content.x, content.y - 4.0f * c.scale),
                       ImVec2(content.z, content.y + fs + 8.0f * c.scale), true);
    for (size_t i = 0; i < len; ++i) {
        SearchReveal& r = g_searchReveal[i];
        r.t += (1.0f - r.t) * revealSpeed;
        if (r.t > 0.996f) r.t = 1.0f;
        const float e = easeOutBack(r.t);
        // One code point = one animation unit: scan the full UTF-8 sequence.
        unsigned int clen = 1;
        while (i + clen < len && ((unsigned char)q[i + clen] & 0xC0) == 0x80) ++clen;
        const std::string glyph(q + i, clen);
        i += clen - 1;
        const float w = measure(c.font, fs, glyph);
            // Each UTF-8 code point rises into place.
        const float a = std::clamp(e, 0.0f, 1.0f);
        text(c.dl, c.font, fs, ImVec2(x, textY + (1.0f - e) * 4.0f * c.scale),
             glyph, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), a * c.a, c.textShadow);
        x += w;
    }
    c.dl->PopClipRect();

    // Caret immediately follows the rendered code points.
    const float caretA = (std::sin(GetTickCount64() / 150.0) + 1.0) * 0.5;
    fill(c.dl, ImVec4(x + 3.0f * c.scale, textY + 1.0f, x + 4.5f * c.scale, textY + fs + 1.0f),
         ImVec4(1.0f, 1.0f, 1.0f, 1.0f), (0.25f + 0.75f * (float)caretA) * c.a, 1.0f);
}

} // namespace

// ---------------------------------------------------------------------------
// Configs and Changelog bodies. Both live inside the same panel content area.
// ---------------------------------------------------------------------------
namespace {

// Small pill button with an eased hover fill. Returns true on click.
bool pillButton(const Ctx& c, const ImVec4& r, const std::string& label, const ImColor& tint,
                const std::string& key) {
    const bool hov = c.live && over(r);
    const float h = hoverOf(key, hov, c.dt);
    const float round = std::min(8.0f * c.scale, (r.w - r.y) * 0.5f);
    fill(c.dl, r, ImVec4(tint.Value.x, tint.Value.y, tint.Value.z, 1.0f),
         (0.14f + 0.28f * h) * c.a, round);
    stroke(c.dl, r, ImVec4(tint.Value.x, tint.Value.y, tint.Value.z, 1.0f),
           (0.30f + 0.40f * h) * c.a, round, 1.0f);
    const float fs = 13.0f * c.scale;
    const float tw = measure(c.bold, fs, label);
    text(c.dl, c.bold, fs, ImVec2(r.x + (r.z - r.x - tw) * 0.5f, (r.y + r.w - fs) * 0.5f),
         label, kTextHi, c.a, c.textShadow);
    return hov && c.lmb;
}

// Returns the laid-out content height so the caller can size its scrollbar.
float drawConfigsBody(const Ctx& c, const ImVec4& area) {
    const float pad = 4.0f * c.scale;
    float y = area.y;

    // --- new profile row ---
    const float inputH = 36.0f * c.scale;
    const float btnW = 84.0f * c.scale;
    const ImVec4 input(area.x, y, area.z - btnW - 8.0f * c.scale, y + inputH);
    fill(c.dl, input, ImVec4(1, 1, 1, 1), 0.05f * c.a, 8.0f * c.scale);
    stroke(c.dl, input, ImVec4(1, 1, 1, 1), 0.09f * c.a, 8.0f * c.scale, 1.0f);

    static char name[64] = "";
    if (c.a > 0.003f && !g_strEdit) {
        const ScopedGuiPointer pointer(c.live);
        const float fs = ImGui::GetFontSize();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::SetNextWindowPos(ImVec2(input.x + 12.0f * c.scale,
                                       (input.y + input.w) * 0.5f - fs * 0.5f));
        ImGui::SetNextWindowSize(ImVec2(input.z - input.x - 20.0f * c.scale, fs + 2.0f));
        ImGui::Begin("##SunsteadCfgName", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        const UiDrawRange inputRange(ImGui::GetWindowDrawList());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(kTextHi.x, kTextHi.y, kTextHi.z, c.a));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(kTextLo.x, kTextLo.y, kTextLo.z, c.a));
        ImGui::SetNextItemWidth(input.z - input.x - 24.0f * c.scale);
        ImGui::InputTextWithHint("##cfgname", "New profile name", name, sizeof(name));
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(1);
        ImGui::End();
        ImGui::PopStyleVar(2);
        inputRange.finish(g_uiTransform.fade);
    }

    const ImVec4 create(area.z - btnW, y, area.z, y + inputH);
    if (pillButton(c, create, "Create", c.accent, "cfgcreate")) {
        const std::string n(name);
        if (!n.empty()) { ConfigManager::Save(n); name[0] = '\0'; }
    }
    y += inputH + 18.0f * c.scale;

    // --- saved list ---
    const auto list = ConfigManager::ListConfigs();
    text(c.dl, c.font, 12.0f * c.scale, ImVec2(area.x + 2.0f, y), std::string("SAVED PROFILES"),
         kTextLo, c.a, false);
    y += 20.0f * c.scale;

    if (list.empty()) {
        text(c.dl, c.font, 14.0f * c.scale, ImVec2(area.x + 2.0f, y),
             std::string("Nothing saved yet."), kTextLo, c.a, false);
        y += 26.0f * c.scale;
    }

    const float rowH = 42.0f * c.scale;
    const float aw = 62.0f * c.scale, gap = 6.0f * c.scale;
    for (const auto& conf : list) {
        const ImVec4 row(area.x, y, area.z, y + rowH - pad);
        // Rows scrolled out of view are skipped, but y still advances so the
        // reported content height stays correct.
        if (row.w > area.y - rowH && row.y < area.w + rowH) {
            const bool hov = c.live && over(row);
            const float h = hoverOf("cfgrow" + conf, hov, c.dt);
            fill(c.dl, row, ImVec4(1, 1, 1, 1), (0.035f + 0.035f * h) * c.a, 9.0f * c.scale);
            if (h > 0.004f) {
                const ImVec4 edge(row.x, row.y + 8.0f * c.scale, row.x + 2.5f * c.scale,
                                  row.w - 8.0f * c.scale);
                fill(c.dl, edge, ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f),
                     h * c.a, 1.5f * c.scale);
            }
            const float fs = 14.5f * c.scale;
            text(c.dl, c.bold, fs, ImVec2(row.x + (14.0f + 4.0f * h) * c.scale,
                                          (row.y + row.w - fs) * 0.5f),
                 conf, kTextHi, c.a, c.textShadow);

            const float by = (row.y + row.w) * 0.5f - 13.0f * c.scale;
            const ImVec4 del(row.z - 10.0f * c.scale - aw, by, row.z - 10.0f * c.scale,
                             by + 26.0f * c.scale);
            const ImVec4 sav(del.x - gap - aw, by, del.x - gap, by + 26.0f * c.scale);
            const ImVec4 load(sav.x - gap - aw, by, sav.x - gap, by + 26.0f * c.scale);

            if (pillButton(c, load, "Load", ImColor(0.35f, 0.60f, 1.0f, 1.0f), "cfgl" + conf))
                ConfigManager::Load(conf);
            if (pillButton(c, sav, "Save", ImColor(0.35f, 0.78f, 0.50f, 1.0f), "cfgs" + conf))
                ConfigManager::Save(conf);
            if (pillButton(c, del, "Delete", ImColor(0.85f, 0.32f, 0.36f, 1.0f), "cfgd" + conf)) {
                std::error_code ec;
                std::filesystem::remove(ConfigManager::GetConfigDirectory() + conf + ".json", ec);
            }
        }
        y += rowH;
    }

    y += 8.0f * c.scale;
    const ImVec4 folder(area.x, y, area.x + 150.0f * c.scale, y + 32.0f * c.scale);
    if (pillButton(c, folder, "Open config folder", ImColor(1.0f, 1.0f, 1.0f, 1.0f), "cfgfolder")) {
        const std::string dir = ConfigManager::GetConfigDirectory();
        ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    y += 32.0f * c.scale;

    return y - area.y;
}

// Returns the laid-out content height so the caller can size its scrollbar.
float drawChangelogBody(const Ctx& c, const ImVec4& area) {
    struct Entry { const char* ver; const char* date; bool latest; std::vector<const char*> notes; };
    static const std::vector<Entry> entries = {
        { "1.4.8", "Oct 10, 2026", true, {
            "Added ViewModel module",
            "Added SuperHeroFX module",
            "Added Player module (WIP, SoundCloud only rn)",
            "Added Trail module",
            "Added Chams module",
            "Added Blink module",
            "Added AirJump module",
            "Added Remove color codes option to Translate",
            "Added Shulker Boxes to BlockESP list",
            "Added Inventory Only option to Offhand",
            "Added Silent mode to OreMiner",
            "Added Render Self option to ESP",
            "Added HP Show to ESP",
            "Added .binds command (just to see active keybinds)",
            "Fixed some stuff in NoClip",
            "Fixed passing through blocks in FreeCam",
            "Fixed In-World overlay with Zoom module enabled",
            "Fixed rotations in Scaffold",
            "Reworked hold keybinds",
            "Some render stuff in ClickGUI and NameTags",
            "Some code optimization",
            "Just added animations to hotbar",
            "Just removed watermark from inventory screen",
        } },
    };

    float y = area.y;
    for (const auto& e : entries) {
        const std::string ver = std::string("v") + e.ver;
        const float vs = 17.0f * c.scale;
        text(c.dl, c.bold, vs, ImVec2(area.x, y), ver,
             e.latest ? ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f) : kTextHi,
             c.a, c.textShadow);
        const float vw = measure(c.bold, vs, ver);
        text(c.dl, c.font, 12.5f * c.scale, ImVec2(area.x + vw + 10.0f * c.scale, y + 3.0f * c.scale),
             std::string(e.date), kTextLo, c.a, false);

        if (e.latest) {
            const std::string tag = "LATEST";
            const float ts = 11.0f * c.scale;
            const float tw = measure(c.bold, ts, tag);
            const ImVec4 badge(area.x + vw + 10.0f * c.scale + measure(c.font, 12.5f * c.scale, e.date) + 12.0f * c.scale,
                               y + 1.0f * c.scale, 0.0f, y + ts + 7.0f * c.scale);
            const ImVec4 b(badge.x, badge.y, badge.x + tw + 12.0f * c.scale, badge.w);
            fill(c.dl, b, ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f),
                 0.18f * c.a, (b.w - b.y) * 0.5f);
            text(c.dl, c.bold, ts, ImVec2(b.x + 6.0f * c.scale, b.y + 3.0f * c.scale), tag,
                 ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f), c.a, false);
        }
        y += 30.0f * c.scale;

        for (const char* note : e.notes) {
            const float ns = 13.5f * c.scale;
            if (y + ns > area.y && y < area.w) {
                c.dl->AddCircleFilled(ImVec2(area.x + 4.0f * c.scale, y + ns * 0.5f), 2.0f * c.scale,
                                      ImGui::GetColorU32(ImVec4(c.accent.Value.x, c.accent.Value.y,
                                                                c.accent.Value.z, 0.8f * c.a)), 8);
                text(c.dl, c.font, ns, ImVec2(area.x + 16.0f * c.scale, y), std::string(note),
                     kTextMid, c.a, false);
            }
            y += 24.0f * c.scale;
        }
        y += 14.0f * c.scale;
    }

    return y - area.y;
}

float drawSettingsBody(const Ctx& c, Module* owner, const ImVec4& area) {
    struct Group { const char* title; int col; int extras; std::vector<const char*> names; };
    static const std::vector<Group> groups = {

        { "APPEARANCE", 0, 0, { "Saturation", "Code Color" } },
        { "DETAILS",    0, 0, { "Glow", "Shadow", "Shadow Strength", "Text Shadow", "Name Casing",
                                "Sort Mode", "Tooltips" } },

        { "MENU",       1, 0, { "Animation", "Layout", "Columns Style", "Ease Speed",
                                "UI Scale", "Rounding",
                                "Row Height", "Opacity", "Blur Strength", "Backdrop Dim",
                                "Menu Buttons", "Block Keys",
                                "Hotbar Animation", "Hotbar Speed" } },
        { "KEYS",       1, 2, {} },
    };

    const auto findSetting = [&](const char* name) -> Setting* {
        for (Setting* s : owner->mSettings)
            if (s && s->mName == name) return s;
        return nullptr;
    };

    // Two columns: the panel is wide and a single column of ~27 rows would need
    // scrolling for settings that all fit side by side.
    const float colGap = 14.0f * c.scale;
    const float colW = (area.z - area.x - colGap) * 0.5f;
    float colY[2] = { area.y, area.y };

    for (const Group& g : groups) {
        // Measure the group first so its backing panel can be drawn before the
        // rows that sit on it.
        float need = 0.0f;
        std::vector<Setting*> rows;
        for (const char* name : g.names) {
            Setting* s = findSetting(name);
            if (!s || !s->mVisible) continue;
            rows.push_back(s);
            need += settingHeight(c, s);
        }
        const bool withKeys = (g.extras > 0);
        if (withKeys) need += g.extras * c.rowH;
        if (rows.empty() && !withKeys) continue;

        const int col = std::clamp(g.col, 0, 1);
        const float x = area.x + col * (colW + colGap);
        float y = colY[col];

        text(c.dl, c.font, 11.5f * c.scale, ImVec2(x + 4.0f * c.scale, y), std::string(g.title),
             kTextLo, c.a, false);
        y += 20.0f * c.scale;

        const ImVec4 panel(x, y - 5.0f * c.scale, x + colW, y + need + 5.0f * c.scale);
        if (c.shadow && c.shadowK > 0.001f) {
            ImRenderUtils::fillShadowRectangle(panel, ImColor(0.0f, 0.0f, 0.0f, 1.0f),
                                               0.5f * c.shadowK * c.a, c.round * 0.7f, 16.0f * c.scale,
                                               0.85f, c.dl);
        }

        fill(c.dl, panel, ImVec4(1, 1, 1, 1), 0.035f * c.surface * c.a, c.round * 0.7f);
        stroke(c.dl, panel, ImVec4(1, 1, 1, 1), 0.05f * c.a, c.round * 0.7f, 1.0f);

        for (Setting* s : rows) {
            const float sh = settingHeight(c, s);
            const ImVec4 row(x + 6.0f * c.scale, y, x + colW - 6.0f * c.scale, y + sh);
            if (row.w > area.y - 4.0f && row.y < area.w + 4.0f) {
                captureTip(c, ImVec4(row.x, row.y, row.z, row.y + c.rowH), s->mDescription);
                drawSetting(c, s, row);
            }
            y += sh;
        }
        if (withKeys) {
            const ImVec4 bindRow(x + 6.0f * c.scale, y, x + colW - 6.0f * c.scale, y + c.rowH);
            if (bindRow.w > area.y - 4.0f && bindRow.y < area.w + 4.0f)
                drawKeybindRow(c, owner, bindRow, "Open menu");
            y += c.rowH;

            const ImVec4 ejectRow(x + 6.0f * c.scale, y, x + colW - 6.0f * c.scale, y + c.rowH);
            if (ejectRow.w > area.y - 4.0f && ejectRow.y < area.w + 4.0f) {
                drawKeyRow(c, ejectRow, "Eject", &Config::UninjectKey, "Ctrl + ",
                           "Hold Ctrl and this key to unload the client");
            }
            y += c.rowH;
        }
        colY[col] = y + 22.0f * c.scale;
    }

    return std::max(colY[0], colY[1]) - area.y;
}

// Theme selection keeps the existing enum indices used by ColorUtils and configs.
namespace {

struct ThemeCard { const char* name; ImVec4 c0, c1, c2; };

const ThemeCard kThemeCards[] = {
    { "Aurora",   ImVec4(58/255.f, 226/255.f, 199/255.f, 1), ImVec4(72/255.f, 149/255.f, 239/255.f, 1), ImVec4(129/255.f, 96/255.f, 240/255.f, 1) },
    { "Ember",    ImVec4(255/255.f, 196/255.f, 84/255.f, 1), ImVec4(255/255.f, 122/255.f, 48/255.f, 1), ImVec4(232/255.f, 58/255.f, 78/255.f, 1) },
    { "Frost",    ImVec4(140/255.f, 214/255.f, 255/255.f, 1), ImVec4(226/255.f, 244/255.f, 255/255.f, 1), ImVec4(126/255.f, 162/255.f, 208/255.f, 1) },
    { "Neon",     ImVec4(255/255.f, 46/255.f, 166/255.f, 1), ImVec4(168/255.f, 62/255.f, 255/255.f, 1), ImVec4(46/255.f, 226/255.f, 255/255.f, 1) },
    { "Mint",     ImVec4(120/255.f, 245/255.f, 168/255.f, 1), ImVec4(64/255.f, 224/255.f, 208/255.f, 1), ImVec4(150/255.f, 250/255.f, 220/255.f, 1) },
    { "Void",     ImVec4(94/255.f, 84/255.f, 214/255.f, 1), ImVec4(158/255.f, 88/255.f, 214/255.f, 1), ImVec4(196/255.f, 108/255.f, 196/255.f, 1) },
    { "Sakura",   ImVec4(255/255.f, 168/255.f, 200/255.f, 1), ImVec4(255/255.f, 214/255.f, 226/255.f, 1), ImVec4(250/255.f, 226/255.f, 170/255.f, 1) },
    { "Spectrum", ImVec4(255/255.f, 80/255.f, 80/255.f, 1), ImVec4(80/255.f, 255/255.f, 180/255.f, 1), ImVec4(90/255.f, 110/255.f, 255/255.f, 1) },
    { "Custom",   ImVec4(1.0f, 1.0f, 1.0f, 1), ImVec4(0.6f, 0.6f, 0.6f, 1), ImVec4(0.3f, 0.3f, 0.3f, 1) },
    { "Fire",     ImVec4(255/255.f, 102/255.f, 0/255.f, 1), ImVec4(255/255.f, 196/255.f, 0/255.f, 1), ImVec4(232/255.f, 58/255.f, 44/255.f, 1) },
    { "Water",    ImVec4(60/255.f, 150/255.f, 255/255.f, 1), ImVec4(80/255.f, 220/255.f, 255/255.f, 1), ImVec4(52/255.f, 90/255.f, 200/255.f, 1) },
    { "Smoke",    ImVec4(120/255.f, 124/255.f, 140/255.f, 1), ImVec4(170/255.f, 174/255.f, 190/255.f, 1), ImVec4(90/255.f, 92/255.f, 105/255.f, 1) },
    { "Sunset",   ImVec4(255/255.f, 120/255.f, 80/255.f, 1), ImVec4(255/255.f, 80/255.f, 160/255.f, 1), ImVec4(120/255.f, 80/255.f, 255/255.f, 1) },
    { "Cotton Candy", ImVec4(255/255.f, 150/255.f, 220/255.f, 1), ImVec4(150/255.f, 200/255.f, 255/255.f, 1), ImVec4(200/255.f, 120/255.f, 255/255.f, 1) },
    { "Forest",   ImVec4(60/255.f, 180/255.f, 90/255.f, 1), ImVec4(120/255.f, 200/255.f, 80/255.f, 1), ImVec4(40/255.f, 120/255.f, 70/255.f, 1) },
    { "Ocean",    ImVec4(0/255.f, 180/255.f, 220/255.f, 1), ImVec4(0/255.f, 120/255.f, 200/255.f, 1), ImVec4(60/255.f, 220/255.f, 240/255.f, 1) },
    { "Purple Haze", ImVec4(150/255.f, 80/255.f, 255/255.f, 1), ImVec4(100/255.f, 60/255.f, 200/255.f, 1), ImVec4(200/255.f, 140/255.f, 255/255.f, 1) },
    { "Lemonade", ImVec4(255/255.f, 236/255.f, 128/255.f, 1), ImVec4(198/255.f, 233/255.f, 92/255.f, 1), ImVec4(124/255.f, 199/255.f, 120/255.f, 1) },
    { "Copper",   ImVec4(217/255.f, 140/255.f, 79/255.f, 1), ImVec4(241/255.f, 193/255.f, 124/255.f, 1), ImVec4(172/255.f, 86/255.f, 62/255.f, 1) },
    { "Abyss",    ImVec4(38/255.f, 64/255.f, 124/255.f, 1), ImVec4(26/255.f, 101/255.f, 133/255.f, 1), ImVec4(19/255.f, 42/255.f, 92/255.f, 1) },
    { "Orchid",   ImVec4(191/255.f, 132/255.f, 222/255.f, 1), ImVec4(224/255.f, 124/255.f, 192/255.f, 1), ImVec4(242/255.f, 192/255.f, 222/255.f, 1) },
    { "Safari",   ImVec4(224/255.f, 186/255.f, 138/255.f, 1), ImVec4(156/255.f, 172/255.f, 122/255.f, 1), ImVec4(198/255.f, 150/255.f, 98/255.f, 1) },
    { "Cobalt",   ImVec4(40/255.f, 90/255.f, 220/255.f, 1), ImVec4(70/255.f, 140/255.f, 255/255.f, 1), ImVec4(140/255.f, 170/255.f, 255/255.f, 1) },
};
constexpr int kThemeCardCount = (int)(sizeof(kThemeCards) / sizeof(kThemeCards[0]));

constexpr int kThemeDisplayOrder[] = {
    0, 1, 2, 3, 4, 5, 6, 7,             // Aurora..Spectrum
    9, 10, 11, 12, 13, 14, 15, 16,       // Fire..Purple Haze
    17, 18, 19, 20, 21, 22,              // Lemonade, Copper, Abyss, Orchid, Safari, Cobalt
    8,                                   // Custom — at the end
};
constexpr int kThemeDisplayCount = (int)(sizeof(kThemeDisplayOrder) / sizeof(kThemeDisplayOrder[0]));
static_assert(kThemeDisplayCount == kThemeCardCount);

ImColor samplePalette(const ImColor* colors, int count, float phase) {
    if (!colors || count <= 0) return ImColor(1.0f, 1.0f, 1.0f, 1.0f);
    if (count == 1) return colors[0];
    phase -= std::floor(phase);
    const float p = phase * static_cast<float>(count);
    const int i0 = static_cast<int>(p) % count;
    const int i1 = (i0 + 1) % count;
    const float t = p - std::floor(p);
    const float smooth = t * t * (3.0f - 2.0f * t);
    ImColor result = mix(colors[i0], colors[i1], smooth);
    result.Value.w = 1.0f;
    return result;
}

void drawThemeGradient(const Ctx& c, const ImVec4& r, float round,
                       const ImColor* colors, int colorCount, float phase) {
    const int v0 = c.dl->VtxBuffer.Size;
    c.dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.z, r.w), IM_COL32_WHITE, round);
    const int v1 = c.dl->VtxBuffer.Size;
    const float width = std::max(1.0f, r.z - r.x);
    for (int i = v0; i < v1; ++i) {
        ImDrawVert& v = c.dl->VtxBuffer[i];
        const float u = std::clamp((v.pos.x - r.x) / width, 0.0f, 1.0f);
        const float fringe = static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFFu) / 255.0f;
        const ImColor color = samplePalette(colors, colorCount, u + phase);
        v.col = ImGui::GetColorU32(ImVec4(color.Value.x, color.Value.y, color.Value.z,
                                          c.a * fringe));
    }
}

} // namespace

float drawThemesBody(const Ctx& c, const ImVec4& area) {
    // The Theme setting lives on this module (ClickGUI) — resolve by name.
    EnumSetting* themeSet = nullptr;
    Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI");
    if (gui) {
        for (Setting* s : gui->mSettings)
            if (s && s->mName == "Theme") { themeSet = static_cast<EnumSetting*>(s); break; }
    }
    if (!themeSet || !gui) return 0.0f;

    const float gap = 12.0f * c.scale;
    const int cols = 3;
    const float cardW = (area.z - area.x - gap * (cols - 1)) / cols;
    const float cardH = 96.0f * c.scale;
    const float round = std::min(18.0f * c.scale, cardH * 0.42f);
    float y = area.y;

    std::vector<ImColor> customPalette(6, ImColor(1.0f, 1.0f, 1.0f, 1.0f));
    int customCount = 3;
    float paletteSpeed = 2.0f;
    float spectrumSaturation = 1.0f;
    for (Setting* s : gui->mSettings) {
        if (!s) continue;
        if (s->mName == "Colors") {
            customCount = std::clamp(static_cast<int>(static_cast<NumberSetting*>(s)->mValue), 1, 6);
        } else if (s->mName == "Color Speed") {
            paletteSpeed = std::max(0.1f, static_cast<NumberSetting*>(s)->mValue);
        } else if (s->mName == "Saturation") {
            spectrumSaturation = std::clamp(static_cast<NumberSetting*>(s)->mValue, 0.0f, 1.0f);
        } else if (s->mName.rfind("Color ", 0) == 0 && s->mName.size() == 7 &&
                   s->mType == SettingType::Color) {
            const int index = s->mName[6] - '1';
            if (index >= 0 && index < 6) {
                const auto* color = static_cast<const ColorSetting*>(s);
                customPalette[index] = ImColor(color->mValue[0], color->mValue[1], color->mValue[2], 1.0f);
            }
        }
    }
    customPalette.resize(static_cast<size_t>(customCount));
    const float palettePhase = static_cast<float>(ColorUtils::GetTimeMs() % 1000000) *
                               0.0001f * paletteSpeed;

    // Centered header.
    {
        const std::string hint = "Tap a theme to apply it";
        const float hw = measure(c.font, 12.5f * c.scale, hint);
        text(c.dl, c.font, 12.5f * c.scale,
             ImVec2(area.x + (area.z - area.x - hw) * 0.5f, y),
             hint, kTextLo, 0.85f * c.a, false);
    }
    y += 26.0f * c.scale;

    for (int di = 0; di < kThemeDisplayCount; ++di) {
        const int i = kThemeDisplayOrder[di];
        const int col = di % cols;
        const int row = di / cols;
        const ThemeCard& t = kThemeCards[i];
        const float x = area.x + col * (cardW + gap);
        const float cy = y + row * (cardH + gap);
        const ImVec4 card(x, cy, x + cardW, cy + cardH);

        if (card.w > area.y - 4.0f && card.y < area.w + 4.0f) {
            const bool active = themeSet->mValue == i;
            const bool hov = c.live && over(card);
            const float h = hoverOf(std::string("theme") + std::to_string(i), hov, c.dt);

            const ImColor stops[] = {
                ImColor(t.c0.x, t.c0.y, t.c0.z, 1.0f),
                ImColor(t.c1.x, t.c1.y, t.c1.z, 1.0f),
                ImColor(t.c2.x, t.c2.y, t.c2.z, 1.0f)
            };
            const ImColor* palette = stops;
            int paletteCount = static_cast<int>(IM_ARRAYSIZE(stops));
            if (i == 7) {
                static ImColor spectrum[3];
                const float hue = std::fmod(palettePhase, 1.0f);
                for (int stop = 0; stop < 3; ++stop)
                    spectrum[stop] = ImColor::HSV(std::fmod(hue + stop / 3.0f, 1.0f),
                                                 spectrumSaturation, 1.0f);
                palette = spectrum;
            } else if (i == 8) {
                palette = customPalette.data();
                paletteCount = static_cast<int>(customPalette.size());
            }
            const float phase = palettePhase + static_cast<float>(di) * 0.071f;
            drawThemeGradient(c, card, round, palette, paletteCount, phase);
            const ImColor accent = samplePalette(palette, paletteCount, phase + 0.5f);
            ImRenderUtils::fillShadowRectangle(card, accent,
                (active ? 0.55f : 0.22f + 0.25f * h) * c.a, round,
                26.0f * c.scale, 0.85f, c.dl);
            shadeDiag(c.dl, card,
                      ImVec4(1.0f, 1.0f, 1.0f, 0.18f * c.a),
                      ImVec4(1.0f, 1.0f, 1.0f, 0.0f), round);
            if (h > 0.004f)
                fill(c.dl, card, ImVec4(1, 1, 1, 1), 0.065f * h * c.a, round);
            stroke(c.dl, card, ImVec4(1, 1, 1, 1),
                   (active ? 0.75f : 0.16f + 0.18f * h) * c.a, round,
                   active ? 1.6f * c.scale : 1.0f);

            const float fs = 16.0f * c.scale;
            text(c.dl, c.bold, fs, ImVec2(card.x + 16.0f * c.scale,
                                          card.y + (card.w - card.y - fs) * 0.5f),
                 t.name, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), c.a, true);

            const float selectorR = 9.0f * c.scale;
            const ImVec2 selector(card.z - 24.0f * c.scale, (card.y + card.w) * 0.5f);
            c.dl->AddCircle(selector, selectorR,
                            ImGui::GetColorU32(ImVec4(1, 1, 1, (active ? 0.95f : 0.55f) * c.a)),
                            24, (active ? 1.8f : 1.3f) * c.scale);
            if (active) {
                c.dl->AddCircleFilled(selector, selectorR - 2.0f * c.scale,
                                      ImGui::GetColorU32(ImVec4(1, 1, 1, 0.94f * c.a)), 24);
                c.dl->AddLine(ImVec2(selector.x - 4.0f * c.scale, selector.y),
                              ImVec2(selector.x - 1.0f * c.scale, selector.y + 3.0f * c.scale),
                              ImGui::GetColorU32(ImVec4(0.10f, 0.12f, 0.16f, c.a)), 1.6f * c.scale);
                c.dl->AddLine(ImVec2(selector.x - 1.0f * c.scale, selector.y + 3.0f * c.scale),
                              ImVec2(selector.x + 4.5f * c.scale, selector.y - 3.5f * c.scale),
                              ImGui::GetColorU32(ImVec4(0.10f, 0.12f, 0.16f, c.a)), 1.6f * c.scale);
            }

            if (hov && c.lmb && !active) {
                themeSet->mValue = i;
                gui->UpdateSettingsVisibility();
            }
        }
    }

    const int rows = (kThemeCardCount + cols - 1) / cols;
    y += rows * (cardH + gap) + 10.0f * c.scale;

    {
        Setting* strength = nullptr;
        for (Setting* s : gui->mSettings)
            if (s && s->mName == "Theme Strength") { strength = s; break; }
        if (strength) {
            const float sh = settingHeight(c, strength);
            const ImVec4 row(area.x, y, area.z, y + sh);
            if (row.w > area.y - 4.0f && row.y < area.w + 4.0f) {
                captureTip(c, row, strength->mDescription);
                drawSetting(c, strength, row);
            }
            y += sh + 12.0f * c.scale;
        }
    }

    {
        EnumSetting* fontSet = nullptr;
        for (Setting* s : gui->mSettings)
            if (s && s->mName == "Font") { fontSet = static_cast<EnumSetting*>(s); break; }
        if (fontSet && !fontSet->mValues.empty()) {
            auto& rm = RenderManager::GetInstance();
            auto fontFor = [&rm](int idx) -> ImFont* {
                ImFont* f = rm.pickFont(idx, false);
                return f ? f : ImGui::GetFont();
            };

            text(c.dl, c.font, 11.5f * c.scale, ImVec2(area.x + 4.0f * c.scale, y),
                 std::string("FONT"), kTextLo, c.a, false);
            y += 22.0f * c.scale;

            const float fGap = 8.0f * c.scale;
            const float fH = 38.0f * c.scale;
            const int fCols = 3;
            const float fW = (area.z - area.x - fGap * (fCols - 1)) / fCols;
            const int nF = (int)fontSet->mValues.size();
            for (int i = 0; i < nF; ++i) {
                const int col = i % fCols;
                const int rowI = i / fCols;
                const float x = area.x + col * (fW + fGap);
                const float cy = y + rowI * (fH + fGap);
                const ImVec4 card(x, cy, x + fW, cy + fH);
                if (card.w <= area.y - 4.0f || card.y >= area.w + 4.0f) continue;

                const bool active = fontSet->mValue == i;
                const bool hov = c.live && over(card);
                const float h = hoverOf(std::string("fontpick") + std::to_string(i), hov, c.dt);

                fill(c.dl, card, ImColor(active ? 15 : 18, active ? 19 : 21, active ? 26 : 30, 255),
                     0.92f * c.a, round);
                if (h > 0.004f)
                    fill(c.dl, card, ImVec4(1, 1, 1, 1), 0.045f * h * c.a, round);
                stroke(c.dl, card, ImVec4(1, 1, 1, 1),
                       (0.05f + 0.16f * h) * c.a, round, 1.0f);

                // Preview each option in its own font.
                ImFont* own = fontFor(i);
                const std::string name = fontSet->mValues[i];
                const float fs = 15.0f * c.scale;
                const float nw = measure(own, fs, name);
                const ImVec4 labelCol = active
                    ? ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f)
                    : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                text(c.dl, own, fs,
                     ImVec2(card.x + (card.z - card.x - nw) * 0.5f,
                            card.y + (card.w - card.y - fs) * 0.5f),
                     name, labelCol,
                     (active ? 1.0f : 0.85f) * c.a, c.textShadow);

                if (hov && c.lmb && !active) {
                    fontSet->mValue = i;
                }
            }
            const int fRows = (nF + fCols - 1) / fCols;
            y += fRows * (fH + fGap) + 4.0f * c.scale;
        }
    }

    {
        BoolSetting* boldSet = nullptr;
        for (Setting* s : gui->mSettings)
            if (s && s->mName == "Bold Font" && s->mType == SettingType::Bool) {
                boldSet = static_cast<BoolSetting*>(s);
                break;
            }
        if (boldSet) {
            const float bH = 34.0f * c.scale;
            const ImVec4 row(area.x, y + 8.0f * c.scale, area.z, y + 8.0f * c.scale + bH);
            if (row.w > area.y - 4.0f && row.y < area.w + 4.0f) {
                const bool on = boldSet->mValue;
                const bool hov = c.live && over(row);
                const float h = hoverOf("boldfont", hov, c.dt);
                // This control is outside the shared settings animation pass.
                static float s_boldAnim = 0.0f;
                s_boldAnim += ((on ? 1.0f : 0.0f) - s_boldAnim) *
                              std::clamp(c.dt * 16.0f, 0.0f, 1.0f);

                fill(c.dl, row, ImColor(18, 21, 30, 255), 0.92f * c.a, round);
                if (h > 0.004f)
                    fill(c.dl, row, ImVec4(1, 1, 1, 1), 0.045f * h * c.a, round);
                stroke(c.dl, row, ImVec4(1, 1, 1, 1),
                       (on ? 0.16f : 0.05f + 0.16f * h) * c.a, round, 1.0f);
                const float bfs = 14.0f * c.scale;
                text(c.dl, c.font, bfs,
                     ImVec2(row.x + 12.0f * c.scale, (row.y + row.w - bfs) * 0.5f),
                     std::string("Bold Font"), on ? kTextHi : kTextMid, c.a, c.textShadow);
                const float tw2 = 34.0f * c.scale, th2 = 18.0f * c.scale;
                const ImVec4 sw(row.z - 12.0f * c.scale - tw2, (row.y + row.w - th2) * 0.5f,
                                row.z - 12.0f * c.scale, (row.y + row.w + th2) * 0.5f);
                drawToggle(c, sw, easeOutCubic(std::clamp(s_boldAnim, 0.0f, 1.0f)), h);
                if (hov && c.lmb) boldSet->mValue = !boldSet->mValue;
            }
            y = row.w + 4.0f * c.scale;
        }
    }

    if (themeSet->mValue == 8) {
        text(c.dl, c.font, 11.5f * c.scale, ImVec2(area.x + 4.0f * c.scale, y),
             std::string("CUSTOM THEME"), kTextLo, c.a, false);
        y += 22.0f * c.scale;

        // Resolve the palette settings (they live on this module).
        NumberSetting* countSet = nullptr;
        ColorSetting* colors[6] = {};
        for (Setting* s : gui->mSettings) {
            if (!s) continue;
            if (s->mName == "Colors") countSet = static_cast<NumberSetting*>(s);
            else if (s->mName.rfind("Color ", 0) == 0 && s->mName.size() == 7) {
                const int idx = s->mName[6] - '1';
                if (idx >= 0 && idx < 6 && s->mType == SettingType::Color)
                    colors[idx] = static_cast<ColorSetting*>(s);
            }
        }
        if (countSet) {
            gui->UpdateSettingsVisibility();
            const int count = std::clamp(static_cast<int>(countSet->mValue), 1, 6);
            static int s_selSwatch = 0;
            if (s_selSwatch >= count) s_selSwatch = count - 1;

            const float swGap = 10.0f * c.scale;
            const int swCols = 6;
            const float swW = (area.z - area.x - swGap * (swCols - 1)) / swCols;
            const float swH = 44.0f * c.scale;
            const int slots = count + (count < 6 ? 1 : 0);
            for (int i = 0; i < slots; ++i) {
                const float x = area.x + i * (swW + swGap);
                const ImVec4 card(x, y, x + swW, y + swH);
                if (card.w > area.y - 4.0f && card.y < area.w + 4.0f) {
                    const bool adder = (i == count);
                    const bool sel = !adder && (i == s_selSwatch);
                    const bool hov = c.live && over(card);
                    const float h = hoverOf(std::string("cswatch") + std::to_string(i), hov, c.dt);

                    if (adder) {
                        // Ghost slot: empty plate with a plus.
                        fill(c.dl, card, ImColor(18, 21, 30, 255), 0.92f * c.a, round);
                        if (h > 0.004f)
                            fill(c.dl, card, ImVec4(1, 1, 1, 1), 0.045f * h * c.a, round);
                        stroke(c.dl, card, ImVec4(1, 1, 1, 1),
                               (0.07f + 0.14f * h) * c.a, round, 1.0f);
                        const std::string plus = "+";
                        const float fs = 20.0f * c.scale;
                        const float pw = measure(c.font, fs, plus);
                        text(c.dl, c.font, fs,
                             ImVec2(card.x + (card.z - card.x - pw) * 0.5f,
                                    card.y + (card.w - card.y - fs) * 0.5f),
                             plus, kTextLo, (0.5f + 0.4f * h) * c.a, false);
                    } else {
                        // Filled swatch in its own color; the selected slot
                        // carries an accent ring, hover lightens the plate.
                        fill(c.dl, card, ImVec4(colors[i] ? colors[i]->mValue[0] : 1.0f,
                                                colors[i] ? colors[i]->mValue[1] : 1.0f,
                                                colors[i] ? colors[i]->mValue[2] : 1.0f, 1.0f),
                             c.a, round);
                        if (h > 0.004f)
                            fill(c.dl, card, ImVec4(1, 1, 1, 1), 0.10f * h * c.a, round);
                        stroke(c.dl, card,
                               sel ? ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f)
                                   : ImVec4(1, 1, 1, 1),
                               (sel ? 0.85f : 0.10f + 0.14f * h) * c.a, round,
                               sel ? 1.8f : 1.0f);
                        const std::string num = std::to_string(i + 1);
                        const float fs = 11.5f * c.scale;
                        const float nw = measure(c.font, fs, num);
                        text(c.dl, c.font, fs,
                             ImVec2(card.x + (card.z - card.x - nw) * 0.5f,
                                    card.w - fs - 5.0f * c.scale),
                             num, ImVec4(1, 1, 1, 1), 0.75f * c.a, c.textShadow);
                    }

                    if (c.live && hov && (c.lmb || c.rmb)) {
                        if (adder && c.lmb && count < 6) {
                            countSet->mValue = (float)(count + 1);
                            gui->UpdateSettingsVisibility();
                        } else if (!adder && c.lmb) {
                            s_selSwatch = i;
                        } else if (!adder && c.rmb && count > 1) {
                            countSet->mValue = (float)(count - 1);
                            if (s_selSwatch >= count - 1) s_selSwatch = count - 2;
                            gui->UpdateSettingsVisibility();
                        }
                    }
                }
            }
            y += swH + 12.0f * c.scale;

            // Inline HSV editor for the selected slot (ImRenderUtils picker:
            // SV square + hue bar, mouse handled inside).
            if (colors[s_selSwatch]) {
                const float pad = 12.0f * c.scale;
                const float svW = (area.z - area.x) - pad * 2.0f;
                const float svH = 96.0f * c.scale;
                const float hueH = 10.0f * c.scale;
                const float headH = 22.0f * c.scale;
                const float panelH = headH + svH + 6.0f * c.scale + hueH + pad * 2.0f;
                const ImVec4 panel(area.x, y, area.z, y + panelH);
                if (panel.w > area.y - 4.0f && panel.y < area.w + 4.0f) {
                    fill(c.dl, panel, ImColor(15, 19, 26, 255), 0.92f * c.a, round);
                    stroke(c.dl, panel, ImVec4(1, 1, 1, 1), 0.05f * c.a, round, 1.0f);

                    // Header: slot label + live hex readout.
                    const ColorSetting* cs = colors[s_selSwatch];
                    char hex[16];
                    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                                  (int)(cs->mValue[0] * 255.0f + 0.5f) & 0xFF,
                                  (int)(cs->mValue[1] * 255.0f + 0.5f) & 0xFF,
                                  (int)(cs->mValue[2] * 255.0f + 0.5f) & 0xFF);
                    const std::string label = "COLOR " + std::to_string(s_selSwatch + 1);
                    text(c.dl, c.font, 12.5f * c.scale,
                         ImVec2(panel.x + pad, panel.y + (headH - 12.5f * c.scale) * 0.5f),
                         label, kTextLo, c.a, false);
                    const float hw = measure(c.font, 12.5f * c.scale, hex);
                    text(c.dl, c.font, 12.5f * c.scale,
                         ImVec2(panel.z - pad - hw, panel.y + (headH - 12.5f * c.scale) * 0.5f),
                         hex, kTextLo, c.a, false);

                    drawGuiColorPicker(c, colors[s_selSwatch]->mValue,
                        ImVec4(panel.x + pad, panel.y + headH + pad, svW, svH));
                }
                y += panelH + 6.0f * c.scale;
            }
        }
    }

    {
        Setting* gradSet = nullptr;
        Setting* speedSet = nullptr;
        for (Setting* s : gui->mSettings) {
            if (!s) continue;
            if      (s->mName == "Gradient")    gradSet  = s;
            else if (s->mName == "Color Speed") speedSet = s;
        }
        if (gradSet || speedSet) {
            text(c.dl, c.font, 11.5f * c.scale, ImVec2(area.x + 4.0f * c.scale, y),
                 std::string("GRADIENT"), kTextLo, c.a, false);
            y += 22.0f * c.scale;

            gui->UpdateSettingsVisibility();
            float ry = y;
            for (Setting* s : { gradSet, speedSet }) {
                if (!s || !s->mVisible) continue;
                const float sh = settingHeight(c, s);
                const ImVec4 rowR(area.x, ry, area.z, ry + sh);
                if (rowR.w > area.y - 4.0f && rowR.y < area.w + 4.0f) {
                    captureTip(c, rowR, s->mDescription);
                    drawSetting(c, s, rowR);
                }
                ry += sh;
            }
            y = ry;
        }
    }

    return y - area.y + 8.0f * c.scale;
}

} // namespace

// ---------------------------------------------------------------------------
// Module surface.
// ---------------------------------------------------------------------------

ClickGUI::ClickGUI()
    : Module("ClickGUI", "Displays the clickable graphical interface", Category::Visual, VK_INSERT) {
    mTheme    = add<EnumSetting>("Theme", "Visual theme of the client", 0,
                                std::vector<std::string>{ "Aurora", "Ember", "Frost", "Neon",
                                                          "Mint", "Void", "Sakura", "Spectrum",
                                                          "Custom",
                                                          "Fire", "Water", "Smoke", "Sunset",
                                                          "Cotton Candy", "Forest", "Ocean",
                                                          "Purple Haze",
                                                          "Lemonade", "Copper", "Abyss",
                                                          "Orchid", "Safari", "Cobalt" });
    mFont     = add<EnumSetting>("Font", "Font of the client", 0,
                                std::vector<std::string>{ "Product Sans",
                                                          "Minecraft", "Comfortaa",
                                                          "Manrope", "Geist" });
    mBoldFont = add<BoolSetting>("Bold Font", "Use the bold weight of the selected font", false);
    mGradient = add<EnumSetting>("Gradient", "Gradient style for themed colors", 0,
                                std::vector<std::string>{ "Smooth", "Wave" });
    mColorCount = add<NumberSetting>("Colors", "Amount of custom colors", 3.0f, 1.0f, 6.0f, 1.0f);
    mColors[0] = add<ColorSetting>("Color 1", "First custom color",  1.0f, 0.0f, 0.0f, 1.0f);
    mColors[1] = add<ColorSetting>("Color 2", "Second custom color", 1.0f, 0.5f, 0.0f, 1.0f);
    mColors[2] = add<ColorSetting>("Color 3", "Third custom color",  1.0f, 0.8f, 0.0f, 1.0f);
    mColors[3] = add<ColorSetting>("Color 4", "Fourth custom color", 0.0f, 1.0f, 0.0f, 1.0f);
    mColors[4] = add<ColorSetting>("Color 5", "Fifth custom color",  0.0f, 0.0f, 1.0f, 1.0f);
    mColors[5] = add<ColorSetting>("Color 6", "Sixth custom color",  0.5f, 0.0f, 1.0f, 1.0f);
    mColorSpeed = add<NumberSetting>("Color Speed", "Speed of color transitions", 2.0f, 0.1f, 10.0f, 0.1f);
    mSaturation = add<NumberSetting>("Saturation", "Color saturation", 1.0f, 0.0f, 1.0f, 0.05f);
    mThemeStrength = add<NumberSetting>("Theme Strength", "Theme color intensity", 0.55f, 0.0f, 1.0f, 0.05f);
    // §z paints with the theme gradient and §y with a fixed theme stop; §x reads
    // this, so it stays usable whatever the theme is set to.
    mCodeColor = add<ColorSetting>("Code Color", "Color used by the \xC2\xA7x text code",
                                  0.45f, 0.75f, 1.0f, 1.0f);

    // --- panel ----------------------------------------------------------------
    mAnim       = add<EnumSetting>("Animation", "Panel entrance style", 1,
                                  std::vector<std::string>{ "Cascade", "Bloom" });
    mEaseSpeed  = add<NumberSetting>("Ease Speed", "Speed of the open/close animation", 12.0f, 3.0f, 30.0f, 0.5f);
    mBlur       = add<NumberSetting>("Blur Strength", "Backdrop blur behind the panel", 8.0f, 0.0f, 20.0f, 0.5f);
    mDim        = add<NumberSetting>("Backdrop Dim", "How much the world darkens", 0.18f, 0.0f, 1.0f, 0.05f);
    mOpacity    = add<NumberSetting>("Opacity", "Panel surface opacity", 1.0f, 0.35f, 1.0f, 0.01f);
    mRounding   = add<NumberSetting>("Rounding", "Panel corner radius", 10.0f, 0.0f, 26.0f, 1.0f);
    mRowHeight  = add<NumberSetting>("Row Height", "Height of setting rows", 32.0f, 26.0f, 44.0f, 1.0f);
    mUiScale    = add<NumberSetting>("UI Scale", "Overall size of the panel", 1.0f, 0.6f, 1.6f, 0.05f);
    mGlow       = add<NumberSetting>("Glow", "Accent glow intensity", 0.85f, 0.0f, 1.5f, 0.05f);
    mShadow     = add<BoolSetting>("Shadow", "Drop shadow under the panel and cards", true);
    mShadowStrength = add<NumberSetting>("Shadow Strength", "How strong the drop shadows are", 0.7f, 0.0f, 2.0f, 0.05f);
    mTextShadow = add<BoolSetting>("Text Shadow", "Draw text with a drop shadow", true);
    mCasing     = add<EnumSetting>("Name Casing", "Casing of module names", 0,
                                  std::vector<std::string>{ "Default", "lowercase" });
    mSort       = add<EnumSetting>("Sort Mode", "Order modules are listed in", 0,
                                  std::vector<std::string>{ "Alphabetical", "Reverse", "Enabled first" });
    if (mCasing->mValue >= (int)mCasing->mValues.size()) mCasing->mValue = 0;
    if (mSort->mValue >= (int)mSort->mValues.size()) mSort->mValue = 0;
    mTooltips   = add<BoolSetting>("Tooltips", "Show setting descriptions on hover", true);
    mMenuButtons = add<BoolSetting>("Menu Buttons", "Show the Settings / Configs / Change Logs buttons on the main and pause screens", true);
    mBlockKeys   = add<BoolSetting>("Block Keys", "Stop keyboard input from reaching Minecraft while the menu is open", false);
    mHotbarAnim  = add<EnumSetting>("Hotbar Animation", "Easing of the hotbar selection frame glide (Off = vanilla instant)", 1,
                                    std::vector<std::string>{ "Linear", "Smooth", "Snap", "Off" });
    mHotbarSpeed = add<NumberSetting>("Hotbar Speed", "How fast the hotbar selection frame travels", 16.0f, 2.0f, 60.0f, 0.5f);
    mLayout     = add<EnumSetting>("Layout", "Menu style: one glass Panel, or floating Columns", 0,
                                  std::vector<std::string>{ "Panel", "Columns" });
    mColStyle   = add<EnumSetting>("Columns Style", "Columns look: glazed 3D buttons, or a flat list fused with the card", 0,
                                  std::vector<std::string>{ "Machined", "Clean" });

    UpdateSettingsVisibility();
}

void ClickGUI::UpdateSettingsVisibility() {
    // Custom palette slots only exist in Custom theme, and only up to Colors.
    // Theme indices: 7 = Spectrum, 8 = Custom (see ColorUtils::getThemedColor).
    const bool custom = (mTheme->mValue == 8);
    // Configs saved before "Force" was removed can still carry index 3 — clamp
    // them back into the live option list so the dropdown never reads blank.
    if (mSort->mValue < 0 || mSort->mValue >= (int)mSort->mValues.size())
        mSort->mValue = 0;
    mColorCount->mVisible = custom;
    const int count = std::clamp(static_cast<int>(mColorCount->mValue), 1, 6);
    for (int i = 0; i < 6; ++i)
        if (mColors[i]) mColors[i]->mVisible = custom && (i < count);
    // Saturation only feeds the HSV sweep the Spectrum theme uses.
    mSaturation->mVisible = (mTheme->mValue == 7);
    // The Columns look switch is meaningless while the Panel layout is up.
    if (mColStyle) mColStyle->mVisible = (mLayout->mValue == 1);
    // Shadow strength only matters while the shadow itself is on.
    mShadowStrength->mVisible = mShadow->mValue;
}

void ClickGUI::OnTick() {
    // Keep visibility in sync even when nothing is drawing the Settings tab.
    UpdateSettingsVisibility();
}

// ---------------------------------------------------------------------------
// Columns input and layout:
//   * LMB toggles a module; RMB opens its inline settings.
//   * Header dragging respects the topmost column; RMB folds and persists it.
//   * Search hides non-matching rows without changing the stable tab order.
//   * layoutSettingList(..., draw=false) measures exactly what drawing lays out.
//   * Longest names come first, with alphabetical ties; Panel sorting is separate.
// ---------------------------------------------------------------------------
namespace {

constexpr float kColW        = 272.0f;   // category card width
constexpr int   kSettingsCol = -1;        // no settings column: tabs instead
constexpr int   kColCount    = kCategoryCount;

float g_colX[kColCount]      = { 40.f, 330.f, 620.f, 910.f, 1200.f };
float g_colY[kColCount]      = { 64.f, 64.f, 64.f, 64.f, 64.f };
bool  g_colPlaced[kColCount] = {};
float g_colBody[kColCount]   = {};   // eased tray height (grows, never overlaps)
bool  g_colFolded[kColCount] = {};   // RMB on a header folds the column into a cap
float g_colFoldAnim[kColCount] = {};
float g_colPanY  = 0.0f;             // eased pan offset added to every column's Y
float g_colPanYT = 0.0f;             // pan target
int   g_colDrag   = -1;
float g_colDragDX = 0.f, g_colDragDY = 0.f;

int   g_colTab = 0;
float g_colTabScroll = 0.0f, g_colTabScrollT = 0.0f;
float g_colTabFade = 1.0f;   // 0 right after a switch, eases to 1
// The centred settings/themes/configs card is draggable like the columns.
bool  g_tabCardPlaced = false;
float g_tabCardX = 0.0f, g_tabCardY = 0.0f;
bool  g_tabCardDrag = false;
float g_tabCardDX = 0.0f, g_tabCardDY = 0.0f;

Module* g_openMod = nullptr;
Module* g_closingMod = nullptr;

constexpr ImVec4 kSlab   { 0.055f, 0.059f, 0.075f, 1.0f };  // card body (glass)
constexpr ImVec4 kHead   { 0.133f, 0.141f, 0.165f, 1.0f };  // lighter header strip
constexpr ImVec4 kWell   { 0.045f, 0.049f, 0.062f, 1.0f };  // settings recess
constexpr ImVec4 kRow    { 0.090f, 0.096f, 0.118f, 1.0f };  // row hover base
constexpr ImVec4 kRowHi  { 0.150f, 0.160f, 0.192f, 1.0f };  // row hover lift
constexpr ImVec4 kRowLo  { 0.070f, 0.075f, 0.093f, 1.0f };  // row pressed
constexpr ImVec4 kTxt    { 0.840f, 0.852f, 0.880f, 1.0f };  // light gray text
constexpr ImVec4 kTxtDim { 0.530f, 0.560f, 0.620f, 1.0f };  // secondary text

// ImVec4 lerp, usable before the per-frame Ctx exists.
ImVec4 mixC0(const ImVec4& a, const ImVec4& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, 1.0f);
}

// A glass card: soft blur behind, near-black fill, soft shadow, strong
// rounding. NO border stroke — ever.
void drawGlassCard(ImDrawList* dl, const ImVec4& r, float a, float round,
                   bool shadowOn, float surface = 1.0f, float shadowK = 1.0f) {
    surface = std::clamp(surface, 0.0f, 1.0f);
    if (a <= 0.004f || r.z - r.x < 0.5f || r.w - r.y < 0.5f) return;
    if (shadowOn && shadowK > 0.001f)
        ImRenderUtils::fillShadowRectangle(r, ImColor(0.0f, 0.0f, 0.0f, 1.0f),
                                           0.58f * shadowK * a * surface, round, 26.0f, 0.85f, dl);
    const float blurK = std::clamp((a - 0.32f) / 0.68f, 0.0f, 1.0f);
    if (blurK > 0.0f)
        D2D::addBlurOptimized(dl, 2.5f * blurK * blurK, std::optional<ImVec4>(r), round);
    // The Opacity setting drives the card surface exactly like the Panel
    // shell (kPanel * surface) — Columns used to ignore it entirely.
    fill(dl, r, kSlab, 0.96f * a * surface, round);
}

void drawRowMinimal(const Ctx& c, const ImVec4& row, float lit, float hov, bool dent,
                    float sA, float round, bool isOpen, bool clean,
                    const ImVec4* full = nullptr, const ImVec4* gradSpace = nullptr,
                    float sheetRound = 0.0f, bool fullWash = false,
                    float joinExt = 0.0f, bool joinUp = false) {
    if (sA <= 0.004f || row.z - row.x < 0.5f || row.w - row.y < 0.5f) return;
    ImDrawList* dl = c.dl;

    if (clean) {
        const ImVec4& area = full ? *full : row;
        if (area.z - area.x < 0.5f) return;
        // The last row of a column carries the card's bottom rounding so the
        // sheet ends in the card's own corners instead of a dark strip.
        const ImDrawFlags sheetFlags = sheetRound > 0.0f ? ImDrawFlags_RoundCornersBottom : 0;
        if (lit > 0.004f) {
            const float wipe = easeOutCubic(std::clamp(lit, 0.0f, 1.0f));
            shadeThemedSheet(dl, area, gradSpace ? *gradSpace : area, wipe, sA,
                             c.gradSpan, c.gradMs, sheetRound, sheetFlags, fullWash,
                             c.gradLut, c.gradLutN);
        }
        const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f);
        if (dent) fill(dl, area, white, 0.07f * sA, sheetRound, sheetFlags);
        else if (hov > 0.004f) fill(dl, area, white, 0.05f * hov * sA, sheetRound, sheetFlags);
        else if (isOpen) fill(dl, area, white, 0.035f * sA, sheetRound, sheetFlags);
        return;
    }

    const ImVec4 face(row.x, row.y, row.z, row.w + joinExt);
    ImDrawFlags joinFlags = 0;   // flags == 0 is ImGui's "ALL corners rounded"
    if (joinExt > 0.0f || joinUp) {
        ImDrawFlags corners = ImDrawFlags_RoundCornersAll;
        if (joinExt > 0.0f) corners &= ~ImDrawFlags_RoundCornersBottom;
        if (joinUp)        corners &= ~ImDrawFlags_RoundCornersTop;
        // Both ends squared is NOT `flags == 0` (that means all rounded) —
        // RoundCornersNone is the explicit "round nothing" value.
        joinFlags = corners ? corners : ImDrawFlags_RoundCornersNone;
    }

    ImVec4 top = mixC0(kRow, kRowHi, 0.20f + 0.60f * hov);
    ImVec4 bot = mixC0(kRowLo, kRow, 0.20f + 0.45f * hov);
    if (dent) { top = kRowLo; bot = mixC0(kRowLo, kSlab, 0.45f); }
    const float faceA = 0.94f * sA * std::clamp(c.surface, 0.0f, 1.0f);
    gradientV(dl, face, top, bot, faceA, faceA, round, joinFlags);

    if (lit > 0.004f || isOpen) {
        const float k = std::max(lit, isOpen ? 0.55f : 0.0f);
        shadeThemedSheet(dl, face, gradSpace ? *gradSpace : row, 1.0f, k * sA,
                         c.gradSpan, c.gradMs, round, joinFlags, fullWash,
                         c.gradLut, c.gradLutN);
        // The glaze: a glossy white sheen travelling the same diagonal and
        // fading out toward the bottom-right — the glass on the button.
        shadeDiag(dl, face,
                  ImVec4(1.0f, 1.0f, 1.0f, 0.16f * k * sA),
                  ImVec4(1.0f, 1.0f, 1.0f, 0.0f),
                  round, joinFlags);
    }
}

void drawWell(ImDrawList* dl, const ImVec4& r, float a, float surface, float round,
              float wipe = 1.0f) {
    a *= std::clamp(surface, 0.0f, 1.0f);
    if (a <= 0.004f || r.z - r.x < 0.5f || r.w - r.y < 0.5f) return;
    if (wipe < 0.999f) {
        shadeDiagWipe(dl, r,
                      ImVec4(kWell.x, kWell.y, kWell.z, 0.85f * a),
                      ImVec4(kWell.x, kWell.y, kWell.z, 0.85f * a),
                      wipe, round);
        return;
    }
    fill(dl, r, kWell, 0.85f * a, round);
}

// One settings group of the single-column stack.
struct SettingGroup {
    const char*              title;      // nullptr = no header
    std::vector<const char*> names;      // pick by name; ignored when all=true
    bool                     all   = false;  // every visible setting of the owner
    bool                     bind  = false;  // the owner's keybind row
    bool                     eject = false;  // the uninject hotkey (ClickGUI only)
    const char*              bindLabel = "Keybind";
};

float layoutSettingList(const Ctx& c, Module* owner, const ImVec4& area,
                        const SettingGroup* groups, int nGroups, bool draw,
                        float appear = 1.0f) {
    const float inset   = 7.0f * c.scale;
    const float rowInset = c.columnsBody ? 2.0f * c.scale : inset;
    const float rowGap  = c.colSlider ? 2.0f * c.scale : 5.0f * c.scale;
    const float headGap = 22.0f * c.scale;
    const float grpGap  = 15.0f * c.scale;
    float y = area.y;

    for (int gi = 0; gi < nGroups; ++gi) {
        const SettingGroup& g = groups[gi];

        std::vector<Setting*> rows;
        if (g.all) {
            for (Setting* s : owner->mSettings)
                if (s && s->mVisible) rows.push_back(s);
        } else {
            for (const char* name : g.names) {
                for (Setting* s : owner->mSettings) {
                    if (!s || s->mName != name) continue;
                    if (s->mVisible) rows.push_back(s);
                    break;
                }
            }
        }

        const float bindH  = (g.bind && owner->canBind) ? c.rowH : 0.0f;
        const float ejectH = g.eject ? c.rowH : 0.0f;
        float need = rows.empty() ? 0.0f : rowGap;
        for (Setting* s : rows) need += settingHeight(c, s);
        need += bindH + ejectH;
        if (rows.empty() && !bindH && !ejectH) continue;

        const float top = y + (g.title ? headGap : 0.0f);
        const ImVec4 well(area.x, top, area.z, top + need + 2.0f * inset);
        const float wr = std::min(c.round * 0.7f, 11.0f * c.scale);

        if (draw) {
            // Headers and wells lead the entrance — they fade in slightly
            // ahead of the rows they contain.
            Ctx hc = c;
            hc.a = c.a * std::clamp(appear * 1.6f, 0.0f, 1.0f);
            if (g.title) {
                const float tfs = 11.0f * c.scale;
                const float tx  = area.x + inset;
                text(c.dl, c.bold, tfs, ImVec2(tx, y + 5.0f * c.scale),
                     std::string(g.title), c.inkMid, hc.a, false);
                // Label plus a rule to the right edge: keeps a long settings page
                // scannable without stacking another box on top of the group.
                const float ruleY = y + 5.0f * c.scale + tfs * 0.58f;
                const float lx = tx + measure(c.bold, tfs, std::string(g.title)) + 8.0f * c.scale;
                c.dl->AddLine(ImVec2(lx, ruleY), ImVec2(area.z - inset, ruleY),
                              col32(ImVec4(1, 1, 1, 1), 0.10f * hc.a), 1.0f);
            }
            if (!c.noWells && !c.columnsBody)
                drawWell(c.dl, well, hc.a, c.surface, wr, c.wellWipe);

            float ry = well.y + inset;
            const float lx = well.x + rowInset, rx = well.z - rowInset;
            int rowIdx = 0;
            const auto rowCtx = [&](float& slideX, float& slideY) -> Ctx {
                const float rk = c.columnsBody
                    ? std::clamp(appear, 0.0f, 1.0f)
                    : std::clamp(appear * 3.0f - rowIdx * (c.cornerReveal ? 0.10f : 0.14f),
                                 0.0f, 1.0f);
                const float e = 1.0f - easeOutCubic(rk);
                if (c.cornerReveal) {
                    slideX = e * -8.0f * c.scale;
                    slideY = e * -10.0f * c.scale;
                } else {
                    slideX = e * ((rowIdx & 1) ? 16.0f : -16.0f) * c.scale;
                    slideY = 0.0f;
                }
                Ctx rc = c;
                rc.a = c.a * rk;
                if (rk < 0.995f) { rc.live = false; rc.lmb = false; rc.rmb = false; }
                ++rowIdx;
                return rc;
            };
            for (Setting* s : rows) {
                const float sh = settingHeight(c, s);
                float slideX = 0.0f, slideY = 0.0f;
                Ctx rc = rowCtx(slideX, slideY);
                if (c.bloomK < 0.999f)
                    slideY += (c.bloomMid - (ry + sh * 0.5f)) * (1.0f - c.bloomK);
                const ImVec4 row(lx + slideX, ry + slideY, rx + slideX, ry + slideY + sh);
                if (row.w > area.y - 64.0f && row.y < area.w + 64.0f) {
                    captureTip(rc, ImVec4(row.x, row.y, row.z, row.y + c.rowH), s->mDescription);
                    drawSetting(rc, s, row);
                }
                ry += sh;
            }
            if (bindH) {
                float slideX = 0.0f, slideY = 0.0f;
                Ctx rc = rowCtx(slideX, slideY);
                if (c.bloomK < 0.999f)
                    slideY += (c.bloomMid - (ry + c.rowH * 0.5f)) * (1.0f - c.bloomK);
                drawKeybindRow(rc, owner, ImVec4(lx + slideX, ry + slideY,
                                                 rx + slideX, ry + slideY + c.rowH),
                               g.bindLabel);
                ry += c.rowH;
            }
            if (ejectH) {
                float slideX = 0.0f, slideY = 0.0f;
                Ctx rc = rowCtx(slideX, slideY);
                if (c.bloomK < 0.999f)
                    slideY += (c.bloomMid - (ry + c.rowH * 0.5f)) * (1.0f - c.bloomK);
                drawKeyRow(rc, ImVec4(lx + slideX, ry + slideY, rx + slideX, ry + slideY + c.rowH),
                           "Eject", &Config::UninjectKey,
                           "Ctrl + ", "Hold Ctrl and this key to unload the client");
                ry += c.rowH;
            }
        }
        y = well.w + grpGap;
    }

    {
        const float rh = 22.0f * c.scale;
        y -= grpGap;
        y += 4.0f * c.scale;
        if (draw) drawResetPill(c, owner, ImVec4(area.x, y, area.z, y + rh), true);
        y += rh;
    }
    return std::max(0.0f, y - area.y);
}

const SettingGroup kColGroups[] = {
    // Theme/Font/Colors live on the THEMES tab — no duplication here.
    // Gradient / Color Speed live on the THEMES tab (shared drawThemesBody).
    { "APPEARANCE", { "Saturation" } },
    { "DETAILS", { "Glow", "Shadow", "Shadow Strength", "Text Shadow", "Name Casing", "Sort Mode",
                   "Tooltips" } },
    { "MENU", { "Layout", "Columns Style", "Animation", "Ease Speed", "UI Scale", "Rounding",
                "Row Height", "Opacity", "Blur Strength", "Backdrop Dim",
                "Menu Buttons", "Block Keys", "Hotbar Animation", "Hotbar Speed" } },
    { "KEYS", {}, false, true, true, "Open menu" },
};
constexpr int kColGroupCount = static_cast<int>(sizeof(kColGroups) / sizeof(kColGroups[0]));

// A module's flyout: everything it exposes, in declaration order, plus its bind.
const SettingGroup kFlyGroup[] = { { nullptr, {}, true, true } };

// A Columns cap: the module and its measured label. No mode label — the first
// enum value next to a name is an ArrayList convention and belongs there only.
struct ColRow {
    Module*     mod = nullptr;
    std::string name;
    float       nameWidth = 0.f;
};

} // namespace

bool ClickGUI::WantsSearchKeys() {
    if (g_binding != nullptr || g_keyTarget != nullptr || g_strEdit) return false;
    int layout = 0;
    if (Module* iface = ModuleManager::GetInstance().GetModuleByName("ClickGUI"))
        for (Setting* s : iface->mSettings)
            if (s && s->mName == "Layout") { layout = static_cast<EnumSetting*>(s)->mValue; break; }
    return layout == 1 ? (g_colTab == 0) : (g_tab == 0);
}

std::vector<ClickGUI::CategoryPos> ClickGUI::GetCategoryPositions() {
    // Entry 0 carries the Panel-mode panel position; entries 1..5 carry the
    // Columns-mode card positions (one per category).
    std::vector<CategoryPos> out;
    out.push_back({ g_panelX, g_panelY, true });
    for (int i = 0; i < kColCount; ++i)
        out.push_back({ g_colX[i], g_colY[i], !g_colFolded[i] });
    return out;
}

void ClickGUI::SetCategoryPositions(const std::vector<CategoryPos>& positions) {
    if (positions.empty()) return;
    const CategoryPos& p = positions.front();
    if (p.x == 0.0f && p.y == 0.0f) return;
    g_panelX = p.x;
    g_panelY = p.y;
    g_placed = true;
    // Entries 1..6 restore the Columns-mode positions when present. Configs
    // written before the settings slab was persistable simply stop early.
    for (int i = 0; i < kColCount && (int)positions.size() > i + 1; ++i) {
        const CategoryPos& cp = positions[i + 1];
        if (cp.x == 0.0f && cp.y == 0.0f) continue;
        g_colX[i] = cp.x;
        g_colY[i] = cp.y;
        g_colPlaced[i] = true;
        g_colFolded[i] = !cp.isExtended;
    }
}

bool ClickGUI::IsBinding() { return g_binding != nullptr || g_keyTarget != nullptr; }
bool ClickGUI::IsStringEditing() { return g_strEdit != nullptr; }

void ClickGUI::SetTab(int tab) {
    const int clamped = (tab >= 0 && tab <= kExtraCount) ? tab : 0;
    if (clamped != g_tab) {
        g_tabFade = 0.0f;
        // Scroll is shared by every tab, so carrying it across a switch would drop
        // the new tab in at an arbitrary offset.
        g_scroll = g_scrollTarget = 0.0f;
    }
    g_tab = clamped;

    int layout = 0;
    if (Module* iface = ModuleManager::GetInstance().GetModuleByName("ClickGUI"))
        for (Setting* s : iface->mSettings)
            if (s && s->mName == "Layout") { layout = static_cast<EnumSetting*>(s)->mValue; break; }
    if (layout == 1) {
        static const int kPanelToCol[] = { 0, 2, 3, 1, 4 };  // mods, settings, configs, themes, changelog
        const int ct = kPanelToCol[clamped];
        if (ct != g_colTab) {
            g_colTabFade = 0.0f;
            g_colTabScroll = g_colTabScrollT = 0.0f;
        }
        g_colTab = ct;
    }
}

void ClickGUI::OnEnable() {
    g_open = 0.0f;
    g_ease = 0.0f;
    g_binding = nullptr;
    g_dragging = false;
    g_tabFade = 1.0f;
    g_scroll = g_scrollTarget = 0.0f;
    g_colTabFade = 1.0f;
    g_tabCardPlaced = false;            // re-centre the tab card on open
    g_tabCardDrag = false;

    if (Module* ts = ModuleManager::GetInstance().GetModuleByName("ToggleSounds"))
        if (ts->IsEnabled()) ToggleSounds::PlayToggleSound(true);

    ClipCursor(nullptr);
    while (ShowCursor(TRUE) < 0) {}
}

void ClickGUI::OnDisable() {
    g_dragging = false;
    g_binding = nullptr;
    g_keyTarget = nullptr;
    g_slider = nullptr;
    g_strEdit = nullptr;
    g_strFocus = false;
    g_openMod = nullptr;   // inline settings state dies with the GUI
    g_closingMod = nullptr;

    if (Module* ts = ModuleManager::GetInstance().GetModuleByName("ToggleSounds"))
        if (ts->IsEnabled()) ToggleSounds::PlayToggleSound(false);
}




void ClickGUI::DrawColumns() {
    const float dt = ImRenderUtils::getDeltaTime();
    const ImVec2 screen = ImRenderUtils::getScreenSize();
    const bool on = IsEnabled();
    const float openAlpha = std::clamp(g_open <= 0.0f ? 0.0f : easeOutCubic(g_open), 0.0f, 1.0f);
    g_uiTransform.pivot = ImVec2(screen.x * 0.5f, screen.y * 0.5f);
    const float scale  = std::clamp(mUiScale->mValue, 0.75f, 1.6f);
    const float tabH   = 34.0f * scale;

    const float headH  = 46.0f * scale;
    const float rowGapBase = 5.0f * scale;
    const float padX   = 10.0f * scale;
    const float padY   = 12.0f * scale;

    const float round  = (std::clamp(mRounding->mValue, 0.0f, 22.0f) * 0.5f + 12.0f) * scale;
    const float capRnd = 6.0f * scale;   // rows are solid gradient buttons: visible rounding

    const bool cleanStyle = (mColStyle && mColStyle->mValue == 1);
    const float rowGap = cleanStyle ? 0.0f : rowGapBase;

    const float nameFs = (cleanStyle ? 18.0f : 16.0f) * scale;
    const float colW   = kColW * scale;
    const float rowH   = std::clamp(mRowHeight->mValue, 26.0f, 44.0f) * scale;

    Ctx c;
    c.dl = ImGui::GetBackgroundDrawList();
    c.a = 1.0f;
    c.dt = dt;
    c.live = on && openAlpha > 0.55f && !g_strEdit;
    c.lmb = c.live && ImGui::IsMouseClicked(0);
    c.rmb = c.live && ImGui::IsMouseClicked(1);
    c.accent = ColorUtils::getThemedColor(0.0f);
    c.scale = scale;
    c.surface = mOpacity->mValue;
    c.round = round;
    c.rowH = rowH;
    c.shadow = mShadow->mValue;
    c.shadowK = mShadowStrength->mValue;
    c.tooltips = mTooltips->mValue;
    c.font = bodyFont();
    c.bold = titleFont();

    c.light = false;
    c.textShadow = false;   // thin light text, no shadow clutter
    c.glow = 0.0f;
    c.ink = kTxt;
    c.inkMid = kTxtDim;
    c.inkLo = mixC0(kTxtDim, kSlab, 0.35f);
    c.tint = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    c.hair = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    c.plate = kRowLo;
    c.plateA = 1.0f;
    c.knob = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    c.gradSpan = ColorUtils::getThemedPeriod();
    c.gradMs = ColorUtils::GetTimeMs();
    c.colSlider = true;   // the Columns slider look — never set in Panel mode
    {
        static ImColor s_sheetLut[128];
        constexpr int kSheetLutN = 128;
        const uint64_t slowMs = c.gradMs * 2 / 5;
        for (int li = 0; li < kSheetLutN; ++li)
            s_sheetLut[li] = ColorUtils::getThemedColor(
                static_cast<float>(li) / static_cast<float>(kSheetLutN) * c.gradSpan, slowMs);
        c.gradLut = s_sheetLut;
        c.gradLutN = kSheetLutN;
    }

    const ImVec2 mp = guiMousePos();

    // Tab content fade: eases 0 -> 1 after every switch, content slides in.
    g_colTabFade += (1.0f - g_colTabFade) * std::clamp(dt * 10.0f, 0.0f, 1.0f);
    if (g_colTabFade > 0.996f) g_colTabFade = 1.0f;
    const float tabA = easeOutCubic(g_colTabFade);
    const float tabSlide = (1.0f - tabA) * 10.0f * scale;

    const char* const kTabs[] = { "Modules", "Themes", "Settings", "Configs", "Changelog" };
    constexpr int kTabCount = 5;
    const float tabFs  = 15.0f * scale;
    const float tabPad = 22.0f * scale;
    const float tabGap = 8.0f * scale;
    float tabTextW[kTabCount];
    float stripW = tabGap * (kTabCount - 1);
    for (int t = 0; t < kTabCount; ++t) {
        tabTextW[t] = measure(c.bold, tabFs, kTabs[t]);
        stripW += tabTextW[t] + 2.0f * tabPad;
    }
    const float stripX = (screen.x - stripW) * 0.5f;
    const float stripY = 14.0f;
    ImVec4 tabR[kTabCount];
    float tabTx = stripX;
    for (int t = 0; t < kTabCount; ++t) {
        const float w = tabTextW[t] + 2.0f * tabPad;
        tabR[t] = ImVec4(tabTx, stripY, tabTx + w, stripY + tabH);
        tabTx += w + tabGap;
    }
    const ImVec4 stripR(stripX - 8.0f * scale, stripY - 6.0f * scale,
                        stripX + stripW + 8.0f * scale, stripY + tabH + 6.0f * scale);
    bool stripHov = false;
    for (int t = 0; t < kTabCount; ++t)
        if (over(tabR[t])) stripHov = true;
    stripHov = stripHov && c.live;


    const float topY = stripY + tabH + 14.0f * scale;

    // Draw last so the switcher owns overlapping hit tests.
    const auto drawStrip = [&]() {
        const float a = c.a;   // the switcher itself never cross-fades
        if (a <= 0.004f) return;
        const float stripRR = (stripR.w - stripR.y) * 0.5f;
        if (c.shadow && c.shadowK > 0.001f)
            ImRenderUtils::fillShadowRectangle(stripR, ImColor(0.0f, 0.0f, 0.0f, 1.0f),
                                               0.45f * c.shadowK * a * c.surface, stripRR, 24.0f * scale, 0.85f, c.dl);
        fill(c.dl, stripR, kSlab, 0.72f * a * c.surface, stripRR);

        // Sliding selection pill: chases the active tab's rect. Snapped on the
        // first frame so opening the menu never shows it gliding in from zero.
        const int act = std::clamp(g_colTab, 0, kTabCount - 1);
        const ImVec4& target = tabR[act];
        static float s_pillX = -1.0f, s_pillW = 0.0f;
        if (s_pillX < 0.0f) { s_pillX = target.x; s_pillW = target.z - target.x; }
        const float chase = std::clamp(dt * 14.0f, 0.0f, 1.0f);
        s_pillX += (target.x - s_pillX) * chase;
        s_pillW += ((target.z - target.x) - s_pillW) * chase;
        const float pillR = (target.w - target.y) * 0.5f;
        {
            const ImVec4 pill(s_pillX, target.y, s_pillX + s_pillW, target.w);
            ImColor acc0 = ColorUtils::getThemedColor(0.0f);
            ImColor acc1 = ColorUtils::getThemedColor(64.0f);
            if (cleanStyle) {
                fill(c.dl, pill, ImVec4(acc0.Value.x, acc0.Value.y, acc0.Value.z, 1.0f),
                     0.85f * a, pillR);
            } else {
                shadeDiag(c.dl, pill,
                          ImVec4(acc0.Value.x, acc0.Value.y, acc0.Value.z, 0.92f * a),
                          ImVec4(acc1.Value.x, acc1.Value.y, acc1.Value.z, 0.92f * a),
                          pillR);
                shadeDiag(c.dl, pill,
                          ImVec4(1.0f, 1.0f, 1.0f, 0.16f * a),
                          ImVec4(1.0f, 1.0f, 1.0f, 0.0f),
                          pillR);
            }
        }

        for (int t = 0; t < kTabCount; ++t) {
            const bool active = (g_colTab == t);
            const bool hov = c.live && over(tabR[t]);
            const float h = hoverOf("coltab" + std::to_string(t), hov, dt);
            if (h > 0.004f && !active)
                fill(c.dl, tabR[t], ImVec4(1, 1, 1, 1), 0.08f * h * a, pillR);
            const ImVec4 tc = active
                ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                : mixC0(kTxtDim, kTxt, 0.25f + 0.75f * h);
            text(c.dl, active ? c.bold : c.font, tabFs,
                 ImVec2(tabR[t].x + (tabR[t].z - tabR[t].x - tabTextW[t]) * 0.5f,
                        tabR[t].y + (tabH - tabFs) * 0.5f),
                 std::string(kTabs[t]), tc, a, false);
            if (hov && c.lmb && !active) {
                g_colTab = t;
                g_colTabFade = 0.0f;
                g_colTabScroll = g_colTabScrollT = 0.0f;
            }
        }
    };

    if (g_colTab == 0) {
        // ================= MODULES: the category columns =====================
        const std::string query = lowered(g_search);
        std::vector<ColRow> colRows[kCategoryCount];

        const auto casingName = [&](Module* m) -> std::string {
            std::string n = m->GetName();
            if (mCasing->mValue == 1) n = lowered(n);

            return n;
        };

        for (int ci = 0; ci < kCategoryCount; ++ci) {
            for (Module* m : ModuleManager::GetInstance().GetModulesInCategory(static_cast<Category>(ci))) {
                if (!m || m == this) continue;
                ColRow r;
                r.mod = m;
                r.name = casingName(m);
                r.nameWidth = measure(c.font, nameFs, r.name);
                colRows[ci].push_back(std::move(r));
            }
            std::stable_sort(colRows[ci].begin(), colRows[ci].end(),
                             [](const ColRow& a, const ColRow& b) {
                                 if (a.nameWidth != b.nameWidth)
                                     return a.nameWidth > b.nameWidth;
                                 return a.name < b.name;
                             });

            for (const ColRow& r : colRows[ci]) {
                Module* m = r.mod;
                const bool hit = query.empty() || lowered(r.name).find(query) != std::string::npos;
                const float target = hit ? 1.0f : 0.0f;
                m->searchAnim += (target - m->searchAnim) * std::clamp(dt * 13.0f, 0.0f, 1.0f);
                if (m->searchAnim < 0.002f) m->searchAnim = 0.0f;
                if (m->searchAnim > 0.998f) m->searchAnim = 1.0f;
                m->UpdateSettingsVisibility();
                m->enableAnim += ((m->IsEnabled() ? 1.0f : 0.0f) - m->enableAnim) *
                                 std::clamp(dt * 15.0f, 0.0f, 1.0f);
                // tickAnims animates cAnim toward mod->showSettings — the
                // Columns open/close state lives THERE, not in g_openMod.
                m->showSettings = (m == g_openMod);
                tickAnims(m, dt, m == g_openMod);
            }
        }
        // A flyout that finished closing retires its ghost; re-opening the
        // same module mid-close simply cancels the close.
        if (g_closingMod == g_openMod) g_closingMod = nullptr;
        if (g_closingMod && g_closingMod->cAnim <= 0.002f) g_closingMod = nullptr;

        float openNeed = 0.0f, closeNeed = 0.0f;
        if (g_openMod) {
            g_openMod->UpdateSettingsVisibility();
            openNeed = layoutSettingList(c, g_openMod,
                                         ImVec4(0.0f, 0.0f, colW - 2.0f * padX - 8.0f * scale, FLT_MAX),
                                         kFlyGroup, 1, false);
        }
        if (g_closingMod) {
            g_closingMod->UpdateSettingsVisibility();
            closeNeed = layoutSettingList(c, g_closingMod,
                                          ImVec4(0.0f, 0.0f, colW - 2.0f * padX - 8.0f * scale, FLT_MAX),
                                          kFlyGroup, 1, false);
        }

        float contentH[kCategoryCount] = {};
        for (int i = 0; i < kCategoryCount; ++i) {
            float h = 0.0f;
            for (const ColRow& r : colRows[i]) {
                Module* m = r.mod;
                h += (rowH + rowGap) * m->searchAnim;
                if (m == g_openMod)
                    h += (openNeed + 6.0f * scale) * easeOutCubic(std::clamp(m->cAnim, 0.0f, 1.0f));
                else if (m == g_closingMod)
                    h += (closeNeed + 6.0f * scale) * easeOutCubic(std::clamp(m->cAnim, 0.0f, 1.0f));
            }
            contentH[i] = std::max(0.0f, h - rowGap);
        }

        const float maxBody = std::max(60.0f, screen.y - topY - 56.0f * scale - headH - 2.0f * padY);
        {
            int   placedIdx[kCategoryCount];
            int   placedTier[kCategoryCount];
            int   nPlaced = 0;
            int   tier = 0;
            float curX = 0.0f, curY = topY, tierH = 0.0f;
            const float wrapX = screen.x - 16.0f;
            for (int i = 0; i < kCategoryCount; ++i) {
                if (g_colPlaced[i]) continue;
                if (nPlaced > 0 && curX + colW > wrapX) {
                    ++tier;
                    curX = 0.0f;
                    curY += tierH + 26.0f;
                    tierH = 0.0f;
                }
                g_colX[i] = curX;
                g_colY[i] = curY;
                g_colPlaced[i] = true;
                placedIdx[nPlaced] = i;
                placedTier[nPlaced] = tier;
                ++nPlaced;
                curX += colW + 18.0f;
                tierH = std::max(tierH, headH + 2.0f * padY + std::min(contentH[i], maxBody));
            }
            // Centre every tier this run laid out. Columns restored from a
            // config were already marked placed and keep their saved spots.
            for (int t = 0; t <= tier && nPlaced > 0; ++t) {
                float minX = FLT_MAX, maxX = -FLT_MAX;
                for (int k2 = 0; k2 < nPlaced; ++k2) {
                    if (placedTier[k2] != t) continue;
                    minX = std::min(minX, g_colX[placedIdx[k2]]);
                    maxX = std::max(maxX, g_colX[placedIdx[k2]] + colW);
                }
                if (minX > maxX) continue;
                const float shift = (screen.x - (maxX - minX)) * 0.5f - minX;
                for (int k2 = 0; k2 < nPlaced; ++k2)
                    if (placedTier[k2] == t) g_colX[placedIdx[k2]] += shift;
            }
        }
        for (int i = 0; i < kCategoryCount; ++i) {
            // Columns may hang past ANY screen edge — the wheel pan always
            // brings them back, so there is no reason to cage the drag.
            g_colX[i] = std::clamp(g_colX[i], -colW - 40.0f, screen.x + 40.0f);
            g_colY[i] = std::clamp(g_colY[i], -headH - 60.0f, screen.y + 40.0f);
        }

        ImVec4 panelR[kCategoryCount];
        float viewH[kCategoryCount] = {};
        float bodyFrac[kCategoryCount] = {};
        for (int i = 0; i < kCategoryCount; ++i) {
            viewH[i] = contentH[i];
            // A folded column (RMB on the header) eases its body to zero height,
            // turning the card into a header-only dropdown cap.
            const float want = g_colFolded[i] ? 0.0f : viewH[i];
            g_colBody[i] += (want - g_colBody[i]) * std::clamp(dt * (want > g_colBody[i] ? 26.0f : 20.0f),
                                                               0.0f, 1.0f);
            if (std::fabs(g_colBody[i] - want) < 0.6f) g_colBody[i] = want;
            bodyFrac[i] = g_colBody[i] / std::max(1.0f, viewH[i]);
            // Header fold slide tracks ONLY the fold flag — see g_colFoldAnim.
            const float foldWant = g_colFolded[i] ? 1.0f : 0.0f;
            g_colFoldAnim[i] += (foldWant - g_colFoldAnim[i]) * std::clamp(dt * 13.0f, 0.0f, 1.0f);
            if (std::fabs(g_colFoldAnim[i] - foldWant) < 0.004f) g_colFoldAnim[i] = foldWant;
        }

        if (c.live && !stripHov && ImGui::GetIO().MouseWheel != 0.0f)
            g_colPanYT += ImGui::GetIO().MouseWheel * 64.0f * scale;
        g_colPanY = MathUtils::animate(g_colPanYT, g_colPanY, std::clamp(dt * 12.0f, 0.0f, 1.0f));

        for (int i = 0; i < kCategoryCount; ++i) {
            const float oy = g_colY[i] + tabSlide + g_colPanY;
            const float padBot = cleanStyle
                ? (padY * (1.0f - bodyFrac[i]) + 3.0f * scale * bodyFrac[i])
                : padY * (1.0f + bodyFrac[i]);
            panelR[i] = ImVec4(g_colX[i], oy, g_colX[i] + colW,
                               oy + headH + g_colBody[i] + padBot);
        }

        ImVec4 gradAll(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (int i = 0; i < kCategoryCount; ++i) {
            gradAll.x = std::min(gradAll.x, panelR[i].x);
            gradAll.y = std::min(gradAll.y, panelR[i].y + headH);
            gradAll.z = std::max(gradAll.z, panelR[i].z);
            gradAll.w = std::max(gradAll.w, panelR[i].w);
        }
        if (gradAll.z - gradAll.x < 1.0f) gradAll = ImVec4(0, 0, 1, 1);
        // Shared with every widget drawn inside the columns (the slider fill
        // samples the same sheet) — the flyout Ctx copies below inherit it.
        c.gradSpace = &gradAll;

        int flyCol = -1;
        {
            Module* const fly[2] = { g_openMod, g_closingMod };
            for (int i = 0; i < kCategoryCount && flyCol < 0; ++i)
                for (const ColRow& r : colRows[i])
                    if (r.mod == fly[0] || r.mod == fly[1]) { flyCol = i; break; }
        }

        int order[kCategoryCount];
        int nOrder = 0;
        for (int i = 0; i < kCategoryCount; ++i)
            if (i != g_colDrag && i != flyCol) order[nOrder++] = i;
        if (g_colDrag >= 0 && g_colDrag < kCategoryCount && g_colDrag != flyCol)
            order[nOrder++] = g_colDrag;
        if (flyCol >= 0) order[nOrder++] = flyCol;

        int topCol = -1;
        if (c.live) {
            for (int oi = 0; oi < nOrder; ++oi) {
                const int i = order[oi];
                const ImVec4& pr = panelR[i];
                const float trayTop = headH + (cleanStyle ? 1.5f * scale : padY * 0.5f);
                const float bottom = g_colFolded[i]
                    ? pr.w
                    : std::max(pr.w, pr.y + trayTop + contentH[i] + 3.0f * scale);
                if (mp.x >= pr.x && mp.y >= pr.y && mp.x <= pr.z && mp.y <= bottom) topCol = i;
            }
        }
        // The tab strip floats above the columns and is drawn last — it wins
        // any overlap, so a column dragged under it stops taking input.
        if (stripHov) topCol = -1;

        // ---- per-column render ---------------------------------------------------
        for (int oi = 0; oi < nOrder; ++oi) {
            const int i = order[oi];
            const ImVec4 panel = panelR[i];
            const float a = c.a * tabA;
            if (a <= 0.004f) continue;

            // Glass card: blur + translucent near-black, no outline.
            drawGlassCard(c.dl, panel, a, round, c.shadow, c.surface, c.shadowK);


            const ImVec4 head(panel.x, panel.y, panel.z, panel.y + headH);
            const bool headHov = c.live && topCol == i && over(head);
            const float hhov = hoverOf("colhead" + std::to_string(i), headHov, dt);
            c.dl->PushClipRect(ImVec2(head.x, head.y), ImVec2(head.z, head.w + 1.0f), true);
            if (cleanStyle)
                fill(c.dl, head, mixC0(kHead, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.08f * hhov),
                     0.92f * a * c.surface, round, ImDrawFlags_RoundCornersTop);
            else
                gradientV(c.dl, head, mixC0(kHead, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.08f * hhov),
                          mixC0(kHead, kSlab, 0.65f), 0.95f * a * c.surface, 0.95f * a * c.surface, round,
                          ImDrawFlags_RoundCornersTop);
            c.dl->PopClipRect();
            {
                const float ts = 22.0f * scale;
                const std::string title(kCategoryNames[i]);
                const float tw = measure(c.bold, ts, title);

                ImFont* icons = RenderManager::GetInstance().iconFont;
                float glyphW = 0.0f;
                if (icons) {
                    const float gs = 20.0f * scale;
                    glyphW = gs + 9.0f * scale;
                    const ImVec4 gc = mixC0(kTxt,
                        ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f),
                        0.35f + 0.55f * hhov);
                    c.dl->AddText(icons, gs,
                                  ImVec2(head.x + 13.0f * scale, head.y + (headH - gs) * 0.5f - 0.5f),
                                  ImGui::GetColorU32(ImVec4(gc.x, gc.y, gc.z, a)),
                                  kRailGlyphs[i + 1]);
                }

                const float foldT = easeOutCubic(std::clamp(g_colFoldAnim[i], 0.0f, 1.0f));
                const float centeredX = head.x + (head.z - head.x - tw) * 0.5f;
                const float dockedX = head.x + 13.0f * scale + glyphW + 2.0f * scale;
                text(c.dl, c.bold, ts,
                     ImVec2(MathUtils::lerp(centeredX, dockedX, foldT),
                            head.y + (headH - ts) * 0.5f),
                     title, kTxt, a, false);
            }

            if (headHov && c.rmb) g_colFolded[i] = !g_colFolded[i];
            if (c.live && g_colDrag < 0 && ImGui::IsMouseClicked(0) && topCol == i && over(head)) {
                g_colDrag = i;
                g_colDragDX = mp.x - g_colX[i];
                g_colDragDY = mp.y - (g_colY[i] + g_colPanY);
            }
            if (g_colDrag == i) {
                if (ImGui::IsMouseDown(0)) {
                    g_colX[i] = mp.x - g_colDragDX;
                    g_colY[i] = mp.y - g_colDragDY - g_colPanY;
                } else {
                    g_colDrag = -1;
                }
            }

            const ImVec4 tray(panel.x + padX, head.w + (cleanStyle ? 1.5f * scale : padY * 0.5f),
                              panel.z - padX, panel.w - (cleanStyle ? 3.0f * scale : padY));

            const float laidBottom = tray.y + contentH[i];
            const float clipBottom = g_colFolded[i]
                ? tray.y + g_colBody[i]
                : std::max(tray.y + g_colBody[i], laidBottom);
            c.dl->PushClipRect(ImVec2(panel.x + 1.0f, tray.y),
                               ImVec2(panel.z - 1.0f, std::max(tray.y, clipBottom)), true);

            // The last VISIBLE row gets the card's bottom rounding in Clean.
            int lastVis = -1;
            if (cleanStyle)
                for (int ri = 0; ri < static_cast<int>(colRows[i].size()); ++ri)
                    if (colRows[i][ri].mod->searchAnim > 0.5f) lastVis = ri;

            float y = tray.y;
            // Clean rows run flush to the tray edges — fused with the column;
            // Machined rows are buttons, so they keep a hair of side inset.
            const float rowInset = cleanStyle ? 0.0f : 1.5f * scale;
            int ri = 0;
            for (const ColRow& r : colRows[i]) {
                Module* m = r.mod;
                const float sA = m->searchAnim;
                const float capH = rowH * sA;
                if (capH <= 1.0f) { y += capH + rowGap * sA; ++ri; continue; }
                const bool isOpen = (g_openMod == m);
                const bool isClosing = (m == g_closingMod) && !isOpen;
                const float bodyNeed = isOpen ? openNeed : (isClosing ? closeNeed : 0.0f);
                const float bodyH = (isOpen || isClosing)
                    ? (bodyNeed + 6.0f * scale) * easeOutCubic(std::clamp(m->cAnim, 0.0f, 1.0f))
                    : 0.0f;
                const ImVec4 row(tray.x + rowInset, y, tray.z - rowInset, y + capH);
                const bool hov = c.live && topCol == i &&
                                 over(ImVec4(row.x, row.y, row.z, row.y + capH)) &&
                                 row.y < clipBottom && row.w > tray.y;
                const float hv = hoverOf("crow" + m->GetName(), hov, c.dt);
                const float enA = m->enableAnim;
                const float enE = easeOutCubic(std::clamp(enA, 0.0f, 1.0f));
                const bool dent = hov && ImGui::IsMouseDown(0);

                if (hov) {
                    // Left click toggles the module. Pinning is a Panel-only
                    // feature — no pin zone is carved out of Columns rows.
                    if (c.lmb) {
                        m->Toggle();
                        if (Module* ts2 = ModuleManager::GetInstance().GetModuleByName("ToggleSounds"))
                            if (ts2->IsEnabled()) ToggleSounds::PlayToggleSound(m->IsEnabled());
                        Notifications::AddNotification(
                            m->GetName(),
                            m->IsEnabled() ? NotifType::ToggleOn : NotifType::ToggleOff);
                    }
                    if (c.rmb) {
                        Module* prevOpen = g_openMod;
                        g_openMod = isOpen ? nullptr : m;
                        // The module losing the flyout keeps blooming OUT —
                        // closing is the same animation reversed, not a cut.
                        if (prevOpen && prevOpen != g_openMod) g_closingMod = prevOpen;
                    }
                    captureTip(c, row, m->GetDescription());
                }

                const bool isLast = (ri == lastVis) && !((isOpen || isClosing) && bodyH > 1.0f);
                bool nextEn = false;
                if (!isLast)
                    for (int nj = ri + 1; nj < static_cast<int>(colRows[i].size()); ++nj) {
                        if (colRows[i][nj].mod->searchAnim <= 0.5f) continue;
                        nextEn = colRows[i][nj].mod->enableAnim > 0.02f;
                        break;
                    }
                bool prevEn = false;
                if (ri > 0) {
                    for (int pj = ri - 1; pj >= 0; --pj) {
                        if (colRows[i][pj].mod->searchAnim <= 0.5f) continue;
                        prevEn = colRows[i][pj].mod->enableAnim > 0.02f;
                        break;
                    }
                }
                const bool joinDown = nextEn && enE > 0.02f;
                const bool joinUp = prevEn && enE > 0.02f;
                const float joinH = joinDown
                    ? (cleanStyle ? 3.0f * scale : rowGap + 1.25f * scale)
                    : 0.0f;
                const ImVec4 fullRow(panel.x + 1.0f, row.y, panel.z - 1.0f, row.w + joinH);
                drawRowMinimal(c, row, enE, hv, dent,
                               a * sA, cleanStyle ? 0.0f : capRnd, isOpen || isClosing, cleanStyle,
                               cleanStyle ? &fullRow : nullptr,
                               &gradAll, isLast ? round : 0.0f, cleanStyle,
                               cleanStyle ? 0.0f : joinH, cleanStyle ? false : joinUp);
                if (capH > nameFs + 6.0f * scale) {
                    const float openE = easeOutCubic(std::clamp(m->cAnim, 0.0f, 1.0f));
                    const float rowW = row.z - row.x;
                    const float cx = row.x + (rowW - r.nameWidth) * 0.5f;
                    const float lx = row.x + 12.0f * scale;
                    const float nx = MathUtils::lerp(cx, lx, openE);
                    const float ty = row.y + (capH - nameFs) * 0.5f;
                    const ImVec4 nc = mixC0(kTxt, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.35f * openE);
                    text(c.dl, c.font, nameFs, ImVec2(nx, ty), r.name, nc,
                         (0.88f + 0.12f * enA) * a * sA, false);
                }

                if ((isOpen || isClosing) && bodyH > 1.0f) {
                    const float openT = std::clamp(m->cAnim, 0.0f, 1.0f);
                    const float bloom = 0.40f + 0.60f * easeOutCubic(std::clamp(openT * 1.08f, 0.0f, 1.0f));
                    const float inset = 2.0f * scale;
                    const ImVec4 area(row.x + inset, row.w + 2.0f * scale,
                                      row.z - inset, row.w + 2.0f * scale + bodyH);
                    Ctx bc = c;
                    bc.a = c.a * std::clamp(openT * 1.6f, 0.0f, 1.0f);

                    bc.noWells = cleanStyle;
                    bc.columnsBody = true;
                    bc.columnsClean = cleanStyle;
                    bc.bloomK = bloom;
                    bc.bloomMid = area.y + bodyH * 0.5f;
                    if (openT < 0.85f) { bc.live = false; bc.lmb = false; bc.rmb = false; }
                    c.dl->PushClipRect(ImVec2(panel.x + 1.0f, area.y),
                                       ImVec2(panel.z - 1.0f, area.y + bodyH), true);
                    layoutSettingList(bc, m, area, kFlyGroup, 1, true, 1.0f);
                    c.dl->PopClipRect();
                }
                y += capH + bodyH + rowGap * sA;
                ++ri;
            }

            c.dl->PopClipRect();
        }


        if (g_openMod && c.lmb && topCol >= 0) {
            bool onOpen = false;
            for (int i = 0; i < kCategoryCount && !onOpen; ++i)
                for (const ColRow& r : colRows[i])
                    if (r.mod == g_openMod) onOpen = true;
            if (!onOpen) { g_closingMod = g_openMod; g_openMod = nullptr; }
        }

        {
            Ctx sc = c;
            sc.a = c.a * tabA;

            const float sFs = 15.0f * scale * 1.4f;
            drawSearchText(sc, ImVec4(0.0f, screen.y - sFs - 30.0f * scale,
                                      screen.x, screen.y - 12.0f * scale), dt, 1.4f);
        }

        drawStrip();
        return;
    }

    // ================= NON-MODULE TABS: one draggable glass card ===============
    UpdateSettingsVisibility();
    tickAnims(this, dt, /*bodyOpen=*/true);

    const float cardW = (g_colTab == 1 || g_colTab == 2 ? 660.0f : 560.0f) * scale;
    const float cardY0 = topY;
    const float maxCardH = screen.y - cardY0 - 24.0f;

    static float s_tabBody[kTabCount] = {};
    static int s_lastTab = -1;
    if (s_lastTab != g_colTab) {
        s_lastTab = g_colTab;
        g_colTabScroll = g_colTabScrollT = 0.0f;
    }
    float bodyH = (g_colTab >= 1) ? s_tabBody[g_colTab] : 0.0f;
    const float innerW = cardW - 2.0f * padX;
    if (g_colTab == 2)
        bodyH = layoutSettingList(c, this, ImVec4(0.0f, 0.0f, innerW, FLT_MAX),
                                  kColGroups, kColGroupCount, false);
    else if (g_colTab == 1 && bodyH <= 0.0f)
        bodyH = 950.0f * scale;   // first-frame estimate until drawn once

    if (!g_tabCardPlaced) {
        g_tabCardX = (screen.x - cardW) * 0.5f;
        g_tabCardY = cardY0;
        g_tabCardPlaced = true;
    }
    g_tabCardX = std::clamp(g_tabCardX, 8.0f - cardW * 0.3f, std::max(8.0f, screen.x - cardW * 0.7f));
    g_tabCardY = std::clamp(g_tabCardY, cardY0, std::max(cardY0, screen.y - 120.0f));

    const float viewH = std::min(bodyH + 8.0f * scale, maxCardH - headH - 2.0f * padY);
    const float maxScroll = std::max(0.0f, bodyH - viewH);
    const ImVec4 card(g_tabCardX, g_tabCardY, g_tabCardX + cardW,
                      g_tabCardY + headH + 2.0f * padY + viewH);
    const ImVec4 head(card.x, card.y, card.z, card.y + headH);

    if (c.live && !stripHov && over(card)) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) g_colTabScrollT -= wheel * 60.0f;
    }
    g_colTabScrollT = std::clamp(g_colTabScrollT, 0.0f, maxScroll);
    g_colTabScroll = MathUtils::animate(g_colTabScrollT, g_colTabScroll, std::clamp(dt * 14.0f, 0.0f, 1.0f));

    const float a = c.a * tabA;
    drawGlassCard(c.dl, card, a, round, c.shadow, c.surface, c.shadowK);

    // Header strip: Machined gets a lighter vertical gradient, Clean keeps one
    // flat strip — no 3D anywhere. Title centred, draggable.
    c.dl->PushClipRect(ImVec2(head.x, head.y), ImVec2(head.z, head.w + 1.0f), true);
    if (cleanStyle)
        fill(c.dl, head, kHead, 0.92f * a * c.surface, round, ImDrawFlags_RoundCornersTop);
    else
        gradientV(c.dl, head, kHead, mixC0(kHead, kSlab, 0.65f), 0.95f * a * c.surface, 0.95f * a * c.surface, round,
                  ImDrawFlags_RoundCornersTop);
    c.dl->PopClipRect();
    {
        const float ts = 14.0f * scale;
        const std::string title(kTabs[g_colTab]);
        const float tw = measure(c.bold, ts, title);
        text(c.dl, c.bold, ts, ImVec2(head.x + (head.z - head.x - tw) * 0.5f,
                                      head.y + (headH - ts) * 0.5f),
             title, kTxt, a, false);
    }
    if (c.live && !stripHov && !g_tabCardDrag && ImGui::IsMouseClicked(0) && over(head)) {
        g_tabCardDrag = true;
        g_tabCardDX = mp.x - g_tabCardX;
        g_tabCardDY = mp.y - g_tabCardY;
    }
    if (g_tabCardDrag) {
        if (ImGui::IsMouseDown(0)) {
            g_tabCardX = mp.x - g_tabCardDX;
            g_tabCardY = mp.y - g_tabCardDY;
        } else {
            g_tabCardDrag = false;
        }
    }

    const float contentTop = card.y + headH + padY;
    const ImVec4 area(card.x + padX, contentTop - g_colTabScroll,
                      card.z - padX, contentTop + viewH);
    c.dl->PushClipRect(ImVec2(area.x - 6.0f * scale, contentTop - 2.0f * scale),
                       ImVec2(area.z + 6.0f * scale, card.w - 3.0f * scale), true);
    if (g_colTab == 1) {
        s_tabBody[1] = drawThemesBody(c, area);
    } else if (g_colTab == 2) {
        layoutSettingList(c, this, area, kColGroups, kColGroupCount, true,
                         easeOutCubic(g_colTabFade));
        s_tabBody[2] = bodyH;
    } else if (g_colTab == 3) {
        s_tabBody[3] = drawConfigsBody(c, area);
    } else if (g_colTab == 4) {
        s_tabBody[4] = drawChangelogBody(c, area);
    }
    c.dl->PopClipRect();

    // Quiet scroll rail.
    if (maxScroll > 1.0f) {
        const float railX = card.z - 3.5f * scale;
        const float t = g_colTabScroll / maxScroll;
        const float thumbH = std::max(24.0f * scale, viewH * viewH / std::max(1.0f, bodyH));
        const float ty = contentTop + t * (viewH - thumbH);
        c.dl->AddRectFilled(ImVec2(railX, ty), ImVec2(railX + 2.0f * scale, ty + thumbH),
                            ImGui::GetColorU32(ImVec4(1, 1, 1, 0.14f * a)), 1.5f * scale);
    }

    drawStrip();
}


void ClickGUI::OnRender() {
    g_tipShow = false;
    g_uiTransform.enabled = false;
    const bool on = IsEnabled();
    if (on) ClipCursor(nullptr);

    const float dt = ImRenderUtils::getDeltaTime();
    const bool bloomAnim = mAnim && mAnim->mValue == 1;
    const float openRate = std::max(6.0f, mEaseSpeed->mValue * 0.6f);
    const float base = std::clamp(dt * openRate, 0.0f, 1.0f);
    const float step = on ? base : std::min(1.0f, base * (bloomAnim ? 1.15f : 2.4f));
    g_open += ((on ? 1.0f : 0.0f) - g_open) * step;
    g_open = std::clamp(g_open, 0.0f, 1.0f);
    if (!on && g_open < 0.004f) { g_open = 0.0f; return; }
    if (on && g_open > 0.996f) g_open = 1.0f;


    const int animMode = mAnim->mValue;
    switch (animMode) {
    case 0:  g_ease = easeOutBack(g_open); break;   // Cascade: springy drop-in
    case 1:  g_ease = easeOutBack(g_open); break;   // Bloom: springy scale-up
    default: g_ease = on ? 1.0f : g_open; break;
    }
    // Fade and zoom share progress; only zoom uses the overshooting curve.
    const float alpha = std::clamp(g_open <= 0.0f ? 0.0f : easeOutCubic(g_open), 0.0f, 1.0f);

    if (on && g_binding == nullptr && g_keyTarget == nullptr && !g_strEdit &&
        (GetAsyncKeyState(VK_ESCAPE) & 0x8000))
        SetEnabled(false);

    const ImVec2 screen = ImRenderUtils::getScreenSize();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    if (mBlur->mValue > 0.0f && alpha > 0.01f) {
        D2D::addBlurOptimized(dl, mBlur->mValue * std::pow(alpha, 1.6f) *
                              (1.0f + 0.8f * (1.0f - alpha)),
                                  std::optional<ImVec4>(ImVec4(0, 0, screen.x, screen.y)), 0.0f);
    }
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(screen.x, screen.y),
                      ImGui::GetColorU32(ImVec4(0.008f, 0.010f, 0.016f, mDim->mValue * alpha)));

    dl->PushClipRectFullScreen();
    const UiDrawRange mainRange(dl);

    const float uiScale = std::clamp(mUiScale->mValue, 0.75f, 1.35f);
    // Saved positions and widget layout always use the unzoomed panel.
    const float restW = kPanelW * uiScale;
    const float restH = std::min(kPanelH * uiScale, screen.y - 40.0f);

    if (!g_placed) {
        g_panelX = (screen.x - restW) * 0.5f;
        g_panelY = (screen.y - restH) * 0.5f;
        g_placed = true;
    }
    // Keep the panel reachable after a resolution change or a stale saved config.
    g_panelX = std::clamp(g_panelX, -restW * 0.35f, screen.x - restW * 0.65f);
    g_panelY = std::clamp(g_panelY, 0.0f, std::max(0.0f, screen.y - kHeadH * uiScale));

    const float slideEase = on ? g_ease : easeOutCubic(g_open);
    const float slide = (1.0f - slideEase) *
                        (animMode == 0 ? 48.0f : 12.0f) * uiScale;
    const float minZoom = bloomAnim ? 0.72f : 0.90f;
    float zoom;
    if (on) {
        const float zoomEase = bloomAnim ? easeOutBack(g_open) : easeOutCubic(g_open);
        zoom = std::clamp(minZoom + (1.0f - minZoom) * zoomEase, 0.75f, 1.06f);
    } else {
        zoom = std::pow(easeOutCubic(g_open), 1.7f);
    }
    const float scale  = uiScale;
    const float panelW = restW;
    const float panelH = restH;
    g_uiTransform.enabled = true;
    g_uiTransform.pivot = ImVec2(g_panelX + restW * 0.5f, g_panelY + restH * 0.5f);
    g_uiTransform.slide = ImVec2(0.0f, slide);
    g_uiTransform.zoom = zoom;
    g_uiTransform.fade = alpha;

    Ctx c;
    c.dl = dl;
    c.a = 1.0f;
    c.dt = dt;
    c.live = on && alpha > 0.55f && !g_strEdit;
    c.lmb = c.live && ImGui::IsMouseClicked(0);
    c.rmb = c.live && ImGui::IsMouseClicked(1);
    c.accent = ColorUtils::getThemedColor(0.0f);
    c.scale = scale;
    c.textShadow = mTextShadow->mValue;
    c.surface = mOpacity->mValue;
    c.round = mRounding->mValue * scale;
    c.rowH = mRowHeight->mValue * scale;
    c.glow = mGlow->mValue;
    c.shadow = mShadow->mValue;
    c.shadowK = mShadowStrength->mValue;
    c.tooltips = mTooltips->mValue;
    c.font = bodyFont();
    c.bold = titleFont();

    if (mLayout->mValue == 1) {

        DrawColumns();
    } else {
    const ImVec4 panel(g_panelX, g_panelY, g_panelX + panelW, g_panelY + panelH);
    const float railW = kRailW * scale;
    const ImVec4 rail(panel.x, panel.y, panel.x + railW, panel.w);

    drawPanelShell(c, panel, rail);

    const float headH = kHeadH * scale;
    const ImVec4 head(rail.z, panel.y, panel.z, panel.y + headH);
    {
        if (c.live && !g_dragging && ImGui::IsMouseClicked(0) && over(head)) {
            g_dragging = true;
            g_dragDX = guiMousePos().x - g_panelX;
            g_dragDY = guiMousePos().y - g_panelY;
        }
        if (g_dragging) {
            if (ImGui::IsMouseDown(0)) {
                g_panelX = guiMousePos().x - g_dragDX;
                g_panelY = guiMousePos().y - g_dragDY;
            } else {
                g_dragging = false;
            }
        }

        const std::string title = (g_tab == 0)
            ? (g_category < 0 ? std::string("All")
                              : std::string(kCategoryNames[std::clamp(g_category, 0,
                                                                     kCategoryCount - 1)]))
            : std::string(kExtraNames[std::clamp(g_tab - 1, 0, kExtraCount - 1)]);
        const float ts = 21.0f * scale;
        text(dl, c.bold, ts, ImVec2(head.x + kPad * scale, head.y + (headH - ts) * 0.5f - 1.0f),
             title, kTextHi, c.a, c.textShadow);

        dl->AddLine(ImVec2(head.x + kPad * scale, head.w), ImVec2(head.z - kPad * scale, head.w),
                    ImGui::GetColorU32(ImVec4(1, 1, 1, 0.055f * c.a)), 1.0f);
    }

    // --- rail ----------------------------------------------------------------
    {
        // Brand block at the top of the rail — name in white, version in the
        // theme accent, nothing else.
        const float bs = 17.0f * scale;
        text(dl, c.bold, bs, ImVec2(rail.x + 14.0f * scale, rail.y + 18.0f * scale),
             std::string(Sunstead::ClientName), kTextHi, c.a, c.textShadow);
        const float bw = measure(c.bold, bs, Sunstead::ClientName);
        text(dl, c.font, 11.0f * scale,
             ImVec2(rail.x + 14.0f * scale + bw + 6.0f * scale, rail.y + 21.0f * scale),
             std::string(Sunstead::ClientVersion),
             ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f), c.a, false);

        const float itemH = 34.0f * scale;
        const float top = rail.y + headH + 6.0f * scale;
        const float ix = rail.x + 8.0f * scale;
        const float iw = rail.z - 8.0f * scale;

        // Rail slot space: 0 = All, 1..kCategoryCount = categories, then extras.
        const int railEntryCount = kCategoryCount + 1;

        const auto catOfEntry = [&](int e) { return e - 1; }; // entry 1..5 -> Category 0..4
        for (int e = 0; e < railEntryCount; ++e) {
            const bool isAll = (e == 0);
            const ImVec4 r(ix, top + e * itemH, iw, top + (e + 1) * itemH);
            const bool active = (g_tab == 0) &&
                                (isAll ? (g_category < 0) : (g_category == catOfEntry(e)));
            // Tab icons (the shared rail/category glyph set).
            drawRailItem(c, r, isAll ? "All" : kCategoryNames[catOfEntry(e)],
                         kRailGlyphs[e], active, "rail" + std::to_string(e));
            if (c.live && c.lmb && over(r)) {
                const int newCat = isAll ? -1 : catOfEntry(e);
                if (g_tab != 0 || g_category != newCat) {
                    g_tabFade = 0.0f;
                    g_scrollTarget = g_scroll = 0.0f;
                }
                g_tab = 0;
                g_category = newCat;
            }
        }

        const float extraTop = top + railEntryCount * itemH + 20.0f * scale;
        dl->AddLine(ImVec2(ix + 6.0f * scale, extraTop - 11.0f * scale),
                    ImVec2(iw - 6.0f * scale, extraTop - 11.0f * scale),
                    ImGui::GetColorU32(ImVec4(1, 1, 1, 0.045f * c.a)), 1.0f);

        for (int i = 0; i < kExtraCount; ++i) {
            const ImVec4 r(ix, extraTop + i * itemH, iw, extraTop + (i + 1) * itemH);
            drawRailItem(c, r, kExtraNames[i], kExtraGlyphs[i], g_tab == i + 1,
                         "railx" + std::to_string(i));
            if (c.live && c.lmb && over(r)) SetTab(i + 1);
        }


        const ImVec4 cl(ix, rail.w - 42.0f * scale, iw, rail.w - 12.0f * scale);
        drawRailItem(c, cl, "Close", nullptr, false, "railclose");
        if (c.live && c.lmb && over(cl)) SetEnabled(false);
    }

    // --- content -------------------------------------------------------------
    g_tabFade += (1.0f - g_tabFade) * std::clamp(dt * 11.0f, 0.0f, 1.0f);
    if (g_tabFade > 0.996f) g_tabFade = 1.0f;

    Ctx cc = c;
    cc.a = c.a * g_tabFade;
    const float contentSlide = (1.0f - easeOutCubic(g_tabFade)) * 16.0f * scale;

    const ImVec4 content(rail.z + kPad * scale, head.w + kPad * scale - 4.0f * scale,
                         panel.z - kPad * scale, panel.w - kPad * scale);

    dl->PushClipRect(ImVec2(content.x - 6.0f * scale, content.y),
                     ImVec2(content.z + 6.0f * scale, content.w), true);
    if (g_tab == 0) {
        // Search line, always reserved at the top of the module list. Empty it
        // shows the hint; once a query is typed the same slot draws the query.
        const float searchLineH = 18.0f * scale + 14.0f * scale;
        drawSearchText(c, ImVec4(content.x, content.y, content.z, content.y + searchLineH), dt);

        // Collect the modules for the active rail entry, honouring search + sort.
        // g_category == -1 is the ALL overview: every category in one list.
        std::vector<Module*> mods;
        const std::string query = lowered(g_search);
        const auto consider = [&](Module* m) {
            if (!m || m == this) return;
            const bool match = query.empty() ||
                               lowered(m->GetName()).find(query) != std::string::npos;
            const float target = match ? 1.0f : 0.0f;
            m->searchAnim += (target - m->searchAnim) *
                             std::clamp(dt * (match ? 14.0f : 17.0f), 0.0f, 1.0f);
            if (m->searchAnim < 0.002f) m->searchAnim = 0.0f;
            if (m->searchAnim > 0.998f) m->searchAnim = 1.0f;
            if (m->searchAnim > 0.004f) mods.push_back(m);
        };
        int matchCount = 0;
        if (g_category < 0) {
            for (Module* m : ModuleManager::GetInstance().GetModules()) consider(m);
        } else {
            for (Module* m : ModuleManager::GetInstance().GetModulesInCategory(
                     static_cast<Category>(std::clamp(g_category, 0, kCategoryCount - 1))))
                consider(m);
        }
        for (Module* m : mods)
            if (m->searchAnim > 0.5f) ++matchCount;
        std::stable_sort(mods.begin(), mods.end(), [](Module* a, Module* b) {
            return _stricmp(a->GetName().c_str(), b->GetName().c_str()) < 0;
        });
        if (mSort->mValue == 1) {
            std::reverse(mods.begin(), mods.end());
        } else if (mSort->mValue == 2) {
            std::stable_sort(mods.begin(), mods.end(), [](Module* a, Module* b) {
                return a->IsEnabled() && !b->IsEnabled();
            });
        }
        // Pinned modules float to the top of the list (All AND category views),
        // above whatever sort is active — stable keeps their relative order.
        std::stable_sort(mods.begin(), mods.end(), [](Module* a, Module* b) {
            return a->guiPinned && !b->guiPinned;
        });

        // Animations advance before measuring so reported and drawn heights match.
        for (Module* m : mods) {
            m->UpdateSettingsVisibility();
            tickAnims(m, dt, m->showSettings);
        }

        const float cardGap = 9.0f * scale;
        constexpr int kCols = 2;
        const float cardW = (content.z - content.x - cardGap * (kCols - 1)) / kCols;
        const auto slotH = [&](Module* m) {
            return cardHeight(cc, m, cardW) * easeOutCubic(m->searchAnim);
        };
        const int modCount = (int)mods.size();
        const int rowCount = (modCount + kCols - 1) / kCols;
        float total = 0.0f;
        for (int r = 0; r < rowCount; ++r) {
            float rh = 0.0f;
            for (int k = 0; k < kCols && r * kCols + k < modCount; ++k)
                rh = std::max(rh, slotH(mods[r * kCols + k]));
            total += rh + cardGap;
        }
        total = std::max(0.0f, total - cardGap);

        const float viewH = content.w - content.y - searchLineH;
        const float maxScroll = std::max(0.0f, total - viewH);
        if (c.live && over(content)) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) g_scrollTarget -= wheel * 60.0f;
        }
        g_scrollTarget = std::clamp(g_scrollTarget, 0.0f, maxScroll);
        g_scroll = MathUtils::animate(g_scrollTarget, g_scroll, std::clamp(dt * 14.0f, 0.0f, 1.0f));

        float y = content.y - g_scroll + contentSlide + searchLineH;
        for (int r = 0; r < rowCount; ++r) {
            float rowH = 0.0f;
            for (int k = 0; k < kCols; ++k) {
                const int idx = r * kCols + k;
                if (idx >= modCount) break;
                rowH = std::max(rowH, slotH(mods[idx]) + cardGap * easeOutCubic(mods[idx]->searchAnim));
            }
            for (int k = 0; k < kCols; ++k) {
                const int idx = r * kCols + k;
                if (idx >= modCount) break;
                Module* m = mods[idx];
                const float sa = easeOutCubic(m->searchAnim);
                const float ch = slotH(m);
                const float x = content.x + k * (cardW + cardGap);
                const ImVec4 box(x, y, x + cardW, y + ch);
                if (box.w > content.y - 4.0f && box.y < content.w + 4.0f && ch > 1.0f) {
                    std::string name = m->GetName();
                    if (mCasing->mValue == 1) name = lowered(name);

                    Ctx sc = cc;
                    sc.a = cc.a * sa;
                    sc.live = cc.live && sa > 0.85f;   // no clicks on a card mid-exit
                    sc.lmb = sc.live && cc.lmb;
                    sc.rmb = sc.live && cc.rmb;
                    const float slide = (1.0f - sa) * 26.0f * scale;
                    const ImVec4 slot(box.x + slide, box.y, box.z + slide, box.w);
                    dl->PushClipRect(ImVec2(x - 6.0f * scale, box.y),
                                     ImVec2(x + cardW + 6.0f * scale, box.w), true);
                    drawCard(sc, m, slot, name);
                    dl->PopClipRect();
                }
            }
            y += rowH;
        }

        if (matchCount == 0) {
            const std::string msg = g_search[0] ? "No modules match that search."
                                                : "This category is empty.";
            text(dl, c.font, 14.5f * scale, ImVec2(content.x + 2.0f, content.y + 10.0f * scale + searchLineH),
                 msg, kTextLo, cc.a, false);
        }

        // Scrollbar, only while it has something to indicate.
        if (maxScroll > 1.0f) {
            const float trackX = content.z + 3.0f * scale;
            const float frac = viewH / std::max(viewH, total);
            const float thumbH = std::max(28.0f * scale, viewH * frac);
            const float t = g_scroll / maxScroll;
            const float thumbY = content.y + (viewH - thumbH) * t;
            fill(dl, ImVec4(trackX, content.y, trackX + 3.0f * scale, content.w),
                 ImVec4(1, 1, 1, 1), 0.04f * c.a, 1.5f * scale);
            fill(dl, ImVec4(trackX, thumbY, trackX + 3.0f * scale, thumbY + thumbH),
                 ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f),
                 0.55f * c.a, 1.5f * scale);
        }
    } else {
        static float bodyHeight[kExtraCount + 1] = {};
        const float viewH = content.w - content.y;
        const float maxScroll = std::max(0.0f, bodyHeight[g_tab] - viewH);
        if (c.live && over(content)) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) g_scrollTarget -= wheel * 60.0f;
        }
        g_scrollTarget = std::clamp(g_scrollTarget, 0.0f, maxScroll);
        g_scroll = MathUtils::animate(g_scrollTarget, g_scroll, std::clamp(dt * 14.0f, 0.0f, 1.0f));

        const ImVec4 area(content.x, content.y - g_scroll + contentSlide, content.z, content.w);
        if (g_tab == 1) {
            UpdateSettingsVisibility();
            tickAnims(this, dt, /*bodyOpen=*/true);
            bodyHeight[1] = drawSettingsBody(cc, this, area);
        } else if (g_tab == 2) {
            bodyHeight[2] = drawConfigsBody(cc, area);
        } else if (g_tab == 3) {
            UpdateSettingsVisibility();
            tickAnims(this, dt, /*bodyOpen=*/true);
            bodyHeight[3] = drawThemesBody(cc, area);
        } else {
            bodyHeight[4] = drawChangelogBody(cc, area);
        }

        if (maxScroll > 1.0f) {
            const float trackX = content.z + 3.0f * scale;
            const float frac = viewH / std::max(viewH, bodyHeight[g_tab]);
            const float thumbH = std::max(28.0f * scale, viewH * frac);
            const float t = g_scroll / maxScroll;
            const float thumbY = content.y + (viewH - thumbH) * t;
            fill(dl, ImVec4(trackX, content.y, trackX + 3.0f * scale, content.w),
                 ImVec4(1, 1, 1, 1), 0.04f * c.a, 1.5f * scale);
            fill(dl, ImVec4(trackX, thumbY, trackX + 3.0f * scale, thumbY + thumbH),
                 ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f),
                 0.55f * c.a, 1.5f * scale);
        }
    }
    dl->PopClipRect();

    } // Panel / Columns mode branch

    if (g_binding || g_keyTarget) {
        // One frame of grace so the click that armed the row is not itself
        // captured as the key.
        static const void* armed = nullptr;
        static bool skip = true;
        const void* now = g_binding ? static_cast<const void*>(g_binding)
                                    : static_cast<const void*>(g_keyTarget);
        if (armed != now) { armed = now; skip = true; }
        if (skip) {
            skip = false;
        } else {
            for (int k = 3; k < 255; ++k) {
                if (!(GetAsyncKeyState(k) & 0x8000)) continue;
                // Mouse buttons are how the row was armed in the first place, and
                // they are needed to click elsewhere; only Escape cancels.
                if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON) continue;
                const int value = (k == VK_ESCAPE) ? 0 : k;
                if (g_binding) g_binding->SetKeybind(value);
                else if (g_keyTarget) *g_keyTarget = value;
                g_binding = nullptr;
                g_keyTarget = nullptr;
                armed = nullptr;
                skip = true;
                break;
            }
        }
    }

    // --- string editor modal --------------------------------------------------
    if (g_strEdit) {
        auto* ss = static_cast<StringSetting*>(static_cast<Setting*>(g_strEdit));
        const float pw = 420.0f * scale, ph = 140.0f * scale;
        const float px = (screen.x - pw) * 0.5f, py = (screen.y - ph) * 0.5f;
        const ImVec4 box(px, py, px + pw, py + ph);

        dl->AddRectFilled(ImVec2(0, 0), ImVec2(screen.x, screen.y),
                          ImGui::GetColorU32(ImVec4(0, 0, 0, 0.45f)));
        ImRenderUtils::fillShadowRectangle(box, ImColor(0.0f, 0.0f, 0.0f, 1.0f), 0.9f,
                                           14.0f * scale, 30.0f * scale, 0.95f, dl);
        fill(dl, box, kPanel, 0.99f, 14.0f * scale);
        stroke(dl, box, ImVec4(1, 1, 1, 1), 0.12f, 14.0f * scale, 1.0f);
        fill(dl, ImVec4(box.x, box.y, box.z, box.y + 3.0f * scale),
             ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f), 1.0f,
             14.0f * scale, ImDrawFlags_RoundCornersTop);

        text(dl, c.bold, 17.0f * scale, ImVec2(px + 20.0f * scale, py + 18.0f * scale),
             "Edit " + ss->mName, kTextHi, 1.0f, true);

        const float bx = px + 20.0f * scale, by = py + 56.0f * scale;
        const float bw = pw - 40.0f * scale, bh = 34.0f * scale;
        fill(dl, ImVec4(bx, by, bx + bw, by + bh), ImVec4(1, 1, 1, 1), 0.07f, 8.0f * scale);
        stroke(dl, ImVec4(bx, by, bx + bw, by + bh),
               ImVec4(c.accent.Value.x, c.accent.Value.y, c.accent.Value.z, 1.0f), 0.5f,
               8.0f * scale, 1.0f);

        const ScopedGuiPointer pointer(on);
        ImGui::SetNextWindowPos(ImVec2(bx, by));
        ImGui::SetNextWindowSize(ImVec2(bw, bh));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(kTextHi.x, kTextHi.y, kTextHi.z, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f, 9.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::Begin("##SunsteadStrEdit", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        const UiDrawRange editorRange(ImGui::GetWindowDrawList());
        char buf[256];
        strncpy_s(buf, ss->mValue.c_str(), sizeof(buf) - 1);
        ImGui::PushItemWidth(bw - 18.0f);
        if (!g_strFocus) { ImGui::SetKeyboardFocusHere(); g_strFocus = true; }
        const bool submitted = ImGui::InputText("##v", buf, sizeof(buf),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        ss->mValue = buf;
        ImGui::PopItemWidth();
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
        editorRange.finish(g_uiTransform.fade);

        text(dl, c.font, 12.5f * scale,
             ImVec2(px + 20.0f * scale, py + ph - 26.0f * scale),
             std::string("Enter or Esc to close"), kTextLo, 1.0f, false);

        if (submitted || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            g_strEdit = nullptr;
            g_strFocus = false;
        }
    }

    dl->PopClipRect();
    mainRange.finish(alpha);

    if (g_tipShow && !g_tip.empty() && alpha > 0.35f) {
        const float tipA = 1.0f;
        static ImVec2      s_tipPos(-1.0f, -1.0f);  // eased position
        static std::string s_tipText;               // last rendered text
        static float       s_tipPop = 1.0f;         // 0..1 appear progress
        static std::string s_dwellText;
        static float       s_dwell = 0.0f;
        if (g_tip == s_dwellText) {
            s_dwell += dt;
        } else {
            s_dwellText = g_tip;
            s_dwell = 0.0f;
        }
        if (s_dwell < 0.25f) return;

        if (g_tip != s_tipText) { s_tipText = g_tip; s_tipPop = 0.0f; }
        s_tipPop += (1.0f - s_tipPop) * std::clamp(dt * 15.0f, 0.0f, 1.0f);
        if (s_tipPop > 0.995f) s_tipPop = 1.0f;

        const float pop = easeOutCubic(s_tipPop);

        const ImVec2 target(g_tipPos.x, g_tipPos.y);
        // Snap on the first frame or after a long jump (the cursor teleported
        // between distant rows) — gliding across the whole screen reads as lag.
        const float jump = std::hypot(target.x - s_tipPos.x, target.y - s_tipPos.y);
        if (s_tipPos.x < -0.5f || jump > 240.0f) s_tipPos = target;
        const float chase = std::clamp(dt * 18.0f, 0.0f, 1.0f);
        s_tipPos.x += (target.x - s_tipPos.x) * chase;
        s_tipPos.y += (target.y - s_tipPos.y) * chase;

        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const UiDrawRange fgRange(fg);
        const float fs = 13.0f * scale;
        const float tw = measure(c.font, fs, g_tip) + 18.0f * scale;
        const float th = fs + 14.0f * scale;

        float tx = s_tipPos.x, ty = s_tipPos.y + (1.0f - pop) * 3.0f * scale;
        if (tx + tw > screen.x - 8.0f) tx = screen.x - tw - 8.0f;
        if (ty + th > screen.y - 8.0f) ty = screen.y - th - 8.0f;

        const float sc = 0.96f + 0.04f * pop;
        const float sw = tw * sc, sh = th * sc;
        const ImVec4 tip(tx + (tw - sw) * 0.5f, ty + (th - sh) * 0.5f,
                         tx + (tw + sw) * 0.5f, ty + (th + sh) * 0.5f);
        const float tAlpha = tipA * std::clamp(s_tipPop * 2.0f, 0.0f, 1.0f);
        fill(fg, tip, ImVec4(0.070f, 0.075f, 0.098f, 1.0f), 0.80f * tAlpha, 0.0f);
        stroke(fg, tip, ImVec4(1, 1, 1, 1), 0.06f * tAlpha, 0.0f, 1.0f);
        text(fg, c.font, fs,
             ImVec2((tip.x + tip.z) * 0.5f - measure(c.font, fs, g_tip) * 0.5f,
                    (tip.y + tip.w) * 0.5f - fs * 0.5f),
             g_tip, kTextHi, tAlpha, false);
        fgRange.finish(alpha);
    }
}
