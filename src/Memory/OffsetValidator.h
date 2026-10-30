#pragma once

// Resolved-offsets report ("offset scanner").
//
// The signature scanner proves that CODE addresses still exist; this is the
// equivalent for DATA offsets — a one-shot dump of every OffsetManager value
// the SDK is actually dereferencing, in the same style as the signature
// report ("Actor component ptrs: stateVector=0x218 aabb=0x220 rotation=0x228").
//
// This deliberately does NOT try to judge whether an offset is "plausible".
// An earlier revision probed live objects per frame and logged FAILs, which
// was actively misleading: it fired on the loading screen before
// ClientInstance existed, set a one-shot reported flag, and then never logged
// the in-game state it was supposed to be checking. A dump of what the values
// actually are is more useful than a guess about whether they look right, and
// it cannot itself crash anything.
//
// Two entry points, both one-shot and both retrying until their object exists:
//   Validate()             — game object graph, from the Present hook.
//   ValidateScreenView()   — UI tree, from the ScreenView::setupAndRender hook
//                            (the only place a live ScreenView is in hand).
class SDKScreenView;
class OffsetValidator {
public:
    static OffsetValidator& GetInstance() {
        static OffsetValidator instance;
        return instance;
    }

    // Logs every game-side OffsetManager value once ClientInstance is live.
    // Returns -1 while still on the loading screen (caller retries); 0 after.
    int Validate();

    // Logs the ScreenView/VisualTree/UIControl offsets once a live view is in
    // hand, plus the screen-name round-trip that proves the whole chain.
    // Returns false while the UI is not ready (loading-screen views).
    bool ValidateScreenView(void* view);

    // True after a report has been logged at least once.
    bool HasReport() const;

private:
    OffsetValidator() = default;
};
