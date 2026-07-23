const float counts_per_rad = counts_per_rev / 6.28318530718f;
const float dt_s = sample_time_s;

theta = static_cast<float>(raw_counts) / counts_per_rad;

const float delta = static_cast<float>(raw_counts) - prev_counts;
omega = rte::RadiansPerSecond(delta / (counts_per_rad * dt_s));
prev_counts = static_cast<float>(raw_counts);
