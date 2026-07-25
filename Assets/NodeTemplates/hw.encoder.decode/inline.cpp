const float counts_per_rad = CountsPerRev / 6.28318530718f;
const float dt_s = SampleTime;

Theta = static_cast<float>(RawCounts) / counts_per_rad;

const float delta = static_cast<float>(RawCounts) - prev_counts;
Omega = rte::RadiansPerSecond(delta / (counts_per_rad * dt_s));
prev_counts = static_cast<float>(RawCounts);
