#include "Velocity.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/Network/Packets/SetEntityMotionPacket.h"
#include "../ModuleManager.h"
#include "../../Core/Logger.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <Windows.h>

// The packet-receive hook and the game tick run on different threads; the
// queue is tiny but it is still shared, so every access takes this lock.
static std::mutex s_velQueueMx;

// Reads the packet's REAL id from its own vtable (getId is vtable index 1),
// instead of trusting the receive-hook's deduped handler->id map, which can
// mislabel packets that share a dispatcher handler.
static int ReadPacketId(SDK::Packet* packet) {
    if (!packet || !SDK::isSafeReadPtr(packet, sizeof(void*))) return -1;
    void** vtable = *reinterpret_cast<void***>(packet);
    if (!vtable || !SDK::isSafeReadPtr(vtable, sizeof(void*) * 2)) return -1;
    using getId_t = int(__fastcall*)(SDK::Packet*);
    __try {
        return reinterpret_cast<getId_t>(vtable[1])(packet);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void Velocity::OnPacketReceive(SDK::Packet* packet, int packetId, bool& cancel) {
    if (!packet) return;

    // Prefer the authoritative id from the packet vtable; fall back to the hook id.
    int realId = ReadPacketId(packet);
    bool isSetActorMotion = (realId == 40) || (realId < 0 && packetId == 40);
    if (!isSetActorMotion) return;

    SDK::ClientInstance* cInst = SDK::ClientInstance::get();
    if (!cInst) return;
    SDK::Actor* lp = cInst->getLocalPlayer();
    if (!lp) return;

    uint64_t lpRuntimeId = lp->getRuntimeID();
    const bool dbgVel = mSettings.size() > 12 && static_cast<BoolSetting*>(mSettings[12])->mValue;
    // A zero runtime id means BOTH readers failed (the game accessor sig
    // missed AND the entt walk is broken on this build). Dropping the packet
    // check in that case still gives a working Cancel mode (a locally
    // dropped SetEntityMotion for ANOTHER entity self-corrects on its next
    // absolute-position packet) instead of a dead module.
    static bool s_warnedNoId = false;
    if (lpRuntimeId == 0) {
        if (!s_warnedNoId) {
            s_warnedNoId = true;
            LOG_WARN("Velocity: local runtime id unreadable — acting on every SetEntityMotion", "Velocity");
        }
        if (dbgVel) LOG_INFO("Velocity: runtimeId unreadable, proceeding without match", "Velocity");
    }

    auto* motionPacket = reinterpret_cast<SDK::SetEntityMotionPacket*>(packet);
    if (!SDK::isSafeReadPtr(motionPacket, sizeof(SDK::SetEntityMotionPacket))) return;
    if (lpRuntimeId != 0 && motionPacket->runtimeId != lpRuntimeId) {
        if (dbgVel)
            LOG_INFO("Velocity: skip foreign motion (pkt=" + std::to_string(motionPacket->runtimeId) +
                     " me=" + std::to_string(lpRuntimeId) + ")", "Velocity");
        return;
    }
    if (dbgVel)
        LOG_INFO("Velocity: SetEntityMotion for ME (mode=" + std::to_string(
                     static_cast<EnumSetting*>(mSettings[0])->mValue) +
                 " motion=" + std::to_string(motionPacket->motion.x) + "," +
                 std::to_string(motionPacket->motion.y) + "," +
                 std::to_string(motionPacket->motion.z) + ")", "Velocity");

    EnumSetting* mode = static_cast<EnumSetting*>(mSettings[0]);
    if (mode->mValue == 0) { // Cancel
        cancel = true; // Cancel the packet entirely
        return;
    }
    if (mode->mValue == 2) { // Delay
        // HOLD the knockback: cancel the packet now, queue the motion, apply
        // it locally when the delay elapses (OnTick, game thread).
        cancel = true;
        PendingKB kb;
        kb.x = motionPacket->motion.x;
        kb.y = motionPacket->motion.y;
        kb.z = motionPacket->motion.z;
        const float delayMs = static_cast<NumberSetting*>(mSettings[3])->mValue;
        const unsigned long long now = GetTickCount64();
        kb.dueMs = now + (unsigned long long)delayMs;
        kb.queuedMs = now;
        std::lock_guard<std::mutex> lk(s_velQueueMx);
        const bool queue = mSettings.size() > 13 &&
                           static_cast<BoolSetting*>(mSettings[13])->mValue;
        if (!queue) {
            // Replace: a combo player only cares about the latest shove.
            mPending.clear();
            mPending.push_back(kb);
            return;
        }
        // Queue: every knockback that lands while one is held is REMEMBERED
        // and applied after its own delay (capped — a lag spike should not
        // build an unbounded backlog).
        if (mPending.size() < 8) mPending.push_back(kb);
        return;
    }

    // Percent: scale the motion vector in place.
    float h = static_cast<NumberSetting*>(mSettings[1])->mValue;
    float v = static_cast<NumberSetting*>(mSettings[2])->mValue;
    motionPacket->motion.x *= h;
    motionPacket->motion.z *= h;
    motionPacket->motion.y *= v;
}

// POD-only velocity write (MSVC C2712: no __try where unwinding is needed).
static bool velApplyMotion(SDK::Actor* lp, float x, float y, float z) {
    __try {
        auto* sv = lp->getStateVector();
        if (!sv) return false;
        sv->velocity.x = x;
        sv->velocity.y = y;
        sv->velocity.z = z;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void Velocity::OnTick() {
    // One lock for the whole pass: the apply loop below iterates the queue
    // and the receive thread may push/clear concurrently otherwise.
    std::lock_guard<std::mutex> lk(s_velQueueMx);
    if (mPending.empty()) return;
    if (static_cast<EnumSetting*>(mSettings[0])->mValue != 2) {
        mPending.clear();
        return;
    }

    const unsigned long long now = GetTickCount64();
    bool due = false;
    for (const PendingKB& kb : mPending)
        if (now >= kb.dueMs) { due = true; break; }
    if (!due) return;

    // Apply every due knockback to the local motion (the exact thing the
    // cancelled SetEntityMotion packet would have done). Sequential mode
    // applies only the FRONT one — the rest wait their turn below.
    const bool sequential = mSettings.size() > 14 &&
                            static_cast<BoolSetting*>(mSettings[14])->mValue;
    SDK::ClientInstance* cInst = SDK::ClientInstance::get();
    if (!cInst) return;
    SDK::Actor* lp = cInst->getLocalPlayer();
    if (!lp) return;
    bool appliedOne = false;
    for (const PendingKB& kb : mPending) {
        if (now < kb.dueMs) continue;
        if (sequential && appliedOne) break;
        velApplyMotion(lp, kb.x, kb.y, kb.z);
        appliedOne = true;
    }

    if (sequential && mPending.size() > 1) {
        // One at a time: only the FRONT (earliest) knockback fired above; the
        // rest start counting their own delay FROM NOW.
        const float delayMs = static_cast<NumberSetting*>(mSettings[3])->mValue;
        std::vector<PendingKB> keep;
        bool droppedFront = false;
        for (const PendingKB& kb : mPending) {
            if (!droppedFront && now >= kb.dueMs) { droppedFront = true; continue; }
            PendingKB n = kb;
            n.dueMs = now + (unsigned long long)delayMs;
            keep.push_back(n);
        }
        mPending.swap(keep);
        return;
    }

    // Drop everything that fired; keep any still-pending newer entry.
    std::vector<PendingKB> keep;
    for (const PendingKB& kb : mPending)
        if (now < kb.dueMs) keep.push_back(kb);
    mPending.swap(keep);
}

void Velocity::OnDisable() {
    std::lock_guard<std::mutex> lk(s_velQueueMx);
    mPending.clear();
}

// The delay readout: a plate above the hotbar area showing the queued
// knockback and a themed slider bar shrinking to zero as the delay elapses.
// The bar is drawn TargetHUD-style: theme gradient fill, a glossy 3D shading
// pass, an accent glow spilling out of the bar and a bright leading cap, on
// top of a blurred, lightly dimmed, rounded plate — all of it driven by the
// module's own settings (Scale / Blur / Dim / Rounding / Bar Height / Bar
// Glow / 3D Bar).
void Velocity::OnRender() {
    if (mPending.empty()) return;
    if (static_cast<EnumSetting*>(mSettings[0])->mValue != 2) return;
    if (!static_cast<BoolSetting*>(mSettings[4])->mValue) return;
    if (!SDK::isGameplayInputActive()) return;

    // Copy the head under the lock; the receive thread may push while drawing.
    PendingKB kb{};
    {
        std::lock_guard<std::mutex> lk(s_velQueueMx);
        if (mPending.empty()) return;
        kb = mPending.front();
    }
    const unsigned long long now = GetTickCount64();
    const float delayMs = static_cast<NumberSetting*>(mSettings[3])->mValue;
    if (delayMs < 1.0f) return;
    const float remainMs = std::max(0.0f, (float)(kb.dueMs - now));
    const float t = std::clamp(remainMs / delayMs, 0.0f, 1.0f);

    // ---- Look settings ----------------------------------------------------
    const float scale    = static_cast<NumberSetting*>(mSettings[5])->mValue * mScale;
    const float blurAmt  = static_cast<NumberSetting*>(mSettings[6])->mValue;
    const float dim      = static_cast<NumberSetting*>(mSettings[7])->mValue;
    const float rounding = static_cast<NumberSetting*>(mSettings[8])->mValue * scale;
    const float barH     = static_cast<NumberSetting*>(mSettings[9])->mValue * scale;
    const bool  glowOn   = static_cast<BoolSetting*>(mSettings[10])->mValue;
    const bool  shadeOn  = static_cast<BoolSetting*>(mSettings[11])->mValue;

    // ---- Geometry ---------------------------------------------------------
    const float w = 220.0f * scale, h = 56.0f * scale;
    const float x = ImGui::GetIO().DisplaySize.x * 0.5f - w * 0.5f;
    const float y = ImGui::GetIO().DisplaySize.y * 0.73f;
    const ImVec4 plate(x, y, x + w, y + h);
    const float  pad = 10.0f * scale;

    // ---- Backdrop blur ----------------------------------------------------
    // A D2D blur callback paints an OPAQUE blurred rect onto the backbuffer
    // at replay time, so it must land BEFORE (and on an earlier list than)
    // everything drawn here — same contract as TargetHUD's backdrop.
    if (blurAmt > 0.05f) {
        bool guiOpen = false;
        if (Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI"))
            guiOpen = gui->IsEnabled();
        // The setting is 0..1 but D2D::addBlurOptimized takes a PIXEL-radius
        // strength (ChestAura passes 4.0) — scale it into the same range, the
        // previous 0.35 straight pass was sub-pixel and therefore invisible.
        if (!guiOpen)
            D2D::addBlurOptimized(ImGui::GetBackgroundDrawList(), 12.0f * blurAmt,
                                  std::optional<ImVec4>(plate), rounding);
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    const uint64_t ms = ColorUtils::GetTimeMs();
    ImColor accent  = ColorUtils::getThemedColor(0.0f, ms);
    ImColor accent2 = ColorUtils::getThemedColor(60.0f, ms);

    // ---- Plate: soft shadow, LIGHT dim fill, hairline border ---------------
    ImRenderUtils::fillShadowRectangle(plate, ImColor(0, 0, 0), 0.32f,
                                       rounding, 12.0f * scale, 0.5f, dl);
    ImRenderUtils::fillRectangle(plate, ImColor(12, 12, 16, (int)(255.0f * dim)),
                                 1.0f, rounding, dl);
    dl->AddRect(ImVec2(plate.x, plate.y), ImVec2(plate.z, plate.w),
                ImColor(255, 255, 255, 28), rounding, 0, 1.0f);

    // ---- Label -------------------------------------------------------------
    char label[48];
    snprintf(label, sizeof(label), "Knockback delayed  %d ms", (int)remainMs);
    ImRenderUtils::drawText(ImVec2(plate.x + pad, plate.y + 8.0f * scale), label,
                            ImColor(235, 235, 240), 14.0f * scale, 0.95f, true, dl);

    // ---- The slider bar ----------------------------------------------------
    // Track: a fully rounded pill the fill slides along.
    const float trackY = plate.w - 16.0f * scale - barH;
    const ImVec4 track(plate.x + pad, trackY, plate.z - pad, trackY + barH);
    ImRenderUtils::fillRectangle(track, ImColor(255, 255, 255, 26), 1.0f, barH * 0.5f, dl);

    const float span = track.z - track.x;
    ImVec4 fill(track.x, track.y, track.x + span * t, track.w);
    if (fill.z - fill.x > 1.0f) {
        // Accent glow spilling AROUND and ABOVE the filled part — drawn first
        // so the bar itself sits on top of its own light.
        if (glowOn) {
            ImRenderUtils::fillShadowRectangle(fill, accent, 0.45f, barH * 0.5f,
                                               10.0f * scale, 0.5f, dl);
            // The literal "glow on top": a short vertical light gradient
            // fading upward from the bar's top edge.
            ImVec4 topGlow(fill.x, track.y - 7.0f * scale, fill.z, track.y + barH * 0.25f);
            ImRenderUtils::fillGradientRounded(dl, topGlow,
                ImColor(accent.Value.x, accent.Value.y, accent.Value.z, 0.0f),
                ImColor(accent.Value.x, accent.Value.y, accent.Value.z, 0.55f),
                barH * 0.35f, 0, /*vertical=*/true);
        }

        // Theme gradient along the fill, rounded like a pill.
        ImColor c0 = accent, c1 = accent2;
        c0.Value.w = 0.95f; c1.Value.w = 0.95f;
        ImRenderUtils::fillGradientRounded(dl, fill, c0, c1,
                                           barH * 0.5f, 0, /*vertical=*/false);

        // 3D volume pass (TargetHUD's shaded-cylinder look): a lit top fading
        // out, a dark bottom fading in, and a crisp glossy line — the bar
        // reads as a lit tube, not a flat stripe.
        if (shadeOn) {
            ImRenderUtils::fillGradientRounded(dl, fill,
                ImColor(255, 255, 255, 80), ImColor(255, 255, 255, 0),
                barH * 0.5f, 0, /*vertical=*/true);
            ImRenderUtils::fillGradientRounded(dl, fill,
                ImColor(0, 0, 0, 0), ImColor(0, 0, 0, 105),
                barH * 0.5f, 0, /*vertical=*/true);
            dl->AddLine(ImVec2(fill.x + barH * 0.4f, fill.y + 1.0f),
                        ImVec2(fill.z - barH * 0.4f, fill.y + 1.0f),
                        ImColor(255, 255, 255, 70), 1.0f);
        }

        // The leading cap (the slider's "thumb"): a bright vertical pill at
        // the fill's head, with its own little glow so the edge reads lit.
        if (fill.z - fill.x > barH * 0.7f) {
            const float capW = 4.0f * scale;
            ImVec4 cap(fill.z - capW, track.y - 1.5f * scale, fill.z, track.w + 1.5f * scale);
            if (glowOn)
                ImRenderUtils::fillShadowRectangle(cap, accent2, 0.55f,
                                                    capW * 0.5f, 8.0f * scale, 0.5f, dl);
            ImRenderUtils::fillRectangle(cap, ImColor(255, 255, 255, 225),
                                         1.0f, capW * 0.5f, dl);
        }
    }
}
