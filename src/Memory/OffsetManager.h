#pragma once
#include <cstdint>
#include <cstddef>

// Runtime field offsets for the current game binary.
// Separate from SigManager (which only stores code addresses / call sites).
// Values start as known fallbacks and are overwritten when a matching
// offset-signature resolves during scan.
class OffsetManager {
public:
    static OffsetManager& GetInstance() {
        static OffsetManager instance;
        return instance;
    }

    // ---- Actor ------------------------------------------------------------
    // Direct component-pointer fields on Actor (mov [reg+disp32] sites).
    int Actor_mStateVector = 0x240;
    int Actor_mAABB        = 0x248;
    int Actor_mRotation    = 0x250;
    // GameMode* field on LocalPlayer — often 0 until dynamic resolve.
    int Actor_mGameMode    = 0xAC8;
    // SerializedSkin pointer field (optional; may stay 0).
    int Actor_mSerializedSkin = 0;

    // ---- Rendering --------------------------------------------------------
    int LevelRenderer_mRendererPlayer   = 0x480;
    // Camera origin inside LevelRendererPlayer (renderLevel reads this).
    int LevelRendererPlayer_mCameraPos  = 0x64C;
    int LevelRendererPlayer_mViewMatrix = 0xF10;
    int LevelRendererPlayer_mProjMatrix = 0xF50;

    // ---- Items ------------------------------------------------------------
    // Item::mName std::string base displacement (runtime-derived).
    int Item_mNameStrOffset = 0x78;

    // ---- MobEffectInstance ------------------------------------------------
    int EffectInstance_id        = 0x00;
    int EffectInstance_duration  = 0x04;
    int EffectInstance_amplifier = 0x20;
    int EffectInstance_noCounter = 0x42;
    int EffectInstance_size      = 0xA8;

    // ---- ClientInstance ---------------------------------------------------
    int CI_MinecraftGame = 0x1C8;
    int CI_Minecraft     = 0x1D0;
    int CI_LevelRenderer = 0x1E0;
    int CI_PacketSender  = 0x1F0;
    int CI_GuiData       = 0x668;
    int CI_WeakEntityRef = 0x268;
    // Deprecated CI-relative camera getters; matrices belong to GameRenderer.
    int CI_ViewMatrix    = 0x3C0;
    int CI_FovX          = 0xF88;
    int CI_FovY          = 0xF9C;

    // ---- MinecraftGame ----------------------------------------------------
    int MG_ClientMap     = 0x9D0;
    int MG_ClientMapSize = 0x9D8;
    int MG_GameRenderer  = 0x1628;

    // ---- Minecraft / GameSession -----------------------------------------
    int MC_GameSession   = 0xD0;
    int MC_Timer         = 0xE8;
    int GS_Level         = 0x40;

    // ---- Platform_GameCore -----------------------------------------------
    // get(): global -> [+0x8] = Platform_GameCore*; game itself does
    // `mov r13,[r15+8]` at the same site the signature anchors (0x14007D8F2).
    int PGC_Self         = 0x8;     // OK  unchanged
    // 1.26.50.04+ does not store MinecraftGame in a field of Platform_GameCore.
    // The WndProc reads [r15+18h], but that slot is a 32-bit scalar (vtable[68]
    // is `mov eax,[rcx+18h]; ret`), so treating it as the pointer handed back
    // 0x1 and failed every ClientInstance lookup. Latite (same build) walks
    // *(pgc+0x118) -> holder, *(holder+0x48) -> MinecraftGame.
    int PGC_GameHolder    = 0x118;
    int PGC_MinecraftGame = 0x48;

    // ---- Network chain: CI -> NetworkSystem -> RemoteConnectorComposite ----
    // On this build the connector OPERATIONS dispatch via the composite's own
    // vtable (getConnectedGameInfo=vt[3], getNetworkIdentifier=vt[17]); there
    // is no plain composite->RakNetConnector field. composite+0x70 is the
    // NonOwnerPointer CONTROL BLOCK.
    int PS_NetworkSystem  = 0x20;   // DEAD — kept for the OffsetValidator printout
    int NS_RemoteConnector = 0x120; // resolved dynamically by the connector signature
    int NS_PeerHolder      = 0x90;  // native pre-gate: active transport pointer at +0x18
    int NS_PeerHolderActive= 0x18;
    int RC_RakNetConnector = 0;     // no such hop exists on 1.26.60.05  // ??  1.26.42 value (unreverified)

