#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstddef>

// How a raw pattern match is turned into a stored result.
enum class SigResolve : uint8_t {
    // Store the match address as-is.
    Direct = 0,
    // RIP-relative: result = match + instrLen + *(int32_t*)(match + relAt)
    // (call E8 / lea / mov [rip+disp] sites).
    Rel32 = 1,
    // Read a signed 32-bit field displacement into an int* target.
    // result int = *(int32_t*)(match + relAt), clamped to (0, 0x2000).
    Offset32 = 2,
    // Two Offset32 reads (e.g. stateVector + aabb from one site).
    // out = first int*, out2 = second int*; relAt / instrLen hold the two byte offs.
    Offset32Pair = 3,
    // Item::mName: scan forward from match for `48 8B 47 ??` and take the disp8.
    ItemNameWalk = 4,
    // Amplifier, duration and noCounter disp8 fields from the effect merge block.
    // Element size resolves independently from Actor::addEffect indexing.
    EffectInstFields = 5,
    // Anchor INSIDE a function, then step back to its entry:
    //   result = match - relAt
    // For functions whose prologue is shared with others (so a prologue pattern
    // would be ambiguous) but which contain a unique interior landmark. The
    // caller supplies the exact byte distance from that landmark to the entry.
    // Verified against a known-good prologue byte before being accepted.
    Backstep = 6,
};

struct SigEntry {
    const char*  name;
    const char*  pattern;
    const char*  altPattern;
    void*        out;
    void*        out2;
    SigResolve   resolve;
    int16_t      relAt;
    int16_t      instrLen;
};

class SigManager {
public:
    static SigManager& GetInstance() {
        static SigManager instance;
        return instance;
    }

    void Initialize();

