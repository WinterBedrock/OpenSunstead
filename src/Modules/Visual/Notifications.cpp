#include "Notifications.h"
#include "../ModuleManager.h"
#include "Chat.h"
#include "../../SDK/Client/ScreenView.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include <imgui.h>
#include <Windows.h>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Active cards. Several notifications may be on screen at once (the previous
// one-at-a-time queue felt dead): each card lives its duration, new cards
// enter from the bottom and push the stack up, expired cards rise out with a
// fade. Rise's EASE_OUT_EXPO entry zoom is kept per card.
// ---------------------------------------------------------------------------
namespace {

// Pending-queue ceiling. The old Rise EvictingList(5) FORCED a cap of 5 even
// with the Nova limit off (extras were silently dropped, which read as "the
// limit never goes away"). Now the queue is wide enough that the on-screen
// STACK is the only real gate, and that gate is exactly the Nova Limit.
constexpr size_t kMaxPending = 32;

struct ActiveCard {
    std::string title;
    std::string description;
    NotifType type = NotifType::Info;
    unsigned long long startMs = 0;
    unsigned long long lifeMs = 0;
    float anim = 0.f;        // eased entry zoom
    bool exiting = false;
    float exitT = 0.f;       // 0..1 exit progress
    float slotY = -100000.f; // Nova: smoothed stack-slot Y (spring glides)
};

// The TargetHUD-style example card: while an editor surface (pause, chat,
// OUR custom chat menu) is open with nothing to show, the placeholder pops
// in with the entry zoom, HOLDS for as long as the surface is open, and only
// ~1s after it closes plays the exit animation (rise + fade) — the exact
// linger TargetHUD's menuHold uses. No cyclic pop/hold/fade repeat.
struct ExampleCard {
    enum class State { Hidden, In, Hold, Out };
    State state = State::Hidden;
    unsigned long long t0 = 0;
    float anim = 0.f;     // eased entry zoom 0..1
    float exitT = 0.f;    // 0..1 exit progress
};
ExampleCard& exampleCard() {
    static ExampleCard e;
    return e;
}

// 1s linger after the menu surface closes (TargetHUD menuHold semantics).
float& menuHold() {
    static float h = 0.f;
    return h;
}

std::vector<ActiveCard>& cards() {
    static std::vector<ActiveCard> c;
    return c;
}

float easeOutExpo(float x) {
    x = std::clamp(x, 0.f, 1.f);
    return (x >= 0.999f) ? 1.f : 1.f - std::pow(2.0f, -10.0f * x);
}

// Bounce-in curve for the Nova slide: slight overshoot past 1 and back.
float easeOutBack(float x) {
    x = std::clamp(x, 0.f, 1.f);
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    return 1.0f + c3 * std::pow(x - 1.0f, 3.0f) + c1 * std::pow(x - 1.0f, 2.0f);
}

float easeOutCubic(float x) {
    x = std::clamp(x, 0.f, 1.f);
    return 1.0f - std::pow(1.0f - x, 3.0f);
}

} // namespace

// The pending queue (header declares the static member accessor).
std::deque<Notifications::Item>& Notifications::queue() {
    static std::deque<Item> q;
    return q;
}

void Notifications::AddNotification(const std::string& text, NotifType type) {
    AddNotification(text, "", type);
}

void Notifications::AddNotification(const std::string& title, const std::string& description,
                                    NotifType type) {
    auto& q = Notifications::queue();
    q.push_back({title, description, type});
    if (q.size() > kMaxPending) q.pop_front();   // sanity ceiling only
}