    // ---- GuiData ---------------------------------------------------------
    int GD_Width        = 0x40;     // ??  raw framebuffer width
    int GD_Height       = 0x44;     // ??  raw framebuffer height
    int GD_ScaledWidth  = 0x50;     // ??  width * guiScaleFrac
    int GD_ScaledHeight = 0x54;     // ??  height * guiScaleFrac
    int GD_GuiScale     = 0x5C;     // ??  1.26.42 value
    int GD_GuiScaleFrac = 0x60;     // ??  1.26.42 value

    // ---- Level -----------------------------------------------------------
    int LVL_PlayerList  = 0x4E0;    // ??  1.26.42 value (unordered_map<UUID,PlayerListEntry>)

    // ---- ScreenView / UIControl ------------------------------------------
    // Reversed on 1.26.50.04 from sub_144837E60 (ScreenView::setupAndRender),
    // the _passViewCommand lambda_68 _R (sub_144876CF0), VisualTree::
    // getControlByName (sub_1445B9650), the name predicate sub_1445C7E30,
    // the tree walker sub_1445B8ED0, and the RTTI-demangled ScreenView ctor
    // (??0ScreenView@@QEAA@AEAVKeyboardManager@@V?$shared_ptr@VScreenController@@...)
    // inlined into the UIScene ctor sub_144851EE0.
    //
    // The whole ScreenView head shifted +8 vs 1.26.30 (0x38/0x40/0x48 -> 0x40/0x48/0x50).
    // SV is a shared_ptr pair: ptr +0x40, control block +0x48.
    int SV_ScreenController = 0x40; // OK  was 0x38 | ScreenView ctor: mov [rcx+40h],rbx / mov [rcx+48h],r13 (shared_ptr pair, lock inc [rax+8] on copy)
    int SV_VisualTree       = 0x50; // OK  was 0x48 | ctor: mov [rcx+50h],r15 (unique_ptr null-out in caller); _passViewCommand lambda: mov rdi,[r14+50h]
    int UI_Position   = 0x10;       // ?? UIControl::mPosition (Vec2)
    int UI_Flags      = 0x18;       // OK  name predicate sub_1445C7E30: (a2+24) & 0x10
    int UI_Name       = 0x20;       // OK  name predicate sub_1445C7E30: a2+32 data / a2+48 size / a2+56 cap
    int UI_RelPos     = 0x40;       // ?? UIControl::mRelativePosition
    int UI_Size       = 0x48;       // ?? UIControl::mSize
    int UI_Parent     = 0x88;       // ?? shared_ptr<UIControl>
    int UI_Children   = 0x98;       // OK  walker sub_1445B8ED0: a3[19]/a3[20] = begin/end, stride 16
    int VT_Color      = 0x30;       // ?? VisualTree::mColor
    int VT_Tessellator = 0xB0;      // ScreenContext::mTessellator
    int VT_RootControl = 0x08;      // OK  getControlByName sub_1445B9650: mov r8,[rcx+8]; sub_1445B9160: mov rsi,[rcx+8]

    // Apply a dynamically discovered GameMode field offset (e.g. from the
    // pick-range swap path). No-op if off is out of the expected band.
    void setActorGameMode(int off) {
        if (off > 0 && off < 0x2000) Actor_mGameMode = off;
    }

private:
    OffsetManager() = default;
};

// Short alias used by hot paths.
inline OffsetManager& Offsets() { return OffsetManager::GetInstance(); }

// ---------------------------------------------------------------------------
// Compile-time layout constants.
//
// These are NOT runtime-resolved (no signature derives them), so they are
// constexpr rather than OffsetManager fields — but they were being re-declared
// in two or three translation units each, which is how the codebase ended up
// with `LevelRenderer -> LevelRendererPlayer` written as 0x468 in OffsetManager
// and 0x478 in Zoom.cpp, and `TextPacket +0xA0` documented as `params` in one
// file and `type` in another.
//
// Anything a reverse engineer would grep for lives here in one place, which is
// also what makes it cheap to hand the whole block to VMProtect.
// ---------------------------------------------------------------------------
namespace Layout {

// Readability probe sizes for the ClientInstance -> Minecraft -> Level ->
// LocalPlayer walk. Written out by hand in 23 files before this.
namespace Probe {
    constexpr size_t ClientInstance = 0x200;
    constexpr size_t Minecraft      = 0xD8;
    constexpr size_t Level          = 0x10;
    constexpr size_t Actor          = 0x258;
    constexpr size_t ActorAabbField = 0x250;
    constexpr size_t HitResult      = 0x4C;
    constexpr size_t ItemStack      = 0x80;
}

