#pragma once
#include <Windows.h>

class HookManager {
public:
    static HookManager& GetInstance() {
        static HookManager instance;
        return instance;
    }

    void Initialize();
    void Shutdown();

    bool isHooked = false;

private:
    HookManager() = default;
};
