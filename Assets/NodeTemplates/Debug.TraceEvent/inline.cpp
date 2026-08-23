if (Initialized < 0.5f) {
    if (platform_trace_event(static_cast<uint8_t>(Channel), Value, true)) {
        LastValue = Value;
        Initialized = 1.0f;
    }
} else if (Value != LastValue) {
    if (platform_trace_event(static_cast<uint8_t>(Channel), Value, false)) {
        LastValue = Value;
    }
}
