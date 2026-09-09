/*
 * sil_max22530.cpp — SIL replacement for Src/Inverter/Drivers/Sensors/MAX22530.cpp.
 *
 * Models the isolated 4-channel ADC: no SPI transactions, voltages arrive
 * directly from the SIL world (DC-link on channel 0, phase pole voltages on
 * channels 1..3, both through the 1516:1 sense divider).  dataReady() is
 * always true once initialized (the real chip free-runs at 20 kHz into DMA).
 */
#include "Inverter/Drivers/Sensors/MAX22530.h"

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

bool MAX22530::setComparatorThreshold(uint8_t, float, float,
                                      bool, bool, bool, bool) {
    return true;
}

bool MAX22530::getComparatorStatus(uint16_t& status) {
    status = 0;
    return true;
}

bool MAX22530::readComparatorThreshold(uint8_t channel, uint16_t& high_counts,
                                       uint16_t& low_counts) {
    (void)channel;
    high_counts = 4095;
    low_counts = 0;
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
}

bool MAX22530::resetInternal(uint16_t) { return true; }
void MAX22530::updateDmaTxBuffer() {}
bool MAX22530::burstTransaction(uint8_t, uint16_t*, uint16_t*) { return true; }
bool MAX22530::parseBurst(const uint8_t*, uint8_t) { return true; }
void MAX22530::raiseFaultsFromInterruptStatus(uint16_t) {}

MAX22530* MAX22530::instanceForPin(uint16_t) { return nullptr; }

} // namespace Inverter
