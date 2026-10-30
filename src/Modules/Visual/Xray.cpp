#include "Xray.h"
#include "../../Core/Logger.h"
#include "../../Memory/SigManager.h"
#include "../../Render/FrameView.h"
#include "../../SDK/Render/BoxOverlay.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/World/BlockScan.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../Setting.h"
#include "MinHook.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// The scan half is the proven BlockESP machinery (progressive budget-limited
// volume walk, double-buffered result swap, legacy classification cache);
// classification/colour/projection come from the shared SDK::BlockScan. It is
// only the OPTIONAL "Overlay Boxes" extra now — the xray itself is the
// tessellator hook below.
using namespace SDK::BlockScan;

static std::atomic<bool>      g_texRedirectOn{ false };
static std::atomic<uint32_t>  g_keepMask{ 0 };

// Group bits — one per ore toggle (must match kindEnabled() below).
static constexpr uint32_t kBitCoal   = 1u << 0;
static constexpr uint32_t kBitIron   = 1u << 1;
static constexpr uint32_t kBitGold   = 1u << 2;
static constexpr uint32_t kBitDiamond= 1u << 3;
static constexpr uint32_t kBitRedstone = 1u << 4;
static constexpr uint32_t kBitLapis  = 1u << 5;
static constexpr uint32_t kBitEmerald= 1u << 6;
static constexpr uint32_t kBitCopper = 1u << 7;
static constexpr uint32_t kBitDebris = 1u << 8;
static constexpr uint32_t kBitAlways = 1u << 31;   // nether quartz etc.

namespace {

bool kindEnabled(const Xray* self, int kind) {
    switch (kind) {
        case Kind_Diamond: return static_cast<BoolSetting*>(self->mSettings[8])->mValue;
        case Kind_Iron:     return static_cast<BoolSetting*>(self->mSettings[9])->mValue;
        case Kind_Gold:     return static_cast<BoolSetting*>(self->mSettings[10])->mValue;
        case Kind_Coal:     return static_cast<BoolSetting*>(self->mSettings[11])->mValue;
        case Kind_Redstone: return static_cast<BoolSetting*>(self->mSettings[12])->mValue;
        case Kind_Lapis:    return static_cast<BoolSetting*>(self->mSettings[13])->mValue;
        case Kind_Emerald:  return static_cast<BoolSetting*>(self->mSettings[14])->mValue;
        case Kind_Copper:   return static_cast<BoolSetting*>(self->mSettings[15])->mValue;
        case Kind_Debris:   return static_cast<BoolSetting*>(self->mSettings[16])->mValue;
        case Kind_Chest:    return static_cast<BoolSetting*>(self->mSettings[17])->mValue;
        case Kind_Spawner:  return static_cast<BoolSetting*>(self->mSettings[18])->mValue;
        default: return false;
    }
}

bool boxesEnabled(const Xray* self) {
    return self && self->mSettings.size() > 20 && static_cast<BoolSetting*>(self->mSettings[20])->mValue;
}

// The keep-mask the lookup hook reads (ore toggles -> group bits).
uint32_t buildKeepMask(const Xray* self) {
    uint32_t m = kBitAlways;
    if (static_cast<BoolSetting*>(self->mSettings[11])->mValue) m |= kBitCoal;
    if (static_cast<BoolSetting*>(self->mSettings[9])->mValue)  m |= kBitIron;
    if (static_cast<BoolSetting*>(self->mSettings[10])->mValue) m |= kBitGold;
    if (static_cast<BoolSetting*>(self->mSettings[8])->mValue)  m |= kBitDiamond;
    if (static_cast<BoolSetting*>(self->mSettings[12])->mValue) m |= kBitRedstone;
    if (static_cast<BoolSetting*>(self->mSettings[13])->mValue) m |= kBitLapis;
    if (static_cast<BoolSetting*>(self->mSettings[14])->mValue) m |= kBitEmerald;
    if (static_cast<BoolSetting*>(self->mSettings[15])->mValue) m |= kBitCopper;
    if (static_cast<BoolSetting*>(self->mSettings[16])->mValue) m |= kBitDebris;
    return m;
}

// ===========================================================================
// v4: TESSELLATOR-LEVEL XRAY (replaces the atlas redirect, which could never
// work: BlockGraphics bakes the UV sets at resource load, the mesher never
// calls TextureAtlas::lookupEntry per block).
//
// Hook: BlockTessellator::tessellateInWorld(tess, ctx, Block*, BlockPos*, char)
// = the per-block dispatcher called by the chunk builder for every block
// (sub_146035800 on 1.26.50.04; the chunk builder ignores its return value).
//   * non-ore block  -> return 0 without calling the original: it emits no
//     geometry at all, so the world is empty except for what we let through;
//   * ore block      -> original, but with the tessellator's face-culling flag
//     (tess+122, read only by the cube path to build its cull mask) cleared for
//     the duration of the call, so ores buried inside opaque stone still emit
//     all six faces. The tessellator is per-build (thread-private), so the
//     temporary write is race-free.
// The hook runs on the chunk-build worker threads: classification is POD-only,
// SEH-guarded and cached in a lock-free direct-mapped table keyed by BlockLegacy*.
// Unreadable names classify as "unknown" -> vanilla rendering (fail-safe).
// ===========================================================================
using TessFn = char(__fastcall*)(void* tess, void* ctx, void* block, void* pos, char a5);
static TessFn oTess = nullptr;

static std::atomic<uint64_t> g_cls[4096];            // (BlockLegacy* << 8) | (slot + 1)
static std::atomic<unsigned> g_dbgSeen{ 0 }, g_dbgHidden{ 0 }, g_dbgKept{ 0 }, g_dbgUnknown{ 0 };
static std::atomic<bool>     g_clsFlushPending{ false };
// Distinct dispatcher caller RVAs (first 8) + per-site hit counts: proves
// WHICH mesher paths reach the hook while the sweep rebuilds. Locked only
// until the table is full (steady state = one atomic each).
static uintptr_t s_callSites[8] = {};
static unsigned  s_callCounts[8] = {};
static std::atomic<int> s_callSiteCount{ 0 };
static void noteCaller(uintptr_t ret) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const uintptr_t rva = (ret >= base) ? ret - base : ret;
    for (int i = 0; i < 8; ++i) {
        if (s_callSites[i] == rva) { ++s_callCounts[i]; return; }
        if (s_callSites[i] == 0) {
            const int idx = s_callSiteCount.fetch_add(1, std::memory_order_relaxed);
            if (idx < 8) { s_callSites[idx] = rva; s_callCounts[idx] = 1; }
            return;
        }
    }
}
// Hidden-name sampling (Debug): the first few hidden block names per enable,
// so the log shows WHAT the hook is actually dropping (stone vs garbage).
static char s_sampleNames[10][112] = {};
static std::atomic<int>  s_sampleCount{ 0 };
static std::atomic<bool> s_sampling{ false };

static uint32_t bitFromBlockName(const char* n) {
    static const struct { const char* tok; uint32_t bit; } kTab[] = {
        { "coal_ore", kBitCoal },       { "iron_ore", kBitIron },
        { "gold_ore", kBitGold },       { "diamond_ore", kBitDiamond },
        { "redstone_ore", kBitRedstone },{ "lapis_ore", kBitLapis },
        { "emerald_ore", kBitEmerald }, { "copper_ore", kBitCopper },
        { "ancient_debris", kBitDebris },{ "quartz_ore", kBitAlways },
    };
    for (const auto& e : kTab)
        if (std::strstr(n, e.tok)) return e.bit;
    return 0;
}

static void* sehReadLegacyPtr(void* block) {
    void* l = nullptr;
    __try { l = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(block) + 0x68); }
    __except (EXCEPTION_EXECUTE_HANDLER) { l = nullptr; }
    return l;
}

// slot: 0..8 = bit index, 9 = kBitAlways, 15 = not an ore; -1 = name unreadable.
static int classifySlot(void* legacy) {
    char buf[112]; size_t len = 0;
    if (!SDK::sehCopyBlockName(reinterpret_cast<SDK::BlockLegacy*>(legacy), buf, sizeof(buf))) return -1;
    len = std::strlen(buf);
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c < 0x20 || c > 0x7E) return -1;
    }
    const uint32_t bit = bitFromBlockName(buf);
    if (bit == 0) return 15;
    if (bit == kBitAlways) return 9;
    int idx = 0;
    while (!((bit >> idx) & 1u)) ++idx;
    return idx;
}

// Cached classification shared by every xray hook (dispatcher, layer resolver,
// mask getter). slot: 0..8 = ore bit index, 9 = kBitAlways, 15 = not an ore,
// -1 = name unreadable -> vanilla. Runs on chunk-build worker threads; POD-only
// reads under SEH with a lock-free direct-mapped cache.
static int classifyCached(void* legacy) {
    if (!legacy) return -1;
    if (g_clsFlushPending.exchange(false, std::memory_order_relaxed))
        for (auto& e : g_cls) e.store(0, std::memory_order_relaxed);
    const size_t h = static_cast<size_t>(((reinterpret_cast<uintptr_t>(legacy) >> 4) * 0x9E3779B1ull) & 4095u);
    const uint64_t v = g_cls[h].load(std::memory_order_relaxed);
    int slot;
    if ((v >> 8) == static_cast<uint64_t>(reinterpret_cast<uintptr_t>(legacy)) && (v & 0xFF) != 0) {
        slot = static_cast<int>(v & 0xFF) - 1;
    } else {
        slot = classifySlot(legacy);
        if (slot >= 0)
            g_cls[h].store((static_cast<uint64_t>(reinterpret_cast<uintptr_t>(legacy)) << 8)
                           | static_cast<uint64_t>(slot + 1), std::memory_order_relaxed);
    }
    return slot;
}

