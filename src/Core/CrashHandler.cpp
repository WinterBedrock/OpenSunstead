#include "CrashHandler.h"
#include "Logger.h"
#include "ConfigManager.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <cstdio>
#include <ctime>

#pragma comment(lib, "DbgHelp.lib")

namespace {

PVOID  g_vehHandle = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
HMODULE g_selfModule = nullptr;
uintptr_t g_selfBase = 0, g_selfEnd = 0;
volatile LONG g_reported = 0;   // report once; a fault while reporting must not loop

// Innermost thing executing on this thread. Thread-local so a render-thread
// fault does not get blamed on whatever the game thread was doing.
thread_local const char* t_context = nullptr;

const char* codeName(DWORD c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
    case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case 0xE06D7363:                      return "C++ exception (unhandled)";
    default:                              return "unknown";
    }
}

// "Sunstead.dll+0x1234" / "Minecraft.Windows.exe+0x4F7AC9" — the form that can be
// pasted straight into a disassembler.
std::string describeAddress(uintptr_t addr) {
    char buf[256];
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) && mod) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* name = strrchr(path, '\\');
        name = name ? name + 1 : path;
        std::snprintf(buf, sizeof(buf), "%s+0x%llX", name,
                      (unsigned long long)(addr - (uintptr_t)mod));
    } else {
        std::snprintf(buf, sizeof(buf), "0x%llX <no module>", (unsigned long long)addr);
    }
    return buf;
}

void writeMinidump(EXCEPTION_POINTERS* ep) {
    std::string dir = ConfigManager::GetSunsteadDir();
    char stamp[64];
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &now);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    const std::string path = dir + "crash-" + stamp + ".dmp";

    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;

    const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory |
                                               MiniDumpScanMemory |
                                               MiniDumpWithThreadInfo);
    if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type,
                          &mei, nullptr, nullptr)) {
        LOG_ERROR("minidump written: " + path, "Crash");
    }
    CloseHandle(f);
}

// One terse line for a fault that some __except downstream is going to swallow.
// Emitted at WARN so it is compiled out of release builds entirely: it is a
// development diagnostic, not a crash. Deliberately does NOT write a minidump
// and does NOT latch g_reported — the process is not dying, and consuming the
// report-once flag here is exactly what used to silence the real crash that
// followed.
void reportNonFatal(EXCEPTION_POINTERS* ep) {
    // Rate-limited: a fault inside a per-frame path would otherwise flood the log
    // faster than anything else in it.
    static volatile LONG shown = 0;
    const LONG n = InterlockedIncrement(&shown);
    if (n > 12) return;

    const uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    char line[512];
    std::snprintf(line, sizeof(line),
                  "non-fatal %s at %s (handled downstream)%s%s%s",
                  codeName(ep->ExceptionRecord->ExceptionCode),
                  describeAddress(at).c_str(),
                  t_context ? " during " : "",
                  t_context ? t_context : "",
                  n == 12 ? " [further reports suppressed]" : "");
    LOG_WARN(line, "Crash");
}

void report(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;

    char line[512];
    std::snprintf(line, sizeof(line), "=== CRASH === %s (0x%08lX) at %s",
                  codeName(code), (unsigned long)code, describeAddress(at).c_str());
    LOG_ERROR(line, "Crash");

    // Whether the faulting instruction is ours is the first thing worth knowing:
    // it separates "our bug" from "we handed the game bad data and it faulted".
    const bool inSelf = (at >= g_selfBase && at < g_selfEnd);
    LOG_ERROR(inSelf ? "fault is INSIDE Sunstead.dll"
                     : "fault is OUTSIDE Sunstead.dll (in the game or a system DLL)",
              "Crash");

    if (t_context) {
        LOG_ERROR(std::string("executing at the time: ") + t_context, "Crash");
    } else {
        LOG_ERROR("executing at the time: <no breadcrumb — not inside a guarded callback>",
                  "Crash");
    }

    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
        const ULONG_PTR addr = ep->ExceptionRecord->ExceptionInformation[1];
        std::snprintf(line, sizeof(line), "  %s address 0x%llX",
                      kind == 0 ? "read from" : (kind == 1 ? "write to" : "execute at"),
                      (unsigned long long)addr);
        LOG_ERROR(line, "Crash");
    }

    // Backtrace. CaptureStackBackTrace is used rather than StackWalk64 because it
    // needs no symbol handler and cannot itself fault on a corrupted stack.
    void* frames[32] = {};
    const USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
    LOG_ERROR("backtrace:", "Crash");
    for (USHORT i = 0; i < n; ++i) {
        std::snprintf(line, sizeof(line), "  [%02u] %s", i,
                      describeAddress((uintptr_t)frames[i]).c_str());
        LOG_ERROR(line, "Crash");
    }

    writeMinidump(ep);
    LOG_ERROR("=== END CRASH REPORT ===", "Crash");
}

