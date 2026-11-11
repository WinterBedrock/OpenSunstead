#include "NameProtect.h"
#include "../Setting.h"
#include "../../SDK/SDK.h"
#include "../../Core/Logger.h"

#include <string>
#include <cstdio>

namespace {

bool sehWriteStr(void* ptr, const char* data, size_t len) {
    if (!ptr || !data) return false;
    __try {
        reinterpret_cast<std::string*>(ptr)->assign(data, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Replace all occurrences of 'from' with 'to' in 'str'.
void replaceAll(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static constexpr uintptr_t kNameTagOffset = 0xBC0;

static bool readNameTag(void* player, std::string& out) {
    if (!player) return false;
    return sdk_safe_string((void*)((uintptr_t)player + kNameTagOffset), out);
}

static int readPacketId(SDK::Packet* packet) {
    if (!packet || !sdk_safe_ptr(packet, sizeof(void*))) return -1;
    void** vtable = *reinterpret_cast<void***>(packet);
    if (!vtable || !sdk_safe_ptr(vtable, sizeof(void*) * 2)) return -1;
    using getId_t = int(__fastcall*)(SDK::Packet*);
    __try {
        return reinterpret_cast<getId_t>(vtable[1])(packet);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

namespace scoreboard {
    constexpr uintptr_t kEntriesVecOff = 0x30;   // std::vector<ScorePacketInfo>
    constexpr size_t    kEntryStride   = 0x60;   // sizeof(ScorePacketInfo) = 96
    constexpr uintptr_t kIdentityType  = 0x58;   // uint8_t
    constexpr uintptr_t kCustomNameOff = 0x38;   // std::string (FakePlayer)
    constexpr uint8_t   kFakePlayer    = 3;

    // Reads the [begin,end) raw pointers of the entries vector. POD-only.
    static bool readVec(void* packet, void** beginOut, void** endOut) {
        if (!packet || !beginOut || !endOut) return false;
        __try {
            void** vec = reinterpret_cast<void**>((uintptr_t)packet + kEntriesVecOff);
            void* begin = vec[0];
            void* end   = vec[1];
            if (!begin || !end || end < begin) return false;
            const uintptr_t span = (uintptr_t)end - (uintptr_t)begin;
            // A span that is not a whole multiple of the stride means the
            // layout drifted — read nothing rather than corrupt memory.
            if (span % kEntryStride != 0) return false;
            // Guard against absurd sizes (corrupt / mislabeled packet).
            if (span > (kEntryStride * 4096)) return false;
            *beginOut = begin;
            *endOut   = end;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Reads the identity type byte of one entry. POD-only.
    static bool readIdentityType(void* entry, uint8_t* out) {
        if (!entry || !out) return false;
        __try {
            *out = *reinterpret_cast<uint8_t*>((uintptr_t)entry + kIdentityType);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}

static bool readStrVec(void* vecPtr, void** beginOut, void** endOut) {
    if (!vecPtr || !beginOut || !endOut) return false;
    if (!sdk_safe_ptr(vecPtr, sizeof(void*) * 3)) return false;
    __try {
        void** vec = reinterpret_cast<void**>(vecPtr);
        void* begin = vec[0];
        void* end   = vec[1];
        if (!begin || !end || end < begin) return false;
        uintptr_t span = (uintptr_t)end - (uintptr_t)begin;
        if (span == 0) return false;
        if (span % sizeof(std::string) != 0) return false;
        if (span / sizeof(std::string) > 64) return false;      // sane count
        if (!sdk_safe_ptr(begin, span)) return false;
        *beginOut = begin;
        *endOut   = end;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

static const char* kColorCodes[] = {
    "\xC2\xA7" "f",  // 0  White
    "\xC2\xA7" "0",  // 1  Black
    "\xC2\xA7" "8",  // 2  Dark Gray
    "\xC2\xA7" "7",  // 3  Gray
    "\xC2\xA7" "c",  // 4  Red
    "\xC2\xA7" "4",  // 5  Dark Red
    "\xC2\xA7" "6",  // 6  Gold
    "\xC2\xA7" "e",  // 7  Yellow
    "\xC2\xA7" "a",  // 8  Green
    "\xC2\xA7" "2",  // 9  Dark Green
    "\xC2\xA7" "b",  // 10 Aqua
    "\xC2\xA7" "3",  // 11 Dark Aqua
    "\xC2\xA7" "9",  // 12 Blue
    "\xC2\xA7" "1",  // 13 Dark Blue
    "\xC2\xA7" "d",  // 14 Light Purple
    "\xC2\xA7" "5",  // 15 Dark Purple
};

std::string NameProtect::BuildFormattedNick() const {
    std::string nick = static_cast<StringSetting*>(mSettings[0])->mValue;
    int colorIdx = static_cast<EnumSetting*>(mSettings[1])->mValue;
    bool bold = static_cast<BoolSetting*>(mSettings[2])->mValue;
    bool obfuscated = static_cast<BoolSetting*>(mSettings[3])->mValue;
    bool italic = static_cast<BoolSetting*>(mSettings[4])->mValue;

    std::string result;
    result.reserve(64);

    // Color enum index 0 == "None" (no color code emitted). Indices 1..16 map to
    // kColorCodes[0..15] (offset by one because "None" was prepended).
    if (colorIdx >= 1 && colorIdx <= 16)
        result += kColorCodes[colorIdx - 1];
    if (italic)     result += "\xC2\xA7" "o";
    if (bold)       result += "\xC2\xA7" "l";
    if (obfuscated) result += "\xC2\xA7" "k";

    result += StripColorCodes(nick);
    result += "\xC2\xA7" "r";
    return result;
}

std::string NameProtect::StripColorCodes(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        // UTF-8 section sign is 0xC2 0xA7; the next byte is the formatting char.
        if (i + 2 < s.size() &&
            (unsigned char)s[i] == 0xC2 &&
            (unsigned char)s[i + 1] == 0xA7) {
            i += 2;  // skip section sign (2 bytes) + formatting char (1 byte)
        } else {
            result += s[i];
        }
    }
    return result;
}

void NameProtect::OnEnable() {
    auto* ci = SDK::ClientInstance::get();
    if (!ci) return;
    auto* player = ci->getLocalPlayer();
    if (!player || !sdk_safe_ptr(player, 0x230)) return;

    std::string cur;
    if (readNameTag(player, cur) && !cur.empty()) mOriginalName = cur;
    else mOriginalName = player->getRawName();
    mHasOriginal = !mOriginalName.empty();
}

void NameProtect::OnDisable() {
    if (!mHasOriginal) return;

    auto* ci = SDK::ClientInstance::get();
    if (!ci) return;
    auto* player = ci->getLocalPlayer();
    if (!player || !sdk_safe_ptr(player, 0x230)) return;

    // Restore the original name at Actor+0xBC0
    sehWriteStr((void*)((uintptr_t)player + kNameTagOffset),
                mOriginalName.data(), mOriginalName.size());

    mHasOriginal = false;
    mOriginalName.clear();
}

void NameProtect::OnTick() {
    auto* ci = SDK::ClientInstance::get();
    if (!ci) return;
    auto* player = ci->getLocalPlayer();
    if (!player || !sdk_safe_ptr(player, 0x230)) return;

    std::string currentName;
    if (!readNameTag(player, currentName) || currentName.empty()) currentName = player->getRawName();
    if (currentName.empty()) return;

    std::string formattedNick = BuildFormattedNick();

    // todo fix(only in chat,scoreboard): change name in module settings bug (needs relog to server)
    std::string nickBare = StripColorCodes(static_cast<StringSetting*>(mSettings[0])->mValue);
    std::string curBare  = StripColorCodes(currentName);

    if (!mHasOriginal) {
        mOriginalName = currentName;
        mHasOriginal = true;
    } else if (curBare != nickBare && currentName != mOriginalName) {
        // Genuinely a game-set name (not any variant of our own nick).
        mOriginalName = currentName;
    }

    // Overwrite the name field with the formatted nick
    sehWriteStr((void*)((uintptr_t)player + kNameTagOffset),
                formattedNick.data(), formattedNick.size());
}

void NameProtect::ReplaceScoreboardNames(SDK::Packet* packet) {
    if (!packet) return;

    void* begin = nullptr;
    void* end = nullptr;
    if (!scoreboard::readVec(packet, &begin, &end)) return;

    std::string formattedNick = BuildFormattedNick();

    for (uintptr_t entry = (uintptr_t)begin;
         entry + scoreboard::kEntryStride <= (uintptr_t)end;
         entry += scoreboard::kEntryStride) {

        uint8_t type = 0;
        if (!scoreboard::readIdentityType((void*)entry, &type)) continue;
        if (type != scoreboard::kFakePlayer) continue;  // only fake-player names

        void* namePtr = (void*)(entry + scoreboard::kCustomNameOff);
        std::string customName;
        if (!sdk_safe_string(namePtr, customName) || customName.empty()) continue;

        if (customName.find(mOriginalName) != std::string::npos) {
            replaceAll(customName, mOriginalName, formattedNick);
            sehWriteStr(namePtr, customName.data(), customName.size());
        }
    }
}

void NameProtect::OnPacketReceive(SDK::Packet* packet, int packetId, bool& cancel) {
    if (!mHasOriginal || mOriginalName.empty()) return;
    if (!packet) return;

    // Prefer the authoritative id from the packet vtable; fall back to hook id.
    int realId = readPacketId(packet);
    int id = (realId >= 0) ? realId : packetId;

    // --- SetScore (108): rewrite fake-player scoreboard entry names ----------
    if (id == 108) {
        bool doScoreboard = static_cast<BoolSetting*>(mSettings[5])->mValue;
        if (doScoreboard) ReplaceScoreboardNames(packet);
        return;
    }

    // --- TextPacket (0x09): rewrite chat author/message ----------------------
    if (id != (int)SDK::PacketID::Text) return;

    // Chat replacement is gated behind the "Messages" toggle (setting [6]).
    if (!static_cast<BoolSetting*>(mSettings[6])->mValue) return;

    std::string formattedNick = BuildFormattedNick();
    RewriteTextPacketNames(packet, formattedNick);
}

void NameProtect::RewriteTextPacketNames(SDK::Packet* packet, const std::string& nick) {
    if (!packet) return;
    uintptr_t base = (uintptr_t)packet;

    for (uintptr_t off = 0x30; off < 0x120; off += 0x08) {
        std::string s;
        if (!sdk_safe_string((void*)(base + off), s) || s.empty()) continue;
        if (s.find(mOriginalName) == std::string::npos) continue;
        replaceAll(s, mOriginalName, nick);
        sehWriteStr((void*)(base + off), s.data(), s.size());
    }

    for (uintptr_t off = 0x30; off < 0x120; off += 0x08) {
        void* begin = nullptr;
        void* end = nullptr;
        if (!readStrVec((void*)(base + off), &begin, &end)) continue;

        // Walk each std::string element (0x20 stride) and rewrite.
        for (uintptr_t e = (uintptr_t)begin; e + sizeof(std::string) <= (uintptr_t)end;
             e += sizeof(std::string)) {
            std::string s;
            if (!sdk_safe_string((void*)e, s) || s.empty()) continue;
            if (s.find(mOriginalName) == std::string::npos) continue;
            replaceAll(s, mOriginalName, nick);
            sehWriteStr((void*)e, s.data(), s.size());
        }
        // Don't break: there can be more than one string vector; rewrite all.
    }
}
