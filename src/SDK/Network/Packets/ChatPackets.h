#pragma once
// Wire-level layouts: TextPacket (id 9, body ctor sub_14660AC70 = createPacket
// case 9); CommandOutputPacket (id 79, sub_14861A820 = case 79, CommandOutput
// move ctor sub_1443A0310).
#include "../../Types.h"
#include "Packet.h"
#include <cstring>
#include <string>

namespace SDK {

// --- TextPacket body ---
// +0x30 translationNeeded; +0x38 xboxUserId (XUID digits); +0x58 platformChatId;
// +0x78 filteredMessage (present flag +0x98); +0xA0 TextPacketType byte;
// +0xA8 string str; +0xC8 variant storage; discriminator @+0xE8.
// The +0xE8 discriminator SELECTS what +0xA8/+0xC8 hold:
//   disc 0 — "message only":   str(+0xA8) = message
//   disc 1 — "author+message": str(+0xA8) = AUTHOR NAME, message rides +0xC8
//   disc 2 — "message+params": str(+0xA8) = message, params at +0xC8
constexpr uintptr_t kTextPktType   = 0xA0;  // byte, the real TextPacketType
constexpr uintptr_t kTextPktStr    = 0xA8;  // string — message, or the AUTHOR when disc == 1
constexpr uintptr_t kTextPktData   = 0xC8;  // variant storage — the message when disc == 1
constexpr uintptr_t kTextPktDisc   = 0xE8;

// --- CommandOutputPacket body ---
constexpr uintptr_t kCmdOutType   = 0x71;  // byte, CommandOutputType (0 Last / 1 Silent / 2 All)
constexpr uintptr_t kCmdOutMsgVec = 0x80;  // vector<CommandOutputMessage>, stride 0x40
constexpr uintptr_t kCmdOutMsgStride = 0x40;
constexpr uintptr_t kCmdOutMsgStr = 0x08;   // message string inside the element (MSVC SSO)
constexpr uintptr_t kCmdOutMsgParams = 0x28; // vector<string>, stride 0x20

inline bool sehReadPacketByte(void* packet, uintptr_t offset, unsigned char* out) {
    __try {
        *out = *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(packet) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool sehReadPacketPtr(void* base, uintptr_t offset, uintptr_t* out) {
    __try {
        *out = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(base) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { *out = 0; return false; }
}

inline bool sehCopyPacketString(void* packet, uintptr_t offset, char* out, size_t cap) {
    __try {
        const std::string* s = reinterpret_cast<const std::string*>(
            reinterpret_cast<uintptr_t>(packet) + offset);
        const size_t n = s->size();
        if (n == 0 || n >= cap) { out[0] = '\0'; return n == 0; }
        std::memcpy(out, s->data(), n);
        out[n] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return false;
    }
}

// The message element's params, read into flat buffers (stride 0x20 std::string).
inline int sehReadMsgParams(void* elem, char (*out)[160], size_t capEach, int maxParams) {
    __try {
        uintptr_t begin = *(uintptr_t*)((uintptr_t)elem + kCmdOutMsgParams);
        uintptr_t end   = *(uintptr_t*)((uintptr_t)elem + kCmdOutMsgParams + 8);
        if (!begin || end < begin) return 0;
        const uintptr_t span = end - begin;
        if (span % 0x20 != 0) return 0;
        int n = (int)(span / 0x20);
        if (n > maxParams) n = maxParams;
        if (n > 64) n = 64;
        for (int i = 0; i < n; ++i) {
            const std::string* s = reinterpret_cast<const std::string*>(
                (uintptr_t)begin + (uintptr_t)i * 0x20);
            if (!s || !sdk_safe_ptr((void*)s, 0x20)) { out[i][0] = '\0'; continue; }
            const size_t len = s->size();
            if (len == 0 || len >= capEach) { out[i][0] = '\0'; continue; }
            std::memcpy(out[i], s->data(), len);
            out[i][len] = '\0';
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// %s substitution in order; leftover params appended space-separated.
inline bool sehFormatCommandOutputMsg(void* elem, char* out, size_t cap) {
    char text[512] = {0};
    if (!sehCopyPacketString(elem, kCmdOutMsgStr, text, sizeof(text))) return false;
    char params[16][160];
    const int nParams = sehReadMsgParams(elem, params, sizeof(params[0]),
                                         (int)(sizeof(params) / sizeof(params[0])));
    size_t oi = 0, ti = 0, pi = 0;
    while (ti < sizeof(text) && text[ti] != '\0' && oi + 1 < cap) {
        if (text[ti] == '%' && ti + 1 < sizeof(text) && text[ti + 1] == 's' && pi < nParams) {
            const char* p = params[pi++];
            for (size_t k = 0; p[k] && oi + 1 < cap; ++k) out[oi++] = p[k];
            ti += 2;
        } else {
            out[oi++] = text[ti++];
        }
    }
    for (; pi < nParams && oi + 1 < cap; ++pi) {
        if (oi > 0) out[oi++] = ' ';
        const char* p = params[pi];
        for (size_t k = 0; p[k] && oi + 1 < cap; ++k) out[oi++] = p[k];
    }
    out[oi] = '\0';
    return oi > 0;
}

} // namespace SDK
