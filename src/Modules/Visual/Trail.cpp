#include "Trail.h"
#include "../../Utils/MCSDK.h"
#include "../../Render/FrameView.h"
#include "../../Hooks/GameHooks.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../SDK/Network/PacketTypes.h"
#include "../Setting.h"
#include <imgui.h>
#include <cmath>
#include <Windows.h>
#include <algorithm>

static constexpr float kTrailEyeToFeet = 1.62f;
static constexpr float kTrailMinStep = 0.001f;

void Trail::OnTick() {
    if (!IsEnabled()) return;
    ImGuiIO& io = ImGui::GetIO();
    const float dt = (io.DeltaTime > 0.0f && io.DeltaTime < 0.25f) ? io.DeltaTime : 1.0f / 60.0f;
    const int   type = static_cast<EnumSetting*>(mSettings[0])->mValue;
    // Circle mode samples on its own (slower) gap; Line keeps "Delay".
    int delayMs = (int)static_cast<NumberSetting*>(mSettings[3])->mValue;
    if (type == 1 && mSettings.size() > 9 && mSettings[9])
        delayMs = (int)static_cast<NumberSetting*>(mSettings[9])->mValue;
    const bool  showOthers = static_cast<BoolSetting*>(mSettings[6])->mValue;
    const bool  showSelf   = static_cast<BoolSetting*>(mSettings[7])->mValue;
    const float maxPts     = static_cast<NumberSetting*>(mSettings[1])->mValue;
    const float fadeSec    = static_cast<NumberSetting*>(mSettings[2])->mValue;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    SDK::Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp) return;
    Vec3 myPos{};
    if (!SDK::sehGetPos(lp, &myPos)) return;

    unsigned long long now = GetTickCount64();

    if (showSelf) {
        Trail::ActorPoints* bucket = nullptr;
        for (auto& b : mPoints) if (b.entity == lp) { bucket = &b; break; }
        if (!bucket) { mPoints.push_back({ (void*)lp, {} }); bucket = &mPoints.back(); }
        if (bucket->points.empty() || (now - bucket->points.back().timeMs) >= (unsigned)delayMs) {
            const float fx = myPos.x, fy = myPos.y - kTrailEyeToFeet, fz = myPos.z;
            bool moved = true;
            if (!bucket->points.empty()) {
                const auto& last = bucket->points.back();
                const float dx = fx - last.x, dy = fy - last.y, dz = fz - last.z;
                if (dx * dx + dy * dy + dz * dz < kTrailMinStep * kTrailMinStep) moved = false;
            }
            if (moved) {
                bucket->points.push_back({ (void*)lp, now, fx, fy, fz });
                while (bucket->points.size() > (size_t)maxPts) bucket->points.erase(bucket->points.begin());
            } else if (!bucket->points.empty()) {
                // standing still, refresh the last samples lifetime
                bucket->points.back().timeMs = now;
            }
        }
    }

    // others
    if (showOthers) {
        std::vector<SDK::Actor*> actors;
        SDK::Level* level = nullptr;
        if (ci) { SDK::Minecraft* mc = ci->getMinecraft(); if (mc) level = mc->getLevel(); }
        if (level && SDK::sehGetActorList(level, &actors)) {
            for (SDK::Actor* a : actors) {
                if (!a || a == lp) continue;
                if (!SDK::sehIsPlayer(a)) continue;
                Vec3 p{};
                if (!SDK::sehGetPos(a, &p)) continue;
                float dx = p.x - myPos.x, dz = p.z - myPos.z;
                if (dx * dx + dz * dz > 64.0f * 64.0f) continue;
                Trail::ActorPoints* bucket = nullptr;
                for (auto& b : mPoints) if (b.entity == a) { bucket = &b; break; }
                if (!bucket) { mPoints.push_back({ (void*)a, {} }); bucket = &mPoints.back(); }
                if (bucket->points.empty() || (now - bucket->points.back().timeMs) >= (unsigned)delayMs) {
                    const float fx = p.x, fy = p.y - kTrailEyeToFeet, fz = p.z;
                    bool moved = true;
                    if (!bucket->points.empty()) {
                        const auto& last = bucket->points.back();
                        const float dx = fx - last.x, dy = fy - last.y, dz = fz - last.z;
                        if (dx * dx + dy * dy + dz * dz < kTrailMinStep * kTrailMinStep) moved = false;
                    }
                    if (moved) {
                        bucket->points.push_back({ (void*)a, now, fx, fy, fz });
                        while (bucket->points.size() > (size_t)maxPts) bucket->points.erase(bucket->points.begin());
                    } else if (!bucket->points.empty()) {
                        bucket->points.back().timeMs = now;
                    }
                }
            }
        }
    }

    const unsigned long long expiry = (unsigned long long)(fadeSec * 1000.0f);
    for (auto it = mPoints.begin(); it != mPoints.end();) {
        auto& pts = it->points;
        pts.erase(std::remove_if(pts.begin(), pts.end(),
            [&](const Trail::Point& p) { return now - p.timeMs > expiry; }), pts.end());
        if (pts.empty()) it = mPoints.erase(it);
        else ++it;
    }
}

