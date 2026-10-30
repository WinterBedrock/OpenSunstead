#pragma once
#include "../Module.h"
#include "../Setting.h"
#include <Windows.h>
#include <vector>

// StarveSprint — allow sprinting when hunger is below the vanilla threshold
// (3 hunger bars / food level 6).
// Optionally spoof PlayerAuthInput sprint flags so the server does not see sprint.
class StarveSprint : public Module {
public:
    StarveSprint()
        : Module("StarveSprint", "Sprint even when hunger is below 3 bars", Category::Movement, 0) {
        AddSetting(new BoolSetting(
            "Hunger Spoof",
            "Clear sprint flags on outgoing PlayerAuthInput (server thinks you are not sprinting)",
            false));
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnUpdate() override;

    bool IsHungerSpoof() const {
        return IsEnabled() && static_cast<const BoolSetting*>(mSettings[0])->mValue;
    }

private:
    uintptr_t mPatchAddr = 0;
    std::vector<unsigned char> mOg;
    bool mPatched = false;

    bool findAndApply();
    void restore();
    void reassert();
    static bool writeBytes(uintptr_t addr, const void* data, size_t len);
    static bool sehReadN(uintptr_t addr, unsigned char* out, size_t n);
};