    // Actor fields that are plain literals in the SDK today.
    namespace Actor {
        constexpr uintptr_t EntityContext = 0x08;
        constexpr uintptr_t Registry      = 0x10;   // EntityContext::registry
        constexpr uintptr_t TicksExisted  = 0x1C0;
        constexpr uintptr_t Supplies      = 0x5E0;  // Player::mSupplies (PlayerInventory*)
        constexpr uintptr_t LocalName     = 0xBE8;  // std::string
        constexpr uintptr_t SerializedSkinRef = 0xAD8;
        // SkinImage inside the SerializedSkinRef wrapper.
        constexpr uintptr_t SkinImageInRef    = 0xA0;
        // "Attack held on a block" input flag. 1.26.42.01: GameMode::
        // continueDestroyBlock (sub_142D2E130) only accumulates break progress
        // while byte[mPlayer+0xB22] == 1, and destroyBlock (sub_142D2DCF0)
        // clears it after a successful break. The vanilla input system sets it
        // while the attack button is held over a block; OreMiner sets it itself.
        constexpr uintptr_t DestroyInput = 0xB4A;
    }

// PlayerInventory.
namespace PlayerInv {
    constexpr uintptr_t SelectedSlot = 0x10;
    constexpr uintptr_t InHandSlot   = 0x1C;
    constexpr uintptr_t Container    = 0xB8;
}

// MoveInputComponent — Baritone and Twerk both declared these.
namespace MoveInput {
    constexpr uintptr_t Flags = 0x10;
    constexpr size_t    Size  = 0x64;
}

namespace MovePlayer {
    constexpr uintptr_t PlayerID = 0x30;  // int64 runtime id
    constexpr uintptr_t Pos      = 0x38;  // vec3
    constexpr uintptr_t Rot      = 0x44;  // vec2
    constexpr uintptr_t YHeadRot = 0x4C;
    constexpr uintptr_t ResetPos = 0x50;  // PositionMode
    constexpr uintptr_t OnGround = 0x51;
    constexpr uintptr_t RidingID = 0x58;
    constexpr uintptr_t Tick     = 0x68;
}

// ContainerScreenController. (Layout shared by 1.26.50.04 and 1.26.51.01 —
// verified in both binaries: the ScreenView ctor pair and handleAutoPlace's
// first derefs are byte-identical apart from relocated disp32s.)
namespace Csc {
    // this+0x0D48 was NonOwnerPointer<IClientInstance> on 1.26.42; on 1.26.50+
    // it is a std::string (the screen name). Kept for reference only.
    constexpr uintptr_t CtxField  = 0x0D48;
    constexpr uintptr_t SlotModel = 0x12C0;  // handleAutoPlace: mov rcx,[rbx+12C0h]
    constexpr size_t    MinSize   = 0x12D8;
    // _getSelectedSlotInfo sits at vtable[172] of the family's BASE vtable
    // (185 slots) — NOT [63] as on 1.26.42. The slot is subclass-dependent
    // (64/120/230/…), so Inventory.cpp now scans the vtable for the marker
    // instead of comparing this fixed index; this value is informational only.
    constexpr int       TypeVIndex = 172;
}

// ChatScreenController (1.26.50.04/1.26.51.01, sub_144DE0780). The submit path
// is a ONE-argument method (this); the typed line is a std::string member, not
// a parameter — data at +0xD60, size +0xD70, capacity +0xD78 (verified in IDA
// from the controller's own SSO branch and its `mov byte [rcx+0xDA2],1`).
namespace ChatScreen {
    constexpr uintptr_t Text = 0xD60;
}

