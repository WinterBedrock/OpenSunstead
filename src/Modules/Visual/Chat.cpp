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

bool SDK::g_chatInputOpen = false;

static char s_inputBuf[512] = { 0 };
char* Chat::InputBuffer() { return s_inputBuf; }

static std::vector<float>& inputReveal() {
    static std::vector<float> r;
    return r;
}

static size_t s_caretCp = 0;
static float s_inputScrollPx = 0.f;

static size_t cpCountOf(const char* s) {
    size_t n = 0;
    for (size_t i = 0; s[i]; ++i)
        if (((unsigned char)s[i] & 0xC0) != 0x80) ++n;
    return n;
}

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
using SDK::sehCopyPacketString;
using SDK::sehReadPacketByte;
using SDK::sehReadPacketPtr;
using SDK::sehFormatCommandOutputMsg;

struct StyledCp {
    unsigned int cp;      // UTF-8 codepoint
    int style;           // -1 default, -2 theme gradient, else palette index
    size_t off;          // byte offset in the raw line (maps links to glyphs)
};
struct ChatStyleRow {
    std::vector<StyledCp> cps;
    unsigned long long atMs;
};

static int utf8Cp(const char* s, size_t remain, unsigned int* out) {
    const unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *out = c; return 1; }
    if ((c & 0xE0) == 0xC0 && remain >= 2) { *out = ((c & 0x1F) << 6) | (s[1] & 0x3F); return 2; }
    if ((c & 0xF0) == 0xE0 && remain >= 3) { *out = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); return 3; }
    if ((c & 0xF8) == 0xF0 && remain >= 4) { *out = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); return 4; }
    *out = c;
    return 1;
}

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

