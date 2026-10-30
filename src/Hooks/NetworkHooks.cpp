// Network layer hooks.
//
// - Packet receive: one detour per resolved packet-handler (high dedup on
//   aliased ids); dispatches modules' OnPacketReceive, cancels packets, and
//   carries the deferred re-delivery machine (Hold/Resend) Translate uses.
// - Packet send: LoopbackPacketSender::send, the choke point every outgoing
//   packet passes through. PlayerAuthInputPacket rotation/flags are rewritten
//   here for silent aim, Scaffold/Baritone spoofing, Twerk and StarveSprint.
// - RakNet::RakPeer::GetAveragePing: captures the live RTT value plus the
//   (peer, AddressOrGUID) pair, so the Ping HUD module can replay the call
//   on its own schedule.
#include "GameHooks.h"
#include "HookShared.h"
#include "../SDK/SDK.h"
#include "../SDK/Client/ClientInstance.h"
#include "../Modules/ModuleManager.h"
#include "../Modules/Misc/Blink.h"
#include "../Modules/Misc/Disabler.h"
#include "../Modules/Misc/PacketLogger.h"
#include "../Modules/Movement/Baritone.h"
#include "../Modules/Movement/Scaffold.h"
#include "../Modules/Movement/Twerk.h"
#include "../Modules/Movement/StarveSprint.h"
#include "../Modules/Combat/SilentRotation.h"
#include "../Core/Logger.h"
#include "../Memory/SigManager.h"
#include "MinHook.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Incoming packets
// ---------------------------------------------------------------------------
namespace {
    using namespace SDK;

    typedef void*(__fastcall* packetHandler_t)(void* dispatcher, void* networkIdentifier,
                                               void* netEventCallback,
                                               const std::shared_ptr<SDK::Packet>& packet);
    struct PacketHookEntry {
        int id = -1;
        uintptr_t handler = 0;
        packetHandler_t original = nullptr;
    };
    std::vector<PacketHookEntry> packetHooks;

    packetHandler_t findOriginalPacketHandler(uintptr_t handler) {
        for (auto& entry : packetHooks)
            if (entry.handler == handler) return entry.original;
        return nullptr;
    }

    // Deferred re-delivery context. The receive hook writes ctx + holds +
    // drains resends; modules call the public Hold/Resend API from other
    // threads. Dispatch pointers are session-long engine objects; held packet
    // lifetime is owned by g_heldPackets, never by the module.
    struct ResendCtx { void* dispatcher; void* netid; void* cb; packetHandler_t original; };
    struct ResendItem { std::shared_ptr<SDK::Packet> packet; ResendCtx ctx; };
    static std::mutex g_heldMutex;
    static std::unordered_map<SDK::Packet*, std::shared_ptr<SDK::Packet>> g_heldPackets;
    static std::unordered_map<int, ResendCtx> g_dispatchCtx;      // realId -> ctx
    static std::deque<ResendItem> g_resendQueue;
    static const std::shared_ptr<SDK::Packet>* g_currentPkt = nullptr;   // net thread only
    static SDK::Packet* g_currentRaw = nullptr;

