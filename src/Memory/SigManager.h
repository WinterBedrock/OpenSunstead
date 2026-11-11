#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstddef>


enum class SigResolve : uint8_t {

    Direct = 0,

    Rel32 = 1,

    Offset32 = 2,

    Offset32Pair = 3,

    ItemNameWalk = 4,

    EffectInstFields = 5,
    Backstep = 6,
};

struct SigEntry {
    const char*  name;
    const char*  pattern;

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
    uintptr_t Memory_freeSized = 0;
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
    uintptr_t Actor_getUniqueID = 0;
    uintptr_t ContainerScreenController_getSelectedSlotInfo = 0;
    uintptr_t ContainerScreenController_getItemStack = 0;

    uintptr_t ItemStackNetManager_requestDrop = 0;
    uintptr_t ContainerScreenController_tryAutoPlace = 0;
    uintptr_t ContainerScreenController_getSlotInfo = 0;
    uintptr_t ClientInstance_grabCursor = 0;
    uintptr_t ClientInstance_releaseCursor = 0;
    uintptr_t ReachClampCapLoadAnchor12660 = 0;
    uintptr_t ReachClampRegisterAnchor12660 = 0;

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

    void buildTable(SigEntry* out, size_t* count);
    static constexpr size_t kMaxSigs = 128;

    bool applyEntry(const SigEntry& e, uintptr_t match) const;
};
