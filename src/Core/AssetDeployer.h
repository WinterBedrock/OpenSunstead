#pragma once
// Extracts bundled assets (compiled into the DLL as RCDATA) onto disk in the
// Minecraft client RoamingState folder on first inject, so users who never had
// the files still get them.
namespace AssetDeployer {
    void DeployAll();
    // Extract a single bundled RCDATA resource to %APPDATA%/Sunstead/assets/<outName>.
    bool Extract(int resId, const char* outName, bool overwrite);
}