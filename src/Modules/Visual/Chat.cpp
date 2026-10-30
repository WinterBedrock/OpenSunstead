#include "Chat.h"
#include "../../Commands/CommandManager.h"
#include "../../SDK/Client/Chat.h"
#include "../../SDK/Client/ClientInstance.h"
#include "../../SDK/Client/ScreenView.h"
#include "../../SDK/Client/I18n.h"
#include "../../SDK/Network/PacketTypes.h"
#include "../../SDK/Network/Packets/ChatPackets.h"
#include "../../Render/HudEditor.h"
#include "../../Render/RenderManager.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include <imgui.h>
#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

Chat* Chat::sInstance = nullptr;
std::mutex Chat::sHistoryMutex;

// ScreenView.h's gameplay-key gate flag — owned here, see Chat::OnRender.
bool SDK::g_chatInputOpen = false;

static char s_inputBuf[512] = { 0 };
char* Chat::InputBuffer() { return s_inputBuf; }

// One appear-progress value per input code point (ClickGUI search reveal).
static std::vector<float>& inputReveal() {
    static std::vector<float> r;
    return r;
}

// Caret position in CODEPOINTS (0 = before the first glyph). Left/Right move
// it one glyph, Home/End jump; typing, Backspace and Delete act at the caret.
static size_t s_caretCp = 0;

// Horizontal scroll of the input text in px — follows the caret, so a long
// line keeps the caret visible instead of always pinning to the end.
static float s_inputScrollPx = 0.f;

static size_t cpCountOf(const char* s) {
    size_t n = 0;
    for (size_t i = 0; s[i]; ++i)
        if (((unsigned char)s[i] & 0xC0) != 0x80) ++n;
    return n;
}

// Byte offset of the caret's codepoint index inside s_inputBuf.
static size_t caretByte() {
    const size_t len = std::strlen(s_inputBuf);
    size_t i = 0, cp = 0;
    while (i < len && cp < s_caretCp) {
        unsigned int clen = 1;
        while (i + clen < len && ((unsigned char)s_inputBuf[i + clen] & 0xC0) == 0x80) ++clen;
        i += clen;
        ++cp;
    }
    return i;
}

void Chat::setInputBuffer(const char* text) {
    if (!text) text = "";
    std::strncpy(s_inputBuf, text, sizeof(s_inputBuf) - 1);
    s_inputBuf[sizeof(s_inputBuf) - 1] = '\0';
    inputReveal().clear();
    inputReveal().assign(cpCountOf(s_inputBuf), 1.f);   // prefills don't animate
    s_caretCp = cpCountOf(s_inputBuf);                  // caret lands at the end
    s_inputScrollPx = 0.f;
}
std::string Chat::InputText() const { return std::string(s_inputBuf); }

namespace {
// Packet body layouts + SEH reads live in SDK/Network/Packets/ChatPackets.h.
using SDK::sehCopyPacketString;
using SDK::sehReadPacketByte;
using SDK::sehReadPacketPtr;
using SDK::sehFormatCommandOutputMsg;

// ---------------------------------------------------------------------------

// § marker in UTF-8 = 0xC2 0xA7.
struct StyledCp {
    unsigned int cp;      // UTF-8 codepoint
    int style;           // -1 default, -2 theme gradient, else palette index
    size_t off;          // byte offset in the raw line (maps links to glyphs)
};
struct ChatStyleRow {
    std::vector<StyledCp> cps;
    unsigned long long atMs;   // for the closed fade (from the source line)
};

// Decode one UTF-8 sequence; returns the sequence length (1 for the § pair
// which is consumed as TWO bytes producing cp == 0xA7 handled by the caller).
static int utf8Cp(const char* s, size_t remain, unsigned int* out) {
    const unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *out = c; return 1; }
    if ((c & 0xE0) == 0xC0 && remain >= 2) { *out = ((c & 0x1F) << 6) | (s[1] & 0x3F); return 2; }
    if ((c & 0xF0) == 0xE0 && remain >= 3) { *out = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); return 3; }
    if ((c & 0xF8) == 0xF0 && remain >= 4) { *out = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); return 4; }
    *out = c;
    return 1;
}

// Bedrock colour palette for §0..§f (index = code char).
static const unsigned int kChatPalette[16] = {
    IM_COL32(0,   0,   0,   255),   // 0 black
    IM_COL32(0,   0,  170, 255),   // 1 dark_blue
    IM_COL32(0,  170,   0, 255),   // 2 dark_green
    IM_COL32(0,  170, 170, 255),   // 3 dark_aqua
    IM_COL32(170,  0,   0, 255),   // 4 dark_red
    IM_COL32(170,   0, 170, 255),   // 5 dark_purple
    IM_COL32(255, 170,   0, 255),   // 6 gold
    IM_COL32(170, 170, 170, 255),   // 7 gray
    IM_COL32(85,   85,  85, 255),   // 8 dark_gray
    IM_COL32(85,   85, 255, 255),   // 9 blue
    IM_COL32(85, 255,  85, 255),   // a green
    IM_COL32(85, 255, 255, 255),   // b aqua
    IM_COL32(255,  85,  85, 255),   // c red
    IM_COL32(255,  85, 255, 255),   // d light_purple
    IM_COL32(255, 255,  85, 255),   // e yellow
    IM_COL32(255, 255, 255, 255),   // f white
};

// Decode a RAW line into styled codepoints (per-char style state machine).
static void decodeStyled(const std::string& raw, std::vector<StyledCp>* out) {
    int style = -1;   // -1 default white, -2 theme gradient, else palette idx
    size_t i = 0;
    while (i < raw.size()) {
        if ((unsigned char)raw[i] == 0xC2 && i + 2 < raw.size() &&
            (unsigned char)raw[i + 1] == 0xA7) {
            unsigned int code = 0;
            const int adv = utf8Cp(raw.c_str() + i + 2, raw.size() - i - 2, &code);
            const char c = (char)code;
            if (c >= '0' && c <= '9') style = c - '0';
            else if (c >= 'a' && c <= 'f') style = 10 + (c - 'a');
            else if (c == 'r') style = -1;      // reset to default
            else if (c == 'z') style = -2;      // client theme gradient
            i += 2 + adv;
            continue;
        }
        unsigned int cp = 0;
        const int adv = utf8Cp(raw.c_str() + i, raw.size() - i, &cp);
        out->push_back({cp, style, i});
        i += adv;
    }
}

