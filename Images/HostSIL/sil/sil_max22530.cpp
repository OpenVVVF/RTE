/*
 * sil_max22530.cpp — SIL replacement for Src/Inverter/Drivers/Sensors/MAX22530.cpp.
 *
 * Models the isolated 4-channel ADC: no SPI transactions, voltages arrive
 * directly from the SIL world (DC-link on channel 0, phase pole voltages on
 * channels 1..3, both through the 1516:1 sense divider).  dataReady() is
 * always true once initialized (the real chip free-runs at 20 kHz into DMA).
 *
 * The comparator windows are modeled at register-behavior level:
 * setComparatorThreshold() stores COUTHI/COUTLO counts and mirrors the
 * hardware driver's INTERRUPT_ENABLE bookkeeping (INT_EEOC plus the
 * per-channel CO_POS/CO_NEG bits); update() re-evaluates the (filtered ==
 * raw in SIL) channel voltages against the windows, sticks matching bits
 * into the latched INTERRUPT_STATUS word, and raises the same FaultManager
 * faults the hardware driver raises from its EXTI/burst path (Max22530Ov /
 * Max22530Uv on the DC-link channel).  SPI/CRC, field-loss and ADC-diagnostic
 * INTERRUPT_STATUS bits are not modelable without a bus model and stay zero.
 */
#include "Inverter/Drivers/Sensors/MAX22530.h"
#include "Inverter/Control/FaultManager.h"

#include "sil_world.h"

namespace Inverter {

MAX22530::MAX22530(SPI_HandleTypeDef* hspi,
                   GPIO_TypeDef* cs_port, uint16_t cs_pin,
                   GPIO_TypeDef* int_port, uint16_t int_pin,
                   IRQn_Type int_irqn)
    : m_hspi(hspi),
      m_cs_port(cs_port), m_cs_pin(cs_pin),
      m_int_port(int_port), m_int_pin(int_pin),
      m_int_irqn(int_irqn) {
}

namespace {

/* INTERRUPT_ENABLE / INTERRUPT_STATUS bits (same layout as the hardware
 * driver's anonymous-namespace table in MAX22530.cpp). */
constexpr uint16_t INT_CO_NEG_1 = (1U << 0);   /* channel 0 below COUTLO */
constexpr uint16_t INT_CO_POS_1 = (1U << 4);   /* channel 0 above COUTHI */
constexpr uint16_t INT_EEOC     = (1U << 12);

/* Modeled comparator registers — one chip in this design (the
 * DC-link sensor's instance), indexed by channel.  s_cout_status is the live
 * (non-latching) COUT_STATUS word; INTERRUPT_STATUS lives in the driver's
 * own m_int_status (latched, sticky until clearInterruptStatus()). */
struct ComparatorModel {
    uint16_t hi_counts = 0;
    uint16_t lo_counts = 0;
};
ComparatorModel s_comp[4];
uint16_t        s_cout_status = 0;

float hostVoltage(uint8_t channel) {
    const SilWorld& w = silWorld();
    switch (channel) {
        case 0: return w.vdc_v / 1516.0f;
        case 1: return w.phase_pole_v[0] / 1516.0f;
        case 2: return w.phase_pole_v[1] / 1516.0f;
        case 3: return w.phase_pole_v[2] / 1516.0f;
        default: return 0.0f;
    }
}
} // namespace

bool MAX22530::init() {
    m_data_ready = true;
    return true;
}

bool MAX22530::reset() { return true; }
bool MAX22530::softReset() { return true; }
bool MAX22530::clearPOR() { return true; }
bool MAX22530::clearFilter(uint8_t) { return true; }
bool MAX22530::enableCRC(bool enable) {
    m_crc_enabled = enable;
    return true;
}

bool MAX22530::readRegister(uint8_t, uint16_t& out) {
    out = 0;
    return true;
}

bool MAX22530::writeRegister(uint8_t, uint16_t) { return true; }

uint16_t MAX22530::voltageToCounts(float v) const {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.8f) v = 1.8f;
    return static_cast<uint16_t>(v / 1.8f * 4095.0f + 0.5f);
}

float MAX22530::countsToVoltage(uint16_t counts) const {
    return static_cast<float>(counts) * (1.8f / 4095.0f);
}

uint16_t MAX22530::readRawCounts(uint8_t channel) {
    return voltageToCounts(hostVoltage(channel));
}

uint16_t MAX22530::readFilteredCounts(uint8_t channel) {
    return voltageToCounts(hostVoltage(channel));
}

float MAX22530::readRawVoltage(uint8_t channel) {
    return hostVoltage(channel);
}

float MAX22530::readFilteredVoltage(uint8_t channel) {
    return hostVoltage(channel);
}

bool MAX22530::burstReadRaw(uint16_t out_counts[4], uint16_t* int_status) {
    for (uint8_t ch = 0; ch < 4; ++ch) out_counts[ch] = readRawCounts(ch);
    if (int_status != nullptr) *int_status = m_int_status;
    return true;
}

