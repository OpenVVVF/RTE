#pragma once

#include <cmath>
#include <cstdint>

namespace Inverter {
// Encoder-DMA-owned velocity estimate. Real timestamps avoid assuming a trigger
// rate; a 2 ms angle window followed by a 10 ms filter rejects sample noise
// without the dashboard RPM estimator's approximately one-second lag.
class ControlSpeedEstimator {
public:
    void reset() { *this = {}; }
    float update(float angle_deg, uint32_t cycles, uint32_t clock_hz) {
        if (!std::isfinite(angle_deg) || clock_hz == 0U) {
            reset();
            return 0.0f;
        }
        if (!initialized_ || uint32_t(cycles - last_cycles_) > clock_hz / 100U) {
            reset();
            initialized_ = true;
            previous_deg_ = angle_deg;
            last_cycles_ = window_cycles_ = cycles;
            return 0.0f;
        }
        if (cycles == last_cycles_) return rpm_;
        float delta = angle_deg - previous_deg_;
        if (delta > 180.0f) delta -= 360.0f;
        else if (delta < -180.0f) delta += 360.0f;
        previous_deg_ = angle_deg;
        last_cycles_ = cycles;
        window_degrees_ += delta;
        const uint32_t elapsed = cycles - window_cycles_;
        if (elapsed >= clock_hz / 500U) {
            const float dt = float(elapsed) / float(clock_hz);
            const float measured = window_degrees_ / (6.0f * dt);
            const float alpha = dt / (0.010f + dt);
            rpm_ = ready_ ? rpm_ + alpha * (measured - rpm_) : measured;
            ready_ = true;
            window_degrees_ = 0.0f;
            window_cycles_ = cycles;
        }
        return rpm_;
    }
    float rpm() const { return rpm_; }

private:
    bool initialized_ = false, ready_ = false;
    uint32_t last_cycles_ = 0, window_cycles_ = 0;
    float previous_deg_ = 0.0f, window_degrees_ = 0.0f, rpm_ = 0.0f;
};
} // namespace Inverter
