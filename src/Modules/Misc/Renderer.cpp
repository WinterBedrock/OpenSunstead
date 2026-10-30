#include "Renderer.h"
#include "../Setting.h"
#include "../../Hooks/DXHooks.h"
#include "../Visual/Notifications.h"

// Push the module's toggle values into the DX-hook control flags. Runs every
// frame from OnRender (the module is registered in the always-render loop so
// this fires whether the module is "enabled" or not, matching Interface).
void Renderer::OnRender() {
    // Force VSync applies live (the Present hook reads g_ForceVSync each frame).
    g_ForceVSync = GetForceVSync();

    bool wantDx11 = GetForceDx11();

    // On the very first frame just latch the current state without notifying.
    if (!mInitialized) {
        mInitialized = true;
        mPrevForceDx11 = wantDx11;
        g_ForceDX11 = wantDx11;
        return;
    }

    // Enabling Force Dx11 takes effect live (RenderManager removes the DX12
    // device on its next init and the game recreates on DX11).
    if (wantDx11 && !mPrevForceDx11) {
        g_ForceDX11 = true;
        Notifications::AddNotification("Force Dx11 enabled — switching renderer to DirectX 11");
    }
    // Disabling cannot swap the backend back at runtime; require a restart.
    else if (!wantDx11 && mPrevForceDx11) {
        g_ForceDX11 = false;
        Notifications::AddNotification("Force Dx11 disabled — restart the game to return to DirectX 12");
    }

    mPrevForceDx11 = wantDx11;
}