bool MAX22530::burstReadFiltered(uint16_t out_counts[4], uint16_t* int_status) {
    for (uint8_t ch = 0; ch < 4; ++ch) out_counts[ch] = readFilteredCounts(ch);
    if (int_status != nullptr) *int_status = m_int_status;
    return true;
}

bool MAX22530::setComparatorThreshold(uint8_t channel, float high_v, float low_v,
                                      bool use_filtered, bool digital_status,
                                      bool enable_pos_interrupt, bool enable_neg_interrupt) {
    if (channel > 3 || high_v < low_v) {
        return false;
    }
    (void)use_filtered;    /* raw == filtered in the SIL model */
    (void)digital_status;  /* always out-of-window digital-status semantics */

    s_comp[channel].hi_counts = voltageToCounts(high_v);
    s_comp[channel].lo_counts = voltageToCounts(low_v);

    /* Mirror the hardware driver's INTERRUPT_ENABLE bookkeeping: the channel's
     * bits are re-written from the enable args so a later call can disable a
     * direction (e.g. UV disabled while OV stays armed). */
    const uint16_t pos_bit = static_cast<uint16_t>(INT_CO_POS_1 << channel);
    const uint16_t neg_bit = static_cast<uint16_t>(INT_CO_NEG_1 << channel);
    uint16_t int_en = m_int_enable;
    int_en = static_cast<uint16_t>(int_en & ~pos_bit);
    int_en = static_cast<uint16_t>(int_en & ~neg_bit);
    int_en = static_cast<uint16_t>(int_en | INT_EEOC);
    if (enable_pos_interrupt) int_en = static_cast<uint16_t>(int_en | pos_bit);
    if (enable_neg_interrupt) int_en = static_cast<uint16_t>(int_en | neg_bit);
    m_int_enable = int_en;

    /* Clear any comparator events latched before the interrupt was enabled so
     * they are not mistaken for a new fault (same as the hardware driver). */
    (void)clearInterruptStatus();
    return true;
}

bool MAX22530::getComparatorStatus(uint16_t& status) {
    status = s_cout_status;
    return true;
}

bool MAX22530::readComparatorThreshold(uint8_t channel, uint16_t& high_counts,
                                       uint16_t& low_counts) {
    if (channel > 3) {
        return false;
    }
    high_counts = s_comp[channel].hi_counts;
    low_counts  = s_comp[channel].lo_counts;
    return true;
}

bool MAX22530::clearInterruptStatus() {
    m_int_status = 0;
    return true;
}

void MAX22530::onInterrupt() { ++m_irq_cnt; }
void MAX22530::onDmaComplete() { ++m_dma_cnt; }
void MAX22530::onDmaError() { ++m_err_cnt; }

void MAX22530::update() {
    /* Samples free-run in SIL; refresh the converted voltages from the
     * world and keep the ready flag so consumers stay "fresh". */
    for (uint8_t ch = 0; ch < 4; ++ch) {
        m_voltages[ch] = hostVoltage(ch);
    }
    m_data_ready = true;

    /* Comparator model: the chip re-evaluates each window per conversion and
     * latches matching INTERRUPT_STATUS bits until cleared. */
    uint16_t cout = 0;
    for (uint8_t ch = 0; ch < 4; ++ch) {
        const uint16_t counts = voltageToCounts(m_voltages[ch]);
        if (counts > s_comp[ch].hi_counts) {
            cout |= static_cast<uint16_t>(INT_CO_POS_1 << ch);
        }
        if (counts < s_comp[ch].lo_counts) {
            cout |= static_cast<uint16_t>(INT_CO_NEG_1 << ch);
        }
    }
    s_cout_status = cout;

    /* Same masking as the hardware driver's raiseFaultsFromInterruptStatus():
     * only interrupt-enabled bits count; channel 0 maps to DC-link OV/UV.
     * Raising on the latch edge keeps a persistent out-of-window condition
     * from spamming; clearInterruptStatus() re-arms. */
    const uint16_t enabled_status =
        static_cast<uint16_t>(cout & m_int_enable);
    const uint16_t prev = m_int_status;
    m_int_status = static_cast<uint16_t>(m_int_status | enabled_status);
    const uint16_t newly = static_cast<uint16_t>(m_int_status & ~prev);
    if ((newly & INT_CO_POS_1) != 0U) {
        FaultManager::instance().raise(FaultSource::Max22530Ov,
                                       FaultReason::Max22530Overvoltage);
    }
    if ((newly & INT_CO_NEG_1) != 0U) {
        FaultManager::instance().raise(FaultSource::Max22530Uv,
                                       FaultReason::Max22530Undervoltage);
    }
}

bool MAX22530::resetInternal(uint16_t) { return true; }
void MAX22530::updateDmaTxBuffer() {}
bool MAX22530::burstTransaction(uint8_t, uint16_t*, uint16_t*) { return true; }
bool MAX22530::parseBurst(const uint8_t*, uint8_t) { return true; }
void MAX22530::raiseFaultsFromInterruptStatus(uint16_t) {}

MAX22530* MAX22530::instanceForPin(uint16_t) { return nullptr; }

} // namespace Inverter