    uintptr_t Fullbright = 0;
    uintptr_t BlockSource_getBlock = 0;
    uintptr_t Platform_GameCore = 0;
    uintptr_t RenderMaterialGroup = 0;
    uintptr_t LevelRenderer_renderLevel = 0;
    uintptr_t Tessellator_begin = 0;
    uintptr_t Tessellator_vertex = 0;
    uintptr_t Tessellator_color = 0;
    uintptr_t MeshHelpers_renderMeshImmediately = 0;
    uintptr_t Level_getRuntimeActorList = 0;
    uintptr_t entt_assure = 0;
    uintptr_t thirdPersonNametag = 0;
    uintptr_t Unknown_renderNametag = 0;
    uintptr_t Nametag_buildForActor = 0;
    uintptr_t ItemUseSlowdownApply = 0;
    uintptr_t mouseDeviceAddr = 0;
    uintptr_t GameCore_handleMouseInput = 0;
    uintptr_t MouseDevice_feed = 0;
    uintptr_t ConnectionRequest_DeviceOS = 0;
    uintptr_t ConnectionRequest_toJson = 0;
    uintptr_t ConnectionRequest_DefaultInput = 0;
    uintptr_t ConnectionRequest_DeviceOS_Legacy = 0;
    uintptr_t ConnectionRequest_DefaultInput_Legacy = 0;
    uintptr_t ConnectionRequest_CurrentInput = 0;
    uintptr_t GuiData_displayClientMessage = 0;
    uintptr_t ChatScreenController_sendChatMessage = 0;
    uintptr_t I18n_get = 0;
    uintptr_t I18n_repo = 0;
    uintptr_t ClientInstanceScreenModel_sendChatMessage = 0;
    uintptr_t RakNetInstance_ctor = 0;
    uintptr_t CameraAvoidanceSystem_tickCached = 0;
    uintptr_t CameraAvoidanceSystem_tickNonCached = 0;
    uintptr_t CameraAvoidanceSystem_vtableCached = 0;
    uintptr_t CameraAvoidanceSystem_vtableNonCached = 0;
    uintptr_t MinecraftPackets_createPacket = 0;
    uintptr_t ContainerScreenController_handleAutoPlace = 0;
    // Actor::getUniqueID(actor, out16) — the game's own runtime-id reader
    // (sub_1419F2CD0 on 1.26.50.04): walks the entt registry buckets by the
    // RuntimeIDComponent hash and reads the u64. The SDK's own entt storage
    // walk returns garbage on this build family, which is why getRuntimeID()
    // routed through here.
    uintptr_t Actor_getUniqueID = 0;
    uintptr_t ContainerScreenController_getSelectedSlotInfo = 0;
    uintptr_t ContainerScreenController_getItemStack = 0;
    // ItemStackNetManagerScreen::requestDrop — see SigManager.cpp for the calling
    // convention (manager this, {std::string collection; int slot;}*, {mode,amount,flag}*).
    uintptr_t ItemStackNetManager_requestDrop = 0;
    // ContainerScreenController::_tryAutoPlace (sub_144DD2ED0 on 1.26.50.04) —
    // the core shift-click move the game's own slot-press handler calls:
    // (this, const std::string& collection, int slot, void* slotInfo) -> bool.
    // handleAutoPlace only reaches this on the touch interaction model; on the
    // classic mouse model it just selects the slot, which is why moves no-op
    // there. slotInfo comes from _getSlotInfo below.
    uintptr_t ContainerScreenController_tryAutoPlace = 0;
    // ContainerScreenController::_getSlotInfo (sub_144DD3020) —
    // (this, const std::string& collection, int slot) -> void* slot info.
    // Never null: falls back to a static empty-info object the game itself
    // passes when a slot is unknown, so the core call stays safe.
    uintptr_t ContainerScreenController_getSlotInfo = 0;
    uintptr_t ClientInstance_grabCursor = 0;
    uintptr_t ClientInstance_releaseCursor = 0;
    uintptr_t ReachClampCapLoadAnchor12660 = 0;
    uintptr_t ReachClampRegisterAnchor12660 = 0;
    uintptr_t WaterBlockLegacy_getCollisionShape = 0;
    uintptr_t ScreenView_setupAndRender = 0;
    uintptr_t ItemStackBase_getDamageValue = 0;
    uintptr_t Options_getPerspective = 0;
    uintptr_t Dimension_getTimeOfDay = 0;
    uintptr_t Dimension_getSkyColor = 0;
    uintptr_t _updatePlayer = 0;
    uintptr_t Actor_attack = 0;
    uintptr_t GameMode_attack = 0;
    uintptr_t GameMode_useItem = 0;
    uintptr_t GameMode_buildBlock = 0;
    uintptr_t GameMode_continueDestroyBlock = 0;
    uintptr_t GameMode_tick = 0;
    uintptr_t GameMode_getDestroyRate = 0;
    uintptr_t LocalPlayer_applyTurnDelta = 0;
    uintptr_t SneakMovementSystem_tickSneakMovementSystem = 0;
    uintptr_t SneakMovementSystem_edgeClampAlt = 0;
    uintptr_t StarveSprint_foodGate = 0;
    uintptr_t SplashText_convert = 0;
    uintptr_t CameraSetup_writeMatrices = 0;
    uintptr_t FlameRenderer_draw = 0;
    uintptr_t OnFireClientSystem_update = 0;
    uintptr_t ClientFireActorEvent_handleFire = 0;
    uintptr_t MolangQuery_isOnFire = 0;
    uintptr_t FlameBillboard_draw = 0;
    uintptr_t RakPeer_GetAveragePing = 0;
    uintptr_t UI_drawText_helper = 0;
    uintptr_t UIContext_drawImage = 0;
    uintptr_t UIHotbarPathBuilder = 0;
    uintptr_t UIHotbarToString = 0;
    uintptr_t Actor_addEffect = 0;
    uintptr_t Actor_removeEffect = 0;
    uintptr_t I18n_sI18n = 0;
    uintptr_t Item_allowOffhandCheckA = 0;
    uintptr_t Item_allowOffhandCheckB = 0;
    uintptr_t Actor_setImmobile = 0;
    uintptr_t entt_assure_actorDataFlag = 0;
    uintptr_t ItemStackBase_ctorFromBlock = 0;
    uintptr_t ItemStackBase_dtor = 0;
    uintptr_t ItemStack_vtable = 0;
    uintptr_t BaseActorRenderContext_ctor = 0;
    uintptr_t BaseActorRenderContext_dtor = 0;
    uintptr_t ItemRenderer_renderGuiItemNew = 0;
    uintptr_t ItemRenderer_flushGuiItem = 0;
    uintptr_t ItemActor_spinAngleMov = 0;
    uintptr_t ItemActor_getBobHeight = 0;
    uintptr_t Matrix_rotateAxisAngle = 0;
    uintptr_t ItemActor_renderStackedCopies = 0;
    uintptr_t MoveInputComponent_get = 0;
    uintptr_t Entt_emplaceFlag = 0;
    uintptr_t ClientInput_tick = 0;
    uintptr_t TextureAtlas_lookup = 0;
    uintptr_t BlockSource_setBlock = 0;
    uintptr_t LevelChunk_setBlock = 0;
    uintptr_t MesherBuild = 0;
    uintptr_t BlockTessellator_tessellateInWorld = 0;
    uintptr_t BlockRenderLayerResolver = 0;
    uintptr_t ActorRenderDispatcher_render = 0;
    uintptr_t ActorRenderDispatcher_renderB = 0; // slot 48
    uintptr_t ActorRenderDispatcher_renderC = 0; // slot 32
    uintptr_t AttributeKey_Health = 0;
    uintptr_t AttributeKey_Movement = 0;
    uintptr_t TextPacket_buildChat = 0;
    uintptr_t TextColor_getColorFromCode = 0;
    uintptr_t TextProcessor_codeRangeGate = 0;
    uintptr_t LocalPlayer_normalTick = 0;
    uintptr_t Mob_getCurrentSwingDuration = 0;
    uintptr_t ItemInHand_swingAngleLoad = 0;
    uintptr_t ItemInHandRenderer_render = 0;

private:
    SigManager() = default;

    void buildTable(SigEntry* out, size_t* count) const;
    static constexpr size_t kMaxSigs = 128;

    bool applyEntry(const SigEntry& e, uintptr_t match) const;
};
