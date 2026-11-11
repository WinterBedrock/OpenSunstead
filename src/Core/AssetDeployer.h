#pragma once
namespace AssetDeployer {
    void DeployAll();
    // Extract a single bundled RCDATA resource to %APPDATA%/Sunstead/assets/<outName>.
    bool Extract(int resId, const char* outName, bool overwrite);
}