// ---------------------------------------------------------------------------
// v5: RENDER-LAYER REDIRECT - backup mechanism for the byte path.
//
// Reverse (1.26.50.04): the chunk mesher buckets every block record by render
// layer BEFORE tessellation:
//   * byte path (mesher 0x141C95238) calls sub_146232480(Block*) which returns
//     the per-block render-layer byte (normally [BlockLegacy+0x162]);
//   * mask path (mesher 0x141C95440) calls the BlockLegacy vtable getter at
//     +0x3D8 which returns a 32-bit "render layers" bitmask;
//   * all 22 buckets (indices 0..21 - pushes are UNCHECKED, >= 22 corrupts the
//     heap) are then tessellated through sub_146035800 per record. The only
//     records skipped there are waterloggable blocks (block+0xA1==1, state id
//     not in the state registry, legacy neither water nor lava) - their liquid
//     half is meshed by the dedicated liquid pass, so they are irrelevant.
//
// The tessellator hook above is the actual gate (it drops every non-ore block
// that reaches the dispatcher, whichever bucket it landed in). The resolver
// redirect below is a harmless backup: it moves non-ore blocks into bucket 20
// in the byte path so their geometry is separated from the ore buckets even
// if the dispatcher hook were ever bypassed.
// ---------------------------------------------------------------------------
static constexpr uint8_t kHideLayer = 20;                 // bucket index 0..21
static std::atomic<uint8_t> g_hideLayer{ kHideLayer };

// -- byte-path resolver: sub_146232480(Block*) -> layer byte (returned in AL).
using ResolverFn = uint8_t(__fastcall*)(void* block);
static ResolverFn oResolver = nullptr;

static uint8_t __fastcall hkResolver(void* block) {
    const uint8_t r = oResolver ? oResolver(block) : 0;
    if (!g_texRedirectOn.load(std::memory_order_relaxed) || !block) return r;
    if (classifyCached(sehReadLegacyPtr(block)) == 15)
        return g_hideLayer.load(std::memory_order_relaxed);
    return r;
}

static char __fastcall hkTessellate(void* tess, void* ctx, void* block, void* pos, char a5) {
    if (!g_texRedirectOn.load(std::memory_order_relaxed) || !tess || !block)
        return oTess(tess, ctx, block, pos, a5);

    // _ReturnAddress() - 1: the detour runs on the original instruction
    // stream, so the value the caller's call pushed is the byte AFTER the
    // call - recording it RAW pointed the caller table at mid-instruction
    // addresses (the +1C97948 mystery: an E8 boundary that does not exist).
    // One byte back lands on the CALL instruction start, which is a real
    // boundary and dedupes per call site correctly.
    noteCaller(reinterpret_cast<uintptr_t>(_ReturnAddress()) - 1);
    g_dbgSeen.fetch_add(1, std::memory_order_relaxed);
    void* legacy = sehReadLegacyPtr(block);
    const int slot = classifyCached(legacy);
    if (slot < 0) {                                   // unreadable names are not ores
        g_dbgUnknown.fetch_add(1, std::memory_order_relaxed);
        g_dbgHidden.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    const uint32_t bit = (slot == 15) ? 0u : (slot == 9 ? kBitAlways : (1u << slot));
    const bool keep = bit != 0 && (g_keepMask.load(std::memory_order_relaxed) & bit) != 0;
    if (!keep) {
        g_dbgHidden.fetch_add(1, std::memory_order_relaxed);
        // Sample what is being hidden (Debug only, first 10 per enable).
        if (s_sampling.load(std::memory_order_relaxed)) {
            const int idx = s_sampleCount.fetch_add(1, std::memory_order_relaxed);
            if (idx >= 0 && idx < 10) {
                char nm[112] = {};
                if (SDK::sehCopyBlockName(
                        reinterpret_cast<SDK::BlockLegacy*>(legacy), nm, sizeof(nm)))
                    std::strncpy(s_sampleNames[idx], nm, sizeof(s_sampleNames[idx]) - 1);
            }
        }
        return 0;                                     // emit nothing
    }

    g_dbgKept.fetch_add(1, std::memory_order_relaxed);
    uint8_t* cull = reinterpret_cast<uint8_t*>(tess) + 122;
    if (*cull != 1) return oTess(tess, ctx, block, pos, a5);
    *cull = 0;                                        // no face culling for this ore
    const char r = oTess(tess, ctx, block, pos, a5);
    *cull = 1;
    return r;
}

// ---------------------------------------------------------------------------
// REBUILD DIAGNOSTIC - the section-mesh build wrapper (sub_141CA2AB0). Every
// section the game re-meshes goes through here, so counting/logging these
// calls shows in the log whether a re-mesh actually happened and (via the
// return address) which caller built it. Purely observational: the original
// is always called unchanged.
// ---------------------------------------------------------------------------
using BuildFn = void(__fastcall*)(void* renderChunk, void* buildCtx, void* a3, int a4, char a5, void* a6);
static BuildFn oBuild = nullptr;
static std::atomic<unsigned> g_buildCount{ 0 };
static std::atomic<unsigned> g_buildLogged{ 0 };

static void __fastcall hkBuild(void* rc, void* ctx, void* a3, int a4, char a5, void* a6) {
    // Count EVERY build (the sub_141CA2AB0 wrapper is the ONLY place tessella-
    // tion results are copied into the render chunk, whichever path produced
    // them): the a5==1 filter I briefly shipped made "section builds total"
    // read 0 while the sweep was actually working and hid the real signal.
    {
        const unsigned n = g_buildCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (g_buildLogged.load(std::memory_order_relaxed) < 30 || n % 1000 == 0) {
            g_buildLogged.fetch_add(1, std::memory_order_relaxed);
            // -1: log the CALL instruction start (a real boundary), not the
            // byte after the call (mid-instruction junk, same as noteCaller).
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - 1;
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
            // Blocks through the dispatcher since the previous build: shows
            // whether a build is a FULL section (~hundreds-thousands of
            // blocks) or a tiny partial one.
            static std::atomic<unsigned> s_seenAtLastBuild{ 0 };
            const unsigned seenNow = g_dbgSeen.load(std::memory_order_relaxed);
            const unsigned perBuild = seenNow - s_seenAtLastBuild.exchange(seenNow, std::memory_order_relaxed);
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "section build #%u: renderChunk=%p ctx=%p a4=%d a5=%d ret=+%llX blocks=%u",
                          n, rc, ctx, a4, (int)a5,
                          (unsigned long long)(ret >= base ? ret - base : ret), perBuild);
            LOG_INFO(buf, "Xray");
        }
    }
    oBuild(rc, ctx, a3, a4, a5, a6);
}

// Install the tessellator + render-layer detours. Retried on every enable
// while a signature is unresolved (the scan may finish after module creation).
void installTextureHook() {
    if (!oTess) {
        const uintptr_t fn = SigManager::GetInstance().BlockTessellator_tessellateInWorld;
        if (!fn) {
            LOG_WARN("BlockTessellator::tessellateInWorld signature not resolved - xray unavailable", "Xray");
        } else if (MH_CreateHook(reinterpret_cast<LPVOID>(fn), reinterpret_cast<LPVOID>(&hkTessellate),
                                 reinterpret_cast<LPVOID*>(&oTess)) != MH_OK
                   || MH_EnableHook(reinterpret_cast<LPVOID>(fn)) != MH_OK) {
            oTess = nullptr;
            LOG_ERROR("BlockTessellator::tessellateInWorld hook failed", "Xray");
        } else {
            LOG_INFO("BlockTessellator::tessellateInWorld hooked", "Xray");
        }
    }
    if (!oResolver) {
        const uintptr_t fn = SigManager::GetInstance().BlockRenderLayerResolver;
        if (!fn) {
            LOG_WARN("BlockRenderLayerResolver signature not resolved - byte-path layer redirect unavailable", "Xray");
        } else if (MH_CreateHook(reinterpret_cast<LPVOID>(fn), reinterpret_cast<LPVOID>(&hkResolver),
                                 reinterpret_cast<LPVOID*>(&oResolver)) != MH_OK
                   || MH_EnableHook(reinterpret_cast<LPVOID>(fn)) != MH_OK) {
            oResolver = nullptr;
            LOG_ERROR("BlockRenderLayerResolver hook failed", "Xray");
        } else {
            LOG_INFO("render-layer resolver hooked", "Xray");
        }
    }
    if (!oBuild) {
        const uintptr_t fn = SigManager::GetInstance().MesherBuild;
        if (!fn) {
            LOG_WARN("MesherBuild signature not resolved - rebuild diagnostic unavailable", "Xray");
        } else if (MH_CreateHook(reinterpret_cast<LPVOID>(fn), reinterpret_cast<LPVOID>(&hkBuild),
                                 reinterpret_cast<LPVOID*>(&oBuild)) != MH_OK
                   || MH_EnableHook(reinterpret_cast<LPVOID>(fn)) != MH_OK) {
            oBuild = nullptr;
            LOG_ERROR("MesherBuild hook failed", "Xray");
        } else {
            LOG_INFO("section build diagnostic hooked", "Xray");
        }
    }
}