// Encode one codepoint back to UTF-8 (for per-char AddText).
static int encodeUtf8(unsigned int cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// ---- rawtext JSON -----------------------------------------------------------
// Some server/system lines arrive as rawtext JSON:
//   {"rawtext":[{"text":"You have 0 friends online."}]}
// The game's own chat shows the DECODED text; decode every "text" member and
// concatenate so the JSON itself never hits the panel.
static bool decodeRawTextJson(const std::string& in, std::string* out) {
    size_t p = in.find_first_not_of(" \t\r\n");
    if (p == std::string::npos || in[p] != '{') return false;
    const size_t key = in.find("\"rawtext\"", p);
    if (key == std::string::npos || key > p + 32) return false;

    std::string acc;
    size_t i = p;
    while (i < in.size()) {
        const size_t k = in.find("\"text\"", i);
        if (k == std::string::npos) break;
        size_t q = in.find_first_not_of(" \t\r\n:", k + 6);
        if (q == std::string::npos) break;
        if (in[q] != '"') { i = k + 6; continue; }
        ++q;
        std::string val;
        bool ok = true;
        while (q < in.size() && in[q] != '"') {
            if (in[q] == '\\' && q + 1 < in.size()) {
                const char e = in[q + 1];
                switch (e) {
                case 'n': val += '\n'; break;
                case 't': val += '\t'; break;
                case 'r': val += '\r'; break;
                case 'b': val += '\b'; break;
                case 'f': val += '\f'; break;
                case '"': val += '"';  break;
                case '\\': val += '\\'; break;
                case '/': val += '/';  break;
                case 'u': {
                    if (q + 5 >= in.size()) { ok = false; break; }
                    unsigned cp = 0;
                    for (int d = 0; d < 4; ++d) {
                        const char h = in[q + 2 + d];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else { ok = false; break; }
                    }
                    if (!ok) break;
                    char u8[8];
                    const int n = encodeUtf8(cp, u8);
                    val.append(u8, n);
                    q += 4;
                    break;
                }
                default: val += e; break;
                }
                q += 2;
            } else {
                val += in[q];
                ++q;
            }
        }
        if (q >= in.size()) break;   // unterminated string
        i = q + 1;
        if (!val.empty()) {
            if (!acc.empty()) acc += ' ';
            acc += val;
        }
    }
    if (acc.empty()) return false;
    *out = acc;
    return true;
}

// ---- clickable links -------------------------------------------------------
// One detected URL in a line, in CODEPOINT indices (the renderer walks cps).
struct LinkSpan {
    size_t cpBegin = 0, cpEnd = 0;   // [begin, end) within the decoded line
    std::string url;
};

// Hit rects of the links drawn this frame (rebuilt on every OnRender).
struct LinkRect { ImVec4 rect; std::string url; };
static std::vector<LinkRect>& frameLinkRects() {
    static std::vector<LinkRect> v;
    return v;
}

// URLs end at whitespace, non-ASCII (Cyrillic etc.) and angle/quote chars.
// The scan runs over the DECODED codepoints — § style codes (e.g. the "§l"
// bold code right before "vk.com") are already consumed there, so a code's
// letter can never bleed into the URL ("lvk.com" bug).
static void findLinkSpans(const std::vector<StyledCp>& cps, std::vector<LinkSpan>* out) {
    const size_t n = cps.size();
    auto isWord = [](unsigned int c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '-' || c == '/';
    };
    auto labelChar = [](unsigned int c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-';
    };
    auto stop = [](unsigned int c) {
        return c <= 0x20 || c >= 0x80 || c == '<' || c == '>' || c == '"' || c == '\'';
    };
    auto ciEq = [](unsigned int c, char ref) {
        if (c == (unsigned char)ref) return true;
        if (ref >= 'a' && ref <= 'z' && c == (unsigned char)(ref - 'a' + 'A')) return true;
        return false;
    };

    size_t i = 0;
    while (i < n) {
        if (i > 0 && isWord(cps[i - 1].cp)) { ++i; continue; }
        size_t b = i, e = i;
        bool isLink = false;
        bool hasScheme = false;

        if (n - i >= 8 && ciEq(cps[i].cp,'h') && ciEq(cps[i+1].cp,'t') && ciEq(cps[i+2].cp,'t') &&
            ciEq(cps[i+3].cp,'p') && ciEq(cps[i+4].cp,'s') && ciEq(cps[i+5].cp,':') &&
            ciEq(cps[i+6].cp,'/') && ciEq(cps[i+7].cp,'/')) {
            hasScheme = true;
        } else if (n - i >= 7 && ciEq(cps[i].cp,'h') && ciEq(cps[i+1].cp,'t') && ciEq(cps[i+2].cp,'t') &&
                   ciEq(cps[i+3].cp,'p') && ciEq(cps[i+4].cp,':') &&
                   ciEq(cps[i+5].cp,'/') && ciEq(cps[i+6].cp,'/')) {
            hasScheme = true;
        }
        if (hasScheme) {
            size_t j = i;
            while (j < n && !stop(cps[j].cp)) ++j;
            e = j;
            isLink = true;
        } else {
            // label(.label)+ — the TLD must be alphabetic (2+), so "1.5" and
            // "e.g." stay plain text while "x.com" and "youtu.be" link.
            size_t j = i, tldStart = i;
            size_t tldLen = 0;
            int labels = 0;
            for (;;) {
                const size_t l0 = j;
                while (j < n && labelChar(cps[j].cp)) ++j;
                const size_t llen = j - l0;
                if (llen == 0) break;
                ++labels;
                tldStart = l0;
                tldLen = llen;
                if (j < n && cps[j].cp == '.') { ++j; continue; }
                break;
            }
            bool ok = false;
            if (labels >= 2 && tldLen >= 2) {
                ok = true;
                for (size_t k = 0; k < tldLen; ++k) {
                    const unsigned int c = cps[tldStart + k].cp;
                    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) { ok = false; break; }
                }
            }
            if (ok) {
                if (j < n && cps[j].cp == '/') {   // consume the path
                    ++j;
                    while (j < n && !stop(cps[j].cp)) ++j;
                }
                e = j;
                isLink = true;
            }
        }
        if (isLink) {
            while (e > b) {   // trailing punctuation is not part of the URL
                const unsigned int c = cps[e - 1].cp;
                if (c == '.' || c == ',' || c == '!' || c == '?' || c == ')' ||
                    c == ']' || c == '}' || c == ';' || c == ':' || c == '\'' || c == '"')
                    --e;
                else break;
            }
            if (e > b && (e - b > 7 || !hasScheme)) {   // "http://" alone is not a URL
                std::string url;
                for (size_t k = b; k < e; ++k) {
                    char u8[8];
                    const int m = encodeUtf8(cps[k].cp, u8);
                    url.append(u8, m);
                }
                if (!hasScheme) url = "https://" + url;   // bare domain -> browser needs a scheme
                LinkSpan sp;
                sp.cpBegin = b;
                sp.cpEnd = e;
                sp.url = std::move(url);
                out->push_back(std::move(sp));
            }
            i = e;
        } else {
            ++i;
        }
    }
}

// Font for a codepoint: the selected chat font, or the default client font
// when the glyph is missing (per-char fallback — Cyrillic in Mojangles etc.).
// The baked data holds the glyph tables for a font AT A GIVEN SIZE (this
// ImGui's API: ImFont::GetFontBaked).
static ImFontBaked* bakedFor(ImFont* font, float fontSize) {
    if (!font || fontSize < 1.0f) return nullptr;
    return font->GetFontBaked(fontSize);
}

static ImFont* fontForCp(ImFont* chatFont, ImFontBaked* chatBaked,
                         ImFont* fallbackFont, unsigned int cp) {
    if (chatFont && chatBaked && cp <= 0xFFFF) {
        // FindGlyphNoFallback returns NULL when the codepoint is missing from
        // THIS font — that is exactly the per-char fallback signal.
        const ImFontGlyph* g = chatBaked->FindGlyphNoFallback((ImWchar)cp);
        if (g) return chatFont;
    }
    return fallbackFont ? fallbackFont : chatFont;
}

static float charAdvance(ImFont* font, ImFontBaked* baked, unsigned int cp, float fontSize) {
    if (baked) {
        const float adv = baked->GetCharAdvance((ImWchar)cp);
        if (adv > 0.0f) return adv;
    }
    char buf[8];
    const int n = encodeUtf8(cp, buf);
    return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, buf, buf + n).x;
}