void Trail::OnRender() {
    if (!IsEnabled()) return;
    const bool alwaysShow = (mSettings.size() > 10 && mSettings[10])
        ? static_cast<BoolSetting*>(mSettings[10])->mValue : false;
    if (!alwaysShow && SDK::ScreenView::currentScreenName() != "hud_screen") return;
    const bool hideFirst = (mSettings.size() > 11 && mSettings[11])
        ? static_cast<BoolSetting*>(mSettings[11])->mValue : false;
    if (hideFirst && GameHooks::LastPerspective() == 0) return;
    ImGuiIO& io = ImGui::GetIO();
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    float mvp[16];
    Vec3 origin(0, 0, 0);
    if (!acquireCameraMVP(ci, mvp, origin)) return;
    ImVec2 screen = io.DisplaySize;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    unsigned long long now = GetTickCount64();
    const float fadeSec = static_cast<NumberSetting*>(mSettings[2])->mValue;
    const float maxWidth = static_cast<NumberSetting*>(mSettings[4])->mValue;
    const int   type = static_cast<EnumSetting*>(mSettings[0])->mValue;
    const bool  rgb  = static_cast<BoolSetting*>(mSettings[5])->mValue;
    float scaleIn = 0.80f;
    if (mSettings.size() > 8 && mSettings[8])
        scaleIn = std::clamp(static_cast<NumberSetting*>(mSettings[8])->mValue, 0.0f, 1.0f);
    const uint64_t nowMs = ColorUtils::GetTimeMs();

    const float lineThick = std::clamp(maxWidth * 0.1f, 1.0f, 2.5f);

    for (auto& bucket : mPoints) {
        ImVec2 prevS;
        bool havePrev = false;
        int hueIdx = 0;
        for (const Trail::Point& p : bucket.points) {
            const float ageSec = (now - p.timeMs) / 1000.0f;
            const float progress = 1.0f - (ageSec / fadeSec);
            if (progress <= 0.0f || progress > 1.0f) { ++hueIdx; continue; }
            ImVec2 s;
            if (!worldToScreen(Vec3(p.x, p.y, p.z), mvp, origin, screen, &s)) { havePrev = false; ++hueIdx; continue; }
            const float animation = progress;

            if (type == 0) { // line
                ImColor col;
                if (rgb)   col = ColorUtils::getThemedColor((float)(hueIdx / 10) * 60.0f, nowMs);
                else       col = ImColor(1.0f, 1.0f, 1.0f, 1.0f);
                col.Value.w *= animation;
                if (havePrev) dl->AddLine(prevS, s, col, lineThick);
            } else {          // circle
                float scaledSize = maxWidth * animation;
                if (animation >= scaleIn) {
                    const float denom = 1.0f - scaleIn;
                    float k = (denom > 1e-4f) ? (animation - scaleIn) / denom : 0.0f;
                    scaledSize *= std::fabs(1.0f - k);
                } else {
                    scaledSize = maxWidth * animation;
                }
                {
                    const float dx = p.x - origin.x, dy = p.y - origin.y, dz = p.z - origin.z;
                    float distScale = std::fabs(std::sqrt(dx * dx + dy * dy + dz * dz) / 2.5f);
                    if (distScale < 1.0f) distScale = 1.0f;
                    scaledSize /= distScale;
                }
                const float r = scaledSize + 1.0f;
                ImColor col;
                if (rgb)   col = ColorUtils::getThemedColor((float)(hueIdx / 10) * 60.0f, nowMs);
                else       col = ImColor(1.0f, 1.0f, 1.0f, 1.0f);
                int segs = (int)(32.0f * animation);
                if (segs < 6) segs = 6;
                ImColor glow(col.Value.x, col.Value.y, col.Value.z, animation);
                ImRenderUtils::fillShadowCircle(s, r, glow, animation, segs, 6.0f, 0.55f);
                dl->AddCircleFilled(s, r, ImColor(col.Value.x, col.Value.y, col.Value.z, animation), segs);
            }
            prevS = s;
            havePrev = true;
            ++hueIdx;
        }
    }
}

void Trail::OnPacketReceive(class SDK::Packet* packet, int packetId, bool& cancel) {
    if (packetId == (int)SDK::TypePacket::ChangeDimension) mPoints.clear();
}
