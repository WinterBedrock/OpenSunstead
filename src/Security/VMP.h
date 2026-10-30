#pragma once

// VMProtect SDK marker bridge (VMProtect Lite 3.10.4, SDK from ext/vmprotect).
//
// Markers are what VMProtect's GUI turns into virtualized / mutated regions
// when the built DLL is processed (see tools/VMPROTECT.md). Two states:
//
//   SUNSTEAD_VMP defined (CMake option ON, the default):
//     The VMProtect* functions are imported from VMProtectSDK64.dll. CMake
//     copies that DLL next to Sunstead.dll so injection resolves it. After the
//     final DLL is processed through VMProtect, the markers are consumed and
//     the SDK dependency is no longer needed at runtime.
//
//   SUNSTEAD_VMP undefined:
//     Everything compiles to nothing — no SDK import, plain build.

#ifdef SUNSTEAD_VMP

#include "VMProtectSDK.h"

#define SUN_VMP_BEGIN_VIRTUAL(name)  VMProtectBeginVirtualization(name)
#define SUN_VMP_BEGIN_MUTATION(name) VMProtectBeginMutation(name)
#define SUN_VMP_BEGIN_ULTRA(name)    VMProtectBeginUltra(name)
#define SUN_VMP_END()                VMProtectEnd()
#define SUN_VMP_IS_PROTECTED()       VMProtectIsProtected()

#else

#define SUN_VMP_BEGIN_VIRTUAL(name)  ((void)0)
#define SUN_VMP_BEGIN_MUTATION(name) ((void)0)
#define SUN_VMP_BEGIN_ULTRA(name)    ((void)0)
#define SUN_VMP_END()                ((void)0)
#define SUN_VMP_IS_PROTECTED()       (false)

#endif
