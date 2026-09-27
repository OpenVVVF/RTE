#pragma once

#include <cstdint>
#include <cstddef>

namespace Inverter {

/**
 * @brief Severity levels for inverter faults.
 */
enum class FaultSeverity {
    Warning  = 0, /**< Logged only.                                           */
    High     = 1, /**< Blocks start(); running motor continues (Option A).    */
    Critical = 2, /**< Triggers triple-redundant shutdown instantly.          */
};

/**
 * @brief Central latched fault sources for the inverter.
 *
 * Sources can be raised from interrupt context (e.g. EXTI/DMA callbacks) and
 * are latched until explicitly cleared.  Use isActive() / isSeverityActive()
 * in safety-critical paths; the main loop prints/clears faults via the shell.
 */
enum class FaultSource : uint16_t {
    None             = 0xFFFF,
    GateDriver       = 0,   /**< Legacy generic gate-driver fault.      */
    PwmBreak         = 1,   /**< TIM1 hardware break (DESAT).           */
    Max22530Ov       = 2,   /**< Vbus overvoltage (MAX22530 comparator) */
    Max22530Uv       = 3,   /**< Vbus undervoltage (MAX22530 comparator)*/
    Max22530Adc      = 4,   /**< MAX22530 ADC functionality diagnostic  */
    Max22530Comm     = 5,   /**< MAX22530 SPI framing/internal CRC error*/
    Max22530Field    = 6,   /**< MAX22530 field-side data-loss fault    */
    PhaseOvercurrent = 7,   /**< Phase current above safe limit         */
    AdcError         = 8,   /**< ADC HAL/overrun/queue error            */
    UartError        = 9,   /**< USART3 shell/telemetry error           */
    GateDriverUvlo   = 10,  /**< Gate-driver supply UVLO (/RDY low)     */
    EncoderDma       = 11,  /**< Encoder ADC DMA error                  */
    EncoderAmplitude = 12,  /**< Encoder sin/cos amplitude collapsed    */
    EncoderOutOfRange= 13,  /**< Encoder signal stuck at rail           */
    EncoderTimeout   = 14,  /**< No new encoder sample                  */
    CanBusOff        = 15,  /**< FDCAN bus-off                          */
    CanErrorPassive  = 16,  /**< FDCAN error-passive                    */
    CanProtocolError = 17,  /**< FDCAN protocol error                   */
    SupplyPvd        = 18,  /**< VDD supply dip (PVD)                   */
    SupplyAvd        = 19,  /**< VDDA supply dip (AVD)                  */
    SupplyVosrdy     = 20,  /**< Voltage scaling ready lost             */
    FramComm         = 21,  /**< SPI4 / F-RAM communication error       */
    AdcWatchdog      = 22,  /**< ADC analog watchdog overcurrent        */
    ThrottlePlausibility = 23, /**< Throttle A/B channels disagree      */
    TempSensor       = 24,  /**< Temperature sensor open/short          */
    OvertemperatureMotor = 25,  /**< Motor temperature above limit    */
    OvertemperatureInverter = 26, /**< Board temperature above limit  */
    CurrentSensorRef = 27,  /**< Current-sensor reference out of window */
    OnboardOvertemperature = 28, /**< I2C5 onboard temp sensor over limit */
    RailOvervoltage  = 29,  /**< I2C4 rail monitor bus overvoltage      */
    RailUndervoltage = 30,  /**< I2C4 rail monitor bus undervoltage     */
    TorqueLoss       = 31,  /**< Commanded torque absent (dead outputs) */
    OverTorque       = 32,  /**< Torque current above calibrated max    */
    EncoderLoss      = 33,  /**< Encoder feedback lost while actuating  */
    DcLinkOvWarning  = 34,  /**< DC-link overvoltage warning (regen off)*/
    DcLinkUvDerate   = 35,  /**< DC-link undervoltage derate active     */
};

/**
 * @brief 1024-bit fault word (32 x uint32_t), indexed by FaultSource.
 */
struct FaultBits {
    static constexpr size_t WORDS = 32U;

    uint32_t w[WORDS] = {};

