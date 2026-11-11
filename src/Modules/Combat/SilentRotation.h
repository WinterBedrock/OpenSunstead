#pragma once

// Silent rotation channel.
namespace SilentRotation {
    // Continuous injection: keep applying (yaw,pitch) every packet until Clear()
    void SetContinuous(float yaw, float pitch);

    // One-shot injection: applied to exactly ONE outgoing packet then auto-cleared by the send hook
    void SetOnce(float yaw, float pitch);

    void SetSmoothing(float factor);

    // Clear the continuous latch. A pending one-shot (SetOnce) survives unless
    // force==true, so it can still land in the next outgoing packet.
    void Clear(bool force = false);

    // Read the pending rotation. Returns true if an injection is active. If the
    // active injection was a one-shot it is consumed (cleared) by this call.
    bool Consume(float& yaw, float& pitch);

    bool Peek(float& yaw, float& pitch);

    void PublishMoveFrame(float realYaw);
    // Live WASD state the client integrated movement against this tick
    // (fwd: +1 W / -1 S, strafe: +1 D / -1 A). Consumed with the frame above.
    void SetMoveInput(int fwd, int strafe);
    // Consume the published frame. Returns true when a restatement is pending
    // AND an injection is active; outputs forward/strafe in the spoofed frame.
    bool ConsumeMoveFrame(float& forward, float& strafe);
}