void Notifications::OnRender() {
    if (!IsEnabled()) return;

    // ---- settings (read by name so the ctor order is free) ----------------
    int   style = 0;   // 0 = Stead (classic), 1 = Nova (modern fixed preset)
    float scaleSet = 1.0f, duration = 3.0f, bgOpacity = 0.85f;
    float dimSet = 0.25f, blurSet = 3.0f, roundingSet = 10.0f, shadowA = 0.5f;
    bool  shadowOn = false;
    float novaBlur = 6.0f, novaDim = 0.35f, novaMax = 3.0f;
    bool  novaLimit = false;
    for (auto* s : mSettings) {
        if (!s) continue;
        if      (s->mName == "Style")            style       = static_cast<EnumSetting*>(s)->mValue;
        else if (s->mName == "Scale")            scaleSet    = static_cast<NumberSetting*>(s)->mValue;
        else if (s->mName == "Duration")         duration    = static_cast<NumberSetting*>(s)->mValue;
        else if (s->mName == "Opacity")          bgOpacity   = static_cast<NumberSetting*>(s)->mValue;
        else if (s->mName == "Dim")              dimSet      = static_cast<NumberSetting*>(s)->mValue;
        else if (s->mName == "Blur")             blurSet     = static_cast<NumberSetting*>(s)->mValue;
        else if (s->mName == "Rounding")         roundingSet = static_cast<NumberSetting*>(s)->mValue;
        else if (s->mName == "Shadow")          shadowOn    = static_cast<BoolSetting*>(s)->mValue;
        else if (s->mName == "Shadow Strength") shadowA      = static_cast<NumberSetting*>(s)->mValue;
        else if (s->mName == "Nova Blur")       novaBlur    = static_cast<NumberSetting*>(s)->mValue;
        else if (s->mName == "Nova Dim")        novaDim     = static_cast<NumberSetting*>(s)->mValue;
        else if (s->mName == "Nova Limit")      novaLimit   = static_cast<BoolSetting*>(s)->mValue;
        else if (s->mName == "Nova Max")        novaMax     = static_cast<NumberSetting*>(s)->mValue;
    }
    // Nova stack cap: with "Nova Limit" OFF there is NO cap at all — the
    // stack grows as far as the pending-queue ceiling allows (kMaxPending),
    // so turning the limit off genuinely removes it. With it ON, the
    // on-screen stack clamps to the configured maximum.
    const size_t stackCap = (style == 1 && novaLimit)
        ? (size_t)std::clamp(novaMax, 1.0f, 10.0f) : kMaxPending;

    // HUD-editor scroll scales the card too (mScale is the editor's knob).
    const float scale = scaleSet * mScale;
    if (scale <= 0.01f) return;

    const unsigned long long now = GetTickCount64();
    const unsigned long long lifeMs = (unsigned long long)(duration * 1000.0f);

    // ---- advance the stack --------------------------------------------------
    auto& q = queue();
    auto& active = cards();

    // Expired -> exit anim; finished exits drop out.
    for (auto it = active.begin(); it != active.end();) {
        const float t = (float)(now - it->startMs);
        if (!it->exiting) {
            it->anim = easeOutExpo(t / 500.0f);
            if (t >= (float)it->lifeMs) it->exiting = true;
        }
        if (it->exiting) {
            it->exitT = std::clamp((t - (float)it->lifeMs) / 250.0f, 0.f, 1.f);
            if (it->exitT >= 1.f) it = active.erase(it);
            else ++it;
        } else {
            ++it;
        }
    }
    // Admit queued cards while there is room (stackCap = 5, or the Nova
    // "Nova Limit" maximum while it is on).
    while (!q.empty() && active.size() < stackCap) {
        ActiveCard card;
        card.title = q.front().title;
        card.description = q.front().description;
        card.type = q.front().type;
        q.pop_front();
        card.startMs = now;
        card.lifeMs = lifeMs;
        card.anim = 0.f;
        card.exiting = false;
        card.exitT = 0.f;
        active.push_back(std::move(card));
    }

    const std::string& screenName = SDK::ScreenView::currentScreenName();
    // The example card shows on EVERY editor surface: the pause and (vanilla)
    // chat screens, plus OUR custom chat menu (which keeps the hud_screen but
    // opens a UI surface).
    const bool menuSurface = (screenName == "pause_screen" || screenName == "chat_screen" ||
                              (screenName == "hud_screen" && ::Chat::IsOpen()));

    // menuHold: 1s linger after the surface closes, TargetHUD-style — the
    // example (and the self-view TargetHUD) keep animating OUT after leaving.
    {
        float dt0 = ImGui::GetIO().DeltaTime;
        if (dt0 <= 0.0f) dt0 = 1.0f / 60.0f;
        if (dt0 > 0.1f) dt0 = 0.1f;
        if (menuSurface) menuHold() = 1.0f;
        else if (menuHold() > 0.0f) menuHold() -= dt0;
    }

    // ---- example card state machine (in -> hold -> out) -------------------
    auto& ex = exampleCard();
    const unsigned long long exNow = now;
    if (ex.state == ExampleCard::State::Hidden) {
        if (menuSurface && active.empty()) {
            ex.state = ExampleCard::State::In;
            ex.t0 = exNow;
            ex.anim = 0.f;
            ex.exitT = 0.f;
        }
    } else if (ex.state == ExampleCard::State::In) {
        ex.anim = easeOutExpo((float)(exNow - ex.t0) / 500.0f);
        if (ex.anim >= 1.f) ex.state = ExampleCard::State::Hold;
        if (!menuSurface || !active.empty()) { ex.state = ExampleCard::State::Out; ex.t0 = exNow; }
    } else if (ex.state == ExampleCard::State::Hold) {
        ex.anim = 1.f;
        if (!menuSurface || !active.empty()) { ex.state = ExampleCard::State::Out; ex.t0 = exNow; }
    } else if (ex.state == ExampleCard::State::Out) {
        ex.exitT = std::clamp((float)(exNow - ex.t0) / 300.0f, 0.f, 1.f);
        if (ex.exitT >= 1.f) ex.state = ExampleCard::State::Hidden;
        // Re-open immediately if the surface comes back mid-exit.
        if (menuSurface && active.empty() && ex.exitT < 1.f) {
            ex.state = ExampleCard::State::In;
            ex.t0 = exNow;
            ex.exitT = 0.f;
        }
    }
    const bool example = (ex.state != ExampleCard::State::Hidden);
    float exampleAlpha = 0.f;
    if (example) {
        // Entry: center-pivot zoom (EASE_OUT_EXPO 500ms). Hold: full. Exit:
        // rise + fade over 300ms (the same language as real-card exits).
        if (ex.state == ExampleCard::State::Out)
            exampleAlpha = 1.f - ex.exitT;
        else
            exampleAlpha = ex.anim;
    }
    if (active.empty() && !example) {
        mWidth = 0.f; mHeight = 0.f;
        return;
    }

    // =========================================================================
    // Nova preset: flat SQUARE toasts pinned to the BOTTOM-RIGHT corner.
    // Sharp corners (zero rounding everywhere), an adjustable frosted backdrop
    // (Nova Blur + Nova Dim) behind each card, and a themed countdown strip
    // along the bottom edge whose width tracks the card's remaining life,
    // carrying a soft themed glow above it. Toggle cards are TWO lines: the
    // module name in the live theme colour on top, and below it the module
    // name in white with the green/red status word. Cards spring in from the
    // right with a small scale-up, the stack glides to make room (per-card
    // smoothed Y), and exits shrink + slide back out right.
    // =========================================================================
    if (style == 1) {
        ImVec2 screen = ImRenderUtils::getScreenSize();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        if (!dl) return;

        const float fontSize = 15.0f * scale;
        const float padX     = 12.0f * scale;
        const float gap      = 8.0f  * scale;
        const float margin   = 16.0f * scale;

        // Square silhouette: Nova draws with ZERO rounding everywhere.
        constexpr float round = 0.0f;

        // Frosted backdrop: blur + dim paint OPAQUE rects onto the backbuffer
        // at replay time, so they go on the BACKGROUND list, which is replayed
        // before the foreground plates (the same ordering rule Stead and
        // TargetHUD follow). Skipped while the ClickGUI already blurs the
        // whole screen.
        bool guiOpen = false;
        if (Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI"))
            guiOpen = gui->IsEnabled();
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        auto drawBackdrop = [&](const ImVec4& r, float a) {
            if (!bg || guiOpen || a <= 0.01f) return;
            if (novaBlur > 0.1f)
                D2D::addBlurOptimized(bg, novaBlur, std::optional<ImVec4>(r), 0.0f);
            if (novaDim > 0.01f)
                bg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.z, r.w),
                                  ImGui::GetColorU32(ImVec4(0, 0, 0, novaDim)), 0.0f);
        };

        // Toggle cards carry the module NAME as the title; the type says which
        // way it went, and drives the status word + its colour.
        auto isToggle = [](NotifType t) {
            return t == NotifType::ToggleOn || t == NotifType::ToggleOff;
        };
        auto statusWord = [](NotifType t) {
            return t == NotifType::ToggleOn ? "Enabled" : "Disabled";
        };

        // Two heights: a slim single-line toast, a taller two-line card for
        // anything with a note — which toggle cards always have (name+status).
        const float hShort = 38.0f * scale, hTall = 52.0f * scale;
        auto cardH = [&](NotifType t, const std::string& desc) {
            return (!desc.empty() || isToggle(t)) ? hTall : hShort;
        };
        auto cardW = [&](NotifType t, const std::string& title, const std::string& desc) {
            float tw = ImRenderUtils::getTextWidth(&title, fontSize);
            float dw = 0.0f;
            if (isToggle(t)) {
                const float fs2 = fontSize * 0.85f;
                const std::string sw = statusWord(t);
                dw = ImRenderUtils::getTextWidth(&title, fs2) + 5.0f * scale +
                     ImRenderUtils::getTextWidth(&sw, fs2);
            } else if (!desc.empty()) {
                dw = ImRenderUtils::getTextWidth(&desc, fontSize * 0.85f);
            }
            return std::max(150.0f * scale, std::max(tw, dw) + padX * 2.0f);
        };

        // Draws one card. `offRight` slides the card right toward/past the
        // screen edge during slides; `scl` scales the whole card around its
        // own center (entry pop / exit shrink); `a` is the global alpha;
        // `remain` (0..1) drives the countdown strip's width.
        auto drawNovaCard = [&](float right, float bottom, float width, float hgt,
                                NotifType type, const std::string& title,
                                const std::string& desc,
                                float a, float offRight, float scl, float remain) {
            if (a <= 0.003f || hgt <= 0.0f || scl <= 0.01f) return;
            const float rx = right + offRight - width, ry = bottom - hgt;
            const float cxc = rx + width * 0.5f, cyc = ry + hgt * 0.5f;
            auto t2 = [&](ImVec2 p) { return ImVec2(cxc + (p.x - cxc) * scl, cyc + (p.y - cyc) * scl); };
            auto rectT = [&](float x0, float y0, float x1, float y1) {
                ImVec2 p0 = t2(ImVec2(x0, y0)), p1 = t2(ImVec2(x1, y1));
                return ImVec4(p0.x, p0.y, p1.x, p1.y);
            };

            // Frosted backdrop under the plate (blur + dim, Nova knobs).
            const ImVec4 r = rectT(rx, ry, rx + width, ry + hgt);
            drawBackdrop(r, a);

            // Plate: barely-there dark glass with a whisper of shadow.
            ImRenderUtils::fillShadowRectangle(r, ImColor(0, 0, 0),
                                               0.25f * a, round, 10.0f * scale, 0.4f, dl);
            ImRenderUtils::fillRectangle(r, ImColor(14, 14, 18, (int)(120.f * a)),
                                         1.0f, round, dl);

            // Top line = title in the live theme colour. Second line = the
            // grey note, or (toggles) the module name in white with the
            // green/red status word after it.
            const ImColor themed = ColorUtils::getThemedColor(0.0f, now);
            const ImVec2 cyPt = t2(ImVec2(cxc, cyc));
            const float tx = t2(ImVec2(rx + padX, cyc)).x;
            if (!isToggle(type) && desc.empty()) {
                ImRenderUtils::drawText(ImVec2(tx, cyPt.y - fontSize * 0.55f * scl), title,
                                        themed, fontSize * scl, a, true, dl);
            } else {
                ImRenderUtils::drawText(ImVec2(tx, cyPt.y - fontSize * 1.02f * scl), title,
                                        themed, fontSize * scl, a, true, dl);
                if (isToggle(type)) {
                    const float fs2 = fontSize * 0.85f * scl;
                    const ImVec2 p2(tx, cyPt.y + fontSize * 0.18f * scl);
                    ImRenderUtils::drawText(p2, title, ImColor(255, 255, 255),
                                            fs2, 0.92f * a, true, dl);
                    const float tw = ImRenderUtils::getTextWidth(&title, fs2);
                    const ImColor stCol = (type == NotifType::ToggleOn)
                        ? ImColor(107, 224, 143) : ImColor(242, 107, 107);
                    ImRenderUtils::drawText(ImVec2(p2.x + tw + 5.0f * scale, p2.y),
                                            statusWord(type), stCol, fs2, 0.92f * a, true, dl);
                } else {
                    ImRenderUtils::drawText(ImVec2(tx, cyPt.y + fontSize * 0.18f * scl), desc,
                                            ImColor(225, 225, 230), fontSize * 0.85f * scl,
                                            0.92f * a, true, dl);
                }
            }

            // Countdown strip: a theme gradient along the bottom edge whose
            // width tracks the card's remaining life, with a soft themed glow
            // (a vertical fade of the same gradient) rising above it.
            const float remainC = std::clamp(remain, 0.0f, 1.0f);
            if (remainC > 0.004f) {
                const float barH = std::max(2.5f * scale, 1.5f);
                const float barW = (r.z - r.x) * remainC;
                if (barW > 1.0f) {
                    const float period = ColorUtils::getThemedPeriod();
                    const ImColor c0 = ColorUtils::getThemedColor(0.0f, now);
                    const ImColor c1 = ColorUtils::getThemedColor(period * 0.25f, now);
                    const float glowH = 7.0f * scale;
                    ImRenderUtils::fillGradientRounded(dl,
                        ImVec4(r.x, r.w - barH - glowH, r.x + barW, r.w - barH),
                        ImVec4(c0.Value.x, c0.Value.y, c0.Value.z, 0.0f),
                        ImVec4(c1.Value.x, c1.Value.y, c1.Value.z, 0.45f * a),
                        0.0f, 0, true);
                    ImRenderUtils::fillGradientRounded(dl,
                        ImVec4(r.x, r.w - barH, r.x + barW, r.w),
                        ImVec4(c0.Value.x, c0.Value.y, c0.Value.z, a),
                        ImVec4(c1.Value.x, c1.Value.y, c1.Value.z, a),
                        0.0f, 0, false);
                }
            }
        };

        // Entry: spring in from the right + a light 0.94 -> 1 scale-up.
        auto entryOff = [&](float t01, float w) {
            const float e = easeOutBack(t01);
            return (1.0f - e) * (w + margin + 20.0f * scale);
        };
        auto entryScl = [&](float t01) {
            return 0.94f + 0.06f * std::clamp(t01 * 1.15f, 0.0f, 1.0f);
        };
        // Exit: shrink to 0.88 + accelerate back out right + fade.
        auto exitOff = [&](float t01, float w) {
            const float e = t01 * t01 * t01;
            return e * (w + margin + 20.0f * scale);
        };
        auto exitScl = [&](float t01) {
            return 1.0f - 0.12f * easeOutCubic(t01);
        };
        const float rightEdge = screen.x - margin;

        // ---- example card (same in->hold->out state machine, Nova skin).
        //      It HOLDS while the surface is open, so its strip stays full. ----
        if (example) {
            const float w = cardW(NotifType::Info, "Example", "Notification preview");
            const float h = cardH(NotifType::Info, "Notification preview");
            float a, off, scl;
            if (ex.state == ExampleCard::State::Out) {
                a = 1.0f - ex.exitT;
                off = exitOff(ex.exitT, w);
                scl = exitScl(ex.exitT);
            } else {
                const float t01 = std::clamp(ex.anim, 0.0f, 1.0f);
                a = t01;
                off = entryOff(t01, w);
                scl = entryScl(t01);
            }
            drawNovaCard(rightEdge, screen.y - margin, w, h, NotifType::Info,
                         "Example", "Notification preview", a, off, scl, 1.0f);
            mWidth = a > 0.02f ? w : 0.f;
            mHeight = a > 0.02f ? h : 0.f;
            // Pin the HUD-editor anchor to the fixed corner so its outline
            // matches where the card actually renders.
            mPosX = rightEdge - w;
            mPosY = screen.y - margin - h;
            return;
        }

        // ---- the stack: newest card at the BOTTOM of a bottom-right stack.
        //      Each card keeps a SMOOTHED slot Y (a spring lerp toward its
        //      target), so a new arrival glides the older cards up smoothly
        //      instead of popping them into their new slots. ----
        float dt = ImGui::GetIO().DeltaTime;
        if (dt <= 0.0f) dt = 1.0f / 60.0f;
        const float yLerp = 1.0f - std::exp(-dt * 16.0f);

        const size_t n = active.size();
        // Measure every slot first, so the layout targets are known before a
        // card starts its entry slide.
        float yCursor = screen.y - margin;
        float maxW = 0.f;
        std::vector<float> targets(n, 0.0f), widths(n, 0.0f), heights(n, 0.0f);
        for (size_t ii = 0; ii < n; ++ii) {
            const size_t i = n - 1 - ii;             // newest (last) first
            widths[i] = cardW(active[i].type, active[i].title, active[i].description);
            heights[i] = cardH(active[i].type, active[i].description);
            targets[i] = yCursor - heights[i];       // slot top edge
            yCursor -= heights[i] + gap;
            maxW = std::max(maxW, widths[i]);
        }

        for (size_t i = 0; i < n; ++i) {
            ActiveCard& card = active[i];
            const float t = (float)(now - card.startMs);
            // Fresh cards spawn one slot BELOW their target and glide into it.
            if (card.slotY < -90000.0f) card.slotY = targets[i] + heights[i] + gap;
            card.slotY += (targets[i] - card.slotY) * yLerp;

            float a, off, scl, remain;
            if (card.exiting) {
                const float e01 = std::clamp(card.exitT, 0.0f, 1.0f);
                a = 1.0f - e01;
                off = exitOff(e01, widths[i]);
                scl = exitScl(e01);
                remain = 0.0f;
            } else {
                // Nova drives its own entry curves off the raw entry clock
                // (the shared state machine eases over 500ms).
                const float t01 = std::clamp(t / 420.0f, 0.0f, 1.0f);
                a = std::clamp(card.anim, 0.0f, 1.0f);
                off = entryOff(t01, widths[i]);
                scl = entryScl(t01);
                // Countdown: the fraction of the card's life still ahead of it.
                remain = 1.0f - std::clamp(t / (float)card.lifeMs, 0.0f, 1.0f);
            }
            drawNovaCard(rightEdge, card.slotY + heights[i], widths[i], heights[i],
                         card.type, card.title, card.description, a, off, scl, remain);
        }

        mWidth = maxW;
        mHeight = (screen.y - margin) - (yCursor + gap);
        mPosX = rightEdge - maxW;
        mPosY = yCursor + gap;
        return;
    }

    // ---- geometry (Rise metrics, scaled) ------------------------------------
    const float fontSize = 15.0f * scale;
    const float spacer   = 5.0f  * scale;
    const float iconSide = 20.0f * scale;
    const float height   = 30.0f * scale;
    const float margin   = 5.0f  * scale;
    // Default band anchor: just BELOW the Watermark (which occupies roughly the
    // top 46px at default scale), so the two never overlap.
    const float bandY    = 64.0f * scale;
    const float gap      = 8.0f  * scale;

    // Measure per card (auto width, min 140 — Rise).
    auto cardWidth = [&](const std::string& title, const std::string& desc) {
        float titleW = ImRenderUtils::getTextWidth(&title, fontSize);
        float descW  = desc.empty() ? 0.0f : ImRenderUtils::getTextWidth(&desc, fontSize);
        float contentW = std::max(titleW, descW);
        return std::max(140.0f * scale, contentW + spacer * 3.0f + iconSide + 2.0f * scale);
    };

    const std::string exTitle = "Example";
    const std::string exDesc  = "Notification preview";
    const float exWidth = example ? cardWidth(exTitle, exDesc) : 0.f;

    ImVec2 screen = ImRenderUtils::getScreenSize();

    // HUD-editor position; default = the band anchor. "Unset" is the (0,0)
    // sentinel OR the stock Module default (10,10) — configs saved the stock
    // default verbatim before the ctor sentinel existed, and both meant
    // "never positioned", spawning the card ON the Watermark.
    const bool unsetPos = (mPosX <= 0.f && mPosY <= 0.f) ||
                          (mPosX == 10.f && mPosY == 10.f);
    if (unsetPos) {
        mPosX = margin;
        mPosY = bandY;
    }
    const float x = mPosX, y = mPosY;

    // Backdrop helper: per-card frosted backdrop (blur + dim) on the
    // BACKGROUND list — an opaque replay must precede the foreground plates
    // (the same ordering rule TargetHUD follows). Skipped while the ClickGUI
    // already blurs the whole screen.
    const float cardRound = roundingSet * scale;
    bool guiOpen = false;
    if (Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI"))
        guiOpen = gui->IsEnabled();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    auto drawBackdrop = [&](const ImVec4& r) {
        if (!bg || guiOpen) return;
        if (blurSet > 0.1f)
            D2D::addBlurOptimized(bg, blurSet, std::optional<ImVec4>(r), cardRound);
        if (dimSet > 0.01f)
            bg->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.z, r.w),
                              ImGui::GetColorU32(ImVec4(0, 0, 0, dimSet)), cardRound);
    };

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    // ---- example card (pop-in -> hold -> rise/fade out) --------------------
    if (example) {
        const float a = exampleAlpha;
        const float d = std::clamp(ex.anim, 0.f, 1.f);
        const float rise = ex.exitT * height * 0.7f;   // exit: rises like real cards
        const float ecx = x + exWidth * 0.5f, ecy = y + height * 0.5f - rise;
        auto epx = [&](float wx, float wy) {
            return ImVec2(ecx + (wx - ecx) * d, ecy + (wy - ecy) * d);
        };
        ImVec2 ep1 = epx(x, y - rise), ep2 = epx(x + exWidth, y + height - rise);
        const ImVec4 rect(ep1.x, ep1.y, ep2.x, ep2.y);
        drawBackdrop(rect);
        if (shadowOn)
            ImRenderUtils::fillShadowRectangle(rect, ImColor(0, 0, 0),
                                               shadowA * a, 10.f * scale, 14.f * scale, 0.6f, dl);
        ImRenderUtils::fillRectangle(rect, ImColor(12, 12, 15, (int)(255.f * 0.85f * a)), 1.f, 10.f * scale, dl);
        dl->AddRect(ImVec2(rect.x, rect.y), ImVec2(rect.z, rect.w),
                    ImColor(255, 255, 255, (int)(28.f * a)), 10.f * scale, 0, 1.f);
        ImRenderUtils::fillRectangle(ImVec4(rect.x + spacer, rect.y + spacer,
                                            rect.x + spacer + iconSide, rect.y + spacer + iconSide),
                                     ImColor(255, 255, 255, (int)(235.f * a)), 1.f, 6.f * scale, dl);
        ImRenderUtils::drawText(ImVec2(rect.x + spacer + iconSide + spacer, rect.y + spacer - 1.f * scale),
                                 exTitle, ImColor(255, 255, 255), fontSize, 0.92f * a, true, dl);
        ImRenderUtils::drawText(ImVec2(rect.x + spacer + iconSide + spacer,
                                       rect.y + spacer - 1.f * scale + fontSize * 0.85f),
                                exDesc, ImColor(225, 225, 228), fontSize * 0.9f, 0.82f * a, true, dl);
        mWidth = a > 0.02f ? exWidth : 0.f;
        mHeight = a > 0.02f ? height : 0.f;
        return;
    }

    // ---- the stack: newest card at the BOTTOM, older ones pushed UP (the
    // Rise behaviour — a new notification enters at the fixed bottom anchor
    // and the previous ones move up). active[] is oldest-first; drawing goes
    // newest-first so each card's Y is computed from the stack bottom.
    const size_t n = active.size();
    const float stackH = (float)n * height + (float)(n > 0 ? n - 1 : 0) * gap;
    const float bottomY = y + stackH - height;   // newest card's top edge
    float totalH = 0.f;
    float maxW = 0.f;
    for (size_t ii = 0; ii < n; ++ii) {
        const size_t i = n - 1 - ii;            // newest (last) first
        ActiveCard& card = active[i];
        const float curY = bottomY - (float)ii * (height + gap);
        // Toggle senders pass just the module NAME (Nova's two-line format
        // derives the status from the type); Stead keeps its classic single
        // "Name Enabled/Disabled" title, so the suffix is rebuilt here.
        std::string title = card.title;
        if (card.type == NotifType::ToggleOn) title += " Enabled";
        else if (card.type == NotifType::ToggleOff) title += " Disabled";
        const std::string& desc  = card.description;
        const float width = cardWidth(title, desc);

        // Entry: center-pivot zoom 0 -> 1 (500ms EASE_OUT_EXPO). Exit: rise
        // (translate up by exitT * height) + fade via 1-10*|anim-1| wings.
        float d1 = std::clamp(1.0f - 10.0f * std::fabs(card.anim - 1.0f), 0.f, 1.f);
        float d = std::clamp(card.anim, 0.f, 1.2f);
        float exitRise = card.exiting ? card.exitT * (height * 0.7f) : 0.f;
        float cardAlpha = card.exiting ? d1 * (1.f - card.exitT) : d1;
        if (cardAlpha <= 0.01f) continue;

        const float cx = x + width * 0.5f;
        const float cy = curY + height * 0.5f - exitRise;
        auto px = [&](float wx, float wy) {
            return ImVec2(cx + (wx - cx) * d, cy + (wy - cy) * d);
        };
        ImVec2 p1 = px(x, curY - exitRise), p2 = px(x + width, curY - exitRise + height);
        ImVec4 rect(p1.x, p1.y, p2.x, p2.y);

        drawBackdrop(rect);

        const int bgA = (int)(255.0f * bgOpacity * cardAlpha + 0.5f);
        if (shadowOn)
            ImRenderUtils::fillShadowRectangle(rect, ImColor(0, 0, 0),
                                               shadowA * cardAlpha, cardRound, 14.0f * scale, 0.6f, dl);
        ImRenderUtils::fillRectangle(rect, ImColor(12, 12, 15, bgA), 1.0f, cardRound, dl);
        const int borderA = (int)(28.0f * cardAlpha + 0.5f);
        if (borderA > 0) {
            dl->AddRect(ImVec2(rect.x, rect.y), ImVec2(rect.z, rect.w),
                        ImColor(255, 255, 255, borderA), cardRound, 0, 1.0f);
        }

        // Icon tile: white rounded square; the mark depends on the kind —
        // ToggleOn = check, ToggleOff = cross, everything else = the themed
        // placeholder square.
        const float iconX = rect.x + spacer, iconY = rect.y + spacer;
        ImRenderUtils::fillRectangle(ImVec4(iconX, iconY, iconX + iconSide, iconY + iconSide),
                                     ImColor(255, 255, 255, (int)(235.f * cardAlpha)),
                                     1.0f, 6.0f * scale, dl);
        const float thick = std::max(1.6f * scale, 2.f);
        if (card.type == NotifType::ToggleOn || card.type == NotifType::ToggleOff) {
            const ImVec2 c(iconX + iconSide * 0.5f, iconY + iconSide * 0.5f);
            const float r = iconSide * 0.22f;
            const ImU32 markCol = IM_COL32(46, 170, 90, (int)(255.f * cardAlpha));   // check green
            const ImU32 crossCol = IM_COL32(220, 70, 70, (int)(255.f * cardAlpha)); // cross red
            if (card.type == NotifType::ToggleOn) {
                // A crisp 2-segment check mark.
                dl->AddLine(ImVec2(c.x - r * 1.3f, c.y + r * 0.1f),
                            ImVec2(c.x - r * 0.35f, c.y + r), markCol, thick);
                dl->AddLine(ImVec2(c.x - r * 0.35f, c.y + r),
                            ImVec2(c.x + r * 1.3f, c.y - r * 0.9f), markCol, thick);
            } else {
                dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), crossCol, thick);
                dl->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x - r, c.y + r), crossCol, thick);
            }
        } else {
            dl->AddRect(ImVec2(iconX + 6.f * scale, iconY + 6.f * scale),
                        ImVec2(iconX + iconSide - 6.f * scale, iconY + iconSide - 6.f * scale),
                        ColorUtils::getThemedColor(0.0f, now), 3.f * scale, 0,
                        std::max(1.5f * scale, 2.f));
        }

        // BOLD WHITE title + LIGHT WHITE description.
        const float textX = rect.x + spacer + iconSide + spacer;
        const float textY = rect.y + spacer - 1.0f * scale;
        ImRenderUtils::drawText(ImVec2(textX, textY), title,
                                ImColor(255, 255, 255), fontSize, cardAlpha, true, dl);
        if (!desc.empty()) {
            ImRenderUtils::drawText(ImVec2(textX, textY + fontSize * 0.85f), desc,
                                    ImColor(225, 225, 228), fontSize * 0.9f, 0.9f * cardAlpha, true, dl);
        }

        maxW = std::max(maxW, width);
        totalH += height + gap;
    }

    mWidth = maxW;
    mHeight = stackH;
}