bool debugEnabled(const Xray* self) {
    return self && self->mSettings.size() > 19 && static_cast<BoolSetting*>(self->mSettings[19])->mValue;
}

void debugLogThrottled(const std::string& msg) {
    static uint64_t last = 0;
    uint64_t now = (uint64_t)GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    LOG_INFO(msg, "Xray");
}

bool readActorPos(SDK::Actor* actor, Vec3* out) {
    if (!actor || !out) return false;
    __try {
        *out = actor->getPos();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

// ===========================================================================
// RE-MESH SWEEP - make ALREADY BUILT render chunks re-tessellate.
//
// The tessellator hook only affects geometry while it is BUILT. Toggling xray
// in an already loaded world must therefore invalidate every loaded render
// chunk so the game re-runs its mesher (which then hits the hook).
//
// Reverse (1.26.50.04) of the re-mesh trigger - this took a while:
//   * BlockSource::setBlock (sub_142D80A20) gates its whole broadcast on
//     (BlockSource+0x1C == 0 && chunk state byte at +0xF0 == 11) and only
//     forwards the BlockSource down to LevelChunk::setBlock when that passes.
//   * LevelChunk::setBlock (sub_1432C31D0) is where the real work happens:
//     it writes the block via SubChunk::_setBlock (which sets the subchunk
//     dirty byte at +0x60) and - ONLY WHEN THE 4TH ARG (the BlockSource) IS
//     NON-NULL - syncs the subchunk's render serial (+4 = +0, clamped >= 0).
//     That serial sync is the change the client mesh builder reacts to; a
//     write without it is invisible to the renderer.
//   * A same-block write is a complete no-op at this level (the function
//     returns early when old == new), which is why the old
//     setBlock(pos, sameBlock, 0x12) sweep re-meshed nothing: only sections
//     that changed NATURALLY (leaf decay - "that one tree") ever showed the
//     xray.
//
// The sweep therefore calls LevelChunk::setBlock DIRECTLY with the live
// BlockSource as the 4th arg (bypassing the BlockSource gate) and performs a
// REAL swap at a safe position: partner in, original back. Two genuine
// old != new writes, two serial syncs, two dirty flags - the exact state a
// real block change produces. Both writes land in the same game-update tick,
// so the mesher only ever sees the original block. Partner blocks are plain
// non-block-entity blocks harvested from the world itself.
//
// BlockSource::fireBlockChanged (vtable slot 63, sub_142D8B9E0) is also
// called directly per position - the game's own block-changed notification,
// bypassing every gate. It is belt-and-braces on top of the serial sync.
//
// The sweep visits every LOADED block section around the player in centre-out
// ring order: one chunk resolve per column, empty sections skipped via the
// sub-chunk storage pointer, ONE real block swap per section with content.
// (The old coverage — row-major order, fixed 3-probe grid, air swaps counted
// as work — is why only a "small random-looking subset" of the world ever
// turned transparent: see docs/re_findings_xray.md §4-5.)
// Draining is budgeted per tick and runs from ModuleManager::OnUpdate
// (Xray_StepRetextureSweep) so the post-DISABLE sweep keeps draining after
// the module's own ticks stop.
// ===========================================================================

using SetBlockFn = bool(__fastcall*)(void* blockSource, const int* pos, const void* block, int updateFlags);
// LevelChunk::setBlock(packedPos, Block const&, BlockSource*)
// packedPos = { x&15, z&15, (uint16)(y - dimension minY) }.
using ChunkSetBlockFn = __int64(__fastcall*)(void* chunk, const unsigned char* packedPos,
                                             const void* block, void* blockSource);
// BlockSource::fireBlockChanged(this, BlockPos const&, layer, Block const& new,
//                               Block const& old, flags, msg, target, extra)
// - vtable slot 63 on 1.26.50.04 (sub_142D8B9E0: iterates the subscriber list
// at this+0x68..0x70 and forwards to each subscriber's vtable slot 4).
using FireBlockChangedFn = void(__fastcall*)(void* blockSource, const int* pos, unsigned layer,
                                             const void* newBlock, const void* oldBlock,
                                             int flags, void* msg, void* target, void* extra);

// Sweep state - only ever touched on the game update thread.
static bool      s_sweepActive = false;
static long long s_sweepCursor = 0, s_sweepTotal = 0;
static int       s_sweepCx = 0, s_sweepCz = 0;      // center chunk column
static int       s_sweepR = 8;                       // radius in chunk columns
static int       s_sweepMinSec = -4, s_sweepSecCount = 24;
static float     s_sweepPx = 0.f, s_sweepPy = 0.f, s_sweepPz = 0.f;
static unsigned  s_sweepProbed = 0, s_sweepFailed = 0;
static unsigned  s_sweepNotified = 0, s_sweepSwapped = 0;
static unsigned  s_swappedOfficial = 0, s_swappedChunk = 0;   // per-path swap wins
static unsigned long long s_buildsAtStart = 0;                // g_buildCount at sweep start
static unsigned long long s_sweepStartMs = 0;                 // wall clock at sweep start
// Coverage-complete pass state (the "everything transparent except ores" fix):
// columns ordered centre-out in rings around the player, one chunk resolve
// per column, empty sections skipped via the sub-chunk storage pointer.
static std::vector<std::pair<int, int>> s_colOrder;   // (dx,dz) ring order
static long long s_curColIdx = -1;                    // column s_curChunk belongs to
static void*     s_curChunk = nullptr;                // LevelChunk* of that column
static unsigned  s_colsLoaded = 0, s_colsSkipped = 0;
static unsigned  s_secEmpty = 0, s_secFallbackAir = 0;
// v6 diagnostics/adaptivity (see "Write-path priority" above):
static bool              s_pathFlipped = false;      // one flip per enable, max
static bool              s_flipChecked = false;      // 32-swap milestone hit this sweep
static unsigned long long s_flipCheckMs = 0;         // ...and when
static unsigned long long s_flipCheckBuilds = 0;     // g_buildCount at that moment
static unsigned long long s_verdictAtMs = 0;         // delayed "+5 s" verdict due
static unsigned long long s_verdictBuilds = 0;       // builds snapshot at finish
static unsigned long long s_verdictSeen = 0;         // hook-blocks snapshot at finish
static bool              s_diagLogged = false;       // per-sweep one-shot diag line
static unsigned          s_serialLogged = 0;         // serial-readback lines this sweep
// Hard wall-clock cap for one sweep. With the coverage rewrite every section
// of a 24-chunk radius takes at most one swap, so 45 s is a generous bound
// even when the builder queue drains slowly (the old 20 s cut full sweeps
// short on busy worlds).
static constexpr unsigned long long kSweepTimeoutMs = 45000;
// Distinct safe Block* harvest: two known-different plain blocks give the
// swap pair (write partner + restore). Filled lazily from the world while
// probing; stays empty only in a perfectly uniform unreadable world.
static SDK::Block* s_swapPool[2] = { nullptr, nullptr };
static int         s_swapCount = 0;
// Keep-mask snapshot of the sweep's TARGET state; a change restarts the sweep
// so ore-toggle edits re-tessellate the world too. Atomic: written from the
// module tick thread, read by the game update thread.
static std::atomic<uint32_t> s_sweepMask{ 0xFFFFFFFFu };
// Pending sweep request (radius in blocks, -1 = none). Requests come from the
// module toggle/keys (any thread); the sweep itself only ever runs on the game
// update thread (Xray_StepRetextureSweep), so the state above stays
// single-threaded.
static std::atomic<int>      s_reqRadius{ -1 };

static SetBlockFn resolveSetBlock() {
    static SetBlockFn fn = nullptr;
    if (!fn) {
        const uintptr_t a = SigManager::GetInstance().BlockSource_setBlock;
        if (a) fn = reinterpret_cast<SetBlockFn>(a);
    }
    return fn;
}

static ChunkSetBlockFn resolveChunkSetBlock() {
    static ChunkSetBlockFn fn = nullptr;
    if (!fn) {
        const uintptr_t a = SigManager::GetInstance().LevelChunk_setBlock;
        if (a) fn = reinterpret_cast<ChunkSetBlockFn>(a);
    }
    return fn;
}

// BlockSource::getChunk({x>>4, z>>4}) - vtable slot 41, exactly what
// BlockSource::setBlock itself uses. POD-only for the __try.
static void* sehGetChunkForPos(void* src, int x, int z) {
    void* chunk = nullptr;
    __try {
        if (!sdk_safe_ptr(src, sizeof(void*) * 42)) return nullptr;
        void** vt = *reinterpret_cast<void***>(src);
        if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * 42)) return nullptr;
        auto getChunk = reinterpret_cast<void* (__fastcall*)(void*, const int*)>(vt[41]);
        if (!getChunk || !sdk_safe_ptr((void*)getChunk, 16)) return nullptr;
        const int cp[2] = { x >> 4, z >> 4 };
        chunk = getChunk(src, cp);
        // chunk+0x82 == 1 marks the invalid/empty chunk - setBlock rejects it.
        if (chunk && !sdk_safe_ptr(chunk, 0x100)) chunk = nullptr;
        if (chunk && *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(chunk) + 0x82) == 1)
            chunk = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        chunk = nullptr;
    }
    return chunk;
}

