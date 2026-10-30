#pragma once

// Silent rotation channel.
namespace SilentRotation {
    // Continuous injection: keep applying (yaw,pitch) every packet until Clear()
    void SetContinuous(float yaw, float pitch);

    // One-shot injection: applied to exactly ONE outgoing packet then auto-cleared by the send hook
    void SetOnce(float yaw, float pitch);

    // Smoothing factor in (0,1]. Each Consume() eases the injected rotation from
    // its current value toward the target by this fraction (shortest-angle for
    // yaw), giving a smooth Solstice-style head turn instead of an instant snap.
    // 1.0 = snap (no smoothing). Persist across ticks; set once from the module.
    void SetSmoothing(float factor);

    // Clear the continuous latch. A pending one-shot (SetOnce) survives unless
    // force==true, so it can still land in the next outgoing packet.
    void Clear(bool force = false);

    // Read the pending rotation. Returns true if an injection is active. If the
    // active injection was a one-shot it is consumed (cleared) by this call.
    bool Consume(float& yaw, float& pitch);

    // Non-consuming read of the current target. Returns true if an injection is
    // active WITHOUT clearing it. Used by the third-person visible-rotation
    // override (render thread) so it can lerp the body/head toward the same aim
    // the packet hook injects, without stealing the one-shot from the sender.
    bool Peek(float& yaw, float& pitch);

    // Movement-frame restatement for the packet hook: when a silent aim is
    // active AND the player is walking, the outgoing move vectors + direction
    // bits must be re-expressed in the SPOOFED yaw frame (same trick as
    // Baritone's sehApplyBaritoneMove). Otherwise the server integrates the
    // direction bits against the spoofed yaw, lands elsewhere, and either
    // corrects the player or flags the "running backwards" mismatch.
    // Publish from the game thread next to SetContinuous/SetOnce; the send
    // hook consumes it right after applying the rotation. Pass the player's
    // REAL yaw (the one the client integrated movement against).
    void PublishMoveFrame(float realYaw);
    // Live WASD state the client integrated movement against this tick
    // (fwd: +1 W / -1 S, strafe: +1 D / -1 A). Consumed with the frame above.
    void SetMoveInput(int fwd, int strafe);
    // Consume the published frame. Returns true when a restatement is pending
    // AND an injection is active; outputs forward/strafe in the spoofed frame.
    bool ConsumeMoveFrame(float& forward, float& strafe);
}
