#include "InventoryMove.h"
#include "../../Utils/MCSDK.h"
#include <Windows.h>
#include <cmath>

// Global set by GameHooks cursor hooks
extern bool g_IsCursorGrabbed;

void InventoryMove::OnTick() {
    if (!IsEnabled()) return;
    // Only work when cursor is NOT grabbed (inventory/screen open)
    if (g_IsCursorGrabbed) return;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp) return;

    bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
    bool a = (GetAsyncKeyState('A') & 0x8000) != 0;
    bool s = (GetAsyncKeyState('S') & 0x8000) != 0;
    bool d = (GetAsyncKeyState('D') & 0x8000) != 0;
    bool space = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (!(w || a || s || d || space || shift)) return;

    auto* stateVec = lp->getStateVector();
    if (!stateVec) return;

    Vec2 rot = lp->getRot();
    float yaw = rot.y * 3.14159265f / 180.0f;
    float forwardX = -sin(yaw), forwardZ = cos(yaw);
    float rightX = cos(yaw), rightZ = sin(yaw);
    float speed = 0.25f;
    float mx = 0, mz = 0;
    if (w) { mx += forwardX; mz += forwardZ; }
    if (s) { mx -= forwardX; mz -= forwardZ; }
    if (a) { mx += rightX; mz += rightZ; }
    if (d) { mx -= rightX; mz -= rightZ; }
    float len = sqrt(mx*mx + mz*mz);
    if (len > 0.001f) { mx = mx/len*speed; mz = mz/len*speed; }
    stateVec->velocity.x = mx;
    stateVec->velocity.z = mz;
    if (space) stateVec->velocity.y = speed;
    if (shift) stateVec->velocity.y = -speed;
}

void InventoryMove::OnPacketReceive(SDK::Packet* packet, int packetId, bool& cancel) {}
