#include "PacketLogger.h"
#include "../ModuleManager.h"
#include "../../Commands/CommandManager.h"
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include "../../Memory/SigManager.h"
#include "../../SDK/Network/PacketTypes.h"
#include "../../Core/Logger.h"

namespace {
    bool shouldIgnore(SDK::PacketID id) {
        switch (id) {
        case SDK::PacketID::LevelChunk:
        case SDK::PacketID::MoveActorAbsolute:
        case SDK::PacketID::MoveActorDelta:
        case SDK::PacketID::PlayerAuthInput:
        case SDK::PacketID::SetTime:
        case SDK::PacketID::PlayerList:
        case SDK::PacketID::PlayerSkin:
            return true;
        default:
            return false;
        }
    }

    std::string getPacketName(int id) {
        return SDK::getPacketTypeName(id);
    }

    // Safe field readers (SEH-guarded; never crash on recycled/garbage packets).
    static bool rdI32(const void* base, size_t off, int& out) {
        const void* p = reinterpret_cast<const char*>(base) + off;
        if (!sdk_safe_ptr((void*)p, 4)) return false;
        __try { out = *reinterpret_cast<const int*>(p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool rdI64(const void* base, size_t off, int64_t& out) {
        const void* p = reinterpret_cast<const char*>(base) + off;
        if (!sdk_safe_ptr((void*)p, 8)) return false;
        __try { out = *reinterpret_cast<const int64_t*>(p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool rdU8(const void* base, size_t off, int& out) {
        const void* p = reinterpret_cast<const char*>(base) + off;
        if (!sdk_safe_ptr((void*)p, 1)) return false;
        __try { out = *reinterpret_cast<const unsigned char*>(p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool rdF32(const void* base, size_t off, float& out) {
        const void* p = reinterpret_cast<const char*>(base) + off;
        if (!sdk_safe_ptr((void*)p, 4)) return false;
        __try { out = *reinterpret_cast<const float*>(p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool rdStr(const void* base, size_t off, std::string& out) {
        void* s = (void*)(reinterpret_cast<const char*>(base) + off);
        return sdk_safe_string(s, out); // validates ptr + length + data, SEH-guarded
    }

    // Returns true if a structured decoder produced output (=> hex dump suppressed).
    bool decodeFields(SDK::Packet* packet, int id, std::ostringstream& oss) {
        std::string s; int i = 0, i2 = 0; int64_t q = 0; float f = 0;
        switch (id) {
        case 0x09: { // Text
            int type = 0; rdI32(packet, 0x30, type);
            std::string author, msg; rdStr(packet, 0x38, author); rdStr(packet, 0x58, msg);
            oss << "  text { type=" << type << ", source='" << author << "', message='" << msg << "' }\n";
            return true;
        }
        case 0x05: { // Disconnect
            rdStr(packet, 0x38, s);
            oss << "  disconnect { reason='" << s << "' }\n";
            return true;
        }
        case 0x14: { // ActorEvent
            rdI64(packet, 0x30, q); rdI32(packet, 0x38, i); rdI32(packet, 0x3C, i2);
            oss << "  actor_event { runtime_id=" << q << ", event=" << i << ", data=" << i2 << " }\n";
            return true;
        }
        case 0x24: { // PlayerAction
            rdI64(packet, 0x30, q); rdI32(packet, 0x38, i);
            oss << "  player_action { runtime_id=" << q << ", action=" << i << " }\n";
            return true;
        }
        case 0x21: { // Interact
            rdU8(packet, 0x30, i); rdI64(packet, 0x38, q);
            oss << "  interact { action=" << i << ", target_runtime_id=" << q << " }\n";
            return true;
        }
        case 0x1B: { // SetActorMotion -> hint only (layout varies); show runtime id
            rdI64(packet, 0x30, q);
            oss << "  set_actor_motion { runtime_id=" << q << " }\n";
            return true;
        }
        case 0x0A: { // SetTime
            rdI32(packet, 0x30, i);
            oss << "  set_time { time=" << i << " }\n";
            return true;
        }
        default:
            return false;
        }
    }

    // SEH wrapper: holds no C++ objects of its own (oss passed by ref), so it is
    // allowed to use __try (avoids C2712 that dumpBytes would hit with its locals).
    static bool sehDecode(SDK::Packet* packet, int id, std::ostringstream& oss) {
        __try { return decodeFields(packet, id, oss); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void dumpBytes(SDK::Packet* packet, int id, const std::string& name, int byteCount) {
        if (!packet || byteCount <= 0) return;
        byteCount = std::clamp(byteCount, 16, 256);

        std::ostringstream oss;
        oss << "[Sunstead PacketLogger] IN 0x" << std::hex << id << std::dec
            << " (" << name << ")\n";

        bool decoded = sehDecode(packet, id, oss);

        // Only fall back to a raw hex/ascii dump for packets we can't decode -
        // this is what made the old logs unreadable. Decoded packets stay clean.
        if (!decoded) {
            auto* bytes = reinterpret_cast<const unsigned char*>(packet);
            oss << "  raw (no decoder) hex | ascii:\n";
            for (int row = 0; row < byteCount; row += 16) {
                oss << "  ";
                for (int k = row; k < row + 16; ++k) {
                    if (k < byteCount)
                        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[k]) << ' ';
                    else oss << "   ";
                }
                oss << " |";
                for (int k = row; k < row + 16 && k < byteCount; ++k) {
                    unsigned char c = bytes[k];
                    oss << static_cast<char>((c >= 0x20 && c < 0x7f) ? c : '.');
                }
                oss << "|\n";
            }
            oss << std::dec;
        }
        LOG_INFO(oss.str(), "PacketLogger");
    }
}

void PacketLogger::LogIncoming(const std::shared_ptr<SDK::Packet>& packet, int id) {
    auto* module = ModuleManager::GetInstance().GetModuleByName("PacketLogger");
    if (!module || !module->IsEnabled() || !packet) return;

    bool ignoreSpam = static_cast<BoolSetting*>(module->mSettings[2])->mValue;
    if (ignoreSpam && shouldIgnore(static_cast<SDK::PacketID>(id))) return;

    std::string name = getPacketName(id);

    using clock = std::chrono::steady_clock;
    static auto start = clock::now();
    auto now = clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

    bool chat = static_cast<BoolSetting*>(module->mSettings[0])->mValue;
    if (chat) {
        std::ostringstream msg;
        msg << "§bPacket IN §7id=§e0x" << std::hex << id << std::dec
            << " §7name=§f" << name
            << " §7t=§a" << ms << "ms";
        CommandManager::GetInstance().PrintToChat(msg.str());
    }

    bool console = static_cast<BoolSetting*>(module->mSettings[1])->mValue;
    if (console) {
        int bytes = static_cast<int>(static_cast<NumberSetting*>(module->mSettings[3])->mValue);
        dumpBytes(packet.get(), id, name, bytes);
    }
}