#include "SigManager.h"
#include "MemoryScanner.h"
#include "SigObfuscate.h"
#include "OffsetManager.h"
#include "../SDK/SDKRuntime.h"
#include "../Core/Logger.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;


inline SigEntry Direct(const char* name, const char* pat, uintptr_t* out) {
    return { name, pat, out, nullptr, SigResolve::Direct, 0, 0 };
}

inline SigEntry Rel32(const char* name, const char* pat, uintptr_t* out,
                      int relAt, int instrLen) {
    return { name, pat, out, nullptr, SigResolve::Rel32,
             static_cast<int16_t>(relAt), static_cast<int16_t>(instrLen) };
}

inline SigEntry Off32(const char* name, const char* pat, int* out, int readAt) {
    return { name, pat, out, nullptr, SigResolve::Offset32,
             static_cast<int16_t>(readAt), 0 };
}

inline SigEntry Backstep(const char* name, const char* pat, uintptr_t* out, int back) {
    return { name, pat, out, nullptr, SigResolve::Backstep,
             static_cast<int16_t>(back), 0 };
}

inline SigEntry OffPair(const char* name, const char* pat,
                        int* outA, int readA, int* outB, int readB) {
    return { name, pat, outA, outB, SigResolve::Offset32Pair,
             static_cast<int16_t>(readA), static_cast<int16_t>(readB) };
}

inline SigEntry ItemWalk(const char* name, const char* pat, int* out) {
    return { name, pat, out, nullptr, SigResolve::ItemNameWalk, 0, 0 };
}

} // namespace

