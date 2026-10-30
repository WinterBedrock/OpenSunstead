#include "HookShared.h"
#include "../Core/Logger.h"
#include <string>

namespace gh {
namespace {
    bool s_batchActive = false;
    int  s_batchQueued = 0;
    std::atomic<bool> s_hooksReady{ false };
}

MH_STATUS HookEnable(LPVOID target) {
    if (s_batchActive) {
        ++s_batchQueued;
        return MH_OK;
    }
    return MH_EnableHook(target);
}

void BeginHookBatch() { s_batchActive = true; }

int FlushHookBatch() {
    s_batchActive = false;
    const int queued = s_batchQueued;
    s_batchQueued = 0;
    if (queued <= 0) return 0;
    const MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) {
        LOG_ERROR("Batch enable failed (MH_EnableHook=" + std::to_string((int)s) +
                  ") — some hooks are NOT live", "GameHooks");
        return -1;
    }
    LOG_INFO("Batch-enabled " + std::to_string(queued) +
             " hooks in one thread freeze", "GameHooks");
    return queued;
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