// Draw one styled row at pos; returns the consumed width.
static float drawStyledRow(ImDrawList* dl, const ImVec2& pos,
                           const std::vector<StyledCp>& cps, size_t begin, size_t count,
                           float fontSize, ImFont* chatFont, ImFont* fallbackFont,
                           float alpha, unsigned long long nowMs,
                           const LinkSpan* spans, size_t spanCount) {
    float x = pos.x;
    const float y = pos.y;
    float gradIdx = 0.0f;
    ImFontBaked* chatBaked = bakedFor(chatFont, fontSize);
    ImFontBaked* fallbackBaked = bakedFor(fallbackFont, fontSize);

    // Links intersecting this row: accent colour + underline + a hit rect.
    struct RowLink { const LinkSpan* span; float x0, x1; };
    RowLink rowLinks[8];
    int nRowLinks = 0;

    for (size_t k = begin; k < begin + count; ++k) {
        const StyledCp& sc = cps[k];
        const bool useFallback = !fontForCp(chatFont, chatBaked, fallbackFont, sc.cp);
        ImFont* font = useFallback ? fallbackFont : chatFont;
        ImFontBaked* baked = useFallback ? fallbackBaked : chatBaked;
        if (!font) continue;
        char buf[8];
        const int n = encodeUtf8(sc.cp, buf);

        const LinkSpan* link = nullptr;
        for (size_t si = 0; si < spanCount; ++si) {
            if (k >= spans[si].cpBegin && k < spans[si].cpEnd) { link = &spans[si]; break; }
        }
        int linkIdx = -1;
        if (link) {
            for (int t = 0; t < nRowLinks; ++t)
                if (rowLinks[t].span == link) { linkIdx = t; break; }
            if (linkIdx < 0 && nRowLinks < 8) {
                rowLinks[nRowLinks] = {link, x, x};
                linkIdx = nRowLinks++;
            }
        }

        unsigned int col;
        if (link) {
            // Link: the live theme accent — clearly tappable.
            ImColor tc = ColorUtils::getThemedColor(0);
            tc.Value.w = 1.0f;
            col = (ImU32)tc;
        } else if (sc.style == -2) {
            // Theme gradient per character — EXACTLY the Watermark language:
            // getThemedColor(index * 22) with the index advancing 6 per char
            // (the same combination ImRenderUtils::drawGradientText +
            // Watermark::getGradColor use), so §z flows like the watermark.
            ImColor tc = ColorUtils::getThemedColor(gradIdx * 132.0f);
            tc.Value.w = 1.0f;
            col = (ImU32)tc;
            gradIdx += 1.0f;
        } else if (sc.style >= 0 && sc.style < 16) {
            col = kChatPalette[sc.style];
        } else {
            col = IM_COL32(235, 235, 235, 255);
        }
        const unsigned int a = (unsigned int)(alpha * 255.0f + 0.5f);
        if (a > 255) col = (col & 0x00FFFFFF) | (255u << 24);
        else col = (col & 0x00FFFFFF) | (a << 24);

        // Shadow (Java-chat look): offset by fontSize/12 px, black, 60%.
        if (alpha > 0.05f) {
            const unsigned int shadowA = (unsigned int)(alpha * 140.0f + 0.5f);
            dl->AddText(font, fontSize, ImVec2(x + 1.0f, y + 1.0f),
                        (col & 0x00FFFFFF) | (shadowA << 24),
                        buf, buf + n);
        }
        dl->AddText(font, fontSize, ImVec2(x, y), col, buf, buf + n);
        const float adv = charAdvance(font, baked, sc.cp, fontSize);
        if (linkIdx >= 0) rowLinks[linkIdx].x1 = x + adv;
        x += adv;
    }

    for (int t = 0; t < nRowLinks; ++t) {
        const unsigned int a = (unsigned int)(alpha * 255.0f + 0.5f);
        ImColor accent = ColorUtils::getThemedColor(0);
        accent.Value.w = 1.0f;
        const ImU32 linkCol = ((ImU32)accent & 0x00FFFFFF) | (a << 24);
        dl->AddLine(ImVec2(rowLinks[t].x0, y + fontSize + 1.5f),
                    ImVec2(rowLinks[t].x1, y + fontSize + 1.5f), linkCol, 1.0f);
        LinkRect lr;
        lr.rect = ImVec4(rowLinks[t].x0, y, rowLinks[t].x1, y + fontSize + 2.5f);
        lr.url = rowLinks[t].span->url;
        frameLinkRects().push_back(std::move(lr));
    }
    return x - pos.x;
}

// Measure a styled row's width (fallback-aware, matching the draw exactly).
static float measureStyledRow(const std::vector<StyledCp>& cps, size_t begin, size_t count,
                              float fontSize, ImFont* chatFont, ImFontBaked* chatBaked,
                              ImFont* fallbackFont) {
    float w = 0.f;
    for (size_t k = begin; k < begin + count; ++k) {
        const bool fb = !fontForCp(chatFont, chatBaked, fallbackFont, cps[k].cp);
        w += charAdvance(fb ? fallbackFont : chatFont, fb ? nullptr : chatBaked, cps[k].cp, fontSize);
    }
    return w;
}

// Greedy word-wrap of styled codepoints into row slices. A "word" is a run
// of non-space cps; colour state is already per-cp so no reconstruction is
// needed. Returns row slices (begin,count into `cps`).
struct RowSlice { size_t begin, count; };
static std::vector<RowSlice> wrapStyled(const std::vector<StyledCp>& cps,
                                        float fontSize, float maxW,
                                        ImFont* chatFont, ImFontBaked* chatBaked,
                                        ImFont* fallbackFont) {
    std::vector<RowSlice> rows;
    size_t i = 0;
    size_t rowBegin = 0, rowEnd = 0;   // [rowBegin, rowEnd) = current row span
    float rowW = 0.f;
    auto pushRow = [&](size_t end) {
        if (end > rowBegin) rows.push_back({rowBegin, end - rowBegin});
        rowBegin = end;
        rowW = 0.f;
    };
    while (i < cps.size()) {
        size_t wordEnd = i;
        while (wordEnd < cps.size() && cps[wordEnd].cp != ' ') ++wordEnd;
        size_t afterSpaces = wordEnd;
        while (afterSpaces < cps.size() && cps[afterSpaces].cp == ' ') ++afterSpaces;

        float wordW = 0.f;
        for (size_t k = i; k < afterSpaces; ++k) {
            const bool fb = !fontForCp(chatFont, chatBaked, fallbackFont, cps[k].cp);
            wordW += charAdvance(fb ? fallbackFont : chatFont, fb ? nullptr : chatBaked, cps[k].cp, fontSize);
        }

        if (rowEnd > rowBegin && rowW + wordW > maxW) {
            pushRow(rowEnd);
        }
        // hard-break a single over-wide word by character
        size_t j = i;
        float curW = measureStyledRow(cps, rowBegin, rowEnd - rowBegin, fontSize, chatFont, chatBaked, fallbackFont);
        while (j < afterSpaces) {
            const bool fb2 = !fontForCp(chatFont, chatBaked, fallbackFont, cps[j].cp);
            const float cw = charAdvance(fb2 ? fallbackFont : chatFont, fb2 ? nullptr : chatBaked, cps[j].cp, fontSize);
            if (rowEnd > rowBegin && curW + cw > maxW && cps[j].cp != ' ') {
                pushRow(rowEnd);
                curW = 0.f;
            }
            rowEnd = j + 1;
            curW += cw;
            ++j;
        }
        rowW = measureStyledRow(cps, rowBegin, rowEnd - rowBegin, fontSize, chatFont, chatBaked, fallbackFont);
        i = afterSpaces;
    }
    pushRow(rowEnd);
    if (rows.empty()) rows.push_back({0, 0});
    return rows;
}

} // namespace

// ---------------------------------------------------------------------------
// History (all access locked — network/game/render threads all touch it)
// ---------------------------------------------------------------------------