    // Vtable slot indices. Written as raw integers at 20+ call sites, sometimes in
    // two notations for the same slot (getLocalPlayer as both 31 and 0x1F).
    //
    // `Guard(slot)` is the sdk_safe_ptr element count the call sites need: reading
    // vt[slot] requires slot+1 entries to be present. Every site spelled that "+1"
    // inline (`sizeof(void*) * 0x6F` next to `vt[0x6E]`), which is exactly the kind
    // of pair that drifts when only one of the two gets updated.
    //
    // GameMode layout for 1.26.42.01, verified against the live vtable at
    // 0x14E804070 AND against runtime behaviour:
    //   1 startDestroyBlock    (sub_142D2D460 — instant-path helper, writes the
    //                           target into gm+0x10/0x1C)
    //   2 destroyBlock         (sub_142D2DCF0 — one-shot finish, clears the
    //                           byte[lp+0xB22] attack flag on success)
    //   3 continueDestroyBlock (sub_142D2E130 — accumulates progress at gm+0x24,
    //                           gated on same-pos AND byte[lp+0xB22]==1, finishes
    //                           itself via vt[2] at progress >= 0.99998999)
    //   4 stopDestroyBlock     (sub_142D2ED50 — resets progress + dig timer)
    // Everything ELSE keeps the old +1 offsets that were already proven at
    // runtime (Aura attack on vt[15], Scaffold buildBlock on vt[7],
    // AutoClicker/PearlTrigger useItem on vt[12]): the destroy block moved down
    // by one slot on this build, the rest of the interface did not.
    namespace VTable {
        constexpr int Packet_getId               = 1;
        constexpr int PacketSender_send          = 2;
        constexpr int ClientInstance_getLocalPlayer = 30;
        constexpr int ClientInstance_getGuiData = 216;
        constexpr int ClientInstance_getGameRenderer = 184;
        constexpr int ClientInstance_getRendererHelper = 186;
        // 1.26.50.04: NetworkSystem moved off the PacketSender onto a virtual.
        // The game's own connection-info reader resolves the NetworkSystem
        // through the IClientInstance virtual and reads the connector at
        // NS_RemoteConnector; the slot is sub_145DCE590 in the IClientInstance
        // vtable off_14E9721B0, which sits at ClientInstance+0x00.
        constexpr int ClientInstance_getNetworkSystem = 292;
        // 1.26.50.04: CI gained one virtual before old slot 187, shifting every
        // slot >= 187 up by one. getSoundEngine 316 -> 317 (316 is now a
        // distractor that loads vtable+0E8h instead of dispatching through
        // MinecraftGame). grabCursor/releaseCursor 309/310 -> 310/311.
        constexpr int ClientInstance_getSoundEngine = 317;
        constexpr int Level_getBlockSource       = 30;
        // CONFIRMED unchanged on 1.26.60.05 (compatibility/actor-list-1.26.60.json):
        // slot 0x145, impl RVA 0x232F290. The game grows the out-vector through
        // its OWN allocator — callers must free the raw block with the game's
        // sized free (see ClientInstance.h), never a CRT vector dtor.
        constexpr int Level_getRuntimeActorList  = 0x145;
        constexpr int Actor_getNameTag           = 63;
        // 1.26.50.04: swing moved DOWN one slot (0x6E -> 0x6D). The recompile
        // inserted a virtual above it. Pinned from the LocalPlayer vtable
        // off_14E8E0BC0 (245 slots, set by the constructor sub_144758FD0 in the
        // same TU as the WeakEntityRef resolver ClientInstance::getLocalPlayer
        // uses): slot 0x6D is overridden per subclass but every override shares
        // the "am I already mid-swing" prologue — flag byte[+0x434], counter
        // dword[+0x410] gated on sub_142078EC0(this)/2, then both are set. The
        // LocalPlayer override (sub_1447626C0) additionally sends the
        // AnimatePacket. Cross-checked against the game's own 10 dispatch sites
        // (mov rax,[rax+368h]; mov dl,<src>; xor r8d,r8d; guard call), which
        // pass exactly the SwingSource values: 2 Mine, 3 Interact, 4 Attack,
        // 5 UseItem, 8 Event — so the enum and the dl calling convention are
        // unchanged. The old 0x6E now resolves to sub_1401EDAA0, a block-feature
        // hash-grid lookup that silently no-ops through the SEH wrapper.
        constexpr int Actor_swing                = 0x6D;
        // UNVERIFIED / UNUSED on 1.26.50.04. On the old build this was
        // Actor::setSprinting; it now resolves to sub_1401E38C0, a
        // float-returning component getter in LocalPlayer vt 0x14E8E0BC0. No
        // ActorDataFlagComponent-writing setter exists anywhere in that 245-slot
        // vtable (the only hash-inlining entries are the is*() getters at slots
        // 31/32/60 and the interact function at 116), so the virtual is gone:
        // Actor::setSprinting now writes the flag through setStatusFlag and
        // nothing dispatches through this slot. Kept for documentation only.
        constexpr int Actor_setSprinting         = 0x8B;
        constexpr int Item_getMaxDamage          = 0x24;
        // GameMode — 1.26.50.04/1.26.51.01. Verified against the live vtable at
    // 0x14E826560 (dumped and decoded slot by slot; re-verified by the fix-0920
    // reverse pass):
    //   1 startDestroyBlock  2 destroyBlock  3 continueDestroyBlock
    //   4 stopDestroyBlock   7 buildBlock   10 getPickRange  12 useItem
    //   15 attack
    // getPickRange moved 11 -> 10. Slot 11 is a pick-range-like getter
    // (sub_14259EF40), NOT a useItem overload — an earlier pass mislabeled it
    // "baseUseItem" and the resulting calls were the false "direct useItem is
    // dead" conclusion. Only slot 12 (or SigManager::GameMode_useItem) may be
    // called; see GM::useItemDirect. buildBlock's 4th arg also changed meaning:
    // it now selects the item source (see GM::buildBlock).
        constexpr int GameMode_startDestroy      = 1;
        constexpr int GameMode_destroyBlock      = 2;
        constexpr int GameMode_continueDestroy   = 3;
        constexpr int GameMode_stopDestroy       = 4;
        constexpr int GameMode_buildBlock        = 7;
        constexpr int GameMode_getPickRange      = 10;
        constexpr int GameMode_useItem           = 12;
        constexpr int GameMode_attack            = 15;

