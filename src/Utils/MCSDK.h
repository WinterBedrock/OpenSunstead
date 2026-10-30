#pragma once
// Backward-compat wrapper
#include "../SDK/SDK.h"
namespace SDK { inline bool isSafeReadPtr(void* ptr, size_t size) { return sdk_safe_ptr(ptr, size); } }