namespace {
// The last plain message bodies we already displayed with an "author > "
// prefix from their TextPacket. The game's own displayClientMessage copy of
// the same line arrives BODY-ONLY (no name), so without this ring it lands
// a second time as a bare duplicate next to the prefixed one.
std::mutex sRecentPlainMutex;
std::vector<std::pair<std::string, unsigned long long>>& recentPlain() {
    static std::vector<std::pair<std::string, unsigned long long>> v;
    return v;
}
void rememberRecentPlain(const std::string& text) {
    std::lock_guard<std::mutex> lock(sRecentPlainMutex);
    auto& v = recentPlain();
    v.push_back({text, GetTickCount64()});
    while (v.size() > 8) v.erase(v.begin());
}
bool wasRecentPlainFromPacket(const std::string& line) {
    std::lock_guard<std::mutex> lock(sRecentPlainMutex);
    auto& v = recentPlain();
    const unsigned long long now = GetTickCount64();
    bool hit = false;
    for (auto it = v.begin(); it != v.end();) {
        if (now - it->second > 3000ull) { it = v.erase(it); continue; }
        if (it->first == line) hit = true;
        ++it;
    }
    return hit;
}
} // namespace

void Chat::AddLine(const std::string& text) {
    if (text.empty()) return;
    // rawtext JSON ({"rawtext":[{"text":"..."}]}) arrives undecoded on some
    // paths — decode it here, the single funnel every chat line passes, so
    // the panel never shows the JSON itself.
    std::string line = text;
    std::string decoded;
    if (decodeRawTextJson(line, &decoded)) line = decoded;
    if (line.empty()) return;
    // The vanilla chat resolves %translation.key tokens through I18n when it
    // draws; this panel keeps the raw text, so resolve them here instead
    // (keys the game does not know come back unchanged). Done outside the
    // history lock — the game call can take its own locks internally.
    line = SDK::I18n::replacePlaceholders(line);
    if (line.empty()) return;
    std::lock_guard<std::mutex> lock(sHistoryMutex);
    auto& ls = lines();
    const unsigned long long now = GetTickCount64();
    if (!ls.empty() && ls.back().text == line && now - ls.back().atMs < 2000ull)
        return;   // TextPacket + displayClientMessage copies of one line
    ls.push_back({line, now});
    if (ls.size() > 200) ls.erase(ls.begin());
}

void Chat::pushSent(const std::string& t) {
    std::lock_guard<std::mutex> lock(sHistoryMutex);
    auto& s = sent();
    if (!s.empty() && s.back() == t) return;
    s.push_back(t);
    if (s.size() > 20) s.erase(s.begin());
}

std::vector<std::string> Chat::sentSnapshot() {
    std::lock_guard<std::mutex> lock(sHistoryMutex);
    return sent();
}

namespace {
// Commands typed in the vanilla chat are executed one render frame later
// (GameHooks::queueChatCommand -> hkLevelRenderer_renderLevel), so by the time
// the reply is printed the chat screen has already closed and the "module owns
// the chat" branch below would swallow it — making a vanilla-chat command look
// like it ran nothing. NoteVanillaCommand() stamps the time; the pass-through
// window then lets the game's own chat show the reply, exactly like the
// chat_screen-open case below it.
std::atomic<unsigned long long> s_vanillaCmdMs{ 0 };
std::atomic<bool> s_vanillaPassthrough{ false };

bool vanillaCommandRecent() {
    const unsigned long long t = s_vanillaCmdMs.load(std::memory_order_relaxed);
    return t != 0 && GetTickCount64() - t < 2000ull;
}
} // namespace

void Chat::NoteVanillaCommand() {
    s_vanillaCmdMs.store(GetTickCount64(), std::memory_order_relaxed);
}

void Chat::ArmVanillaPassthrough() {
    s_vanillaPassthrough.store(true, std::memory_order_release);
}

bool Chat::DisarmVanillaPassthrough() {
    return s_vanillaPassthrough.exchange(false, std::memory_order_acq_rel);
}

bool Chat::RecordMessage(const std::string& line) {
    const bool pass = s_vanillaPassthrough.exchange(false, std::memory_order_acq_rel);
    Chat* self = instance();
    if (!self) return false;
    // Skip the plain copy of a line we already showed with its "author > "
    // prefix from the TextPacket — that would leave the message twice
    // (once bare, once prefixed).
    if (!wasRecentPlainFromPacket(line))
        AddLine(line);   // RAW: § codes stay — the renderer interprets them
    // Translate asked for this line in the real HUD chat as well as the panel.
    if (pass) return false;
    // While the VANILLA chat screen is open, never suppress its copy — the
    // game's chat is on screen and suppressing it made server command replies
    // typed in the vanilla chat look "broken" with the module enabled.
    if (SDK::ScreenView::currentScreenName() == "chat_screen") return false;
    // A command typed in the vanilla chat replies on the NEXT render frame,
    // after that screen closed — let it through for the same reason.
    if (vanillaCommandRecent()) return false;
    return self->IsEnabled();   // otherwise the module owns the chat
}

// Incoming TextPackets: the wire-level source of server chat lines
// (Tip/Popup/JukeboxPopup are toasts, skipped). CommandOutputPackets: command
// replies some servers send exclusively through this packet — see
// SDK/Network/Packets/ChatPackets.h for the layouts.
void Chat::OnPacketReceive(SDK::Packet* packet, int packetId, bool& cancel) {
    // Translate (Delay) cancels the packet before this runs, then prints the
    // translated line itself through AddLine. Recording the wire text here
    // would show the original next to the translation.
    if (cancel) return;
    if (packetId != static_cast<int>(SDK::PacketID::Text) &&
        packetId != static_cast<int>(SDK::PacketID::CommandOutput)) return;
    if (!packet || !sdk_safe_ptr(packet, 0x100)) return;
    void* pkt = static_cast<void*>(packet);

    if (packetId == static_cast<int>(SDK::PacketID::CommandOutput)) {
        unsigned char outType = 0;
        if (!sehReadPacketByte(pkt, SDK::kCmdOutType, &outType)) return;
        if (outType == 1) return;   // CommandOutputType::Silent — no output

        uintptr_t begin = 0, end = 0;
        if (!sehReadPacketPtr(pkt, SDK::kCmdOutMsgVec, &begin)) return;
        if (!sehReadPacketPtr(pkt, SDK::kCmdOutMsgVec + 8, &end)) return;
        if (!begin || end < begin || (end - begin) % SDK::kCmdOutMsgStride != 0) return;
        size_t n = (end - begin) / SDK::kCmdOutMsgStride;
        if (n > 256) n = 256;
        for (size_t i = 0; i < n; ++i) {
            void* elem = (void*)(begin + (uintptr_t)i * SDK::kCmdOutMsgStride);
            if (!sdk_safe_ptr(elem, SDK::kCmdOutMsgStride)) continue;
            char line[768] = {0};
            if (sehFormatCommandOutputMsg(elem, line, sizeof(line)))
                AddLine(std::string(line));
        }
        return;
    }

    char msg[512] = {0};
    char raw[512] = {0};
    char author[128] = {0};
    unsigned char disc = 0xFF, type = 0;
    if (!sehReadPacketByte(pkt, SDK::kTextPktDisc, &disc)) return;
    sehReadPacketByte(pkt, SDK::kTextPktType, &type);

    if (type == 3 || type == 4 || type == 5) return;   // Popup/JukeboxPopup/Tip — toasts

    sehCopyPacketString(pkt, SDK::kTextPktStr, raw, sizeof(raw));
    sehCopyPacketString(pkt, SDK::kTextPktData, msg, sizeof(msg));

    std::string text;
    if (disc == 1) {
        // "author and message" variant: +0xA8 carries the AUTHOR NAME, the
        // body rides the variant storage (+0xC8). Player chat lands here —
        // system lines come with disc 0/2 and no author.
        if (raw[0] != '\0' && msg[0] != '\0') {
            rememberRecentPlain(msg);
            text = std::string(raw) + " > " + std::string(msg);
        } else {
            text = (msg[0] != '\0') ? msg : raw;
        }
    } else {
        text = raw;
    }
    if (text.empty()) return;
    AddLine(text);
}

