#pragma once
// Camera state belongs to the raw renderer field, not the CI renderer-helper virtual.
#include "../Types.h"
#include <Windows.h>

namespace SDK {

class ClientInstance;
class LevelRenderer;
class LevelRendererPlayer;

LevelRendererPlayer* sehGetLevelRendererPlayer();

bool sehGetFovFieldPtrs(float** fovXOut, float** fovYOut);

bool sehReadLrpMatrices(float viewOut[16], float projOut[16]);

// Live FOV focal scales (projection [0] / [5]). Used by world-to-screen to
// rescale a captured MVP when Zoom/Sprint wrote the fields after the capture.
bool sehReadFovValues(float* fovXOut, float* fovYOut);

bool sehApplyFovMod(float total);

// The total FOV scale most recently applied by sehApplyFovMod (1.0 = none).
float sehGetAppliedFovMod();

} // namespace SDK
