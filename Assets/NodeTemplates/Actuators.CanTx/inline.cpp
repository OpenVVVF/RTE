/* CAN transmit, rate-limited by wall clock: sends when 1000/Rate ms have
 * elapsed since the previous send (Rate <= 0 sends every step).
 * D0..D7 = payload bytes (unconnected = 0), Dlc = how many of the 8 to
 * send, Ext: 0 std / 1 ext. */
const uint32_t period_ms = (Rate > 0.0f) ? static_cast<uint32_t>(1000.0f / Rate) : 0U;
const uint32_t now_ms = platform_millis();
if (period_ms == 0U || (now_ms - static_cast<uint32_t>(LastMs)) >= period_ms) {
    LastMs = static_cast<float>(now_ms);
    const uint8_t b[8] = {
        static_cast<uint8_t>(D0), static_cast<uint8_t>(D1),
        static_cast<uint8_t>(D2), static_cast<uint8_t>(D3),
        static_cast<uint8_t>(D4), static_cast<uint8_t>(D5),
        static_cast<uint8_t>(D6), static_cast<uint8_t>(D7),
    };
    uint8_t n = static_cast<uint8_t>(Dlc);
    if (n > 8) n = 8;
    platform_can_send(static_cast<uint8_t>(Bus), static_cast<uint32_t>(Id),
                      Ext > 0.5f, b, n);
}
