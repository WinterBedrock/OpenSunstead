#pragma once
#include "../Module.h"
#include "../ModuleManager.h"
#include "../../Commands/CommandManager.h"
#include "../../Core/ConfigManager.h"
#include "../../SDK/Client/ScreenView.h"
#include "../../Render/RenderManager.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/MathUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>
#include <Windows.h>

// ---------------------------------------------------------------------------
// CommandMenu — the command hint overlay.
//
// Opens on the command-prefix key while the HUD screen is up. The typed line
// is shown at the top of the panel and the rows underneath are the matches for
// whatever token is being typed right now: the command name on the first word,
// then that command's own arguments (a module, one of its settings, a config
// name, a key) on every word after it. Up/Down move the selection, Tab fills
// the selected token in, Enter runs the whole line, Esc closes.
// ---------------------------------------------------------------------------
class CommandMenu {
public:
    void OnRender() {
        if (SDK::ScreenView::currentScreenName() != "hud_screen" && !s_open) return;

        const float dt = ImGui::GetIO().DeltaTime;
        s_fade += ((s_open ? 1.0f : 0.0f) - s_fade) * std::clamp(dt * 14.0f, 0.0f, 1.0f);
        if (s_fade < 0.002f) { s_fade = 0.0f; return; }
        if (s_open) {
            s_caret += dt;
            RebuildList();
        }

        // ---- Animations -----------------------------------------------------
        // Per-glyph reveal, the Chat input language: every typed codepoint owns
        // one 0..1 progress that settles with an easeOutBack curve while the
        // glyph drops in from a few px above and fades up. Backspace simply
        // drops the tail entries.
        auto& rev = s_glyphReveal;
        size_t cpCount = 0;
        for (size_t i = 0; i < s_line.size(); ) {
            unsigned int clen = 1;
            while (i + clen < s_line.size() && ((unsigned char)s_line[i + clen] & 0xC0) == 0x80) ++clen;
            i += clen;
            ++cpCount;
        }
        if (rev.size() < cpCount) rev.resize(cpCount, 0.f);
        else if (rev.size() > cpCount) rev.resize(cpCount);
        const float revealSpeed = std::clamp(dt * 9.0f, 0.0f, 1.0f);
        for (auto& t : rev) {
            t += (1.0f - t) * revealSpeed;
            if (t > 0.996f) t = 1.0f;
        }
        auto easeOutBack = [](float t) {
            if (t <= 0.0f) return 0.0f;
            if (t >= 1.0f) return 1.0f;
            constexpr float c1 = 1.70158f;
            constexpr float c3 = c1 + 1.0f;
            const float u = t - 1.0f;
            return 1.0f + c3 * u * u * u + c1 * u * u;
        };

        // Row appear animation: entries that were not on screen last frame
        // (new matches after a keystroke) ease 0 -> 1 and slide in; rows that
        // left simply stop being drawn. Keyed by label+desc so the same row
        // keeps its amount while it stays in the list.
        for (const auto& e : s_list) {
            const std::string key = e.label + "\x1f" + e.desc;
            float& a = s_rowAnim[key];
            if (a < 0.0001f) a = 0.0001f;   // "just created" marker
            a = MathUtils::animate(1.0f, a, std::clamp(dt * 13.0f, 0.0f, 1.0f));
            if (a > 0.999f) a = 1.0f;
        }
        for (auto it = s_rowAnim.begin(); it != s_rowAnim.end();) {
            bool present = false;
            for (const auto& e : s_list)
                if (it->first == e.label + "\x1f" + e.desc) { present = true; break; }
            if (!present) it = s_rowAnim.erase(it); else ++it;
        }
        // Selection bar slides between rows instead of jumping.
        const float selTargetY = s_sel >= 0 ? (float)s_sel : 0.0f;
        s_selBarY = MathUtils::animate(selTargetY, s_selBarY, std::clamp(dt * 18.0f, 0.0f, 1.0f));
        if (std::fabs(s_selBarY - selTargetY) < 0.01f) s_selBarY = selTargetY;

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (!dl) return;
        const ImVec2 screen = ImGui::GetIO().DisplaySize;

        const float w = 620.0f;
        const float rowH = 32.0f;
        const float inputH = 46.0f;
        const int visRows = std::min<int>((int)s_list.size(), 12);
        // The panel grows/shrinks with the row count instead of popping: the
        // eased height decides what is drawn this frame, and the row at the
        // seam fades in with the reveal.
        const float listTargetH = visRows > 0 ? visRows * rowH : rowH;
        s_listH = MathUtils::animate(listTargetH, s_listH, std::clamp(dt * 16.0f, 0.0f, 1.0f));
        if (std::fabs(s_listH - listTargetH) < 0.25f) s_listH = listTargetH;
        const float listH = s_listH;
        const float footH = 26.0f;
        const float h = inputH + listH + footH;
        const float x = (screen.x - w) * 0.5f;
        const float y = screen.y * 0.30f;
        const ImVec4 panel(x, y, x + w, y + h);
        const float round = 12.0f;

        // The plate is the only thing darkened. The world around it stays as it
        // is, so the hint window does not dim the game.
        D2D::addBlurOptimized(dl, 8.0f * s_fade, std::optional<ImVec4>(panel), round);
        dl->AddRectFilled(ImVec2(panel.x, panel.y), ImVec2(panel.z, panel.w),
                          ImGui::GetColorU32(ImVec4(0.07f, 0.075f, 0.09f, 0.72f * s_fade)), round);
        dl->AddRect(ImVec2(panel.x, panel.y), ImVec2(panel.z, panel.w),
                    ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.10f * s_fade)), round);

        auto& rm = RenderManager::GetInstance();
        ImFont* font = rm.defaultFont ? rm.defaultFont : ImGui::GetFont();
        ImFont* bold = rm.defaultFontBold ? rm.defaultFontBold : font;
        const float inputFs = 20.0f, nameFs = 16.0f, descFs = 13.0f;
        const float padX = 16.0f;
        const std::string& prefix = CommandManager::GetInstance().mPrefix;
        ImColor accent = ColorUtils::getThemedColor(0);
        const ImU32 accentU = ImGui::GetColorU32(ImVec4(accent.Value.x, accent.Value.y,
                                                        accent.Value.z, s_fade));

        // The line the user is actually typing, prefix included. The prefix is
        // static; every typed codepoint is drawn with its own reveal progress
        // (drops in from above + fades up, the Chat input language), the caret
        // rides after the last one.
        const ImVec2 inputPos(panel.x + padX, panel.y + (inputH - inputFs) * 0.5f);
        dl->AddText(bold, inputFs, inputPos, ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, s_fade)),
                    prefix.c_str());
        const float prefixW = bold->CalcTextSizeA(inputFs, FLT_MAX, 0.f, prefix.c_str()).x;
        const float textY = inputPos.y;
        float gx = inputPos.x + prefixW;
        size_t cp = 0;
        for (size_t i = 0; i < s_line.size(); ) {
            unsigned int clen = 1;
            while (i + clen < s_line.size() && ((unsigned char)s_line[i + clen] & 0xC0) == 0x80) ++clen;
            const std::string glyph(s_line.c_str() + i, clen);
            const float t = cp < rev.size() ? rev[cp] : 1.0f;
            const float e = easeOutBack(std::clamp(t, 0.0f, 1.0f));
            i += clen;
            ++cp;
            const float w = bold->CalcTextSizeA(inputFs, FLT_MAX, 0.f,
                                                glyph.c_str(), glyph.c_str() + glyph.size()).x;
            if (e > 0.01f)
                dl->AddText(bold, inputFs,
                            ImVec2(gx, textY + (1.0f - e) * 4.0f),
                            ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, std::clamp(e, 0.0f, 1.0f) * s_fade)),
                            glyph.c_str(), glyph.c_str() + glyph.size());
            gx += w;
        }
        const float caretX = gx;
        if (std::fmod(s_caret, 1.0f) < 0.55f) {
            dl->AddRectFilled(ImVec2(caretX + 2.0f, textY + 1.0f),
                              ImVec2(caretX + 3.5f, textY + inputFs - 1.0f), accentU);
        }
        dl->AddLine(ImVec2(panel.x + 10.0f, panel.y + inputH),
                    ImVec2(panel.z - 10.0f, panel.y + inputH),
                    ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.06f * s_fade)));

        const float listTop = panel.y + inputH;
        if (s_list.empty()) {
            dl->AddText(font, nameFs, ImVec2(panel.x + padX, listTop + 8.0f),
                        ImGui::GetColorU32(ImVec4(0.55f, 0.57f, 0.62f, s_fade)),
                        "No matches");
        }

        // The eased reveal decides how many rows exist this frame; the row
        // straddling the seam fades in with what is left of the height.
        const float reveal = s_listH / rowH;
        // Selection plate: one rounded bar that SLIDES between rows at the
        // eased index instead of snapping, plus the accent tick on its left.
        if (!s_list.empty() && s_sel >= 0 && s_sel < (int)s_list.size()) {
            const float sIdx = std::clamp(s_selBarY, 0.0f, (float)((int)s_list.size() - 1));
            const float sy = listTop + sIdx * rowH;
            const float plateA = s_fade * std::clamp(reveal - sIdx, 0.0f, 1.0f);
            if (plateA > 0.01f) {
                dl->AddRectFilled(ImVec2(panel.x + 8.0f, sy + 2.0f),
                                  ImVec2(panel.z - 8.0f, sy + rowH - 2.0f),
                                  ImGui::GetColorU32(ImVec4(accent.Value.x, accent.Value.y,
                                                            accent.Value.z, 0.20f * plateA)),
                                  6.0f);
                dl->AddRectFilled(ImVec2(panel.x + 8.0f, sy + 6.0f),
                                  ImVec2(panel.x + 10.0f, sy + rowH - 6.0f),
                                  ImGui::GetColorU32(ImVec4(accent.Value.x, accent.Value.y,
                                                            accent.Value.z, plateA)), 1.0f);
            }
        }
        const ImVec2 mp = ImGui::GetIO().MousePos;
        for (int i = 0; i < (int)s_list.size(); ++i) {
            const float rowReveal = std::clamp(reveal - (float)i, 0.0f, 1.0f);
            if (rowReveal <= 0.0f) break;
            const float ry = listTop + i * rowH;
            const ImVec4 row(panel.x + 8.0f, ry + 2.0f, panel.z - 8.0f, ry + rowH - 2.0f);
            const bool sel = (i == s_sel);
            const float rowAnim = MathUtils::easeOutCubic(s_rowAnim[s_list[i].label + "\x1f" + s_list[i].desc]);
            const float rowA = s_fade * rowReveal * rowAnim;
            if (rowA < 0.01f) continue;
            // New rows slide in from the left with their fade.
            const float slide = (1.0f - rowAnim) * 18.0f;
            const ImVec4 rowSlid(row.x + slide, row.y, row.z + slide, row.w);

            const bool hov = mp.x >= rowSlid.x && mp.x <= rowSlid.z && mp.y >= rowSlid.y && mp.y <= rowSlid.w;
            if (hov && !sel) { s_sel = i; }
            if (hov && ImGui::IsMouseClicked(0)) Complete();

            const ImColor nameCol = sel ? accent : ImColor(235, 237, 242, 255);
            dl->AddText(bold, nameFs, ImVec2(rowSlid.x + padX, ry + (rowH - nameFs) * 0.5f),
                        ImGui::GetColorU32(ImVec4(nameCol.Value.x, nameCol.Value.y,
                                                  nameCol.Value.z, rowA)),
                        s_list[i].label.c_str());
            if (!s_list[i].desc.empty()) {
                const float lw = bold->CalcTextSizeA(nameFs, FLT_MAX, 0.f, s_list[i].label.c_str()).x;
                float maxW = (rowSlid.z - padX) - (rowSlid.x + padX + lw + 14.0f);
                std::string desc = s_list[i].desc;
                if (maxW < 24.0f) { desc.clear(); maxW = 0.0f; }
                while (!desc.empty() &&
                       font->CalcTextSizeA(descFs, FLT_MAX, 0.f, desc.c_str()).x > maxW)
                    desc.pop_back();
                if (desc.size() != s_list[i].desc.size()) {
                    while (desc.size() > 1 && (desc.back() & 0xC0) == 0x80) desc.pop_back();
                    desc += "...";
                }
                if (!desc.empty())
                    dl->AddText(font, descFs,
                                ImVec2(rowSlid.x + padX + lw + 14.0f, ry + (rowH - descFs) * 0.5f),
                                ImGui::GetColorU32(ImVec4(0.62f, 0.64f, 0.70f, 0.9f * rowA)),
                                desc.c_str());
            }
        }

        const char* hint = "Tab  complete     Enter  run     Up/Down  select     Esc  close";
        dl->AddText(font, 12.0f, ImVec2(panel.x + padX, panel.y + h - footH + 6.0f),
                    ImGui::GetColorU32(ImVec4(0.50f, 0.52f, 0.58f, 0.9f * s_fade)), hint);
    }

    static bool IsOpen() { return s_open; }

    static void Open() {
        s_open = true;
        s_line.clear();
        s_sel = 0;
        s_caret = 0.0f;
        // Fresh overlay: glyph reveal starts empty, rows enter from scratch, the
        // panel grows out of the input line and the selection plate starts put.
        s_glyphReveal.clear();
        s_rowAnim.clear();
        s_listH = 0.0f;
        s_selBarY = 0.0f;
        RebuildList();
    }
    static void Close() { s_open = false; }

    static void FeedKeyDown(unsigned int vk) {
        if (!s_open) return;
        switch (vk) {
        case VK_ESCAPE: s_open = false; return;
        case VK_RETURN:
            Run();
            s_open = false;
            return;
        case VK_UP:
            if (!s_list.empty())
                s_sel = (s_sel + (int)s_list.size() - 1) % (int)s_list.size();
            return;
        case VK_DOWN:
            if (!s_list.empty()) s_sel = (s_sel + 1) % (int)s_list.size();
            return;
        case VK_TAB:
            Complete();
            return;
        case VK_BACK:
            if (!s_line.empty()) {
                s_line.pop_back();
                while (!s_line.empty() && (s_line.back() & 0xC0) == 0x80) s_line.pop_back();
                s_caret = 0.0f;
                RebuildList();
            }
            return;
        default: break;
        }
        const bool text = (vk >= '0' && vk <= 'Z') || vk == VK_SPACE ||
                          (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) ||
                          (vk >= 0xBA && vk <= 0xC0) || (vk >= 0xDB && vk <= 0xDF);
        if (!text) return;
        BYTE keys[256] = {};
        if (!GetKeyboardState(keys)) return;
        wchar_t out[8] = {};
        const int n = ToUnicode((UINT)vk, 0, keys, out, 8, 0);
        for (int i = 0; i < n; ++i) {
            if (out[i] < 0x20 || out[i] == 0x7F) continue;
            const unsigned int ch = out[i];
            if (ch < 0x80) s_line += (char)ch;
            else if (ch < 0x800) {
                s_line += (char)(0xC0 | (ch >> 6));
                s_line += (char)(0x80 | (ch & 0x3F));
            } else {
                s_line += (char)(0xE0 | (ch >> 12));
                s_line += (char)(0x80 | ((ch >> 6) & 0x3F));
                s_line += (char)(0x80 | (ch & 0x3F));
            }
        }
        s_caret = 0.0f;
        RebuildList();
    }

