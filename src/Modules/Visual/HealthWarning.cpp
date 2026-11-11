#include "HealthWarning.h"
#include "../../Utils/MCSDK.h"
#include "../Setting.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Client/ClientInstance.h"
#include "../../SDK/Actor/Actor.h"
#include <imgui.h>
#include <optional>

void HealthWarning::OnRender() {
    if (!IsEnabled()) return;
    if (mSettings.size() < 4) return;

    float threshold = static_cast<NumberSetting*>(mSettings[0])->mValue;
    float maxOpacity = static_cast<NumberSetting*>(mSettings[1])->mValue;
    float sizeFrac   = static_cast<NumberSetting*>(mSettings[2])->mValue;
    ColorSetting* col = static_cast<ColorSetting*>(mSettings[3]);

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, 0x200)) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp || !sdk_safe_ptr(lp, 0x230)) return;

    std::optional<float> hpOpt = lp->getHealth();
    if (!hpOpt.has_value()) return;
    float hp = hpOpt.value();
    if (hp <= 0.0f || hp >= threshold) return;

    float t = 1.0f - (hp / threshold);
    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
    float alpha = maxOpacity * t;
    if (alpha <= 0.001f) return;

    ImGuiIO& io = ImGui::GetIO();
    float w = io.DisplaySize.x, h = io.DisplaySize.y;
    if (w <= 0.0f || h <= 0.0f) return;
    float band = (w < h ? w : h) * sizeFrac;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float cr = col->mValue[0], cg = col->mValue[1], cb = col->mValue[2];
    float ca = alpha * col->mValue[3];
    ImU32 edge = ImColor(cr, cg, cb, ca);
    ImU32 inner = ImColor(cr, cg, cb, 0.0f);

    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, band), edge, edge, inner, inner);
    dl->AddRectFilledMultiColor(ImVec2(0, h - band), ImVec2(w, h), inner, inner, edge, edge);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(band, h), edge, inner, inner, edge);
    dl->AddRectFilledMultiColor(ImVec2(w - band, 0), ImVec2(w, h), inner, edge, edge, inner);
}