    static constexpr uint16_t index(FaultSource s) {
        return static_cast<uint16_t>(s);
    }
    constexpr void set(FaultSource s) {
        if (s == FaultSource::None) return;
        w[index(s) >> 5] |= (1UL << (index(s) & 31U));
    }
    constexpr void clear(FaultSource s) {
        if (s == FaultSource::None) return;
        w[index(s) >> 5] &= ~(1UL << (index(s) & 31U));
    }
    constexpr bool test(FaultSource s) const {
        if (s == FaultSource::None) return false;
        return (w[index(s) >> 5] & (1UL << (index(s) & 31U))) != 0U;
    }
    constexpr void setFrom(const FaultBits& o) {
        for (size_t i = 0; i < WORDS; ++i) w[i] |= o.w[i];
    }
    constexpr void clearFrom(const FaultBits& o) {
        for (size_t i = 0; i < WORDS; ++i) w[i] &= ~o.w[i];
    }
    constexpr bool intersects(const FaultBits& o) const {
        for (size_t i = 0; i < WORDS; ++i) {
            if ((w[i] & o.w[i]) != 0U) return true;
        }
        return false;
    }
    constexpr bool any() const {
        for (size_t i = 0; i < WORDS; ++i) {
            if (w[i] != 0U) return true;
        }
        return false;
    }

    static constexpr FaultBits bit(FaultSource s) {
        FaultBits b;
        b.set(s);
        return b;
    }
    constexpr FaultBits operator|(const FaultBits& o) const {
        FaultBits r = *this;
        r.setFrom(o);
        return r;
    }
};

/**
 * @brief Typed reason code carried with a fault raise.
 *
 * Avoids copying runtime strings from interrupt context; the printable string is
 * resolved only when the fault is logged.
 */
enum class FaultReason : uint8_t {
    Unspecified = 0,
    UserInjected,
    DesatBreak,
    GateDriverNotReady,
    PhaseOvercurrentSoftware,
    AdcWatchdogTrip,
    AdcHalError,
    UartHalError,
    EncoderAmplitudeLow,
    EncoderAtRail,
    EncoderDmaError,
    EncoderSampleTimeout,
    CanBusOff,
    CanErrorPassive,
    CanErrorLogOverflow,
    CanHalError,
    VosNotReady,
    PvdTriggered,
    AvdTriggered,
    Max22530CrcMismatch,
    Max22530SpiFrameError,
    Max22530SpiDmaError,
    Max22530AdcDiagnostic,
    Max22530FieldLoss,
    Max22530Overvoltage,
    Max22530Undervoltage,
    FramInitIdMismatch,
    FramReadFailed,
    FramWriteFailed,
    FramCommandFailed,
    ThrottlePlausibilityMismatch,
    TempSensorOpenInv1,
    TempSensorOpenInv2,
    TempSensorOpenInv3,
    TempSensorOpenMot,
    TempSensorShortInv1,
    TempSensorShortInv2,
    TempSensorShortInv3,
    TempSensorShortMot,
    OvertemperatureInv1,
    OvertemperatureInv2,
    OvertemperatureInv3,
    OvertemperatureMotor,
    SensorRefOutOfRange,
    OnboardOvertemperature,
    RailOvervoltage,
    RailUndervoltage,
    TorqueLossAbsent,
    OverTorqueLimit,
    EncoderLossWhileDriving,
    DcLinkOvervoltageWarning,
    DcLinkUndervoltageDerate,
    GateDriverFaultPin,
    Count
};

/** @brief Human-readable string for a fault reason code. */
const char* faultReasonString(FaultReason r);

/**
 * @brief Static metadata for each fault source.
 */
struct FaultMeta {
    FaultSource   source;
    const char*   name;
    const char*   category;
    const char*   description;
    FaultSeverity severity;
};

class FaultManager {
public:
    static FaultManager& instance();

    /**
     * @brief Raise one or more fault sources.
     *
     * Safe to call from ISR context.  The @p reason code is stored per source
     * and resolved to a string later by service().
     */
    void raise(FaultSource src, FaultReason reason = FaultReason::Unspecified);

    /**
     * @brief Clear one or more fault sources.
     *
     * Safe to call from ISR context.
     */
    void clear(FaultSource src);