private:
    struct Entry { std::string label; std::string desc; std::string insert; };

    static bool s_open;
    static float s_fade;
    static float s_caret;
    static int s_sel;
    static std::string s_line;
    static std::vector<Entry> s_list;

    // ---- Animation state -----------------------------------------------------
    // Per-glyph reveal progress for the typed line (one entry per codepoint,
    // 0 = just typed) — the Chat input language.
    static std::vector<float> s_glyphReveal;
    // Per-row appear amount, keyed by label+desc so a row that stays in the
    // list keeps its amount while a brand-new match starts at 0 and eases in.
    static std::unordered_map<std::string, float> s_rowAnim;
    // Eased list height (rows * rowH) and eased selection row index — together
    // they make the panel grow/shrink and the highlight slide between rows.
    static float s_listH;
    static float s_selBarY;

    static std::string Lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    static std::vector<std::string> Words(const std::string& line) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : line) {
            if (c == ' ') {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    static bool StartsWith(const std::string& text, const std::string& prefix) {
        return Lower(text).rfind(prefix, 0) == 0;
    }

    static void Add(std::vector<Entry>& out, const std::string& label,
                    const std::string& desc, const std::string& typed) {
        if (!typed.empty() && !StartsWith(label, typed)) return;
        out.push_back({ label, desc, label });
    }

    static Command* FindCommand(const std::string& word) {
        const std::string w = Lower(word);
        for (const auto& c : CommandManager::GetInstance().mCommands) {
            if (!c) continue;
            if (Lower(c->mName) == w) return c.get();
            for (const auto& a : c->mAliases)
                if (Lower(a) == w) return c.get();
        }
        return nullptr;
    }

    // What the word currently being typed can be, given the words before it.
    static void SuggestArgs(const std::vector<std::string>& done, const std::string& typed,
                            std::vector<Entry>& out) {
        if (done.empty()) return;
        Command* cmd = FindCommand(done[0]);
        if (!cmd) return;
        const std::string name = Lower(cmd->mName);
        const int arg = (int)done.size() - 1;   // 0 = the first argument

        auto modules = [&]() {
            for (Module* m : ModuleManager::GetInstance().GetModules())
                if (m) Add(out, m->GetName(), m->GetDescription(), typed);
        };
        auto keys = [&]() {
            static const char* kKeys[] = {
                "SPACE", "SHIFT", "CTRL", "ALT", "ENTER", "TAB",
                "UP", "DOWN", "LEFT", "RIGHT", "INSERT" };
            for (const char* k : kKeys) Add(out, k, "key", typed);
        };

        if (name == "bind") {
            if (arg == 0) modules();
            else if (arg == 1) keys();
        } else if (name == "unbind") {
            if (arg == 0) { Add(out, "all", "remove every bind", typed); modules(); }
        } else if (name == "config") {
            if (arg == 0) {
                Add(out, "save", "write the current settings", typed);
                Add(out, "load", "read a saved config", typed);
                Add(out, "list", "show every saved config", typed);
            } else if (arg == 1 && (Lower(done[1]) == "save" || Lower(done[1]) == "s" ||
                                    Lower(done[1]) == "load" || Lower(done[1]) == "l")) {
                for (const std::string& c : ConfigManager::ListConfigs())
                    Add(out, c, "config", typed);
            }
        } else if (name == "goto") {
            if (arg == 0) {
                Add(out, "stop", "cancel the current path", typed);
                Add(out, "here", "path to where you stand", typed);
                Add(out, "~ ~ ~", "relative coordinates", typed);
            }
        }
    }

    static void RebuildList() {
        const bool typing = !s_line.empty() && s_line.back() != ' ';
        const std::vector<std::string> words = Words(s_line);

        // The token being completed is the last word, unless the line ends in a
        // space, in which case the user has moved on to the next argument.
        std::string typed;
        std::vector<std::string> done;
        if (typing && !words.empty()) {
            typed = Lower(words.back());
            done.assign(words.begin(), words.end() - 1);
        } else {
            done = words;
        }

        std::vector<Entry> list;
        if (done.empty()) {
            for (const auto& c : CommandManager::GetInstance().mCommands) {
                if (!c) continue;
                bool match = StartsWith(c->mName, typed);
                if (!match)
                    for (const auto& a : c->mAliases)
                        if (StartsWith(a, typed)) { match = true; break; }
                if (match) list.push_back({ c->mName, c->mDescription, c->mName });
            }
            std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) {
                return Lower(a.label) < Lower(b.label);
            });
        } else {
            SuggestArgs(done, typed, list);
        }

        s_list = std::move(list);
        if (s_sel >= (int)s_list.size()) s_sel = (int)s_list.size() - 1;
        if (s_sel < 0) s_sel = 0;
    }

    // Replace the token being typed with the selected row and leave a trailing
    // space, so the next keypress starts the following argument.
    static void Complete() {
        if (s_sel < 0 || s_sel >= (int)s_list.size()) return;
        const bool typing = !s_line.empty() && s_line.back() != ' ';
        if (typing) {
            const size_t sp = s_line.find_last_of(' ');
            s_line.erase(sp == std::string::npos ? 0 : sp + 1);
        } else if (!s_line.empty() && s_line.back() != ' ') {
            s_line.push_back(' ');
        }
        s_line += s_list[s_sel].insert;
        if (s_line.empty() || s_line.back() != ' ') s_line.push_back(' ');
        s_sel = 0;
        s_caret = 0.0f;
        RebuildList();
    }

    static void Run() {
        std::string line = s_line;
        // A bare command name runs the selected row, so Enter on a highlighted
        // command does not require Tab first.
        if (line.find(' ') == std::string::npos &&
            s_sel >= 0 && s_sel < (int)s_list.size())
            line = s_list[s_sel].insert;
        if (line.empty()) return;
        CommandManager::GetInstance().ExecuteCommand(
            CommandManager::GetInstance().mPrefix + line);
    }
};

inline bool CommandMenu::s_open = false;
inline float CommandMenu::s_fade = 0.0f;
inline float CommandMenu::s_caret = 0.0f;
inline int CommandMenu::s_sel = 0;
inline std::string CommandMenu::s_line;
inline std::vector<CommandMenu::Entry> CommandMenu::s_list;
inline std::vector<float> CommandMenu::s_glyphReveal;
inline std::unordered_map<std::string, float> CommandMenu::s_rowAnim;
inline float CommandMenu::s_listH = 0.0f;
inline float CommandMenu::s_selBarY = 0.0f;
