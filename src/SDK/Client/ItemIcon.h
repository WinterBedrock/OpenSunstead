#pragma once
#include <cstdint>


namespace SDK {

// True when every function this needs was resolved.
bool itemIconAvailable();

bool drawBlockIcon(void* mcuirc, void* block, float x, float y, float pixels);

void drawBlockIconImGui(void* drawList, float x, float y, float pixels,
                        const char* blockName, float alpha = 1.0f);

} // namespace SDK
