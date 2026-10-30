#include "AssetDeployer.h"
#include "../resource.h"
#include "ConfigManager.h"
#include "Logger.h"
#include <Windows.h>
#include <string>
#include <cstdio>

extern HMODULE g_hSelfModule;

// Write one RCDATA resource to <%APPDATA%/Sunstead/assets>/<outName>.
// Mirrors ConfigManager::GetSunsteadDir() so assets live alongside configs.
static bool ExtractResource(int resId, const char* outName, bool overwrite) {
    HMODULE mod = g_hSelfModule ? g_hSelfModule : GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(mod, MAKEINTRESOURCEW(resId), (LPCWSTR)RT_RCDATA);
    if (!hRes) { LOG_ERROR("FindResource failed for resource " + std::to_string(resId), "AssetDeployer"); return false; }
    DWORD sz = SizeofResource(mod, hRes);
    HGLOBAL hData = LoadResource(mod, hRes);
    if (!hData || sz == 0) { LOG_ERROR("LoadResource failed for resource " + std::to_string(resId), "AssetDeployer"); return false; }
    void* data = LockResource(hData);
    if (!data) return false;

    std::string dir = ConfigManager::GetSunsteadDir() + "assets\\";
    CreateDirectoryA(ConfigManager::GetSunsteadDir().c_str(), nullptr); // ensure base
    CreateDirectoryA(dir.c_str(), nullptr);                             // ensure assets
    std::string outPath = dir + outName;

    if (!overwrite) {
        DWORD attr = GetFileAttributesA(outPath.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) { LOG_INFO(std::string(outName) + " already exists, skipping", "AssetDeployer"); return true; }
    }
    HANDLE h = CreateFileA(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { LOG_ERROR("Failed to open output file " + std::string(outPath), "AssetDeployer"); return false; }
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, sz, &written, nullptr);
    CloseHandle(h);
    LOG_INFO("Wrote " + std::string(outPath) + " (" + std::to_string(written) + " bytes)", "AssetDeployer");
    return ok && written == sz;
}

// Public wrapper so other modules (e.g. ToggleSounds) can lazily extract a
// bundled resource on demand if DeployAll has not run yet.
bool AssetDeployer::Extract(int resId, const char* outName, bool overwrite) {
    return ExtractResource(resId, outName, overwrite);
}

void AssetDeployer::DeployAll() {
    // Toggle sounds are embedded in the DLL as RCDATA. On first inject, drop
    // them into %APPDATA%/Sunstead/assets so they exist for playback (and so the
    // user can replace them with their own .wav later). Do not overwrite.
    ExtractResource(IDR_SOUND_ON,         "on.wav",         false);
    ExtractResource(IDR_SOUND_OFF,        "off.wav",        false);
    ExtractResource(IDR_SOUND_MODERN_ON,  "modern_on.wav",  false);
    ExtractResource(IDR_SOUND_MODERN_OFF, "modern_off.wav", false);
    ExtractResource(IDR_SOUND_SMOOTH_ON,  "smooth_on.wav",  false);
    ExtractResource(IDR_SOUND_SMOOTH_OFF, "smooth_off.wav", false);

    // Fonts: overwrite so users always get the current set bundled in the DLL.
    ExtractResource(IDR_FONT_ICONS,            "Icons.ttf",             true);
    ExtractResource(IDR_FONT_PRODUCTSANS_REG,  "Product-Sans.ttf",      true);
    ExtractResource(IDR_FONT_PRODUCTSANS_BOLD, "Product-Sans-Bold.ttf", true);
    ExtractResource(IDR_FONT_MOJANGLES,        "Mojangles.ttf",         true);
    ExtractResource(IDR_FONT_MOJANGLES_BOLD,   "Mojangles-Bold.ttf",    true);
    ExtractResource(IDR_FONT_COMFORTAA,        "Comfortaa.ttf",         true);
    ExtractResource(IDR_FONT_COMFORTAA_BOLD,   "Comfortaa-Bold.ttf",    true);
    ExtractResource(IDR_FONT_MANROPE_BOLD,     "Manrope-Bold.ttf",     true);
    ExtractResource(IDR_FONT_BANGERS,          "Bangers-Regular.ttf",  true);
    ExtractResource(IDR_FONT_GEIST_LIGHT,      "Geist-Light.ttf",      true);
    ExtractResource(IDR_FONT_GEIST_BOLD,       "Geist-Bold.ttf",       true);
    ExtractResource(IDR_FONT_MANROPE_REG,      "Manrope-Regular.ttf",  true);
    // Legacy id still maps to bold Product Sans for older extractors.
    ExtractResource(IDR_FONT_PRODUCTSANS,      "Product-Sans-Bold.ttf", true);

}