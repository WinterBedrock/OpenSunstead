#include "ToggleSounds.h"
#include "../../Core/ConfigManager.h"
#include "../../Core/AssetDeployer.h"
#include "../../resource.h"
#include "../Setting.h"
#include <Windows.h>
#include <mmsystem.h>
#include <filesystem>
#pragma comment(lib, "winmm.lib")

ToggleSounds* ToggleSounds::sInstance = nullptr;

int ToggleSounds::GetSoundType() {
    if (!sInstance || sInstance->mSettings.empty()) return 0;
    return static_cast<EnumSetting*>(sInstance->mSettings[0])->mValue;
}

// Maps (pack, enabled) -> (asset filename, RCDATA resource id).
static void ResolveSound(int type, bool enabled, const char** outName, int* outResId) {
    switch (type) {
        case 1: // Modern
            *outName  = enabled ? "modern_on.wav" : "modern_off.wav";
            *outResId = enabled ? IDR_SOUND_MODERN_ON : IDR_SOUND_MODERN_OFF;
            break;
        case 2: // Smooth
            *outName  = enabled ? "smooth_on.wav" : "smooth_off.wav";
            *outResId = enabled ? IDR_SOUND_SMOOTH_ON : IDR_SOUND_SMOOTH_OFF;
            break;
        case 0: // Default
        default:
            *outName  = enabled ? "on.wav" : "off.wav";
            *outResId = enabled ? IDR_SOUND_ON : IDR_SOUND_OFF;
            break;
    }
}

// Returns the on-disk path of the selected bundled toggle sound, extracting it
// from the DLL's embedded RCDATA on demand if it is not already there.
static std::string EnsureSound(int type, bool enabled, int* resIdOut) {
    const char* name = nullptr;
    int resId = 0;
    ResolveSound(type, enabled, &name, &resId);
    *resIdOut = resId;
    std::string path = ConfigManager::GetSunsteadDir() + "assets\\" + name;
    if (!std::filesystem::exists(path)) {
        AssetDeployer::Extract(resId, name, false);
    }
    return path;
}

void ToggleSounds::PlayToggleSound(bool enabled) {
    int type = GetSoundType();
    int resId = 0;
    std::string path = EnsureSound(type, enabled, &resId);
    if (std::filesystem::exists(path)) {
        PlaySoundA(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        return;
    }
    // Last-resort fallback: play straight from the embedded resource in memory.
    extern HMODULE g_hSelfModule;
    HMODULE mod = g_hSelfModule ? g_hSelfModule : GetModuleHandleW(nullptr);
    PlaySoundW(MAKEINTRESOURCEW(resId), mod, SND_RESOURCE | SND_ASYNC | SND_NODEFAULT);
}
