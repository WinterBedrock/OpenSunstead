#include "ChatLogger.h"
#include "../Setting.h"
#include "../../Utils/MCSDK.h"
#include <string>
#include <vector>
#include <ctime>
#include <cstdio>
#include <cctype>
#include <Windows.h>
#include <cstdlib>

// ---------------------------------------------------------------------------
// TextPacket layout (1.26.4x, verified against the current Latite SDK — the
// 1.26.30 layout this parser used before is GONE, which is why nothing was
// logged: every read came back empty and the packet was skipped):
//   base Packet = 0x30 bytes
//   +0x30  bool  translationNeeded (+pad)
//   +0x38  std::string xboxUserId
//   +0x58  std::string platformChatId
//   +0x78  std::optional<std::string> filteredMessage   (0x28)
//   +0xA0  uint8_t type (TextPacketType)
//   +0xA8  std::string str        (message OR author, per the variant below)
//   +0xC8  std::variant<monostate, string, vector<string>> body storage (0x20)
//   +0xE8  uint8_t variant index  (0 = none, 1 = string, 2 = vector)
// Semantics (Latite TextPacket::getMessage/getAuthor):
//   index 0: message = str
//   index 1: author = str, message = variant storage (string)
//   index 2: message = str, params = variant storage (vector<string>)
// Every read goes through sdk_safe_* so a recycled/garbage packet degrades to
// an empty read instead of faulting the network thread.
// ---------------------------------------------------------------------------
namespace {

// TextPacketType — controls how mMessage / mParams should be interpreted.
enum : uint8_t {
    TPT_Raw = 0, TPT_Chat = 1, TPT_Translate = 2, TPT_Popup = 3,
    TPT_JukeboxPopup = 4, TPT_Tip = 5, TPT_System = 6, TPT_Whisper = 7,
    TPT_Announcement = 8, TPT_TextObjectWhisper = 9, TPT_TextObject = 10,
    TPT_TextObjectAnnouncement = 11,
};

// Strip Minecraft "§x" formatting codes (UTF-8 § == 0xC2 0xA7).
std::string stripFormatting(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if ((unsigned char)in[i] == 0xC2 && i + 1 < in.size() && (unsigned char)in[i + 1] == 0xA7) {
            i += 2; // skip § and the following code char
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

// A string that is a bare translation key ("chat.type.text", "commands.xp.success")
// or a pure numeric id (an XUID leaking through) should never be logged as-is.
bool looksLikeTranslationKey(const std::string& s) {
    if (s.empty()) return false;
    bool hasDot = false;
    for (char c : s) {
        if (c == '.') { hasDot = true; continue; }
        if (c == '_' || c == '%') continue;
        if (!islower((unsigned char)c) && !isdigit((unsigned char)c)) return false; // has spaces/caps -> real text
    }
    return hasDot; // dotted, all-lowercase, no spaces == a key
}

bool isAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!isdigit((unsigned char)c)) return false;
    return true;
}

// Extract the concatenation of every "text":"..." value from a rawtext JSON
// envelope (TextObject packets). Handles simple escaping. Returns "" if the
// input is not JSON.
std::string extractRawtext(const std::string& in) {
    if (in.find("\"text\"") == std::string::npos) return "";
    std::string out;
    size_t pos = 0;
    const std::string key = "\"text\"";
    while ((pos = in.find(key, pos)) != std::string::npos) {
        pos += key.size();
        while (pos < in.size() && (in[pos] == ' ' || in[pos] == ':')) ++pos;
        if (pos >= in.size() || in[pos] != '"') continue;
        ++pos;
        while (pos < in.size() && in[pos] != '"') {
            if (in[pos] == '\\' && pos + 1 < in.size()) {
                char n = in[pos + 1];
                if (n == 'n') out.push_back('\n');
                else if (n == 't') out.push_back(' ');
                else out.push_back(n);
                pos += 2;
            } else {
                out.push_back(in[pos]);
                ++pos;
            }
        }
        ++pos;
    }
    return out;
}

std::string nowStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

std::string logFilePath() {
    char* appdata = nullptr;
    size_t len = 0;
    if (_dupenv_s(&appdata, &len, "APPDATA") != 0 || !appdata) return "";
    std::string dir = appdata;
    free(appdata);
    dir += "\\Sunstead";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\chat_log.txt";
}

void appendToFile(const std::string& line) {
    std::string path = logFilePath();
    if (path.empty()) return;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "ab") != 0 || !f) return;
    fwrite(line.data(), 1, line.size(), f);
    fputc('\n', f);
    fclose(f);
}

bool readPacketStr(SDK::Packet* packet, size_t off, std::string& out) {
    void* s = (void*)(reinterpret_cast<const char*>(packet) + off);
    return sdk_safe_string(s, out);
}

