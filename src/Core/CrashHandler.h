#pragma once
#include <string>

// ---------------------------------------------------------------------------
// CrashHandler — turns a silent process death into a readable log entry.
//
// Why this exists: when the client faults on a game thread there is no console,
// no dialog and nothing in the log — the process simply disappears, and the only
// evidence is that the last line written was whatever happened to run before the
// fault. Diagnosing that means guessing. This installs a top-level handler that,
// on any unhandled fault, writes:
//
//   * the exception code and faulting address,
//   * whether the address lands inside our DLL or in the game (and the offset
//     from that module's base, which is what you paste into IDA),
//   * a backtrace with the same module+offset treatment per frame,
//   * the breadcrumb — which module/hook was executing at the time.
//
// The breadcrumb is the part that usually answers the question outright, since
// most faults happen inside one specific module's OnRender/OnUpdate.
//
// A minidump is written next to the log when DbgHelp is available, so a full
// stack with locals can be opened later in a debugger.
// ---------------------------------------------------------------------------
namespace CrashHandler {

// Install the vectored/unhandled exception filter. Safe to call twice.
void Initialize();

// Remove the filter (on unload) so a later fault is not reported against a DLL
// that is no longer mapped.
void Shutdown();

// Breadcrumb: the innermost thing currently executing. Cheap enough (one pointer
// store to a thread-local) to set around every module callback.
void SetContext(const char* what);
void ClearContext();

inline thread_local int t_probeDepth = 0;

inline void EnterProbe() { ++t_probeDepth; }
inline void LeaveProbe() { --t_probeDepth; }
inline bool InProbe()    { return t_probeDepth > 0; }

// RAII form of SetContext/ClearContext.
struct Scope {
    explicit Scope(const char* what) { SetContext(what); }
    ~Scope() { ClearContext(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace CrashHandler
