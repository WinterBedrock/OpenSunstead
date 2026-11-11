#pragma once
#include <cstdint>
#include <cstddef>

class OffsetManager {
public:
    static OffsetManager& GetInstance() {
        static OffsetManager instance;
        return instance;
    }

    int Actor_mStateVector = 0x240;
    int Actor_mAABB        = 0x248;
    int Actor_mRotation    = 0x250;
    int Actor_mGameMode    = 0;
    int Actor_mSerializedSkin = 0;

    int LevelRenderer_mRendererPlayer   = 0x480;
    int LevelRendererPlayer_mCameraPos  = 0x64C;
    int LevelRendererPlayer_mViewMatrix = 0xF10;
    int LevelRendererPlayer_mProjMatrix = 0xF50;

    // Item::mName std::string base displacement
    int Item_mNameStrOffset = 0x78;

    int EffectInstance_id        = 0x00;
    int EffectInstance_duration  = 0x04;
    int EffectInstance_amplifier = 0x20;
    int EffectInstance_noCounter = 0x42;
    int EffectInstance_size      = 0xA8;

    int CI_MinecraftGame = 0x1C8;
    int CI_Minecraft     = 0x1D0;
    int CI_LevelRenderer = 0x1E0;
    int CI_PacketSender  = 0x1F0;
    int CI_GuiData       = 0x668;
    int CI_WeakEntityRef = 0x268;
    int CI_ViewMatrix    = 0x3C0;
    int CI_FovX          = 0xF88;
    int CI_FovY          = 0xF9C;

    int MG_ClientMap     = 0x9D0;
    int MG_ClientMapSize = 0x9D8;
    int MG_GameRenderer  = 0x1628;

    int MC_GameSession   = 0xD0;
    int MC_Timer         = 0xE8;
    int GS_Level         = 0x40;

    int PGC_Self         = 0x8;     // OK  unchanged
    int PGC_GameHolder    = 0x118;
    int PGC_MinecraftGame = 0x48;


    int NS_RemoteConnector = 0x120; // resolved dynamically by the connector signature
    int NS_PeerHolder      = 0x90;  // native pre-gate: active transport pointer at +0x18
    int NS_PeerHolderActive= 0x18;


    int GD_Width        = 0x40;     // ??  raw framebuffer width
    int GD_Height       = 0x44;     // ??  raw framebuffer height
    int GD_ScaledWidth  = 0x50;     // ??  width * guiScaleFrac
    int GD_ScaledHeight = 0x54;     // ??  height * guiScaleFrac
    int GD_GuiScale     = 0x5C;     // ??  1.26.4 value
    int GD_GuiScaleFrac = 0x60;     // ??  1.26.4 value

    int LVL_PlayerList  = 0x4E8;    // OK  1.26.62 value (unordered_map<UUID,PlayerListEntry>, Latite-dumped)

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


private:
    OffsetManager() = default;
};

inline OffsetManager& Offsets() { return OffsetManager::GetInstance(); }

namespace Layout {

// td remove this probe shit
namespace Probe {
    constexpr size_t ClientInstance = 0x200;
    constexpr size_t Minecraft      = 0xD8;
    constexpr size_t Level          = 0x10;
    constexpr size_t Actor          = 0x258;
    constexpr size_t ActorAabbField = 0x250;
    constexpr size_t HitResult      = 0x4C;
    constexpr size_t ItemStack      = 0x80;
}

    namespace Actor {
        constexpr uintptr_t EntityContext = 0x08;
        constexpr uintptr_t Registry      = 0x10;   // EntityContext::registry
        constexpr uintptr_t TicksExisted  = 0x1C0;
        constexpr uintptr_t Supplies      = 0x5E0;  // Player::mSupplies (PlayerInventory*)
        constexpr uintptr_t LocalName     = 0xBE8;  // std::string
        constexpr uintptr_t SerializedSkinRef = 0xAD8;  // Player+0xAD8 -> SerializedSkinRef* (Latite-dumped)
        // SkinImage inside the skinImpl wrapper (SerializedSkinRef->skinImpl + 0x80).
        constexpr uintptr_t SkinImageInRef    = 0x80;
        constexpr uintptr_t DestroyInput = 0xB4A;
    }

namespace PlayerInv {
    constexpr uintptr_t SelectedSlot = 0x10;
    constexpr uintptr_t InHandSlot   = 0x1C;
    constexpr uintptr_t Container    = 0xB8;
}

// todo
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

namespace Csc {
    constexpr uintptr_t CtxField  = 0x0D48;
    constexpr uintptr_t SlotModel = 0x12C0;  // handleAutoPlace: mov rcx,[rbx+12C0h]
    constexpr size_t    MinSize   = 0x12D8;
    constexpr int       TypeVIndex = 172;
}

namespace ChatScreen {
    constexpr uintptr_t Text = 0xD60;
}

    namespace VTable {
        constexpr int Packet_getId               = 1;
        constexpr int PacketSender_send          = 2;
        constexpr int ClientInstance_getLocalPlayer = 30;
        constexpr int ClientInstance_getGuiData = 216;
        constexpr int ClientInstance_getGameRenderer = 184;
        constexpr int ClientInstance_getRendererHelper = 186;
        constexpr int ClientInstance_getNetworkSystem = 292;
        constexpr int ClientInstance_getSoundEngine = 317;
        constexpr int Level_getBlockSource       = 30;
        constexpr int Level_getRuntimeActorList  = 0x145;
        constexpr int Actor_getNameTag           = 63;
        constexpr int Actor_swing                = 0x6D;
        constexpr int Actor_setSprinting         = 0x8B;
        constexpr int Item_getMaxDamage          = 0x24;
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

namespace GameMode {
    constexpr uintptr_t PlayerBackRef = 0x8;   // GameMode::mPlayer
    constexpr uintptr_t BreakProgress = 0x24;
    constexpr uintptr_t BuildCooldownLastNs = 0x88;

    constexpr uintptr_t UseRepeatNextNs     = 0x90;
    constexpr uintptr_t UseRepeatIntervalMs = 0xA0;


}

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