    // Authoritative packet id from the packet's own getId() virtual
    // (vtable[1]); the hook-side handler id can alias several packet ids.
    int rawRealId(SDK::Packet* p) {
        if (!p || !sdk_safe_ptr(p, sizeof(void*))) return -1;
        void** vtable = *reinterpret_cast<void***>(p);
        if (!vtable || !sdk_safe_ptr(vtable, sizeof(void*) * 2)) return -1;
        using getId_t = int(__fastcall*)(SDK::Packet*);
        __try {
            return reinterpret_cast<getId_t>(vtable[1])(p);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }

    void SaveDispatchContext(int realId, void* d, void* n, void* c, packetHandler_t original) {
        if (realId < 0 || !original) return;
        std::lock_guard<std::mutex> lock(g_heldMutex);
        auto& e = g_dispatchCtx[realId];
        if (e.dispatcher != d || e.original != original) e = { d, n, c, original };
    }

    // POD-only frame so __try is legal (ResendItem's shared_ptr destructor
    // stays in the caller's frame).
    void callResendOriginal(void* d, void* n, void* c, packetHandler_t original,
                            const std::shared_ptr<SDK::Packet>* pkt) {
        __try {
            original(d, n, c, *pkt);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void DrainResendQueue() {
        for (;;) {
            ResendItem item;
            {
                std::lock_guard<std::mutex> lock(g_heldMutex);
                if (g_resendQueue.empty()) return;
                item = std::move(g_resendQueue.front());
                g_resendQueue.pop_front();
            }
            if (item.packet && item.ctx.original)
                callResendOriginal(item.ctx.dispatcher, item.ctx.netid, item.ctx.cb,
                                   item.ctx.original, &item.packet);
        }
    }

    void* __fastcall hkPacketReceive(void* dispatcher, void* networkIdentifier,
                                      void* netEventCallback,
                                      const std::shared_ptr<SDK::Packet>& packet) {
        uintptr_t handler = 0;
        if (dispatcher && sdk_safe_ptr(dispatcher, sizeof(void*))) {
            void** vtable = *reinterpret_cast<void***>(dispatcher);
            if (vtable && sdk_safe_ptr(vtable, sizeof(void*) * 2)) {
                handler = reinterpret_cast<uintptr_t>(vtable[1]);
            }
        }

        int packetId = -1;
        auto original = findOriginalPacketHandler(handler);
        for (const auto& entry : packetHooks) {
            if (entry.handler == handler) {
                packetId = entry.id;
                break;
            }
        }

        // Flush packets modules finished with, then make THIS packet holdable
        // before the module pipeline runs.
        DrainResendQueue();
        if (packet) {
            g_currentPkt = &packet;
            g_currentRaw = packet.get();
            SaveDispatchContext(rawRealId(packet.get()), dispatcher, networkIdentifier,
                                netEventCallback, original);
        }

        bool cancel = false;
        if (packet) {
            for (auto* mod : ModuleManager::GetInstance().GetModules()) {
                if (mod && mod->IsEnabled()) {
                    mod->OnPacketReceive(packet.get(), packetId, cancel);
                }
            }
        }
        g_currentPkt = nullptr;
        g_currentRaw = nullptr;

        if (cancel) return nullptr;

        PacketLogger::LogIncoming(packet, packetId);

        if (original) {
            return original(dispatcher, networkIdentifier, netEventCallback, packet);
        }
        return nullptr;
    }

    // MinecraftPackets::createPacket: resolve every distinct handler (several
    // packet ids share one), detour it, queue-enable in one apply.
    void initializePacketReceiveHooks() {
        if (!SigManager::GetInstance().MinecraftPackets_createPacket || !packetHooks.empty()) return;

        int hooked = 0;
        for (int id = 0; id <= static_cast<int>(SDK::PacketID::EndId); ++id) {
            std::shared_ptr<SDK::Packet> packet = SDK::MinecraftPackets::createPacket(static_cast<SDK::PacketID>(id));
            if (!packet || !packet->dispatcher) continue;

            uintptr_t handler = packet->dispatcher->getPacketHandler();
            if (!handler) continue;

            bool duplicate = false;
            for (const auto& entry : packetHooks) {
                if (entry.handler == handler) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            PacketHookEntry entry;
            entry.id = id;
            entry.handler = handler;
            if (MH_CreateHook(reinterpret_cast<LPVOID>(handler), &hkPacketReceive,
                              reinterpret_cast<LPVOID*>(&entry.original)) == MH_OK) {
                MH_QueueEnableHook(reinterpret_cast<LPVOID>(handler));
                packetHooks.push_back(entry);
                ++hooked;
            }
        }

        if (hooked > 0)
            MH_ApplyQueued();

        LOG_INFO("PacketLogger: hooked " + std::to_string(hooked) + " packet receive handlers", "GameHooks");
    }

    // ---------------------------------------------------------------------------
    // Outgoing packets (PlayerAuthInputPacket rewriting)
    // ---------------------------------------------------------------------------
    // LoopbackPacketSender::send — vtable[2]. The server-side view is written
    // right before serialization so the local first-person camera stays
    // 100% mouse-driven (silent rotation). Payload layout (validated against
    // the 1.26.50.04 payload validator sub_143C8A890): Packet base 0x30.
    static constexpr uintptr_t kPAI_mRot              = 0x30;  // vec2 {pitch, yaw}
    static constexpr uintptr_t kPAI_mPos              = 0x38;  // vec3
    static constexpr uintptr_t kPAI_mYHeadRot         = 0x44;
    static constexpr uintptr_t kPAI_mAnalogMoveVector = 0x5C;  // vec2
    static constexpr uintptr_t kPAI_mMove             = 0x64;  // vec2
    static constexpr uintptr_t kPAI_mCamOrient        = 0x74;  // vec3 Euler — servers derive BODY facing from it
    static constexpr uintptr_t kPAI_mRawMoveVector    = 0x80;  // vec2
    static constexpr uintptr_t kPAI_mInputData        = 0x88;  // AuthInputAction bitset

    // AuthInputAction bits (cross-checked against known-good sneak/sprint bits).
    static constexpr uint64_t kAuth_SNEAKING         = 1ULL << 8;
    static constexpr uint64_t kAuth_SNEAK_DOWN       = 1ULL << 9;
    static constexpr uint64_t kAuth_SPRINT_DOWN      = 1ULL << 4;
    static constexpr uint64_t kAuth_SPRINTING        = 1ULL << 20;
    static constexpr uint64_t kAuth_PERSIST_SNEAK    = 1ULL << 24;
    static constexpr uint64_t kAuth_START_SPRINTING = 1ULL << 25;
    static constexpr uint64_t kAuth_STOP_SPRINTING   = 1ULL << 26;
    static constexpr uint64_t kAuth_START_SNEAKING  = 1ULL << 27;
    static constexpr uint64_t kAuth_STOP_SNEAKING    = 1ULL << 28;
    static constexpr uint64_t kAuth_SprintMask =
        kAuth_SPRINT_DOWN | kAuth_SPRINTING | kAuth_START_SPRINTING | kAuth_STOP_SPRINTING;

    static constexpr uint64_t kAuth_UP    = 1ULL << 10;
    static constexpr uint64_t kAuth_DOWN  = 1ULL << 11;
    static constexpr uint64_t kAuth_LEFT  = 1ULL << 12;
    static constexpr uint64_t kAuth_RIGHT = 1ULL << 13;
    static constexpr uint64_t kAuth_DirMask = kAuth_UP | kAuth_DOWN | kAuth_LEFT | kAuth_RIGHT;

    static constexpr uintptr_t kPAI_mInteractRots     = 0x6C;  // vec2 {pitch, yaw}

    // SEH helpers stay free of C++ objects (C2712).
    static bool sehGetPacketId(void* packet, int* idOut) {
        __try {
            *idOut = static_cast<int>(reinterpret_cast<SDK::Packet*>(packet)->getId());
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Rewrite rotation, head rot, interact rots and camera orientation to the
    // spoofed aim; pre[3] receives the original values for the first-call log.
    // The camera orientation matters: the server derives the BODY facing from
    // it, so without that write only the head follows the spoof.
    static bool sehApplyPacketRot(void* packet, float yaw, float pitch, float* pre /*[3]*/) {
        __try {
            uintptr_t base = reinterpret_cast<uintptr_t>(packet);
            pre[0] = *reinterpret_cast<float*>(base + kPAI_mRot);
            pre[1] = *reinterpret_cast<float*>(base + kPAI_mRot + 4);
            pre[2] = *reinterpret_cast<float*>(base + kPAI_mYHeadRot);
            *reinterpret_cast<float*>(base + kPAI_mRot)              = pitch;
            *reinterpret_cast<float*>(base + kPAI_mRot + 4)          = yaw;
            *reinterpret_cast<float*>(base + kPAI_mYHeadRot)         = yaw;
            *reinterpret_cast<float*>(base + kPAI_mInteractRots)     = pitch;
            *reinterpret_cast<float*>(base + kPAI_mInteractRots + 4) = yaw;
            *reinterpret_cast<float*>(base + kPAI_mCamOrient)        = pitch;
            *reinterpret_cast<float*>(base + kPAI_mCamOrient + 4)    = yaw;
            *reinterpret_cast<float*>(base + kPAI_mCamOrient + 8)    = 0.0f;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Scaffold bridge-line lift: raise the reported eye height to the bridge
    // line, never lower than what the client reports.
    static bool sehApplyYSpoof(void* packet, float bridgeEyeY) {
        __try {
            float* posY = reinterpret_cast<float*>(
                reinterpret_cast<uintptr_t>(packet) + kPAI_mPos + 4);
            if (bridgeEyeY > *posY) *posY = bridgeEyeY;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool sehApplyTwerkSneak(void* packet, bool want, bool start, bool stop) {
        __try {
            uintptr_t base = reinterpret_cast<uintptr_t>(packet);
            uint64_t* flags = reinterpret_cast<uint64_t*>(base + kPAI_mInputData);
            uint64_t f = *flags;
            // Clear previous sneak edge bits so we don't leave STOP sticky.
            f &= ~(kAuth_START_SNEAKING | kAuth_STOP_SNEAKING);
            if (want) {
                f |= kAuth_SNEAKING | kAuth_SNEAK_DOWN | kAuth_PERSIST_SNEAK;
                if (start) f |= kAuth_START_SNEAKING;
            } else {
                f &= ~(kAuth_SNEAKING | kAuth_SNEAK_DOWN | kAuth_PERSIST_SNEAK);
                if (stop) f |= kAuth_STOP_SNEAKING;
            }
            *flags = f;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // StarveSprint hunger spoof: report walking to the server.
    static bool sehClearSprintFlags(void* packet) {
        __try {
            uintptr_t base = reinterpret_cast<uintptr_t>(packet);
            uint64_t* flags = reinterpret_cast<uint64_t*>(base + kPAI_mInputData);
            *flags &= ~kAuth_SprintMask;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Baritone / ConsumeMoveFrame path: the movement is already expressed in
    // the spoofed frame. Packet layout: mMove = {x: strafe, y: fwd}; direction
    // bits fwd -> UP/DOWN, strafe -> LEFT/RIGHT.
    static bool sehApplyBaritoneMove(void* packet, float fwd, float strafe) {
        __try {
            uintptr_t base = reinterpret_cast<uintptr_t>(packet);
            *reinterpret_cast<float*>(base + kPAI_mMove)                  = strafe;
            *reinterpret_cast<float*>(base + kPAI_mMove + 4)              = fwd;
            *reinterpret_cast<float*>(base + kPAI_mAnalogMoveVector)      = strafe;
            *reinterpret_cast<float*>(base + kPAI_mAnalogMoveVector + 4)  = fwd;
            *reinterpret_cast<float*>(base + kPAI_mRawMoveVector)         = strafe;
            *reinterpret_cast<float*>(base + kPAI_mRawMoveVector + 4)     = fwd;

            auto* flags = reinterpret_cast<uint64_t*>(base + kPAI_mInputData);
            uint64_t v = *flags & ~kAuth_DirMask;
            if (fwd    >  0.3f) v |= kAuth_UP;
            if (fwd    < -0.3f) v |= kAuth_DOWN;
            if (strafe >  0.3f) v |= kAuth_LEFT;
            if (strafe < -0.3f) v |= kAuth_RIGHT;
            *flags = v;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Scaffold's own latched channel: the client walked against realYaw, the
    // packet now claims spoofYaw — re-express the WASD frame in the spoofed
    // one or the server's prediction lands elsewhere and corrects the player
    // (the "Scaffold shoves me off the bridge" effect).
    static bool sehApplySilentMove(void* packet, float realYaw, float spoofYaw, int fwd, int strafe) {
        if (fwd == 0 && strafe == 0) return false; // standing still: nothing to restate
        __try {
            constexpr float kD2R = 3.141592653589793f / 180.0f;
            const float ang = std::atan2((float)strafe, (float)fwd);
            const float worldAng = (realYaw + ang * 180.0f / 3.141592653589793f + 90.0f) * kD2R;
            const float wx = std::cos(worldAng);
            const float wz = std::sin(worldAng);
            const float sy = spoofYaw * kD2R;
            const float fx = -std::sin(sy), fz = std::cos(sy); // spoofed forward
            const float sx =  std::cos(sy), sz = std::sin(sy); // spoofed right
            const float outFwd    = wx * fx + wz * fz;
            const float outStrafe = wx * sx + wz * sz;

            uintptr_t base = reinterpret_cast<uintptr_t>(packet);
            *reinterpret_cast<float*>(base + kPAI_mMove)                  = outStrafe;
            *reinterpret_cast<float*>(base + kPAI_mMove + 4)              = outFwd;
            *reinterpret_cast<float*>(base + kPAI_mAnalogMoveVector)      = outStrafe;
            *reinterpret_cast<float*>(base + kPAI_mAnalogMoveVector + 4)  = outFwd;
            *reinterpret_cast<float*>(base + kPAI_mRawMoveVector)         = outStrafe;
            *reinterpret_cast<float*>(base + kPAI_mRawMoveVector + 4)     = outFwd;

            auto* flags = reinterpret_cast<uint64_t*>(base + kPAI_mInputData);
            uint64_t v = *flags & ~kAuth_DirMask;
            if (outFwd    >  0.3f) v |= kAuth_UP;
            if (outFwd    < -0.3f) v |= kAuth_DOWN;
            if (outStrafe >  0.3f) v |= kAuth_LEFT;
            if (outStrafe < -0.3f) v |= kAuth_RIGHT;
            *flags = v;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    typedef void* (__fastcall* packetSend_t)(void* _this, void* packet);
    static packetSend_t oPacketSend = nullptr;
    static bool g_packetSendHooked = false;

    void* __fastcall hkPacketSend(void* _this, void* packet) {
        // Disabler's latency flush re-enters here; don't run the pipeline twice.
        if (Disabler::IsFlushing())
            return oPacketSend ? oPacketSend(_this, packet) : nullptr;

        // Blink freeze: check before any module pipeline runs.
        if (packet) {
            int blinkId = -1;
            if (sehGetPacketId(packet, &blinkId)) {
                Module* blink = ModuleManager::GetInstance().GetModuleByName("Blink");
                if (blink && blink->IsEnabled() &&
                    static_cast<Blink*>(blink)->ConsumeFrost((SDK::Packet*)packet, blinkId)) {
                    return nullptr;   // frozen — the caller treats nullptr as success
                }
            }
        }

        if (packet) {
            int id = -1;
            if (sehGetPacketId(packet, &id)) {
                if (id == static_cast<int>(SDK::PacketID::PlayerAuthInput)) {
                    // Disabler's validation disruption runs first, downstream rewrites intact.
                    {
                        Module* dis = ModuleManager::GetInstance().GetModuleByName("Disabler");
                        if (dis && dis->IsEnabled())
                            static_cast<Disabler*>(dis)->ModifyAuthInput(packet);
                    }

                    // Silent aim (Aura/OreMiner/ChestAura). Only the rotation is
                    // injected; positions go out honest through the game's own
                    // InventoryTransactionPacket — spoofing mPos made the server
                    // rewind with CorrectPlayerMovePrediction (the teleporting).
                    float yaw = 0.0f, pitch = 0.0f;
                    if (SilentRotation::Consume(yaw, pitch)) {
                        float pre[3] = { 0, 0, 0 };
                        if (sehApplyPacketRot(packet, yaw, pitch, pre)) {
                            static bool s_logged = false;
                            if (!s_logged) {
                                s_logged = true;
                                LOG_INFO("PAI preRot pitch=" + std::to_string(pre[0]) +
                                         " yaw=" + std::to_string(pre[1]) +
                                         " head=" + std::to_string(pre[2]) +
                                         " -> inject pitch=" + std::to_string(pitch) +
                                         " yaw=" + std::to_string(yaw), "GameHooks");
                            }
                        }
                        // Baritone rides the same rotation channel; a walking
                        // spoof must also restate the MOVEMENT in the spoofed
                        // frame or the server corrects the player.
                        float bFwd = 0.0f, bStrafe = 0.0f;
                        if (Baritone::ConsumeSilentMove(bFwd, bStrafe)) {
                            sehApplyBaritoneMove(packet, bFwd, bStrafe);
                        } else {
                            float mFwd = 0.0f, mStrafe = 0.0f;
                            if (SilentRotation::ConsumeMoveFrame(mFwd, mStrafe))
                                sehApplyBaritoneMove(packet, mFwd, mStrafe);
                        }
                    }

                    // Scaffold "Silent": latched aim at the block being placed +
                    // bridge-line Y bypass (never below the real height).
                    {
                        Module* scf = ModuleManager::GetInstance().GetModuleByName("Scaffold");
                        if (scf && scf->IsEnabled()) {
                            if (static_cast<Scaffold*>(scf)->GetYSpoof()) {
                                float bridgeEyeY = 0.0f;
                                if (Scaffold::ConsumeYSpoof(bridgeEyeY))
                                    sehApplyYSpoof(packet, bridgeEyeY);
                            }
                            float sYaw = 0.0f, sPitch = 0.0f;
                            if (Scaffold::ConsumeSilentRot(sYaw, sPitch)) {
                                float pre[3] = { 0, 0, 0 };
                                sehApplyPacketRot(packet, sYaw, sPitch, pre);
                                float realYaw = 0.0f; int fwd = 0, strafe = 0;
                                if (Scaffold::ConsumeSilentMove(realYaw, fwd, strafe))
                                    sehApplySilentMove(packet, realYaw, sYaw, fwd, strafe);
                            }
                        }
                    }

                    // Twerk: inject sneak AuthInput flags (visual + server).
                    {
                        bool start = false, stop = false;
                        bool edge = Twerk::ConsumeEdge(start, stop);
                        bool want = Twerk::WantSneak();
                        if (want || edge)
                            sehApplyTwerkSneak(packet, want, start, stop);
                    }

                    // StarveSprint hunger spoof.
                    {
                        Module* ss = ModuleManager::GetInstance().GetModuleByName("StarveSprint");
                        if (ss && static_cast<StarveSprint*>(ss)->IsHungerSpoof())
                            sehClearSprintFlags(packet);
                    }
                }
                // Module-owned send logic (NoFall etc.) is not hardcoded here.
                bool cancel = false;
                ModuleManager::GetInstance().OnPacketSend(reinterpret_cast<SDK::Packet*>(packet), id, cancel);
                if (cancel) return nullptr;
            }
        }
        return oPacketSend ? oPacketSend(_this, packet) : nullptr;
    }

    // Resolve LoopbackPacketSender::send (PacketSender vtable[2]) once the
    // PacketSender exists — installed lazily from _updatePlayer (post join).
    static void* sehResolveSendFn() {
        __try {
            SDK::ClientInstance* ci = SDK::ClientInstance::get();
            if (!ci || !sdk_safe_ptr(ci, sizeof(void*))) return nullptr;
            SDK::PacketSender* ps = ci->getPacketSender();
            if (!ps || !sdk_safe_ptr(ps, sizeof(void*))) return nullptr;
            void** vt = *reinterpret_cast<void***>(ps);
            if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * 3)) return nullptr;
            void* fn = vt[2];
            if (!fn || !gh::isExecutablePtr(fn)) return nullptr;
            return fn;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    // ---------------------------------------------------------------------------
    // Live ping (RakNet)
    // ---------------------------------------------------------------------------
    // RakNet::RakPeer::GetAveragePing(AddressOrGUID): averages the last five
    // ping/pong samples into milliseconds, -1 when the remote is unknown.
    // 1.26.60: RVA 0x24FCEF0, RakPeerInterface vtable slot 0x27; every caller
    // reaches it virtually, so detouring the implementation catches all of
    // them. The hook also snapshots the (peer, AddressOrGUID) pair so the Ping
    // module can re-poll on its own schedule.
    typedef int(__fastcall* getAveragePing_t)(void* peer, void* addressOrGuid);
    static getAveragePing_t oRakPeerGetAveragePing = nullptr;
    static constexpr size_t kAddressOrGuidSize = 136;

    static std::mutex s_pingMutex;
    static void* s_pingPeer = nullptr;
    static unsigned char s_pingAog[kAddressOrGuidSize] = {};
    static std::atomic<bool> s_pingSeeded{ false };
    std::atomic<int> s_lastPing{ -1 };
    std::atomic<DWORD> s_lastPingStamp{ 0 };

    int __fastcall hkRakPeerGetAveragePing(void* peer, void* addressOrGuid) {
        const int ping = oRakPeerGetAveragePing(peer, addressOrGuid);
        if (ping >= 0) {
            s_lastPing.store(ping, std::memory_order_relaxed);
            s_lastPingStamp.store(GetTickCount(), std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> lock(s_pingMutex);
            s_pingPeer = peer;
            std::memcpy(s_pingAog, addressOrGuid, kAddressOrGuidSize);
        }
        s_pingSeeded.store(true, std::memory_order_release);
        return ping;
    }
}

namespace NetPing {
    int LastMs() { return s_lastPing.load(std::memory_order_relaxed); }
    DWORD AgeMs() { return GetTickCount() - s_lastPingStamp.load(std::memory_order_relaxed); }

    // __try wants a POD frame, so the actual call lives in its own helper
    // (C2712: no object unwinding around __try).
    static int sehPollCaptured(void* peer, void* aog) {
        int ping = -1;
        __try {
            ping = oRakPeerGetAveragePing(peer, aog);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
        return ping;
    }

    // Re-poll the captured remote at display cadence; a pure read of the
    // peer's ping slots, SEH-guarded against a torn-down connection.
    void ForcePoll() {
        if (!s_pingSeeded.load(std::memory_order_acquire) || !oRakPeerGetAveragePing)
            return;
        void* peer = nullptr;
        unsigned char aog[kAddressOrGuidSize];
        {
            std::lock_guard<std::mutex> lock(s_pingMutex);
            peer = s_pingPeer;
            std::memcpy(aog, s_pingAog, kAddressOrGuidSize);
        }
        if (!peer) return;
        const int ping = sehPollCaptured(peer, aog);
        if (ping >= 0) {
            s_lastPing.store(ping, std::memory_order_relaxed);
            s_lastPingStamp.store(GetTickCount(), std::memory_order_relaxed);
        }
    }
}

void gh::tryHookPacketSend() {
    if (g_packetSendHooked) return;
    void* sendFn = sehResolveSendFn();
    if (!sendFn) return;
    if (MH_CreateHook(sendFn, reinterpret_cast<LPVOID>(&hkPacketSend),
                      reinterpret_cast<LPVOID*>(&oPacketSend)) == MH_OK &&
        gh::HookEnable(sendFn) == MH_OK) {
        g_packetSendHooked = true;
        LOG_INFO("LoopbackPacketSender::send hooked (vtable[2]) @ " +
                 std::to_string(reinterpret_cast<uintptr_t>(sendFn)), "GameHooks");
    }
}

// ---- Deferred incoming-packet re-delivery (public API) -----------------------
bool GameHooks::HoldIncomingPacket(SDK::Packet* raw) {
    if (!raw || !g_currentPkt || g_currentRaw != raw || !*g_currentPkt) return false;
    std::lock_guard<std::mutex> lock(g_heldMutex);
    if (g_heldPackets.size() >= 64) return false;
    g_heldPackets[raw] = *g_currentPkt;
    return true;
}

bool GameHooks::ResendIncomingPacket(SDK::Packet* raw, int realId) {
    if (!raw) return false;
    std::lock_guard<std::mutex> lock(g_heldMutex);
    auto it = g_heldPackets.find(raw);
    if (it == g_heldPackets.end()) return false;
    auto cit = g_dispatchCtx.find(realId);
    if (cit == g_dispatchCtx.end()) return false;
    g_resendQueue.push_back({ it->second, cit->second });
    g_heldPackets.erase(it);
    return true;
}

void InstallNetworkHooks() {
    initializePacketReceiveHooks();

    // ConnectionRequest EditionFaker patch sites (in-place 6-byte writers, no
    // MinHook; see EditionFaker.cpp). Just report availability here.
    {
        auto& sig = SigManager::GetInstance();
        int sites = (sig.ConnectionRequest_DeviceOS ? 1 : 0)
                  + (sig.ConnectionRequest_DefaultInput ? 1 : 0)
                  + (sig.ConnectionRequest_DeviceOS_Legacy ? 1 : 0)
                  + (sig.ConnectionRequest_DefaultInput_Legacy ? 1 : 0);
        if (sites > 0) {
            LOG_INFO("ConnectionRequest EditionFaker patch sites resolved: " + std::to_string(sites) + "/4", "GameHooks");
        } else {
            LOG_WARN("ConnectionRequest patch sites not resolved; EditionFaker will bail out safely", "GameHooks");
        }
    }

    // Live ping capture for the Ping HUD module.
    {
        uintptr_t pingAddr = SigManager::GetInstance().RakPeer_GetAveragePing;
        if (pingAddr) {
            if (MH_CreateHook(reinterpret_cast<LPVOID>(pingAddr), &hkRakPeerGetAveragePing,
                              reinterpret_cast<LPVOID*>(&oRakPeerGetAveragePing)) == MH_OK) {
                gh::HookEnable(reinterpret_cast<LPVOID>(pingAddr));
                LOG_INFO("RakPeer::GetAveragePing hooked (Ping HUD source)", "GameHooks");
            } else {
                LOG_ERROR("FAILED to hook RakPeer::GetAveragePing", "GameHooks");
            }
        } else {
            LOG_WARN("RakPeer::GetAveragePing signature not found; Ping HUD inactive", "GameHooks");
        }
    }
}
