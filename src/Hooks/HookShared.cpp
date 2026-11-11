#include "HookShared.h"
#include <string>

namespace gh {
namespace {
    std::atomic<bool> s_hooksReady{ false };
}

bool HooksInstalled() { return s_hooksReady.load(std::memory_order_acquire); }
void SetHooksInstalled() { s_hooksReady.store(true, std::memory_order_release); }

bool isExecutablePtr(void* ptr) {
    if (!ptr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
}