        constexpr size_t Guard(int slot) { return sizeof(void*) * (size_t)(slot + 1); }
    }

// GameMode field + the band its pointer is searched in on LocalPlayer.
namespace GameMode {
    constexpr uintptr_t PlayerBackRef = 0x8;   // GameMode::mPlayer
    // float, [0..1] — the destroy progress continueDestroyBlock accumulates
    // (sub_142D2E130: `*(float*)(gm+0x24) += elapsed/0.05 * rate`), finished at
    // >= 0.99998999 by a self-dispatch to vtable[destroyBlock].
    constexpr uintptr_t BreakProgress = 0x24;
    // uint64, timestamp (ns) of the last successful buildBlock placement. The
    // engine (sub_14259C690, GameMode::buildBlock) refuses to place again until
    // ~100-300ms (a speed/latency-derived cooldown) has elapsed since this stamp:
    //   v45 = *(u64*)(gm + 0x88) + (u64)cooldownMs * 1000000;
    //   if (v45 < now_ns) { ...dispatch the place...; gm[0x88] = ...; }
    // Vanilla places once per click, so the cooldown never binds there. Scaffold
    // spams buildBlock every frame and gets throttled to ~3-10 blocks/sec — the
    // "slow, and I fall off while sprinting" report in both modes. Zeroing the
    // stamp before every call removes that internal ceiling so the module's own
    // Speed gate becomes the only limit.
    constexpr uintptr_t BuildCooldownLastNs = 0x88;

    // HOLD-TO-PLACE repeat gate (GameMode::continueDestroyBlock, sub_14259B120).
    // While RMB is held the game re-enters continueDestroyBlock every tick, and
    // each entry only re-places while this stamp is already in the past:
    //     if (*(u64*)(gm + UseRepeatNextNs) < now_ns) {
    //         ... buildBlock(...) ...
    //         *(u64*)(gm + UseRepeatNextNs) = now_ns + 1000000 * *(u64*)(gm + UseRepeatIntervalMs);
    //     }
    // GameMode::startDestroyBlock (sub_14259A470) primes BOTH stamps on the
    // initial click, which is exactly why the old "zero gm+0x88 only" code
    // behaved like a double-click: the first click's write to gm+0x90 still
    // gated every subsequent repeat. Zeroing BOTH is what actually removes the
    // hold delay. The interval lives at gm+0xA0 and is the per-use gap in ms.
    constexpr uintptr_t UseRepeatNextNs     = 0x90;
    constexpr uintptr_t UseRepeatIntervalMs = 0xA0;

    constexpr uintptr_t ScanBegin     = 0x800; // LocalPlayer field scan band
    constexpr uintptr_t ScanEnd       = 0xC00;
    constexpr int       FallbackField = 0xAC8; // last-resort Actor::mGameMode
}

// PlayerAuthInputPacket payload offsets (1.26.42.01), read back from
// PlayerAuthInputPayload_validate (0x142DACAA0) which names every field it
// bounds-checks; packet base and payload base coincide (mRot is +0x30 in both).
namespace PAI {
    constexpr uintptr_t Rot              = 0x30; // vec2 {pitch, yaw}
    constexpr uintptr_t Pos              = 0x38; // vec3
    constexpr uintptr_t YHeadRot         = 0x44; // float
    constexpr uintptr_t PosDelta         = 0x48; // vec3
    constexpr uintptr_t VehicleRot       = 0x54; // vec2
    constexpr uintptr_t AnalogMoveVector = 0x5C; // vec2
    constexpr uintptr_t Move             = 0x64; // vec2
    constexpr uintptr_t InteractRotation = 0x6C; // vec2
    constexpr uintptr_t RawMoveVector    = 0x80; // vec2
    constexpr uintptr_t InputData        = 0x88; // uint64 AuthInputAction bitset
}

} // namespace Layout
