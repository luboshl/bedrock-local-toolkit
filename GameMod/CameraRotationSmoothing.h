#pragma once

#include <cmath>

namespace zoom
{
    struct TurnDelta
    {
        double yaw;
        double pitch;
    };

    // One instance per active mouse-look stream. Reset at every gameplay interruption.
    class CameraRotationSmoothing
    {
    public:
        void Reset() { pending = {}; }

        // Called on camera updates, including updates with zero input.
        // elapsedSeconds is the monotonic interval since the previous update.
        TurnDelta Update(TurnDelta input, double elapsedSeconds, double strength)
        {
            if (!std::isfinite(elapsedSeconds) || elapsedSeconds < 0 || elapsedSeconds > 0.25 ||
                !(strength >= 1 && strength <= 10) ||
                !std::isfinite(input.yaw) || !std::isfinite(input.pitch))
            {
                Reset();
                return input;
            }

            pending.yaw += input.yaw;
            pending.pitch += input.pitch;
            // Strength 1 to 10 corresponds to a time constant of 4 to 40 ms.
            const double fraction = -std::expm1(-elapsedSeconds / (0.004 * strength));
            const TurnDelta output{pending.yaw * fraction, pending.pitch * fraction};
            pending.yaw -= output.yaw;
            pending.pitch -= output.pitch;
            return output;
        }

    private:
        TurnDelta pending{};
    };
}
