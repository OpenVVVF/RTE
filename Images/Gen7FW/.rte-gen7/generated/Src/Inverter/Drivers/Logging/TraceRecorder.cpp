#include "Inverter/Drivers/Logging/TraceRecorder.h"

#include "Inverter/Drivers/CAN/CanBus.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"
#include "inverter_protocol/trace_protocol.h"
#include "main.h"

#include <cmath>
#include <cstring>

namespace Inverter {
namespace {
TraceRecorder s_recorder __attribute__((section(".dma_buffers")));
TraceRecorder::Sample s_samples[TraceRecorder::SAMPLE_RING]
    __attribute__((section(".trace_buffers")));

float kvOr(const char* key, float fallback) {
    float value = fallback;
    if (RteParamStore::isReady()) RteParamStore::get(key, &value);
    return value;
}
} // namespace

TraceRecorder& traceRecorder() { return s_recorder; }

void TraceRecorder::resetCapture() {
    m_head = 0;
    m_tail = 0;
    m_next_sample_sequence = 0;
    m_samples_captured = 0;
    m_samples_dropped = 0;
    m_block_sequence = 0;
    m_event_sequence = 0;
    m_frames_dropped = 0;
    m_schema_next = 0;
    m_last_status_ms = HAL_GetTick();
}

void TraceRecorder::init() {
    /* Both storage sections are NOLOAD; initialize all validity state here. */
    m_enabled = false;
    m_running = false;
    m_bus = 0;
    m_id_base = 0x680;
    m_capture_id = 0;
    for (uint8_t i = 0; i < MAX_CHANNELS; ++i) {
        std::memset(m_names[i], 0, sizeof(m_names[i]));
        m_registered[i] = false;
        if (i < CHANNELS) m_scales[i] = 1.0f;
    }
    resetCapture();

    if (kvOr("Can.Trace.En", 0.0f) == 0.0f) return;
    const float bus = kvOr("Can.Trace.Bus", 1.0f);
    const float id_base = kvOr("Can.Trace.IdBase", 1664.0f);
    if ((bus != 1.0f && bus != 2.0f) || id_base < 0.0f || id_base > 2044.0f ||
        id_base != static_cast<float>(static_cast<uint16_t>(id_base))) {
        Telemetry::printf("[TRACE] invalid Can.Trace.Bus or Can.Trace.IdBase");
        return;
    }
    m_bus = static_cast<uint8_t>(bus) - 1U;
    m_id_base = static_cast<uint16_t>(id_base);
    if (!canBus().fdEnabled(m_bus)) {
        Telemetry::printf("[TRACE] bus %u is disabled or not configured for FD",
                          static_cast<unsigned>(m_bus + 1U));
        return;
    }
    m_enabled = true;
    if (kvOr("Can.Trace.AutoStart", 1.0f) != 0.0f) start();
}

void TraceRecorder::start() {
    if (!m_enabled || m_running) return;
    ++m_capture_id;
    resetCapture();
    __DMB();
    m_running = true;
    Telemetry::printf("[TRACE] capture %u started on CAN bus %u ids 0x%03X-0x%03X",
                      m_capture_id, static_cast<unsigned>(m_bus + 1U),
                      m_id_base, static_cast<unsigned>(m_id_base + 3U));
}

void TraceRecorder::stop() {
    m_running = false;
    __DMB();
    if (m_enabled) sendStatus();
}

void TraceRecorder::configure8(const char* const names[CHANNELS],
                               const float scales[CHANNELS]) {
    for (uint8_t i = 0; i < CHANNELS; ++i) {
        std::memset(m_names[i], 0, sizeof(m_names[i]));
        if (names != nullptr && names[i] != nullptr) {
            std::strncpy(m_names[i], names[i], sizeof(m_names[i]) - 1U);
        }
        const float scale = scales == nullptr ? 1.0f : scales[i];
        m_scales[i] = std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
        m_registered[i] = true;
    }
    m_schema_next = 0;
}

bool TraceRecorder::registerEventChannel(uint8_t channel, const char* name) {
    if (channel < CHANNELS || channel >= MAX_CHANNELS || name == nullptr) return false;
    std::memset(m_names[channel], 0, sizeof(m_names[channel]));
    std::strncpy(m_names[channel], name, sizeof(m_names[channel]) - 1U);
    m_registered[channel] = true;
    if (channel < m_schema_next) m_schema_next = channel;
    return true;
}

void TraceRecorder::capture8(float value0, float value1, float value2, float value3,
                             float value4, float value5, float value6, float value7) {
    if (!m_running) return;
    const uint32_t sequence = m_next_sample_sequence++;
    const uint16_t head = m_head;
    const uint16_t next = static_cast<uint16_t>((head + 1U) % SAMPLE_RING);
    if (next == m_tail) {
        ++m_samples_dropped;
        return;
    }
    Sample& sample = s_samples[head];
    sample.sequence = sequence;
    sample.cycles = DWT->CYCCNT;
    sample.values[0] = value0;
    sample.values[1] = value1;
    sample.values[2] = value2;
    sample.values[3] = value3;
    sample.values[4] = value4;
    sample.values[5] = value5;
    sample.values[6] = value6;
    sample.values[7] = value7;
    __DMB();
    m_head = next;
    ++m_samples_captured;
}

int16_t TraceRecorder::quantize(float value, float scale) {
    if (!std::isfinite(value)) return 0;
    const float scaled = value / scale;
    if (scaled >= 32767.0f) return 32767;
    if (scaled <= -32768.0f) return -32768;
    return static_cast<int16_t>(std::lround(scaled));
}

bool TraceRecorder::sendSchemas() {
    while (m_schema_next < MAX_CHANNELS) {
        if (!m_registered[m_schema_next]) {
            ++m_schema_next;
            continue;
        }
        ivp_trace_schema_frame_t schema{};
        schema.capture_id = m_capture_id;
        schema.channel = m_schema_next;
        schema.scale = m_schema_next < CHANNELS ? m_scales[m_schema_next] : 1.0f;
        std::memcpy(schema.name, m_names[m_schema_next], sizeof(schema.name));
        uint8_t payload[IVP_TRACE_PAYLOAD_SIZE];
        ivp_trace_encode_schema(&schema, payload);
        if (!canBus().sendFd(m_bus, m_id_base + 1U, payload, sizeof(payload))) return false;
        ++m_schema_next;
    }
    return true;
}

bool TraceRecorder::sendStatus() {
    ivp_trace_status_frame_t status{};
    status.capture_id = m_capture_id;
    status.samples_captured = m_samples_captured;
    status.samples_dropped = m_samples_dropped;
    status.frames_dropped = m_frames_dropped;
    uint8_t payload[IVP_TRACE_PAYLOAD_SIZE];
    ivp_trace_encode_status(&status, payload);
    return canBus().sendFd(m_bus, m_id_base + 3U, payload, sizeof(payload));
}

bool TraceRecorder::publishEvent(uint8_t channel, float value, bool snapshot) {
    if (!m_running || channel >= MAX_CHANNELS || !m_registered[channel]) return false;
    /* Constructors register all sparse channels before the first app step.
     * Queue their schemas ahead of the first snapshot/event. */
    if (!sendSchemas()) return false;
    ivp_trace_event_frame_t event{};
    event.capture_id = m_capture_id;
    event.channel = channel;
    event.snapshot = snapshot;
    event.sample_sequence = m_next_sample_sequence;
    event.event_sequence = m_event_sequence++;
    event.value = value;
    uint8_t payload[IVP_TRACE_PAYLOAD_SIZE];
    ivp_trace_encode_event(&event, payload);
    if (!canBus().sendFd(m_bus, m_id_base + 2U, payload, sizeof(payload))) {
        ++m_frames_dropped;
        return false;
    }
    return true;
}

void TraceRecorder::update() {
    if (!m_running) return;
    if (!sendSchemas()) return;

    /* Bound foreground work. At 5 kHz / 100 Hz the expected batch is 50;
     * allowing 126 samples gives recovery headroom after a slow loop pass. */
    for (uint8_t block = 0; block < 42U; ++block) {
        const uint16_t tail0 = m_tail;
        const uint16_t tail1 = static_cast<uint16_t>((tail0 + 1U) % SAMPLE_RING);
        const uint16_t tail2 = static_cast<uint16_t>((tail1 + 1U) % SAMPLE_RING);
        if (tail0 == m_head || tail1 == m_head || tail2 == m_head) break;
        __DMB();
        const Sample a = s_samples[tail0];
        const Sample b = s_samples[tail1];
        const Sample c = s_samples[tail2];
        if (b.sequence != a.sequence + 1U || c.sequence != b.sequence + 1U) {
            /* Drop only the incomplete edge of a gap.  The next data frame's
             * first sequence makes the loss explicit to the host. */
            m_tail = tail1;
            continue;
        }
        ivp_trace_data_frame_t frame{};
        frame.capture_id = m_capture_id;
        frame.first_sample_sequence = a.sequence;
        frame.first_sample_cycles = a.cycles;
        const uint32_t delta1 = (b.cycles - a.cycles + 4U) / 8U;
        const uint32_t delta2 = (c.cycles - b.cycles + 4U) / 8U;
        frame.delta_cycles_div8[0] = static_cast<uint16_t>(
            delta1 > 65535U ? 65535U : delta1);
        frame.delta_cycles_div8[1] = static_cast<uint16_t>(
            delta2 > 65535U ? 65535U : delta2);
        const Sample samples[3] = {a, b, c};
        for (uint8_t s = 0; s < 3U; ++s)
            for (uint8_t ch = 0; ch < CHANNELS; ++ch)
                frame.samples[s][ch] = quantize(samples[s].values[ch], m_scales[ch]);
        uint8_t payload[IVP_TRACE_PAYLOAD_SIZE];
        ivp_trace_encode_data(&frame, payload);
        if (!canBus().sendFd(m_bus, m_id_base, payload, sizeof(payload))) {
            break;
        }
        ++m_block_sequence;
        __DMB();
        m_tail = static_cast<uint16_t>((tail2 + 1U) % SAMPLE_RING);
    }

    const uint32_t now = HAL_GetTick();
    if (now - m_last_status_ms >= 1000U) {
        if (sendStatus()) m_last_status_ms = now;
    }
}

void TraceRecorder::printStatus() const {
    Telemetry::printf("[TRACE] enabled=%d running=%d bus=%u id=0x%03X capture=%u samples=%lu dropped=%lu blocks=%lu frame_drop=%lu",
                      m_enabled ? 1 : 0, m_running ? 1 : 0,
                      static_cast<unsigned>(m_bus + 1U), m_id_base, m_capture_id,
                      static_cast<unsigned long>(m_samples_captured),
                      static_cast<unsigned long>(m_samples_dropped),
                      static_cast<unsigned long>(m_block_sequence),
                      static_cast<unsigned long>(m_frames_dropped));
}

} // namespace Inverter