    /** @brief Clear all latched software faults and their pending log entries. */
    void clearAll();

    /** @brief Clear every source in the mask.  Safe to call from ISR context. */
    void clearMask(const FaultBits& mask);

    /** @brief Return true if any fault is active. */
    bool isActive() const;

    /** @brief Return true if the given source is active. */
    bool isActive(FaultSource src) const;

    /** @brief Return true if any source in the mask is active. */
    bool isActiveMask(const FaultBits& mask) const;

    /** @brief Return true if any active fault has the given severity. */
    bool isSeverityActive(FaultSeverity severity) const;

    /** @brief Word 0 (bits 0-31) of the active fault word — legacy view. */
    uint32_t activeFlags() const;

    /** @brief Full 1024-bit active fault word. */
    FaultBits activeBits() const;

    /** @brief Emit all active faults to telemetry as "print" messages. */
    void printSummary();

    /** @brief Publish the current fault mask and active names as telemetry. */
    void publishStatus();

    /**
     * @brief Log any newly-raised faults to telemetry.
     *
     * Call this from the main loop.  It avoids logging from interrupt context.
     */
    void service();

    /**
     * @brief Perform safety actions for active Critical faults.
     *
     * Call once per main-loop iteration after service().  This forces a TIM1
     * software break, asserts the gate-driver reset line, and turns off the
     * gate-driver power rail.
     */
    void executeSafetyActions();

    /**
     * @brief Inject a fault for testing.
     *
     * Call from main-loop / shell context only.
     */
    void testFault(FaultSource src, FaultReason reason = FaultReason::UserInjected);

    /** @brief Look up a fault source by its short name (case-insensitive). */
    static FaultSource sourceFromName(const char* name);

    /** @brief Metadata lookup. */
    static const FaultMeta* metaFor(FaultSource src);
    static const FaultMeta* metaTable();
    static constexpr size_t metaCount();

private:
    FaultManager() = default;

    /* Reason slots are indexed by meta-table index, so table growth never
     * needs a second change here. */
    static constexpr size_t REASON_COUNT = 64U;

    FaultBits         m_active;
    FaultBits         m_pending_log;
    volatile bool     m_safety_executed = false;
    uint32_t          m_gate_flt_count = 0;

    FaultReason       m_reason[REASON_COUNT] = {};

