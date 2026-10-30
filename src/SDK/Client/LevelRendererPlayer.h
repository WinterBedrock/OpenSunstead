#pragma once
// Camera state belongs to the raw renderer field, not the CI renderer-helper virtual.
#include "../Types.h"
#include <Windows.h>

namespace SDK {

class ClientInstance;
class LevelRenderer;
class LevelRendererPlayer;

// Resolve the LevelRendererPlayer via the FIELD path
// (CI+0x1E0 -> LevelRenderer -> +0x480), SEH-guarded and probed.
// Returns nullptr when any link is stale/invalid.
LevelRendererPlayer* sehGetLevelRendererPlayer();

// The FOV focal-scale fields (projection [0] at +0xF50 and [5] at +0xF64).
// Out-params receive direct pointers into the live object. Returns false when
// the chain is bad. Both FOV modules (Zoom, Sprint) go through this so the
// probe rules and offsets live once.
bool sehGetFovFieldPtrs(float** fovXOut, float** fovYOut);

// Copy the LevelRendererPlayer's stored view/projection matrices (the ones
// the world render projects with — includes any module FOV scaling).
// Returns false when the LRP is unreachable or the matrices are not finite.
bool sehReadLrpMatrices(float viewOut[16], float projOut[16]);

// Live FOV focal scales (projection [0] / [5]). Used by world-to-screen to
// rescale a captured MVP when Zoom/Sprint wrote the fields after the capture.
bool sehReadFovValues(float* fovXOut, float* fovYOut);

// FOV module-scaling — ONE writer, called from the renderLevel hook BEFORE
// the world render. Writes RELATIVELY: divides out the scale it applied last
// time (restoring the game's current value — animation included — as the
// base) and multiplies in the new total. If the fields no longer match what
// we last wrote, the game replaced them and the divide-out is skipped, so this
// stays correct whether or not render-prep rewrites the fields each frame.
bool sehApplyFovMod(float total);

// The total FOV scale most recently applied by sehApplyFovMod (1.0 = none).
float sehGetAppliedFovMod();

} // namespace SDK