// Only faults we plausibly caused are worth reporting. The game raises plenty of
// first-chance exceptions in normal operation (C++ throws, guard-page hits from
// its own allocator), and reporting those would bury the real one.
bool worthReporting(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return true;
    default:
        return false;
    }
}

// A vectored handler runs FIRST-CHANCE: before any __except downstream has had
// the chance to say "I've got this". So at this point it is fundamentally
// unknowable whether a fault is fatal. This handler therefore never claims a
// crash — it only logs, and only when the fault is not an expected one.
//
// The authoritative "this really killed us" signal is unhandledFilter below,
// which by definition runs only after the whole SEH chain declined.
LONG CALLBACK vehHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!worthReporting(code)) return EXCEPTION_CONTINUE_SEARCH;

    // A pointer probe faulting is the probe WORKING. sdk_safe_ptr answers "is
    // this game pointer readable?" by dereferencing it and catching the fault,
    // so an access violation here carries no information about a crash at all.
    // This single check is what stops opening the ClickGUI from being reported
    // as a crash: MouseDevice::get() probes a not-yet-populated pointer, the
    // probe faults by design, and every layer above recovers correctly.
    if (CrashHandler::InProbe()) return EXCEPTION_CONTINUE_SEARCH;

    // Faults outside our DLL are the game's business. It raises plenty of its
    // own first-chance exceptions (guard pages, C++ throws) and handles them.
    const uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    if (at < g_selfBase || at >= g_selfEnd) return EXCEPTION_CONTINUE_SEARCH;

    // A fault in our code that is NOT a probe is worth knowing about — the SEH
    // guards around module callbacks will swallow it and the module will simply
    // stop working with no explanation. But it is logged as non-fatal, without a
    // minidump, and crucially without latching g_reported: if this does turn out
    // to be fatal, unhandledFilter still gets to write the full report.
    reportNonFatal(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
    // The entire SEH chain declined this one, so the process IS going down. This
    // is the only place a crash can be diagnosed with certainty. Report even when
    // the fault was outside our DLL — at this point it is the only record.
    if (ep && ep->ExceptionRecord && InterlockedExchange(&g_reported, 1) == 0)
        report(ep);
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void CrashHandler::Initialize() {
    if (g_vehHandle) return;

    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&CrashHandler::Initialize, &g_selfModule);
    if (g_selfModule) {
        g_selfBase = (uintptr_t)g_selfModule;
        // Image size is read straight from the PE headers rather than via
        // GetModuleInformation, so this does not pull in psapi.
        auto* dos = (IMAGE_DOS_HEADER*)g_selfModule;
        auto* nt  = (IMAGE_NT_HEADERS*)((uintptr_t)dos + dos->e_lfanew);
        g_selfEnd = g_selfBase + nt->OptionalHeader.SizeOfImage;
    }

    g_vehHandle  = AddVectoredExceptionHandler(1, vehHandler);
    g_prevFilter = SetUnhandledExceptionFilter(unhandledFilter);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "crash reporting active (dll 0x%llX..0x%llX)",
                  (unsigned long long)g_selfBase, (unsigned long long)g_selfEnd);
    LOG_INFO(buf, "Crash");
}

void CrashHandler::Shutdown() {
    if (g_vehHandle) {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }
    SetUnhandledExceptionFilter(g_prevFilter);
    g_prevFilter = nullptr;
}

void CrashHandler::SetContext(const char* what) { t_context = what; }
void CrashHandler::ClearContext() { t_context = nullptr; }