    static constexpr FaultMeta s_meta[] = {
        { FaultSource::GateDriver,       "GateDriver",       "Gate Drive",   "legacy generic gate-driver fault",         FaultSeverity::Critical },
        { FaultSource::PwmBreak,         "PwmBreak",         "Gate Drive",   "hardware DESAT break event",               FaultSeverity::Critical },
        { FaultSource::Max22530Ov,       "Max22530Ov",       "Voltage Sense","Vbus overvoltage (MAX22530 comparator)",   FaultSeverity::Critical },
        { FaultSource::Max22530Uv,       "Max22530Uv",       "Voltage Sense","Vbus undervoltage (MAX22530 comparator)",  FaultSeverity::Critical },
        { FaultSource::Max22530Adc,      "Max22530Adc",      "Isolated ADC", "MAX22530 ADC functionality diagnostic",    FaultSeverity::High     },
        { FaultSource::Max22530Comm,     "Max22530Comm",     "Isolated ADC", "MAX22530 SPI framing/internal CRC error",  FaultSeverity::High     },
        { FaultSource::Max22530Field,    "Max22530Field",    "Isolated ADC", "MAX22530 field-side data-loss",            FaultSeverity::High     },
        { FaultSource::PhaseOvercurrent, "PhaseOvercurrent", "Current Sense","phase current above safe limit",           FaultSeverity::Critical },
        { FaultSource::AdcError,         "AdcError",         "STM32 ADC",    "ADC HAL/overrun/queue error",              FaultSeverity::High     },
        { FaultSource::UartError,        "UartError",        "Telemetry",    "USART3 shell/telemetry error",             FaultSeverity::Warning  },
        { FaultSource::GateDriverUvlo,   "GateDriverUvlo",   "Gate Drive",   "gate-driver supply UVLO (/RDY low)",       FaultSeverity::Critical },
        { FaultSource::EncoderDma,       "EncoderDma",       "Encoder",      "encoder ADC DMA error",                    FaultSeverity::High     },
        { FaultSource::EncoderAmplitude, "EncoderAmplitude", "Encoder",      "encoder sin/cos amplitude collapsed",      FaultSeverity::Warning  },
        { FaultSource::EncoderOutOfRange,"EncoderOutOfRange","Encoder",      "encoder signal stuck at rail",             FaultSeverity::Warning  },
        { FaultSource::EncoderTimeout,   "EncoderTimeout",   "Encoder",      "no new encoder sample",                    FaultSeverity::High     },
        { FaultSource::CanBusOff,        "CanBusOff",        "CAN",          "FDCAN bus-off",                            FaultSeverity::Warning  },
        { FaultSource::CanErrorPassive,  "CanErrorPassive",  "CAN",          "FDCAN error-passive",                      FaultSeverity::Warning  },
        { FaultSource::CanProtocolError, "CanProtocolError", "CAN",          "FDCAN protocol error",                     FaultSeverity::Warning  },
        { FaultSource::SupplyPvd,        "SupplyPvd",        "Supply",       "VDD supply dip (PVD)",                     FaultSeverity::Critical },
        { FaultSource::SupplyAvd,        "SupplyAvd",        "Supply",       "VDDA supply dip (AVD)",                    FaultSeverity::Critical },
        { FaultSource::SupplyVosrdy,     "SupplyVosrdy",     "Supply",       "voltage scaling ready lost",               FaultSeverity::Critical },
        { FaultSource::FramComm,         "FramComm",         "Storage",      "SPI4 / F-RAM communication error",         FaultSeverity::High     },
        { FaultSource::AdcWatchdog,      "AdcWatchdog",      "STM32 ADC",    "ADC analog watchdog overcurrent",          FaultSeverity::Critical },
        { FaultSource::ThrottlePlausibility, "ThrottlePlausibility", "Throttle", "throttle A/B channels disagree",       FaultSeverity::Critical },
        { FaultSource::TempSensor,       "TempSensor",       "Temperature",  "temperature sensor open/short",            FaultSeverity::Warning  },
        { FaultSource::OvertemperatureMotor,    "OvertemperatureMotor",    "Temperature", "motor temperature above limit",        FaultSeverity::Critical },
        { FaultSource::OvertemperatureInverter, "OvertemperatureInverter", "Temperature", "board temperature above limit",        FaultSeverity::Critical },
        { FaultSource::CurrentSensorRef, "CurrentSensorRef", "Current Sense", "current-sensor reference out of window",      FaultSeverity::Warning  },
        { FaultSource::OnboardOvertemperature, "OnboardOvertemperature", "Temperature", "I2C5 onboard temp sensor over limit", FaultSeverity::Warning  },
        { FaultSource::RailOvervoltage,  "RailOvervoltage",  "Rail Monitor", "I2C4 rail bus overvoltage",                    FaultSeverity::Warning  },
        { FaultSource::RailUndervoltage, "RailUndervoltage", "Rail Monitor", "I2C4 rail bus undervoltage",                   FaultSeverity::Warning  },
        { FaultSource::TorqueLoss,       "TorqueLoss",       "Gate Drive",   "commanded torque absent (outputs unresponsive)", FaultSeverity::Critical },
        { FaultSource::OverTorque,       "OverTorque",       "Current Sense","torque current above 110% of calibrated max", FaultSeverity::Critical },
        { FaultSource::EncoderLoss,      "EncoderLoss",      "Encoder",      "encoder feedback lost while actuating",    FaultSeverity::Critical },
        { FaultSource::DcLinkOvWarning,  "DcLinkOvWarning",  "Voltage Sense","Vbus above OV warning threshold (regen disabled)", FaultSeverity::Warning },
        { FaultSource::DcLinkUvDerate,   "DcLinkUvDerate",   "Voltage Sense","Vbus below UV derate threshold (current limited)", FaultSeverity::Warning },
    };
};

constexpr size_t FaultManager::metaCount() {
    return sizeof(s_meta) / sizeof(s_meta[0]);
}

} // namespace Inverter
