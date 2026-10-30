#pragma once

// Publishes SigManager addresses into the g_* symbols that SDK headers
// consume. Called once after signature scan. Offsets live in OffsetManager.
namespace SDKRuntime {
    void Publish();
}
