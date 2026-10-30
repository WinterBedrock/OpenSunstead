#pragma once
#include "../Module.h"
#include <Windows.h>
#include <vector>
#include <cstddef>

// Anchor — real SafeWalk: MoveRequest edge clamp without crouch spam.
// Sites resolved by SigManager (wildcards on displacements / relocs).
class Anchor : public Module {
public:
    Anchor() : Module("Anchor", "Safe walk — edge clip without holding sneak", Category::Movement, 0) {}

    void OnEnable() override;
    void OnDisable() override;
    void OnUpdate() override;

private:
    struct PatchSite {
        uintptr_t addr = 0;
        std::vector<unsigned char> og;
        std::vector<unsigned char> patch;
    };

    PatchSite mPrimary;
    PatchSite mAlt;
    bool mPatched = false;

    bool findAndApplyPatches();
    void restorePatches();
    void reassertPatches();
    static bool writeBytes(uintptr_t addr, const void* data, size_t len);
    static bool sehReadN(uintptr_t addr, unsigned char* out, size_t n);
};
