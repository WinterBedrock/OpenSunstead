#include "GuiScale.h"
#include "../../SDK/SDK.h"

void GuiScale::OnTick() {
    auto* ci = SDK::ClientInstance::get();
    if (!ci) return;
    auto* gui = ci->getGuiData();
    if (!gui) return;

    auto* scaleSet = static_cast<NumberSetting*>(mSettings[0]);
    gui->updateGuiScale(scaleSet->mValue);
}