// The dimension minY exactly like setBlock reads it (this+58, int16).
static int sehReadMinY(void* src) {
    __try {
        if (!sdk_safe_ptr(src, 0x40)) return 0;
        return *reinterpret_cast<short*>(reinterpret_cast<uintptr_t>(src) + 58);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// fireBlockChanged is resolved from the LIVE BlockSource vtable (slot 63), so
// it can never drift from the instance actually being notified.
static FireBlockChangedFn resolveFireBlockChanged(void* src) {
    FireBlockChangedFn fn = nullptr;
    __try {
        if (!sdk_safe_ptr(src, sizeof(void*) * 64)) return nullptr;
        void** vt = *reinterpret_cast<void***>(src);
        if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * 64)) return nullptr;
        fn = reinterpret_cast<FireBlockChangedFn>(vt[63]);
        if (!fn || !sdk_safe_ptr((void*)fn, 16)) fn = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fn = nullptr;
    }
    return fn;
}

static SDK::Block* sehGetBlockAt(void* src, const SDK::BlockPos* pos) {
    SDK::Block* block = nullptr;
    __try {
        block = reinterpret_cast<SDK::BlockSource*>(src)->getBlock(*pos);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        block = nullptr;
    }
    return block;
}

static SDK::BlockLegacy* sehGetBlockLegacy(SDK::Block* block) {
    SDK::BlockLegacy* legacy = nullptr;
    __try {
        legacy = block->getLegacy();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        legacy = nullptr;
    }
    return legacy;
}

static bool sehGetClientWorld(SDK::ClientInstance* ci, SDK::Actor** player,
                              SDK::BlockSource** blockSource) {
    *player = nullptr;
    *blockSource = nullptr;
    __try {
        *player = ci->getLocalPlayer();
        *blockSource = ci->getBlockSource();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *player = nullptr;
        *blockSource = nullptr;
    }
    return *player && *blockSource;
}

