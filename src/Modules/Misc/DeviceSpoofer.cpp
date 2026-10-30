#include "DeviceSpoofer.h"
#include <random>
#include <sstream>
#include <iomanip>

static std::string generateUUID() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; i++) ss << dis(gen);
    ss << "-4"; // UUID version 4
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    ss << dis2(gen);
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; i++) ss << dis(gen);
    return ss.str();
}

static int64_t generateRandomInt64() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<int64_t> dis;
    return dis(gen);
}

void DeviceSpoofer::OnEnable() {
    mSpoofedDeviceId = generateUUID();
    mSpoofedClientRandomId = generateRandomInt64();
    mSpoofedSelfSignedId = generateUUID();
    mSpoofedPlatformUserId = std::to_string(generateRandomInt64() & 0x7FFFFFFFFFFFFFFFLL);
}

void DeviceSpoofer::OnDisable() {
}
