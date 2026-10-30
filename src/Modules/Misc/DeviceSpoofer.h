#pragma once
#include "../Module.h"
#include <string>

class DeviceSpoofer : public Module {
public:
    DeviceSpoofer() : Module("DeviceSpoofer", "Spoofs login IDs (DeviceID, ClientRandomID, etc.)", Category::Misc) {
        AddSetting(new BoolSetting("Device ID", "Spoof Device ID", true));
        AddSetting(new BoolSetting("Client Random ID", "Spoof Client Random ID", true));
        AddSetting(new BoolSetting("Self Signed ID", "Spoof Self Signed ID", true));
        AddSetting(new BoolSetting("Platform User ID", "Spoof Platform User ID", true));
    }

    void OnEnable() override;
    void OnDisable() override;

    std::string mSpoofedDeviceId;
    int64_t mSpoofedClientRandomId = 0;
    std::string mSpoofedSelfSignedId;
    std::string mSpoofedPlatformUserId;
};
