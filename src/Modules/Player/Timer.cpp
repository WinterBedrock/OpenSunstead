#include "Timer.h"
#include "../../Utils/MCSDK.h"

void Timer::OnEnable() {
    float speed = static_cast<NumberSetting*>(mSettings[0])->mValue;
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (ci) {
        SDK::Minecraft* mc = ci->getMinecraft();
        if (mc) {
            SDK::Timer* timer = mc->getTimer();
            if (timer && SDK::isSafeReadPtr(timer, sizeof(SDK::Timer))) {
                timer->tps = speed;
            }
        }
    }
}

void Timer::OnDisable() {
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (ci) {
        SDK::Minecraft* mc = ci->getMinecraft();
        if (mc) {
            SDK::Timer* timer = mc->getTimer();
            if (timer && SDK::isSafeReadPtr(timer, sizeof(SDK::Timer))) {
                timer->tps = 20.0f;
            }
        }
    }
}

void Timer::OnTick() {
    if (!IsEnabled()) return;
    float speed = static_cast<NumberSetting*>(mSettings[0])->mValue;
    SetTps(speed);
}

bool Timer::SetTps(float tps) {
    __try {
        SDK::ClientInstance* ci = SDK::ClientInstance::get();
        if (!ci) return false;
        SDK::Minecraft* mc = ci->getMinecraft();
        if (!mc) return false;
        SDK::Timer* timer = mc->getTimer();
        if (!timer || !SDK::isSafeReadPtr(timer, sizeof(SDK::Timer))) return false;
        timer->tps = tps;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