static void decodeStyled(const std::string& raw, std::vector<StyledCp>* out) {
    int style = -1;
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

static int encodeUtf8(unsigned int cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

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

struct LinkSpan {
    size_t cpBegin = 0, cpEnd = 0;
    std::string url;
};

struct LinkRect { ImVec4 rect; std::string url; };
static std::vector<LinkRect>& frameLinkRects() {
    static std::vector<LinkRect> v;
    return v;
}

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
                if (j < n && cps[j].cp == '/') {
                    ++j;
                    while (j < n && !stop(cps[j].cp)) ++j;
                }
                e = j;
                isLink = true;
            }
        }
        if (isLink) {
            while (e > b) {
                const unsigned int c = cps[e - 1].cp;
                if (c == '.' || c == ',' || c == '!' || c == '?' || c == ')' ||
                    c == ']' || c == '}' || c == ';' || c == ':' || c == '\'' || c == '"')
                    --e;
                else break;
            }
            if (e > b && (e - b > 7 || !hasScheme)) {
                std::string url;
                for (size_t k = b; k < e; ++k) {
                    char u8[8];
                    const int m = encodeUtf8(cps[k].cp, u8);
                    url.append(u8, m);
                }
                if (!hasScheme) url = "https://" + url;
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

static ImFontBaked* bakedFor(ImFont* font, float fontSize) {
    if (!font || fontSize < 1.0f) return nullptr;
    return font->GetFontBaked(fontSize);
}

static ImFont* fontForCp(ImFont* chatFont, ImFontBaked* chatBaked,
                         ImFont* fallbackFont, unsigned int cp) {
    if (chatFont && chatBaked && cp <= 0xFFFF) {
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
            ImColor tc = ColorUtils::getThemedColor(0);
            tc.Value.w = 1.0f;
            col = (ImU32)tc;
        } else if (sc.style == -2) {
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

struct RowSlice { size_t begin, count; };
static std::vector<RowSlice> wrapStyled(const std::vector<StyledCp>& cps,
                                        float fontSize, float maxW,
                                        ImFont* chatFont, ImFontBaked* chatBaked,
                                        ImFont* fallbackFont) {
    std::vector<RowSlice> rows;
    size_t i = 0;
    size_t rowBegin = 0, rowEnd = 0;
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

namespace {
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
    std::string line = text;
    std::string decoded;
    if (decodeRawTextJson(line, &decoded)) line = decoded;
    if (line.empty()) return;
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
    if (!wasRecentPlainFromPacket(line))
        AddLine(line);
    if (pass) return false;
    if (SDK::ScreenView::currentScreenName() == "chat_screen") return false;
    if (vanillaCommandRecent()) return false;
    return self->IsEnabled();
}

void Chat::OnPacketReceive(SDK::Packet* packet, int packetId, bool& cancel) {
    if (cancel) return;
    if (packetId != static_cast<int>(SDK::PacketID::Text) &&
        packetId != static_cast<int>(SDK::PacketID::CommandOutput)) return;
    if (!packet || !sdk_safe_ptr(packet, 0x100)) return;
    void* pkt = static_cast<void*>(packet);

    if (packetId == static_cast<int>(SDK::PacketID::CommandOutput)) {
        unsigned char outType = 0;
        if (!sehReadPacketByte(pkt, SDK::kCmdOutType, &outType)) return;
        if (outType == 1) return;

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

    if (type == 3 || type == 4 || type == 5) return;   // Popup/JukeboxPopup/Tip

    sehCopyPacketString(pkt, SDK::kTextPktStr, raw, sizeof(raw));
    sehCopyPacketString(pkt, SDK::kTextPktData, msg, sizeof(msg));

    std::string text;
    if (disc == 1) {
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

namespace {

float easeOutBack(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.f;
    const float u = t - 1.f;
    return 1.f + c3 * u * u * u + c1 * u * u;
}

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
    const size_t b2 = caretByte();
    --s_caretCp;
    const size_t b1 = caretByte();
    const size_t len = std::strlen(s_inputBuf);
    std::memmove(s_inputBuf + b1, s_inputBuf + b2, len - b2 + 1);
    auto& r = inputReveal();
    if (s_caretCp < r.size()) r.erase(r.begin() + (ptrdiff_t)s_caretCp);
}

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
        if (s_selectAll) chatInputClearAll();
        else chatInputBackspace();
        return;
    }
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
                if (s_selectAll) chatInputClearAll();
                for (const wchar_t* p = w; *p; ++p)
                    if (*p < 0x10000u) chatInputInsert((unsigned)*p);
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        return;
    }

    const bool text =
        (vk >= '0' && vk <= 'Z') ||
        vk == VK_SPACE ||
        (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) ||
        (vk >= 0xBA && vk <= 0xC0) ||
        (vk >= 0xDB && vk <= 0xDF);
    if (!text) {
        s_selectAll = false;
        return;
    }

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
    SDK::g_chatInputOpen = false;
}

void Chat::SendCurrent() {
    std::string text = s_inputBuf;
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
    if (text.empty()) return;

    const std::string& prefix = CommandManager::GetInstance().mPrefix;
    if (text.rfind(prefix, 0) == 0) {
        AddLine(text);
        CommandManager::GetInstance().ExecuteCommand(text);
    } else if (text[0] == '/') {
        SDK::ClientInstance* ci = SDK::ClientInstance::get();
        if (ci) {
            void* ps = ci->getPacketSender();
            if (ps) SDK::chat::sendCommand((void*)ci, ps, text);
        }
    } else {
        SDK::Chat::sendMessage(text);
    }
    pushSent(text);
}

void Chat::UpdateSettingsVisibility() {
    for (auto* s : mSettings) {
        if (!s || s->mName != "Shadow") continue;
        const bool on = static_cast<BoolSetting*>(s)->mValue;
        for (auto* s2 : mSettings)
            if (s2 && s2->mName == "Shadow Opacity") s2->mVisible = on;
    }
}

void Chat::OnRender() {
    SDK::g_chatInputOpen = mOpen;
    if (!IsEnabled()) return;
    if (!HudEditor::HudVisible()) return;
    if (sInstance != this) sInstance = this;
    if (!mOpen) s_selectAll = false;
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
    const float openLines  = static_cast<NumberSetting*>(mSettings[11])->mValue;

    const float fontSize = 16.f * scale;
    const float lineH    = fontSize * 1.05f + 4.f * scale;
    const float padX = 9.f * scale, padY = 8.f * scale;
    const float inputH = 34.f * scale;

    auto& rm = RenderManager::GetInstance();
    ImFont* chatFont = (fontIdx == 1 && rm.minecraftFont) ? rm.minecraftFont : ImGui::GetFont();
    ImFont* fallbackFont = rm.defaultFont ? rm.defaultFont : ImGui::GetFont();
    ImFontBaked* chatBaked = bakedFor(chatFont, fontSize);

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

    struct Row { size_t line, begin, count; unsigned long long atMs; };
    std::vector<Row> rows;
    for (size_t li = 0; li < histTexts.size(); ++li)
        for (const RowSlice& rs : lineRows[li])
            rows.push_back({li, rs.begin, rs.count, histTimes[li]});

    const unsigned long long now = GetTickCount64();
    const unsigned long long fadeMs = (unsigned long long)(fadeSecs * 1000.0f);
    if (rows.size() > mLastRowCount) {
        const float dRows = (float)(rows.size() - mLastRowCount);
        mVisibleRows += dRows;
        mSlideIn.bump(dRows * lineH, 500.f, Chat::kEaseOutExpo);
    }
    mLastRowCount = rows.size();
    if (mVisibleRows > (float)rows.size()) mVisibleRows = (float)rows.size();
    if (mVisibleRows > closedRows) mVisibleRows = closedRows;
    if (now - mFadeStampMs >= fadeMs) {
        mFadeStampMs = now;
        mVisibleRows = std::max(0.f, mVisibleRows - 1.f);
    }

    const float contentH = (float)rows.size() * lineH;
    const float chromeH = padY * 2.f + (mOpen ? inputH + 6.f * scale : 0.f);
    const float openRowsCap = std::min(contentH, (float)openLines * lineH);
    const float targetH = mOpen ? std::min(openHeight, openRowsCap + chromeH)
                                : (mVisibleRows <= 0.f ? 0.f
                                    : std::min(closedRows * lineH + padY * 2.f,
                                               mVisibleRows * lineH + padY * 2.f));
    mHeightAnim.set(targetH, 500.f, Chat::kEaseOutExpo);
    const float curH = mHeightAnim.tick();

    mWidth = width;
    mHeight = openHeight;
    if (curH < 0.5f) return;

    if (mPosY <= 0.f) mPosY = screen.y - 78.f - openHeight;
    const float bottomY = mPosY + openHeight;
    const ImVec4 rect(mPosX, bottomY - curH, mPosX + width, bottomY);

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
        mScrollTargetPx = 0.f;
    }
    mScrollPx += (mScrollTargetPx - mScrollPx) * (1.f - std::pow(0.99f, std::max(dt, 0.f) * 1000.f));

    const float gap = 6.f * scale;
    const float msgPlateBottom = mOpen ? bottomY - inputH - gap : bottomY;

    bool guiOpen = false;
    if (Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI"))
        guiOpen = gui->IsEnabled();
    const ImVec4 msgRect(rect.x, rect.y, rect.z, msgPlateBottom);
    if (msgRect.w > msgRect.y + 0.5f) {
        if (!guiOpen && blurSet > 0.1f)
            D2D::addBlurOptimized(dl, blurSet, std::optional<ImVec4>(msgRect), rounding);
        if (shadowOn && shadowA > 0.01f) {
            ImRenderUtils::fillShadowRectangle(msgRect, ImColor(0, 0, 0),
                                               shadowA * 0.7f, rounding,
                                               16.f * scale, 0.55f, dl);
        }
        dl->AddRectFilled(ImVec2(msgRect.x, msgRect.y), ImVec2(msgRect.z, msgRect.w),
                          ImGui::GetColorU32(ImVec4(0.02f, 0.02f, 0.04f, opacity)), rounding);
        dl->AddRect(ImVec2(msgRect.x, msgRect.y), ImVec2(msgRect.z, msgRect.w),
                    ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.07f)), rounding);
    }

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

    {
        const float inX = rect.x + padX;
        const float inY = bottomY - inputH;
        const float inW = (rect.z - rect.x) - padX * 2.f;
        const float textX0 = inX + 7.f * scale;
        const float textY = inY + (inputH - fontSize) * 0.5f;

        if (!chatFont) return;
        const size_t len = std::strlen(s_inputBuf);

        const float fullW = chatFont->CalcTextSizeA(fontSize, FLT_MAX, 0.f,
                                                     s_inputBuf, s_inputBuf + len).x;
        const float availW = inW - 14.f * scale - 5.f * scale;

        const size_t totalCp = cpCountOf(s_inputBuf);
        if (s_caretCp > totalCp) s_caretCp = totalCp;
        const float caretPx = chatFont->CalcTextSizeA(fontSize, FLT_MAX, 0.f,
                                                      s_inputBuf, s_inputBuf + caretByte()).x;
        if (caretPx - s_inputScrollPx > availW) s_inputScrollPx = caretPx - availW;
        if (caretPx - s_inputScrollPx < 0.f)    s_inputScrollPx = caretPx;
        s_inputScrollPx = std::clamp(s_inputScrollPx, 0.f, std::max(0.f, fullW - availW));
        const float textX = textX0 - s_inputScrollPx;

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

        const float caretA = ((float)std::sin(GetTickCount64() / 150.0) + 1.f) * 0.5f;
        const float ca = (0.25f + 0.75f * caretA) * ia;
        const float caretX = textX + caretPx;
        dl->AddRectFilled(ImVec2(caretX + 3.f * scale, textY),
                          ImVec2(caretX + 4.5f * scale, textY + fontSize),
                          ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, ca)));
        dl->PopClipRect();
    }

}
double Chat::RiseAnim::nowMs() {
    return (double)GetTickCount64();
}
float Chat::RiseAnim::tick() {
    if (durMs <= 0.f) { value = target; return value; }
    const float t = (float)((nowMs() - t0) / durMs);
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
