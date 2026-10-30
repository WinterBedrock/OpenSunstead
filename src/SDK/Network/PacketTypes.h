#pragma once
#include <string>

namespace SDK {

// Centralised Minecraft Bedrock packet id <-> name table (1.26.30.05).
// Use from any module, e.g. PacketSender::sendToServer(..., (int)TypePacket::Text).
enum class TypePacket : int {
    LoginTokens = 1,
    PlayStatus = 2,
    ServerToClientHandshake = 3,
    ClientToServerHandshake = 4,
    Disconnect = 5,
    ResourcePacksInfo = 6,
    ResourcePackStack = 7,
    ResourcePackClientResponse = 8,
    Text = 9,
    SetTime = 10,
    StartGame = 11,
    AddPlayer = 12,
    AddEntity = 13,
    RemoveEntity = 14,
    AddItemEntity = 15,
    ServerPostMove = 16,
    TakeItemEntity = 17,
    MoveEntity = 18,
    MovePlayer = 19,
    RiderJump = 20,
    UpdateBlock = 21,
    AddPainting = 22,
    TickSync = 23,
    LevelSoundEventOld = 24,
    LevelEvent = 25,
    BlockEvent = 26,
    EntityEvent = 27,
    MobEffect = 28,
    UpdateAttributes = 29,
    InventoryTransaction = 30,
    MobEquipment = 31,
    MobArmorEquipment = 32,
    Interact = 33,
    BlockPickRequest = 34,
    EntityPickRequest = 35,
    PlayerAction = 36,
    HurtArmor = 38,
    SetEntityData = 39,
    SetEntityMotion = 40,
    SetEntityLink = 41,
    SetHealth = 42,
    SetSpawnPosition = 43,
    Animate = 44,
    Respawn = 45,
    ContainerOpen = 46,
    ContainerClose = 47,
    PlayerHotbar = 48,
    InventoryContent = 49,
    InventorySlot = 50,
    ContainerSetData = 51,
    CraftingData = 52,
    CraftingEvent = 53,
    GuiDataPickItem = 54,
    AdventureSettings = 55,
    BlockEntityData = 56,
    PlayerInput = 57,
    LevelChunk = 58,
    SetCommandsEnabled = 59,
    SetDifficulty = 60,
    ChangeDimension = 61,
    SetPlayerGameType = 62,
    PlayerList = 63,
    SimpleEvent = 64,
    Event = 65,
    SpawnExperienceOrb = 66,
    ClientboundMapItemData = 67,
    MapInfoRequest = 68,
    RequestChunkRadius = 69,
    ChunkRadiusUpdate = 70,
    GameRulesChanged = 72,
    Camera = 73,
    BossEvent = 74,
    ShowCredits = 75,
    AvailableCommands = 76,
    CommandRequest = 77,
    CommandBlockUpdate = 78,
    CommandOutput = 79,
    UpdateTrade = 80,
    UpdateEquipment = 81,
    ResourcePackDataInfo = 82,
    ResourcePackChunkData = 83,
    ResourcePackChunkRequest = 84,
    Transfer = 85,
    PlaySound = 86,
    StopSound = 87,
    SetTitle = 88,
    AddBehaviorTree = 89,
    StructureBlockUpdate = 90,
    ShowStoreOffer = 91,
    PurchaseReceipt = 92,
    PlayerSkin = 93,
    SubClientLogin = 94,
    InitiateWebSocketConnection = 95,
    SetLastHurtBy = 96,
    BookEdit = 97,
    NpcRequest = 98,
    PhotoTransfer = 99,
    ModalFormRequest = 100,
    ModalFormResponse = 101,
    ServerSettingsRequest = 102,
    ServerSettingsResponse = 103,
    ShowProfile = 104,
    SetDefaultGameType = 105,
    RemoveObjective = 106,
    SetDisplayObjective = 107,
    SetScore = 108,
    LabTable = 109,
    UpdateBlockSynced = 110,
    MoveEntityDelta = 111,
    SetScoreboardIdentity = 112,
    SetLocalPlayerAsInitialized = 113,
    UpdateSoftEnum = 114,
    NetworkStackLatency = 115,
    ScriptCustomEvent = 117,
    SpawnParticleEffect = 118,
    AvailableEntityIdentifiers = 119,
    LevelSoundEventV2 = 120,
    NetworkChunkPublisherUpdate = 121,
    BiomeDefinitionList = 122,
    LevelSoundEvent = 123,
    LevelEventGeneric = 124,
    LecternUpdate = 125,
    VideoStreamConnect = 126,
    ClientCacheStatus = 129,
    OnScreenTextureAnimation = 130,
    MapCreateLockedCopy = 131,
    StructureTemplateDataExportRequest = 132,
    StructureTemplateDataExportResponse = 133,
    UpdateBlockProperties = 134,
    ClientCacheBlobStatus = 135,
    ClientCacheMissResponse = 136,
    EducationSettings = 137,
    Emote = 138,
    MultiplayerSettings = 139,
    SettingsCommand = 140,
    AnvilDamage = 141,
    CompletedUsingItem = 142,
    NetworkSettings = 143,
    PlayerAuthInput = 144,
    CreativeContent = 145,
    PlayerEnchantOptions = 146,
    ItemStackRequest = 147,
    ItemStackResponse = 148,
    PlayerArmorDamage = 149,
    CodeBuilder = 150,
    UpdatePlayerGameType = 151,
    EmoteList = 152,
    PositionTrackingDbBroadcast = 153,
    PositionTrackingDbRequest = 154,
    DebugInfo = 155,
    PacketViolationWarning = 156,
    MotionPredictionHints = 157,
    AnimateEntity = 158,
    CameraShake = 159,
    PlayerFog = 160,
    CorrectPlayerMovePrediction = 161,
    ItemRegistry = 162,
    FilterTextPacket = 163,
    DebugRenderer = 164,
    SyncEntityProperty = 165,
    AddVolumeEntity = 166,
    RemoveVolumeEntity = 167,
    SimulationType = 168,
    NpcDialogue = 169,
    EduUriResourcePacket = 170,
    CreatePhoto = 171,
    UpdateSubchunkBlocks = 172,
    Subchunk = 173,
    SubchunkRequest = 175,
    ClientStartItemCooldown = 176,
    ScriptMessage = 177,
    CodeBuilderSource = 178,
    TickingAreasLoadStatus = 179,
    DimensionData = 180,
    AgentAction = 181,
    ChangeMobProperty = 182,
    LessonProgress = 183,
    RequestAbility = 184,
    RequestPermissions = 185,
    ToastRequest = 186,
    UpdateAbilities = 187,
    UpdateAdventureSettings = 188,
    DeathInfo = 189,
    EditorNetwork = 190,
    FeatureRegistry = 191,
    ServerStats = 192,
    RequestNetworkSettings = 193,
    GameTestRequest = 194,
    GameTestResults = 195,
    UpdateClientInputLocks = 196,
    ClientCheatAbility = 197,
    CameraPresets = 198,
    UnlockedRecipes = 199,
    CameraInstruction = 300,
    CompressedBiomeDefinitions = 301,
    TrimData = 302,
    OpenSign = 303,
    AgentAnimation = 304,
    RefreshEntitlements = 305,
    ToggleCrafterSlotRequest = 306,
    SetPlayerInventoryOptions = 307,
    SetHudElement = 308,
    AwardAchievement = 309,
    ClientboundCloseForm = 310,
    ServerboundLoadingScreen = 312,
    JigsawStructureData = 313,
    CurrentStructureFeature = 314,
    ServerboundDiagnostics = 315,
    CameraAimAssist = 316,
    ContainerRegistryCleanup = 317,
    MovementEffect = 318,
    SetMovementAuthority = 319,
    CameraAimAssistPresets = 320,
    ClientCameraAimAssist = 321,
    ClientMovementPredictionSync = 322,
    UpdateClientOptions = 323,
    PlayerVideoCapture = 324,
    PlayerUpdateEntityOverrides = 325,
    PlayerLocation = 326,
    ClientboundControlsScheme = 327,
    ServerScriptDebugDrawer = 328,
    ServerboundPackSettingChange = 329,
    ClientboundDataStore = 330,
    GraphicsOverrideParameter = 331,
    ServerboundDataStore = 332,
};

inline std::string getPacketTypeName(int id) {
    switch (static_cast<TypePacket>(id)) {
    case TypePacket::LoginTokens: return "LoginTokens";
    case TypePacket::PlayStatus: return "PlayStatus";
    case TypePacket::ServerToClientHandshake: return "ServerToClientHandshake";
    case TypePacket::ClientToServerHandshake: return "ClientToServerHandshake";
    case TypePacket::Disconnect: return "Disconnect";
    case TypePacket::ResourcePacksInfo: return "ResourcePacksInfo";
    case TypePacket::ResourcePackStack: return "ResourcePackStack";
    case TypePacket::ResourcePackClientResponse: return "ResourcePackClientResponse";
    case TypePacket::Text: return "Text";
    case TypePacket::SetTime: return "SetTime";
    case TypePacket::StartGame: return "StartGame";
    case TypePacket::AddPlayer: return "AddPlayer";
    case TypePacket::AddEntity: return "AddEntity";
    case TypePacket::RemoveEntity: return "RemoveEntity";
    case TypePacket::AddItemEntity: return "AddItemEntity";
    case TypePacket::ServerPostMove: return "ServerPostMove";
    case TypePacket::TakeItemEntity: return "TakeItemEntity";
    case TypePacket::MoveEntity: return "MoveEntity";
    case TypePacket::MovePlayer: return "MovePlayer";
    case TypePacket::RiderJump: return "RiderJump";
    case TypePacket::UpdateBlock: return "UpdateBlock";
    case TypePacket::AddPainting: return "AddPainting";
    case TypePacket::TickSync: return "TickSync";
    case TypePacket::LevelSoundEventOld: return "LevelSoundEventOld";
    case TypePacket::LevelEvent: return "LevelEvent";
    case TypePacket::BlockEvent: return "BlockEvent";
    case TypePacket::EntityEvent: return "EntityEvent";
    case TypePacket::MobEffect: return "MobEffect";
    case TypePacket::UpdateAttributes: return "UpdateAttributes";
    case TypePacket::InventoryTransaction: return "InventoryTransaction";
    case TypePacket::MobEquipment: return "MobEquipment";
    case TypePacket::MobArmorEquipment: return "MobArmorEquipment";
    case TypePacket::Interact: return "Interact";
    case TypePacket::BlockPickRequest: return "BlockPickRequest";
    case TypePacket::EntityPickRequest: return "EntityPickRequest";
    case TypePacket::PlayerAction: return "PlayerAction";
    case TypePacket::HurtArmor: return "HurtArmor";
    case TypePacket::SetEntityData: return "SetEntityData";
    case TypePacket::SetEntityMotion: return "SetEntityMotion";
    case TypePacket::SetEntityLink: return "SetEntityLink";
    case TypePacket::SetHealth: return "SetHealth";
    case TypePacket::SetSpawnPosition: return "SetSpawnPosition";
    case TypePacket::Animate: return "Animate";
    case TypePacket::Respawn: return "Respawn";
    case TypePacket::ContainerOpen: return "ContainerOpen";
    case TypePacket::ContainerClose: return "ContainerClose";
    case TypePacket::PlayerHotbar: return "PlayerHotbar";
    case TypePacket::InventoryContent: return "InventoryContent";
    case TypePacket::InventorySlot: return "InventorySlot";
    case TypePacket::ContainerSetData: return "ContainerSetData";
    case TypePacket::CraftingData: return "CraftingData";
    case TypePacket::CraftingEvent: return "CraftingEvent";
    case TypePacket::GuiDataPickItem: return "GuiDataPickItem";
    case TypePacket::AdventureSettings: return "AdventureSettings";
    case TypePacket::BlockEntityData: return "BlockEntityData";
    case TypePacket::PlayerInput: return "PlayerInput";
    case TypePacket::LevelChunk: return "LevelChunk";
    case TypePacket::SetCommandsEnabled: return "SetCommandsEnabled";
    case TypePacket::SetDifficulty: return "SetDifficulty";
    case TypePacket::ChangeDimension: return "ChangeDimension";
    case TypePacket::SetPlayerGameType: return "SetPlayerGameType";
    case TypePacket::PlayerList: return "PlayerList";
    case TypePacket::SimpleEvent: return "SimpleEvent";
    case TypePacket::Event: return "Event";
    case TypePacket::SpawnExperienceOrb: return "SpawnExperienceOrb";
    case TypePacket::ClientboundMapItemData: return "ClientboundMapItemData";
    case TypePacket::MapInfoRequest: return "MapInfoRequest";
    case TypePacket::RequestChunkRadius: return "RequestChunkRadius";
    case TypePacket::ChunkRadiusUpdate: return "ChunkRadiusUpdate";
    case TypePacket::GameRulesChanged: return "GameRulesChanged";
    case TypePacket::Camera: return "Camera";
    case TypePacket::BossEvent: return "BossEvent";
    case TypePacket::ShowCredits: return "ShowCredits";
    case TypePacket::AvailableCommands: return "AvailableCommands";
    case TypePacket::CommandRequest: return "CommandRequest";
    case TypePacket::CommandBlockUpdate: return "CommandBlockUpdate";
    case TypePacket::CommandOutput: return "CommandOutput";
    case TypePacket::UpdateTrade: return "UpdateTrade";
    case TypePacket::UpdateEquipment: return "UpdateEquipment";
    case TypePacket::ResourcePackDataInfo: return "ResourcePackDataInfo";
    case TypePacket::ResourcePackChunkData: return "ResourcePackChunkData";
    case TypePacket::ResourcePackChunkRequest: return "ResourcePackChunkRequest";
    case TypePacket::Transfer: return "Transfer";
    case TypePacket::PlaySound: return "PlaySound";
    case TypePacket::StopSound: return "StopSound";
    case TypePacket::SetTitle: return "SetTitle";
    case TypePacket::AddBehaviorTree: return "AddBehaviorTree";
    case TypePacket::StructureBlockUpdate: return "StructureBlockUpdate";
    case TypePacket::ShowStoreOffer: return "ShowStoreOffer";
    case TypePacket::PurchaseReceipt: return "PurchaseReceipt";
    case TypePacket::PlayerSkin: return "PlayerSkin";
    case TypePacket::SubClientLogin: return "SubClientLogin";
    case TypePacket::InitiateWebSocketConnection: return "InitiateWebSocketConnection";
    case TypePacket::SetLastHurtBy: return "SetLastHurtBy";
    case TypePacket::BookEdit: return "BookEdit";
    case TypePacket::NpcRequest: return "NpcRequest";
    case TypePacket::PhotoTransfer: return "PhotoTransfer";
    case TypePacket::ModalFormRequest: return "ModalFormRequest";
    case TypePacket::ModalFormResponse: return "ModalFormResponse";
    case TypePacket::ServerSettingsRequest: return "ServerSettingsRequest";
    case TypePacket::ServerSettingsResponse: return "ServerSettingsResponse";
    case TypePacket::ShowProfile: return "ShowProfile";
    case TypePacket::SetDefaultGameType: return "SetDefaultGameType";
    case TypePacket::RemoveObjective: return "RemoveObjective";
    case TypePacket::SetDisplayObjective: return "SetDisplayObjective";
    case TypePacket::SetScore: return "SetScore";
    case TypePacket::LabTable: return "LabTable";
    case TypePacket::UpdateBlockSynced: return "UpdateBlockSynced";
    case TypePacket::MoveEntityDelta: return "MoveEntityDelta";
    case TypePacket::SetScoreboardIdentity: return "SetScoreboardIdentity";
    case TypePacket::SetLocalPlayerAsInitialized: return "SetLocalPlayerAsInitialized";
    case TypePacket::UpdateSoftEnum: return "UpdateSoftEnum";
    case TypePacket::NetworkStackLatency: return "NetworkStackLatency";
    case TypePacket::ScriptCustomEvent: return "ScriptCustomEvent";
    case TypePacket::SpawnParticleEffect: return "SpawnParticleEffect";
    case TypePacket::AvailableEntityIdentifiers: return "AvailableEntityIdentifiers";
    case TypePacket::LevelSoundEventV2: return "LevelSoundEventV2";
    case TypePacket::NetworkChunkPublisherUpdate: return "NetworkChunkPublisherUpdate";
    case TypePacket::BiomeDefinitionList: return "BiomeDefinitionList";
    case TypePacket::LevelSoundEvent: return "LevelSoundEvent";
    case TypePacket::LevelEventGeneric: return "LevelEventGeneric";
    case TypePacket::LecternUpdate: return "LecternUpdate";
    case TypePacket::VideoStreamConnect: return "VideoStreamConnect";
    case TypePacket::ClientCacheStatus: return "ClientCacheStatus";
    case TypePacket::OnScreenTextureAnimation: return "OnScreenTextureAnimation";
    case TypePacket::MapCreateLockedCopy: return "MapCreateLockedCopy";
    case TypePacket::StructureTemplateDataExportRequest: return "StructureTemplateDataExportRequest";
    case TypePacket::StructureTemplateDataExportResponse: return "StructureTemplateDataExportResponse";
    case TypePacket::UpdateBlockProperties: return "UpdateBlockProperties";
    case TypePacket::ClientCacheBlobStatus: return "ClientCacheBlobStatus";
    case TypePacket::ClientCacheMissResponse: return "ClientCacheMissResponse";
    case TypePacket::EducationSettings: return "EducationSettings";
    case TypePacket::Emote: return "Emote";
    case TypePacket::MultiplayerSettings: return "MultiplayerSettings";
    case TypePacket::SettingsCommand: return "SettingsCommand";
    case TypePacket::AnvilDamage: return "AnvilDamage";
    case TypePacket::CompletedUsingItem: return "CompletedUsingItem";
    case TypePacket::NetworkSettings: return "NetworkSettings";
    case TypePacket::PlayerAuthInput: return "PlayerAuthInput";
    case TypePacket::CreativeContent: return "CreativeContent";
    case TypePacket::PlayerEnchantOptions: return "PlayerEnchantOptions";
    case TypePacket::ItemStackRequest: return "ItemStackRequest";
    case TypePacket::ItemStackResponse: return "ItemStackResponse";
    case TypePacket::PlayerArmorDamage: return "PlayerArmorDamage";
    case TypePacket::CodeBuilder: return "CodeBuilder";
    case TypePacket::UpdatePlayerGameType: return "UpdatePlayerGameType";
    case TypePacket::EmoteList: return "EmoteList";
    case TypePacket::PositionTrackingDbBroadcast: return "PositionTrackingDbBroadcast";
    case TypePacket::PositionTrackingDbRequest: return "PositionTrackingDbRequest";
    case TypePacket::DebugInfo: return "DebugInfo";
    case TypePacket::PacketViolationWarning: return "PacketViolationWarning";
    case TypePacket::MotionPredictionHints: return "MotionPredictionHints";
    case TypePacket::AnimateEntity: return "AnimateEntity";
    case TypePacket::CameraShake: return "CameraShake";
    case TypePacket::PlayerFog: return "PlayerFog";
    case TypePacket::CorrectPlayerMovePrediction: return "CorrectPlayerMovePrediction";
    case TypePacket::ItemRegistry: return "ItemRegistry";
    case TypePacket::FilterTextPacket: return "FilterTextPacket";
    case TypePacket::DebugRenderer: return "DebugRenderer";
    case TypePacket::SyncEntityProperty: return "SyncEntityProperty";
    case TypePacket::AddVolumeEntity: return "AddVolumeEntity";
    case TypePacket::RemoveVolumeEntity: return "RemoveVolumeEntity";
    case TypePacket::SimulationType: return "SimulationType";
    case TypePacket::NpcDialogue: return "NpcDialogue";
    case TypePacket::EduUriResourcePacket: return "EduUriResourcePacket";
    case TypePacket::CreatePhoto: return "CreatePhoto";
    case TypePacket::UpdateSubchunkBlocks: return "UpdateSubchunkBlocks";
    case TypePacket::Subchunk: return "Subchunk";
    case TypePacket::SubchunkRequest: return "SubchunkRequest";
    case TypePacket::ClientStartItemCooldown: return "ClientStartItemCooldown";
    case TypePacket::ScriptMessage: return "ScriptMessage";
    case TypePacket::CodeBuilderSource: return "CodeBuilderSource";
    case TypePacket::TickingAreasLoadStatus: return "TickingAreasLoadStatus";
    case TypePacket::DimensionData: return "DimensionData";
    case TypePacket::AgentAction: return "AgentAction";
    case TypePacket::ChangeMobProperty: return "ChangeMobProperty";
    case TypePacket::LessonProgress: return "LessonProgress";
    case TypePacket::RequestAbility: return "RequestAbility";
    case TypePacket::RequestPermissions: return "RequestPermissions";
    case TypePacket::ToastRequest: return "ToastRequest";
    case TypePacket::UpdateAbilities: return "UpdateAbilities";
    case TypePacket::UpdateAdventureSettings: return "UpdateAdventureSettings";
    case TypePacket::DeathInfo: return "DeathInfo";
    case TypePacket::EditorNetwork: return "EditorNetwork";
    case TypePacket::FeatureRegistry: return "FeatureRegistry";
    case TypePacket::ServerStats: return "ServerStats";
    case TypePacket::RequestNetworkSettings: return "RequestNetworkSettings";
    case TypePacket::GameTestRequest: return "GameTestRequest";
    case TypePacket::GameTestResults: return "GameTestResults";
    case TypePacket::UpdateClientInputLocks: return "UpdateClientInputLocks";
    case TypePacket::ClientCheatAbility: return "ClientCheatAbility";
    case TypePacket::CameraPresets: return "CameraPresets";
    case TypePacket::UnlockedRecipes: return "UnlockedRecipes";
    case TypePacket::CameraInstruction: return "CameraInstruction";
    case TypePacket::CompressedBiomeDefinitions: return "CompressedBiomeDefinitions";
    case TypePacket::TrimData: return "TrimData";
    case TypePacket::OpenSign: return "OpenSign";
    case TypePacket::AgentAnimation: return "AgentAnimation";
    case TypePacket::RefreshEntitlements: return "RefreshEntitlements";
    case TypePacket::ToggleCrafterSlotRequest: return "ToggleCrafterSlotRequest";
    case TypePacket::SetPlayerInventoryOptions: return "SetPlayerInventoryOptions";
    case TypePacket::SetHudElement: return "SetHudElement";
    case TypePacket::AwardAchievement: return "AwardAchievement";
    case TypePacket::ClientboundCloseForm: return "ClientboundCloseForm";
    case TypePacket::ServerboundLoadingScreen: return "ServerboundLoadingScreen";
    case TypePacket::JigsawStructureData: return "JigsawStructureData";
    case TypePacket::CurrentStructureFeature: return "CurrentStructureFeature";
    case TypePacket::ServerboundDiagnostics: return "ServerboundDiagnostics";
    case TypePacket::CameraAimAssist: return "CameraAimAssist";
    case TypePacket::ContainerRegistryCleanup: return "ContainerRegistryCleanup";
    case TypePacket::MovementEffect: return "MovementEffect";
    case TypePacket::SetMovementAuthority: return "SetMovementAuthority";
    case TypePacket::CameraAimAssistPresets: return "CameraAimAssistPresets";
    case TypePacket::ClientCameraAimAssist: return "ClientCameraAimAssist";
    case TypePacket::ClientMovementPredictionSync: return "ClientMovementPredictionSync";
    case TypePacket::UpdateClientOptions: return "UpdateClientOptions";
    case TypePacket::PlayerVideoCapture: return "PlayerVideoCapture";
    case TypePacket::PlayerUpdateEntityOverrides: return "PlayerUpdateEntityOverrides";
    case TypePacket::PlayerLocation: return "PlayerLocation";
    case TypePacket::ClientboundControlsScheme: return "ClientboundControlsScheme";
    case TypePacket::ServerScriptDebugDrawer: return "ServerScriptDebugDrawer";
    case TypePacket::ServerboundPackSettingChange: return "ServerboundPackSettingChange";
    case TypePacket::ClientboundDataStore: return "ClientboundDataStore";
    case TypePacket::GraphicsOverrideParameter: return "GraphicsOverrideParameter";
    case TypePacket::ServerboundDataStore: return "ServerboundDataStore";
    default: return "Packet_" + std::to_string(id);
    }
}

inline std::string getPacketTypeName(TypePacket t) { return getPacketTypeName(static_cast<int>(t)); }

} // namespace SDK