void SigManager::buildTable(SigEntry* t, size_t* count) {
    auto* self = this;
    auto& off  = OffsetManager::GetInstance();
    size_t n = 0;

    auto push = [&](SigEntry e) {
        if (n < kMaxSigs) t[n++] = e;
    };
    push(Direct(SUNSIG("Level::getRuntimeActorList"),
        SUNSIG("55 41 56 56 57 53 48 83 EC 40 48 8D 6C 24 ? 48 C7 45 ? ? ? ? ? 48 8B B1 ? ? ? ? 0F 57 C0"),
        &self->Level_getRuntimeActorList));
    push(Direct(SUNSIG("Memory::sizedDelete"),
        SUNSIG("55 48 83 EC 30 48 8D 6C 24 ? 48 C7 45 ? ? ? ? ? 48 89 CA 48 8B 0D"),
        &self->Memory_freeSized));

    push(Direct(SUNSIG("Options::getGamma"),
        SUNSIG("48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 ? 48 8B 01 48 8B 40 08 48 8D 54 24 ? 41 B8 32 00 00 00"),
        &self->Fullbright));
    push(Rel32(SUNSIG("Platform_GameCore"),
        SUNSIG("4C 89 3D ? ? ? ? 4D 85 FF"),
        &self->Platform_GameCore, 3, 7));
    push(Rel32(SUNSIG("RenderMaterialGroup::common"),
        SUNSIG("48 8D 15 ? ? ? ? 4C 8D 45 D0 E8 ? ? ? ? 90 48 8D 4D D0 E8 ? ? ? ? 48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8D 0D ? ? ? ? E8 ? ? ? ? E9 ? ? ? ? 48 8D 0D ? ? ? ? E8 ? ? ? ?"),
        &self->RenderMaterialGroup, 3, 7));
    push(Rel32(SUNSIG("LevelRenderer::renderLevel"),
        SUNSIG("E8 ? ? ? ? 31 FF 48 8B 86 A8 01 00 00 48 85 C0 75 ?"),
        &self->LevelRenderer_renderLevel, 1, 5));
    push(Direct(SUNSIG("Tessellator::begin"),
        SUNSIG("56 57 55 53 48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 80 B9 A0 02 00 00 00 0F 85 ? ? ? ? 48 89 CE 80 B9 55 02 00 00 00 0F 85 ? ? ? ? 44 89 CF 44 89 C3 0F B6 AC 24 80 00 00 00"),
        &self->Tessellator_begin));
    push(Direct(SUNSIG("Tessellator::vertex"),
        SUNSIG("41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 78 44 0F 29 4C 24 60 44 0F 29 44 24 50 0F 29 7C 24 40 0F 29 74 24 30 8B 81 98 02 00 00 3B 81 9C 02 00 00 0F 84 ? ? ? ? 0F 28 F3 48 89 CE FF C0 89 81 98 02 00 00 C6 01 01 0F 28 F9 0F 14 FA 80 B9 10 02 00 00 01"),
        &self->Tessellator_vertex));
    push(Direct(SUNSIG("Tessellator::color"),
        SUNSIG("80 B9 54 02 00 00 00 0F 85 ? ? ? ? F3 0F 10 05 ? ? ? ? F3 0F 59 C8 F3 0F 2C C1 45 31 C0 85 C0 41 0F 4E C0 3D FF 00 00 00 41 B9 FF 00 00 00 41 0F 4D C1"),
        &self->Tessellator_color));
    push(Direct(SUNSIG("MeshHelpers::renderMeshImmediately"),
        SUNSIG("55 41 57 41 56 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 80 BA ? ? ? ? ? 0F 85 ? ? ? ? 4C 89 CF"),
        &self->MeshHelpers_renderMeshImmediately));
    push(OffPair(SUNSIG("Actor::mStateVector_mAABB"),
        SUNSIG("48 8B 81 ? ? ? ? 48 8B 89 ? ? ? ? F3 0F 10 00"),
        &off.Actor_mStateVector, 3, &off.Actor_mAABB, 10));
    push(Off32(SUNSIG("Actor::mRotation"),
        SUNSIG("48 8B B6 ? ? ? ? F2 0F 10 4C 24 ? F3 0F 10 3D ? ? ? ? F3 0F 10 44 24 ? F3 0F 58 C7 F2 0F 11 0E"),
        &off.Actor_mRotation, 3));

    push(Rel32(SUNSIG("entt::registry::assure"),
        SUNSIG("BA 19 A4 6B 4F E8 ? ? ? ? 48 89 C7 48 89 D9 BA 97 27 0C 8A E8"),
        &self->entt_assure, 6, 10));

    push(Rel32(SUNSIG("entt::registry::assure<ActorDataFlagComponent>"),
        SUNSIG("BA F3 26 74 C6 E8 ? ? ? ? 89 F9 81 E1 00 F8 03 00 C1 E9 08 48 8B 50 08 48 8B 40 50 48 8B 0C 0A 81 E7 FF 07 00 00"),
        &self->entt_assure_actorDataFlag, 6, 10));
    push(Direct(SUNSIG("ThirdPersonNametag"),
        SUNSIG("74 ? 48 8B 03 48 8B 80 ? ? ? ? 48 89 D9 FF 15 ? ? ? ? 90 84 C0 75 ? 48 8B 4B"),
        &self->thirdPersonNametag));
    push(Rel32(SUNSIG("mouseDevice"),
        SUNSIG("48 8D 0D ? ? ? ? B2 01 41 B0 01 E8 ? ? ? ? 90 E9 ? ? ? ? 66"),
        &self->mouseDeviceAddr, 3, 7));
    push(Direct(SUNSIG("MouseDevice::feed"),
        SUNSIG("41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 48 44 89 CF 44 89 C3 89 D5 48 89 CE 44 0F B7 A4 24 ? ? ? ?"),
        &self->MouseDevice_feed));
    push(Direct(SUNSIG("GameCore::handleMouseInput"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC E8 02 00 00 48 8D AC 24 ? ? ? ? 44 0F 29 AD ? ? ? ? 44 0F 29 A5 ? ? ? ? 44 0F 29 9D ? ? ? ? 44 0F 29 95 ? ? ? ? 44 0F 29 8D ? ? ? ? 44 0F 29 85 ? ? ? ? 0F 29 BD ? ? ? ? 0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? FE FF FF FF 48 89 CE 8B 05 ? ? ? ?"),
        &self->GameCore_handleMouseInput));
    push(Direct(SUNSIG("ConnectionRequest::toJson"),
        SUNSIG("55 41 56 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? FE FF FF FF 4C 89 CB 4C 89 C7 48 89 95 ? ? ? ? 48 89 CE 48 8D 4D 60 4C 89 C2"),
        &self->ConnectionRequest_toJson));
    push(Direct(SUNSIG("ConnectionRequest::DeviceOS_vcall"),
        SUNSIG("BA FF FF FF FF 48 85 C0 74 ? 80 38 01 75 ? 48 8B 85 ? ? ? ? 48 8B 48 10 48 8B 01 48 8B 80 98 00 00 00 FF 15 ? ? ? ? 90 89 C2"),
        &self->ConnectionRequest_DeviceOS));
    push(Direct(SUNSIG("ConnectionRequest::DefaultInputMode_vcall"),
        SUNSIG("31 D2 48 85 C0 74 ? 80 38 01 75 ? 48 8B 85 ? ? ? ? 48 8B 48 10 48 8B 01 48 8B 80 70 08 00 00 FF 15 ? ? ? ? 90 89 C2 48 8D 8D"),
        &self->ConnectionRequest_DefaultInput));
    push(Direct(SUNSIG("ConnectionRequest::DeviceOS_vcall.legacy"),
        SUNSIG("48 8B 48 10 48 8B 01 48 8B 80 98 00 00 00 FF 15 ? ? ? ? 90 48 8D 4D 40 89 C2"),
        &self->ConnectionRequest_DeviceOS_Legacy));
    push(Direct(SUNSIG("ConnectionRequest::DefaultInputMode_vcall.legacy"),
        SUNSIG("48 8B 48 10 48 8B 01 48 8B 80 70 08 00 00 FF 15 ? ? ? ? 90 89 C2 48 8D 4D"),
        &self->ConnectionRequest_DefaultInput_Legacy));
    push(Direct(SUNSIG("GuiData::displayClientMessage"),
        SUNSIG("55 56 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 44 88 CB"),
        &self->GuiData_displayClientMessage));
    push(Rel32(SUNSIG("ClientInstanceScreenModel::sendChatMessage"),
        SUNSIG("90 EB ? E8 ? ? ? ? 3C 01 75 ? 48 8B 8E 58 0D 00 00 48 C7 45 ? 00 00 00 00 48 8B 01 48 8B 40 20"),
        &self->ClientInstanceScreenModel_sendChatMessage, 4, 8));
    push(Direct(SUNSIG("CameraAvoidanceSystem::tickCached"),
        SUNSIG("41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 68 01 00 00 48 89 CE 80 B9 A0 01 00 00 00"),
        &self->CameraAvoidanceSystem_tickCached));
    push(Direct(SUNSIG("CameraAvoidanceSystem::tickNonCached"),
        SUNSIG("41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 28 01 00 00 48 89 CE 80 B9 78 01 00 00 00"),
        &self->CameraAvoidanceSystem_tickNonCached));
    push(Rel32(SUNSIG("CameraAvoidanceSystem::vtableCached"),
        SUNSIG("48 8D 05 ? ? ? ? 48 89 07 48 89 3B 48 8D 05 ? ? ? ? 48 89 45 ? 48 C7 45 ? 15 00 00 00 48 8D 55 ? 48 89 F1 41 B0 02 E8 ? ? ? ? 90 EB"),
        &self->CameraAvoidanceSystem_vtableCached, 3, 7));
    push(Rel32(SUNSIG("CameraAvoidanceSystem::vtableNonCached"),
        SUNSIG("48 8D 05 ? ? ? ? 48 89 07 48 89 3B 48 8D 05 ? ? ? ? 48 89 45 ? 48 C7 45 ? 15 00 00 00 48 8D 55 ? 48 89 F1 41 B0 02 E8 ? ? ? ? 90 48 8B 45"),
        &self->CameraAvoidanceSystem_vtableNonCached, 3, 7));
    push(Direct(SUNSIG("ChatScreenController::sendChatMessage"),
        SUNSIG("55 41 57 41 56 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 0F 29 75 ? 48 C7 45 ? FE FF FF FF 48 89 CE C6 81 ? ? ? ? 01"),
        &self->ChatScreenController_sendChatMessage));
    push(Direct(SUNSIG("MinecraftPackets::createPacket"),
        SUNSIG("56 48 83 EC ? 48 89 CE 81 FA"),
        &self->MinecraftPackets_createPacket));
    push(Direct(SUNSIG("ContainerScreenController::handleAutoPlace"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 58 01 00 00 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? FE FF FF FF 44 89 CE 4C 89 C7 41 89 D6 48 89 CB 48 8B 01 48 8B 80 38 01 00 00 FF 15 ? ? ? ? 84 C0"),
        &self->ContainerScreenController_handleAutoPlace));
    push(Direct(SUNSIG("Actor::getUniqueID"),
        SUNSIG("48 83 EC 28 4C 8B 41 ? 8B 41 ? 49 8B 48 ? 4D 8B 48 ? 49 29 C9 49 C1 E9 03 41 FF C9 41 81 E1 B5 BB 0D FC 4E 8D 14 C9 49 8B 48 ? 0F 1F 40 ? 4D 8B 0A 49 83 F9 FF 0F 84 ? ? ? ? 49 C1 E1 05 4E 8D 14 09 42 81 7C 09 ? ? ? ? ? 75 ? 4C 01 C9 49 39 48 ? 0F 84"),
        &self->Actor_getUniqueID));
    push(Direct(SUNSIG("ContainerScreenController::_getSelectedSlotInfo"),
        SUNSIG("41 56 56 57 55 53 48 83 EC 40 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 ? 44 8B 81"),
        &self->ContainerScreenController_getSelectedSlotInfo));
    push(Rel32(SUNSIG("ContainerScreenController::getItemStack"),
        SUNSIG("44 8B 85 ? ? ? ? 48 89 D9 4C 89 F2 E8 ? ? ? ? 90 48 89 D9 48 89 FA 41 89 F0 49 89 C1 E8 ? ? ? ? 90 84 C0"),
        &self->ContainerScreenController_getItemStack, 14, 18));
    push(Direct(SUNSIG("ItemStackNetManagerScreen::requestDrop"),
        SUNSIG("48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 49 8B 00 48 89 44 24 20 41 8B 40 08 89 44 24 28 4C 8D 44 24 20 41 B9 01 00 00 00 E8 ? ? ? ? 48 8B 4C 24 30 48 31 E1"),
        &self->ItemStackNetManager_requestDrop));
    push(Direct(SUNSIG("ContainerScreenController::_tryAutoPlace"),
        SUNSIG("55 41 56 56 57 53 48 83 EC 70 48 8D 6C 24 70 48 C7 45 F8 FE FF FF FF 4C 89 CE 48 89 CF 48 8D 5D D0 48 89 D9 E8 ? ? ? ? 48 8B 07 48 8B 80 D0 01 00 00"),
        &self->ContainerScreenController_tryAutoPlace));
    push(Rel32(SUNSIG("ContainerScreenController::_getSlotInfo"),
        SUNSIG("44 8B 85 ? ? ? ? 48 89 D9 4C 89 F2 E8 ? ? ? ? 90 48 89 D9 48 89 FA 41 89 F0 49 89 C1 E8 ? ? ? ? 90 84 C0"),
        &self->ContainerScreenController_getSlotInfo, 14, 18));

    push(Direct(SUNSIG("Actor::setImmobile"),
        SUNSIG("56 53 48 83 EC 58 89 D3 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 ? 48 8B 41 ? 8B 71 ? 48 89 C1 BA CB 40 4E AB"),
        &self->Actor_setImmobile));
    push(Direct(SUNSIG("ItemStackBase::ItemStackBase(Block const&,int,CompoundTag const*)"),
        SUNSIG("55 56 48 83 EC ? 48 8D 6C 24 ? 48 C7 45 ? ? ? ? ? 4C 89 CE 48 8D 05"),
        &self->ItemStackBase_ctorFromBlock));
    push(Direct(SUNSIG("ItemStackBase::~ItemStackBase"),
        SUNSIG("56 57 48 83 EC 28 48 89 CE 48 8D 05 ? ? ? ? 48 89 01 48 8B 49 78 48 85 C9 74 11 48 8B 01 48 8B 00 BA 01 00 00 00 FF 15"),
        &self->ItemStackBase_dtor));
    push(Rel32(SUNSIG("ItemStack vtable"),
        SUNSIG("48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? C7 05 ? ? ? ? 00 00 00 00 C6 05 ? ? ? ? 00 48 8D 0D ? ? ? ? E8 ? ? ? ?"),
        &self->ItemStack_vtable, 15, 19));
    push(Direct(SUNSIG("BaseActorRenderContext::BaseActorRenderContext"),
        SUNSIG("55 41 57 41 56 56 57 53 48 83 EC 48 48 8D 6C 24 40 48 C7 45 00 FE FF FF FF 4C 89 C6 48 89 D7 49 89 CF 48 8D 05 ? ? ? ? 48 89 01 0F 57 C0 0F 11 41 08 4C 89 41 18 4C 89 49 20 48 89 51 28"),
        &self->BaseActorRenderContext_ctor));
    push(Direct(SUNSIG("BaseActorRenderContext::~BaseActorRenderContext"),
        SUNSIG("56 57 53 48 83 EC 20 48 89 CE 48 8D 05 ? ? ? ? 48 89 01 48 8B 89"),
        &self->BaseActorRenderContext_dtor));
    push(Rel32(SUNSIG("ItemRenderer::renderGuiItemNew"),
        SUNSIG("E8 ? ? ? ? 90 48 8D 55 E0 4C 8D 85 A7 00 00 00 48 89 F1 E8 ? ? ? ? 90 80 BF BC 00 00 00 01"),
        &self->ItemRenderer_renderGuiItemNew, 1, 5));
    push(Rel32(SUNSIG("AttributeKey::Health"),
        SUNSIG("0F 28 F1 48 89 CE 4C 8D 05 ? ? ? ? 48 8D 55 B8 E8"),
        &self->AttributeKey_Health, 9, 13));
    push(Rel32(SUNSIG("AttributeKey::Movement"),
        SUNSIG("48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 4C 8D 05 ? ? ? ? 48 8D 54 24 28 E8"),
        &self->AttributeKey_Movement, 18, 22));
    push(Rel32(SUNSIG("TextPacket::buildChat"),
        SUNSIG("48 8D 4D E0 48 8D 95 70 01 00 00 4C 8D 4D B8 49 89 F8 E8 ? ? ? ?"),
        &self->TextPacket_buildChat, 19, 23));
    push(Direct(SUNSIG("ItemRenderer::flushGuiItem"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 48 8B 42 28 48 8B B0 ? ? 00 00 80 BE ? ? 00 00 01"),
        &self->ItemRenderer_flushGuiItem));
    push(Direct(SUNSIG("ClientInstance::grabCursor"),
        SUNSIG("56 48 83 EC 20 48 89 CE 48 8B 01 48 8B 80 00 05 00 00 FF 15 ? ? ? ? 84 C0 74 ? 48 8B 8E C8 01 00 00 48 8B 01 48 8B 80 38 04 00 00"),
        &self->ClientInstance_grabCursor));
    push(Direct(SUNSIG("ClientInstance::releaseCursor"),
        SUNSIG("56 48 83 EC 20 48 89 CE 48 8B 01 48 8B 80 00 05 00 00 FF 15 ? ? ? ? 84 C0 74 ? 48 8B 8E C8 01 00 00 48 8B 01 48 8B 80 40 04 00 00"),
        &self->ClientInstance_releaseCursor));
    push(Direct(SUNSIG("ReachClamp::CapLoad12660"),
        SUNSIG("84 C0 75 ? F3 0F 10 3D ? ? ? ? EB ? F3 0F 10 3D ? ? ? ? 48 8B 03 48 8B 80 78 0A 00 00 48 89 D9 FF 15 ? ? ? ? 90 8B 40 18 83 C0 FC"),
        &self->ReachClampCapLoadAnchor12660));
    push(Direct(SUNSIG("ReachClamp::Register12660"),
        SUNSIG("84 C0 74 ? F3 44 0F 5D C7 EB ? 44 0F 2E C7 76 ? 40 B6 01 44 0F 28 C7 EB ? 31 F6 48 8B 8D ? ? ? ?"),
        &self->ReachClampRegisterAnchor12660));

    push(Rel32(SUNSIG("ScreenView::setupAndRender"),
        SUNSIG("E8 ? ? ? ? 90 48 8B 4B 20 48 85 C9 74 ? 48 8B 01 48 8B 40 10 48 89 FA FF 15 ? ? ? ? 90 48 8D 4D ?"),
        &self->ScreenView_setupAndRender, 1, 5));
    push(Backstep(SUNSIG("TextColor::getColorFromCode"),
        SUNSIG("48 8D 05 ? ? ? ? 48 89 85 40 04 00 00 48 8D 4D A8 48 89 8D 28 05 00 00 C6 85 37 05 00 00 00"),
        &self->TextColor_getColorFromCode, 0x1FB));
    // Letter-range gate in the text processor: `lea edx,[rax-61h]; cmp dl,16h`
    push(Direct(SUNSIG("TextProcessor::codeRangeGate"),
        SUNSIG("8D 50 9F 80 FA 16 0F 87 ? ? ? ? 83 C1 95 83 F9 07"),
        &self->TextProcessor_codeRangeGate));
    push(Direct(SUNSIG("ItemStackBase::getDamageValue"),
        SUNSIG("56 57 48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 48 8B 41 08 48 85 C0 74 ? 48 83 38 00 74 ? 48 8B 71 10 48 85 F6 74 ? 48 8D 3D ? ? ? ? 48 89 7C 24 20 48 C7 44 24 28 06 00 00 00"),
        &self->ItemStackBase_getDamageValue));
    push(Direct(SUNSIG("ItemActor::spinAngle"),
        SUNSIG("F3 0F 59 3D ? ? ? ? 48 8D 4D ? 4C 8D 4D ? 4C 89 FA 0F 28 D7 E8"),
        &self->ItemActor_spinAngleMov));
    push(Direct(SUNSIG("Matrix::rotateAxisAngle"),
        SUNSIG("56 57 53 48 81 EC 90 00 00 00 44 0F 29 A4 24 80 00 00 00 44 0F 29 5C 24 70 44 0F 29 54 24 60 44 0F 29 4C 24 50 44 0F 29 44 24 40 0F 29 7C 24 30 0F 29 74 24 20 4C 89 CB 0F 28 FA 48 89 D7 48 89 CE 0F 28 C2 FF 15 ? ? ? ? 0F 28 F0"),
        &self->Matrix_rotateAxisAngle));
    push(Direct(SUNSIG("ItemActor::renderStackedCopies"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC B8 00 00 00 48 8D AC 24 80 00 00 00 0F 29 7D 20 0F 29 75 10 48 C7 45 08 FE FF FF FF 4C 89 C3 48 89 D6 48 89 CF F3 0F 10 B5 A8 00 00 00 F3 0F 10 3D ? ? ? ? F3 0F 5E BD A0 00 00 00"),
        &self->ItemActor_renderStackedCopies));
    push(Direct(SUNSIG("MoveInputComponent::getOrEmplace"),
        SUNSIG("56 48 83 EC 30 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 ? 48 8B 41 ? "
               "8B 71 ? 48 89 C1 BA 87 18 8B 01"),
        &self->MoveInputComponent_get));
    push(Direct(SUNSIG("ClientInputUpdateSystem::tick"),
        SUNSIG("41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC ? ? ? ? 4C 8B 94 24 ? ? ? ? 48 8B 84 24 ? ? ? ? 48 8B 18 8B 40 08"),
        &self->ClientInput_tick));
    push(Direct(SUNSIG("TextureAtlas::lookupEntry"),
        SUNSIG("41 57 41 56 41 55 41 54 56 57 53 48 83 EC ? 48 89 D6 48 8B 7A ? 48 83 7A ? 10"),
        &self->TextureAtlas_lookup));
    push(Direct(SUNSIG("BlockSource::setBlock"),
        SUNSIG("41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 31 E0 48 89 84 24 ? ? ? ? 49 8B 40 68 80 B8 64 01 00 00 00 78"),
        &self->BlockSource_setBlock));
    push(Direct(SUNSIG("LevelChunk::setBlock"),
        SUNSIG("41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 4C 89 CE 4C 89 C7 48 89 D3 49 89 CE 48 8B 05 ? ? ? ? 48 31 E0 48 89 84 24 ? ? ? ? 0F BF 42 02"),
        &self->LevelChunk_setBlock));
    push(Direct(SUNSIG("MesherBuild"),
        SUNSIG("55 41 57 41 56 41 54 56 57 53 48 83 EC 40 48 8D 6C 24 ? 48 C7 45 ? ? ? ? ? 44 89 CB 48 89 D6"),
        &self->MesherBuild));
    push(Direct(SUNSIG("BlockTessellator::tessellateInWorld"),
        SUNSIG("41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC ? ? ? ? 0F 29 B4 24 ? ? ? ? 4C 89 CF 4D 89 C6 48 89 D3 48 89 CE 0F B6 41 78"),
        &self->BlockTessellator_tessellateInWorld));
    push(Direct(SUNSIG("BlockRenderLayerResolver"),
        SUNSIG("56 48 83 EC 20 48 89 CE E8 ?? ?? ?? ?? 48 8B 4E ?? 48 85 C0"),
        &self->BlockRenderLayerResolver));
    push(Direct(SUNSIG("Entt::emplaceFlag"),
        SUNSIG("41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC ? 44 88 4C 24 ? 4C 89 C3 48 89 D6 48 89 CF"),
        &self->Entt_emplaceFlag));
    push(Direct(SUNSIG("ActorRenderDispatcher::render"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC E8 00 00 00 48 8D AC 24 80 00 00 00 0F 29 7D 50 0F 29 75 40 48 C7 45 38 FE FF FF FF 4C 89 C6 48 89 D7 48 89 CB 4C 89 C1 E8 ?? ?? ?? ??"),
        &self->ActorRenderDispatcher_render));
    push(Direct(SUNSIG("ActorRenderDispatcher::renderB (slot48)"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 01 00 00 48 8D AC 24 ? ? ? ? 44 0F 29 A5 ? ? ? ? 44 0F 29 5D ? 44 0F 29 55 ? 44 0F 29 4D ? 44 0F 29 45 ? 0F 29 7D ? 0F 29 75 ? 48 C7 45 ? ? ? ? ? 4D 89 CE"),
        &self->ActorRenderDispatcher_renderB));
    push(Direct(SUNSIG("ActorRenderDispatcher::renderC (slot32)"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 01 00 00 48 8D AC 24 ? ? ? ? 44 0F 29 A5 ? ? ? ? 44 0F 29 5D ? 44 0F 29 55 ? 44 0F 29 4D ? 44 0F 29 45 ? 0F 29 7D ? 0F 29 75 ? 48 C7 45 ? ? ? ? ? 41 80 B8 89 02 00 00 00 0F 85 ? ? ? ? 4C 89 C7"),
        &self->ActorRenderDispatcher_renderC));
    push(Direct(SUNSIG("Options::getPerspective"),
        SUNSIG("48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 ? 48 8B 01 48 8B 40 08 48 8D 54 24 ? 41 B8 03 00 00 00"),
        &self->Options_getPerspective));
    push(Direct(SUNSIG("Dimension::getTimeOfDay"),
        SUNSIG("48 63 C2 48 69 C8 ? ? ? ? 48 89 CA 48 C1 EA ? 48 C1 F9"),
        &self->Dimension_getTimeOfDay));
    push(Direct(SUNSIG("Dimension::getSkyColor"),
        SUNSIG("48 89 D0 41 0F 10 00 0F 11 02 F3 0F 10 05 ? ? ? ?"),
        &self->Dimension_getSkyColor));
    push(Direct(SUNSIG("UpdatePlayerFromCameraSystemUtil::_updatePlayer"),
        SUNSIG("41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC ? ? ? ? 44 0F 29 94 24 ? ? ? ? 44 0F 29 8C 24 ? ? ? ? 44 0F 29 84 24 ? ? ? ? 0F 29 BC 24 ? ? ? ? 0F 29 B4 24 ? ? ? ? 4C 89 C6"),
        &self->_updatePlayer));
    push(Direct(SUNSIG("Actor::attack"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? FE FF FF FF 4D 89 CD 4D 89 C6 48 89 D6 48 89 CF 41 8B 80"),
        &self->Actor_attack));
    push(Direct(SUNSIG("GameMode::attack"),
        SUNSIG("56 48 83 EC 30 48 89 D6 48 8D 05 ? ? ? ? 48 89 44 24 20 E8 ? ? ? ? 48 89 F0 48 83 C4 30 5E C3"),
        &self->GameMode_attack));
    push(Direct(SUNSIG("GameMode::useItem"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 45 ? FE FF FF FF 45 89 CF 4D 89 C6 48 89 D7 49 89 CC"),
        &self->GameMode_useItem));
    push(Direct(SUNSIG("GameMode::buildBlock"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 01 00 00 48 8D AC 24 ? ? ? ? 44 0F 29 85 ? ? ? ? 0F 29 BD ? ? ? ? 0F 29 75 ? 48 C7 45 ? ? ? ? ? 45 89 CE"),
        &self->GameMode_buildBlock));
    push(Direct(SUNSIG("GameMode::continueDestroyBlock"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 0F 29 7D ? 0F 29 75 ? 48 C7 45 ? FE FF FF FF 4D 89 CF 44 88"),
        &self->GameMode_continueDestroyBlock));
    push(Direct(SUNSIG("GameMode::getDestroyRate"),
        SUNSIG("56 57 48 83 EC 38 0F 29 74 24 ? 48 89 CE 48 8B 49 08 E8 ? ? ? ? 0F 28 F0 48 8B 4E 08 BA 03 00 00 00"),
        &self->GameMode_getDestroyRate));
    push(Direct(SUNSIG("GameMode::tick"),
        SUNSIG("56 48 83 EC ? 48 89 CE 48 8B 89 ? ? ? ? 48 8B 01 48 8B 40 ? FF 15"),
        &self->GameMode_tick));
    push(Direct(SUNSIG("LocalPlayer::applyTurnDelta"),
        SUNSIG("55 41 57 41 56 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 44 0F 29 55 ? 44 0F 29 4D ? 44 0F 29 45 ? 0F 29 7D ? 0F 29 75 ? 48 C7 45 ? FE FF FF FF 49 89 D4 48 89 CE 48 8B 89"),
        &self->LocalPlayer_applyTurnDelta));
    push(Direct(SUNSIG("SafeWalk::edgeClampPrimary"),
        SUNSIG("83 E1 01 88 48 ? 75 ? E9 ? ? ? ?"),
        &self->SneakMovementSystem_tickSneakMovementSystem));
    push(Direct(SUNSIG("SafeWalk::edgeClampAlt"),
        SUNSIG("44 88 50 ? 48 85 C9 0F 84 ? ? ? ?"),
        &self->SneakMovementSystem_edgeClampAlt));
    push(Direct(SUNSIG("StarveSprint::foodGate"),
        SUNSIG("FF C1 F3 0F 10 05 ? ? ? ? 41 0F 2E 40 ? B8 01 00 00 00 0F 43 C1"),
        &self->StarveSprint_foodGate));
    push(Rel32(SUNSIG("BlockSource::getBlock"),
        SUNSIG("48 8D 54 24 30 48 89 CB 48 89 F9 4D 89 C6 E8 ? ? ? ? 48 89 06"),
        &self->BlockSource_getBlock, 15, 19));
    push(Direct(SUNSIG("NameTag_buildForActor"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 44 0F 29 95 ? ? ? ? 44 0F 29 8D ? ? ? ? 44 0F 29 85 ? ? ? ? 0F 29 BD ? ? ? ? 0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? FE FF FF FF 4C 89 8D ? ? ? ? 4D 89 C7 49 89 D6 48 89 CB 48 8B BD ? ? ? ?"),
        &self->Nametag_buildForActor));
    push(Backstep(SUNSIG("NameTag_buildForActor(backstep)"),
        SUNSIG("48 8B BD 20 02 00 00 48 8B 07 48 8B 80 28 02 00 00 48 89 F9 FF 15 ? ? ? ? 48 8B 10 B9 28 02 00 00"),
        &self->Nametag_buildForActor, 0x5C));
    push(Direct(SUNSIG("Unknown::renderNametag"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 58 02 00 00 48 8D AC 24 ? ? ? ? 0F 29 BD ? ? ? ? 0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 48 89 CE"),
        &self->Unknown_renderNametag));
    push(Direct(SUNSIG("ItemUseSlowdownApply"),
        SUNSIG("F3 0F 10 02 F3 0F 59 C0 F2 41 0F 10 48 ? 0F 14 C0 0F 59 C1 41 0F 13 40 ? 41 C7 01 00 00 00 00 C3"),
        &self->ItemUseSlowdownApply));
    push(ItemWalk(SUNSIG("Item::buildDescriptionName (mName reader)"),
        SUNSIG("4C 8B AF ? ? ? ? 48 B8 F4 FF FF FF FF FF FF 7F"),
        &off.Item_mNameStrOffset));
    push(Rel32(SUNSIG("SplashText_convert"),
        SUNSIG("41 B8 14 00 00 00 4C 89 F1 E8"),
        &self->SplashText_convert, 10, 14));
    push(Direct(SUNSIG("CameraSetup_writeMatrices"),
        SUNSIG("56 57 48 83 EC 68 44 0F 29 4C 24 50 44 0F 29 44 24 40 0F 29 7C 24 30 0F 29 74 24 20 48 89 D7 48 89 CE 48 8B 81 A8 01 00 00 48 85 C0 0F 84 ? ? ? ? 80 38 00 75 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 8E B8 01 00 00 48 8B 01 48 8B 80 C0 05 00 00 FF 15 ? ? ? ?"),
        &self->CameraSetup_writeMatrices));
    push(Direct(SUNSIG("FlameRenderer_draw"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 44 0F 29 AD ? ? ? ? 44 0F 29 A5 ? ? ? ? 44 0F 29 9D ? ? ? ? 44 0F 29 95 ? ? ? ? 44 0F 29 8D ? ? ? ? 44 0F 29 85 ? ? ? ? 0F 29 BD ? ? ? ? 0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? FE FF FF FF 48 8B 3A 48 85 FF"),
        &self->FlameRenderer_draw));
    push(Direct(SUNSIG("OnFireClientSystem_update"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 45 ? FE FF FF FF 48 89 D3 48 89 55 ? 0F 57 C0 0F 11 45 ? 0F 11 45 ? 8B 05 ? ? ? ? 8B 0D ? ? ? ? 65 48 8B 14 25 58 00 00 00 48 8B 0C CA 3B 81 04 00 00 00 0F 8F ? ? ? ? 48 83 C3 30 48 89 D9 BA 00 38 B9 85 E8 ? ? ? ? 48 89 C7 48 89 D9 BA F3 3F 61 7E"),
        &self->OnFireClientSystem_update));
    push(Direct(SUNSIG("ClientFireActorEvent_handleFire"),
        SUNSIG("41 57 41 56 56 57 53 48 83 EC ? 48 8B 1A 80 BB ? ? ? ? 00 0F 85 ? ? ? ? 4C 89 CE 48 89 CF 4D 8B 30 4D 8B 78 08"),
        &self->ClientFireActorEvent_handleFire));
    push(Direct(SUNSIG("MolangQuery_isOnFire"),
        SUNSIG("48 83 EC 28 48 8B 4A 38 48 85 C9 74 ?? 48 8B 01 48 8B 80 78 01 00 00 FF 15"),
        &self->MolangQuery_isOnFire));
    push(Direct(SUNSIG("FlameBillboard_draw"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 80 00 00 00 48 C7 85 ? ? ? ? FE FF FF FF 4C 89 C3 48 89 D6 48 89 CF 44 8B 62 70"),
        &self->FlameBillboard_draw));
    push(Direct(SUNSIG("RakPeer_GetAveragePing"),
        SUNSIG("48 81 EC B8 00 00 00 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 84 24 B0 00 00 00 4C 8B 02 4C 3B 05 ?? ?? ?? ?? 0F 85 DE 00 00 00 0F B7 42 10 F2 0F 10 82 90 00 00 00 66 89 44 24 28"),
        &self->RakPeer_GetAveragePing));
    push(Direct(SUNSIG("UI_drawText_helper"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 48 01 00 00 48 8D AC 24 80 00 00 00 44 0F 29 8D ? ? ? ? 44 0F 29 85 ? ? ? ? 0F 29 BD ? ? ? ? 0F 29 B5 ? ? ? ? 48 C7 45 ? FE FF FF FF 0F 28 F3 4C 89 C3 48 89 D6 48 89 CF"),
        &self->UI_drawText_helper));
    push(Direct(SUNSIG("Actor::addEffect"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? FE FF FF FF 48 89 D7 48 89 CE 8B 12"),
        &self->Actor_addEffect));
    push(Direct(SUNSIG("Actor::removeEffect"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC ? 48 8D 6C 24 ? 48 C7 45 ? FE FF FF FF 4C 8B 41 10 8B 41 18 4D 8B 48 48 4D 8B 50 50 4D 29 CA 49 C1 EA 03 41 FF CA 41 81 E2 50 B5 A1 E6"),
        &self->Actor_removeEffect));
    push(Rel32(SUNSIG("I18n::sI18n"),
        SUNSIG("48 8D 0D ? ? ? ? 48 8D 55 E0 4C 8D 45 B8 49 89 F9 E8 ? ? ? ? 90 48 8D 55 E0"),
        &self->I18n_sI18n, 3, 7));
    push(Direct(SUNSIG("I18n::get"),
        SUNSIG("55 56 57 53 48 81 EC 98 00 00 00 48 8D AC 24 ? ? ? ? 48 C7 45 ? ? ? ? ? 4C 89 CB 48 89 D6 0F 57 C0 0F 29 45 ? 48 8B 01 48 8B 80 ? ? ? ? 48 8D 7D ? 4C 8D 4D ? 48 89 FA FF 15 ? ? ? ? 48 8B 43"),
        &self->I18n_get));
    push(Rel32(SUNSIG("I18n::repo_lea"),
        SUNSIG("48 8D 0D ? ? ? ? 48 8D 55 E0 4C 8D 45 B8 49 89 F9 E8 ? ? ? ? 90 48 8D 55 E0"),
        &self->I18n_repo, 3, 7));
    push(Direct(SUNSIG("Item::allowOffhandCheckA"),
        SUNSIG("56 48 83 EC ? 4C 89 C6 49 8B 00 48 8B 40 28 4C 89 C1 FF 15 ? ? ? ? 89 C1 B0 01 84 C9 74 ? 48 8B 46 08"),
        &self->Item_allowOffhandCheckA));
    push(Direct(SUNSIG("Item::allowOffhandCheckB"),
        SUNSIG("56 48 83 EC ? 4C 89 CE 49 8B 01 48 8B 40 28 4C 89 C9 FF 15 ? ? ? ? 89 C1 B0 01 84 C9 74 ? 48 8B 46 08"),
        &self->Item_allowOffhandCheckB));
    push(Direct(SUNSIG("LocalPlayer::normalTick"),
        SUNSIG("55 56 57 53 48 83 EC 78 48 8D 6C 24 70 48 C7 45 00 FE FF FF FF 48 89 CE 48 8B 89 00 02 00 00"),
        &self->LocalPlayer_normalTick));
    push(Direct(SUNSIG("Mob::getCurrentSwingDuration"),
        SUNSIG("56 57 53 48 83 EC 20 89 D3 48 89 CE BA 03 00 00 00 E8 ? ? ? ? 48 85 C0"),
        &self->Mob_getCurrentSwingDuration));
    push(Direct(SUNSIG("ItemInHandRenderer::swingRotate"),
        SUNSIG("41 C6 45 38 01 F3 44 0F 59 0D ? ? ? ? F2 0F 11 B5 A0 00 00 00 C7 85 A8 00 00 00 00 00 00 00 F3 44 0F 59 0D ? ? ? ? 48 8D 8D 20 01 00 00 4C 8D 8D A0 00 00 00 4C 89 FA 41 0F 28 D1 E8 ? ? ? ?"),
        &self->ItemInHand_swingAngleLoad));
    push(Direct(SUNSIG("ItemInHandRenderer::render"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 68 02 00 00 48 8D AC 24 80 00 00 00 44 0F 29 A5 D0 01 00 00 44 0F 29 9D C0 01 00 00 44 0F 29 95 B0 01 00 00 44 0F 29 8D A0 01 00 00 44 0F 29 85 90 01 00 00 0F 29 BD 80 01 00 00 0F 29 B5 70 01 00 00 48 C7 85 68 01 00 00 FE FF FF FF 44 89 CF 4D 89 C6 48 89 D3 48 89 CE 48 8B 49 38 48 8B 01 48 8B 80 F0 00 00 00"),
        &self->ItemInHandRenderer_render));

    push(Direct(SUNSIG("UIContext::drawImage"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC B8 00 00 00 48 8D AC 24 ? ? ? ? 44 0F 29 4D ? 44 0F 29 45 ? 0F 29 7D ? 0F 29 75 ? 48 C7 45 ? FE FF FF FF 4D 89 CE 4D 89 C4 48 89 55 ? 48 89 CF 4C 8B AD ? ? ? ? 48 8B B5 ? ? ? ? 0F B6 85 ? ? ? ? 88 45 ? 48 8B 41 10"),
        &self->UIContext_drawImage));
    // todo remove this garbage.
    push(Direct(SUNSIG("HotbarRenderer::pathBuilder"),
        SUNSIG("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 05 00 00 48 8D AC 24 80 00 00 00 48 C7 85 90 04 00 00 FE FF FF FF 4D 89 C7 48 89 D3 48 89 CF 4C 89 8D 68 04 00 00 49 8B 81 A0 00 00 00 48 8B 70 F0"),
        &self->UIHotbarPathBuilder));
    {
        SigEntry e{};
        e.name = SUNSIG("EffectInstance fields (addEffect merge)");
        e.pattern =
            SUNSIG("44 89 49 ? 8B 72 ? 89 71 ? 0F B6 42 ? 88 41 ?");

        e.out = &off.EffectInstance_amplifier;
        e.out2 = &off.EffectInstance_duration;
        e.resolve = SigResolve::EffectInstFields;
        e.relAt = 0;
        e.instrLen = 0;
        push(e);
    }
    push(Direct(SUNSIG("RakNetInstance::ctor"),
        SUNSIG("48 8D 05 ? ? ? ? 48 89 07 48 8D 05 ? ? ? ? 48 89 47 10 48 8D 05 ? ? ? ? 48 89 47 60 C6 87 88 00 00 00 00"),
        &self->RakNetInstance_ctor));
    push(Off32(SUNSIG("NetworkSystem::remoteConnector"),
        SUNSIG("48 8B 89 ? ? ? ? 48 85 C9 0F 84 ? ? ? ? 0F 57 C0 0F 29 45 D0 48 C7 45 E0 00 00 00 00 48 8B 41 70 48 85 C0 74 65 F0 FF 40 08 48 8B 75 D8 0F 10 41 68 48 8B 41 68 0F 29 45 D0 48 85 F6 74"),
        &off.NS_RemoteConnector, 3));

    push(Off32(SUNSIG("EffectInstance::size"),
        SUNSIG("4D 69 FC ? ? ? ? 42 8B 04 39 3B 05 ? ? ? ? 75 ? 48 8B 8E 00 02 00 00"),
        &off.EffectInstance_size, 3));

    *count = n;
}

bool SigManager::applyEntry(const SigEntry& e, uintptr_t match) const {
    if (!match || !e.out) return false;

    switch (e.resolve) {
    case SigResolve::Direct:
        *static_cast<uintptr_t*>(e.out) = match;
        return true;

    case SigResolve::Rel32: {
        const int32_t disp = *reinterpret_cast<const int32_t*>(match + e.relAt);
        *static_cast<uintptr_t*>(e.out) = match + static_cast<uintptr_t>(e.instrLen) + disp;
        return true;
    }

    case SigResolve::Backstep: {
        const uintptr_t entry = match - static_cast<uintptr_t>(e.relAt);
        const unsigned char first = *reinterpret_cast<const unsigned char*>(entry);
        if (first != 0x41 && (first < 0x53 || first > 0x57)) return false;
        *static_cast<uintptr_t*>(e.out) = entry;
        return true;
    }

    case SigResolve::Offset32: {
        const int32_t value = *reinterpret_cast<const int32_t*>(match + e.relAt);
        if (value <= 0 || value >= 0x2000) return false;
        *static_cast<int*>(e.out) = value;
        return true;
    }

    case SigResolve::Offset32Pair: {
        const int32_t a = *reinterpret_cast<const int32_t*>(match + e.relAt);
        const int32_t b = *reinterpret_cast<const int32_t*>(match + e.instrLen);
        if (!e.out2 || a <= 0 || a >= 0x2000 || b <= 0 || b >= 0x2000) return false;
        *static_cast<int*>(e.out) = a;
        *static_cast<int*>(e.out2) = b;
        return true;
    }

    case SigResolve::ItemNameWalk: {
        for (int i = 0; i < 0x40; ++i) {
            auto* p = reinterpret_cast<const uint8_t*>(match + i);
            if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x47 && p[3] != 0) {
                *static_cast<int*>(e.out) = static_cast<int>(p[3]);
                return true;
            }
        }
        return false;
    }

    case SigResolve::EffectInstFields: {
        auto* p = reinterpret_cast<const uint8_t*>(match);
        if (p[0] != 0x44 || p[1] != 0x89 || p[2] != 0x49 ||
            p[4] != 0x8B || p[5] != 0x72 || p[7] != 0x89 || p[8] != 0x71 ||
            p[10] != 0x0F || p[11] != 0xB6 || p[12] != 0x42 ||
            p[14] != 0x88 || p[15] != 0x41 || p[6] != p[9] || p[13] != p[16]) return false;
        const int amp = p[3], dur = p[6], noC = p[13];
        if (!e.out2 || amp <= 0 || dur <= 0 || noC <= 0 || amp >= 0x80 || dur >= 0x80 || noC >= 0x80) return false;
        *static_cast<int*>(e.out) = amp;
        *static_cast<int*>(e.out2) = dur;
        auto& o = OffsetManager::GetInstance();
        o.EffectInstance_id = 0;
        o.EffectInstance_noCounter = noC;
        return true;
    }
    }
    return false;
}

void SigManager::Initialize() {
    SigEntry table[kMaxSigs]{};
    size_t count = 0;
    buildTable(table, &count);

    std::vector<std::string> names;
    std::vector<std::string> patterns;
    names.reserve(count);
    patterns.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        names.emplace_back(table[i].name);
        patterns.emplace_back(table[i].pattern ? table[i].pattern : "");
    }

    std::vector<uintptr_t> matches(count, 0);
    auto& scanner = MemoryScanner::GetInstance();
    scanner.SetVerbose(false);
    scanner.ScanAllChunked(names, patterns, matches);

    for (size_t i = 0; i < count; ++i) {
        if (!applyEntry(table[i], matches[i]))
            LOG_WARN(std::string("Signature unavailable: ") + table[i].name, "SigManager");
    }
    SDKRuntime::Publish();
}
