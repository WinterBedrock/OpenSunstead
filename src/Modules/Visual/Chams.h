#pragma once
#include "../Module.h"
#include <imgui.h>
#include <d3d12.h>

class Chams : public Module {
public:
    Chams() : Module("Chams", "All actors rendered through walls", Category::Visual, 0) {
        AddSetting(new EnumSetting("Style", "Through walls render style", 0,
                                   { "Solid", "Wireframe", "Test" }));               // [0]
        AddSetting(new BoolSetting("Debug", "Log draw/substitution counters", false));                                  // [1]
    }
    void OnEnable() override;
    void OnDisable() override;

    static void InstallHook();
    static void UninstallHook();
    static void InjectFreeCamGhost(void* self_ard, void* barc, __int64 cameraPos, void* skipActor);
    static void InjectFreeCamGhostCached();
    static void EnsureDeviceHooks();
    static void NoteCommandList(ID3D12CommandList* list);

    static int CurrentStyle();   // 0 solid, 1 wireframe, 2 test
    static bool ShowDebug();
};
