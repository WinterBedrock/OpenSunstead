#pragma once
#include <Windows.h>

// Central hooking engine built on SafetyHook
namespace HookEngine {

bool Create(void* target, void* detour, void** original);
bool Create(void* target, void* detour, void** original, bool startEnabled);

bool IsHooked(void* target);

bool Enable(void* target);
bool Disable(void* target);
void Remove(void* target);

void ShutdownAll();
}