bool Chat::HandlesOpenKey(int vk) {
    Chat* self = instance();
    if (!self || !self->IsEnabled()) return false;
    const bool isKey   = (vk != 0 && vk == self->GetKeybind());
    const bool isEnter = (vk == VK_RETURN);
    const bool isSlash = (vk == VK_OEM_2);   // vanilla "open command" key
    if (!isKey && !isEnter && !isSlash) return false;
    if (SDK::ScreenView::currentScreenName() != "hud_screen") return false;
    return true;
}

// ---------------------------------------------------------------------------
// Hand-rolled input — the exact ClickGUI-search machinery (ToUnicode layout/
// shift/cyrillic translation, per-glyph reveal animation, sine caret), so the
// typing animation matches the one the search bar already has. No ImGui
// InputText anywhere (its own caret was the "|" duplicate).
// ---------------------------------------------------------------------------
namespace {

// Overshoots slightly past 1, then settles (the ClickGUI reveal curve).
float easeOutBack(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.f;
    const float u = t - 1.f;
    return 1.f + c3 * u * u * u + c1 * u * u;
}

// Insert one codepoint AT the caret (encodeUtf8 lives above, shared with the
// renderer). The reveal slot is inserted at the same index so only the new
// glyph animates.
void chatInputInsert(unsigned int ch) {
    char enc[4] = { 0 };
    const int n = encodeUtf8(ch, enc);
    const size_t len = std::strlen(s_inputBuf);
    if (len + (size_t)n >= sizeof(s_inputBuf)) return;
    if (s_caretCp > cpCountOf(s_inputBuf)) s_caretCp = cpCountOf(s_inputBuf);
    const size_t at = caretByte();
    std::memmove(s_inputBuf + at + n, s_inputBuf + at, len - at + 1);
    std::memcpy(s_inputBuf + at, enc, (size_t)n);
    auto& r = inputReveal();
    if (s_caretCp <= r.size()) r.insert(r.begin() + (ptrdiff_t)s_caretCp, 0.f);
    else r.push_back(0.f);
    ++s_caretCp;
}

void chatInputBackspace() {
    if (s_caretCp == 0) return;
    const size_t b2 = caretByte();   // end of the glyph before the caret
    --s_caretCp;
    const size_t b1 = caretByte();   // its start
    const size_t len = std::strlen(s_inputBuf);
    std::memmove(s_inputBuf + b1, s_inputBuf + b2, len - b2 + 1);
    auto& r = inputReveal();
    if (s_caretCp < r.size()) r.erase(r.begin() + (ptrdiff_t)s_caretCp);
}

// Delete: removes the glyph AFTER the caret, caret stays put.
void chatInputDeleteForward() {
    const size_t len = std::strlen(s_inputBuf);
    if (s_caretCp >= cpCountOf(s_inputBuf)) return;
    const size_t b1 = caretByte();
    size_t b2 = b1 + 1;
    while (b2 < len && ((unsigned char)s_inputBuf[b2] & 0xC0) == 0x80) ++b2;
    std::memmove(s_inputBuf + b1, s_inputBuf + b2, len - b2 + 1);
    auto& r = inputReveal();
    if (s_caretCp < r.size()) r.erase(r.begin() + (ptrdiff_t)s_caretCp);
}

void chatInputSetText(const std::string& text) {
    std::strncpy(s_inputBuf, text.c_str(), sizeof(s_inputBuf) - 1);
    s_inputBuf[sizeof(s_inputBuf) - 1] = '\0';
    inputReveal().clear();
    inputReveal().assign(cpCountOf(s_inputBuf), 1.f);   // no reveal on jump
    s_caretCp = cpCountOf(s_inputBuf);
}

// Ctrl+A select-all latch: while latched, the whole typed line is highlighted
// and the next printable key / backspace / paste replaces it (Java-chat TextBox
// behaviour — there is no caret model to anchor a real selection range).
static bool s_selectAll = false;

static void chatInputClearAll() {
    chatInputSetText("");
    s_selectAll = false;
}

static void chatInputCopyAll() {
    const size_t len = std::strlen(s_inputBuf);
    if (len == 0) return;
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, s_inputBuf, (int)len, nullptr, 0);
    if (wlen <= 0) return;
    if (!OpenClipboard(nullptr)) return;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (size_t)(wlen + 1) * sizeof(wchar_t));
    if (h) {
        wchar_t* w = (wchar_t*)GlobalLock(h);
        if (w) {
            MultiByteToWideChar(CP_UTF8, 0, s_inputBuf, (int)len, w, wlen);
            w[wlen] = L'\0';
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

} // namespace

void Chat::FeedKeyDown(unsigned int vk) {
    Chat* self = instance();
    if (!self || !self->mOpen) return;

    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    // --- select-all combos (Java-style TextBox) ---------------------------
    if (ctrl && vk == 'A') { s_selectAll = true; return; }
    if (ctrl && vk == 'C') { if (s_selectAll) chatInputCopyAll(); return; }

    if (vk == VK_RETURN) {
        self->SendCurrent();
        setInputBuffer("");
        s_selectAll = false;
        self->Close();
        return;
    }
    if (vk == VK_ESCAPE) {
        s_selectAll = false;
        self->Close();
        return;
    }
    if (vk == VK_BACK) {
        // Backspace with a selection wipes the whole line, else one glyph.
        if (s_selectAll) chatInputClearAll();
        else chatInputBackspace();
        return;
    }
    // Caret movement — like a Java chat TextBox: arrows walk one glyph,
    // Home/End jump to the ends. Any of these drops an active selection.
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_HOME || vk == VK_END) {
        s_selectAll = false;
        const size_t cps = cpCountOf(s_inputBuf);
        if (s_caretCp > cps) s_caretCp = cps;
        if (vk == VK_LEFT) { if (s_caretCp > 0) --s_caretCp; }
        else if (vk == VK_RIGHT) { if (s_caretCp < cps) ++s_caretCp; }
        else if (vk == VK_HOME) s_caretCp = 0;
        else s_caretCp = cps;
        return;
    }
    if (vk == VK_DELETE) {
        if (s_selectAll) chatInputClearAll();
        else chatInputDeleteForward();
        return;
    }
    if (vk == VK_UP || vk == VK_DOWN) {
        s_selectAll = false;
        std::vector<std::string> sent = sentSnapshot();
        if (sent.empty() && vk == VK_UP) return;
        int idx = self->HistoryIndex();
        if (vk == VK_UP) {
            if (idx == -1) {
                self->SaveDraft(std::string(s_inputBuf));
                idx = 0;
            } else {
                idx = (std::min)(idx + 1, (int)sent.size() - 1);
            }
        } else {
            if (idx <= -1) return;
            --idx;
        }
        self->SetHistoryIndex(idx);
        const std::string text = (idx < 0) ? self->Draft() : sent[sent.size() - 1 - idx];
        chatInputSetText(text);
        return;
    }
    if (vk == 'V' && ctrl) {
        if (!OpenClipboard(nullptr)) return;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
                if (s_selectAll) chatInputClearAll();   // paste replaces the selection
                for (const wchar_t* p = w; *p; ++p)
                    if (*p < 0x10000u) chatInputInsert((unsigned)*p);
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        return;
    }

    // Only keys that PRODUCE text (ClickGUI search key set).
    const bool text =
        (vk >= '0' && vk <= 'Z') ||
        vk == VK_SPACE ||
        (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) ||
        (vk >= 0xBA && vk <= 0xC0) ||
        (vk >= 0xDB && vk <= 0xDF);
    if (!text) {
        s_selectAll = false;   // any non-editing key drops the selection
        return;
    }

    // Typing with a selection replaces the whole line first.
    if (s_selectAll) chatInputClearAll();

    BYTE keys[256] = {};
    if (!GetKeyboardState(keys)) return;
    wchar_t out[8] = {};
    const int n = ToUnicode((UINT)vk, 0, keys, out, 8, 0);
    for (int i = 0; i < n; ++i)
        if (out[i] >= 0x20 && out[i] != 0x7F) chatInputInsert((unsigned int)out[i]);
}