// 1.26.4x layout offsets (see the header comment).
constexpr size_t OFF_TYPE        = 0xA0;
constexpr size_t OFF_STR         = 0xA8;
constexpr size_t OFF_BODY        = 0xC8; // variant storage (string or vector)
constexpr size_t OFF_VARIANT_IDX = 0xE8;

uint8_t readType(SDK::Packet* packet) {
    if (!sdk_safe_ptr(packet, OFF_TYPE + 1)) return TPT_Chat;
    return *reinterpret_cast<uint8_t*>((uintptr_t)packet + OFF_TYPE);
}

uint8_t readVariantIndex(SDK::Packet* packet) {
    if (!sdk_safe_ptr(packet, OFF_VARIANT_IDX + 1)) return 0;
    return *reinterpret_cast<uint8_t*>((uintptr_t)packet + OFF_VARIANT_IDX);
}

// Read up to `cap` entries of the std::vector<std::string> body (variant==2).
std::vector<std::string> readBodyParams(SDK::Packet* packet, size_t cap = 8) {
    std::vector<std::string> out;
    if (!sdk_safe_ptr(packet, OFF_BODY + sizeof(void*) * 2)) return out;
    uintptr_t begin = *reinterpret_cast<uintptr_t*>((uintptr_t)packet + OFF_BODY);
    uintptr_t end   = *reinterpret_cast<uintptr_t*>((uintptr_t)packet + OFF_BODY + sizeof(void*));
    if (!begin || end <= begin) return out;
    const size_t strSize = sizeof(std::string); // MSVC == 0x20
    size_t count = (end - begin) / strSize;
    if (count == 0 || count > 64) return out;   // sanity: not a string array
    if (count > cap) count = cap;
    for (size_t i = 0; i < count; ++i) {
        std::string s;
        if (sdk_safe_string((void*)(begin + i * strSize), s) && !s.empty())
            out.push_back(stripFormatting(s));
    }
    return out;
}

} // namespace

void ChatLogger::OnPacketReceive(SDK::Packet* packet, int packetId, bool& /*cancel*/) {
    if (packetId != 0x09 || !packet) return;

    const bool logSystem = static_cast<BoolSetting*>(mSettings[1])->mValue;
    const uint8_t type = readType(packet);
    const uint8_t vidx = readVariantIndex(packet);

    // Variant body selects where author / message / params live.
    std::string strField, bodyStr;
    readPacketStr(packet, OFF_STR, strField);
    strField = stripFormatting(strField);

    std::string author, message;
    std::vector<std::string> params;
    if (vidx == 1) {           // author in str, message in the variant storage
        author = strField;
        readPacketStr(packet, OFF_BODY, bodyStr);
        message = stripFormatting(bodyStr);
    } else if (vidx == 2) {    // message in str, params in the variant storage
        message = strField;
        params  = readBodyParams(packet);
    } else {                   // plain body: message only
        message = strField;
    }

    std::string line;

    switch (type) {
    case TPT_Chat:
    case TPT_Whisper: {
        if (message.empty() || isAllDigits(message)) return; // no real text -> skip
        line = author.empty() ? message : (author + ": " + message);
        break;
    }
    case TPT_TextObject:
    case TPT_TextObjectWhisper:
    case TPT_TextObjectAnnouncement: {
        // message is a JSON rawtext envelope; pull the visible text out.
        std::string text = extractRawtext(message);
        if (text.empty()) text = message;              // some servers send plain text here
        if (text.empty() || looksLikeTranslationKey(text) || isAllDigits(text)) return;
        line = text;
        break;
    }
    case TPT_Translate: {
        // The readable pieces are the body params; str is the translation key.
        if (params.empty()) {
            if (message.empty() || looksLikeTranslationKey(message)) return;
            line = message;                            // plain text fallback
            break;
        }
        if (params.size() >= 2)
            line = params[0] + ": " + params[1];       // typical chat.type.text form
        else
            line = params[0];
        break;
    }
    case TPT_Raw:
    case TPT_System:
    case TPT_Announcement:
    case TPT_Tip:
    case TPT_Popup:
    case TPT_JukeboxPopup: {
        if (!logSystem) return;
        std::string text = message.empty() ? author : message;
        if (text.empty() || looksLikeTranslationKey(text) || isAllDigits(text)) return;
        line = text;
        break;
    }
    default:
        return;
    }

    if (line.empty()) return;

    if (static_cast<BoolSetting*>(mSettings[0])->mValue)   // Timestamps
        appendToFile("[" + nowStamp() + "] " + line);
    else
        appendToFile(line);
}
