#pragma once

#include <cstddef>
#include <cstdint>

namespace Inverter {

/**
 * @brief One applied voltage vector with timing.
 *
 * Modulation strategies (SVPWM, SHE-PWM, six-step, etc.) push these into the
 * schedule.  The current sampler uses them to choose quiet sampling windows;
 * the current observer uses them to predict between measurements.
 */
struct VoltageVector {
    float valpha_v;        /**< Stationary alpha voltage [V]. */
    float vbeta_v;         /**< Stationary beta voltage [V]. */
    uint32_t start_us;     /**< Vector start time [us, DWT based]. */
    uint32_t duration_us;  /**< Vector duration [us]. */
    bool is_zero_vector;   /**< true for null / freewheeling vectors. */
};

/**
 * @brief Small ring buffer of upcoming voltage vectors.
 *
 * The modulation ISR pushes vectors as it applies them; the observer and
 * sampler consume them.  Interrupt-safe single-producer / single-consumer.
 */
class VoltageVectorSchedule {
public:
    static constexpr size_t CAPACITY = 8;

    VoltageVectorSchedule() = default;

    /**
     * @brief Push a new voltage vector.  Called from modulation ISR context.
     * @return true if the entry was stored, false if the buffer was full.
     */
    bool push(const VoltageVector& v);

    /**
     * @brief Peek at the oldest pending vector without removing it.
     * @return true if a vector is available.
     */
    bool peek(VoltageVector& out) const;

    /**
     * @brief Remove and return the oldest pending vector.
     * @return true if a vector was consumed.
     */
    bool next(VoltageVector& out);

    /**
     * @brief Number of vectors currently queued.
     */
    size_t size() const;

    /**
     * @brief Clear the schedule.
     */
    void clear();

private:
    VoltageVector m_buf[CAPACITY] = {};
    volatile size_t m_head = 0;
    volatile size_t m_tail = 0;
};

/**
 * @brief Global schedule instance shared by modulation, sampler, and observer.
 */
VoltageVectorSchedule& voltageVectorSchedule();

} // namespace Inverter