void Chat::OnEnable() {
    if (sInstance != this) sInstance = this;
}

void Chat::OnDisable() {
    Close();
    // OnRender is skipped for disabled modules — clear the key gate here so a
    // disable mid-typing can never leave the gameplay keys blocked.
    SDK::g_chatInputOpen = false;
}

void Chat::SendCurrent() {
    std::string text = s_inputBuf;
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
    if (text.empty()) return;

    const std::string& prefix = CommandManager::GetInstance().mPrefix;
    if (text.rfind(prefix, 0) == 0) {
        // Client command: it never leaves the machine, so nothing else would
        // ever show the typed line — echo it into the panel's own history.
        AddLine(text);
        CommandManager::GetInstance().ExecuteCommand(text);
    } else if (text[0] == '/') {
        // Server COMMAND — CommandRequestPacket (ID 77). NO local echo: the
        // Java chat does not print the typed command either; the server's
        // reply (TextPacket or CommandOutputPacket — both parsed above)
        // lands in the history on its own.
        SDK::ClientInstance* ci = SDK::ClientInstance::get();
        if (ci) {
            void* ps = ci->getPacketSender();
            if (ps) SDK::chat::sendCommand((void*)ci, ps, text);
        }
    } else {
        SDK::Chat::sendMessage(text);
        // No local echo here either: the server's TextPacket echo lands in
        // the history (displayClientMessage / OnPacketReceive capture it).
    }
    pushSent(text);
}

void Chat::UpdateSettingsVisibility() {
    // "Shadow Opacity" only exists while "Shadow" is on (the ClickGUI skips
    // mVisible == false rows).
    for (auto* s : mSettings) {
        if (!s || s->mName != "Shadow") continue;
        const bool on = static_cast<BoolSetting*>(s)->mValue;
        for (auto* s2 : mSettings)
            if (s2 && s2->mName == "Shadow Opacity") s2->mVisible = on;
    }
}

// ---------------------------------------------------------------------------
// Rendering — Rise "Shader" look: frosted backdrop, styled text, input row
// ---------------------------------------------------------------------------

