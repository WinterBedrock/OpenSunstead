#include "OffsetValidator.h"
#include "OffsetManager.h"
#include "MemoryScanner.h"
#include "../Core/Logger.h"
#include "../Core/CrashHandler.h"
#include "../SDK/Types.h"
#include "../SDK/Client/ClientInstance.h"
#include "../SDK/Client/ScreenView.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

// -----------------------------------------------------------------------------
// Resolved-offsets report.
//
// What this is NOT: a per-frame plausibility prober that guesses whether a
// field "looks right". That was built once and was worse than useless — it
// logged FAILs during the loading screen (ClientInstance not live yet), set a
// one-shot "reported" flag, and then never reported the in-game state at all.
//
// What this IS: a one-shot dump of every OffsetManager value actually in use,
// in the same style as the signature scan report ("Actor component ptrs:
// stateVector=0x218 aabb=0x220 rotation=0x228"). The signature scanner proves
// CODE addresses; this proves which DATA offsets the SDK is dereferencing.
// When something reads garbage, this line is what tells you which offset it
// was, without having to attach a debugger.
//
// Both entry points retry each frame until the game object they need exists,
// then log exactly once.
// -----------------------------------------------------------------------------

namespace {

bool g_gameReported = false;
bool g_uiReported   = false;

// Log one grouped line. Buffer is on the stack and short — every group fits.
void logGroup(const char* group, const char* fmt, ...) {
    char b[224];
    int n = std::snprintf(b, sizeof(b), "%s: ", group);
    if (n < 0 || n >= (int)sizeof(b)) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b + n, sizeof(b) - (size_t)n, fmt, ap);
    va_end(ap);
    LOG_INFO(b, "Offsets");
}

} // namespace

// Logs the resolved offsets once ClientInstance is live. Returns -1 while the
// game is still on the loading screen (caller retries); >= 0 afterwards.
int OffsetValidator::Validate() {
    if (g_gameReported) return 0;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, Layout::Probe::ClientInstance)) return -1;

    g_gameReported = true;

    auto& o = Offsets();

    logGroup("Actor",
        "stateVector=0x%X aabb=0x%X rotation=0x%X gameMode=0x%X serializedSkin=0x%X",
        o.Actor_mStateVector, o.Actor_mAABB, o.Actor_mRotation,
        o.Actor_mGameMode, o.Actor_mSerializedSkin);

    logGroup("ClientInstance",
        "MinecraftGame=0x%X Minecraft=0x%X LevelRenderer=0x%X PacketSender=0x%X "
        "GuiData=0x%X WeakEntityRef=0x%X",
        o.CI_MinecraftGame, o.CI_Minecraft, o.CI_LevelRenderer,
        o.CI_PacketSender, o.CI_GuiData, o.CI_WeakEntityRef);

    logGroup("MinecraftGame",
        "ClientMap=0x%X ClientMapSize=0x%X GameRenderer=0x%X",
        o.MG_ClientMap, o.MG_ClientMapSize, o.MG_GameRenderer);

    logGroup("Minecraft/GameSession",
        "GameSession=0x%X Timer=0x%X Level=0x%X",
        o.MC_GameSession, o.MC_Timer, o.GS_Level);

    logGroup("Platform_GameCore", "Self=0x%X GameHolder=0x%X MinecraftGame=0x%X",
        o.PGC_Self, o.PGC_GameHolder, o.PGC_MinecraftGame);

    logGroup("Network",
        "CI->NetworkSystem=vt[%d] (PS_NetworkSystem=0x%X is DEAD) "
        "NS_RemoteConnector=0x%X RC_RakNetConnector=0x%X",
        Layout::VTable::ClientInstance_getNetworkSystem, o.PS_NetworkSystem,
        o.NS_RemoteConnector, o.RC_RakNetConnector);

    logGroup("GuiData",
        "Width=0x%X Height=0x%X ScaledW=0x%X ScaledH=0x%X GuiScale=0x%X GuiScaleFrac=0x%X",
        o.GD_Width, o.GD_Height, o.GD_ScaledWidth, o.GD_ScaledHeight,
        o.GD_GuiScale, o.GD_GuiScaleFrac);

    logGroup("Level", "PlayerList=0x%X", o.LVL_PlayerList);

    logGroup("LevelRenderer",
        "RendererPlayer=0x%X CameraPos=0x%X",
        o.LevelRenderer_mRendererPlayer, o.LevelRendererPlayer_mCameraPos);

    logGroup("Items/Effects",
        "Item_mName=0x%X Effect: id=0x%X dur=0x%X amp=0x%X noC=0x%X size=0x%X",
        o.Item_mNameStrOffset, o.EffectInstance_id, o.EffectInstance_duration,
        o.EffectInstance_amplifier, o.EffectInstance_noCounter, o.EffectInstance_size);

    // The resolved context, so the log line and the code that produced it can
    // be compared directly. Nulls here mean the walk degrades to no-op, not
    // that an offset is wrong — the loading screen legitimately has no player.
    SDK::Minecraft* mc = ci->getMinecraft();
    SDK::Level* level = mc ? mc->getLevel() : nullptr;
    SDK::Actor* lp = ci->getLocalPlayer();
    char ctx[160];
    std::snprintf(ctx, sizeof(ctx),
        "Resolved context: ci=%p minecraft=%p level=%p localPlayer=%p",
        (void*)ci, (void*)mc, (void*)level, (void*)lp);
    LOG_INFO(ctx, "Offsets");

    return 0;
}

// Logs the UI offsets once a live ScreenView is in hand. Present-time reporting
// cannot reach the UI graph — this hook is the only place a view exists.
bool OffsetValidator::ValidateScreenView(void* viewRaw) {
    if (g_uiReported) return true;
    auto* view = reinterpret_cast<SDK::ScreenView*>(viewRaw);
    if (!view || !sdk_safe_ptr(view, 0x60)) return false;

    g_uiReported = true;

    auto& o = Offsets();
    logGroup("ScreenView",
        "ScreenController=0x%X VisualTree=0x%X",
        o.SV_ScreenController, o.SV_VisualTree);
    logGroup("UIControl",
        "Position=0x%X Flags=0x%X Name=0x%X RelPos=0x%X Size=0x%X Parent=0x%X Children=0x%X",
        o.UI_Position, o.UI_Flags, o.UI_Name, o.UI_RelPos, o.UI_Size,
        o.UI_Parent, o.UI_Children);
    logGroup("VisualTree",
        "RootControl=0x%X Color=0x%X Tessellator=0x%X",
        o.VT_RootControl, o.VT_Color, o.VT_Tessellator);

    // The screen name round-trip is the end-to-end proof for the whole
    // SV/VT/UI chain: it only resolves if every offset in it is right.
    // (No SEH here — an SEH frame cannot hold the std::string, and every
    // other call site reads getScreenName unguarded anyway.)
    std::string name = view->getScreenName();
    LOG_INFO(std::string("Resolved screen name: \"") + name + "\"", "Offsets");

    return true;
}

bool OffsetValidator::HasReport() const { return g_gameReported; }
