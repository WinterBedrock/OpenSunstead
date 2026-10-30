#include "NameProtect.h"
#include "../Setting.h"
#include "../../SDK/SDK.h"
#include "../../Core/Logger.h"

#include <string>
#include <cstdio>

namespace {

// SEH-guarded string write to a raw pointer.
// Must be a plain POD function: __try can't live in a frame that owns C++
// objects with destructors.  All params are POD / raw pointers.
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

// Reads a packet's REAL id from its own vtable (getId is vtable index 1),
// instead of trusting the receive-hook's deduped handler->id map, which can
// mislabel packets that share a dispatcher handler.  Same pattern as Velocity.
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

// ---- SetScore (scoreboard) packet layout helpers ----------------------------
//
// MCBE SetScorePacket, layout REVERSED ON THE LIVE BUILD (1.26.50.04 IDB;
// entry-copy code byte-identical on the live 1.26.51.01 exe — FakePlayer
// entry copy sub_143B49020 RVA 0x3B49020 / 51.01 RVA 0x3B4A150). SetScore is
// a cereal-only packet now; the old 1.26.30.05 offsets (action@+0x30,
// entries@+0x38, stride 0x68) were stale: reading the vector at +0x38 sees
// end/cap instead of begin/end, so a right-sized vector looked EMPTY.
//   +0x30  std::vector<ScorePacketInfo>   entries  (begin/end/cap pointers;
//          send-side ctor sub_14055F030, cereal read vfn sub_143A693D0)
//
// ScorePacketInfo — 0x60 / 96 bytes per entry (stride from vector copy
// sub_143B48650 `+= 96`; per-type copy/destroy switch on the +0x58 byte;
// dtor sub_14056A9C0 frees the +0x38 std::string only for type 3):
//   +0x00  int64_t     scoreboardId   ("Id")
//   +0x10  std::string objectiveName  ("Objective Name")
//   +0x30  int32_t     score
//   +0x38  std::string customName     (FakePlayer; Player/Entity keep the
//                                      int64 actorUniqueId there instead)
//   +0x58  uint8_t     identityType   (1=Player, 2=Entity, 3=FakePlayer)
//
// Only FakePlayer entries carry a printable name we can spoof; Player/Entity
// entries resolve their name through the actor list, which the on-tick 0xBC0
// write already covers. We only rewrite the +0x38 customName std::string.
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

// Safely reads a candidate std::vector<std::string> at 'vecPtr', returning its
// [begin,end) element pointers only if the layout is plausible:
//   - begin/end non-null, end >= begin
//   - byte span is a whole multiple of sizeof(std::string) (0x20)
//   - element count is sane (<= 64) so garbage 3-pointer patterns are rejected
// POD-only (no C++ objects) so __try is legal. Used to locate TextPacket::params
// without hardcoding its offset.
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

// MCBE color codes.  \u00a7 in UTF-8 is 0xC2 0xA7 (2 bytes).
// String concatenation ("\xC2\xA7" "f") prevents the \x escape from
// consuming hex-digit color codes like f, 0, a, b, c, d, e.
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

// Walks every ScorePacketInfo entry in a SetScore packet and, for FakePlayer
// entries whose customName contains our real name, substitutes the nick. This
// is what makes the spoofed name appear on server sidebars / below-name lines,
// matching the FlarialSRC name-spoof behaviour.
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

    // TextPacket layout (base Packet = 0x30 bytes, verified getId()==9 in
    // Minecraft.Windows.exe 1.26.30.05):
    //   +0x30  type            (uint8_t: 0=Raw, 1=Chat, 6=System, 7=Whisper...)
    //   +0x38  author          (std::string)
    //   +0x58  message         (std::string)
    //   +0x78  filteredMessage (std::string)
    //   +0xA0  params          (std::vector<std::string>)  <-- used by TRANSLATE
    //                          messages (type 2), e.g. "chat.type.text" with
    //                          params[0]=name, params[1]=message. Many servers
    //                          route player chat through this path, which the
    //                          old fixed-offset scan of only 0x38/0x58/0x78
    //                          MISSED entirely -- that's why chat wasn't being
    //                          rewritten while the scoreboard was.
    //
    // Rather than trust exact offsets (which drift), we do a robust two-part
    // scan that finds the real name wherever the server put it:
    //   (1) brute-scan the whole packet body as candidate std::string fields, and
    //   (2) walk the params vector<string> and rewrite each element.
    std::string formattedNick = BuildFormattedNick();
    RewriteTextPacketNames(packet, formattedNick);
}

// Robustly rewrites the real name to the nick everywhere it appears inside a
// TextPacket, without depending on exact field offsets. Two passes:
//   (1) Scan the packet body [0x30, 0x120) on 8-byte boundaries; at each slot
//       try to interpret the memory as a std::string (SEH + length-validated by
//       sdk_safe_string). If it contains the real name, rewrite in place.
//   (2) Scan the params std::vector<std::string> (each std::string is 0x20 on
//       MSVC) and rewrite each element that contains the real name. This covers
//       translate-type chat (chat.type.text / chat.type.announcement) where the
//       name lives in params[0], not in the inline message string.
// Everything is SEH-guarded so a malformed/hostile packet can never crash.
void NameProtect::RewriteTextPacketNames(SDK::Packet* packet, const std::string& nick) {
    if (!packet) return;
    uintptr_t base = (uintptr_t)packet;

    // ---- Pass 1: brute-scan inline std::string fields -----------------------
    // Cover the whole plausible TextPacket body. 0x08 stride catches every
    // 8-byte-aligned std::string start (author/message/filtered/xuid/platformId).
    for (uintptr_t off = 0x30; off < 0x120; off += 0x08) {
        std::string s;
        if (!sdk_safe_string((void*)(base + off), s) || s.empty()) continue;
        if (s.find(mOriginalName) == std::string::npos) continue;
        replaceAll(s, mOriginalName, nick);
        sehWriteStr((void*)(base + off), s.data(), s.size());
    }

    // ---- Pass 2: params std::vector<std::string> ----------------------------
    // Find the vector by brute-scanning the body for a [begin,end) pair whose
    // element stride is sizeof(std::string)==0x20 and whose elements read as
    // valid strings. This avoids hardcoding the params offset.
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