static bool sehFireBlockChanged(FireBlockChangedFn fire, void* src, const int* pos,
                                SDK::Block* newBlock, SDK::Block* oldBlock) {
    __try {
        fire(src, pos, 1, newBlock, oldBlock, 2, nullptr, nullptr, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool sehSwapChunk(ChunkSetBlockFn setBlock, void* chunk, const unsigned char* packed,
                         SDK::Block* partner, void* src, FireBlockChangedFn fire,
                         const int* pos, SDK::Block* original) {
    __try {
        setBlock(chunk, packed, partner, src);
        if (fire) fire(src, pos, 1, partner, original, 2, nullptr, nullptr, nullptr);
        setBlock(chunk, packed, original, src);
        if (fire) fire(src, pos, 1, original, partner, 2, nullptr, nullptr, nullptr);
        ++s_sweepSwapped;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool sehSwapBlockSource(SetBlockFn setBlock, void* src, const int* pos,
                               SDK::Block* partner, SDK::Block* original) {
    // The bool returns are the gate verdict: BlockSource::setBlock refuses
    // (returns false) when its gate fails or the write is a same-block no-op.
    // A silent ignore here is exactly how a dead sweep pretends to work.
    __try {
        if (!setBlock(src, pos, partner, 2)) return false;
        if (!setBlock(src, pos, original, 2)) {
            // Partner went in but the restore was refused: NEVER leave the
            // world modified — retry the restore once, then report failure.
            setBlock(src, pos, original, 2);
            return false;
        }
        ++s_sweepSwapped;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void startRetextureSweepAt(int radiusBlocks) {
    // The user-facing Radius setting (16..128) is the BOX-SCAN radius for the
    // optional overlay. The re-tessellation sweep MUST cover the whole render
    // distance regardless: anything narrower leaves a visible ring of
    // untouched chunks ("делает прозрачной только малую часть" — a 4-chunk
    // sweep = 64 blocks while the render distance is 8-12 chunks). 24 chunks
    // (384 blocks) covers every realistic render distance; sections beyond it
    // are unloaded anyway and are skipped by the probe.
    (void)radiusBlocks;
    s_sweepR = 24;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, 0x200)) { s_sweepActive = false; return; }
    SDK::Actor* lp = nullptr;
    __try { lp = ci->getLocalPlayer(); } __except (EXCEPTION_EXECUTE_HANDLER) { lp = nullptr; }
    Vec3 pos;
    if (!lp || !readActorPos(lp, &pos)) { s_sweepActive = false; return; }
    s_sweepPx = pos.x; s_sweepPy = pos.y; s_sweepPz = pos.z;

    SDK::BlockSource* src = ci->getBlockSource();
    int minY = -64, maxY = 320;
    if (src) {
        __try {
            // BlockSource::setBlock validates y against the dimension height
            // as int16 fields (this+58 = min, this+56 = max) - read the same.
            const short lo = *(const short*)((uintptr_t)src + 58);
            const short hi = *(const short*)((uintptr_t)src + 56);
            if (lo < hi && hi - lo <= 1024 && lo > -2048) { minY = lo; maxY = hi; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    s_sweepMinSec = minY >> 4;
    const int maxSec = (maxY - 1) >> 4;
    s_sweepSecCount = maxSec - s_sweepMinSec + 1;
    if (s_sweepSecCount < 1) s_sweepSecCount = 1;

    const int cols = s_sweepR * 2 + 1;
    s_sweepCx = (int)std::floor(pos.x) >> 4;
    s_sweepCz = (int)std::floor(pos.z) >> 4;
    // Centre-out ring order: the visible area around the player is covered
    // FIRST, the horizon last. (The old row-major order started in the far
    // corner, so the world turned transparent in seemingly random patches —
    // the "only some trees" symptom.)
    s_colOrder.clear();
    s_colOrder.reserve((size_t)cols * (size_t)cols);
    for (int ring = 0; ring <= s_sweepR; ++ring) {
        for (int dz = -ring; dz <= ring; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) continue;
                s_colOrder.emplace_back(dx, dz);
            }
        }
    }
    s_curColIdx = -1;
    s_curChunk = nullptr;
    s_sweepTotal = (long long)s_colOrder.size() * s_sweepSecCount;
    s_sweepCursor = 0;
    s_sweepProbed = s_sweepFailed = 0;
    s_sweepNotified = s_sweepSwapped = 0;
    s_swappedOfficial = s_swappedChunk = 0;
    s_colsLoaded = s_colsSkipped = 0;
    s_secEmpty = s_secFallbackAir = 0;
    s_flipChecked = false;
    s_flipCheckMs = 0;
    s_verdictAtMs = 0;
    s_diagLogged = false;
    s_serialLogged = 0;
    s_sweepStartMs = GetTickCount64();
    s_buildsAtStart = g_buildCount.load(std::memory_order_relaxed);
    s_sweepActive = true;
}

void Xray_StartRetextureSweep(int radiusBlocks) {
    s_sweepMask.store(g_keepMask.load(std::memory_order_relaxed), std::memory_order_relaxed);
    s_reqRadius.store(radiusBlocks, std::memory_order_relaxed);
}

static void finishSweep() {
    s_sweepActive = false;
    const unsigned long long builtDuring =
        g_buildCount.load(std::memory_order_relaxed) - s_buildsAtStart;
    LOG_INFO("rebuild sweep done: columns " + std::to_string(s_colsLoaded) + " loaded / " +
                 std::to_string(s_colsSkipped) + " unloaded -> sections " +
                 std::to_string(s_sweepProbed) + " with blocks (" +
                 std::to_string(s_secEmpty) + " empty skipped, " +
                 std::to_string(s_secFallbackAir) + " via air slot), " +
                 std::to_string(s_sweepNotified) + " notified, " +
                 std::to_string(s_sweepSwapped) + " swapped" +
                 " (official=" + std::to_string(s_swappedOfficial) +
                 " chunk=" + std::to_string(s_swappedChunk) +
                 (s_pathFlipped ? ", path flipped" : "") + ")" +
                 (s_sweepFailed ? ", failed " + std::to_string(s_sweepFailed) : "") +
                 " | builds delta " + std::to_string(builtDuring) +
                 " | mesher hook so far: seen " + std::to_string(g_dbgSeen.load()) +
                 " hidden " + std::to_string(g_dbgHidden.load()) +
                 " kept " + std::to_string(g_dbgKept.load()) +
                 " unknown " + std::to_string(g_dbgUnknown.load()),
             "Xray");
    // Builds lag the sweep by seconds (the builder queue drains after the
    // last swap), so the real verdict is measured 5 s later in
    // Xray_StepRetextureSweep.
    s_verdictAtMs = GetTickCount64() + 5000;
    s_verdictBuilds = g_buildCount.load(std::memory_order_relaxed);
    s_verdictSeen = g_dbgSeen.load(std::memory_order_relaxed);
    // Every REAL swap must produce at least one section build; fewer means
    // the renderer ignored invalidated sections (the old half-cover bug).
    if (s_sweepSwapped > 0 && builtDuring == 0)
        LOG_ERROR("swaps landed but no section builds followed - the mesh builder "
                  "is not reacting to the invalidations on this build", "Xray");
    const int sampled = s_sampleCount.load(std::memory_order_relaxed);
    if (sampled > 0) {
        std::string names;
        for (int i = 0; i < sampled && i < 10; ++i) {
            if (i) names += ", ";
            names += s_sampleNames[i];
        }
        LOG_INFO("hidden sample (" + std::to_string(sampled > 10 ? 10 : sampled) +
                 "): " + names, "Xray");
    }
    {
        // Distinct dispatcher caller sites (RVAs): which mesher paths the
        // hook actually served during the sweep.
        std::string sites;
        for (int i = 0; i < 8 && s_callSites[i]; ++i) {
            char b[40];
            std::snprintf(b, sizeof(b), " +%llX(x%u)",
                          (unsigned long long)s_callSites[i], s_callCounts[i]);
            sites += b;
        }
        LOG_INFO("sweep callers:" + (sites.empty() ? " none" : sites), "Xray");
    }
    if (g_dbgSeen.load() == 0)
        LOG_WARN("rebuild sweep finished but the mesher hook saw no blocks - "
                 "chunks were not re-tessellated", "Xray");
}

// Block names whose position must not be force-written (block entities / state
// holders). A same-block write is harmless for plain blocks, but we never touch these.
static bool sweepUnsafeName(const char* n) {
    static const char* const kBad[] = {
        "chest", "furnace", "smoker", "barrel", "hopper", "dropper", "dispenser", "spawner",
        "sign", "bed", "banner", "skull", "head", "command", "beacon", "jukebox", "lectern",
        "bell", "campfire", "brewing", "enchant", "shulker", "lodestone", "sculk", "conduit",
        "pot", "frame", "piston", "observer", "comparator", "repeater", "cauldron", "loom",
        "grindstone", "stonecutter", "smithing", "cartography", "fletching", "composter",
        "bee", "decorated", "bookshelf", "crafter", "vault", "trial", "suspicious", "portal",
        "structure", "jigsaw", "moving", "door", "trapdoor", "gate", "lever", "button",
        "pressure", "rail", "redstone_wire", "torch", "water", "lava",
        // Gravity / state-sensitive: a same-tick swap through the official
        // setBlock path schedules real block updates, so never touch these.
        "sand", "gravel", "powder", "anvil", "scaffolding", "snow", "dragon_egg",
        "turtle_egg", "frogspawn", nullptr };
    for (int i = 0; kBad[i]; ++i)
        if (std::strstr(n, kBad[i])) return true;
    return false;
}

// Exact air test on the registered name. Substring matching ("air" in name)
// is unusable here: "oak_stairs", "warped_stairs" etc. all contain "air" —
// a stairs block must NOT be skipped as air.
static bool sweepNameIsAir(const char* n) {
    return !std::strcmp(n, "air") || !std::strcmp(n, "minecraft:air") ||
           !std::strcmp(n, "cave_air") || !std::strcmp(n, "void_air") ||
           !std::strcmp(n, "minecraft:cave_air") || !std::strcmp(n, "minecraft:void_air");
}

// entry+0x00 / entry+0x04 of the sub-chunk table (content/render serials) —
// the "did the swap actually move the section state" readback, the same
// diagnostic the old logs used ("serial0=-2147483648 serial1=0 -> 0,0").
static bool sehReadSubchunkSerials(void* chunk, int secIdx, int* s0, int* s1) {
    __try {
        if (!sdk_safe_ptr(chunk, 0x150)) return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(chunk);
        char* begin = *reinterpret_cast<char**>(base + 0x140);
        char* end = *reinterpret_cast<char**>(base + 0x148);
        if (!begin || !end || end <= begin) return false;
        const long long count = (long long)(end - begin) / 104;
        if (secIdx < 0 || (long long)secIdx >= count) return false;
        int* e = reinterpret_cast<int*>(begin + (size_t)secIdx * 104);
        *s0 = e[0];
        *s1 = e[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Read the chunk-state byte the setBlock gates key on (+0xF0; 11 = live).
static int sehReadChunkState(void* chunk) {
    __try { return *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(chunk) + 0xF0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

// Per-section "has blocks" test straight out of the LevelChunk sub-chunk
// table (layout verified against sub_1432C31D0: vector at chunk+0x140/+0x148,
// 104-byte entries, index = (y - minY) >> 4, entry+0x38 = SubChunk*). A null
// SubChunk* means the section has no block storage at all (all air) — probing
// it can only waste a rebuild, which is what the old sweep did thousands of
// times per pass.
static bool sehSectionHasStorage(void* chunk, int secIdx) {
    bool has = false;
    __try {
        if (!sdk_safe_ptr(chunk, 0x150)) return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(chunk);
        char* begin = *reinterpret_cast<char**>(base + 0x140);
        char* end = *reinterpret_cast<char**>(base + 0x148);
        if (!begin || !end || end <= begin) return false;
        const long long count = (long long)(end - begin) / 104;
        if (secIdx < 0 || (long long)secIdx >= count) return false;
        void* sub = *reinterpret_cast<void**>(begin + (size_t)secIdx * 104 + 0x38);
        if (sub && !sdk_safe_ptr(sub, 0x10)) return false;
        has = (sub != nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        has = false;
    }
    return has;
}

// Pick a swap partner from the harvested pool (any block whose BlockLegacy is
// readable right now and differs from `exclude`). Returns nullptr when the
// pool has nothing usable this tick.
static SDK::Block* pickPartner(SDK::BlockLegacy* exclude) {
    for (int j = 0; j < s_swapCount; ++j) {
        SDK::Block* cand = s_swapPool[j];
        if (!cand) continue;
        SDK::BlockLegacy* pj = sehGetBlockLegacy(cand);
        if (!pj) continue;                       // stale entry (chunk unloaded)
        if (exclude && pj == exclude) continue;
        return cand;
    }
    return nullptr;
}

// Write-path priority. MEASURED, per session:
//   * 14:57 session: official=2974 / chunk=5  -> 3 577 section builds,
//     684 424 blocks tessellated (the world DID turn transparent);
//   * 09:58 session: chunk-first, official=0 / chunk=360 -> ~30 builds,
//     187 blocks (nothing visible changed).
// The official BlockSource::setBlock carries the engine's own broadcast
// (sub_142D80790) which the re-mesh scheduler actually listens to; the direct
// LevelChunk::setBlock can succeed as a WRITE on server/sub-client worlds
// while scheduling no rebuild. Official FIRST now; chunk as fallback. If the
// first 32 swaps still produce no builds, the sweep flips the priority once
// and retries (see s_officialFirst / s_flipChecked below).
static std::atomic<bool> s_officialFirst{ true };

// Direct render-serial invalidation — the ONLY signal that does not depend
// on the engine accepting the content write (the 09:58 server session took
// 360/360 chunk-path writes and scheduled ~no rebuilds). The coordinator
// rebuilds sections whose stored built-serial differs from the current one,
// so a strictly increasing bump can never collide with a built snapshot.
// Mirrors the real write's arithmetic: +0x00 = content serial, +0x04 =
// render serial synced as max(serial0, 0) (verified in sub_1432C31D0).
static unsigned s_serialEpoch = 16;
static void sehForceSectionSerial(void* chunk, int secIdx, unsigned* outVal) {
    __try {
        if (!sdk_safe_ptr(chunk, 0x150)) return;
        const uintptr_t base = reinterpret_cast<uintptr_t>(chunk);
        char* begin = *reinterpret_cast<char**>(base + 0x140);
        char* end = *reinterpret_cast<char**>(base + 0x148);
        if (!begin || !end || end <= begin) return;
        const long long count = (long long)(end - begin) / 104;
        if (secIdx < 0 || (long long)secIdx >= count) return;
        if (!sdk_safe_ptr(begin + (size_t)secIdx * 104, 104)) return;
        int* e = reinterpret_cast<int*>(begin + (size_t)secIdx * 104);
        unsigned nv = ++s_serialEpoch;
        if (nv >= 0x7FFFFFFFu) { s_serialEpoch = 16; nv = 16; }
        e[0] = (int)nv;
        e[1] = (int)nv;
        if (outVal) *outVal = nv;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// One invalidation action at a world position: direct block-changed
// notification, the guaranteed render-serial bump for this section, then
// partner-in / original-back through the two write paths in s_officialFirst
// order. The world ends each call with the SAME content it started with;
// only the renderer is told that something changed.
static bool sweepSwapAt(ChunkSetBlockFn chunkSetBlock, void* chunk,
                        SetBlockFn setBlock, void* src,
                        FireBlockChangedFn fireBlockChanged, int minY,
                        const SDK::BlockPos& bp, SDK::Block* partner, SDK::Block* original) {
    const int p3[3] = { bp.x, bp.y, bp.z };
    if (fireBlockChanged) {
        if (sehFireBlockChanged(fireBlockChanged, src, p3, original, original)) ++s_sweepNotified;
    }
    if (chunk)
        sehForceSectionSerial(chunk, (bp.y - minY) >> 4, nullptr);
    const bool officialFirst = s_officialFirst.load(std::memory_order_relaxed);
    bool swapped = false;
    if (officialFirst && setBlock) {
        if (sehSwapBlockSource(setBlock, src, p3, partner, original)) {
            swapped = true;
            ++s_swappedOfficial;
        }
    }
    if (!swapped && chunkSetBlock && chunk) {
        const unsigned char packed[4] = {
            (unsigned char)(bp.x & 15),
            (unsigned char)(bp.z & 15),
            (unsigned char)((bp.y - minY) & 0xFF),
            (unsigned char)(((unsigned)(bp.y - minY) >> 8) & 0xFF),
        };
        if (sehSwapChunk(chunkSetBlock, chunk, packed, partner, src,
                         fireBlockChanged, p3, original)) {
            swapped = true;
            ++s_swappedChunk;
        }
    }
    if (!swapped && !officialFirst && setBlock) {
        if (sehSwapBlockSource(setBlock, src, p3, partner, original)) {
            swapped = true;
            ++s_swappedOfficial;
        }
    }
    return swapped;
}

// POD-only body (SEH-legal): one budgeted step of the sweep.
//
// Coverage-complete invalidation pass (re_findings_xray.md §5 — the "only
// some blocks turn transparent" fix):
//   * columns are visited centre-out in rings (visible area first);
//   * a column's chunk is resolved ONCE and the whole column is skipped when
//     it is not loaded (90% of the old sweep's probes died on exactly this);
//   * a section is skipped without a single getBlock when its sub-chunk has
//     no block storage (all air — nothing to hide, nothing to rebuild);
//   * the swap target is a REAL block whenever one is probeable; the air slot
//     is only the fallback for sections that provably store blocks (a swap on
//     air is a net-zero content change that costs a full rebuild while hiding
//     nothing).
static void sweepStepRaw(void* src) {
    ChunkSetBlockFn chunkSetBlock = resolveChunkSetBlock();
    SetBlockFn setBlock = resolveSetBlock();
    FireBlockChangedFn fireBlockChanged = resolveFireBlockChanged(src);
    if (!chunkSetBlock && !setBlock) {
        // Neither write path resolved (signatures missed on this build): the
        // sweep could never invalidate a single section, and without this
        // early-out it would spin silently forever while only naturally
        // re-meshed sections (leaf decay near trees) ever showed the xray.
        // Fail loud instead of pretending to sweep.
        s_sweepActive = false;
        LOG_ERROR("re-mesh sweep aborted: LevelChunk::setBlock and BlockSource::setBlock "
                  "are both unresolved - already loaded chunks cannot re-tessellate", "Xray");
        return;
    }

    // The cached column chunk and the swap-partner pool must never outlive
    // ONE call: chunks unload on the level tick, so a LevelChunk*/Block*
    // carried over from a previous tick could dangle (heap memory is often
    // still mapped, so SEH alone cannot catch that). Re-resolving costs one
    // vtable call per tick at most; the pool refills from the world on the
    // first probes of the call.
    s_curChunk = nullptr;
    s_curColIdx = -1;
    s_swapPool[0] = nullptr;
    s_swapPool[1] = nullptr;
    s_swapCount = 0;

    // 8 spread-out candidate positions per section.
    static const int kOff[8][3] = {
        { 8, 8, 8 }, { 3, 5, 11 }, { 12, 11, 4 }, { 5, 13, 9 },
        { 10, 2, 14 }, { 1, 9, 2 }, { 14, 6, 7 }, { 7, 1, 12 },
    };
    // Empty-section skips are pure pointer reads now, so the probe budget is
    // generous; setBudget paces the invalidations near the empirically proven
    // rate of the 14:57 session (~13 swaps/tick produced 3 577 builds there;
    // flooding the scheduler with hundreds per tick in 09:58 produced ~30).
    const long long probeBudget = 4096;     // sections examined per tick
    const int setBudget = 24;               // section rebuilds triggered per tick
    int sets = 0;

    const long long secCount = (long long)s_sweepSecCount;
    const long long colTotal = (long long)s_colOrder.size();
    const int minY = sehReadMinY(src);

    for (long long n = 0; n < probeBudget && s_sweepCursor < s_sweepTotal; ++n) {
        if (sets >= setBudget) break;
        const long long i = s_sweepCursor++;
        const long long colIdx = i / secCount;
        const int sy = (int)(i - colIdx * secCount);
        if (colIdx >= colTotal) { s_sweepCursor = s_sweepTotal; break; }

        // New column: resolve its chunk ONCE; skip the whole column when it
        // is not loaded.
        if (colIdx != s_curColIdx) {
            const std::pair<int, int>& cr0 = s_colOrder[(size_t)colIdx];
            s_curChunk = sehGetChunkForPos(src, (s_sweepCx + cr0.first) * 16,
                                           (s_sweepCz + cr0.second) * 16);
            s_curColIdx = colIdx;
            if (!s_curChunk) {
                ++s_colsSkipped;
                s_sweepCursor = (colIdx + 1) * secCount;   // jump past the column
                continue;
            }
            ++s_colsLoaded;
            if (!s_diagLogged) {
                s_diagLogged = true;
                char b[160];
                std::snprintf(b, sizeof(b),
                              "sweep diag: fireBlockChanged=%p chunkState(+0xF0)=%d officialFirst=%d",
                              reinterpret_cast<void*>(fireBlockChanged), sehReadChunkState(s_curChunk),
                              static_cast<int>(s_officialFirst.load(std::memory_order_relaxed)));
                LOG_INFO(b, "Xray");
            }
        }

        // Air-only sections cost nothing and hide nothing.
        if (!sehSectionHasStorage(s_curChunk, sy)) { ++s_secEmpty; continue; }
        ++s_sweepProbed;

        const std::pair<int, int>& cr = s_colOrder[(size_t)colIdx];
        const int bx = (s_sweepCx + cr.first) * 16;
        const int by = (s_sweepMinSec + sy) * 16;
        const int bz = (s_sweepCz + cr.second) * 16;

        // Find ONE safe real block to swap; remember the first air slot as
        // the fallback for sections whose probes only hit air/unsafe names.
        int airPos[3] = { 0, 0, 0 };
        bool haveAir = false;
        bool swapped = false;
        for (int k = 0; k < 8; ++k) {
            if (sets >= setBudget) break;
            const SDK::BlockPos bp(bx + kOff[k][0], by + kOff[k][1], bz + kOff[k][2]);
            SDK::Block* b = sehGetBlockAt(src, &bp);
            if (!b) break;                            // reads dying mid-column: stop
            char nm[96] = {};
            SDK::BlockLegacy* legacy = sehGetBlockLegacy(b);
            if (!legacy || !SDK::sehCopyBlockName(legacy, nm, sizeof(nm))) break;
            if (sweepNameIsAir(nm)) {
                if (!haveAir) { airPos[0] = bp.x; airPos[1] = bp.y; airPos[2] = bp.z; haveAir = true; }
                continue;
            }
            if (sweepUnsafeName(nm)) continue;

            // Harvest swap partners: the first two distinct plain blocks,
            // compared by BlockLegacy (different legacy = different block).
            // SEH-guarded: the pool outlives chunk reloads, so never trust a
            // raw dereference here.
            if (s_swapCount < 2) {
                bool known = false;
                for (int j = 0; j < s_swapCount; ++j) {
                    SDK::BlockLegacy* pj = s_swapPool[j] ? sehGetBlockLegacy(s_swapPool[j]) : nullptr;
                    if (pj && pj == legacy) known = true;
                }
                if (!known) s_swapPool[s_swapCount++] = b;
            }
            SDK::Block* partner = pickPartner(legacy);
            if (!partner) continue;                   // pool only holds this legacy so far

            // The official BlockSource::setBlock path FIRST (its broadcast is
            // what the re-mesh scheduler provably listens to — see
            // "Write-path priority"), direct LevelChunk::setBlock as fallback.
            ++sets;
            if (s_serialLogged < 3) {
                // Serial readback around the write: the single most decisive
                // line for "did the swap move the section state".
                int a0 = 0, a1 = 0, b0 = 0, b1 = 0;
                sehReadSubchunkSerials(s_curChunk, sy, &a0, &a1);
                swapped = sweepSwapAt(chunkSetBlock, s_curChunk, setBlock, src,
                                      fireBlockChanged, minY, bp, partner, b);
                sehReadSubchunkSerials(s_curChunk, sy, &b0, &b1);
                ++s_serialLogged;
                char vb[192];
                std::snprintf(vb, sizeof(vb),
                              "swap verify #%u %s @%d,%d,%d ok=%d serial %d,%d -> %d,%d",
                              s_serialLogged, nm, bp.x, bp.y, bp.z, swapped ? 1 : 0, a0, a1, b0, b1);
                LOG_INFO(vb, "Xray");
            } else {
                swapped = sweepSwapAt(chunkSetBlock, s_curChunk, setBlock, src,
                                      fireBlockChanged, minY, bp, partner, b);
            }
            if (swapped) break;
        }

        // Fallback: the section HAS block storage but every probe landed on
        // air/unsafe names (cave floors, mixed sections). Swap through the
        // air slot with a harvested non-air partner: a net-zero content
        // change that still bumps the sub-chunk serial.
        if (!swapped && haveAir && sets < setBudget) {
            SDK::Block* partner = pickPartner(nullptr);
            if (partner) {
                const SDK::BlockPos bp(airPos[0], airPos[1], airPos[2]);
                SDK::Block* airBlock = sehGetBlockAt(src, &bp);
                if (airBlock) {
                    ++sets;
                    swapped = sweepSwapAt(chunkSetBlock, s_curChunk, setBlock, src,
                                          fireBlockChanged, minY, bp, partner, airBlock);
                    if (swapped) ++s_secFallbackAir;
                }
            }
        }
        // Milestone for the adaptive write-path check: remember when the 32nd
        // swap landed; ~2.5 s later Xray_StepRetextureSweep measures whether
        // the builder produced section builds for them (and flips paths when
        // it did not).
        if (swapped && !s_flipChecked && s_sweepSwapped >= 32) {
            s_flipChecked = true;
            s_flipCheckMs = GetTickCount64();
            s_flipCheckBuilds = g_buildCount.load(std::memory_order_relaxed);
        }
        if (!swapped) ++s_sweepFailed;
    }

    // Sweep timeout: the cursor can spin without progress when every probe is
    // air/unloaded (no swap partners) or every write is refused. Finish with a
    // verdict instead of looping forever with the world half-vanilla.
    if (s_sweepStartMs && GetTickCount64() - s_sweepStartMs > kSweepTimeoutMs) {
        LOG_WARN("re-mesh sweep timed out after " +
                 std::to_string(kSweepTimeoutMs / 1000) + "s: probed " +
                 std::to_string(s_sweepProbed) + ", swapped " +
                 std::to_string(s_sweepSwapped) + " - world may be partially unrefreshed", "Xray");
        s_sweepStartMs = 0;
        finishSweep();
        return;
    }
    if (s_sweepCursor >= s_sweepTotal) finishSweep();
}

void Xray_StepRetextureSweep() {
    // Consume a pending request (module enable/disable / ore-toggle change).
    const int req = s_reqRadius.exchange(-1, std::memory_order_relaxed);
    if (req >= 0) {
        if (!resolveChunkSetBlock() && !resolveSetBlock()) {
            LOG_WARN("LevelChunk::setBlock and BlockSource::setBlock unresolved - cannot re-mesh", "Xray");
        }
        startRetextureSweepAt(req);
    }

    // Delayed sweep verdict (+5 s after finish): section builds keep draining
    // long after the last swap lands, so the finish-time builds delta alone
    // cannot tell "no rebuilds" from "rebuilds still queued".
    if (s_verdictAtMs && GetTickCount64() >= s_verdictAtMs) {
        s_verdictAtMs = 0;
        const unsigned long long dB =
            g_buildCount.load(std::memory_order_relaxed) - s_verdictBuilds;
        const unsigned long long dS =
            g_dbgSeen.load(std::memory_order_relaxed) - s_verdictSeen;
        const std::string verdict = (dB >= 8 || dS >= 1000)
            ? "rebuilds landing"
            : "REBUILDS NOT FOLLOWING - this session's scheduler ignores the swap "
              "(server-authoritative meshing? only network block updates re-mesh here)";
        LOG_INFO("sweep verdict +5s: builds +" + std::to_string(dB) +
                 ", hook blocks +" + std::to_string(dS) + " - " + verdict, "Xray");
    }

    // Adaptive write-path check: 2.5 s after the 32nd swap, count what the
    // builder actually produced; if it produced nothing, the PRIMARY path is
    // not scheduling re-meshes on this session - flip once and re-sweep.
    if (s_flipCheckMs && !s_pathFlipped &&
        GetTickCount64() - s_flipCheckMs > 2500) {
        const unsigned long long built =
            g_buildCount.load(std::memory_order_relaxed) - s_flipCheckBuilds;
        s_flipCheckMs = 0;
        if (built < 8) {
            const bool nowOfficialFirst = !s_officialFirst.load(std::memory_order_relaxed);
            s_officialFirst.store(nowOfficialFirst, std::memory_order_relaxed);
            s_pathFlipped = true;
            LOG_WARN("first 32 swaps produced " + std::to_string(built) +
                     " builds in 2.5s - flipping to " +
                     (nowOfficialFirst ? "official" : "chunk") +
                     "-first writes and re-sweeping", "Xray");
            startRetextureSweepAt(0);
            return;
        }
    }

    if (!s_sweepActive) return;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, 0x200)) { s_sweepActive = false; return; }
    SDK::Actor* lp = nullptr;
    SDK::BlockSource* src = nullptr;
    if (!sehGetClientWorld(ci, &lp, &src)) { s_sweepActive = false; return; }

    Vec3 pos;
    if (!readActorPos(lp, &pos)) { s_sweepActive = false; return; }

    // Teleport / long movement mid-sweep: restart around the new position so
    // the sweep always covers what is actually on screen. (startRetexture-
    // SweepAt now forces the 24-chunk render-distance radius on its own.)
    const float dx = pos.x - s_sweepPx, dy = pos.y - s_sweepPy, dz = pos.z - s_sweepPz;
    if (dx * dx + dy * dy + dz * dz > 48.0f * 48.0f) {
        startRetextureSweepAt(0);
        return;
    }

    sweepStepRaw(src);
}

void Xray::resetScan(const SDK::BlockPos& center, int radius) {
    mCenter = center;
    mRadius = radius;
    mCursor = 0;
    // Restart the in-progress pass, but keep the LAST completed set
    // rendering until the new pass swaps in — clearing it here would blank
    // every box for a few ticks on every recenter (movement/teleport).
    mScanBuffer.clear();
    mLegacyKindCache.clear();
}

void Xray::OnEnable() {
    installTextureHook();
    g_keepMask.store(buildKeepMask(this), std::memory_order_relaxed);
    if (oTess || oResolver) { g_clsFlushPending.store(true, std::memory_order_relaxed); g_texRedirectOn.store(true, std::memory_order_relaxed); }
    // Sweep write-path diagnostics: log exactly which invalidation mechanism
    // is available on this build (a silent null here used to be the whole
    // "only some tree turns transparent" failure).
    {
        ChunkSetBlockFn csb = resolveChunkSetBlock();
        SetBlockFn sb = resolveSetBlock();
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            "sweep paths: official setBlock=%p chunk setBlock=%p",
            (void*)sb, (void*)csb);
        LOG_INFO(buf, "Xray");
        if (!csb && !sb)
            LOG_ERROR("no sweep write path resolved - loaded chunks cannot re-tessellate", "Xray");
    }
    s_sampleCount.store(0, std::memory_order_relaxed);
    s_sampling.store(debugEnabled(this), std::memory_order_relaxed);
    // Every enable re-tests the write path from the official-first default
    // (the adaptive check flips it only when this session ignores it).
    s_officialFirst.store(true, std::memory_order_relaxed);
    s_pathFlipped = false;
    // Re-tessellate the already loaded world through the hook (and with a
    // fresh keep-mask) - see "RE-MESH SWEEP" above.
    Xray_StartRetextureSweep((int)static_cast<NumberSetting*>(mSettings[1])->mValue);

    std::lock_guard<std::mutex> lock(mBlocksMutex);
    mFoundBlocks.clear();
    mScanBuffer.clear();
    mLegacyKindCache.clear();
    mCursor = 0;
    LOG_INFO((oTess || oResolver)
                 ? "Xray enabled - the mesher drops every non-ore block; ores render without face culling"
                 : "Xray enabled (mesher hooks unavailable - box overlay only)", "Xray");
}

void Xray::OnDisable() {
    // Instant at the lookup level: the next lookup answers with the real
    // entries again. The sweep re-tessellates the already built chunks back
    // to their real textures.
    g_texRedirectOn.store(false, std::memory_order_relaxed);
    Xray_StartRetextureSweep((int)static_cast<NumberSetting*>(mSettings[1])->mValue);

    std::lock_guard<std::mutex> lock(mBlocksMutex);
    mFoundBlocks.clear();
    mScanBuffer.clear();
    mLegacyKindCache.clear();
    mCursor = 0;
}

void Xray::OnTick() {
    if (!IsEnabled()) return;
    // Ore toggles feed the redirect's keep-mask (read from the tessellation
    // threads inside the hook) - rebuilt every tick so setting changes land
    // immediately. A mask CHANGE restarts the re-mesh sweep so the already
    // built world re-tessellates with the new ore selection as well.
    const uint32_t mask = buildKeepMask(this);
    g_keepMask.store(mask, std::memory_order_relaxed);
    s_sampling.store(debugEnabled(this), std::memory_order_relaxed);
    if (mask != s_sweepMask.load(std::memory_order_relaxed))
        Xray_StartRetextureSweep((int)static_cast<NumberSetting*>(mSettings[1])->mValue);

    if (!boxesEnabled(this)) return;   // the scan only feeds the optional boxes
    bool dbg = debugEnabled(this);
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, 0x200)) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp || !sdk_safe_ptr(lp, 0x230)) return;
    SDK::BlockSource* src = ci->getBlockSource();
    if (!src) return;

    Vec3 pos;
    if (!readActorPos(lp, &pos)) return;
    int radius = (int)static_cast<NumberSetting*>(mSettings[1])->mValue;
    if (radius < 8) radius = 8;
    SDK::BlockPos center((int)std::floor(pos.x), (int)std::floor(pos.y), (int)std::floor(pos.z));
    if (radius != mRadius ||
        std::abs(center.x - mCenter.x) > 8 || std::abs(center.y - mCenter.y) > 8 || std::abs(center.z - mCenter.z) > 8) {
        resetScan(center, radius);
    }

    int side = radius * 2 + 1;
    int total = side * side * side;
    int budget = (int)static_cast<NumberSetting*>(mSettings[2])->mValue;
    if (budget < 256) budget = 256;
    bool onlyExposed = static_cast<BoolSetting*>(mSettings[5])->mValue;
    int r2 = radius * radius;

    // Progressive, double-buffered scan: matches accumulate in mScanBuffer
    // (tick-thread only) and are swapped into mFoundBlocks only when the
    // pass completes, so the render thread always sees a COMPLETE set.
    for (int i = 0; i < budget && total > 0; ++i) {
        if (mCursor == 0) mScanBuffer.clear();   // begin a fresh pass
        int cur = mCursor++;
        bool passDone = (mCursor >= total);

        int lx = cur % side;
        int ly = (cur / side) % side;
        int lz = cur / (side * side);
        int dx = lx - radius, dy = ly - radius, dz = lz - radius;
        if (dx * dx + dy * dy + dz * dz <= r2) {
            SDK::BlockPos bp(center.x + dx, center.y + dy, center.z + dz);
            SDK::Block* block = src->getBlock(bp);
            SDK::BlockLegacy* legacy = block ? block->getLegacy() : nullptr;
            if (legacy) {
                int kind = Kind_None;
                auto it = mLegacyKindCache.find(legacy);
                if (it != mLegacyKindCache.end()) {
                    kind = it->second;
                } else {
                    kind = classify(legacy);
                    mLegacyKindCache[legacy] = kind;
                }
                if (kind != Kind_None && kindEnabled(this, kind) &&
                    (!onlyExposed || exposedToAir(src, bp))) {
                    mScanBuffer[bp] = { bp, colorFor(kind), kind };
                }
            }
        }

        if (passDone) {
            std::lock_guard<std::mutex> lock(mBlocksMutex);
            mFoundBlocks.swap(mScanBuffer);
            mCursor = 0;
            break;
        }
    }

    if (dbg) {
        size_t found = 0;
        {
            std::lock_guard<std::mutex> lock(mBlocksMutex);
            found = mFoundBlocks.size();
        }
        debugLogThrottled("tick: center=" + std::to_string(mCenter.x) + "," + std::to_string(mCenter.y) +
            "," + std::to_string(mCenter.z) + " radius=" + std::to_string(mRadius) +
            " cursor=" + std::to_string(mCursor) + "/" + std::to_string(total) +
            " found=" + std::to_string(found));
    }
}

void Xray::OnRender() {
    if (!IsEnabled()) return;
    bool dbg = debugEnabled(this);
    if (SDK::ScreenView::currentScreenName() != "hud_screen") return;

    // Darkness FIRST: this module is registered before every other overlay,
    // so the plate lands UNDER the boxes of every module that draws later
    // (ESP players, BlockESP, ...) — the world dims, the ores keep glowing.
    // It is independent of the box overlay and draws with or without it.
    bool darkness = static_cast<BoolSetting*>(mSettings[6])->mValue;
    float darkA = static_cast<NumberSetting*>(mSettings[7])->mValue;
    if (darkA < 0.0f) darkA = 0.0f; if (darkA > 1.0f) darkA = 1.0f;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 screen = ImGui::GetIO().DisplaySize;

    if (darkness && darkA > 0.0f)
        dl->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(0, 0, 0, (int)(darkA * 255.0f)), 0.0f);

    // The texture redirect IS the xray; the boxes below are the optional
    // BlockESP-style extra ("Overlay Boxes") and cost a world scan.
    if (!boxesEnabled(this)) return;

    std::vector<FoundBlock> blocks;
    {
        std::lock_guard<std::mutex> lock(mBlocksMutex);
        if (mFoundBlocks.empty()) return;
        blocks.reserve(mFoundBlocks.size());
        for (const auto& kv : mFoundBlocks) blocks.push_back(kv.second);
    }

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, 0x200)) return;

    float mvp[16];
    Vec3 origin(0, 0, 0);
    if (!acquireCameraMVP(ci, mvp, origin)) {
        if (dbg) debugLogThrottled("render: camera MVP unavailable");
        return;
    }

    int frame = ImGui::GetFrameCount();
    static int s_drawnFrame = -1;
    if (s_drawnFrame == frame) return;
    s_drawnFrame = frame;

    int style = static_cast<EnumSetting*>(mSettings[0])->mValue;   // 0=3D 1=2D 2=Outline
    bool doFill = static_cast<BoolSetting*>(mSettings[3])->mValue;
    float fillA = static_cast<NumberSetting*>(mSettings[4])->mValue;
    if (fillA < 0.0f) fillA = 0.0f; if (fillA > 1.0f) fillA = 1.0f;

    // Cube topology comes from the shared BoxOverlay tables (same canonical
    // corner layout projectBlockBox emits).

    // Draw cap: pathological worlds (deepslate everywhere, huge radius)
    // can hold tens of thousands of matches; the far half of them is
    // sub-pixel anyway. Keep the frame budget bounded.
    const size_t kDrawCap = 4096;
    size_t drawn = 0;
    for (const FoundBlock& fb : blocks) {
        if (drawn >= kDrawCap) break;
        ImVec2 p[8]; bool v[8];
        if (projectBlockBox(fb.pos, mvp, origin, screen, p, v) < 2) continue;
        ++drawn;
        ImColor col = fb.color;
        col.Value.x = std::min(col.Value.x * 1.1f, 1.0f);
        col.Value.y = std::min(col.Value.y * 1.1f, 1.0f);
        col.Value.z = std::min(col.Value.z * 1.1f, 1.0f);
        ImColor fill(fb.color.Value.x, fb.color.Value.y, fb.color.Value.z, fillA);
        ImU32 outline = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.8f));

        if (style == 1) {
            // 2D: screen-space bounding rectangle over the projected corners.
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            for (int i = 0; i < 8; ++i) {
                if (!v[i]) continue;
                if (p[i].x < minX) minX = p[i].x;
                if (p[i].y < minY) minY = p[i].y;
                if (p[i].x > maxX) maxX = p[i].x;
                if (p[i].y > maxY) maxY = p[i].y;
            }
            if (maxX <= minX || maxY <= minY) continue;
            ImVec2 mn(minX, minY), mx(maxX, maxY);
            if (mn.x < -50 || mn.y < -50 || mx.x > screen.x + 50 || mx.y > screen.y + 50) continue;
            if (doFill) dl->AddRectFilled(mn, mx, fill, 0.0f);
            dl->AddRect(mn, mx, outline, 0.0f, 0, 2.0f);
            dl->AddRect(mn, mx, col, 0.0f, 0, 1.2f);
        } else {
            // 3D wireframe or Outline (silhouette hull of the cube).
            if (doFill) {
                for (int f = 0; f < 6; ++f) {
                    const int* q = SDK::BoxOverlay::kCubeFaces[f];
                    if (!(v[q[0]] && v[q[1]] && v[q[2]] && v[q[3]])) continue;
                    ImVec2 quad[4] = { p[q[0]], p[q[1]], p[q[2]], p[q[3]] };
                    dl->AddConvexPolyFilled(quad, 4, fill);
                }
            }
            if (style == 0) {
                for (int i = 0; i < 12; ++i) {
                    int a = SDK::BoxOverlay::kCubeEdges[i][0], b = SDK::BoxOverlay::kCubeEdges[i][1];
                    if (v[a] && v[b]) {
                        dl->AddLine(p[a], p[b], outline, 2.2f);
                        dl->AddLine(p[a], p[b], col, 1.2f);
                    }
                }
            } else {
                ImVec2 valids[8]; int nv = 0;
                for (int i = 0; i < 8; ++i) if (v[i]) valids[nv++] = p[i];
                if (nv >= 3) {
                    // Silhouette hull of the 8 projected corners (shared).
                    ImVec2 hull[18];
                    const int hn = SDK::BoxOverlay::convexHull2D(valids, nv, hull);
                    if (hn >= 3) {
                        dl->AddPolyline(hull, hn, outline, ImDrawFlags_Closed, 2.2f);
                        dl->AddPolyline(hull, hn, col, ImDrawFlags_Closed, 1.2f);
                    }
                }
            }
        }
    }

    if (dbg)
        debugLogThrottled("render: blocks=" + std::to_string(blocks.size()) + " drawn=" + std::to_string(drawn));
}