void Chat::OnRender() {
    // Publish the input-open state BEFORE any early return so the gameplay-key
    // gate (SDK::isGameplayInputActive) can never run stale for a frame.
    SDK::g_chatInputOpen = mOpen;
    if (!IsEnabled()) return;
    if (!HudEditor::HudVisible()) return;
    if (sInstance != this) sInstance = this;
    if (!mOpen) s_selectAll = false;   // selection never survives a close
    UpdateSettingsVisibility();

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 screen = io.DisplaySize;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    const float scale = mScale;
    const float width      = static_cast<NumberSetting*>(mSettings[0])->mValue * scale;
    const float openHeight = static_cast<NumberSetting*>(mSettings[1])->mValue * scale;
    const float closedRows = static_cast<NumberSetting*>(mSettings[2])->mValue;
    const float fadeSecs   = static_cast<NumberSetting*>(mSettings[3])->mValue;
    const float opacity    = static_cast<NumberSetting*>(mSettings[4])->mValue;
    const int   fontIdx    = static_cast<EnumSetting*>(mSettings[5])->mValue;
    const float blurSet    = static_cast<NumberSetting*>(mSettings[6])->mValue;
    const float rounding   = static_cast<NumberSetting*>(mSettings[7])->mValue * scale;
    const bool  shadowOn   = static_cast<BoolSetting*>(mSettings[8])->mValue;
    const float shadowA   = static_cast<NumberSetting*>(mSettings[9])->mValue;
    // Max history lines visible in the upper chat while open (the px height
    // still caps on top of this).
    const float openLines  = static_cast<NumberSetting*>(mSettings[11])->mValue;

    const float fontSize = 16.f * scale;
    const float lineH    = fontSize * 1.05f + 4.f * scale;
    const float padX = 9.f * scale, padY = 8.f * scale;
    const float inputH = 34.f * scale;

    // Fonts: Theme = the currently pushed interface font; Minecraft = Mojangles.
    // Fallback for missing glyphs = the default client font.
    auto& rm = RenderManager::GetInstance();
    ImFont* chatFont = (fontIdx == 1 && rm.minecraftFont) ? rm.minecraftFont : ImGui::GetFont();
    ImFont* fallbackFont = rm.defaultFont ? rm.defaultFont : ImGui::GetFont();
    ImFontBaked* chatBaked = bakedFor(chatFont, fontSize);

    // Snapshot the history under the lock (render must never hold a live
    // reference into a vector another thread can realloc).
    std::vector<std::string> histTexts;
    std::vector<unsigned long long> histTimes;
    {
        std::lock_guard<std::mutex> lock(sHistoryMutex);
        histTexts.reserve(lines().size());
        for (const auto& l : lines()) {
            histTexts.push_back(l.text);
            histTimes.push_back(l.atMs);
        }
    }

    // Decode + wrap each line into rows of styled codepoints; detect links.
    const float maxW = width - padX * 2.f;
    const bool linksOn = mSettings.size() > 10 &&
                         static_cast<BoolSetting*>(mSettings[10])->mValue;
    frameLinkRects().clear();

    std::vector<std::vector<StyledCp>> decoded(histTexts.size());
    std::vector<std::vector<RowSlice>> lineRows(histTexts.size());
    std::vector<std::vector<LinkSpan>> lineSpans(histTexts.size());
    for (size_t li = 0; li < histTexts.size(); ++li) {
        decodeStyled(histTexts[li], &decoded[li]);
        lineRows[li] = wrapStyled(decoded[li], fontSize, maxW, chatFont, chatBaked, fallbackFont);
        if (linksOn) findLinkSpans(decoded[li], &lineSpans[li]);
    }

    // Flatten into visual rows (newest last).
    struct Row { size_t line, begin, count; unsigned long long atMs; };
    std::vector<Row> rows;
    for (size_t li = 0; li < histTexts.size(); ++li)
        for (const RowSlice& rs : lineRows[li])
            rows.push_back({li, rs.begin, rs.count, histTimes[li]});

    // ---- Rise's visibleLines accounting ----------------------------------
    // New arrivals push the counter up (capped by Closed Lines); every Fade
    // Time seconds it drops one line and the panel height eases after it — the
    // top row is progressively wiped by the shrinking clip, exactly Rise's
    // renderChat disappearance. New rows also spike the slide-in offset.
    const unsigned long long now = GetTickCount64();
    const unsigned long long fadeMs = (unsigned long long)(fadeSecs * 1000.0f);
    if (rows.size() > mLastRowCount) {
        const float dRows = (float)(rows.size() - mLastRowCount);
        mVisibleRows += dRows;
        mSlideIn.bump(dRows * lineH, 500.f, Chat::kEaseOutExpo);   // stack pushed DOWN, eases up
    }
    mLastRowCount = rows.size();
    if (mVisibleRows > (float)rows.size()) mVisibleRows = (float)rows.size();
    if (mVisibleRows > closedRows) mVisibleRows = closedRows;
    if (now - mFadeStampMs >= fadeMs) {
        mFadeStampMs = now;
        mVisibleRows = std::max(0.f, mVisibleRows - 1.f);
    }

    // ---- panel height: bottom-pinned animated height (Rise renderChat) --
    // The bottom edge stays at the HUD-editor anchor; the height eases with
    // EASE_OUT_EXPO over 500ms between the open target and the closed
    // visible-rows target.
    const float contentH = (float)rows.size() * lineH;
    const float chromeH = padY * 2.f + (mOpen ? inputH + 6.f * scale : 0.f);
    // Closed with nothing left to show: the height targets ZERO so the panel
    // fully collapses instead of leaving a plate-thin sliver on screen (the
    // "line" that used to sit there while the history was empty).
    // Open: the "Open Lines" knob caps the visible history length (the px
    // height still caps on top of it).
    const float openRowsCap = std::min(contentH, (float)openLines * lineH);
    const float targetH = mOpen ? std::min(openHeight, openRowsCap + chromeH)
                                : (mVisibleRows <= 0.f ? 0.f
                                    : std::min(closedRows * lineH + padY * 2.f,
                                               mVisibleRows * lineH + padY * 2.f));
    mHeightAnim.set(targetH, 500.f, Chat::kEaseOutExpo);
    const float curH = mHeightAnim.tick();

    // HUD-editor bounds stay at the OPEN size — a stable grab rect while the
    // drawn panel animates (the editor outline only ever shows on the
    // pause/chat screens, where the panel can be opened to verify).
    mWidth = width;
    mHeight = openHeight;
    if (curH < 0.5f) return;

    if (mPosY <= 0.f) mPosY = screen.y - 78.f - openHeight;   // leave the hotbar clear
    const float bottomY = mPosY + openHeight;
    const ImVec4 rect(mPosX, bottomY - curH, mPosX + width, bottomY);

    // ---- scroll: the wheel walks the history in PX, eased at Rise's 1%/ms.
    // GATED BY HOVER: the wheel only scrolls the chat when the cursor is over
    // the chat panel — editing some other HUD element (watermark in the
    // HudEditor) must never scroll a chat that sits at the bottom of the
    // screen.
    const float dt = io.DeltaTime;
    if (mOpen) {
        const bool hover = io.MousePos.x >= rect.x && io.MousePos.x <= rect.z &&
                           io.MousePos.y >= rect.y && io.MousePos.y <= rect.w;
        const float wheel = io.MouseWheel;
        if (hover && wheel != 0.f) {
            const float viewH = (bottomY - inputH - 6.f * scale - padY) - rect.y;
            const float maxScroll = std::max(0.f, contentH - viewH);
            mScrollTargetPx = std::clamp(mScrollTargetPx + wheel * lineH * 3.f, 0.f, maxScroll);
        }
    } else {
        mScrollTargetPx = 0.f;   // closed: glide back to the bottom (Rise)
    }
    mScrollPx += (mScrollTargetPx - mScrollPx) * (1.f - std::pow(0.99f, std::max(dt, 0.f) * 1000.f));

    // The input lives in its OWN plate below the message list, with a small
    // gap between them — both plates share the same dim style, so the field
    // reads as part of the chat instead of a black bar inside the list.
    const float gap = 6.f * scale;
    const float msgPlateBottom = mOpen ? bottomY - inputH - gap : bottomY;

    // Rise "Shader" backdrop: frosted blur + dim plate (skipped while the
    // ClickGUI blurs the whole screen anyway).
    bool guiOpen = false;
    if (Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI"))
        guiOpen = gui->IsEnabled();
    const ImVec4 msgRect(rect.x, rect.y, rect.z, msgPlateBottom);
    if (msgRect.w > msgRect.y + 0.5f) {
        if (!guiOpen && blurSet > 0.1f)
            D2D::addBlurOptimized(dl, blurSet, std::optional<ImVec4>(msgRect), rounding);
        if (shadowOn && shadowA > 0.01f) {
            // BLACK soft drop shadow — the same look the rest of the client's
            // panels use (RenderManager plates).
            ImRenderUtils::fillShadowRectangle(msgRect, ImColor(0, 0, 0),
                                               shadowA * 0.7f, rounding,
                                               16.f * scale, 0.55f, dl);
        }
        dl->AddRectFilled(ImVec2(msgRect.x, msgRect.y), ImVec2(msgRect.z, msgRect.w),
                          ImGui::GetColorU32(ImVec4(0.02f, 0.02f, 0.04f, opacity)), rounding);
        dl->AddRect(ImVec2(msgRect.x, msgRect.y), ImVec2(msgRect.z, msgRect.w),
                    ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.07f)), rounding);
    }

    // Message rows, bottom-anchored. The clip is the ANIMATED panel itself, so
    // the shrinking closed height wipes the oldest row exactly like Rise.
    const float msgBottom = bottomY - (mOpen ? (inputH + gap + padY) : padY);
    dl->PushClipRect(ImVec2(rect.x, rect.y), ImVec2(rect.z, msgBottom), true);
    const float slide = mSlideIn.tick();
    for (size_t k = 0; k < rows.size(); ++k) {
        const float rowY = msgBottom - lineH - (float)k * lineH + mScrollPx + slide;
        if (rowY > msgBottom) continue;            // pushed below the view (over-scroll)
        if (rowY + lineH < rect.y) break;         // above the panel — older rows are higher
        const Row& row = rows[rows.size() - 1 - k];
        const std::vector<LinkSpan>& spans = lineSpans[row.line];
        drawStyledRow(dl, ImVec2(rect.x + padX, rowY), decoded[row.line],
                      row.begin, row.count, fontSize, chatFont, fallbackFont, 1.f, now,
                      spans.data(), spans.size());
    }
    dl->PopClipRect();

    // ---- links: hover cursor + click opens the browser ----------------------
    // Clicks only land here while OUR input is open (the WndProc hands the
    // mouse to the game otherwise), so the hit-test is menu-gated.
    if (linksOn && mOpen && !frameLinkRects().empty()) {
        ImGuiIO& ioL = ImGui::GetIO();
        for (const auto& lr : frameLinkRects()) {
            if (ioL.MousePos.x >= lr.rect.x && ioL.MousePos.x <= lr.rect.z &&
                ioL.MousePos.y >= lr.rect.y && ioL.MousePos.y <= lr.rect.w) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    ShellExecuteA(nullptr, "open", lr.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            }
        }
    }

    // ---- input row: Rise's elastic pop-in / expo pop-out -------------------
    // Open: EASE_OUT_ELASTIC scale 0->1 over 850ms (overshoot damped to 40%,
    // the exact d2 formula) with a LINEAR 400ms text alpha. Close: EASE_IN_EXPO
    // 300ms scale->0 + the same alpha fade. The typing cue is ImGui's own
    // caret — no glow.
    if (mOpen) {
        mInputPop.set(1.f, 850.f, Chat::kEaseOutElastic);
        mInputAlpha.set(1.f, 400.f, Chat::kEaseLinear);
    } else {
        mInputPop.set(0.f, 300.f, Chat::kEaseInExpo);
        mInputAlpha.set(0.f, 400.f, Chat::kEaseLinear);
    }
    const float popRaw = mInputPop.tick();
    const float ia = mInputAlpha.tick();
    const float pop = popRaw > 1.f ? 1.f + (popRaw - 1.f) * 0.4f : popRaw;
    if (pop > 0.01f && ia > 0.01f) {
        // The input plate: same dim plate + hairline border as the message
        // list above it, sitting below the gap.
        const float cx = rect.x + width * 0.5f;
        const float cy = bottomY - inputH * 0.5f;
        const ImVec2 a(cx - width * 0.5f * pop, cy - inputH * 0.5f * pop);
        const ImVec2 b(cx + width * 0.5f * pop, cy + inputH * 0.5f * pop);
        if (!guiOpen && blurSet > 0.1f)
            D2D::addBlurOptimized(dl, blurSet,
                                  std::optional<ImVec4>(ImVec4(a.x, a.y, b.x, b.y)), rounding);
        if (shadowOn && shadowA > 0.01f)
            ImRenderUtils::fillShadowRectangle(ImVec4(a.x, a.y, b.x, b.y), ImColor(0, 0, 0),
                                               shadowA * 0.7f * ia, rounding,
                                               16.f * scale, 0.55f, dl);
        dl->AddRectFilled(a, b,
                          ImGui::GetColorU32(ImVec4(0.02f, 0.02f, 0.04f, opacity * ia)),
                          rounding);
        dl->AddRect(a, b,
                    ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.07f * ia)),
                    rounding, 0, 1.f);
    }
    if (!mOpen) return;

    // ---------------- input row (bottom of the open panel) ----------------
    // Hand-drawn, the ClickGUI-search language: per-glyph reveal settle,
    // plain white text and the soft sine caret after the last glyph. No ImGui
    // InputText anywhere — its own caret was the "|" duplicate. The reveal
    // vector is keyed by CODEPOINT (one entry per typed char) — the render
    // walk counts codepoints, not bytes, so multi-byte input (cyrillic)
    // animates the right glyphs. Text is clipped to the field and scrolls
    // left when it outgrows it (Rise TextBox keeps the caret visible).
    {
        const float inX = rect.x + padX;
        const float inY = bottomY - inputH;
        const float inW = (rect.z - rect.x) - padX * 2.f;
        const float textX0 = inX + 7.f * scale;
        const float textY = inY + (inputH - fontSize) * 0.5f;

        if (!chatFont) return;
        const size_t len = std::strlen(s_inputBuf);

        // Total advance of the full buffer (unclipped width, for the scroll).
        const float fullW = chatFont->CalcTextSizeA(fontSize, FLT_MAX, 0.f,
                                                     s_inputBuf, s_inputBuf + len).x;
        const float availW = inW - 14.f * scale - 5.f * scale;

        // The scroll follows the CARET (Rise TextBox): keep the caret's px
        // position inside the visible window, clamped to the text.
        const size_t totalCp = cpCountOf(s_inputBuf);
        if (s_caretCp > totalCp) s_caretCp = totalCp;
        const float caretPx = chatFont->CalcTextSizeA(fontSize, FLT_MAX, 0.f,
                                                      s_inputBuf, s_inputBuf + caretByte()).x;
        if (caretPx - s_inputScrollPx > availW) s_inputScrollPx = caretPx - availW;
        if (caretPx - s_inputScrollPx < 0.f)    s_inputScrollPx = caretPx;
        s_inputScrollPx = std::clamp(s_inputScrollPx, 0.f, std::max(0.f, fullW - availW));
        const float textX = textX0 - s_inputScrollPx;

        // Codepoint count — the reveal vector's real index space.
        size_t cpCount = 0;
        for (size_t i = 0; i < len; ) {
            unsigned int clen = 1;
            while (i + clen < len && ((unsigned char)s_inputBuf[i + clen] & 0xC0) == 0x80) ++clen;
            i += clen;
            ++cpCount;
        }
        if (inputReveal().size() < cpCount) inputReveal().resize(cpCount, 1.f);
        else if (inputReveal().size() > cpCount) inputReveal().resize(cpCount);

        const float revealSpeed = std::clamp(io.DeltaTime * 9.0f, 0.f, 1.f);
        dl->PushClipRect(ImVec2(inX, inY - 2.f * scale), ImVec2(inX + inW, inY + inputH), true);

        // Ctrl+A selection highlight: a soft accent plate behind the whole
        // line while the latch is up (the next key replaces it).
        if (s_selectAll && fullW > 0.f) {
            ImColor sel = ColorUtils::getThemedColor(0);
            dl->AddRectFilled(ImVec2(textX, textY - 1.f * scale),
                              ImVec2(textX + fullW, textY + fontSize + 1.f * scale),
                              ImGui::GetColorU32(ImVec4(sel.Value.x, sel.Value.y, sel.Value.z, 0.30f * ia)),
                              3.f * scale);
        }

        float x = textX;
        size_t cp = 0;
        for (size_t i = 0; i < len; ) {
            float& t = inputReveal()[cp];
            t += (1.f - t) * revealSpeed;
            if (t > 0.996f) t = 1.f;
            const float e = easeOutBack(t);
            unsigned int clen = 1;
            while (i + clen < len && ((unsigned char)s_inputBuf[i + clen] & 0xC0) == 0x80) ++clen;
            const std::string glyph(s_inputBuf + i, clen);
            i += clen;
            ++cp;
            const float w = chatFont->CalcTextSizeA(fontSize, FLT_MAX, 0.f,
                                                    glyph.c_str(), glyph.c_str() + glyph.size()).x;
            const float a = std::clamp(e, 0.f, 1.f);
            if (a > 0.01f)
                dl->AddText(chatFont, fontSize,
                            ImVec2(x, textY + (1.f - e) * 4.f * scale),
                            ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, a * ia)),
                            glyph.c_str(), glyph.c_str() + glyph.size());
            x += w;
        }

        // Soft sine caret AT the caret position (ClickGUI search caret:
        // (sin(ms/150)+1)/2, 0.25 + 0.75 * that, ~1.5px wide). The scroll
        // above keeps it inside the field when the text overflows.
        const float caretA = ((float)std::sin(GetTickCount64() / 150.0) + 1.f) * 0.5f;
        const float ca = (0.25f + 0.75f * caretA) * ia;
        const float caretX = textX + caretPx;
        dl->AddRectFilled(ImVec2(caretX + 3.f * scale, textY),
                          ImVec2(caretX + 4.5f * scale, textY + fontSize),
                          ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, ca)));
        dl->PopClipRect();
    }

}
// RiseAnim — the retargetable eased value (Rise's Animation class verbatim).
// ---------------------------------------------------------------------------
double Chat::RiseAnim::nowMs() {
    return (double)GetTickCount64();
}
float Chat::RiseAnim::tick() {
    if (durMs <= 0.f) { value = target; return value; }
    const float t = (float)((nowMs() - t0) / durMs);   // unclamped — elastic overshoots
    float e;
    switch (ease) {
    case Chat::kEaseInExpo:  e = (t <= 0.f) ? 0.f : std::pow(2.0f, 10.0f * t - 10.0f); break;
    case Chat::kEaseLinear:  e = t; break;
    case Chat::kEaseOutElastic:
        if (t <= 0.f) e = 0.f;
        else if (t >= 1.f) e = 1.f;
        else e = std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * 2.0943951023931953f) * 0.5f + 1.0f;
        break;
    case Chat::kEaseOutExpo:
    default:
        e = (t >= 1.f) ? 1.f : 1.f - std::pow(2.0f, -10.0f * t); break;
    }
    value = from + (target - from) * e;
    if (t >= 1.f) value = target;
    return value;
}
