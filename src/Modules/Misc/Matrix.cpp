#include "Matrix.h"
#include "../Setting.h"
#include "../ModuleManager.h"
#include "../../Render/RenderManager.h"
#include "../../SDK/Client/ScreenView.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include <imgui.h>
#include <cmath>
#include <algorithm>

// Mojangles covers Latin + digits reliably; keep the set ASCII-only.
static const char kGlyphs[] =
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "$%#@&*+=<>";

Matrix::Matrix()
    : Module("Matrix", "Matrix-style falling green glyphs (GUI/chat/pause only)", Category::Misc, 0) {
    AddSetting(new NumberSetting("Density", "Number of falling columns", 48.f, 12.f, 120.f, 1.f));
    AddSetting(new NumberSetting("Speed", "Fall speed multiplier", 1.0f, 0.3f, 3.0f, 0.05f));
    AddSetting(new NumberSetting("Size", "Glyph size (Mojangles)", 16.f, 10.f, 28.f, 1.f));
    AddSetting(new NumberSetting("Opacity", "Overall opacity", 0.85f, 0.15f, 1.0f, 0.05f));
    canBind = true;
}

void Matrix::OnEnable() {
    mDrops.clear();
    mLastW = mLastH = 0.f;
}

void Matrix::OnDisable() {
    mDrops.clear();
}

void Matrix::reseatDrop(Drop& d, float screenW, float screenH, float fontSize, bool randomY) {
    d.x = nextF() * screenW;
    d.y = randomY ? (nextF() * screenH) : (-nextF() * screenH * 0.5f);
    d.speed = (55.f + nextF() * 140.f);
    d.len = 8 + (int)(nextU() % 18);
    d.phase = nextF() * 6.28318f;
    d.glyphs.resize((size_t)d.len);
    const size_t nGlyph = sizeof(kGlyphs) - 1;
    for (int i = 0; i < d.len; ++i)
        d.glyphs[(size_t)i] = kGlyphs[nextU() % nGlyph];
    (void)fontSize;
}

void Matrix::ensureDrops(float screenW, float screenH, float fontSize) {
    int want = (int)static_cast<NumberSetting*>(mSettings[0])->mValue;
    if (want < 4) want = 4;
    if ((int)mDrops.size() != want || std::fabs(screenW - mLastW) > 8.f || std::fabs(screenH - mLastH) > 8.f) {
        mDrops.clear();
        mDrops.resize((size_t)want);
        for (auto& d : mDrops) reseatDrop(d, screenW, screenH, fontSize, true);
        mLastW = screenW;
        mLastH = screenH;
    }
}

void Matrix::OnRender() {
    if (!IsEnabled()) return;

    // Only ClickGUI / chat / pause — never on plain HUD or other menus.
    bool allow = false;
    if (Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI"))
        if (gui->IsEnabled()) allow = true;
    const std::string& sn = SDK::ScreenView::currentScreenName();
    if (sn == "chat_screen" || sn == "pause_screen") allow = true;
    if (!allow) return;

    auto& rm = RenderManager::GetInstance();
    ImFont* font = rm.minecraftFont ? rm.minecraftFont : ImGui::GetFont();
    if (!font) return;

    const float fontSize = static_cast<NumberSetting*>(mSettings[2])->mValue;
    const float speedMul = static_cast<NumberSetting*>(mSettings[1])->mValue;
    const float opac     = static_cast<NumberSetting*>(mSettings[3])->mValue;
    const float dt = ImRenderUtils::getDeltaTime();
    ImVec2 screen = ImRenderUtils::getScreenSize();
    if (screen.x < 32.f || screen.y < 32.f) return;

    ensureDrops(screen.x, screen.y, fontSize);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImGui::PushFont(font);

    // Occasionally scramble glyphs so columns feel alive.
    static float scrambleT = 0.f;
    scrambleT += dt;
    const bool scramble = scrambleT > 0.07f;
    if (scramble) scrambleT = 0.f;
    const size_t nGlyph = sizeof(kGlyphs) - 1;

    for (auto& d : mDrops) {
        d.y += d.speed * speedMul * dt;
        const float step = fontSize * 0.95f;
        if (d.y - (float)d.len * step > screen.y) {
            reseatDrop(d, screen.x, screen.y, fontSize, false);
            continue;
        }
        if (scramble) {
            // flip a couple of characters per column
            for (int k = 0; k < 2 && d.len > 0; ++k) {
                int idx = (int)(nextU() % (unsigned)d.len);
                d.glyphs[(size_t)idx] = kGlyphs[nextU() % nGlyph];
            }
        }

        for (int i = 0; i < d.len; ++i) {
            const float gy = d.y - (float)i * step;
            if (gy < -fontSize || gy > screen.y + fontSize) continue;
            // Head is brightest near-white green; trail fades darker.
            const float t = (float)i / (float)std::max(d.len - 1, 1);
            const float head = (i == 0) ? 1.f : 0.f;
            const float g = 0.25f + 0.75f * (1.f - t);
            const float r = 0.02f + 0.18f * head + 0.05f * (1.f - t);
            const float b = 0.02f + 0.10f * head;
            const float a = opac * (0.15f + 0.85f * (1.f - t * 0.92f));
            char buf[5] = { d.glyphs[(size_t)i], 0, 0, 0, 0 };
            // Soft dark underlay for legibility on bright panels.
            dl->AddText(font, fontSize, ImVec2(d.x + 1.f, gy + 1.f),
                        ImGui::GetColorU32(ImVec4(0.f, 0.f, 0.f, a * 0.55f)), buf);
            dl->AddText(font, fontSize, ImVec2(d.x, gy),
                        ImGui::GetColorU32(ImVec4(r, g, b, a)), buf);
        }
    }

    ImGui::PopFont();
}
